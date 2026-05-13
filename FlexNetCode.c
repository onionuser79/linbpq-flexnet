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
#include <errno.h>
#include <unistd.h>   /* unlink() for the path-cache atomic-rename save */

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
#define FLEXNET_VERSION_STR   "v1.9.5"
#define FLEXNET_VERSION_PROTO "linbpq-1.9"

const char FlexNetVersion[] = FLEXNET_VERSION_STR;

/* These may not be in the header — define if missing */
#ifndef FLEXNET_MAX_SESSIONS
#define FLEXNET_MAX_SESSIONS  8
#endif
#ifndef FLEXNET_MAX_PATH_HOPS
#define FLEXNET_MAX_PATH_HOPS 16
#endif
#define FLEXNET_MAX_PROBES         4
#define FLEXNET_MAX_PATH_PROBES    8   /* CE type-6 outstanding probes */
#define FLEXNET_PATH_CACHE_TTL  14400  /* 4h — covers a full round-robin probe
                                          cycle. With ~190 dests at 60s/probe
                                          the cycle is ~3h, so 4h leaves
                                          headroom and avoids re-rendering
                                          stale fallback paths between probes
                                          for the same target. (item #9 partial) */
#define FLEXNET_PROBE_TIMEOUT     15   /* seconds before probe times out */
#define FLEXNET_PATH_PROBE_TIMEOUT 15  /* seconds before CE type-6 probe times out */
#define FLEXNET_PATH_PROBE_INTERVAL 60 /* seconds between background path probes (item #10) */

/* On-disk path cache (v2.x item #1).
   Eliminates the ~3h post-restart re-probe warm-up by reloading the
   last-known path_hops[] + path_updated from a flat file. Entries
   older than FLEXNET_PATH_CACHE_PERSIST_TTL are skipped on load.
   Saved periodically (every FLEXNET_PATH_CACHE_SAVE_INTERVAL seconds)
   from FlexNet_Timer when at least one entry has changed since the
   last save — bounds disk writes without depending on a clean
   shutdown hook. */
#define FLEXNET_PATH_CACHE_FILE         "flexnet_path_cache.dat"
#define FLEXNET_PATH_CACHE_PERSIST_TTL  (5 * 3600)  /* 5h freshness window */
#define FLEXNET_PATH_CACHE_SAVE_INTERVAL 300        /* 5 min */

/* v2.x #3 — multi-FlexNet-neighbour bootstrap.
   Scan all connected L2 links every FLEXNET_PROACTIVE_INIT_INTERVAL
   seconds and send CE init to any FlexNet-mapped peer that doesn't
   yet have a FlexNet session. Without this, two peers can stall
   waiting for each other to send the first CE frame. */
#define FLEXNET_PROACTIVE_INIT_INTERVAL 30

/* v1.9.4 — transit-role D-table re-advertisement.
   Period between proactive re-advertisements of our learned routes to
   each FlexNet neighbour. Each cycle we send a CE compact batch to
   every active session, filtered by split-horizon (skip routes whose
   chosen via_session_idx is the target neighbour itself). RTT is
   adjusted by our_link_time so peers see the real cost through us.
   Without this, neighbours never learn destinations reachable through
   us — they only see linbpq as a leaf. Matches the protocol pattern
   used by xnet at IW2OHX-14 (we receive equivalent batches every
   few minutes). */
#define FLEXNET_READVERTISE_INTERVAL  300   /* 5 min */
#define FLEXNET_BATCH_MAX_BYTES       200   /* per-frame cap */

/* CE type-6/7 wire constants (matches flexnetd/ce_proto.c) */
#define CE_PATH_HOP_BYTE_BASE  0x20   /* hop_byte = base + hop_count */
#define CE_PATH_QSO_FIELD_LEN  5      /* fixed 5-char ASCII numeric */
#define CE_PATH_TRACE_BIT      0x40   /* high bit on QSO field byte 0 */
#define CE_PATH_KIND_ROUTE     0
#define CE_PATH_KIND_TRACE     1

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
    int  via_session_idx;   /* index into FlexNetSessions[]; -1 = unknown */
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

/* CE type-6/7 outstanding-probe table (item #7+#8, v1.4.0). */
struct FLEXNET_PATH_PROBE
{
    BOOL    active;
    int     qso;                                  /* 1..65535, 0 = free */
    int     trace;                                /* 1 if TRACE-kind */
    char    target_call[FLEXNET_MAX_CALLSIGN];
    int     target_ssid;
    int     dest_index;                           /* into FlexNetDests[] */
    time_t  sent_time;
    char    reply_hops[FLEXNET_MAX_PATH_HOPS][FLEXNET_MAX_CALLSIGN];
    int     reply_hop_count;
    BOOL    got_reply;
};

struct FLEXNET_PATH_PROBE FlexNetPathProbes[FLEXNET_MAX_PATH_PROBES];
static uint16_t g_path_qso_counter = 0;

/* Item #10 — background path probing state.
   Round-robin index into FlexNetDests[] + last-probe timestamp. */
static int    g_path_probe_idx  = 0;
static time_t g_last_path_probe = 0;

/* v2.x #1 — on-disk path cache state.
   See flex_path_cache_save / flex_path_cache_load near
   flex_show_dest_detail for implementation. */
static int     g_path_cache_dirty     = 0;
static time_t  g_path_cache_last_save = 0;
static int     g_path_cache_loaded    = 0;
static int     flex_path_cache_load(void);
static int     flex_path_cache_save(void);

/* v2.x #3 — multi-neighbour routing. Single-shot cache populated by
   FlexNet_FindRoute and consumed by the next FlexNet_GetNeighborCall
   so that connect-path routing picks the cost-selected session even
   when multiple FlexNet neighbours share a BPQ port. -1 = no recent
   FindRoute on record. */
static int     g_findroute_last_dest  = -1;
static time_t  g_last_proactive_init_scan = 0;

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
static int  flex_dtable_merge(struct FLEXNET_DEST_ENTRY * incoming,
                              struct FLEXNET_SESSION * sess);
static int  flex_find_dest(const char * call, int ssid_lo, int ssid_hi);
static struct FLEXNET_SESSION * flex_find_session(LINKTABLE * LINK);
static void flex_send_frame(LINKTABLE * LINK, unsigned char pid,
                unsigned char * data, int len);
static void flex_send_own_routes(LINKTABLE * LINK, int port);
static int  flex_send_routes_to(struct FLEXNET_SESSION * sess, int mode);
static time_t g_last_readvertise = 0;
static void flex_get_neighbor_call(int port, char * buf, int buflen);
static int  flex_send_l3rtt_probe(int dest_idx,
                const char * target_call, int target_ssid);
static int  flex_check_probe_reply(unsigned char * data, int len,
                LINKTABLE * LINK);
/* CE type-6/7 (item #7+#8, v1.4.0) */
static int  flex_parse_path_frame(const unsigned char * data, int len,
                int * out_is_reply,
                int * out_qso, int * out_trace, int * out_hop_count,
                char * out_origin,
                char  out_hops[][FLEXNET_MAX_CALLSIGN],
                int * out_n_hops);
static int  flex_build_path_req(unsigned char * buf, int buflen,
                int qso, int trace,
                const char * origin, const char * next_hop,
                const char * target);
static int  flex_build_path_rep(unsigned char * buf, int buflen,
                int qso, int trace,
                const char * const * hops, int n_hops);
static void flex_handle_path_req(LINKTABLE * LINK,
                struct FLEXNET_SESSION * sess,
                unsigned char * data, int len);
static void flex_handle_path_rep(LINKTABLE * LINK,
                struct FLEXNET_SESSION * sess,
                unsigned char * data, int len);
static int  flex_send_path_req(int dest_idx,
                const char * target_call, int target_ssid);
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
#define CE_FRAME_PATH_REQ     8   /* wire byte '6' (0x36) — was DEST_BCAST */
#define CE_FRAME_INIT         9
#define CE_FRAME_PATH_REP    10   /* wire byte '7' (0x37) — type-7 PATH_REPLY */

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

    /* Identity rule for multi-neighbour:
        1. Match by LINK pointer first (same neighbour, same LINK):
           refresh in place.
        2. Otherwise, match by neighbour callsign on this port. If
           found, the old L2 link dropped and reconnected with a
           fresh LINK pointer — update the slot's LINK in place so
           we don't leave an orphan session pointing at the stale
           LINK (which would show up in FL as a ghost row and skew
           cost-based attribution).
        3. Otherwise, allocate a new slot. */
    for (int i = 0; i < FLEXNET_MAX_SESSIONS; i++)
    {
        if (FlexNetSessions[i].active && FlexNetSessions[i].LINK == LINK)
        {
            FlexNetSessions[i].sent_routes = FALSE;  /* re-advertise */
            FlexNetSessions[i].got_peer_init = FALSE;
            FlexNetSessions[i].keepalive_count = 0;
            FlexNetSessions[i].session_start = time(NULL);
            FlexNetSessions[i].last_keepalive = time(NULL);
            LINK->FlexNetLink = TRUE;

            int node_ssid = (MYCALL[6] >> 1) & 0x0F;
            unsigned char init[8];
            int ilen = flex_build_init(init, sizeof(init), node_ssid);
            if (ilen > 0)
                flex_send_frame(LINK, FLEXNET_PID_CE, init, ilen);

            unsigned char ka[FLEXNET_KEEPALIVE_LEN];
            int klen = flex_build_keepalive(ka, sizeof(ka));
            if (klen > 0)
                flex_send_frame(LINK, FLEXNET_PID_CE, ka, klen);

            Consoleprintf("FlexNet: session reconnected on port %d "
                          "(same LINK, re-sent init + keepalive)", Port);
            return;
        }
    }
    for (int i = 0; i < FLEXNET_MAX_SESSIONS; i++)
    {
        if (FlexNetSessions[i].active && FlexNetSessions[i].port == Port &&
            FlexNetSessions[i].LINK && FlexNetSessions[i].LINK != LINK &&
            memcmp(FlexNetSessions[i].LINK->LINKCALL,
                   LINK->LINKCALL, 7) == 0)
        {
            FlexNetSessions[i].LINK->FlexNetLink = FALSE; /* old LINK demoted */
            FlexNetSessions[i].LINK = LINK;
            FlexNetSessions[i].sent_routes = FALSE;
            FlexNetSessions[i].got_peer_init = FALSE;
            FlexNetSessions[i].keepalive_count = 0;
            FlexNetSessions[i].session_start = time(NULL);
            FlexNetSessions[i].last_keepalive = time(NULL);
            LINK->FlexNetLink = TRUE;

            int node_ssid = (MYCALL[6] >> 1) & 0x0F;
            unsigned char init[8];
            int ilen = flex_build_init(init, sizeof(init), node_ssid);
            if (ilen > 0)
                flex_send_frame(LINK, FLEXNET_PID_CE, init, ilen);

            unsigned char ka[FLEXNET_KEEPALIVE_LEN];
            int klen = flex_build_keepalive(ka, sizeof(ka));
            if (klen > 0)
                flex_send_frame(LINK, FLEXNET_PID_CE, ka, klen);

            Consoleprintf("FlexNet: session reconnected on port %d "
                          "(new LINK for same callsign, slot %d updated)",
                          Port, i);
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

        flex_dtable_merge(&nbr_entry, sess);
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
            int rc = flex_dtable_merge(&entries[i], sess);
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

    case CE_FRAME_PATH_REQ:
        flex_handle_path_req(LINK, sess, data, len);
        break;

    case CE_FRAME_PATH_REP:
        flex_handle_path_rep(LINK, sess, data, len);
        break;

    default:
        if (FLEXNET_DEBUG) Consoleprintf("FlexNet CE: unknown frame type from %s "
                    "(byte0=0x%02X, len=%d)", nbr, data[0], len);
        break;
    }

    ReleaseBuffer(Buffer);
}

/* ── CF Frame Processing (L3RTT) ─────────────────────────────────────── */
/*
 * Returns 1 if the frame was an L3RTT probe/reply that we handled (caller
 * must NOT re-process it), 0 if the frame is NOT L3RTT and the caller
 * should fall through to the normal NetROM L3/L4 dispatch. v1.9.5: this
 * is the fix for "C <flexnet-neighbour> returns 0 bytes" — NetROM L4 also
 * uses pid=CF, so unconditionally swallowing every CF frame on a FlexNet
 * link drops CACK/INFO during a user-originated connect.
 */
int FlexNet_ProcessCF(LINKTABLE * LINK, struct DATAMESSAGE * Buffer)
{
    {
        char entry_call[20] = {0};
        ConvFromAX25((char *)LINK->LINKCALL, entry_call);
        FlexNet_Log("CF-ENTRY: from=%s LINK=%p Buffer->LENGTH=%d",
                    entry_call, (void *)LINK, Buffer->LENGTH);
    }

    unsigned char * data = &Buffer->PID;
    int len = Buffer->LENGTH - MSGHDDRLEN;

    /* Too short to be anything useful — drop. */
    if (len < 2) { ReleaseBuffer(Buffer); return 1; }

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
        /* L3RTT path consumed the frame either way. */
        ReleaseBuffer(Buffer);
        return 1;
    }

    /* Not L3RTT — most likely a NetROM L3/L4 frame (CACK, INFO with
     * banner data, IACK, DREQ, DACK) being delivered over the L2
     * session that we have flagged as a FlexNet link. Return 0 without
     * releasing the buffer; the caller (L2 dispatcher) falls through
     * to the normal CF processing path which delivers the frame to
     * the NetROM L4 layer and ultimately to the user session. */
    FlexNet_Log("CF-NOT-L3RTT: from=%s len=%d — falling through to "
                "NetROM L3/L4", nbr, len);
    return 0;
}

/* ── Timer — called periodically from LinBPQ main loop ───────────────── */

void FlexNet_Timer(void)
{
    time_t now = time(NULL);

    /* v2.x #1 — on-disk path cache: one-shot load on first tick,
       then periodic save when at least one cache row has changed
       since the last save. The load creates placeholder dest rows
       with rtt=INFINITY; the next CE-COMPACT-BATCH from a peer
       promotes them while leaving path_hops[] intact, so the user
       sees fully-cached D output immediately after a restart. */
    if (!g_path_cache_loaded)
    {
        flex_path_cache_load();
        g_path_cache_loaded   = 1;
        g_path_cache_last_save = now;
    }
    if (g_path_cache_dirty &&
        (now - g_path_cache_last_save) >= FLEXNET_PATH_CACHE_SAVE_INTERVAL)
    {
        if (flex_path_cache_save() >= 0)
        {
            g_path_cache_dirty     = 0;
            g_path_cache_last_save = now;
        }
    }

    /* Session reaper — drop FlexNetSessions slots whose underlying
       L2 LINK is gone, in CLOSED state, or has lost its LINKCALL.
       Without this, a neighbour reconnecting with a new LINK slot
       can leave an orphan FlexNetSessions entry pointing at a
       stale LINK that BPQ later recycled (visible in FL as a
       ghost row with empty callsign). */
    for (int i = 0; i < FLEXNET_MAX_SESSIONS; i++)
    {
        struct FLEXNET_SESSION * sess = &FlexNetSessions[i];
        if (!sess->active) continue;
        int reap = 0;
        if (!sess->LINK) reap = 1;
        else if (sess->LINK->L2STATE != 5) reap = 1;
        else if (sess->LINK->LINKCALL[0] == 0) reap = 1;
        if (reap)
        {
            if (FLEXNET_DEBUG)
                Consoleprintf("FlexNet: reaping ghost session slot %d "
                              "(LINK=%p)", i, (void *)sess->LINK);
            if (sess->LINK) sess->LINK->FlexNetLink = FALSE;
            /* Demote dest entries that pointed at this slot so
               cost-based attribution can re-elect a live session. */
            for (int d = 0; d < FlexNetDestCount; d++)
                if (FlexNetDests[d].via_session_idx == i)
                    FlexNetDests[d].via_session_idx = -1;
            memset(sess, 0, sizeof(*sess));
        }
    }

    /* v2.x #3 — proactive CE init. Scan connected L2 links for any
       FlexNet-mapped peer that doesn't have a FlexNet session yet
       and bootstrap one. Without this, two FlexNet peers can both
       wait for the other to send the first CE frame and never
       converge. Throttle to once per FLEXNET_PROACTIVE_INIT_INTERVAL
       to bound the scan cost. */
    if ((now - g_last_proactive_init_scan) >= FLEXNET_PROACTIVE_INIT_INTERVAL)
    {
        g_last_proactive_init_scan = now;
        for (int li = 0; li < MAXLINKS; li++)
        {
            LINKTABLE * L = &LINKS[li];
            if (L->LINKCALL[0] == 0) continue;
            if (L->L2STATE != 5) continue;
            if (L->FlexNetLink) continue;
            if (!L->LINKPORT) continue;
            if (FlexNet_IsPeerFlexNetMapped(L->LINKCALL,
                                            L->LINKPORT->PORTNUMBER))
            {
                char pcall[20] = {0};
                ConvFromAX25(L->LINKCALL, pcall);
                { int sl = (int)strlen(pcall);
                  while (sl > 0 && pcall[sl-1] == ' ') pcall[--sl] = '\0'; }
                Consoleprintf("FlexNet: proactive CE init to %s on port %d",
                              pcall, L->LINKPORT->PORTNUMBER);
                FlexNet_InitSession(L, L->LINKPORT->PORTNUMBER);
            }
        }
    }

    /* v1.9.4 — periodic transit-role D-table re-advertisement.
       Every FLEXNET_READVERTISE_INTERVAL seconds, push our learned
       routes to every active session with split-horizon (skip the
       neighbour the route was learned from) and cost adjustment
       (RTT + our_link_time). Without this linbpq is a leaf node:
       neighbours never learn about destinations reachable through us.
       Sends only on fully-converged sessions (got_peer_init &&
       sent_routes), so a freshly-bootstrapped session does its
       initial '3+/3-' handshake via flex_send_own_routes first. */
    if ((now - g_last_readvertise) >= FLEXNET_READVERTISE_INTERVAL)
    {
        g_last_readvertise = now;
        for (int i = 0; i < FLEXNET_MAX_SESSIONS; i++)
        {
            struct FLEXNET_SESSION * sess = &FlexNetSessions[i];
            if (!sess->active || !sess->LINK) continue;
            if (!sess->got_peer_init || !sess->sent_routes) continue;
            flex_send_routes_to(sess, 0);   /* record-only refresh */
        }
    }

    for (int i = 0; i < FLEXNET_MAX_SESSIONS; i++)
    {
        struct FLEXNET_SESSION * sess = &FlexNetSessions[i];
        if (!sess->active || !sess->LINK) continue;

        /* Item #4 — proactive keepalive threshold 300 s (matches
           flexnetd poll_cycle.c:501). 300 s > xnet's 189 s native KA
           cadence, so xnet's reactive KAs always reset last_keepalive
           before the 300 s threshold is reached. Result: on a healthy
           xnet link the proactive never fires; on a degraded link
           (xnet silent >= 300 s) it fires as backup. */
        if (now - sess->last_keepalive >= 300)
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

    /* Expire timed-out CE type-6 path probes (item #7+#8, v1.4.0) */
    for (int p = 0; p < FLEXNET_MAX_PATH_PROBES; p++)
    {
        if (FlexNetPathProbes[p].active &&
            (now - FlexNetPathProbes[p].sent_time) > FLEXNET_PATH_PROBE_TIMEOUT)
        {
            FlexNet_Log("PATH-TIMEOUT: qso=%d target=%s (%lds elapsed)",
                        FlexNetPathProbes[p].qso,
                        FlexNetPathProbes[p].target_call,
                        (long)(now - FlexNetPathProbes[p].sent_time));
            if (FLEXNET_DEBUG) Consoleprintf("FlexNet: PATH-REQ to %s qso=%d timed out",
                        FlexNetPathProbes[p].target_call,
                        FlexNetPathProbes[p].qso);
            FlexNetPathProbes[p].active = FALSE;
            FlexNetPathProbes[p].qso    = 0;
        }
    }

    /* Item #10 — background path probing. Round-robin through
       FlexNetDests[] one entry per FLEXNET_PATH_PROBE_INTERVAL seconds,
       sending a CE type-6 PATH_REQ. Replies (type-7 PATH_REP) populate
       FlexNetDests[].path_hops[] via flex_handle_path_rep so the BPQ
       D command renders the cached path. Mirrors flexnetd's
       poll_cycle.c:612-662 path-probe pattern. */
    if (FlexNetDestCount > 0 &&
        (now - g_last_path_probe) >= FLEXNET_PATH_PROBE_INTERVAL)
    {
        /* Decode our own callsign once for skip comparison. */
        char mycall_norm[20] = {0};
        ConvFromAX25(MYCALL, mycall_norm);
        { int sl = (int)strlen(mycall_norm);
          while (sl > 0 && mycall_norm[sl-1] == ' ') mycall_norm[--sl] = '\0'; }

        /* Round-robin selection — advance past unreachable / own-call
           entries, send one probe per interval. */
        int tries = 0;
        while (tries < FlexNetDestCount)
        {
            if (g_path_probe_idx >= FlexNetDestCount) g_path_probe_idx = 0;
            int idx = g_path_probe_idx++;
            tries++;

            struct FLEXNET_DEST_ENTRY * e = &FlexNetDests[idx];
            if (e->rtt >= FLEXNET_RTT_INFINITY) continue;
            if (strcasecmp(e->callsign, mycall_norm) == 0) continue;
            /* Also skip if we already have a fresh cached path —
               don't re-probe unnecessarily. */
            if (e->path_len > 0 &&
                (now - e->path_updated) < FLEXNET_PATH_CACHE_TTL)
                continue;

            int sent = flex_send_path_req(idx, e->callsign, e->ssid_lo);
            if (sent == 0)
            {
                FlexNet_Log("PATH-PROBE-BG: idx=%d target=%s (round-robin)",
                            idx, e->callsign);
                g_last_path_probe = now;
                break;  /* one probe per interval — don't burst */
            }
            /* sent < 0 means pending table full — try a later tick */
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

/* ── CE type-6/7 PATH_REQ / PATH_REP (item #7+#8, v1.4.0) ─────────────── */
/*
 * Wire format (matches flexnetd/ce_proto.c):
 *   byte 0: '6' (PATH_REQ) or '7' (PATH_REP)
 *   byte 1: HOP_BYTE = CE_PATH_HOP_BYTE_BASE (0x20) + hop_count
 *   bytes 2-6: QSO field — 5 ASCII chars, right-justified numeric.
 *              High bit of byte 0 = CE_PATH_TRACE_BIT (TRACE-kind flag).
 *   bytes 7+: space-separated callsigns.
 *              REQ: <origin> <target>
 *              REP: <origin> <hop1> [<hop2> ...]
 */

static int flex_parse_path_frame(const unsigned char * data, int len,
                                 int * out_is_reply,
                                 int * out_qso, int * out_trace,
                                 int * out_hop_count,
                                 char * out_origin,
                                 char  out_hops[][FLEXNET_MAX_CALLSIGN],
                                 int * out_n_hops)
{
    if (!data || len < 2 + CE_PATH_QSO_FIELD_LEN) return -1;
    if (data[0] != '6' && data[0] != '7') return -1;

    int is_reply  = (data[0] == '7') ? 1 : 0;
    int hop_count = (int)data[1] - CE_PATH_HOP_BYTE_BASE;
    if (hop_count < 0) hop_count = 0;

    /* QSO field: mask trace bit, parse ASCII number tolerating spaces */
    char qso_buf[CE_PATH_QSO_FIELD_LEN + 1];
    memcpy(qso_buf, data + 2, CE_PATH_QSO_FIELD_LEN);
    qso_buf[CE_PATH_QSO_FIELD_LEN] = '\0';
    int trace = (qso_buf[0] & CE_PATH_TRACE_BIT) ? 1 : 0;
    qso_buf[0] = (char)((unsigned char)qso_buf[0] & ~CE_PATH_TRACE_BIT);
    for (int i = 0; i < CE_PATH_QSO_FIELD_LEN; i++)
    {
        if (qso_buf[i] < '0' || qso_buf[i] > '9')
            qso_buf[i] = ' ';
    }
    int qso = atoi(qso_buf);

    if (out_is_reply)  *out_is_reply  = is_reply;
    if (out_qso)       *out_qso       = qso;
    if (out_trace)     *out_trace     = trace;
    if (out_hop_count) *out_hop_count = hop_count;

    /* Parse space-separated callsign list. First call = origin; rest = hops. */
    int pos = 2 + CE_PATH_QSO_FIELD_LEN;
    int n   = 0;
    char first[FLEXNET_MAX_CALLSIGN] = {0};
    if (out_origin) out_origin[0] = '\0';

    while (pos < len && n < FLEXNET_MAX_PATH_HOPS + 1)
    {
        /* skip spaces */
        while (pos < len && data[pos] == ' ') pos++;
        if (pos >= len) break;
        char call[FLEXNET_MAX_CALLSIGN] = {0};
        int ci = 0;
        while (pos < len && data[pos] != ' ' && data[pos] != '\r' &&
               data[pos] != '\0' && ci < (int)sizeof(call) - 1)
        {
            call[ci++] = (char)data[pos++];
        }
        if (ci == 0) break;
        if (n == 0)
        {
            strncpy(first, call, sizeof(first) - 1);
            if (out_origin)
                strncpy(out_origin, call, FLEXNET_MAX_CALLSIGN - 1);
        }
        else if (out_hops)
        {
            strncpy(out_hops[n - 1], call, FLEXNET_MAX_CALLSIGN - 1);
        }
        n++;
    }
    int n_hops = (n > 0) ? n - 1 : 0;
    if (out_n_hops) *out_n_hops = n_hops;
    return is_reply ? CE_FRAME_PATH_REP : CE_FRAME_PATH_REQ;
}

static int flex_build_path_req(unsigned char * buf, int buflen,
                               int qso, int trace,
                               const char * origin, const char * next_hop,
                               const char * target)
{
    if (!buf || !origin || !next_hop || !target) return -1;

    /* QSO field: 5 chars right-justified, set TRACE bit on byte 0 if trace */
    char qso_buf[CE_PATH_QSO_FIELD_LEN + 1];
    snprintf(qso_buf, sizeof(qso_buf), "%5u",
             (unsigned)((qso < 0 ? 0 : qso) % 100000));
    if (trace)
        qso_buf[0] = (char)((unsigned char)qso_buf[0] | CE_PATH_TRACE_BIT);

    int ol = (int)strlen(origin);
    int nl = (int)strlen(next_hop);
    int tl = (int)strlen(target);
    int needed = 1 + 1 + CE_PATH_QSO_FIELD_LEN + ol + 1 + nl + 1 + tl;
    if (needed > buflen) return -1;

    /* Wire format (observed on real network, dual-port capture 2026-05-12):
       byte 0    : '6'
       byte 1    : 0x20 + hop_count (1 = "one intermediate named in body")
       bytes 2-6 : QSO field (5-char right-justified)
       bytes 7+  : <origin> ' ' <next_hop> ' ' <target>
       Each forwarder appends its own next-hop selection and bumps the
       hop-count byte. */
    int pos = 0;
    buf[pos++] = '6';
    buf[pos++] = (unsigned char)(CE_PATH_HOP_BYTE_BASE + 1);  /* hop=1 */
    memcpy(buf + pos, qso_buf, CE_PATH_QSO_FIELD_LEN);
    pos += CE_PATH_QSO_FIELD_LEN;
    memcpy(buf + pos, origin, ol);
    pos += ol;
    buf[pos++] = ' ';
    memcpy(buf + pos, next_hop, nl);
    pos += nl;
    buf[pos++] = ' ';
    memcpy(buf + pos, target, tl);
    pos += tl;
    return pos;
}

static int flex_build_path_rep(unsigned char * buf, int buflen,
                               int qso, int trace,
                               const char * const * hops, int n_hops)
{
    if (!buf) return -1;
    if (n_hops < 1 || n_hops > FLEXNET_MAX_PATH_HOPS) return -1;

    char qso_buf[CE_PATH_QSO_FIELD_LEN + 1];
    snprintf(qso_buf, sizeof(qso_buf), "%5u",
             (unsigned)((qso < 0 ? 0 : qso) % 100000));
    if (trace)
        qso_buf[0] = (char)((unsigned char)qso_buf[0] | CE_PATH_TRACE_BIT);

    if (buflen < 2 + CE_PATH_QSO_FIELD_LEN) return -1;
    int pos = 0;
    buf[pos++] = '7';
    buf[pos++] = (unsigned char)(CE_PATH_HOP_BYTE_BASE + n_hops);
    memcpy(buf + pos, qso_buf, CE_PATH_QSO_FIELD_LEN);
    pos += CE_PATH_QSO_FIELD_LEN;

    /* Hops space-separated; first hop has NO leading space (per
       flexnetd's note). */
    for (int i = 0; i < n_hops; i++)
    {
        const char * h = hops ? hops[i] : NULL;
        if (!h || !*h) continue;
        int hl = (int)strlen(h);
        int need = (i == 0 ? 0 : 1) + hl;
        if (pos + need >= buflen) return -1;
        if (i > 0) buf[pos++] = ' ';
        memcpy(buf + pos, h, hl);
        pos += hl;
    }
    return pos;
}

/*
 * Compare incoming target callsign to MYCALL (case-insensitive, ignoring
 * SSID suffix). Returns 1 if match.
 */
static int flex_target_is_us(const char * target)
{
    char mycall_norm[20] = {0};
    ConvFromAX25(MYCALL, mycall_norm);
    /* trim trailing spaces */
    { int sl = (int)strlen(mycall_norm);
      while (sl > 0 && mycall_norm[sl-1] == ' ') mycall_norm[--sl] = '\0'; }

    /* Case-insensitive equal */
    if (strcasecmp(mycall_norm, target) == 0) return 1;

    /* Compare base part (strip SSID suffix after '-') */
    char a[FLEXNET_MAX_CALLSIGN] = {0};
    char b[FLEXNET_MAX_CALLSIGN] = {0};
    strncpy(a, mycall_norm, sizeof(a) - 1);
    strncpy(b, target,      sizeof(b) - 1);
    char * d;
    if ((d = strchr(a, '-'))) *d = '\0';
    if ((d = strchr(b, '-'))) *d = '\0';
    return (strcasecmp(a, b) == 0) ? 1 : 0;
}

/* Incoming PATH_REQ: if target is us, reply with type-7; else drop. */
static void flex_handle_path_req(LINKTABLE * LINK,
                                 struct FLEXNET_SESSION * sess,
                                 unsigned char * data, int len)
{
    int is_reply, qso, trace, hop_count, n_hops;
    char origin[FLEXNET_MAX_CALLSIGN] = {0};
    char hops[FLEXNET_MAX_PATH_HOPS][FLEXNET_MAX_CALLSIGN] = {{0}};

    int rc = flex_parse_path_frame(data, len,
                                   &is_reply, &qso, &trace, &hop_count,
                                   origin, hops, &n_hops);
    if (rc < 0 || is_reply)
    {
        FlexNet_Log("PATH-REQ-DROP: parse failed (len=%d rc=%d)", len, rc);
        return;
    }

    /* REQ payload layout: hops[0..n_hops-1] are space-separated callsigns
       AFTER the origin. For a fresh REQ, n_hops == 1 (the target). For an
       intermediate-relayed REQ the array contains the accumulated path,
       last element being the target. */
    if (n_hops <= 0)
    {
        FlexNet_Log("PATH-REQ-DROP: no target field");
        return;
    }
    const char * target = hops[n_hops - 1];

    if (FLEXNET_DEBUG) Consoleprintf("FlexNet: PATH-REQ qso=%d origin=%s target=%s "
                "hop_count=%d (us=%d)",
                qso, origin, target, hop_count, flex_target_is_us(target));

    if (!flex_target_is_us(target))
    {
        FlexNet_Log("PATH-REQ-DROP: target=%s not us (M6 forwarding "
                    "not implemented)", target);
        return;
    }

    /* Build single-hop reply: our own normalised callsign. */
    char mycall_norm[20] = {0};
    ConvFromAX25(MYCALL, mycall_norm);
    { int sl = (int)strlen(mycall_norm);
      while (sl > 0 && mycall_norm[sl-1] == ' ') mycall_norm[--sl] = '\0'; }

    const char * reply_hops[1] = { mycall_norm };
    unsigned char reply[64];
    int rlen = flex_build_path_rep(reply, sizeof(reply), qso, trace,
                                   reply_hops, 1);
    if (rlen <= 0)
    {
        FlexNet_Log("PATH-REQ-DROP: build_rep failed");
        return;
    }
    flex_send_frame(LINK, FLEXNET_PID_CE, reply, rlen);
    FlexNet_Log("PATH-REP-TX: -> origin=%s qso=%d trace=%d hops=%s (%d bytes)",
                origin, qso, trace, mycall_norm, rlen);
}

/* Incoming PATH_REP: match QSO to pending probe, populate path cache. */
static void flex_handle_path_rep(LINKTABLE * LINK,
                                 struct FLEXNET_SESSION * sess,
                                 unsigned char * data, int len)
{
    (void)LINK; (void)sess;
    int is_reply, qso, trace, hop_count, n_hops;
    char origin[FLEXNET_MAX_CALLSIGN] = {0};
    char hops[FLEXNET_MAX_PATH_HOPS][FLEXNET_MAX_CALLSIGN] = {{0}};

    int rc = flex_parse_path_frame(data, len,
                                   &is_reply, &qso, &trace, &hop_count,
                                   origin, hops, &n_hops);
    if (rc < 0 || !is_reply)
    {
        FlexNet_Log("PATH-REP-DROP: parse failed (len=%d rc=%d)", len, rc);
        return;
    }

    /* Find matching pending probe */
    int slot = -1;
    for (int i = 0; i < FLEXNET_MAX_PATH_PROBES; i++)
    {
        if (FlexNetPathProbes[i].active &&
            FlexNetPathProbes[i].qso == qso)
        {
            slot = i;
            break;
        }
    }
    if (slot < 0)
    {
        FlexNet_Log("PATH-REP-DROP: unsolicited qso=%d origin=%s hops=%d",
                    qso, origin, n_hops);
        return;
    }

    struct FLEXNET_PATH_PROBE * probe = &FlexNetPathProbes[slot];
    probe->got_reply       = TRUE;
    probe->reply_hop_count = n_hops;
    for (int h = 0; h < n_hops && h < FLEXNET_MAX_PATH_HOPS; h++)
        strncpy(probe->reply_hops[h], hops[h], FLEXNET_MAX_CALLSIGN - 1);

    /* Populate destination's path cache. */
    if (probe->dest_index >= 0 && probe->dest_index < FlexNetDestCount)
    {
        struct FLEXNET_DEST_ENTRY * dest = &FlexNetDests[probe->dest_index];
        dest->path_len = n_hops;
        for (int h = 0; h < n_hops && h < FLEXNET_MAX_PATH_HOPS; h++)
            strncpy(dest->path_hops[h], hops[h],
                    FLEXNET_MAX_CALLSIGN - 1);
        dest->path_updated = time(NULL);
        g_path_cache_dirty = 1;   /* trigger persist on next save tick */
    }

    FlexNet_Log("PATH-REP-RX: qso=%d origin=%s hops=%d target=%s "
                "(matched slot=%d, %lds elapsed)",
                qso, origin, n_hops, probe->target_call, slot,
                (long)(time(NULL) - probe->sent_time));

    /* Clear pending */
    probe->active = FALSE;
    probe->qso    = 0;
}

/* Allocate a fresh QSO, send PATH_REQ via the first active FlexNet
   session. Returns 0 on success, -1 on error. */
static int flex_send_path_req(int dest_idx,
                              const char * target_call, int target_ssid)
{
    /* Find a free probe slot */
    struct FLEXNET_PATH_PROBE * probe = NULL;
    int slot = -1;
    for (int i = 0; i < FLEXNET_MAX_PATH_PROBES; i++)
    {
        if (!FlexNetPathProbes[i].active)
        {
            probe = &FlexNetPathProbes[i];
            slot = i;
            break;
        }
    }
    if (!probe) return -1;

    /* Cost-based session selection: route the probe through the
       neighbour the D-table picked for this destination. Falls back
       to the first active session if the dest has no recorded
       via_session_idx (e.g. just loaded from disk and not yet
       refreshed by a CE-COMPACT-BATCH). */
    struct FLEXNET_SESSION * sess = NULL;
    if (dest_idx >= 0 && dest_idx < FlexNetDestCount)
    {
        int vidx = FlexNetDests[dest_idx].via_session_idx;
        if (vidx >= 0 && vidx < FLEXNET_MAX_SESSIONS &&
            FlexNetSessions[vidx].active &&
            FlexNetSessions[vidx].LINK)
            sess = &FlexNetSessions[vidx];
    }
    if (!sess)
    {
        for (int i = 0; i < FLEXNET_MAX_SESSIONS; i++)
        {
            if (FlexNetSessions[i].active && FlexNetSessions[i].LINK)
            {
                sess = &FlexNetSessions[i];
                break;
            }
        }
    }
    if (!sess || !sess->LINK) return -1;

    /* Allocate fresh non-zero QSO not currently in use. */
    int qso = 0;
    for (int tries = 0; tries < 65536; tries++)
    {
        g_path_qso_counter++;
        if (g_path_qso_counter == 0) g_path_qso_counter = 1;
        int candidate = (int)g_path_qso_counter;
        int collision = 0;
        for (int j = 0; j < FLEXNET_MAX_PATH_PROBES; j++)
        {
            if (FlexNetPathProbes[j].active &&
                FlexNetPathProbes[j].qso == candidate)
            { collision = 1; break; }
        }
        if (!collision) { qso = candidate; break; }
    }
    if (qso == 0) return -1;

    /* Build target string with SSID suffix. */
    char target_full[FLEXNET_MAX_CALLSIGN + 4];
    if (target_ssid >= 0)
        snprintf(target_full, sizeof(target_full), "%s-%d",
                 target_call, target_ssid);
    else
        snprintf(target_full, sizeof(target_full), "%s", target_call);

    /* Our origin: normalised MYCALL. */
    char mycall_norm[20] = {0};
    ConvFromAX25(MYCALL, mycall_norm);
    { int sl = (int)strlen(mycall_norm);
      while (sl > 0 && mycall_norm[sl-1] == ' ') mycall_norm[--sl] = '\0'; }

    /* Next-hop callsign: the FlexNet neighbour we're sending the REQ to.
       Required for xnet-compatible wire format (it expects each forwarder
       to name its next-hop in the body so the path accumulates as the
       frame propagates). */
    char next_hop[20] = {0};
    ConvFromAX25(sess->LINK->LINKCALL, next_hop);
    { int sl = (int)strlen(next_hop);
      while (sl > 0 && next_hop[sl-1] == ' ') next_hop[--sl] = '\0'; }

    unsigned char frame[128];
    int flen = flex_build_path_req(frame, sizeof(frame), qso, 0,
                                   mycall_norm, next_hop, target_full);
    if (flen <= 0) return -1;
    flex_send_frame(sess->LINK, FLEXNET_PID_CE, frame, flen);

    /* Record pending probe. */
    memset(probe, 0, sizeof(*probe));
    probe->active        = TRUE;
    probe->qso           = qso;
    probe->trace         = 0;
    strncpy(probe->target_call, target_call, FLEXNET_MAX_CALLSIGN - 1);
    probe->target_ssid   = target_ssid;
    probe->dest_index    = dest_idx;
    probe->sent_time     = time(NULL);
    probe->got_reply     = FALSE;

    FlexNet_Log("PATH-REQ-TX: -> origin=%s next=%s target=%s qso=%d slot=%d "
                "(%d bytes)",
                mycall_norm, next_hop, target_full, qso, slot, flen);
    if (FLEXNET_DEBUG) Consoleprintf("FlexNet: PATH-REQ-TX next=%s target=%s qso=%d",
                next_hop, target_full, qso);
    return 0;
}

/* ── On-disk path cache (v2.x item #1) ────────────────────────────────── */
/*
 * File format (text, line-oriented, one destination per line):
 *
 *   # linbpq-flexnet path cache v1
 *   <call> <ssid_lo> <ssid_hi> <path_updated> <path_len> <hop1> [<hop2> ...]
 *
 * Lines starting with '#' are comments. Entries with path_len < 1 are
 * skipped. Loader merges into an existing FlexNetDests[] slot when the
 * (call, ssid_lo, ssid_hi) tuple already exists, otherwise creates a
 * placeholder slot with rtt=FLEXNET_RTT_INFINITY — the next
 * CE-COMPACT-BATCH from a peer promotes that slot to reachable while
 * leaving the cached path intact.
 *
 * Save is gated on a dirty flag set whenever flex_handle_path_rep
 * updates a cache row; that bounds the disk-write rate to one write
 * per FLEXNET_PATH_CACHE_SAVE_INTERVAL even if many probes land.
 */

static int flex_path_cache_save(void)
{
    FILE * fp = fopen(FLEXNET_PATH_CACHE_FILE ".tmp", "w");
    if (!fp)
    {
        FlexNet_Log("PATH-CACHE-SAVE: fopen failed errno=%d (%s)",
                    errno, strerror(errno));
        return -1;
    }
    fprintf(fp, "# linbpq-flexnet path cache v1\n");
    fprintf(fp, "# format: call ssid_lo ssid_hi path_updated path_len hop1 hop2 ...\n");

    time_t now = time(NULL);
    int written = 0;
    for (int i = 0; i < FlexNetDestCount; i++)
    {
        struct FLEXNET_DEST_ENTRY * e = &FlexNetDests[i];
        if (e->path_len <= 0) continue;
        if (e->callsign[0] == '\0') continue;
        if ((now - e->path_updated) >= FLEXNET_PATH_CACHE_PERSIST_TTL) continue;

        fprintf(fp, "%s %d %d %ld %d",
                e->callsign, e->ssid_lo, e->ssid_hi,
                (long)e->path_updated, e->path_len);
        for (int h = 0; h < e->path_len && h < FLEXNET_MAX_PATH_HOPS; h++)
        {
            if (e->path_hops[h][0])
                fprintf(fp, " %s", e->path_hops[h]);
        }
        fputc('\n', fp);
        written++;
    }

    if (fflush(fp) != 0 || fclose(fp) != 0)
    {
        FlexNet_Log("PATH-CACHE-SAVE: flush/close failed errno=%d", errno);
        unlink(FLEXNET_PATH_CACHE_FILE ".tmp");
        return -1;
    }
    if (rename(FLEXNET_PATH_CACHE_FILE ".tmp",
               FLEXNET_PATH_CACHE_FILE) != 0)
    {
        FlexNet_Log("PATH-CACHE-SAVE: rename failed errno=%d", errno);
        return -1;
    }

    FlexNet_Log("PATH-CACHE-SAVE: wrote %d entries to %s",
                written, FLEXNET_PATH_CACHE_FILE);
    if (FLEXNET_DEBUG)
        Consoleprintf("FlexNet: path cache saved (%d entries)", written);
    return written;
}

static int flex_path_cache_load(void)
{
    FILE * fp = fopen(FLEXNET_PATH_CACHE_FILE, "r");
    if (!fp)
    {
        if (errno != ENOENT)
            FlexNet_Log("PATH-CACHE-LOAD: fopen failed errno=%d (%s)",
                        errno, strerror(errno));
        return 0;
    }

    char line[1024];
    int loaded = 0, skipped_stale = 0, skipped_bad = 0;
    time_t now = time(NULL);

    while (fgets(line, sizeof(line), fp))
    {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') continue;

        char call[FLEXNET_MAX_CALLSIGN] = {0};
        int  ssid_lo = 0, ssid_hi = 0, path_len = 0;
        long path_updated = 0;
        int  consumed = 0;
        if (sscanf(line, "%15s %d %d %ld %d%n",
                   call, &ssid_lo, &ssid_hi,
                   &path_updated, &path_len, &consumed) < 5)
        {
            skipped_bad++;
            continue;
        }
        if (path_len < 1 || path_len > FLEXNET_MAX_PATH_HOPS)
        {
            skipped_bad++;
            continue;
        }
        if ((now - (time_t)path_updated) >= FLEXNET_PATH_CACHE_PERSIST_TTL)
        {
            skipped_stale++;
            continue;
        }

        /* Parse path_len hop callsigns after the consumed prefix */
        char hops[FLEXNET_MAX_PATH_HOPS][FLEXNET_MAX_CALLSIGN] = {{0}};
        int  n_parsed = 0;
        const char * p = line + consumed;
        while (n_parsed < path_len)
        {
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\n' || *p == '\0') break;
            int hi = 0;
            while (*p && *p != ' ' && *p != '\t' &&
                   *p != '\n' && hi < FLEXNET_MAX_CALLSIGN - 1)
            {
                hops[n_parsed][hi++] = *p++;
            }
            hops[n_parsed][hi] = '\0';
            n_parsed++;
        }
        if (n_parsed != path_len)
        {
            skipped_bad++;
            continue;
        }

        /* Locate existing slot or allocate placeholder */
        int idx = flex_find_dest(call, ssid_lo, ssid_hi);
        if (idx < 0)
        {
            if (FlexNetDestCount >= FLEXNET_MAX_DESTS)
            {
                skipped_bad++;
                continue;
            }
            idx = FlexNetDestCount++;
            struct FLEXNET_DEST_ENTRY * ne = &FlexNetDests[idx];
            memset(ne, 0, sizeof(*ne));
            strncpy(ne->callsign, call, FLEXNET_MAX_CALLSIGN - 1);
            ne->ssid_lo = ssid_lo;
            ne->ssid_hi = ssid_hi;
            ne->rtt     = FLEXNET_RTT_INFINITY;
            ne->via_session_idx = -1;   /* will be set by next CE-COMPACT-BATCH */
        }
        struct FLEXNET_DEST_ENTRY * e = &FlexNetDests[idx];
        e->path_len     = path_len;
        e->path_updated = (time_t)path_updated;
        for (int h = 0; h < path_len; h++)
            strncpy(e->path_hops[h], hops[h], FLEXNET_MAX_CALLSIGN - 1);
        loaded++;
    }
    fclose(fp);

    FlexNet_Log("PATH-CACHE-LOAD: loaded=%d stale=%d bad=%d from %s",
                loaded, skipped_stale, skipped_bad,
                FLEXNET_PATH_CACHE_FILE);
    if (FLEXNET_DEBUG)
        Consoleprintf("FlexNet: path cache loaded (%d entries, %d stale, %d bad)",
                      loaded, skipped_stale, skipped_bad);
    return loaded;
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
        /* Show cached path. PATH_REP from xnet does not include the
           originator in the hop list (we were the originator), so prepend
           MYCALL to match xnet's own D-command display convention. */
        char mycall_norm[20] = {0};
        ConvFromAX25(MYCALL, mycall_norm);
        { int sl = (int)strlen(mycall_norm);
          while (sl > 0 && mycall_norm[sl-1] == ' ') mycall_norm[--sl] = '\0'; }

        *Bufferptr_p = Cmdprintf(Session, *Bufferptr_p,
            "*** route: %s", mycall_norm);
        for (int h = 0; h < e->path_len; h++)
            *Bufferptr_p = Cmdprintf(Session, *Bufferptr_p,
                " %s", e->path_hops[h]);
        *Bufferptr_p = Cmdprintf(Session, *Bufferptr_p, "\r");
    }
    else
    {
        /* No cached PATH_REP data (path_hops[] empty or stale).
           Synthesize a path from our local D-table.

           Algorithm: render `<MYCALL> <via_callsign> <target>`.
           That's 3 callsigns when via_callsign != target (relayed),
           or 2 when via_callsign == target (direct neighbor).

           This is a PARTIAL path — full chains require type-7
           PATH_REP frames from a peer that implements the type-6
           responder side. Observed behaviour on the current live
           network is that our background PATH_REQ probes do not
           receive replies, so the cached-path branch above stays
           empty in practice and this fallback is what renders the
           visible route. When a responding peer becomes reachable,
           the cached-path branch will kick in automatically. */
        char mycall_norm[20] = {0};
        ConvFromAX25(MYCALL, mycall_norm);
        { int sl = (int)strlen(mycall_norm);
          while (sl > 0 && mycall_norm[sl-1] == ' ') mycall_norm[--sl] = '\0'; }

        *Bufferptr_p = Cmdprintf(Session, *Bufferptr_p,
            "*** route: %s", mycall_norm);
        if (e->via_callsign[0] &&
            strcasecmp(e->via_callsign, e->callsign) != 0)
        {
            *Bufferptr_p = Cmdprintf(Session, *Bufferptr_p,
                " %s", e->via_callsign);
        }
        if (query_ssid >= 0)
            *Bufferptr_p = Cmdprintf(Session, *Bufferptr_p,
                " %s-%d\r", e->callsign, query_ssid);
        else
            *Bufferptr_p = Cmdprintf(Session, *Bufferptr_p,
                " %s\r", e->callsign);
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
        "Dest     SSID    RTT Path\r");
    Bufferptr = Cmdprintf(Session, Bufferptr,
        "-------- ----- ----- ----\r");

    time_t now_list = time(NULL);
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

        /* Path marker: '!' if PATH_REP cache is populated and fresh.
           Empty cell otherwise — the destination is still reachable
           (RTT column carries the cost) but no full hop chain is on
           file yet. The D-detail view falls back to local-walk in
           that case. */
        const char * path_mark =
            (e->path_len > 0 &&
             (now_list - e->path_updated) < FLEXNET_PATH_CACHE_TTL)
            ? "!" : "";

        Bufferptr = Cmdprintf(Session, Bufferptr,
            "%-8s %5s %5d %s\r",
            e->callsign, ssid_range, e->rtt, path_mark);
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
        if (sess->LINK->LINKCALL[0] == 0) continue;  /* ghost */
        if (sess->LINK->L2STATE != 5) continue;      /* dead */

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

        /* Count routes attributed to THIS neighbour (= the
           destination's chosen via_session_idx). With multiple
           FlexNet neighbours on the same BPQ port, port-based
           counting would double-count every destination; the
           cost-based attribution puts each destination on exactly
           one session. */
        int sess_idx = (int)(sess - FlexNetSessions);
        int route_count = 0;
        for (int j = 0; j < FlexNetDestCount; j++)
        {
            if (FlexNetDests[j].via_session_idx == sess_idx &&
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

    /* CE type-6 PATH_REQUEST: '6' prefix */
    if (data[0] == '6')
        return CE_FRAME_PATH_REQ;

    /* CE type-7 PATH_REPLY: '7' prefix */
    if (data[0] == '7')
        return CE_FRAME_PATH_REP;

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

static int flex_dtable_merge(struct FLEXNET_DEST_ENTRY * incoming,
                              struct FLEXNET_SESSION * sess)
{
    int  port = sess ? sess->port : 0;
    int  sess_idx = sess ? (int)(sess - FlexNetSessions) : -1;

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

    /* Resolve neighbour callsign from this session's LINK (informational) */
    char via[FLEXNET_MAX_CALLSIGN] = {0};
    if (sess && sess->LINK)
    {
        ConvFromAX25(sess->LINK->LINKCALL, via);
        int sl = (int)strlen(via);
        while (sl > 0 && via[sl-1] == ' ') via[--sl] = '\0';
    }

    int idx = flex_find_dest(incoming->callsign,
                             incoming->ssid_lo, incoming->ssid_hi);
    if (idx >= 0)
    {
        /* Multi-neighbour cost-based selection: prefer the lowest-RTT
           announcement across all neighbours. A neighbour can also
           refresh its own entry (same session_idx) and a withdrawn
           route from the current chosen neighbour always wins so the
           entry can fail over. */
        int existing_is_current = (FlexNetDests[idx].via_session_idx == sess_idx);
        int new_is_better = (incoming->rtt < FlexNetDests[idx].rtt);
        int new_is_withdraw_from_current =
            existing_is_current && incoming->is_infinity;

        if (existing_is_current || new_is_better || new_is_withdraw_from_current)
        {
            FlexNetDests[idx].rtt              = incoming->rtt;
            FlexNetDests[idx].is_infinity      = incoming->is_infinity;
            FlexNetDests[idx].port             = port;
            FlexNetDests[idx].via_session_idx  = sess_idx;
            FlexNetDests[idx].last_updated     = time(NULL);
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
        FlexNetDests[FlexNetDestCount].port             = port;
        FlexNetDests[FlexNetDestCount].via_session_idx  = sess_idx;
        FlexNetDests[FlexNetDestCount].last_updated     = time(NULL);
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

/* Item #3 (v1.3.6) — wire LT value sent on every CE LT frame.
   Matches flexnetd's poll_cycle.c:531 hardcoded 2 and pre-v1.3.4
   linbpq-flexnet behavior. We keep the IIR-smoothed value in
   sess->our_link_time up-to-date for internal logging and future
   use (see flex_link_time_sample), but the CE LT round-trip we
   sample is dominated by the peer's reply scheduling, not network
   latency, so advertising the IIR output on the wire inflates
   xnet's T column display (peers see T=54-65 instead of the
   healthy T=1-3 range) and skews link-quality routing. */
#define FLEXNET_WIRE_LT 2

static int flex_send_link_time(LINKTABLE * LINK,
                               struct FLEXNET_SESSION * sess)
{
    unsigned char lt[16];
    int ltlen = flex_build_link_time(lt, sizeof(lt), FLEXNET_WIRE_LT);
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

/* v1.9.4 — build a single CE compact-batch frame: '3' + N records + '\r'.
 *
 *   buf, buflen        : destination frame buffer (max FLEXNET_BATCH_MAX_BYTES)
 *   target_sess        : the FlexNet session this frame is being built for
 *                        (its index in FlexNetSessions[] is the split-horizon key)
 *   dest_cursor_io     : in/out — index into FlexNetDests[]; advance over each
 *                        destination consumed. Caller loops calls until cursor
 *                        reaches FlexNetDestCount.
 *   include_mycall     : when TRUE, prepend MYCALL as the first record.
 *
 * Returns: frame length in bytes, or -1 on error / empty batch.
 */
static int flex_build_compact_batch(unsigned char * buf, int buflen,
                                    struct FLEXNET_SESSION * target_sess,
                                    int * dest_cursor_io,
                                    int include_mycall,
                                    int * out_records)
{
    if (!buf || !target_sess || !dest_cursor_io) return -1;
    if (buflen < 16) return -1;

    int pos = 0;
    buf[pos++] = '3';
    int records = 0;
    int target_idx = (int)(target_sess - FlexNetSessions);
    int cost_bias  = target_sess->our_link_time > 0 ? target_sess->our_link_time : 1;

    if (include_mycall)
    {
        char mycall[20] = {0};
        ConvFromAX25(MYCALL, mycall);
        { int sl = (int)strlen(mycall);
          while (sl > 0 && mycall[sl-1] == ' ') mycall[--sl] = '\0'; }
        int my_ssid = (MYCALL[6] >> 1) & 0x0F;
        char * dash = strchr(mycall, '-');
        if (dash) *dash = '\0';
        if (mycall[0])
        {
            char rec[16];
            int rlen = snprintf(rec, sizeof(rec), "%-6.6s%c%c1 ",
                                mycall,
                                (char)(FLEXNET_SSID_BASE + my_ssid),
                                (char)(FLEXNET_SSID_BASE + my_ssid));
            if (rlen > 0 && pos + rlen + 1 < buflen)
            {
                memcpy(buf + pos, rec, rlen);
                pos += rlen; records++;
            }
        }
    }

    int cursor = *dest_cursor_io;
    while (cursor < FlexNetDestCount && pos + 14 < buflen)
    {
        struct FLEXNET_DEST_ENTRY * d = &FlexNetDests[cursor++];
        if (d->callsign[0] == '\0')                continue;
        if (d->rtt >= FLEXNET_RTT_INFINITY)        continue;
        if (d->via_session_idx < 0)                continue;  /* unknown via */
        if (d->via_session_idx == target_idx)      continue;  /* split-horizon */

        int adj_rtt = d->rtt + cost_bias;
        if (adj_rtt < 1) adj_rtt = 1;
        if (adj_rtt >= FLEXNET_RTT_INFINITY) adj_rtt = FLEXNET_RTT_INFINITY - 1;

        char rec[16];
        int rlen = snprintf(rec, sizeof(rec), "%-6.6s%c%c%d ",
                            d->callsign,
                            (char)(FLEXNET_SSID_BASE + d->ssid_lo),
                            (char)(FLEXNET_SSID_BASE + d->ssid_hi),
                            adj_rtt);
        if (rlen <= 0)             break;
        if (pos + rlen + 1 > buflen) { cursor--; break; }  /* rewind */
        memcpy(buf + pos, rec, rlen);
        pos += rlen; records++;
    }
    *dest_cursor_io = cursor;

    if (records == 0) return -1;
    if (pos < buflen) buf[pos++] = '\r';
    if (out_records) *out_records = records;
    return pos;
}

/* v1.9.4 — advertise to one neighbour with split-horizon + cost adjustment.
 *
 *   mode 0 = record-only (periodic refresh; no token request/release)
 *   mode 1 = '3+' + records + '3-' (initial advertisement on session up)
 *
 * Returns total records sent across all batch frames. */
static int flex_send_routes_to(struct FLEXNET_SESSION * sess, int mode)
{
    if (!sess || !sess->LINK) return 0;
    char nbr[20] = {0};
    ConvFromAX25(sess->LINK->LINKCALL, nbr);
    { int sl = (int)strlen(nbr);
      while (sl > 0 && nbr[sl-1] == ' ') nbr[--sl] = '\0'; }

    if (mode == 1)
    {
        unsigned char req[] = { '3', '+', '\r' };
        flex_send_frame(sess->LINK, FLEXNET_PID_CE, req, 3);
    }

    int total_records = 0;
    int total_frames  = 0;
    int dest_cursor   = 0;
    int include_mycall = 1;  /* only in first batch frame */
    for (;;)
    {
        unsigned char frame[FLEXNET_BATCH_MAX_BYTES];
        int recs_this = 0;
        int flen = flex_build_compact_batch(frame, sizeof(frame),
                                            sess, &dest_cursor,
                                            include_mycall, &recs_this);
        include_mycall = 0;   /* mycall only in first frame */
        if (flen <= 0) break;
        flex_send_frame(sess->LINK, FLEXNET_PID_CE, frame, flen);
        total_records += recs_this;
        total_frames++;
        if (dest_cursor >= FlexNetDestCount) break;
        if (total_frames >= 20) break;  /* safety cap */
    }

    if (mode == 1)
    {
        unsigned char rel[] = { '3', '-', '\r' };
        flex_send_frame(sess->LINK, FLEXNET_PID_CE, rel, 3);
    }

    FlexNet_Log("ROUTES-ADVERTISE: -> %s records=%d frames=%d mode=%d "
                "link_cost=%d (mycall + split-horizon transit)",
                nbr, total_records, total_frames, mode, sess->our_link_time);
    if (FLEXNET_DEBUG)
        Consoleprintf("FlexNet: advertised %d routes in %d frame(s) to %s "
                      "(mode=%d, +link_cost=%d)",
                      total_records, total_frames, nbr,
                      mode, sess->our_link_time);
    return total_records;
}

/* Legacy wrapper — kept so the existing call sites compile. The
   token-passing variant is `mode=1` on the new helper. */
static void flex_send_own_routes(LINKTABLE * LINK, int port)
{
    (void)port;
    struct FLEXNET_SESSION * sess = flex_find_session(LINK);
    if (!sess) return;
    flex_send_routes_to(sess, 1);
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

        /* Found — cache the chosen dest so FlexNet_GetNeighborCall
           can pick the right session even when multiple FlexNet
           neighbours share the same BPQ port. The caller (Cmd.c)
           invokes FindRoute then GetNeighborCall back-to-back, so
           the cache is single-shot. */
        g_findroute_last_dest = i;
        Consoleprintf("FlexNet: routing C %s via FlexNet port %d "
                      "(RTT=%d, via session %d)",
                      target, e->port, e->rtt, e->via_session_idx);
        return e->port;
    }

    if (FLEXNET_DEBUG) Consoleprintf("FlexNet: FindRoute — '%s' not found in dest table",
                  target_base);
    g_findroute_last_dest = -1;
    return 0;  /* not a FlexNet destination */
}

/* ── Get FlexNet Neighbor AX.25 Callsign ────────────────────────────── */
/*
 * Returns the AX.25-encoded callsign (7 bytes) of the FlexNet neighbour
 * that owns the most-recent successful FlexNet_FindRoute. Falls back to
 * the first active session on `port` if no recent FindRoute is on
 * record (e.g. caller used the routing table directly).
 */

BOOL FlexNet_GetNeighborCall(int port, unsigned char * axcall_out)
{
    /* Preferred path: use the session chosen by the last FindRoute */
    if (g_findroute_last_dest >= 0 &&
        g_findroute_last_dest < FlexNetDestCount)
    {
        struct FLEXNET_DEST_ENTRY * e =
            &FlexNetDests[g_findroute_last_dest];
        int vidx = e->via_session_idx;
        if (vidx >= 0 && vidx < FLEXNET_MAX_SESSIONS &&
            FlexNetSessions[vidx].active &&
            FlexNetSessions[vidx].LINK &&
            FlexNetSessions[vidx].port == port)
        {
            memcpy(axcall_out,
                   FlexNetSessions[vidx].LINK->LINKCALL, 7);
            g_findroute_last_dest = -1;  /* consume */
            return TRUE;
        }
    }
    /* Fallback: first active session on this port */
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
