/*
 * FlexNetCode.c — FlexNet CE/CF protocol support for LinBPQ
 *
 * Adds native FlexNet routing to LinBPQ via AXUDP MAP entries with the F flag.
 * Protocol implementation based on flexnetd v0.3.0 by IW2OHX.
 *
 * MAP IW2OHX-14 44.134.24.4 UDP 10093 B F
 *                                         ^-- enables FlexNet on this link
 *
 * Author: IW2OHX, April 2026
 * License: GPL v2 (same as LinBPQ)
 */

#include "CHeaders.h"
#include "asmstrucs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

/* ── External LinBPQ globals ─────────────────────────────────────────── */

extern BOOL IncludesMail;
extern char * SESSLINE;
extern struct DATAMESSAGE * REPLYBUFFER;
extern int MAXLINKS;
extern LINKTABLE * LINKS;
extern int NUMBEROFPORTS;
extern char * PortConfig[];

/* ── FlexNet globals ─────────────────────────────────────────────────── */

#define FLEXNET_MAX_SESSIONS  8

struct FLEXNET_DEST_ENTRY FlexNetDests[FLEXNET_MAX_DESTS];
int FlexNetDestCount = 0;

struct FLEXNET_SESSION FlexNetSessions[FLEXNET_MAX_SESSIONS];
int FlexNetSessionCount = 0;

/* ── Forward declarations ────────────────────────────────────────────── */

static int  flex_parse_ce_frame(unsigned char * data, int len);
static int  flex_parse_compact_records(unsigned char * data, int len,
                struct FLEXNET_DEST_ENTRY * out, int max_entries);
static int  flex_build_keepalive(unsigned char * buf, int buflen);
static int  flex_build_link_time(unsigned char * buf, int buflen, int value);
static int  flex_build_init(unsigned char * buf, int buflen, int max_ssid);
static int  flex_build_route(unsigned char * buf, int buflen,
                const char * callsign, int ssid_lo, int ssid_hi, int rtt);
static int  flex_dtable_merge(struct FLEXNET_DEST_ENTRY * incoming, int port);
static struct FLEXNET_SESSION * flex_find_session(LINKTABLE * LINK);
static void flex_send_frame(LINKTABLE * LINK, unsigned char pid,
                unsigned char * data, int len);
static void flex_send_own_routes(LINKTABLE * LINK, int port);

/* ── CE frame type constants ─────────────────────────────────────────── */

#define CE_FRAME_KEEPALIVE    1
#define CE_FRAME_STATUS_POS   2
#define CE_FRAME_STATUS_NEG   3
#define CE_FRAME_STATUS_10    4
#define CE_FRAME_COMPACT      5
#define CE_FRAME_LINK_TIME    6
#define CE_FRAME_TOKEN        7
#define CE_FRAME_DEST_BCAST   8
#define CE_FRAME_INIT         9

/* ── Initialization ──────────────────────────────────────────────────── */

void FlexNet_Init(void)
{
    memset(FlexNetDests, 0, sizeof(FlexNetDests));
    FlexNetDestCount = 0;
    memset(FlexNetSessions, 0, sizeof(FlexNetSessions));
    FlexNetSessionCount = 0;
    Debugprintf("FlexNet: initialized (max %d dests, %d sessions)",
                FLEXNET_MAX_DESTS, FLEXNET_MAX_SESSIONS);
}

/* ── Session management ──────────────────────────────────────────────── */

static struct FLEXNET_SESSION * flex_find_session(LINKTABLE * LINK)
{
    for (int i = 0; i < FlexNetSessionCount; i++)
        if (FlexNetSessions[i].LINK == LINK && FlexNetSessions[i].active)
            return &FlexNetSessions[i];
    return NULL;
}

void FlexNet_InitSession(LINKTABLE * LINK, int Port)
{
    struct FLEXNET_SESSION * sess = NULL;

    /* Find a free slot */
    for (int i = 0; i < FLEXNET_MAX_SESSIONS; i++)
    {
        if (!FlexNetSessions[i].active)
        {
            sess = &FlexNetSessions[i];
            break;
        }
    }
    if (!sess)
    {
        Debugprintf("FlexNet: no free session slot for port %d", Port);
        return;
    }

    memset(sess, 0, sizeof(*sess));
    sess->LINK = LINK;
    sess->port = Port;
    sess->active = TRUE;
    sess->our_link_time = 2;  /* 200ms — typical AXUDP */
    sess->session_start = time(NULL);

    if (FlexNetSessionCount < FLEXNET_MAX_SESSIONS)
        FlexNetSessionCount++;

    LINK->FlexNetLink = TRUE;

    /* Send CE init handshake: max SSID 15 (full range) */
    unsigned char init[8];
    int ilen = flex_build_init(init, sizeof(init), 15);
    if (ilen > 0)
        flex_send_frame(LINK, FLEXNET_PID_CE, init, ilen);

    /* Send keepalive to kick-start the exchange */
    unsigned char ka[FLEXNET_KEEPALIVE_LEN];
    int klen = flex_build_keepalive(ka, sizeof(ka));
    if (klen > 0)
        flex_send_frame(LINK, FLEXNET_PID_CE, ka, klen);

    sess->last_keepalive = time(NULL);

    Debugprintf("FlexNet: session started on port %d", Port);
}

void FlexNet_CloseSession(LINKTABLE * LINK)
{
    struct FLEXNET_SESSION * sess = flex_find_session(LINK);
    if (!sess) return;

    /* Remove routes learned from this neighbor */
    int removed = 0;
    for (int i = 0; i < FlexNetDestCount; i++)
    {
        if (FlexNetDests[i].port == sess->port)
        {
            FlexNetDests[i].rtt = FLEXNET_RTT_INFINITY;
            FlexNetDests[i].is_infinity = 1;
            removed++;
        }
    }

    sess->active = FALSE;
    sess->LINK = NULL;
    LINK->FlexNetLink = FALSE;

    Debugprintf("FlexNet: session closed, %d routes withdrawn", removed);
}

/* ── CE Frame Processing ─────────────────────────────────────────────── */

void FlexNet_ProcessCE(LINKTABLE * LINK, struct DATAMESSAGE * Buffer)
{
    unsigned char * data = &Buffer->PID;  /* PID byte + payload */
    int len = Buffer->LENGTH - MSGHDDRLEN;

    if (len < 2) return;

    /* Skip PID byte — we already know it's CE */
    data++;
    len--;

    struct FLEXNET_SESSION * sess = flex_find_session(LINK);
    if (!sess)
    {
        /* Incoming FlexNet frame on a non-session link — auto-create */
        FlexNet_InitSession(LINK, LINK->LINKPORT->PORTNUMBER);
        sess = flex_find_session(LINK);
        if (!sess) return;
    }

    int frame_type = flex_parse_ce_frame(data, len);

    switch (frame_type)
    {
    case CE_FRAME_INIT:
    {
        /* Init handshake: byte 1 = 0x30 + max_ssid */
        int upper_ssid = (len >= 2) ? (int)(data[1]) - FLEXNET_SSID_BASE : 15;
        if (upper_ssid < 0)  upper_ssid = 0;
        if (upper_ssid > 15) upper_ssid = 15;
        sess->got_peer_init = TRUE;
        sess->peer_max_ssid = upper_ssid;
        Debugprintf("FlexNet: init handshake, peer max SSID=%d", upper_ssid);
        break;
    }

    case CE_FRAME_KEEPALIVE:
    {
        sess->keepalive_count++;

        /* Echo keepalive back */
        unsigned char ka[FLEXNET_KEEPALIVE_LEN];
        int klen = flex_build_keepalive(ka, sizeof(ka));
        if (klen > 0)
            flex_send_frame(LINK, FLEXNET_PID_CE, ka, klen);

        /* Send link time on every keepalive cycle */
        unsigned char lt[16];
        int ltlen = flex_build_link_time(lt, sizeof(lt), sess->our_link_time);
        if (ltlen > 0)
            flex_send_frame(LINK, FLEXNET_PID_CE, lt, ltlen);

        /* Advertise our routes after first keepalive + init */
        if (!sess->sent_routes && sess->got_peer_init)
        {
            flex_send_own_routes(LINK, sess->port);
            sess->sent_routes = TRUE;
        }

        sess->last_keepalive = time(NULL);
        break;
    }

    case CE_FRAME_LINK_TIME:
    {
        /* Parse link time value: skip '1' prefix */
        char tbuf[16] = {0};
        int ti = 0;
        for (int i = 1; i < len && i < 15; i++)
        {
            if (isdigit(data[i]))
                tbuf[ti++] = (char)data[i];
            else
                break;
        }
        if (ti > 0)
        {
            sess->peer_link_time = atol(tbuf);
            Debugprintf("FlexNet: peer link time = %ld", sess->peer_link_time);
        }

        /* Reply with our link time */
        unsigned char lt[16];
        int ltlen = flex_build_link_time(lt, sizeof(lt), sess->our_link_time);
        if (ltlen > 0)
            flex_send_frame(LINK, FLEXNET_PID_CE, lt, ltlen);
        break;
    }

    case CE_FRAME_TOKEN:
    {
        /* Echo token back */
        flex_send_frame(LINK, FLEXNET_PID_CE, data, len);
        break;
    }

    case CE_FRAME_STATUS_POS:
    {
        /* '3+' = request token — send our routes if not already done */
        if (!sess->sent_routes)
        {
            flex_send_own_routes(LINK, sess->port);
            sess->sent_routes = TRUE;
        }
        else
        {
            /* Already sent — just ack */
            unsigned char ack[] = { '3', '+', '\r' };
            flex_send_frame(LINK, FLEXNET_PID_CE, ack, 3);
            unsigned char rel[] = { '3', '-', '\r' };
            flex_send_frame(LINK, FLEXNET_PID_CE, rel, 3);
        }
        break;
    }

    case CE_FRAME_STATUS_NEG:
        /* '3-' = release token — end of batch */
        break;

    case CE_FRAME_STATUS_10:
        break;

    case CE_FRAME_COMPACT:
    {
        /* Multi-entry compact routing records */
        struct FLEXNET_DEST_ENTRY entries[64];
        int n = flex_parse_compact_records(data, len, entries, 64);
        int merged = 0;
        for (int i = 0; i < n; i++)
        {
            if (flex_dtable_merge(&entries[i], sess->port) > 0)
                merged++;
        }
        Debugprintf("FlexNet: compact frame — %d entries, %d merged", n, merged);
        break;
    }

    case CE_FRAME_DEST_BCAST:
        /* Individual destination update — parse and merge */
        break;

    default:
        break;
    }

    ReleaseBuffer(Buffer);
}

/* ── CF Frame Processing (L3RTT) ─────────────────────────────────────── */

void FlexNet_ProcessCF(LINKTABLE * LINK, struct DATAMESSAGE * Buffer)
{
    unsigned char * data = &Buffer->PID;
    int len = Buffer->LENGTH - MSGHDDRLEN;

    if (len < 2) { ReleaseBuffer(Buffer); return; }

    /* Skip PID byte */
    data++;
    len--;

    /* Check for L3RTT prefix */
    if (len >= 6 && memcmp(data, "L3RTT:", 6) == 0)
    {
        Debugprintf("FlexNet: L3RTT probe received (len=%d)", len);
        /* Echo back with our timestamps — simplified for now */
        flex_send_frame(LINK, FLEXNET_PID_CF, data, len);
    }

    ReleaseBuffer(Buffer);
}

/* ── Timer — called periodically from LinBPQ main loop ───────────────── */

void FlexNet_Timer(void)
{
    time_t now = time(NULL);

    for (int i = 0; i < FLEXNET_MAX_SESSIONS; i++)
    {
        struct FLEXNET_SESSION * sess = &FlexNetSessions[i];
        if (!sess->active || !sess->LINK) continue;

        /* Send keepalive every 21 seconds */
        if (now - sess->last_keepalive >= 21)
        {
            unsigned char ka[FLEXNET_KEEPALIVE_LEN];
            int klen = flex_build_keepalive(ka, sizeof(ka));
            if (klen > 0)
                flex_send_frame(sess->LINK, FLEXNET_PID_CE, ka, klen);

            unsigned char lt[16];
            int ltlen = flex_build_link_time(lt, sizeof(lt), sess->our_link_time);
            if (ltlen > 0)
                flex_send_frame(sess->LINK, FLEXNET_PID_CE, lt, ltlen);

            sess->last_keepalive = now;
        }
    }
}

/* ── D Command Handler ───────────────────────────────────────────────── */

void FlexNet_CmdDest(TRANSPORTENTRY * Session, char * Bufferptr,
                     char * CmdTail, struct CMDX * CMD)
{
    char callsign_filter[FLEXNET_MAX_CALLSIGN] = {0};
    int  have_filter = 0;

    /* Optional callsign filter: "D IW2OHX" */
    if (CmdTail && *CmdTail && *CmdTail != '\r' && *CmdTail != '\n')
    {
        int fi = 0;
        while (*CmdTail == ' ') CmdTail++;
        while (*CmdTail && *CmdTail != ' ' && *CmdTail != '\r' &&
               fi < FLEXNET_MAX_CALLSIGN - 1)
        {
            callsign_filter[fi++] = (char)toupper((unsigned char)*CmdTail++);
        }
        callsign_filter[fi] = '\0';
        if (fi > 0) have_filter = 1;
    }

    if (FlexNetDestCount == 0)
    {
        Bufferptr = Cmdprintf(Session, Bufferptr,
            "No FlexNet destinations\r");
        SendCommandReply(Session, REPLYBUFFER,
            (int)(Bufferptr - (char *)REPLYBUFFER));
        return;
    }

    Bufferptr = Cmdprintf(Session, Bufferptr,
        "FlexNet Destinations:\r");
    Bufferptr = Cmdprintf(Session, Bufferptr,
        "Dest     SSID    RTT Gateway\r");
    Bufferptr = Cmdprintf(Session, Bufferptr,
        "-------- ----- ----- -------\r");

    int shown = 0;

    for (int i = 0; i < FlexNetDestCount; i++)
    {
        struct FLEXNET_DEST_ENTRY * e = &FlexNetDests[i];
        if (e->rtt >= FLEXNET_RTT_INFINITY) continue;
        if (e->callsign[0] == '\0') continue;

        if (have_filter)
        {
            if (strncasecmp(e->callsign, callsign_filter,
                            strlen(callsign_filter)) != 0)
                continue;
        }

        char ssid_range[12];
        snprintf(ssid_range, sizeof(ssid_range), "%d-%d",
                 e->ssid_lo, e->ssid_hi);

        Bufferptr = Cmdprintf(Session, Bufferptr,
            "%-8s %5s %5d %s\r",
            e->callsign, ssid_range, e->rtt,
            e->via_callsign[0] ? e->via_callsign : "direct");
        shown++;
    }

    if (shown == 0)
        Bufferptr = Cmdprintf(Session, Bufferptr,
            "(no reachable destinations)\r");
    else
        Bufferptr = Cmdprintf(Session, Bufferptr,
            "\r%d destinations\r", shown);

    SendCommandReply(Session, REPLYBUFFER,
        (int)(Bufferptr - (char *)REPLYBUFFER));
}

/* ── CE Frame Classifier ─────────────────────────────────────────────── */

static int flex_parse_ce_frame(unsigned char * data, int len)
{
    if (len <= 0) return -1;

    /* Keepalive: 241 bytes starting with '2' */
    if (len == FLEXNET_KEEPALIVE_LEN && data[0] == '2')
        return CE_FRAME_KEEPALIVE;

    /* 3-byte status frames */
    if (len == 3)
    {
        if (data[0]=='3' && data[1]=='+' && data[2]=='\r')
            return CE_FRAME_STATUS_POS;
        if (data[0]=='3' && data[1]=='-' && data[2]=='\r')
            return CE_FRAME_STATUS_NEG;
        if (data[0]=='1' && data[1]=='0' && data[2]=='\r')
            return CE_FRAME_STATUS_10;
    }

    /* Init handshake: '0' prefix */
    if (data[0] == '0' && len >= 2)
        return CE_FRAME_INIT;

    /* Link time: '1' prefix, > 3 bytes */
    if (data[0] == '1' && len > 3)
        return CE_FRAME_LINK_TIME;

    /* Token: '4' prefix */
    if (data[0] == '4' && len >= 3)
        return CE_FRAME_TOKEN;

    /* Dest broadcast: '6' prefix */
    if (data[0] == '6')
        return CE_FRAME_DEST_BCAST;

    /* Compact record: '3' prefix (not status) */
    if (data[0] == '3')
        return CE_FRAME_COMPACT;

    return -1;
}

/* ── Compact Record Parser ───────────────────────────────────────────── */

static int flex_parse_compact_records(unsigned char * data, int len,
    struct FLEXNET_DEST_ENTRY * out, int max_entries)
{
    if (len < 4 || data[0] != '3') return 0;

    /* Work on the payload after '3' prefix */
    char payload[2048];
    int plen = len - 1;
    if (plen >= (int)sizeof(payload)) plen = (int)sizeof(payload) - 1;
    memcpy(payload, data + 1, plen);
    payload[plen] = '\0';

    /* Check for withdrawal: trailing '-\r' */
    int withdrawal = 0;
    char * end = payload + strlen(payload) - 1;
    while (end >= payload && (*end == '\r' || *end == '\n'))
        *end-- = '\0';
    if (end >= payload && *end == '-')
    {
        withdrawal = 1;
        *end-- = '\0';
    }
    while (end >= payload && *end == ' ')
        *end-- = '\0';

    const char * p = payload;
    int count = 0;

    while (*p && count < max_entries)
    {
        while (*p == ' ') p++;
        if (!*p || *p == '\r' || *p == '\n') break;

        /* CALLSIGN: 6 chars */
        if ((int)(strlen(p)) < 8) break;

        char call[FLEXNET_MAX_CALLSIGN];
        int ci = 0;
        for (int i = 0; i < 6; i++)
        {
            if (p[i] != ' ' && p[i] != '\0')
                call[ci++] = p[i];
        }
        call[ci] = '\0';
        p += 6;
        if (ci == 0) break;

        /* SSID_LO: 1 char */
        int ssid_lo = (int)(*p) - FLEXNET_SSID_BASE;
        if (ssid_lo < 0)  ssid_lo = 0;
        if (ssid_lo > 15) ssid_lo = 15;
        p++;

        /* SSID_HI: 1 char */
        int ssid_hi = (int)(*p) - FLEXNET_SSID_BASE;
        if (ssid_hi < 0)  ssid_hi = 0;
        if (ssid_hi > 15) ssid_hi = 15;
        p++;

        /* RTT: digits */
        char rtt_buf[8] = {0};
        int ri = 0;
        while (*p && isdigit((unsigned char)*p) && ri < 6)
            rtt_buf[ri++] = *p++;
        int rtt = ri ? atoi(rtt_buf) : 0;
        if (withdrawal) rtt = FLEXNET_RTT_INFINITY;

        memset(&out[count], 0, sizeof(out[count]));
        strncpy(out[count].callsign, call, FLEXNET_MAX_CALLSIGN - 1);
        out[count].ssid_lo     = ssid_lo;
        out[count].ssid_hi     = ssid_hi;
        out[count].rtt         = rtt;
        out[count].is_infinity = (rtt >= FLEXNET_RTT_INFINITY);
        count++;

        while (*p == ' ') p++;
    }

    return count;
}

/* ── Destination Table Merge ─────────────────────────────────────────── */

static int flex_dtable_merge(struct FLEXNET_DEST_ENTRY * incoming, int port)
{
    /* Find existing entry with same callsign + SSID range */
    for (int i = 0; i < FlexNetDestCount; i++)
    {
        if (strcasecmp(FlexNetDests[i].callsign, incoming->callsign) == 0 &&
            FlexNetDests[i].ssid_lo == incoming->ssid_lo &&
            FlexNetDests[i].ssid_hi == incoming->ssid_hi)
        {
            /* Update if better RTT or same source */
            if (incoming->rtt < FlexNetDests[i].rtt ||
                FlexNetDests[i].port == port)
            {
                FlexNetDests[i].rtt         = incoming->rtt;
                FlexNetDests[i].is_infinity = incoming->is_infinity;
                FlexNetDests[i].port        = port;
                FlexNetDests[i].last_updated = time(NULL);
            }
            return 1;
        }
    }

    /* New entry */
    if (FlexNetDestCount < FLEXNET_MAX_DESTS)
    {
        memcpy(&FlexNetDests[FlexNetDestCount], incoming,
               sizeof(struct FLEXNET_DEST_ENTRY));
        FlexNetDests[FlexNetDestCount].port = port;
        FlexNetDests[FlexNetDestCount].last_updated = time(NULL);
        FlexNetDestCount++;
        return 1;
    }

    return 0;  /* Table full */
}

/* ── Frame Builders ──────────────────────────────────────────────────── */

static int flex_build_keepalive(unsigned char * buf, int buflen)
{
    if (buflen < FLEXNET_KEEPALIVE_LEN) return -1;
    buf[0] = '2';
    memset(buf + 1, ' ', 240);
    return FLEXNET_KEEPALIVE_LEN;
}

static int flex_build_link_time(unsigned char * buf, int buflen, int value)
{
    char tmp[16];
    int len = snprintf(tmp, sizeof(tmp), "1%d\r", value);
    if (len < 0 || len >= buflen) return -1;
    memcpy(buf, tmp, len);
    return len;
}

static int flex_build_init(unsigned char * buf, int buflen, int max_ssid)
{
    if (buflen < 5) return -1;
    buf[0] = 0x30;                     /* init marker */
    buf[1] = (unsigned char)(0x30 + max_ssid);
    buf[2] = 0x25;                     /* capability flags */
    buf[3] = 0x21;
    buf[4] = 0x0D;                     /* CR terminator */
    return 5;
}

static int flex_build_route(unsigned char * buf, int buflen,
    const char * callsign, int ssid_lo, int ssid_hi, int rtt)
{
    char tmp[32];
    int len = snprintf(tmp, sizeof(tmp), "3%-6.6s%c%c%d \r",
        callsign,
        (char)(FLEXNET_SSID_BASE + ssid_lo),
        (char)(FLEXNET_SSID_BASE + ssid_hi),
        rtt);
    if (len < 0 || len >= buflen) return -1;
    memcpy(buf, tmp, len);
    return len;
}

/* ── Send Helpers ────────────────────────────────────────────────────── */

static void flex_send_frame(LINKTABLE * LINK, unsigned char pid,
    unsigned char * data, int len)
{
    /* Allocate a BPQ message buffer and queue it for transmission */
    struct DATAMESSAGE * Msg;

    Msg = (struct DATAMESSAGE *)GetBuff();
    if (!Msg) return;

    Msg->PID = pid;
    memcpy(Msg->L2DATA, data, len);
    Msg->LENGTH = len + MSGHDDRLEN + 1;  /* +1 for PID byte */

    C_Q_ADD(&LINK->TX_Q, (UINT *)Msg);
    LINK->L2ACKREQ = 0;  /* Trigger send */
}

static void flex_send_own_routes(LINKTABLE * LINK, int port)
{
    /* Request token */
    unsigned char req[] = { '3', '+', '\r' };
    flex_send_frame(LINK, FLEXNET_PID_CE, req, 3);

    /* Our route: node callsign, SSID 0-15, RTT=1 (direct) */
    /* Use the port's callsign as our FlexNet identity */
    char mycall[10] = {0};
    if (LINK->LINKPORT && LINK->LINKPORT->PORTCALL[0])
    {
        /* Convert AX.25 format callsign to ASCII */
        ConvFromAX25(LINK->LINKPORT->PORTCALL, mycall);
        /* Strip SSID for the route callsign (base call only) */
        char * dash = strchr(mycall, '-');
        if (dash) *dash = '\0';
        /* Trim trailing spaces */
        int slen = strlen(mycall);
        while (slen > 0 && mycall[slen-1] == ' ')
            mycall[--slen] = '\0';
    }

    if (mycall[0])
    {
        unsigned char route[32];
        int rlen = flex_build_route(route, sizeof(route),
                                    mycall, 0, 15, 1);
        if (rlen > 0)
            flex_send_frame(LINK, FLEXNET_PID_CE, route, rlen);
    }

    /* Release token */
    unsigned char rel[] = { '3', '-', '\r' };
    flex_send_frame(LINK, FLEXNET_PID_CE, rel, 3);

    Debugprintf("FlexNet: routes advertised (%s SSID 0-15 RTT=1)", mycall);
}
