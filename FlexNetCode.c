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

#include "cheaders.h"
#include "asmstrucs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdint.h>

#include "flexnet_l3.h"

/* ── FlexNet protocol constants (self-contained) ─────────────────────── */

#ifndef FLEXNET_PID_CE
#define FLEXNET_PID_CE        0xCE
#define FLEXNET_PID_CF        0xCF
#define FLEXNET_RTT_INFINITY  60000
#define FLEXNET_KEEPALIVE_LEN 241
#define FLEXNET_MAX_DESTS     2000
#define FLEXNET_MAX_CALLSIGN  10
#define FLEXNET_MAX_ALIAS     8
#define FLEXNET_SSID_BASE     0x30
#endif

/* ── Module version (single source of truth) ────────────────────────── */
/*
 * Bump these together when releasing a new version of linbpq-flexnet.
 *
 *   FLEXNET_VERSION_STR   — user-facing, shown by Cmd.c's V command:
 *                              "Version 6.0.x.y (64 bit) and FlexNet v1.3"
 *   FLEXNET_VERSION_PROTO — protocol identity in the L3RTT version slot:
 *                              "L3RTT: ... LEVEL3_V2.1 linbpq-1.3 $M..."
 *
 * FlexNetVersion below has external linkage so Cmd.c can refer to it
 * without including this file.
 */
#define FLEXNET_VERSION_STR   "v1.3"
#define FLEXNET_VERSION_PROTO "linbpq-1.3"

const char FlexNetVersion[] = FLEXNET_VERSION_STR;

/* These may not be in the header — define if missing */
#ifndef FLEXNET_MAX_SESSIONS
#define FLEXNET_MAX_SESSIONS  8
#endif
#ifndef FLEXNET_MAX_PATH_HOPS
#define FLEXNET_MAX_PATH_HOPS 16
#endif
#define FLEXNET_MAX_PROBES     4
#define FLEXNET_PATH_CACHE_TTL 120  /* seconds before path cache expires */
#define FLEXNET_PROBE_TIMEOUT   15  /* seconds before probe times out */

/* ── Debug control ──────────────────────────────────────────────────── */
/*
 * Set FLEXNET_DEBUG=1 for verbose protocol trace to console and
 * traffic log to /tmp/flexnet_axudp.log.
 *
 * Rebuild with:  make CFLAGS+="-DFLEXNET_DEBUG=1"
 * Or uncomment:  #define FLEXNET_DEBUG 1
 */
/* #define FLEXNET_DEBUG 1 */
#ifndef FLEXNET_DEBUG
#define FLEXNET_DEBUG 0
#endif

/* ── FlexNet data structures (self-contained) ────────────────────────── */

#ifndef FLEXNET_DEST_DEFINED
#define FLEXNET_DEST_DEFINED

struct FLEXNET_DEST_ENTRY
{
    char callsign[FLEXNET_MAX_CALLSIGN];
    int  ssid_lo;
    int  ssid_hi;
    int  rtt;
    int  is_infinity;
    char via_callsign[FLEXNET_MAX_CALLSIGN];
    int  port;
    time_t last_updated;
    /* L3RTT path cache */
    char path_hops[FLEXNET_MAX_PATH_HOPS][FLEXNET_MAX_CALLSIGN];
    int  path_len;          /* 0 = no cached path */
    time_t path_updated;    /* when path was last populated */
};

struct FLEXNET_SESSION
{
    LINKTABLE * LINK;
    int  port;
    BOOL active;
    BOOL got_peer_init;
    BOOL sent_routes;
    int  peer_max_ssid;
    int  keepalive_count;
    long peer_link_time;
    int  our_link_time;
    /* Link-time IIR filter (item #5). Internal state in 10ms ticks;
       our_link_time above stays in 100ms wire units. */
    uint32_t lt_smoothed_10ms;
    uint32_t lt_tx_tick;
    uint32_t lt_sample_count;
    BOOL     lt_tx_pending;
    time_t last_keepalive;
    time_t session_start;
};

#endif

/* ── External LinBPQ globals ─────────────────────────────────────────── */

extern struct DATAMESSAGE * REPLYBUFFER;
extern char MYCALL[];          /* node callsign in AX.25 format (7 bytes) */
extern char MYALIASTEXT[];     /* node alias, 6 chars space-padded, NOT NUL-term */

/* Forward declarations for LinBPQ functions */
extern char * Cmdprintf(TRANSPORTENTRY * Session, char * Bufferptr,
                        const char * format, ...);
extern VOID __cdecl Consoleprintf(const char * format, ...);

/* ── FlexNet globals ─────────────────────────────────────────────────── */

struct FLEXNET_DEST_ENTRY FlexNetDests[FLEXNET_MAX_DESTS];
int FlexNetDestCount = 0;

struct FLEXNET_SESSION FlexNetSessions[FLEXNET_MAX_SESSIONS];
int FlexNetSessionCount = 0;

struct FLEXNET_PROBE
{
    int  active;
    char target_call[FLEXNET_MAX_CALLSIGN];
    int  target_ssid;       /* -1 = any */
    int  dest_index;        /* index into FlexNetDests[] */
    time_t sent_time;
    BOOL got_reply;
    char reply_hops[FLEXNET_MAX_PATH_HOPS][FLEXNET_MAX_CALLSIGN];
    int  reply_hop_count;
};

struct FLEXNET_PROBE FlexNetProbes[FLEXNET_MAX_PROBES];

/* ── AXUDP Traffic Logger ───────────────────────────────────────────── */
/*
 * Writes timestamped traffic log to /tmp/flexnet_axudp.log
 * Called from bpqaxip.c and L2Code.c at key decision points.
 */

static FILE * flexlog_fp = NULL;

static void flexlog_open(void)
{
    if (!flexlog_fp)
    {
        flexlog_fp = fopen("/tmp/flexnet_axudp.log", "a");
        if (flexlog_fp)
            setvbuf(flexlog_fp, NULL, _IOLBF, 0);  /* line buffered */
    }
}

void FlexNet_Log(const char * format, ...)
{
    if (!FLEXNET_DEBUG) return;
    flexlog_open();
    if (!flexlog_fp) return;

    /* Timestamp */
    time_t now = time(NULL);
    struct tm * tm = localtime(&now);
    fprintf(flexlog_fp, "%02d:%02d:%02d ",
            tm->tm_hour, tm->tm_min, tm->tm_sec);

    /* Message */
    va_list args;
    va_start(args, format);
    vfprintf(flexlog_fp, format, args);
    va_end(args);
    fprintf(flexlog_fp, "\n");
}

/* Decode an AX.25 frame header for logging: from, to, digis, ctl, pid */
void FlexNet_LogFrame(const char * tag, unsigned char * frame, int len)
{
    if (!FLEXNET_DEBUG) return;
    if (len < 15) return;  /* need at least dest(7) + src(7) + ctl(1) */

    char dest[20] = {0}, src[20] = {0};
    ConvFromAX25((char *)&frame[0], dest);
    ConvFromAX25((char *)&frame[7], src);
    { int sl = strlen(dest); while (sl > 0 && dest[sl-1] == ' ') dest[--sl] = '\0'; }
    { int sl = strlen(src);  while (sl > 0 && src[sl-1] == ' ')  src[--sl] = '\0'; }

    /* Count digipeaters */
    int ndigi = 0;
    char digis[128] = {0};
    int pos = 14;
    /* Check if address field extends (bit 0 of byte 6 and 13) */
    if (!(frame[13] & 0x01))
    {
        /* More address bytes — digipeaters */
        while (pos + 7 <= len && ndigi < 8)
        {
            char digi[20] = {0};
            ConvFromAX25((char *)&frame[pos], digi);
            { int sl = strlen(digi); while (sl > 0 && digi[sl-1] == ' ') digi[--sl] = '\0'; }
            int repeated = (frame[pos + 6] & 0x80) ? 1 : 0;
            char tmp[32];
            snprintf(tmp, sizeof(tmp), " %s%s", digi, repeated ? "*" : "");
            strncat(digis, tmp, sizeof(digis) - strlen(digis) - 1);
            ndigi++;
            if (frame[pos + 6] & 0x01) break;  /* last address */
            pos += 7;
        }
        pos += 7;  /* skip last digi */
    }

    /* Control byte */
    int ctl_offset = 14 + ndigi * 7;
    unsigned char ctl = (ctl_offset < len) ? frame[ctl_offset] : 0;

    /* Decode control */
    const char * ctl_name = "???";
    if ((ctl & 0xEF) == 0x2F) ctl_name = "SABM";
    else if ((ctl & 0xEF) == 0x63) ctl_name = "UA";
    else if ((ctl & 0xEF) == 0x0F) ctl_name = "DM";
    else if ((ctl & 0xEF) == 0x43) ctl_name = "DISC";
    else if ((ctl & 0x01) == 0)    ctl_name = "I";
    else if ((ctl & 0x0F) == 0x01) ctl_name = "RR";
    else if ((ctl & 0x0F) == 0x05) ctl_name = "RNR";
    else if ((ctl & 0x0F) == 0x09) ctl_name = "REJ";
    else if ((ctl & 0xEF) == 0x03) ctl_name = "UI";

    /* PID (only for I and UI frames) */
    int pid_offset = ctl_offset + 1;
    char pid_str[8] = "";
    if (pid_offset < len && ((ctl & 0x01) == 0 || (ctl & 0xEF) == 0x03))
        snprintf(pid_str, sizeof(pid_str), " pid=%02X", frame[pid_offset]);

    FlexNet_Log("%s: %s -> %s%s ctl=%s(0x%02X)%s len=%d",
                tag, src, dest,
                digis[0] ? digis : "",
                ctl_name, ctl, pid_str, len);
}

/* ── Tick source — 10 ms-resolution monotonic counter ────────────────── */
/*
 * flex_get_ticks_10ms — returns 10ms ticks of CLOCK_MONOTONIC, modulo 2^32.
 *
 * Used for FlexNet L3RTT counters c1..c4 (protocol uses 10ms granularity).
 * The reference frame is "monotonic time since some unspecified point"
 * (typically boot on Linux). Each peer uses its own reference; the
 * protocol only cares about differences, computed with unsigned 32-bit
 * arithmetic that wraps cleanly every ≈497 days.
 *
 * Earlier versions of this helper used a "first call sets base, return
 * delta-from-base" pattern. That had a cold-start bug: the very first
 * reply emitted c3=0/c4=0, which xnet interprets as link-down per the
 * protocol — wrong signal even though our routes were healthy. Using
 * the raw monotonic value avoids that entirely (boot uptime is well
 * over 0 in any realistic deployment).
 *
 * Returns 0 only if clock_gettime fails (extremely unlikely on Linux).
 */
static uint32_t flex_get_ticks_10ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;

    /* tv_sec * 100 + tv_nsec / 10_000_000 = total 10ms ticks. */
    uint64_t total_10ms = (uint64_t)now.tv_sec * 100ull
                        + (uint64_t)now.tv_nsec / 10000000ull;
    return (uint32_t)total_10ms;
}

/* ── Binary buffer search for the "L3RTT:" tag ───────────────────────── */
/*
 * flex_find_l3rtt_tag — locate "L3RTT:" anywhere in a binary buffer.
 *
 * xnet wraps L3RTT probes in a NetRom L3 packet. The wire layout is:
 *   [PID=0xCF] [NetRom L3 header: src(7) dst(7) ttl(1) ...] [L3RTT: ASCII]
 * so the "L3RTT:" text is at offset ~15, not at the start of the payload.
 * The CE/CF processor strips the PID before calling us, but the L3 header
 * remains. This helper scans the binary buffer (no NUL-termination assumed)
 * for the "L3RTT:" tag so the gate can match wrapped frames as well as
 * any raw-format frames a future peer might send.
 *
 * Returns a pointer to the first occurrence, or NULL if not found.
 */
static const unsigned char * flex_find_l3rtt_tag(const unsigned char * data, int len)
{
    static const char tag[] = "L3RTT:";
    const int tag_len = (int)(sizeof(tag) - 1);

    if (!data || len < tag_len)
        return NULL;

    for (int i = 0; i <= len - tag_len; i++)
        if (data[i] == (unsigned char)tag[0] &&
            memcmp(data + i, tag, (size_t)tag_len) == 0)
            return data + i;

    return NULL;
}

/* ── L3RTT counter parser ────────────────────────────────────────────── */
/*
 * flex_parse_l3rtt_counters — extract c1..c4 from a received L3RTT frame.
 *
 * Wire format: "L3RTT:%11lu%11lu%11lu%11lu %-6.6s LEVEL3_V2.1 ..." \r
 *
 * Lenient on whitespace between counter fields: real captures show the
 * L3 monitor's 80-char line wrap can split fields across lines. This
 * matches flexnetd's cf_parse_l3rtt() behaviour and the protocol's
 * "be liberal in what you accept" tradition.
 *
 * Counter values >2^32-1 are truncated to uint32_t — the protocol uses
 * 32-bit ticks; any peer emitting larger values is buggy.
 *
 * Returns 0 on success (all four counters written), -1 on any parse
 * failure (missing prefix, non-numeric field, fewer than four fields,
 * null pointers, or truncated input).
 */
static int flex_parse_l3rtt_counters(const unsigned char * data, int len,
                                     uint32_t * c1, uint32_t * c2,
                                     uint32_t * c3, uint32_t * c4)
{
    if (!data || !c1 || !c2 || !c3 || !c4 || len < 7)
        return -1;

    /* Copy to a NUL-terminated buffer. We replace internal NUL/CR/LF bytes
     * with spaces so a single strstr scan finds "L3RTT:" even when the
     * frame is preceded by binary headers (NetRom L3+L4) that contain
     * 0x00 bytes — strstr would otherwise stop at the first inner NUL.
     * The terminator at text[copy_len] is left untouched. */
    char text[1024];
    int copy_len = (len < (int)sizeof(text) - 1) ? len : (int)sizeof(text) - 1;
    memcpy(text, data, (size_t)copy_len);
    text[copy_len] = '\0';
    for (int i = 0; i < copy_len; i++)
        if (text[i] == '\n' || text[i] == '\r' || text[i] == '\0')
            text[i] = ' ';

    const char * p = strstr(text, "L3RTT:");
    if (!p) return -1;
    p += 6;

    /* Four whitespace-separated decimal counters. */
    uint32_t out[4];
    for (int i = 0; i < 4; i++)
    {
        while (*p == ' ') p++;
        if (!isdigit((unsigned char)*p)) return -1;
        char * end = NULL;
        unsigned long v = strtoul(p, &end, 10);
        if (end == p) return -1;
        out[i] = (uint32_t)v;
        p = end;
    }

    *c1 = out[0]; *c2 = out[1]; *c3 = out[2]; *c4 = out[3];
    return 0;
}

/* ── L3RTT counter builder ───────────────────────────────────────────── */
/*
 * flex_build_l3rtt — build a fresh L3RTT frame payload.
 *
 * Wire format (mirrors flexnetd's cf_build_l3rtt):
 *   "L3RTT:%11lu%11lu%11lu%11lu %-6.6s LEVEL3_V2.1 linbpq-1.3 $M%u $N\r"
 *
 * "LEVEL3_V2.1" is the FlexNet L3RTT protocol marker (matches flexnetd
 * and xnet). The version slot is "linbpq-1.3" — our distinct identity,
 * so peers that log L3RTT replies can tell us apart from flexnetd / xnet.
 *
 * The alias parameter may be exactly 6 chars without NUL termination
 * (e.g. BPQ's MYALIASTEXT[6]); the "%-6.6s" precision caps the read at
 * 6 bytes, so this is safe.
 *
 * Returns the byte count written on success, or -1 if buf is too small.
 */
static int flex_build_l3rtt(unsigned char * buf, int buflen,
                            uint32_t c1, uint32_t c2,
                            uint32_t c3, uint32_t c4,
                            const char * alias, uint32_t max_dest)
{
    if (!buf || buflen <= 0)
        return -1;

    char payload[256];
    int len = snprintf(payload, sizeof(payload),
        "L3RTT:%11lu%11lu%11lu%11lu %-6.6s LEVEL3_V2.1 " FLEXNET_VERSION_PROTO " $M%u $N\r",
        (unsigned long)c1, (unsigned long)c2,
        (unsigned long)c3, (unsigned long)c4,
        alias ? alias : "NONE  ",
        (unsigned int)max_dest);

    if (len < 0 || len >= (int)sizeof(payload))
        return -1;
    if (len >= buflen)
        return -1;

    memcpy(buf, payload, (size_t)len);
    return len;
}

/* ── Reachable-route counter ─────────────────────────────────────────── */
/*
 * flex_count_reachable — return the number of dest entries currently
 * considered reachable.
 *
 * Mirrors flexnetd's dtable_count_reachable(): an entry counts if its
 * RTT is strictly less than FLEXNET_RTT_INFINITY (the "withdrawn route"
 * sentinel, 60000). The is_infinity flag is derived from the same check
 * (set at line 1492), so rtt < INFINITY is the canonical predicate.
 *
 * Used by step 5 to set the L3RTT link-down guard: when we have zero
 * reachable routes, our reply carries c3=0/c4=0 and peers know to remove
 * routes that go through us.
 */
static int flex_count_reachable(void)
{
    int n = 0;
    for (int i = 0; i < FlexNetDestCount; i++)
        if (FlexNetDests[i].rtt < FLEXNET_RTT_INFINITY)
            n++;
    return n;
}

/* ── Forward declarations ────────────────────────────────────────────── */

static int  flex_parse_ce_frame(unsigned char * data, int len);
static int  flex_parse_compact_records(unsigned char * data, int len,
                struct FLEXNET_DEST_ENTRY * out, int max_entries);
static int  flex_build_keepalive(unsigned char * buf, int buflen);
static int  flex_build_link_time(unsigned char * buf, int buflen, int value);
static int  flex_send_link_time(LINKTABLE * LINK,
                struct FLEXNET_SESSION * sess);
static void flex_link_time_sample(struct FLEXNET_SESSION * sess);
static int  flex_build_init(unsigned char * buf, int buflen, int max_ssid);
static int  flex_build_route(unsigned char * buf, int buflen,
                const char * callsign, int ssid_lo, int ssid_hi, int rtt);
static int  flex_dtable_merge(struct FLEXNET_DEST_ENTRY * incoming, int port);
static int  flex_find_dest(const char * call, int ssid_lo, int ssid_hi);
static struct FLEXNET_SESSION * flex_find_session(LINKTABLE * LINK);
static void flex_send_frame(LINKTABLE * LINK, unsigned char pid,
                unsigned char * data, int len);
static void flex_send_own_routes(LINKTABLE * LINK, int port);
static void flex_get_neighbor_call(int port, char * buf, int buflen);
static int  flex_send_l3rtt_probe(int dest_idx,
                const char * target_call, int target_ssid);
static int  flex_check_probe_reply(unsigned char * data, int len,
                LINKTABLE * LINK);
static void flex_show_dest_detail(TRANSPORTENTRY * Session,
                char ** Bufferptr_p, int dest_idx,
                const char * query_call, int query_ssid);
static void flex_format_uptime(time_t elapsed, char * buf, int buflen);
static uint32_t flex_get_ticks_10ms(void);
static const unsigned char * flex_find_l3rtt_tag(const unsigned char * data, int len);
static int flex_parse_l3rtt_counters(const unsigned char * data, int len,
                uint32_t * c1, uint32_t * c2,
                uint32_t * c3, uint32_t * c4);
static int flex_build_l3rtt(unsigned char * buf, int buflen,
                uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4,
                const char * alias, uint32_t max_dest);
static int flex_count_reachable(void);

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
    memset(FlexNetProbes, 0, sizeof(FlexNetProbes));
    Consoleprintf("FlexNet: initialized (max %d dests, %d sessions)",
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

    /* Only one FlexNet session per port — if one exists, update LINK
       and re-send init+keepalive on the new L2 connection */
    for (int i = 0; i < FLEXNET_MAX_SESSIONS; i++)
    {
        if (FlexNetSessions[i].active && FlexNetSessions[i].port == Port)
        {
            FlexNetSessions[i].LINK = LINK;
            FlexNetSessions[i].sent_routes = FALSE;  /* re-advertise */
            FlexNetSessions[i].got_peer_init = FALSE;
            FlexNetSessions[i].keepalive_count = 0;
            FlexNetSessions[i].session_start = time(NULL);
            FlexNetSessions[i].last_keepalive = time(NULL);
            LINK->FlexNetLink = TRUE;

            /* Re-send init + keepalive on the new LINK */
            int node_ssid = (MYCALL[6] >> 1) & 0x0F;
            unsigned char init[8];
            int ilen = flex_build_init(init, sizeof(init), node_ssid);
            if (ilen > 0)
                flex_send_frame(LINK, FLEXNET_PID_CE, init, ilen);

            unsigned char ka[FLEXNET_KEEPALIVE_LEN];
            int klen = flex_build_keepalive(ka, sizeof(ka));
            if (klen > 0)
                flex_send_frame(LINK, FLEXNET_PID_CE, ka, klen);

            Consoleprintf("FlexNet: session on port %d reconnected "
                          "(re-sent init + keepalive)", Port);
            return;
        }
    }

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
        Consoleprintf("FlexNet: no free session slot for port %d", Port);
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

    /* Send CE init handshake: max SSID = our node SSID */
    int node_ssid = (MYCALL[6] >> 1) & 0x0F;  /* extract SSID from AX.25 */
    unsigned char init[8];
    int ilen = flex_build_init(init, sizeof(init), node_ssid);
    if (ilen > 0)
        flex_send_frame(LINK, FLEXNET_PID_CE, init, ilen);

    /* Send keepalive to kick-start the exchange */
    unsigned char ka[FLEXNET_KEEPALIVE_LEN];
    int klen = flex_build_keepalive(ka, sizeof(ka));
    if (klen > 0)
        flex_send_frame(LINK, FLEXNET_PID_CE, ka, klen);

    sess->last_keepalive = time(NULL);

    /* Decode neighbor callsign + SSID */
    char snbr[20] = {0};
    ConvFromAX25(LINK->LINKCALL, snbr);
    { int sl = strlen(snbr); while (sl > 0 && snbr[sl-1] == ' ') snbr[--sl] = '\0'; }

    /* Add the neighbor itself as a direct destination (RTT=1) */
    {
        char nbr_base[FLEXNET_MAX_CALLSIGN] = {0};
        int nbr_ssid = 0;
        strncpy(nbr_base, snbr, FLEXNET_MAX_CALLSIGN - 1);
        char * dash = strchr(nbr_base, '-');
        if (dash)
        {
            nbr_ssid = atoi(dash + 1);
            *dash = '\0';
        }

        struct FLEXNET_DEST_ENTRY nbr_entry;
        memset(&nbr_entry, 0, sizeof(nbr_entry));
        strncpy(nbr_entry.callsign, nbr_base, FLEXNET_MAX_CALLSIGN - 1);
        nbr_entry.ssid_lo = nbr_ssid;
        nbr_entry.ssid_hi = nbr_ssid;
        nbr_entry.rtt = 1;
        strncpy(nbr_entry.via_callsign, snbr, FLEXNET_MAX_CALLSIGN - 1);
        nbr_entry.port = Port;

        flex_dtable_merge(&nbr_entry, Port);
        Consoleprintf("FlexNet: added neighbor %s (%d-%d) RTT=1 "
                      "as direct destination", nbr_base, nbr_ssid, nbr_ssid);
    }

    Consoleprintf("FlexNet: session started on port %d with %s "
                "(sent init max_ssid=%d + keepalive)", Port, snbr, node_ssid);
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

    char cnbr[20] = {0};
    ConvFromAX25(LINK->LINKCALL, cnbr);
    { int sl = strlen(cnbr); while (sl > 0 && cnbr[sl-1] == ' ') cnbr[--sl] = '\0'; }
    Consoleprintf("FlexNet: session with %s closed, "
                "%d routes withdrawn", cnbr, removed);
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

    /* Decode neighbor callsign for logging */
    char nbr[20] = {0};
    ConvFromAX25(LINK->LINKCALL, nbr);
    { int sl = strlen(nbr); while (sl > 0 && nbr[sl-1] == ' ') nbr[--sl] = '\0'; }

    struct FLEXNET_SESSION * sess = flex_find_session(LINK);
    if (!sess)
    {
        /* Incoming FlexNet frame on a non-session link — auto-create */
        if (FLEXNET_DEBUG) Consoleprintf("FlexNet CE: new session auto-created for %s", nbr);
        FlexNet_InitSession(LINK, LINK->LINKPORT->PORTNUMBER);
        sess = flex_find_session(LINK);
        if (!sess) return;
    }

    int frame_type = flex_parse_ce_frame(data, len);

    /* Log every CE frame with type name */
    static const char * ce_names[] = {
        "???", "KEEPALIVE", "STATUS+", "STATUS-", "STATUS_10",
        "COMPACT", "LINK_TIME", "TOKEN", "DEST_BCAST", "INIT"
    };
    const char * tname = (frame_type >= 1 && frame_type <= 9)
                         ? ce_names[frame_type] : "UNKNOWN";
    if (FLEXNET_DEBUG) Consoleprintf("FlexNet CE: %s from %s (len=%d)", tname, nbr, len);

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
        if (FLEXNET_DEBUG) Consoleprintf("FlexNet: init from %s max_ssid=%d, "
                    "replying max_ssid=15", nbr, upper_ssid);
        break;
    }

    case CE_FRAME_KEEPALIVE:
    {
        sess->keepalive_count++;
        if (FLEXNET_DEBUG) Consoleprintf("FlexNet: keepalive #%d from %s, "
                    "echoing + sending LT=%d",
                    sess->keepalive_count, nbr, sess->our_link_time);

        /* Echo keepalive back */
        unsigned char ka[FLEXNET_KEEPALIVE_LEN];
        int klen = flex_build_keepalive(ka, sizeof(ka));
        if (klen > 0)
            flex_send_frame(LINK, FLEXNET_PID_CE, ka, klen);

        /* Send link time on every keepalive cycle (stamps lt_tx_tick). */
        flex_send_link_time(LINK, sess);

        /* Advertise our routes after first keepalive + init */
        if (!sess->sent_routes && sess->got_peer_init)
        {
            if (FLEXNET_DEBUG) Consoleprintf("FlexNet: first keepalive after init — "
                        "sending our routes");
            flex_send_own_routes(LINK, sess->port);
            sess->sent_routes = TRUE;
        }

        sess->last_keepalive = time(NULL);
        break;
    }

    case CE_FRAME_LINK_TIME:
    {
        /* Fold this LT (peer's reply to our previous LT) into the IIR
           before doing anything else — the reply we send below re-arms
           the pending stamp. */
        flex_link_time_sample(sess);

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
            if (FLEXNET_DEBUG) Consoleprintf("FlexNet: link time from %s: %ldms "
                        "(ours: %dms)", nbr,
                        sess->peer_link_time, sess->our_link_time);
        }

        /* Reply with our link time (stamps lt_tx_tick). */
        flex_send_link_time(LINK, sess);
        break;
    }

    case CE_FRAME_TOKEN:
    {
        if (FLEXNET_DEBUG) Consoleprintf("FlexNet: token exchange with %s", nbr);
        /* Echo token back */
        flex_send_frame(LINK, FLEXNET_PID_CE, data, len);
        break;
    }

    case CE_FRAME_STATUS_POS:
    {
        /* '3+' = request token — send our routes if not already done */
        if (FLEXNET_DEBUG) Consoleprintf("FlexNet: route request (3+) from %s", nbr);
        if (!sess->sent_routes)
        {
            flex_send_own_routes(LINK, sess->port);
            sess->sent_routes = TRUE;
        }
        else
        {
            /* Already sent — just ack */
            if (FLEXNET_DEBUG) Consoleprintf("FlexNet: routes already sent, acking");
            unsigned char ack[] = { '3', '+', '\r' };
            flex_send_frame(LINK, FLEXNET_PID_CE, ack, 3);
            unsigned char rel[] = { '3', '-', '\r' };
            flex_send_frame(LINK, FLEXNET_PID_CE, rel, 3);
        }
        break;
    }

    case CE_FRAME_STATUS_NEG:
        /* '3-' = release token — end of batch */
        if (FLEXNET_DEBUG) Consoleprintf("FlexNet: route release (3-) from %s", nbr);
        break;

    case CE_FRAME_STATUS_10:
        if (FLEXNET_DEBUG) Consoleprintf("FlexNet: status 10 from %s", nbr);
        break;

    case CE_FRAME_COMPACT:
    {
        /* Multi-entry compact routing records */
        struct FLEXNET_DEST_ENTRY entries[64];
        int n = flex_parse_compact_records(data, len, entries, 64);
        int new_cnt = 0, upd_cnt = 0, skip_cnt = 0;
        for (int i = 0; i < n; i++)
        {
            int rc = flex_dtable_merge(&entries[i], sess->port);
            int is_refresh = (entries[i].rtt == 0 && !entries[i].is_infinity);
            if (rc == 1)            new_cnt++;
            else if (rc == 2)       upd_cnt++;
            else if (is_refresh)    skip_cnt++;

            /* Log each route entry */
            const char * tag;
            if (is_refresh)
                tag = "skip (refresh)";
            else if (entries[i].rtt >= FLEXNET_RTT_INFINITY)
                tag = "withdrawn";
            else if (rc == 1)
                tag = "new";
            else if (rc == 2)
                tag = "updated";
            else
                tag = "skip (other)";
            if (FLEXNET_DEBUG) Consoleprintf("FlexNet:   route: %s (%d-%d) RTT=%d [%s]",
                        entries[i].callsign,
                        entries[i].ssid_lo, entries[i].ssid_hi,
                        entries[i].rtt, tag);
        }
        if (FLEXNET_DEBUG) Consoleprintf("FlexNet: compact batch — %d entries "
                    "(%d new, %d updated, %d skipped), total=%d",
                    n, new_cnt, upd_cnt, skip_cnt, FlexNetDestCount);
        FlexNet_Log("CE-COMPACT-BATCH: from=%s entries=%d new=%d updated=%d "
                    "skipped=%d total=%d", nbr, n, new_cnt, upd_cnt,
                    skip_cnt, FlexNetDestCount);
        break;
    }

    case CE_FRAME_DEST_BCAST:
        if (FLEXNET_DEBUG) Consoleprintf("FlexNet: dest broadcast from %s (len=%d)",
                    nbr, len);
        break;

    default:
        if (FLEXNET_DEBUG) Consoleprintf("FlexNet CE: unknown frame type from %s "
                    "(byte0=0x%02X, len=%d)", nbr, data[0], len);
        break;
    }

    ReleaseBuffer(Buffer);
}

/* ── CF Frame Processing (L3RTT) ─────────────────────────────────────── */

void FlexNet_ProcessCF(LINKTABLE * LINK, struct DATAMESSAGE * Buffer)
{
    {
        char entry_call[20] = {0};
        ConvFromAX25((char *)LINK->LINKCALL, entry_call);
        FlexNet_Log("CF-ENTRY: from=%s LINK=%p Buffer->LENGTH=%d",
                    entry_call, (void *)LINK, Buffer->LENGTH);
    }

    unsigned char * data = &Buffer->PID;
    int len = Buffer->LENGTH - MSGHDDRLEN;

    if (len < 2) { ReleaseBuffer(Buffer); return; }

    /* Skip PID byte */
    data++;
    len--;

    /* Decode neighbor for logging */
    char nbr[20] = {0};
    ConvFromAX25(LINK->LINKCALL, nbr);
    { int sl = strlen(nbr); while (sl > 0 && nbr[sl-1] == ' ') nbr[--sl] = '\0'; }

    /* Show first bytes of CF payload */
    char preview[48] = {0};
    int plen = len > 40 ? 40 : len;
    for (int i = 0; i < plen; i++)
        preview[i] = (data[i] >= 0x20 && data[i] < 0x7F) ? data[i] : '.';
    preview[plen] = '\0';
    if (FLEXNET_DEBUG) Consoleprintf("FlexNet CF: from %s len=%d [%s]", nbr, len, preview);

    /* Check for the L3RTT tag anywhere in the payload. xnet wraps L3RTT
     * probes in a NetRom L3 packet (~15-byte header before the "L3RTT:"
     * text), so a strict offset-0 memcmp would miss every wrapped frame.
     * The downstream parser already uses search-style scanning, so passing
     * the full buffer (header + payload) is harmless for parse correctness. */
    if (flex_find_l3rtt_tag(data, len) != NULL)
    {
        /* Check if this is a reply to one of our pending probes */
        int handled = flex_check_probe_reply(data, len, LINK);

        if (handled)
        {
            if (FLEXNET_DEBUG) Consoleprintf("FlexNet: L3RTT reply matched our probe");
            FlexNet_Log("L3RTT-RX: %s reply matched our pending probe", nbr);
        }
        else
        {
            /* v1.3: incoming probe — build a real reply with our own
             * c3/c4 ticks instead of echoing the peer's bytes. Sets
             * c3=0/c4=0 when we have zero reachable routes so peers
             * detect our link-down state and stop routing through us.
             *
             * Option B (post-capture 2026-05-10): xnet binds replies to
             * its pending-probe table via the NetRom L3 envelope's IN/ID
             * fields. A bare L3RTT payload is parsed but never bound, so
             * the F-row rtt freezes and the Q-row queue grows. When the
             * incoming probe arrives wrapped in an L3 INFO frame, mirror
             * the envelope on TX with IN/ID echoed; otherwise fall back
             * to the bare payload (preserves behaviour with non-xnet
             * peers that send raw L3RTT). */
            uint32_t peer_c1, peer_c2, peer_c3, peer_c4;
            if (flex_parse_l3rtt_counters(data, len,
                                          &peer_c1, &peer_c2,
                                          &peer_c3, &peer_c4) == 0)
            {
                uint32_t recv_tick = flex_get_ticks_10ms();
                int reachable      = flex_count_reachable();
                uint32_t reply_c3  = (reachable > 0) ? recv_tick : 0;
                uint32_t reply_c4  = (reachable > 0) ? flex_get_ticks_10ms() : 0;

                unsigned char l3rtt[256];
                int plen = flex_build_l3rtt(l3rtt, sizeof(l3rtt),
                                            peer_c1, peer_c2,
                                            reply_c3, reply_c4,
                                            MYALIASTEXT,
                                            (uint32_t)FlexNetDestCount);

                /* Detect whether the incoming probe arrived L3-wrapped.
                 * flexl3_is_connection_frame is misleadingly named — it
                 * really tests "does the buffer start with a valid AX.25
                 * callsign byte and have ≥ FLEXL3_MIN_LEN bytes", which
                 * is exactly the guard we need before flexl3_parse runs.
                 * Without this guard, a raw L3RTT frame can yield a false
                 * positive opcode if its byte[19] happens to land on
                 * ASCII '1'..'6' (≈10% chance per probe). */
                int have_l3 = 0;
                struct FLEXL3_HEADER in_hdr;
                if (flexl3_is_connection_frame((unsigned char *)data, len)
                    && flexl3_parse((unsigned char *)data, len, &in_hdr) == 0
                    && in_hdr.opcode == FLEXL3_INFO)
                {
                    have_l3 = 1;
                }

                unsigned char frame[FLEXL3_MAX_FRAME];
                int flen = -1;
                if (plen > 0)
                {
                    if (have_l3)
                    {
                        /* L3 envelope for the reply:
                         *
                         *   dest  ← peer's call (unicast back to the originator)
                         *   src   ← MYCALL (our node identity)
                         *   ttl   ← echo incoming TTL — the low TTL (~2) is
                         *           xnet's L3RTT class marker. Mirroring is
                         *           the only way the reply gets routed to
                         *           xnet's L3RTT handler instead of being
                         *           treated as a generic INFO frame.
                         *   IN/ID ← echo — xnet's pending-probe table is
                         *           keyed on these (load-bearing).
                         *
                         * NEVER set dest to the incoming "L3RTT" pseudo-
                         * callsign: that pseudo is a multicast group, and
                         * xnet's L3 forwarder re-broadcasts it to every
                         * L3RTT subscriber including the originator,
                         * producing a forwarding loop that only ends when
                         * TTL hits 0. Confirmed on iw2ohx-gw 2026-05-10
                         * with v1.3.2 (now reverted).
                         */
                        flen = flexl3_build_info(frame, sizeof(frame),
                            in_hdr.source,                   /* dest = peer call (unicast) */
                            (unsigned char *)MYCALL,         /* source = our node call */
                            in_hdr.ttl,                      /* TTL echo — L3RTT class marker */
                            in_hdr.circuit_index,            /* IN echo — load-bearing */
                            in_hdr.circuit_id,               /* ID echo — load-bearing */
                            0, 0,                            /* connectionless — no S/R */
                            l3rtt, plen);
                    }
                    else
                    {
                        if ((size_t)plen <= sizeof(frame))
                        {
                            memcpy(frame, l3rtt, (size_t)plen);
                            flen = plen;
                        }
                    }
                }

                if (flen > 0)
                {
                    if (FLEXNET_DEBUG)
                        Consoleprintf("FlexNet: L3RTT reply -> %s "
                                      "c1=%u c2=%u c3=%u c4=%u reachable=%d wrap=%s",
                                      nbr,
                                      (unsigned int)peer_c1, (unsigned int)peer_c2,
                                      (unsigned int)reply_c3, (unsigned int)reply_c4,
                                      reachable, have_l3 ? "L3-INFO" : "bare");
                    FlexNet_Log("L3RTT-TX: -> %s peer_c1=%u peer_c2=%u "
                                "our_c3=%u our_c4=%u reachable=%d alias=%-6.6s "
                                "version=%s wrap=%s IN=%d ID=%d",
                                nbr,
                                (unsigned int)peer_c1, (unsigned int)peer_c2,
                                (unsigned int)reply_c3, (unsigned int)reply_c4,
                                reachable,
                                MYALIASTEXT,
                                FLEXNET_VERSION_PROTO,
                                have_l3 ? "L3-INFO" : "bare",
                                have_l3 ? in_hdr.circuit_index : -1,
                                have_l3 ? in_hdr.circuit_id    : -1);
                    flex_send_frame(LINK, FLEXNET_PID_CF, frame, flen);
                }
                else
                {
                    if (FLEXNET_DEBUG)
                        Consoleprintf("FlexNet: L3RTT build failed — dropping");
                    FlexNet_Log("L3RTT-DROP: build failed (plen=%d flen=%d wrap=%s) -> %s",
                                plen, flen, have_l3 ? "L3-INFO" : "bare", nbr);
                }
            }
            else
            {
                if (FLEXNET_DEBUG)
                    Consoleprintf("FlexNet: L3RTT parse failed from %s (len=%d) — dropping",
                                  nbr, len);
                FlexNet_Log("L3RTT-DROP: parse failed from %s (len=%d)", nbr, len);
            }
        }
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
            char tnbr[20] = {0};
            ConvFromAX25(sess->LINK->LINKCALL, tnbr);
            { int sl = strlen(tnbr); while (sl > 0 && tnbr[sl-1] == ' ') tnbr[--sl] = '\0'; }
            if (FLEXNET_DEBUG) Consoleprintf("FlexNet: sending keepalive + LT=%d to %s",
                        sess->our_link_time, tnbr);

            unsigned char ka[FLEXNET_KEEPALIVE_LEN];
            int klen = flex_build_keepalive(ka, sizeof(ka));
            if (klen > 0)
                flex_send_frame(sess->LINK, FLEXNET_PID_CE, ka, klen);

            /* Proactive LT (stamps lt_tx_tick). */
            flex_send_link_time(sess->LINK, sess);

            sess->last_keepalive = now;
        }
    }

    /* Expire timed-out L3RTT probes */
    for (int p = 0; p < FLEXNET_MAX_PROBES; p++)
    {
        if (FlexNetProbes[p].active &&
            (now - FlexNetProbes[p].sent_time) > FLEXNET_PROBE_TIMEOUT)
        {
            if (FLEXNET_DEBUG) Consoleprintf("FlexNet: L3RTT probe to %s timed out",
                        FlexNetProbes[p].target_call);
            FlexNetProbes[p].active = 0;
        }
    }
}

/* ── L3RTT Probe ────────────────────────────────────────────────────── */

static int flex_send_l3rtt_probe(int dest_idx,
    const char * target_call, int target_ssid)
{
    /* Find a free probe slot */
    struct FLEXNET_PROBE * probe = NULL;
    for (int i = 0; i < FLEXNET_MAX_PROBES; i++)
    {
        if (!FlexNetProbes[i].active)
        {
            probe = &FlexNetProbes[i];
            break;
        }
    }
    if (!probe) return -1;

    /* Find the session for this destination's port */
    int port = FlexNetDests[dest_idx].port;
    struct FLEXNET_SESSION * sess = NULL;
    for (int i = 0; i < FLEXNET_MAX_SESSIONS; i++)
    {
        if (FlexNetSessions[i].active && FlexNetSessions[i].port == port)
        {
            sess = &FlexNetSessions[i];
            break;
        }
    }
    if (!sess || !sess->LINK) return -1;

    /* Get our callsign */
    char mycall[20] = {0};
    if (sess->LINK->LINKPORT && sess->LINK->LINKPORT->PORTCALL[0])
    {
        ConvFromAX25(sess->LINK->LINKPORT->PORTCALL, mycall);
        int slen = strlen(mycall);
        while (slen > 0 && mycall[slen - 1] == ' ')
            mycall[--slen] = '\0';
    }
    if (!mycall[0]) return -1;

    /* Build target with SSID */
    char target_full[FLEXNET_MAX_CALLSIGN + 4];
    if (target_ssid >= 0)
        snprintf(target_full, sizeof(target_full), "%s-%d",
                 target_call, target_ssid);
    else
        snprintf(target_full, sizeof(target_full), "%s", target_call);

    /* Build L3RTT probe frame: L3RTT:<target>\r<our_call>\r */
    unsigned char frame[128];
    int flen = snprintf((char *)frame, sizeof(frame),
                        "L3RTT:%s\r%s\r", target_full, mycall);
    if (flen <= 0 || flen >= (int)sizeof(frame)) return -1;

    flex_send_frame(sess->LINK, FLEXNET_PID_CF, frame, flen);

    /* Record pending probe */
    memset(probe, 0, sizeof(*probe));
    probe->active = 1;
    strncpy(probe->target_call, target_call, FLEXNET_MAX_CALLSIGN - 1);
    probe->target_ssid = target_ssid;
    probe->dest_index = dest_idx;
    probe->sent_time = time(NULL);
    probe->got_reply = FALSE;

    if (FLEXNET_DEBUG) Consoleprintf("FlexNet: L3RTT probe sent for %s", target_full);
    return 0;
}

static int flex_check_probe_reply(unsigned char * data, int len,
                                  LINKTABLE * LINK)
{
    /* Parse after "L3RTT:" prefix — extract target callsign */
    const char * p = (const char *)(data + 6);  /* skip "L3RTT:" */
    int remaining = len - 6;

    /* Target callsign is first field before \r */
    char target[FLEXNET_MAX_CALLSIGN + 4] = {0};
    int ti = 0;
    while (ti < remaining && ti < (int)sizeof(target) - 1 &&
           p[ti] != '\r' && p[ti] != '\0')
    {
        target[ti] = p[ti];
        ti++;
    }
    target[ti] = '\0';

    /* Strip SSID from target for matching */
    char target_base[FLEXNET_MAX_CALLSIGN] = {0};
    strncpy(target_base, target, FLEXNET_MAX_CALLSIGN - 1);
    char * dash = strchr(target_base, '-');
    if (dash) *dash = '\0';

    /* Search pending probes for a match */
    for (int i = 0; i < FLEXNET_MAX_PROBES; i++)
    {
        struct FLEXNET_PROBE * probe = &FlexNetProbes[i];
        if (!probe->active) continue;
        if (strcasecmp(probe->target_call, target_base) != 0) continue;

        /* Match found — parse hops from the reply */
        /* Skip past target\r to get to hop list */
        const char * hp = p + ti;
        int hrem = remaining - ti;
        if (hrem > 0 && *hp == '\r') { hp++; hrem--; }

        probe->reply_hop_count = 0;

        while (hrem > 0 && probe->reply_hop_count < FLEXNET_MAX_PATH_HOPS)
        {
            /* Skip whitespace/CR */
            while (hrem > 0 && (*hp == '\r' || *hp == '\n' || *hp == ' '))
            { hp++; hrem--; }
            if (hrem <= 0) break;

            /* Read hop callsign until \r or end */
            char hop[FLEXNET_MAX_CALLSIGN] = {0};
            int hi = 0;
            while (hrem > 0 && hi < FLEXNET_MAX_CALLSIGN - 1 &&
                   *hp != '\r' && *hp != '\n' && *hp != '\0')
            {
                hop[hi++] = *hp++;
                hrem--;
            }
            hop[hi] = '\0';

            /* Trim trailing spaces */
            while (hi > 0 && hop[hi - 1] == ' ')
                hop[--hi] = '\0';

            if (hi > 0)
            {
                strncpy(probe->reply_hops[probe->reply_hop_count],
                        hop, FLEXNET_MAX_CALLSIGN - 1);
                probe->reply_hop_count++;
            }
        }

        probe->got_reply = TRUE;

        /* Copy path into destination entry cache */
        if (probe->dest_index >= 0 &&
            probe->dest_index < FlexNetDestCount)
        {
            struct FLEXNET_DEST_ENTRY * dest =
                &FlexNetDests[probe->dest_index];
            dest->path_len = probe->reply_hop_count;
            for (int h = 0; h < probe->reply_hop_count; h++)
                strncpy(dest->path_hops[h], probe->reply_hops[h],
                        FLEXNET_MAX_CALLSIGN - 1);
            dest->path_updated = time(NULL);
        }

        if (FLEXNET_DEBUG) Consoleprintf("FlexNet: L3RTT reply for %s, %d hops",
                    target, probe->reply_hop_count);
        probe->active = 0;
        return 1;  /* handled */
    }

    return 0;  /* not our probe */
}

/* ── D Command: Detail View ─────────────────────────────────────────── */

static void flex_show_dest_detail(TRANSPORTENTRY * Session,
    char ** Bufferptr_p, int dest_idx,
    const char * query_call, int query_ssid)
{
    struct FLEXNET_DEST_ENTRY * e = &FlexNetDests[dest_idx];

    /* Header: *** CALL  (lo-hi) T=rtt */
    *Bufferptr_p = Cmdprintf(Session, *Bufferptr_p,
        "*** %s  (%d-%d) T=%d\r",
        e->callsign, e->ssid_lo, e->ssid_hi, e->rtt);

    /* Check for cached path */
    time_t now = time(NULL);
    if (e->path_len > 0 &&
        (now - e->path_updated) < FLEXNET_PATH_CACHE_TTL)
    {
        /* Show cached path */
        *Bufferptr_p = Cmdprintf(Session, *Bufferptr_p, "*** route:");
        for (int h = 0; h < e->path_len; h++)
            *Bufferptr_p = Cmdprintf(Session, *Bufferptr_p,
                " %s", e->path_hops[h]);
        *Bufferptr_p = Cmdprintf(Session, *Bufferptr_p, "\r");
    }
    else
    {
        /* No cached path — show via and send probe */
        if (e->via_callsign[0])
            *Bufferptr_p = Cmdprintf(Session, *Bufferptr_p,
                "*** route: via %s\r", e->via_callsign);

        /* Send L3RTT probe */
        if (flex_send_l3rtt_probe(dest_idx, query_call, query_ssid) == 0)
            *Bufferptr_p = Cmdprintf(Session, *Bufferptr_p,
                "    L3RTT probe sent. Re-issue D %s to see path.\r",
                e->callsign);
    }
}

/* ── D Command Handler ───────────────────────────────────────────────── */

void FlexNet_CmdDest(TRANSPORTENTRY * Session, char * Bufferptr,
                     char * CmdTail, struct CMDX * CMD)
{
    char callsign_filter[FLEXNET_MAX_CALLSIGN] = {0};
    int  have_filter = 0;

    /* Optional callsign filter: "D IW2OHX" or "D IW*" or "D W4MLB-1" */
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

    /* ── Specific destination query (no wildcard) ────────────── */

    if (have_filter && strchr(callsign_filter, '*') == NULL)
    {
        /* Parse optional SSID: "W4MLB-1" -> call=W4MLB, ssid=1 */
        char query_call[FLEXNET_MAX_CALLSIGN] = {0};
        int  query_ssid = -1;
        char * dash = strchr(callsign_filter, '-');
        if (dash)
        {
            int clen = (int)(dash - callsign_filter);
            if (clen > 0 && clen < FLEXNET_MAX_CALLSIGN)
            {
                strncpy(query_call, callsign_filter, clen);
                query_call[clen] = '\0';
                query_ssid = atoi(dash + 1);
            }
        }
        else
        {
            strncpy(query_call, callsign_filter,
                    FLEXNET_MAX_CALLSIGN - 1);
        }

        /* Find exact match in dest table */
        int found_idx = -1;
        for (int i = 0; i < FlexNetDestCount; i++)
        {
            struct FLEXNET_DEST_ENTRY * e = &FlexNetDests[i];
            if (e->rtt >= FLEXNET_RTT_INFINITY) continue;
            if (strcasecmp(e->callsign, query_call) != 0) continue;
            if (query_ssid >= 0 &&
                (query_ssid < e->ssid_lo || query_ssid > e->ssid_hi))
                continue;
            found_idx = i;
            break;
        }

        if (found_idx >= 0)
        {
            flex_show_dest_detail(Session, &Bufferptr, found_idx,
                                  query_call, query_ssid);
            SendCommandReply(Session, REPLYBUFFER,
                (int)(Bufferptr - (char *)REPLYBUFFER));
            return;
        }
        /* Not found as exact match — fall through to list mode
           using callsign_filter as prefix match */
    }

    /* ── Wildcard / list mode ────────────────────────────────── */

    int  match_mode = 0;  /* 0=prefix, 1=suffix, 2=substring */
    char match_str[FLEXNET_MAX_CALLSIGN] = {0};

    if (have_filter)
    {
        int flen = strlen(callsign_filter);

        /* Bare "*" means show all */
        if (flen == 1 && callsign_filter[0] == '*')
        {
            have_filter = 0;
        }
        else
        {
        int has_leading  = (callsign_filter[0] == '*');
        int has_trailing = (flen > 0 && callsign_filter[flen - 1] == '*');

        if (has_leading && has_trailing && flen > 2)
        {
            /* *HU* = substring match */
            match_mode = 2;
            int mi = 0;
            for (int i = 1; i < flen - 1 && mi < FLEXNET_MAX_CALLSIGN - 1; i++)
                match_str[mi++] = callsign_filter[i];
            match_str[mi] = '\0';
        }
        else if (has_leading && flen > 1)
        {
            /* *MLB = suffix match */
            match_mode = 1;
            strncpy(match_str, callsign_filter + 1,
                    FLEXNET_MAX_CALLSIGN - 1);
        }
        else if (has_trailing && flen > 1)
        {
            /* IW* = prefix match (strip *) */
            match_mode = 0;
            strncpy(match_str, callsign_filter, flen - 1);
            match_str[flen - 1] = '\0';
        }
        else
        {
            /* IW2OHX = plain prefix match */
            match_mode = 0;
            strncpy(match_str, callsign_filter,
                    FLEXNET_MAX_CALLSIGN - 1);
        }
        }  /* end else (not bare "*") */
    }

    Bufferptr = Cmdprintf(Session, Bufferptr,
        "FlexNet Destinations:\r");
    Bufferptr = Cmdprintf(Session, Bufferptr,
        "Dest     SSID    RTT Via\r");
    Bufferptr = Cmdprintf(Session, Bufferptr,
        "-------- ----- ----- -----------\r");

    int shown = 0;

    for (int i = 0; i < FlexNetDestCount; i++)
    {
        struct FLEXNET_DEST_ENTRY * e = &FlexNetDests[i];
        if (e->rtt >= FLEXNET_RTT_INFINITY) continue;
        if (e->callsign[0] == '\0') continue;

        if (have_filter)
        {
            int skip = 0;
            int mlen = strlen(match_str);

            switch (match_mode)
            {
            case 0: /* prefix */
                if (strncasecmp(e->callsign, match_str, mlen) != 0)
                    skip = 1;
                break;
            case 1: /* suffix */
            {
                int clen = strlen(e->callsign);
                if (clen < mlen ||
                    strcasecmp(e->callsign + clen - mlen, match_str) != 0)
                    skip = 1;
                break;
            }
            case 2: /* substring */
            {
                char uc[FLEXNET_MAX_CALLSIGN], um[FLEXNET_MAX_CALLSIGN];
                int ci;
                for (ci = 0; e->callsign[ci] && ci < FLEXNET_MAX_CALLSIGN - 1; ci++)
                    uc[ci] = toupper((unsigned char)e->callsign[ci]);
                uc[ci] = '\0';
                for (ci = 0; match_str[ci] && ci < FLEXNET_MAX_CALLSIGN - 1; ci++)
                    um[ci] = toupper((unsigned char)match_str[ci]);
                um[ci] = '\0';
                if (strstr(uc, um) == NULL)
                    skip = 1;
                break;
            }
            }
            if (skip) continue;
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
            "(no matching destinations)\r");
    else
        Bufferptr = Cmdprintf(Session, Bufferptr,
            "\r%d destinations\r", shown);

    SendCommandReply(Session, REPLYBUFFER,
        (int)(Bufferptr - (char *)REPLYBUFFER));
}

/* ── FL Command Handler ─────────────────────────────────────────────── */

static void flex_format_uptime(time_t elapsed, char * buf, int buflen)
{
    if (elapsed < 0) elapsed = 0;

    int days = (int)(elapsed / 86400);
    int hrs  = (int)((elapsed % 86400) / 3600);
    int mins = (int)((elapsed % 3600) / 60);
    int secs = (int)(elapsed % 60);

    if (days > 0)
        snprintf(buf, buflen, "%dd %02d:%02d", days, hrs, mins);
    else
        snprintf(buf, buflen, "%02d:%02d:%02d", hrs, mins, secs);
}

void FlexNet_CmdLinks(TRANSPORTENTRY * Session, char * Bufferptr,
                      char * CmdTail, struct CMDX * CMD)
{
    int shown = 0;

    Bufferptr = Cmdprintf(Session, Bufferptr,
        "FlexNet Links:\r");
    Bufferptr = Cmdprintf(Session, Bufferptr,
        "Link         Port  Status     LT     KA     Uptime      Routes\r");
    Bufferptr = Cmdprintf(Session, Bufferptr,
        "------------ ----  ---------  ------ -----  ----------  ------\r");

    for (int i = 0; i < FLEXNET_MAX_SESSIONS; i++)
    {
        struct FLEXNET_SESSION * sess = &FlexNetSessions[i];
        if (!sess->active || !sess->LINK) continue;

        /* Decode neighbor callsign */
        char normcall[20] = {0};
        ConvFromAX25(sess->LINK->LINKCALL, normcall);
        int slen = strlen(normcall);
        while (slen > 0 && normcall[slen - 1] == ' ')
            normcall[--slen] = '\0';

        /* Determine status */
        const char * status;
        if (sess->got_peer_init && sess->sent_routes)
            status = "CONNECTED";
        else if (sess->got_peer_init)
            status = "INIT";
        else
            status = "PENDING";

        /* Uptime */
        char uptime_str[16];
        flex_format_uptime(time(NULL) - sess->session_start,
                           uptime_str, sizeof(uptime_str));

        /* Count routes from this neighbor */
        int route_count = 0;
        for (int j = 0; j < FlexNetDestCount; j++)
        {
            if (FlexNetDests[j].port == sess->port &&
                FlexNetDests[j].rtt < FLEXNET_RTT_INFINITY)
                route_count++;
        }

        /* Format link time: ticks (100ms units) → seconds */
        char lt_str[8];
        long lt_ms = sess->peer_link_time * 100;
        if (lt_ms >= 1000)
            snprintf(lt_str, sizeof(lt_str), "%lds", lt_ms / 1000);
        else
            snprintf(lt_str, sizeof(lt_str), "0.%lds", lt_ms / 100);

        Bufferptr = Cmdprintf(Session, Bufferptr,
            "%-12s %-4d  %-9s  %-6s %-5d  %-10s  %d\r",
            normcall, sess->port, status,
            lt_str, sess->keepalive_count,
            uptime_str, route_count);
        shown++;
    }

    if (shown == 0)
        Bufferptr = Cmdprintf(Session, Bufferptr,
            "(no active FlexNet links)\r");

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

/* Resolve neighbor callsign from port number */
static void flex_get_neighbor_call(int port, char * buf, int buflen)
{
    buf[0] = '\0';
    for (int i = 0; i < FLEXNET_MAX_SESSIONS; i++)
    {
        struct FLEXNET_SESSION * s = &FlexNetSessions[i];
        if (s->active && s->port == port && s->LINK)
        {
            ConvFromAX25(s->LINK->LINKCALL, buf);
            /* Trim trailing spaces */
            int slen = strlen(buf);
            while (slen > 0 && buf[slen - 1] == ' ')
                buf[--slen] = '\0';
            return;
        }
    }
}

static int flex_find_dest(const char * call, int ssid_lo, int ssid_hi)
{
    for (int i = 0; i < FlexNetDestCount; i++)
    {
        if (strcasecmp(FlexNetDests[i].callsign, call) == 0 &&
            FlexNetDests[i].ssid_lo == ssid_lo &&
            FlexNetDests[i].ssid_hi == ssid_hi)
            return i;
    }
    return -1;
}

static int flex_dtable_merge(struct FLEXNET_DEST_ENTRY * incoming, int port)
{
    /* Item #6 — RTT=0 refresh-marker skip (matches flexnetd v0.7.5
       dtable.c:50-75). xnet sends its dtable in two rounds after
       session init: real RTTs first, then ~20 s later RTT=0 for every
       record as a refresh/keepalive marker. Without this guard the
       refresh round overwrites measured RTTs with 0, leaving the
       D-table user-display full of RTT=0 rows.

       Withdrawn-route broadcasts arrive with rtt=FLEXNET_RTT_INFINITY
       AND is_infinity=1 — they must NOT be swallowed by this skip, so
       gate on !incoming->is_infinity. */
    if (incoming->rtt == 0 && !incoming->is_infinity)
    {
        int idx = flex_find_dest(incoming->callsign,
                                 incoming->ssid_lo, incoming->ssid_hi);
        if (idx >= 0)
            FlexNetDests[idx].last_updated = time(NULL);
        return 0;
    }

    /* Resolve neighbor callsign for via field */
    char via[FLEXNET_MAX_CALLSIGN] = {0};
    flex_get_neighbor_call(port, via, sizeof(via));

    int idx = flex_find_dest(incoming->callsign,
                             incoming->ssid_lo, incoming->ssid_hi);
    if (idx >= 0)
    {
        /* Update if better RTT or same source */
        if (incoming->rtt < FlexNetDests[idx].rtt ||
            FlexNetDests[idx].port == port)
        {
            FlexNetDests[idx].rtt          = incoming->rtt;
            FlexNetDests[idx].is_infinity  = incoming->is_infinity;
            FlexNetDests[idx].port         = port;
            FlexNetDests[idx].last_updated = time(NULL);
            if (via[0])
                strncpy(FlexNetDests[idx].via_callsign, via,
                        FLEXNET_MAX_CALLSIGN - 1);
        }
        return 2;  /* updated */
    }

    /* New entry */
    if (FlexNetDestCount < FLEXNET_MAX_DESTS)
    {
        memcpy(&FlexNetDests[FlexNetDestCount], incoming,
               sizeof(struct FLEXNET_DEST_ENTRY));
        FlexNetDests[FlexNetDestCount].port = port;
        FlexNetDests[FlexNetDestCount].last_updated = time(NULL);
        if (via[0])
            strncpy(FlexNetDests[FlexNetDestCount].via_callsign, via,
                    FLEXNET_MAX_CALLSIGN - 1);
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

/* ── Link-time IIR filter (item #5) ──────────────────────────────────── */
/*
 * Samples come from CE LT round-trips: we stamp lt_tx_tick on every LT
 * we send; on the peer's next CE LT (their reply) we fold the delta
 * into lt_smoothed_10ms with the 3:1 IIR flexnetd uses at
 * poll_cycle.c:864-868. Smoothed value, rounded to 100ms wire units
 * and clamped to PCFlexnet's 12-bit cap, becomes our_link_time for
 * subsequent outbound LT frames. Only one outstanding pending LT per
 * session — a fresh TX overwrites an unmatched stamp.
 */

static int flex_send_link_time(LINKTABLE * LINK,
                               struct FLEXNET_SESSION * sess)
{
    unsigned char lt[16];
    int ltlen = flex_build_link_time(lt, sizeof(lt), sess->our_link_time);
    if (ltlen <= 0) return -1;

    flex_send_frame(LINK, FLEXNET_PID_CE, lt, ltlen);

    sess->lt_tx_tick    = flex_get_ticks_10ms();
    sess->lt_tx_pending = TRUE;
    return ltlen;
}

static void flex_link_time_sample(struct FLEXNET_SESSION * sess)
{
    if (!sess->lt_tx_pending) return;

    uint32_t now    = flex_get_ticks_10ms();
    uint32_t sample = now - sess->lt_tx_tick;   /* wrap-safe uint32 sub */

    /* Discard sample==0 (clock-granularity edge) and >60s (mis-pairing
       or process stall). 6000 ticks = 60s at 10ms granularity. */
    if (sample == 0 || sample > 6000U)
    {
        sess->lt_tx_pending = FALSE;
        return;
    }

    if (sess->lt_sample_count == 0)
        sess->lt_smoothed_10ms = sample;                          /* seed */
    else
        sess->lt_smoothed_10ms =
            (sess->lt_smoothed_10ms * 3U + sample) / 4U;          /* IIR  */
    sess->lt_sample_count++;

    /* 10ms ticks → 100ms wire units, round to nearest, clamp [1, 4095].
       "10\r" on the wire is CE_FRAME_STATUS_10, a different frame type
       — so the floor must be 1, not 0. */
    uint32_t wire = (sess->lt_smoothed_10ms + 5U) / 10U;
    if (wire < 1U)    wire = 1U;
    if (wire > 4095U) wire = 4095U;
    sess->our_link_time = (int)wire;

    sess->lt_tx_pending = FALSE;

    if (FLEXNET_DEBUG)
    {
        char nbr[20] = {0};
        ConvFromAX25(sess->LINK->LINKCALL, nbr);
        { int sl = (int)strlen(nbr);
          while (sl > 0 && nbr[sl-1] == ' ') nbr[--sl] = '\0'; }
        Consoleprintf("FlexNet: lt_sample peer=%s sample=%u smoothed=%u "
                      "wire=%u (n=%u)", nbr,
                      (unsigned)sample,
                      (unsigned)sess->lt_smoothed_10ms,
                      (unsigned)wire,
                      (unsigned)sess->lt_sample_count);
    }
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

    /* Use the NODE callsign (MYCALL) as our FlexNet identity */
    char mycall[20] = {0};
    int my_ssid = 0;

    ConvFromAX25(MYCALL, mycall);

    /* Trim trailing spaces */
    int slen = strlen(mycall);
    while (slen > 0 && mycall[slen - 1] == ' ')
        mycall[--slen] = '\0';

    /* Parse SSID from "IW2OHX-13" */
    char * dash = strchr(mycall, '-');
    if (dash)
    {
        my_ssid = atoi(dash + 1);
        *dash = '\0';  /* base call only for route record */
    }

    if (mycall[0])
    {
        unsigned char route[32];
        int rlen = flex_build_route(route, sizeof(route),
                                    mycall, my_ssid, my_ssid, 1);
        if (rlen > 0)
            flex_send_frame(LINK, FLEXNET_PID_CE, route, rlen);
    }

    /* Release token */
    unsigned char rel[] = { '3', '-', '\r' };
    flex_send_frame(LINK, FLEXNET_PID_CE, rel, 3);

    /* Decode neighbor for logging */
    char nbr[20] = {0};
    ConvFromAX25(LINK->LINKCALL, nbr);
    slen = strlen(nbr);
    while (slen > 0 && nbr[slen - 1] == ' ') nbr[--slen] = '\0';

    Consoleprintf("FlexNet: advertising %s (%d-%d) RTT=1 to %s",
                mycall, my_ssid, my_ssid, nbr);
}

/* ── Incoming Connection Check ──────────────────────────────────────── */
/*
 * Called from L2Code.c NOTFORUS path when a SABM doesn't match any
 * configured callsign. Accepts the connection if addressed to MYCALL.
 * This handles the case where the port has PORTL3FLAG set (L3-only),
 * which skips the normal MYCALL check at line 426.
 */

BOOL FlexNet_CheckIncoming(PPORTCONTROL PORT, unsigned char * dest)
{
    /* Accept any SABM addressed to our node callsign */
    if (memcmp(dest, MYCALL, 7) != 0)
        return FALSE;

    char caller[20] = {0};
    ConvFromAX25(dest, caller);
    { int sl = strlen(caller); while (sl > 0 && caller[sl-1] == ' ') caller[--sl] = '\0'; }
    Consoleprintf("FlexNet: accepting incoming L2 to %s on port %d",
                  caller, PORT->PORTNUMBER);
    return TRUE;
}

/* ── Outgoing Connection Route Lookup ───────────────────────────────── */
/*
 * Called from Cmd.c connect handler when no NET/ROM route is found.
 * Searches FlexNet destination table for the callsign. Returns the
 * port number of the FlexNet session that can reach it, or 0 if
 * not found.
 */

int FlexNet_FindRoute(unsigned char * axcall)
{
    /* Decode the target callsign from AX.25 format */
    char target[20] = {0};
    ConvFromAX25(axcall, target);
    { int sl = strlen(target); while (sl > 0 && target[sl-1] == ' ') target[--sl] = '\0'; }

    /* Parse callsign and SSID */
    char target_base[FLEXNET_MAX_CALLSIGN] = {0};
    int target_ssid = -1;
    strncpy(target_base, target, FLEXNET_MAX_CALLSIGN - 1);
    char * dash = strchr(target_base, '-');
    if (dash)
    {
        target_ssid = atoi(dash + 1);
        *dash = '\0';
    }

    if (FLEXNET_DEBUG) Consoleprintf("FlexNet: FindRoute called for '%s' base='%s' "
                  "ssid=%d (table has %d entries)",
                  target, target_base, target_ssid, FlexNetDestCount);

    /* Search FlexNet destination table */
    for (int i = 0; i < FlexNetDestCount; i++)
    {
        struct FLEXNET_DEST_ENTRY * e = &FlexNetDests[i];
        if (e->rtt >= FLEXNET_RTT_INFINITY) continue;
        if (strcasecmp(e->callsign, target_base) != 0) continue;

        /* If SSID specified, check range */
        if (target_ssid >= 0)
        {
            if (target_ssid < e->ssid_lo || target_ssid > e->ssid_hi)
                continue;
        }

        /* Found — return the port of the FlexNet session */
        Consoleprintf("FlexNet: routing C %s via FlexNet port %d "
                      "(RTT=%d)", target, e->port, e->rtt);
        return e->port;
    }

    if (FLEXNET_DEBUG) Consoleprintf("FlexNet: FindRoute — '%s' not found in dest table",
                  target_base);
    return 0;  /* not a FlexNet destination */
}

/* ── Get FlexNet Neighbor AX.25 Callsign ────────────────────────────── */
/*
 * Returns the AX.25-encoded callsign (7 bytes) of the FlexNet neighbor
 * on the given port. Used by Cmd.c to add the neighbor as a digipeater
 * when routing outgoing connections through FlexNet.
 */

BOOL FlexNet_GetNeighborCall(int port, unsigned char * axcall_out)
{
    for (int i = 0; i < FLEXNET_MAX_SESSIONS; i++)
    {
        struct FLEXNET_SESSION * sess = &FlexNetSessions[i];
        if (sess->active && sess->port == port && sess->LINK)
        {
            memcpy(axcall_out, sess->LINK->LINKCALL, 7);
            return TRUE;
        }
    }
    return FALSE;
}
