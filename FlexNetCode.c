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
#define FLEXNET_VERSION_STR   "v2.2.0-rc6"
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

/* v2.2 transit-role constants (RFC_TRANSIT_ROLE_V2.md). */
#define FLEXNET_MAX_LEARNED_PER_NEIGHBOUR  256
#define FLEXNET_MAX_TRANSIT_SESSIONS       32
#define FLEXNET_ADVERT_INTERVAL            120  /* seconds, per Phase 1 */
#define FLEXNET_TRANSIT_SESSION_TIMEOUT    600  /* seconds, idle reap */
#define FLEXNET_MAX_RECORDS_PER_BATCH      5

/* rc4 event-driven re-advertisement (RFC §5.9). Emission is driven by
   table mutations, gated by a per-peer token bucket — the cap+rotating-
   cursor model these replace is preserved in RFC §16/§17, along with the
   three soaks it lost. The refill rates are xnet's own observed cadence
   to each peer family (Phase 2: ~1 record / 50 s to PC/Flexnet) with a
   safety margin, because exceeding a PCF peer's ingestion rate saturates
   its RTT to 4095 and puts it in a state that needs a manual reset. */
#define FLEXNET_MAX_ADVERTISED_PER_PEER    256
#define FLEXNET_REFRESH_THRESHOLD_PCT      10   /* relative jitter floor */
#define FLEXNET_REFRESH_THRESHOLD_ABS      1    /* absolute floor, 100ms ticks */
#define FLEXNET_BUCKET_REFILL_PCF_S        5    /* 1 record / 5 s to PC/Flexnet */
#define FLEXNET_BUCKET_REFILL_XNET_S       2    /* 1 record / 2 s to (X)Net-like */
#define FLEXNET_BUCKET_SIZE_PCF            2
#define FLEXNET_BUCKET_SIZE_XNET           4

/* Records packed into ONE compact CE frame. The wire format is one '3'
   per FRAME, then N fixed-shape records, then '\r' — which is what
   every peer in the mesh already sends us (measured 2026-09-19 over
   17 h: (X)Net fills to 248 info bytes, PC/Flexnet to 205). We were
   emitting one record per I-frame, 15 bytes of a 236-byte PACLEN, so a
   ~210-destination re-dump took 210 frames — 17.6 minutes at the PCF
   bucket rate — and the queue to -12 was non-empty 80 % of the time
   because it was fed at ~27 records/min and drained at 12.

   A token now buys a FRAME, not a record, so the I-frame rate PC/Flexnet
   sees is UNCHANGED. That distinction is the whole safety argument: the
   rc1 flood (§16.2) saturated PCF with ~50 I-frames in under 2 s, and
   nothing here raises frames per second. Bytes per frame is the only
   thing that grows, and it grows to what PCF itself transmits.

   The byte budget stays under the 236-byte PACLEN with headroom for the
   '3' prefix and the '\r' terminator. */
#define FLEXNET_ADVERT_FRAME_BYTES         200
#define FLEXNET_ADVERT_RECS_PCF            16
#define FLEXNET_ADVERT_RECS_XNET           20

/* Loop containment. These two exist because of one incident
   (2026-09-17, RFC §13.3): a test node flapped, IR2UFV withdrew it
   correctly, the peers echoed the route straight back, we re-learned
   it and re-advertised a finite cost — contradicting our own
   withdrawal — and the destination then circulated for half an hour
   with the cost climbing, outliving the node it named.

   FLEXNET_LEARNED_MAX_AGE prunes learned[] entries nobody refreshes.
   (X)Net does not withdraw a destination it has aged out, it simply
   stops mentioning it, so without this we keep advertising routes our
   own source gave up on.

   The value is a SAFETY NET, not a freshness policy, and it was
   measured the wrong way round first. The initial 600 s came from
   xnet's < 480 s *destination* ageing window — but that is how fast
   xnet drops a route it stops hearing about, not how often it
   re-advertises one it still holds. xnet is event-driven exactly as we
   are, so a stable route is simply not re-mentioned: at 600 s the pass
   pruned live Greek nodes (SV1DZI, SV1HCC) at age=609 s that IW2OHX-14
   still held, then re-learned them, 87 prunes for a net learned-table
   drop of 7 — pure churn, visible only because the poison hold-down
   was suppressing the resulting withdrawals. No RTT=0 refresh markers
   were arriving either (`rtt0-skips` stayed 0), so there is no
   periodic refresh to lean on.

   An hour is therefore the floor: long enough that nothing a peer
   genuinely holds should reach it, short enough that a silently-dropped
   destination cannot outlive its origin by more than that. Withdrawals
   (RTT=60000) remain the primary mechanism; this only catches the case
   where a peer ages a route out without telling anyone. If prunes are
   not ~0 in steady state, the threshold is still too low — do not
   "fix" that by tuning the hold-down. */

#define FLEXNET_LEARNED_MAX_AGE           3600  /* s — prune learned[] */
#define FLEXNET_LEARNED_AGE_SCAN            30  /* s — prune scan period */

/* FLEXNET_POISON_HOLDDOWN stops us un-poisoning a destination on
   hearsay. A finite path reappearing within seconds of our withdrawal
   is our own poison echoing back through the mesh, not a recovery. A
   direct neighbour is exempt — its return is proven by our own session
   coming up, not by what a peer tells us. */
#define FLEXNET_POISON_HOLDDOWN             90  /* s — don't un-poison */

/* ADVERTISEMENT SCOPE — always exactly what we can CARRY.
 *
 * No longer a compile-time choice: the scope is derived from
 * g_flexnet_l2_transit_enabled, because the two are the same question.
 * `flex_advertise_direct_only()` below is the single place that decides.
 *
 *   FLEXNETL2TRANSIT NO  -> direct neighbours only. All we can deliver
 *                           is a destination adjacent to us, reached by
 *                           the stock digipeat.
 *   FLEXNETL2TRANSIT YES -> everything we know. L2 forwarding carries
 *                           multi-hop, verified IW2OHX-14 -> IR2UFV ->
 *                           IW2OHX-4 -> IQ2LB-6.
 *
 * Encoding it this way makes the black hole unreachable by
 * construction. The history below is why that matters:
 *
 *   - A destination that is our DIRECT neighbour works. (X)Net connects
 *     to it with an AX.25 two-digi chain `<peer>* <us>`, we repeat it
 *     (DIGIFLAG=1) and the frame arrives. Proven: `C IW2OHX-4 IR2UFV`
 *     on IW2OHX-14 connects, with our H-bit set in both directions.
 *
 *   - A destination 2+ hops beyond us DOES NOT work, and RFC §4.3's
 *     premise is why. (X)Net does not send a NetROM L4 CREQ for it: it
 *     sends the SAME two-digi chain and expects us to route the frame
 *     onward at layer 2. We repeat it, all digis are then consumed, the
 *     destination is remote, and no node downstream has any role in the
 *     chain — `*** link failure`. Verified with a destination we had
 *     never answered a path query for, so nothing we said misled the
 *     peer: zero CF frames, CF-TRANSIT-FWD stayed 0.
 *
 * Advertising the second class made IW2OHX-4 install 67 destinations
 * via us and PREFER us (67 vs 48 via PC/Flexnet) — 67 black holes on a
 * live network. Re-advertisement makes peers prefer us, so advertising
 * a route we cannot carry is worse than advertising nothing.
 *
 * That milestone landed: FlexNet_L2Transit() implements the symmetric
 * digi-chain rewriting the real routers use, so the second class is now
 * carryable and advertising it is correct — but only on a node that has
 * L2 forwarding switched on. The decision function sits beside
 * g_flexnet_l2_transit_enabled, further down, so it can see it. */
#define FLEXNET_PATH_CACHE_TTL  14400  /* 4h — covers a full round-robin probe
                                          cycle. With ~190 dests at 60s/probe
                                          the cycle is ~3h, so 4h leaves
                                          headroom and avoids re-rendering
                                          stale fallback paths between probes
                                          for the same target. (item #9 partial) */
/* AX.25's address field holds at most 8 digipeaters, and that ceiling is
   FlexNet's only hop limit — AX.25 has no TTL. It bounds TWO things:
   how far FlexNet_L2Transit() may grow a chain, and how long a path we
   may answer a PATH_REQ with. Answering with more digis than this hands
   the asking peer a chain it cannot express, which is strictly worse
   than silence (see flex_handle_path_req). */
#define FLEXNET_L2_MAX_DIGIS        8
/* How long a departed peer's learned routes stay adoptable. Long enough
   to cover a DISC/SABM cycle (seconds) and an operator restart, short
   enough that a peer genuinely gone does not come back to a stale view. */
#define FLEXNET_LEARNED_ADOPT_MAX_AGE 600
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

/* ── Production build switch ────────────────────────────────────────────
 *
 * FLEXNET_PROD=1 silences ALL informational FlexNet console output
 * (session lifecycle, route advertisement, neighbour add, etc.). Per-
 * frame trace gated by FLEXNET_DEBUG is silenced separately.
 *
 * Rebuild silent (production):
 *     make CFLAGS+="-DFLEXNET_PROD=1"
 * Rebuild verbose (development — default):
 *     make
 */
#ifndef FLEXNET_PROD
#define FLEXNET_PROD 0
#endif

/* FlexNet_Info — informational Console output, suppressed in production
 * builds (FLEXNET_PROD=1). All previous unconditional
 * FlexNet_Info("FlexNet: ...") sites should use this macro. */
#define FlexNet_Info(...) \
    do { if (!FLEXNET_PROD) Consoleprintf(__VA_ARGS__); } while (0)

/* FlexNet_Trace — the v2.2 transit state-machine lines (RFC §10.1.a:
 * ADVERT-CHECK / BUCKET / POISON / 3PLUS-WALK, plus SEED and
 * NBR-REFRESH). These four are the substitute for unit tests, and
 * "grep, diff and reason about" needs a time base the console does not
 * carry — so they go to the timestamped traffic log as well. Both
 * halves are FLEXNET_DEBUG-gated, so a production build emits neither.
 */
#define FlexNet_Trace(...) \
    do { if (FLEXNET_DEBUG) { FlexNet_Log(__VA_ARGS__); \
                              FlexNet_Info(__VA_ARGS__); } } while (0)

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
    /* v2.1.39 — establishment inferred from sustained peer traffic when
       the peer's one-shot type-0 INIT was missed (BPQ recycled our
       LINKTABLE slot mid-session while the peer's L2 link stayed up, so
       its INIT — emitted once at L2 setup — is long gone). Kept separate
       from got_peer_init so the latter stays the literal "we saw the
       peer's INIT frame" signal. See flex_note_peer_established(). */
    BOOL flex_est_inferred;
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
    /* Peer KA shape, captured from the last KA we accepted. Used by
       flex_build_keepalive to echo a matching-shape frame so PC/Flexnet
       (201 B, '2'+199 sp+CR) and (X)Net (241 B, '2'+240 sp, no CR) peers
       each receive what they emit. Zero = no KA seen yet → fall back to
       the (X)Net default. */
    int           peer_ka_len;
    unsigned char peer_ka_term;
    /* v2.1.12 — PCF active-probe pong heartbeat (see asmstrucs.h). */
    time_t        last_status10;
    /* v2.1.13 — last outbound LT TX; see asmstrucs.h for the rate-
       limit rationale (PCF's link.ts math forces ≥ 320s for PCF). */
    time_t        last_lt_tx;
    /* v2.1.14 — reap hysteresis (see asmstrucs.h). */
    int           reap_strikes;
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

/* v2.2 — per-neighbour table of destinations learned FROM this neighbour.
   Used by the periodic transit re-advertisement timer to re-emit these
   destinations into OTHER sessions, with RTT = learned_rtt + link_rtt
   to this session's peer. Split-horizon: never re-advertise back to
   the source session. Defined OUTSIDE the FLEXNET_DEST_DEFINED guard
   (asmstrucs.h defines that guard for FLEXNET_SESSION but doesn't carry
   our new v2.2 types). */
struct FLEXNET_LEARNED_ROUTE
{
    char    dest_call[FLEXNET_MAX_CALLSIGN];
    int     ssid_lo;
    int     ssid_hi;
    int     rtt_at_neighbour;
    time_t  last_heard;
    /* Set by FlexNet_InitSession for the neighbour's own call, never
       cleared: direct neighbours are the load-bearing transit-shape
       carriers, so RFC §5.5 refreshes them on a timer even when their
       RTT hasn't moved. Everything else is emitted on change only. */
    BOOL    is_direct_neighbour;
};

struct FLEXNET_LEARNED_STATE
{
    struct FLEXNET_LEARNED_ROUTE routes[FLEXNET_MAX_LEARNED_PER_NEIGHBOUR];
    int     count;
    BOOL    dirty;
    time_t  last_advert;
    /* RFC §15 Q6 — anchor for trigger (b). `expected` depends on our
       link RTT to this peer, but walking 200+ learned routes on every
       IIR wiggle would be pathological, so trigger (b) only fires once
       our_link_time has moved >= 1 tick from this anchor. */
    int     lt_anchor;
    /* Whose table this is, and when its session went away. Session
       slots are reused, so the slot index alone cannot tell us whether
       a reconnecting peer is the previous occupant — and rebuilding a
       returning peer's table from scratch is what turns a link blip
       into a full re-advertisement. See flex_learned_adopt(). */
    char    peer_call[20];
    time_t  died_at;
};

/* Parallel to FlexNetSessions[] — indexed by the same session_idx.
   Kept separate from FLEXNET_SESSION so we don't have to modify the
   external struct definition in asmstrucs.h. */
struct FLEXNET_LEARNED_STATE FlexNetLearned[FLEXNET_MAX_SESSIONS];

/* v2.2 rc4 — what we have TOLD each peer, per destination (RFC §5.1).
   Same key space as learned[] but indexed by the peer we advertise TO,
   not the peer we learned FROM. This is the source of truth for "does
   this peer need an update?", and it is what makes emission idempotent:
   without it the only way to know what a peer already knows is to
   re-send everything, which is how rc1 flooded PC/Flexnet. */
struct FLEXNET_ADVERTISED_ROUTE
{
    char    dest_call[FLEXNET_MAX_CALLSIGN];
    int     ssid_lo;
    int     ssid_hi;
    /* Last RTT actually put on the wire to this peer. -1 = never
       advertised, which always fires the decision rule and is also
       what suppresses poisoning a route the peer never heard. */
    int     last_advertised_rtt;
    time_t  last_advertised_at;
    /* Queue slot. advs[] doubles as the pending queue so a burst of
       changes to one destination collapses into a single wire frame:
       pending_rtt is overwritten in place while the bucket is dry. */
    BOOL    pending;
    int     pending_rtt;
};

struct FLEXNET_ADVERTISED_STATE
{
    struct FLEXNET_ADVERTISED_ROUTE advs[FLEXNET_MAX_ADVERTISED_PER_PEER];
    int     count;
    /* Token bucket (RFC §5.4). Fractional credit, so a 2 s refill on a
       1 s timer tick accumulates correctly instead of truncating to 0. */
    double  tokens;
    time_t  last_tokens_refill;
    /* A trailing '3-' owed to this peer after its `3+` walk drains. */
    BOOL    eob_pending;
    BOOL    warned_full;
};

struct FLEXNET_ADVERTISED_STATE FlexNetAdvertised[FLEXNET_MAX_SESSIONS];

/* v2.2 — in-flight transit-forwarded NetROM L4 sessions. One entry
   per CREQ we accept and forward to a downstream neighbour. */
struct FLEXNET_TRANSIT_SESSION
{
    BOOL    active;
    int     in_session_idx;
    int     in_circuit_index;
    int     in_circuit_id;
    int     out_session_idx;
    int     out_circuit_index;
    int     out_circuit_id;
    char    origin_user[7];
    char    origin_node[7];
    char    dest_call[7];
    time_t  last_activity;
};

struct FLEXNET_TRANSIT_SESSION FlexNetTransitSessions[FLEXNET_MAX_TRANSIT_SESSIONS];

/* FLEXNETTRANSIT directive master enable.
   Compiled default NO — RFC §15 Q2 as superseded 2026-09-14 (rc4 D1).
   Transit is a role a node opts into, never one it inherits by omission:
   a YES default silently enabled re-advertisement on any node whose
   bpq32.cfg lacked the directive, which is how production ended up
   emitting transit records for months. When FALSE the node behaves as a
   pure v2.1 leaf — no re-advertisement, no CREQ forwarding, no transit
   bookkeeping. A transit node must set `FLEXNETTRANSIT YES` explicitly. */
BOOL g_flexnet_transit_enabled = FALSE;

/* FLEXNETPATHFORWARD — relay CE type-6 path traversals (see
   flex_forward_path_req). Separate switch, separately defaulted off:
   forwarding puts our callsign into other stations' path queries and
   generates a frame per hop, which is router behaviour a node should
   not begin doing because it inherited a setting. */
BOOL g_flexnet_path_forward_enabled = FALSE;
static unsigned long g_path_fwd_sent     = 0;
static unsigned long g_path_fwd_declined = 0;
static unsigned long g_path_rep_relayed  = 0;

/* FLEXNETL2TRANSIT — the L2 forwarding switch (see FlexNet_L2Transit).
   Default NO, and deliberately a SEPARATE directive from
   FLEXNETTRANSIT: advertising routes is comparatively harmless, whereas
   rewriting other stations' frames is not something a node should begin
   doing because it inherited a setting. Requires FLEXNETTRANSIT too —
   carrying traffic for destinations we do not advertise would be
   pointless, and advertising without carrying is the black hole this
   whole exercise started from. */
BOOL g_flexnet_l2_transit_enabled = FALSE;

/* FLEXNETLT3BYTE — accept a 3-byte "1n\r" as LINK_TIME. Default NO, so
   production cannot inherit it.

   Captured 2026-09-17: (X)Net answers our 3-byte LT within 1 ms, and the
   reply's LENGTH follows the VALUE — IW2OHX-4 sends '1600\r',
   IW2OHX-12 '12348\r', IW2OHX-14 '10\r'. But flex_parse_ce_frame only
   reaches CE_FRAME_LINK_TIME for len > 3, so the STATUS_10 / STATUS_1N
   branches claim the 3-byte forms and we discard exactly the SMALLEST
   link times — the ones a fast link reports. `lt_sample` has fired twice
   each for -4 and -12 and never once for -14.

   It matters because our_link_time is the second term in every cost we
   advertise (expected = learned_rtt + our_link_time); with no sample
   folded it stays at the 2 FlexNet_InitSession seeds.

   Gated rather than simply fixed: the LINK_TIME path REPLIES, so
   enabling this makes us answer frames we currently ignore, a cadence
   change on any link whose peer reports a single-digit time.
   PC/Flexnet is unaffected either way — it already sends len > 3. */
BOOL g_flexnet_lt3byte_enabled = FALSE;

/* L2 forwarding outcome counters, surfaced by `FL`. `declined` being
   large is not a fault: it counts every frame we looked at and left to
   the stock digipeat, which is the correct answer for an adjacent
   destination. */
static unsigned long g_l2_fwd_extended = 0;
static unsigned long g_l2_fwd_contracted = 0;
static unsigned long g_l2_fwd_declined = 0;

/* Advertisement scope — see the note beside the bucket constants.
   We advertise exactly what we can carry: direct neighbours only until
   L2 forwarding is switched on, everything once it is. */
static BOOL flex_advertise_direct_only(void)
{
    return !g_flexnet_l2_transit_enabled;
}

/* Last learned[] prune sweep — see flex_learned_age_scan(). */
static time_t g_last_learned_age_scan = 0;

/* Count of RTT=0 refresh-marker records skipped (§15 Q5 / test B8).
   These are dropped in flex_dtable_merge before learned[], so they can
   never reach the decision rule — the counter is what makes that
   visible rather than merely asserted. */
static unsigned long g_flexnet_rtt0_skips = 0;

/* SSID range advertised to FlexNet peers (v1.10.0).
   Configured via the `FLEXNETSSIDRANGE N-M` directive in bpq32.cfg.
   When unset, defaults to (MYCALL_SSID, MYCALL_SSID) — preserving
   pre-v1.10.0 behaviour where only the node SSID is announced.
   The range is purely a FlexNet-layer advertisement: incoming
   connects to MYCALL-N (N in [lo, hi]) are still routed by BPQ's
   existing APPLICATION-call matching — so a SSID in the advertised
   range only accepts connects if there is an APPLICATION line
   binding it. */
static int g_flexnet_ssid_lo = -1;   /* -1 sentinel = not configured */
static int g_flexnet_ssid_hi = -1;

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

/* v2.1.23 — REVERTED: the v2.1.17→v2.1.22 INIT cooldown machinery
   has been removed. Wire-trace evidence (IR2UFV ↔ IW2OHX-12,
   2026-05-28) showed PC/Flexnet intentionally cycles the L2 link
   itself with `DISC+` / new-`SABM+` after each token-handover round.
   On every fresh L2 session PCF expects a full CE handshake
   (our INIT, then PCF's INIT, then RTT-ping/pong and route
   exchange). Suppressing our INIT made PCF rebuild the peer entry
   with default `max_ssid=15` instead of our configured `0-8`. The
   periodic `600 4095` cost-ring reseed is now accepted as the
   normal side-effect of PCF's protocol — v2.1.13's LT rate-limit
   re-converges the ring to `2/2` within minutes after each
   reseed, so the cost remains correct in steady state. */

/* Helper kept from v2.1.19 — converts an AX.25 LINKCALL to its
   human-readable form (e.g. "IW2OHX-12") for byte-format-agnostic
   comparison. Still used by the v2.1.16 reaper-time LINK-migration
   scan. */
static void flex_normalize_callsign(const unsigned char * ax25_callsign,
                                    char * out, size_t outlen)
{
    char tmp[20] = {0};
    ConvFromAX25((unsigned char *)ax25_callsign, tmp);
    int sl = (int)strlen(tmp);
    while (sl > 0 && tmp[sl-1] == ' ') tmp[--sl] = '\0';
    if (outlen == 0) return;
    strncpy(out, tmp, outlen - 1);
    out[outlen - 1] = '\0';
}

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
static int  flex_build_keepalive(unsigned char * buf, int buflen,
                                 const struct FLEXNET_SESSION * sess);
static int  flex_build_link_time(unsigned char * buf, int buflen, int value);
static int  flex_send_link_time(LINKTABLE * LINK,
                struct FLEXNET_SESSION * sess);
static void flex_link_time_sample(struct FLEXNET_SESSION * sess);
static int  flex_build_init(unsigned char * buf, int buflen, int max_ssid);
static int  flex_build_route(unsigned char * buf, int buflen,
                const char * callsign, int ssid_lo, int ssid_hi, int rtt);
static int  flex_dtable_merge(struct FLEXNET_DEST_ENTRY * incoming,
                              struct FLEXNET_SESSION * sess);
/* v2.2 — record a destination learned from this session in our
   per-session learned[] table. Idempotent: find-or-update by
   (callsign, ssid_lo, ssid_hi). Called from flex_dtable_merge for
   every record we accept. Sets the source session's dirty flag so
   the next periodic timer picks the change up for re-advertisement. */
/* Adopt a returning peer's learned routes into its new session slot.
 *
 * A peer that DISCs and comes back seconds later still knows everything
 * it knew before, and so do we — but the slot it lands in is whichever
 * one happened to be free, and we used to memset that slot's learned
 * table. The console showed the cost plainly: `total learned=141` …
 * `total learned=1`, followed by ~198 routes re-learned and
 * re-advertised to every other peer.
 *
 * Returns TRUE if a table was adopted. Matching is by callsign, bounded
 * by FLEXNET_LEARNED_ADOPT_MAX_AGE so a peer absent for a long time
 * starts clean rather than resurrecting a stale view.
 *
 * Deliberately does NOT adopt advertised[]: what a returning peer still
 * remembers of OUR routes is its business, not ours, and under-
 * advertising to a peer that dropped its table is the worse failure.
 */
static BOOL flex_learned_adopt(int fresh_idx, const char * peer_call)
{
    if (!peer_call || !peer_call[0]) return FALSE;

    time_t now = time(NULL);
    int    near_miss_age = -1;

    for (int si = 0; si < FLEXNET_MAX_SESSIONS; si++)
    {
        /* fresh_idx is deliberately NOT skipped. A peer usually lands
           back in the slot it just vacated, because InitSession takes
           the first free slot and that is the one it freed. Skipping
           that case meant the common reconnect adopted nothing and the
           caller then memset the very table we could have kept —
           IW2OHX-12 reconnected at 08:19 on 2026-09-18 with
           LEARNED-ADOPT still at 0, which is what exposed it. */
        if (si != fresh_idx && FlexNetSessions[si].active)
            continue;                               /* still someone's */

        struct FLEXNET_LEARNED_STATE * old = &FlexNetLearned[si];
        if (old->count <= 0) continue;
        if (strcasecmp(old->peer_call, peer_call) != 0) continue;
        if (old->died_at &&
            (now - old->died_at) > FLEXNET_LEARNED_ADOPT_MAX_AGE)
        {
            /* Right peer, too long gone. Worth logging: silence here is
               indistinguishable from "no table found at all". */
            near_miss_age = (int)(now - old->died_at);
            continue;
        }

        if (si == fresh_idx)
        {
            /* Already in the right slot — keep it in place. The caller
               must not memset, which is what the TRUE return prevents. */
            long gone = old->died_at ? (long)(now - old->died_at) : 0L;
            FlexNet_Info("FlexNet: LEARNED-ADOPT %s slot %d in place, kept "
                         "%d routes (gone %lds) — no re-advertisement storm",
                         peer_call, si, old->count, gone);
            old->died_at = 0;
            return TRUE;
        }

        int  adopted = old->count;
        long gone    = old->died_at ? (long)(now - old->died_at) : 0L;

        memcpy(&FlexNetLearned[fresh_idx], old, sizeof(*old));
        FlexNetLearned[fresh_idx].died_at = 0;
        /* The old slot must not keep a second copy: two slots claiming
           the same routes would double-count in flex_expected_rtt. */
        memset(old, 0, sizeof(*old));

        FlexNet_Info("FlexNet: LEARNED-ADOPT %s slot %d -> %d, kept %d "
                     "routes (gone %lds) — no re-advertisement storm",
                     peer_call, si, fresh_idx, adopted, gone);
        return TRUE;
    }

    if (near_miss_age >= 0)
    {
        FlexNet_Info("FlexNet: LEARNED-ADOPT declined for %s — table was "
                     "%ds old, limit %ds; starting clean",
                     peer_call, near_miss_age, FLEXNET_LEARNED_ADOPT_MAX_AGE);
        return FALSE;
    }

    /* Nothing matched, and three rounds of inferring why from the
       outside got it wrong each time. Dump what the scan actually saw so
       the next reconnect answers the question instead of prompting
       another guess. */
    FlexNet_Info("FlexNet: LEARNED-ADOPT found nothing for %s (fresh slot "
                 "%d) — slot states follow", peer_call, fresh_idx);
    for (int si = 0; si < FLEXNET_MAX_SESSIONS; si++)
    {
        struct FLEXNET_LEARNED_STATE * st = &FlexNetLearned[si];
        if (!st->count && !st->peer_call[0] && !FlexNetSessions[si].active)
            continue;                       /* never used — say nothing */
        FlexNet_Info("FlexNet:   slot %d active=%d count=%d peer_call='%s' "
                     "died_at=%s", si, FlexNetSessions[si].active ? 1 : 0,
                     st->count, st->peer_call,
                     st->died_at ? "set" : "0");
    }
    return FALSE;
}

static void flex_learned_add(int sess_idx,
                             const struct FLEXNET_DEST_ENTRY * route);
/* v2.2 rc4 event-driven re-advertisement (RFC §5). flex_advertise_check
   is the single decision point: it recomputes what we'd tell `peer_idx`
   about one destination and queues a record only when that differs from
   what we last told it. Everything else — inbound RTT change, link-RTT
   drift, session loss, the direct-neighbour timer, a `3+` request —
   funnels through it, so there is exactly one place where emission
   policy lives. `force` bypasses only the jitter floor (the §5.5
   keepalive path), never the split-horizon or poison guards. */
static int  flex_expected_rtt(int peer_idx, const char * dest_call,
                              int ssid_lo, int ssid_hi, int * src_idx_out,
                              BOOL * src_is_direct_out);
static void flex_learned_age_scan(time_t now);
static BOOL flex_advertise_direct_only(void);
static int  flex_parse_l2transit_line(const char * line);
static int  flex_session_for_call(const char * call);
static int  flex_parse_bool_line(const char * line, const char * key,
                                 BOOL * out);
static int  flex_parse_lt3byte_line(const char * line);
static int  flex_find_dest_for_target(const char * target);
/* Callsign decoders. ConvFromAX25 writes more than 10 chars, so both
   buffers must be >= 20 — a FLEXNET_MAX_CALLSIGN-sized one overflows. */
static void flex_own_base_call(char * buf, int buflen, int * ssid_out);
static void flex_sess_peer_call(const struct FLEXNET_SESSION * sess,
                                char * buf, int buflen);
static BOOL flex_peer_is_pcf(const struct FLEXNET_SESSION * sess);
static void flex_advertise_check(int peer_idx, const char * dest_call,
                                 int ssid_lo, int ssid_hi, BOOL force);
static void flex_advertise_drain(int peer_idx);
static void flex_advertise_walk_for_peer(int peer_idx, BOOL direct_only,
                                         BOOL force, int * walked,
                                         int * queued);
static int  flex_find_dest(const char * call, int ssid_lo, int ssid_hi);
static struct FLEXNET_SESSION * flex_find_session(LINKTABLE * LINK);
static void flex_send_frame(LINKTABLE * LINK, unsigned char pid,
                unsigned char * data, int len);
/* Emit our OWN route record to one peer. `defer_eob` suppresses the
   trailing '3-' so a `3+` response can send exactly one end-of-batch
   token after the transit records it queued have drained. */
static void flex_send_own_routes(LINKTABLE * LINK, BOOL defer_eob);
static void flex_advertise_neighbours(int peer_idx);
static void flex_advertise_seed_peer(int peer_idx);
static void flex_advertise_poison_session(int dead_idx,
                                          const char * dead_call);
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
#define CE_FRAME_STATUS_1N   11   /* "1n\r" status family (n=1..9; n=0 is STATUS_10) */

/* ── Initialization ──────────────────────────────────────────────────── */

/* Parse one configuration line for a `FLEXNETSSIDRANGE N-M` directive.
   Case-insensitive on the keyword. Returns 1 if matched (whether or not
   the values were valid), 0 if the line is unrelated. */
static int flex_parse_ssidrange_line(const char * line)
{
    /* Skip leading whitespace */
    while (*line == ' ' || *line == '\t') line++;
    /* Ignore comments and blank lines */
    if (*line == '\0' || *line == ';' || *line == '#' || *line == '\r' ||
        *line == '\n')
        return 0;

    static const char key[] = "FLEXNETSSIDRANGE";
    int klen = (int)sizeof(key) - 1;
    for (int i = 0; i < klen; i++)
    {
        char a = line[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (a != key[i]) return 0;
    }
    /* After keyword, expect whitespace or '=' */
    const char * p = line + klen;
    if (*p != ' ' && *p != '\t' && *p != '=' && *p != ':') return 1;
    while (*p == ' ' || *p == '\t' || *p == '=' || *p == ':') p++;

    int lo = -1, hi = -1;
    /* Accept "N-M" or "N M" or just "N" (single SSID) */
    if (*p < '0' || *p > '9') return 1;
    lo = 0;
    while (*p >= '0' && *p <= '9') { lo = lo * 10 + (*p - '0'); p++; }
    while (*p == ' ' || *p == '\t' || *p == '-') p++;
    if (*p >= '0' && *p <= '9')
    {
        hi = 0;
        while (*p >= '0' && *p <= '9') { hi = hi * 10 + (*p - '0'); p++; }
    }
    else
    {
        hi = lo;
    }

    if (lo < 0 || lo > 15 || hi < 0 || hi > 15 || lo > hi)
    {
        FlexNet_Info("FlexNet: ignoring invalid FLEXNETSSIDRANGE %d-%d "
                      "(must be 0..15, lo<=hi)", lo, hi);
        return 1;
    }
    g_flexnet_ssid_lo = lo;
    g_flexnet_ssid_hi = hi;
    return 1;
}

/* v2.2 — parse `FLEXNETTRANSIT YES|NO|ON|OFF|1|0` directive.
   Per RFC §15 Q2 the default is YES. Setting NO returns the node to
   pure v2.1 leaf behaviour. Returns 1 if matched, 0 otherwise. */
static int flex_parse_transit_line(const char * line)
{
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == ';' || *line == '#' || *line == '\r' ||
        *line == '\n')
        return 0;

    static const char key[] = "FLEXNETTRANSIT";
    int klen = (int)sizeof(key) - 1;
    for (int i = 0; i < klen; i++)
    {
        char a = line[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (a != key[i]) return 0;
    }
    const char * p = line + klen;
    if (*p != ' ' && *p != '\t' && *p != '=' && *p != ':') return 1;
    while (*p == ' ' || *p == '\t' || *p == '=' || *p == ':') p++;

    /* Accept YES/NO/ON/OFF/1/0 case-insensitive. */
    char buf[8] = {0};
    int bi = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' &&
           bi < (int)sizeof(buf) - 1)
    {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        buf[bi++] = c;
        p++;
    }
    buf[bi] = '\0';

    if (strcmp(buf, "YES") == 0 || strcmp(buf, "ON") == 0 ||
        strcmp(buf, "1") == 0   || strcmp(buf, "TRUE") == 0)
    {
        g_flexnet_transit_enabled = TRUE;
    }
    else if (strcmp(buf, "NO")  == 0 || strcmp(buf, "OFF") == 0 ||
             strcmp(buf, "0")   == 0 || strcmp(buf, "FALSE") == 0)
    {
        g_flexnet_transit_enabled = FALSE;
    }
    else
    {
        FlexNet_Info("FlexNet: ignoring invalid FLEXNETTRANSIT value '%s' "
                      "(expected YES|NO|ON|OFF|1|0)", buf);
    }
    return 1;
}

/* Generic `KEY YES|NO|ON|OFF|1|0|TRUE|FALSE` parser. Returns 1 if the
   line matched KEY (whether or not the value was valid), 0 otherwise.
   New directives should use this rather than copying the block below —
   there are already two near-identical copies and a third would be the
   point at which they start drifting apart. */
static int flex_parse_bool_line(const char * line, const char * key,
                                BOOL * out)
{
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == ';' || *line == '#' || *line == '\r' ||
        *line == '\n')
        return 0;

    int klen = (int)strlen(key);
    for (int i = 0; i < klen; i++)
    {
        char a = line[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (a != key[i]) return 0;
    }

    const char * p = line + klen;
    if (*p != ' ' && *p != '\t' && *p != '=' && *p != ':') return 1;
    while (*p == ' ' || *p == '\t' || *p == '=' || *p == ':') p++;

    char buf[8] = {0};
    int bi = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' &&
           bi < (int)sizeof(buf) - 1)
    {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        buf[bi++] = c;
        p++;
    }
    buf[bi] = '\0';

    if (strcmp(buf, "YES") == 0 || strcmp(buf, "ON") == 0 ||
        strcmp(buf, "1") == 0   || strcmp(buf, "TRUE") == 0)
        *out = TRUE;
    else if (strcmp(buf, "NO")  == 0 || strcmp(buf, "OFF") == 0 ||
             strcmp(buf, "0")   == 0 || strcmp(buf, "FALSE") == 0)
        *out = FALSE;
    else
        FlexNet_Info("FlexNet: ignoring invalid %s value '%s' "
                      "(expected YES|NO|ON|OFF|1|0)", key, buf);
    return 1;
}

/* v2.3 — parse `FLEXNETL2TRANSIT YES|NO|ON|OFF|1|0`.
   Deliberately a separate directive from FLEXNETTRANSIT, and separately
   defaulted off: advertising routes is comparatively harmless, whereas
   rewriting other stations' frames is not something a node should begin
   doing because it inherited a setting. Returns 1 if matched. */
static int flex_parse_l2transit_line(const char * line)
{
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == ';' || *line == '#' || *line == '\r' ||
        *line == '\n')
        return 0;

    static const char key[] = "FLEXNETL2TRANSIT";
    int klen = (int)sizeof(key) - 1;
    for (int i = 0; i < klen; i++)
    {
        char a = line[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (a != key[i]) return 0;
    }
    const char * p = line + klen;
    if (*p != ' ' && *p != '\t' && *p != '=' && *p != ':') return 1;
    while (*p == ' ' || *p == '\t' || *p == '=' || *p == ':') p++;

    char buf[8] = {0};
    int bi = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' &&
           bi < (int)sizeof(buf) - 1)
    {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        buf[bi++] = c;
        p++;
    }
    buf[bi] = '\0';

    if (strcmp(buf, "YES") == 0 || strcmp(buf, "ON") == 0 ||
        strcmp(buf, "1") == 0   || strcmp(buf, "TRUE") == 0)
        g_flexnet_l2_transit_enabled = TRUE;
    else if (strcmp(buf, "NO")  == 0 || strcmp(buf, "OFF") == 0 ||
             strcmp(buf, "0")   == 0 || strcmp(buf, "FALSE") == 0)
        g_flexnet_l2_transit_enabled = FALSE;
    else
        FlexNet_Info("FlexNet: ignoring invalid FLEXNETL2TRANSIT value "
                      "'%s' (expected YES|NO|ON|OFF|1|0)", buf);
    return 1;
}

/* Parse `FLEXNETLT3BYTE YES|NO|ON|OFF|1|0`. Returns 1 if matched. */
static int flex_parse_lt3byte_line(const char * line)
{
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == ';' || *line == '#' || *line == '\r' ||
        *line == '\n')
        return 0;

    static const char key[] = "FLEXNETLT3BYTE";
    int klen = (int)sizeof(key) - 1;
    for (int i = 0; i < klen; i++)
    {
        char a = line[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (a != key[i]) return 0;
    }
    const char * p = line + klen;
    if (*p != ' ' && *p != '\t' && *p != '=' && *p != ':') return 1;
    while (*p == ' ' || *p == '\t' || *p == '=' || *p == ':') p++;

    char buf[8] = {0};
    int bi = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' &&
           bi < (int)sizeof(buf) - 1)
    {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        buf[bi++] = c;
        p++;
    }
    buf[bi] = '\0';

    if (strcmp(buf, "YES") == 0 || strcmp(buf, "ON") == 0 ||
        strcmp(buf, "1") == 0   || strcmp(buf, "TRUE") == 0)
        g_flexnet_lt3byte_enabled = TRUE;
    else if (strcmp(buf, "NO")  == 0 || strcmp(buf, "OFF") == 0 ||
             strcmp(buf, "0")   == 0 || strcmp(buf, "FALSE") == 0)
        g_flexnet_lt3byte_enabled = FALSE;
    else
        FlexNet_Info("FlexNet: ignoring invalid FLEXNETLT3BYTE value "
                      "'%s' (expected YES|NO|ON|OFF|1|0)", buf);
    return 1;
}

/* Read bpq32.cfg from the cwd and look for our directives. LinBPQ has
   already parsed its own keys before our Init runs; we only consume the
   ones it ignores. Soft-failure: if the file can't be opened, just keep
   defaults. */
static void flex_load_config(void)
{
    FILE * fp = fopen("bpq32.cfg", "r");
    if (!fp)
    {
        FlexNet_Info("FlexNet: bpq32.cfg not readable — keeping defaults");
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), fp))
    {
        if (flex_parse_ssidrange_line(line)) continue;
        if (flex_parse_transit_line(line))   continue;
        if (flex_parse_l2transit_line(line)) continue;
        if (flex_parse_bool_line(line, "FLEXNETPATHFORWARD",
                                 &g_flexnet_path_forward_enabled)) continue;
        if (flex_parse_lt3byte_line(line))   continue;
    }
    fclose(fp);
}

static int g_flexnet_init_done = 0;

void FlexNet_Init(void)
{
    if (g_flexnet_init_done) return;
    g_flexnet_init_done = 1;

    memset(FlexNetDests, 0, sizeof(FlexNetDests));
    FlexNetDestCount = 0;
    memset(FlexNetSessions, 0, sizeof(FlexNetSessions));
    FlexNetSessionCount = 0;
    memset(FlexNetProbes, 0, sizeof(FlexNetProbes));
    memset(FlexNetLearned, 0, sizeof(FlexNetLearned));
    memset(FlexNetAdvertised, 0, sizeof(FlexNetAdvertised));
    memset(FlexNetTransitSessions, 0, sizeof(FlexNetTransitSessions));
    flex_load_config();
    FlexNet_Info("FlexNet: initialized (max %d dests, %d sessions, "
                  "transit-role %s)",
                FLEXNET_MAX_DESTS, FLEXNET_MAX_SESSIONS,
                g_flexnet_transit_enabled ? "ENABLED (v2.2)" : "disabled");
    if (g_flexnet_transit_enabled)
        FlexNet_Info("FlexNet: L2 forwarding %s — advertising %s",
                      g_flexnet_l2_transit_enabled ? "ENABLED (digi-chain "
                          "rewriting)" : "disabled",
                      g_flexnet_l2_transit_enabled
                          ? "ALL learned destinations (we can carry them)"
                          : "direct neighbours only (all we can carry)");
    if (g_flexnet_lt3byte_enabled)
        FlexNet_Info("FlexNet: 3-byte LINK_TIME accepted (FLEXNETLT3BYTE) "
                      "— single-digit link times now fold into "
                      "our_link_time");
    if (g_flexnet_ssid_lo >= 0)
        FlexNet_Info("FlexNet: advertising SSID range %d-%d "
                      "(from FLEXNETSSIDRANGE)",
                      g_flexnet_ssid_lo, g_flexnet_ssid_hi);
}

/* ── Session management ──────────────────────────────────────────────── */

static struct FLEXNET_SESSION * flex_find_session(LINKTABLE * LINK)
{
    for (int i = 0; i < FlexNetSessionCount; i++)
        if (FlexNetSessions[i].LINK == LINK && FlexNetSessions[i].active)
            return &FlexNetSessions[i];
    return NULL;
}

/* v2.1.39 — a session is "established" for FL-status / route-advert /
   re-init-guard purposes if EITHER we saw the peer's INIT (got_peer_init)
   OR we inferred establishment from sustained peer traffic. */
static BOOL flex_is_established(const struct FLEXNET_SESSION * sess)
{
    return sess->got_peer_init || sess->flex_est_inferred;
}

/* v2.1.39 — infer FlexNet-layer establishment from sustained peer traffic
   when the one-shot type-0 INIT was missed.

   The peer emits its type-0 INIT exactly once, right after the AX.25 L2
   session comes up (skill §1.3). Whenever BPQ recycles our LINKTABLE slot
   mid-session (reaper / auto-recreate at flex_find_session, or the
   new-LINK-same-callsign path) while the peer's L2 link stays continuously
   up, our session object is reborn long after that INIT — so got_peer_init
   can never flip and FL pins the link at PENDING, even though KA, link-time
   and compact-route frames all flow normally and the peer's own L-table
   shows us fully converged (observed 2026-07-09: -14/-4 report our nodes at
   Q=4 RTT=2/5, rr+% ~0.1%, while IW2OHX-13/IR2UFV showed PENDING).

   A link exchanging valid CE frames over a healthy L2 *is* up — the
   symmetric truth to the v2.1.11 "INIT receipt is sufficient evidence the
   FlexNet layer is up" note. Promote so FL, the KA route-advert gate and
   the re-init guard reflect reality. Idempotent; a real later INIT still
   sets got_peer_init independently. */
static void flex_note_peer_established(struct FLEXNET_SESSION * sess)
{
    if (sess->got_peer_init || sess->flex_est_inferred)
        return;
    if (!sess->LINK || sess->LINK->L2STATE != 5)
        return;
    sess->flex_est_inferred = TRUE;
    FlexNet_Info("FlexNet: session established via sustained peer traffic "
                 "(one-shot INIT missed — BPQ recycled the LINK mid-session); "
                 "promoting out of PENDING");
}

void FlexNet_InitSession(LINKTABLE * LINK, int Port)
{
    /* Lazy first-time init. FlexNet_Init has no external caller in
       stock LinBPQ startup; we self-bootstrap on the first session
       event. By then MYCALL is set and bpq32.cfg is in our cwd, so
       flex_load_config() can find our directives. The init_done flag
       inside makes subsequent calls cheap. */
    FlexNet_Init();

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
            /* v2.1.15 — guard against spurious re-init when the only thing
               that changed is LINK->FlexNetLink going FALSE. Some BPQ-internal
               L2 maintenance paths clear LINKTABLE state without sending any
               U-frame on the wire, which our proactive scan would otherwise
               interpret as a need to re-handshake — re-sending INIT to the
               peer reseeds its link-cost ring (the `600 4095 …` outliers we
               observed on PC/Flexnet's L *) even though the L2 link is fine.
               If our session for this LINK is already established (i.e. we
               have received the peer's INIT), just re-promote the LINK flag
               and return. */
            LINK->FlexNetLink = TRUE;
            /* v2.1.16 — refresh stashed callsign in case BPQ remapped the
               LINKCALL underneath us (rare but harmless to refresh). */
            memcpy(FlexNetSessions[i].peer_callsign, LINK->LINKCALL, 7);

            if (flex_is_established(&FlexNetSessions[i]))
            {
                if (FLEXNET_DEBUG)
                    FlexNet_Info("FlexNet: re-promoted LINK on port %d "
                                  "(session slot %d already established, no re-init)",
                                  Port, i);
                return;
            }

            /* Session was never established (still mid-handshake) — full
               original reset-and-retransmit flow. */
            FlexNetSessions[i].sent_routes = FALSE;  /* re-advertise */
            FlexNetSessions[i].got_peer_init = FALSE;
            FlexNetSessions[i].flex_est_inferred = FALSE;
            FlexNetSessions[i].keepalive_count = 0;
            FlexNetSessions[i].session_start = time(NULL);
            FlexNetSessions[i].last_keepalive = time(NULL);

            int node_ssid = (MYCALL[6] >> 1) & 0x0F;
            int init_max  = (g_flexnet_ssid_hi >= 0) ? g_flexnet_ssid_hi : node_ssid;
            unsigned char init[8];
            int ilen = flex_build_init(init, sizeof(init), init_max);
            if (ilen > 0)
                flex_send_frame(LINK, FLEXNET_PID_CE, init, ilen);

            unsigned char ka[FLEXNET_KEEPALIVE_LEN];
            int klen = flex_build_keepalive(ka, sizeof(ka), &FlexNetSessions[i]);
            if (klen > 0)
                flex_send_frame(LINK, FLEXNET_PID_CE, ka, klen);

            FlexNet_Info("FlexNet: session re-handshake on port %d "
                          "(same LINK, slot %d was not yet established)", Port, i);
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
            FlexNetSessions[i].flex_est_inferred = FALSE;
            FlexNetSessions[i].keepalive_count = 0;
            FlexNetSessions[i].session_start = time(NULL);
            FlexNetSessions[i].last_keepalive = time(NULL);
            LINK->FlexNetLink = TRUE;
            memcpy(FlexNetSessions[i].peer_callsign, LINK->LINKCALL, 7);

            int node_ssid = (MYCALL[6] >> 1) & 0x0F;
            int init_max  = (g_flexnet_ssid_hi >= 0) ? g_flexnet_ssid_hi : node_ssid;
            unsigned char init[8];
            int ilen = flex_build_init(init, sizeof(init), init_max);
            if (ilen > 0)
                flex_send_frame(LINK, FLEXNET_PID_CE, init, ilen);

            unsigned char ka[FLEXNET_KEEPALIVE_LEN];
            int klen = flex_build_keepalive(ka, sizeof(ka), &FlexNetSessions[i]);
            if (klen > 0)
                flex_send_frame(LINK, FLEXNET_PID_CE, ka, klen);

            FlexNet_Info("FlexNet: session reconnected on port %d "
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
        FlexNet_Info("FlexNet: no free session slot for port %d", Port);
        return;
    }

    memset(sess, 0, sizeof(*sess));
    /* Clear the parallel v2.2 tables for this slot too. The session
       reaper can free a slot without going through
       FlexNet_CloseSession, and a new peer inheriting the previous
       occupant's advertised[] would be told nothing — we'd believe it
       already knew routes it has never heard. */
    {
        int fresh_idx = (int)(sess - FlexNetSessions);
        char new_call[20] = {0};
        ConvFromAX25((unsigned char *)LINK->LINKCALL,
                     (unsigned char *)new_call);
        { int sl = (int)strlen(new_call);
          while (sl > 0 && new_call[sl-1] == ' ') new_call[--sl] = '\0'; }

        /* advertised[] is always cleared — see flex_learned_adopt() for
           why that asymmetry is deliberate. */
        memset(&FlexNetAdvertised[fresh_idx], 0, sizeof(FlexNetAdvertised[0]));

        if (!flex_learned_adopt(fresh_idx, new_call))
            memset(&FlexNetLearned[fresh_idx], 0, sizeof(FlexNetLearned[0]));

        strncpy(FlexNetLearned[fresh_idx].peer_call, new_call,
                sizeof(FlexNetLearned[0].peer_call) - 1);
        FlexNetLearned[fresh_idx].peer_call[
            sizeof(FlexNetLearned[0].peer_call) - 1] = '\0';
        FlexNetLearned[fresh_idx].died_at = 0;
    }
    sess->LINK = LINK;
    sess->port = Port;
    sess->active = TRUE;
    sess->our_link_time = 2;  /* 200ms — typical AXUDP */
    sess->session_start = time(NULL);
    memcpy(sess->peer_callsign, LINK->LINKCALL, 7);

    if (FlexNetSessionCount < FLEXNET_MAX_SESSIONS)
        FlexNetSessionCount++;

    LINK->FlexNetLink = TRUE;

    /* Send CE init handshake. The init byte declares the highest
       SSID this node is willing to host on its callsign — peers use
       this to clamp incoming route adverts. When FLEXNETSSIDRANGE
       is configured we declare ssid_hi so the SSID range we
       subsequently advertise isn't truncated to the node SSID. */
    int node_ssid = (MYCALL[6] >> 1) & 0x0F;
    int init_max  = (g_flexnet_ssid_hi >= 0) ? g_flexnet_ssid_hi : node_ssid;
    unsigned char init[8];
    int ilen = flex_build_init(init, sizeof(init), init_max);
    if (ilen > 0)
        flex_send_frame(LINK, FLEXNET_PID_CE, init, ilen);

    /* Send keepalive to kick-start the exchange. sess is fresh — no
       peer KA observed yet, so flex_build_keepalive falls back to the
       (X)Net 241-B default. The first KA we get back from the peer
       reshapes us. */
    unsigned char ka[FLEXNET_KEEPALIVE_LEN];
    int klen = flex_build_keepalive(ka, sizeof(ka), sess);
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

        /* §5.5 — flag the neighbour's own entry as a direct neighbour.
           Must happen after the merge created it in learned[] and
           before any CE compact record can arrive, because the flag is
           what the 120 s keepalive walk selects on and nothing ever
           clears it. */
        if (g_flexnet_transit_enabled)
        {
            int nidx = (int)(sess - FlexNetSessions);
            struct FLEXNET_LEARNED_STATE * nst = &FlexNetLearned[nidx];
            for (int ri = 0; ri < nst->count; ri++)
            {
                if (nst->routes[ri].ssid_lo == nbr_ssid &&
                    nst->routes[ri].ssid_hi == nbr_ssid &&
                    strncmp(nst->routes[ri].dest_call, nbr_base,
                            FLEXNET_MAX_CALLSIGN) == 0)
                {
                    nst->routes[ri].is_direct_neighbour = TRUE;
                    break;
                }
            }
        }

        FlexNet_Info("FlexNet: added neighbor %s (%d-%d) RTT=1 "
                      "as direct destination", nbr_base, nbr_ssid, nbr_ssid);
    }

    FlexNet_Info("FlexNet: session started on port %d with %s "
                  "(sent init max_ssid=%d + keepalive)", Port, snbr, init_max);
}

void FlexNet_CloseSession(LINKTABLE * LINK)
{
    struct FLEXNET_SESSION * sess = flex_find_session(LINK);
    if (!sess) return;

    int dead_idx = (int)(sess - FlexNetSessions);

    /* Invalidate only what we reached THROUGH THIS SESSION.
     *
     * This used to match on `port`, and every FlexNet peer of this node
     * lives on the same AXIP port — so one peer's DISC marked every
     * destination from every peer unreachable. With 3 peers and ~198
     * destinations, a single -12 link cycle withdrew -14's entire table
     * and then re-advertised it on recovery. 63 inbound DISC frames
     * landed in the 12 hours of 2026-09-17/18, against 16897
     * advertisements fired and a PCF queue that was non-empty in 4165
     * of 4165 bucket ticks. See
     * research/path_query_2026-09-18/LINK_INSTABILITY.md.
     *
     * via_session_idx is the right key and the ghost reaper already
     * used it; only this path did not. */
    int removed = 0;
    for (int i = 0; i < FlexNetDestCount; i++)
    {
        if (FlexNetDests[i].via_session_idx == dead_idx)
        {
            FlexNetDests[i].rtt = FLEXNET_RTT_INFINITY;
            FlexNetDests[i].is_infinity = 1;
            FlexNetDests[i].via_session_idx = -1;
            removed++;
        }
    }

    /* Stamp the learned table so a quick reconnect can adopt it and a
       long absence cannot. */
    FlexNetLearned[dead_idx].died_at = time(NULL);

    /* Deactivate BEFORE the poison walk: flex_expected_rtt only counts
       active sessions as sources, so this is what makes the walk below
       see the network as it is now rather than as it was. */
    sess->active = FALSE;
    sess->LINK = NULL;
    LINK->FlexNetLink = FALSE;

    /* §5.7 poison-reverse — see flex_advertise_poison_session(). */
    {
        char dead_call[20] = {0};
        ConvFromAX25((unsigned char *)LINK->LINKCALL,
                     (unsigned char *)dead_call);
        { int dl = (int)strlen(dead_call);
          while (dl > 0 && dead_call[dl-1] == ' ') dead_call[--dl] = '\0'; }
        flex_advertise_poison_session(dead_idx, dead_call);
    }

    char cnbr[20] = {0};
    ConvFromAX25(LINK->LINKCALL, cnbr);
    { int sl = strlen(cnbr); while (sl > 0 && cnbr[sl-1] == ' ') cnbr[--sl] = '\0'; }
    FlexNet_Info("FlexNet: session with %s closed, "
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
        if (FLEXNET_DEBUG) FlexNet_Info("FlexNet CE: new session auto-created for %s", nbr);
        FlexNet_InitSession(LINK, LINK->LINKPORT->PORTNUMBER);
        sess = flex_find_session(LINK);
        if (!sess) return;
    }

    int frame_type = flex_parse_ce_frame(data, len);

    /* Log every CE frame with type name */
    static const char * ce_names[] = {
        "???", "KEEPALIVE", "STATUS+", "STATUS-", "STATUS_10",
        "COMPACT", "LINK_TIME", "TOKEN", "PATH_REQ", "INIT",
        "PATH_REP", "STATUS_1x"
    };
    const char * tname = (frame_type >= 1 && frame_type <= 11)
                         ? ce_names[frame_type] : "UNKNOWN";
    if (FLEXNET_DEBUG) FlexNet_Info("FlexNet CE: %s from %s (len=%d)", tname, nbr, len);

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
        if (FLEXNET_DEBUG) FlexNet_Info("FlexNet: init from %s max_ssid=%d, "
                    "replying max_ssid=15", nbr, upper_ssid);

        /* v2.1.11: infer peer KA flavor from INIT length and re-send a
           shape-correct KA when the inferred flavor is PC/Flexnet.
           Background: flex_session_start fires our INIT + KA together,
           before any peer-originated frame arrives — at that moment
           peer_ka_term is 0 so flex_build_keepalive falls back to the
           (X)Net shape (241 B no CR). PCF silently discards that, can
           never derive a valid link-time sample, and its smoothed cost
           stays saturated at 4095 (observed on the IR2UFV ↔ IW2OHX-12
           link, 2026-05-27 wire study).
           PCF's INIT is 6 bytes (`0<ssid>  !\r`); ours and (X)Net's are
           5 bytes (`0<ssid>%!\r`). Use that to seed peer_ka_len /
           peer_ka_term now; once the real KA arrives, line ~1112's
           store wins. The corrective KA below covers the gap. */
        if (sess->peer_ka_len == 0)
        {
            if (len >= 6)
            {
                sess->peer_ka_len  = 201;
                sess->peer_ka_term = '\r';

                /* PCF flavour — re-send KA in PCF shape now that we
                   know the peer can't accept the (X)Net-shape one our
                   flex_session_start emitted. */
                unsigned char ka[FLEXNET_KEEPALIVE_LEN];
                int klen = flex_build_keepalive(ka, sizeof(ka), sess);
                if (klen > 0)
                    flex_send_frame(LINK, FLEXNET_PID_CE, ka, klen);
            }
            else
            {
                sess->peer_ka_len  = FLEXNET_KEEPALIVE_LEN;  /* 241 */
                sess->peer_ka_term = ' ';
            }
        }

        /* v2.1.11: emit our routes once peer has INIT'd, not only on
           first peer KA. PC/Flexnet peers don't reliably send their own
           KAs after the SABM/UA handshake — observed on IW2OHX-12 where
           the KA-gated trigger at case CE_FRAME_KEEPALIVE never fired,
           leaving IR2UFV's routes silent and PCF reaching us only via
           transit (link cost saturated to 4095 on the direct port).
           INIT receipt is sufficient evidence the FlexNet layer is up;
           the records-per-emit cap above keeps the burst safe. */
        if (!sess->sent_routes)
        {
            flex_send_own_routes(LINK, FALSE);
            sess->sent_routes = TRUE;
            flex_advertise_seed_peer((int)(sess - FlexNetSessions));
        }
        break;
    }

    case CE_FRAME_KEEPALIVE:
    {
        sess->keepalive_count++;
        flex_note_peer_established(sess);  /* v2.1.39 */
        if (FLEXNET_DEBUG) FlexNet_Info("FlexNet: keepalive #%d from %s, "
                    "echoing + sending LT=%d",
                    sess->keepalive_count, nbr, sess->our_link_time);

        /* Capture peer's KA shape so the echo below mirrors it. PC/Flexnet
           sends 201 B with a CR terminator; (X)Net sends 241 B with a
           trailing space. */
        if (len >= 2 && len <= FLEXNET_KEEPALIVE_LEN)
        {
            sess->peer_ka_len  = len;
            sess->peer_ka_term = data[len - 1];
        }

        /* v2.1.13 — echo KA + send LT for ALL peer flavors. flexnetd's
           own poll_cycle.c:744-782 (the source-of-truth flexnetd that
           runs on IW2OHX-4 and gets correctly probed by PCF) does
           exactly this: echo `send_ce_keepalive(fd)` unconditionally,
           then conditionally send a rate-limited LT. v2.1.12's
           "PCF → `10\r` only" branch was based on misreading -14
           (xnet) which uses a different reply style; flexnetd's
           KA-echo pattern is what PCF's state machine actually
           classifies as "probable". The LT rate-limit gate now lives
           inside flex_send_link_time itself (see that function's
           v2.1.13 comment block for the PCF link.ts math). */
        unsigned char ka[FLEXNET_KEEPALIVE_LEN];
        int klen = flex_build_keepalive(ka, sizeof(ka), sess);
        if (klen > 0)
            flex_send_frame(LINK, FLEXNET_PID_CE, ka, klen);

        /* Send link time on every keepalive cycle (rate-limited
           inside flex_send_link_time per peer flavor). */
        flex_send_link_time(LINK, sess);

        /* Advertise our routes after first keepalive + init */
        if (!sess->sent_routes && flex_is_established(sess))
        {
            if (FLEXNET_DEBUG) FlexNet_Info("FlexNet: first keepalive after init — "
                        "sending our routes");
            flex_send_own_routes(LINK, FALSE);
            sess->sent_routes = TRUE;
            flex_advertise_seed_peer((int)(sess - FlexNetSessions));
        }

        sess->last_keepalive = time(NULL);
        break;
    }

    case CE_FRAME_LINK_TIME:
    {
        flex_note_peer_established(sess);  /* v2.1.39 */

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
            if (FLEXNET_DEBUG) FlexNet_Info("FlexNet: link time from %s: %ldms "
                        "(ours: %dms)", nbr,
                        sess->peer_link_time, sess->our_link_time);
        }

        /* Reply with our link time (stamps lt_tx_tick). */
        flex_send_link_time(LINK, sess);
        break;
    }

    case CE_FRAME_TOKEN:
    {
        if (FLEXNET_DEBUG) FlexNet_Info("FlexNet: token exchange with %s", nbr);
        /* Echo token back */
        flex_send_frame(LINK, FLEXNET_PID_CE, data, len);
        break;
    }

    case CE_FRAME_STATUS_POS:
    {
        /* '3+' = REQUEST token from peer: "give me your current view"
           (skill §1.6). Our own record goes out at once, then §5.6
           walks everything learned from the OTHER sessions through the
           decision rule and the token bucket meters it onto the wire.
           This is deliberately NOT a force-send-everything: jitter
           suppression still applies, so a peer whose view is already
           current gets just the trailing '3-'. That token is deferred
           until the queue drains, which is why the '3-' is not emitted
           inline here. */
        if (FLEXNET_DEBUG) FlexNet_Info("FlexNet: route request (3+) from %s", nbr);
        flex_note_peer_established(sess);  /* v2.1.39 */
        {
            int req_idx = (int)(sess - FlexNetSessions);
            BOOL walk = (g_flexnet_transit_enabled &&
                         req_idx >= 0 && req_idx < FLEXNET_MAX_SESSIONS);
            flex_send_own_routes(LINK, walk);
            if (walk)
            {
                int walked = 0, queued = 0;
                flex_advertise_walk_for_peer(req_idx, FALSE, FALSE,
                                             &walked, &queued);
                if (FLEXNET_DEBUG)
                    FlexNet_Trace("FlexNet: 3PLUS-WALK from=%s entries=%d "
                                 "queued=%d", nbr, walked, queued);
                FlexNetAdvertised[req_idx].eob_pending = TRUE;
                flex_advertise_drain(req_idx);
            }
        }
        sess->sent_routes = TRUE;
        break;
    }

    case CE_FRAME_STATUS_NEG:
        /* '3-' = release token — end of batch */
        if (FLEXNET_DEBUG) FlexNet_Info("FlexNet: route release (3-) from %s", nbr);
        break;

    case CE_FRAME_STATUS_10:
        if (FLEXNET_DEBUG) FlexNet_Info("FlexNet: status 10 from %s", nbr);
        break;

    case CE_FRAME_STATUS_1N:
        /* "1n\r" status sibling (n=1..9). Semantic undocumented;
           treated as benign. Log the digit so we can inventory which
           variants peers actually send without filling the log with
           the verbose CE-UNKNOWN hex+ASCII dump. */
        FlexNet_Log("CE-STATUS-1n: from=%s digit=%c", nbr, data[1]);
        if (FLEXNET_DEBUG) FlexNet_Info("FlexNet: status 1%c from %s",
                    data[1], nbr);
        break;

    case CE_FRAME_COMPACT:
    {
        flex_note_peer_established(sess);  /* v2.1.39 */

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
            if (FLEXNET_DEBUG) FlexNet_Info("FlexNet:   route: %s (%d-%d) RTT=%d [%s]",
                        entries[i].callsign,
                        entries[i].ssid_lo, entries[i].ssid_hi,
                        entries[i].rtt, tag);
        }
        if (FLEXNET_DEBUG) FlexNet_Info("FlexNet: compact batch — %d entries "
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
    {
        /* v1.9.5+ investigation: dump unknown CE frames into the log
         * file (not just console) so we can identify what peers send
         * that our parser doesn't classify. Print up to 32 bytes of
         * hex + ASCII preview of the payload (after the PID byte). */
        char hex[3*32 + 1]   = {0};
        char ascii[32 + 1]   = {0};
        int dump_n = len > 32 ? 32 : len;
        for (int i = 0; i < dump_n; i++)
        {
            snprintf(&hex[i*3], 4, "%02X ", data[i]);
            ascii[i] = (data[i] >= 0x20 && data[i] < 0x7F)
                       ? (char)data[i] : '.';
        }
        FlexNet_Log("CE-UNKNOWN: from=%s byte0=0x%02X len=%d  hex=[%s] ascii=[%s]",
                    nbr, data[0], len, hex, ascii);
        if (FLEXNET_DEBUG) FlexNet_Info("FlexNet CE: unknown frame type from %s "
                    "(byte0=0x%02X, len=%d)", nbr, data[0], len);
        break;
    }
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
    if (FLEXNET_DEBUG) FlexNet_Info("FlexNet CF: from %s len=%d [%s]", nbr, len, preview);

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
            if (FLEXNET_DEBUG) FlexNet_Info("FlexNet: L3RTT reply matched our probe");
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
                        FlexNet_Info("FlexNet: L3RTT reply -> %s "
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
                        FlexNet_Info("FlexNet: L3RTT build failed — dropping");
                    FlexNet_Log("L3RTT-DROP: build failed (plen=%d flen=%d wrap=%s) -> %s",
                                plen, flen, have_l3 ? "L3-INFO" : "bare", nbr);
                }
            }
            else
            {
                if (FLEXNET_DEBUG)
                    FlexNet_Info("FlexNet: L3RTT parse failed from %s (len=%d) — dropping",
                                  nbr, len);
                FlexNet_Log("L3RTT-DROP: parse failed from %s (len=%d)", nbr, len);
            }
        }
        /* L3RTT path consumed the frame either way. */
        ReleaseBuffer(Buffer);
        return 1;
    }

    /* v2.2 RFC §6 — TRANSIT FORWARDING.
       The frame is NOT L3RTT but might be a NetROM L4 envelope
       (CREQ/CACK/IACK/INFO/DREQ/DACK) addressed to a callsign we
       know via a FlexNet downstream neighbour. If so, forward at L2
       to that neighbour with TTL-1, rewriting only the L2 src/dst
       (the L4 envelope stays verbatim per skill §3 and Phase 3
       observations). If the destination is local (MYCALL) or we
       don't know a transit route, return 0 — the existing NetROM
       L3/L4 dispatcher handles local delivery and same-stack
       routing as before. */
    if (g_flexnet_transit_enabled && len >= 20)
    {
        const unsigned char * l3_dst = &data[7];   /* skill §2.3 */

        /* Local-callsign check: compare base 6 bytes of L3DST with
           MYCALL base. If equal, this frame is FOR US — let normal
           NetROM L3/L4 dispatch handle it. (We don't iterate the
           APPLICATION call table here — those are LOCAL bound and
           would also pass through the existing dispatcher; the
           transit hook only fires when the destination is provably
           not us.) */
        int dst_is_local = (memcmp(l3_dst, MYCALL, 6) == 0);

        if (!dst_is_local)
        {
            /* Decode l3_dst to an ASCII call so we can look it up in
               our FlexNet D-table (which is keyed on ASCII strings). */
            char dst_str[20] = {0};
            ConvFromAX25((char *)l3_dst, dst_str);
            { int sl = (int)strlen(dst_str);
              while (sl > 0 && dst_str[sl-1] == ' ')
                  dst_str[--sl] = '\0'; }

            /* Identify the arrival session so split-horizon excludes
               sending the frame back the way it came. */
            int arrival_sess_idx = -1;
            for (int si = 0; si < FLEXNET_MAX_SESSIONS; si++)
            {
                if (FlexNetSessions[si].active &&
                    FlexNetSessions[si].LINK == LINK)
                {
                    arrival_sess_idx = si;
                    break;
                }
            }

            /* Search FlexNetDests for a matching callsign whose
               via_session_idx is a DIFFERENT session from where the
               frame arrived. Match on base callsign + SSID-in-range. */
            int via_sess_idx = -1;
            for (int di = 0; di < FlexNetDestCount; di++)
            {
                struct FLEXNET_DEST_ENTRY * d = &FlexNetDests[di];
                if (d->rtt >= FLEXNET_RTT_INFINITY) continue;
                if (d->via_session_idx < 0) continue;
                if (d->via_session_idx == arrival_sess_idx) continue;
                if (strcasecmp(d->callsign, dst_str) != 0)
                {
                    /* Try matching against base callsign (no SSID
                       in dst_str if it came in plain). */
                    char base[20]; strncpy(base, dst_str, sizeof(base)-1);
                    char * dash = strchr(base, '-');
                    int dst_ssid = -1;
                    if (dash) { dst_ssid = atoi(dash+1); *dash = '\0'; }
                    if (strcasecmp(d->callsign, base) != 0) continue;
                    if (dst_ssid >= 0 &&
                        (dst_ssid < d->ssid_lo || dst_ssid > d->ssid_hi))
                        continue;
                }
                via_sess_idx = d->via_session_idx;
                break;
            }

            if (via_sess_idx >= 0 &&
                FlexNetSessions[via_sess_idx].active &&
                FlexNetSessions[via_sess_idx].LINK)
            {
                /* TTL decrement before forward. If TTL was already 0
                   or 1, drop the frame (loop-prevention). */
                unsigned char * ttl = (unsigned char *)&data[14];
                if (*ttl <= 1)
                {
                    FlexNet_Log("CF-TRANSIT-TTL-EXPIRED: from=%s "
                                "dst=%s ttl=%d — dropping",
                                nbr, dst_str, *ttl);
                    ReleaseBuffer(Buffer);
                    return 1;
                }
                (*ttl)--;

                LINKTABLE * out_link = FlexNetSessions[via_sess_idx].LINK;
                char out_nbr[20] = {0};
                ConvFromAX25((char *)out_link->LINKCALL, out_nbr);
                { int sl = (int)strlen(out_nbr);
                  while (sl > 0 && out_nbr[sl-1] == ' ')
                      out_nbr[--sl] = '\0'; }
                FlexNet_Info("FlexNet: transit-forwarding L3 dst=%s "
                              "from %s -> %s ttl=%d",
                              dst_str, nbr, out_nbr, *ttl);
                FlexNet_Log("CF-TRANSIT-FWD: l3_dst=%s in=%s out=%s "
                            "ttl=%d opcode=0x%02X len=%d",
                            dst_str, nbr, out_nbr, *ttl,
                            len >= 20 ? data[19] : 0, len);
                flex_send_frame(out_link, FLEXNET_PID_CF, data, len);
                ReleaseBuffer(Buffer);
                return 1;
            }
        }
    }

    /* Not L3RTT, not transit — most likely a NetROM L4 frame (CACK,
     * INFO with banner data, IACK, DREQ, DACK) for a session that
     * ends locally. Return 0 without releasing the buffer; the caller
     * (L2 dispatcher) falls through to the normal CF processing path
     * which delivers the frame to the NetROM L4 layer. */
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
       ghost row with empty callsign).

       v2.1.14 — REAP HYSTERESIS. A single observation of L2STATE!=5
       used to fire the reap on the spot; field evidence on the
       IR2UFV ↔ IW2OHX-12 link (2026-05-28) showed this triggers
       false-positively during routine AX.25 internal state
       transitions (N(S) wrap, mod-128 negotiation, retry windows)
       — the wire shows continuous I-frame exchange but our internal
       session slot still gets reaped, then auto-recreated on the
       next inbound CE frame, which re-INITs the peer and reseeds
       PCFlexnet's link-cost ring with `600 2998 …` outliers every
       ~90 min. Now we require FLEXNET_REAP_STRIKES consecutive
       observations of the bad condition before destroying the slot.
       LINK==NULL still fires immediately — that's not a transient. */
    #define FLEXNET_REAP_STRIKES  3
    for (int i = 0; i < FLEXNET_MAX_SESSIONS; i++)
    {
        struct FLEXNET_SESSION * sess = &FlexNetSessions[i];
        if (!sess->active) continue;
        int bad = 0;
        int hard = 0;   /* hard = reap immediately (no hysteresis) */
        if (!sess->LINK) { bad = 1; hard = 1; }
        else if (sess->LINK->L2STATE != 5) bad = 1;
        else if (sess->LINK->LINKCALL[0] == 0) bad = 1;

        if (!bad)
        {
            sess->reap_strikes = 0;
            continue;
        }

        /* v2.1.16 — LINK-migration second chance. If the only thing wrong
           is that BPQ has zeroed our LINKTABLE slot under us (CLEAROUTLINK
           on idle-timer, etc.) but the peer is still up and BPQ has
           allocated a NEW LINKTABLE slot for the same callsign on the
           same port, migrate sess->LINK to that new pointer instead of
           reaping. This keeps the session established (got_peer_init
           stays TRUE, sent_routes stays TRUE) and the peer never sees a
           re-INIT — so its link-cost ring isn't reseeded with `600 …`
           outliers. We need the stashed peer_callsign because sess->LINK
           may already be pointing at a memset'd slot whose LINKCALL[0]
           is zero. */
        if (sess->peer_callsign[0] != 0)
        {
            for (int li = 0; li < MAXLINKS; li++)
            {
                LINKTABLE * NL = &LINKS[li];
                if (NL == sess->LINK) continue;
                if (NL->L2STATE != 5) continue;
                if (NL->LINKCALL[0] == 0) continue;
                if (!NL->LINKPORT) continue;
                if (NL->LINKPORT->PORTNUMBER != sess->port) continue;
                /* Match by full 7-byte AX.25 LINKCALL (incl. SSID). */
                /* Compare via normalized human form to bypass AX.25
                   byte-layout differences between BPQ code paths. */
                {
                    char a[12], b[12];
                    flex_normalize_callsign(NL->LINKCALL, a, sizeof(a));
                    flex_normalize_callsign(sess->peer_callsign, b, sizeof(b));
                    if (a[0] == 0 || strcmp(a, b) != 0) continue;
                }
                /* Migrate. */
                if (sess->LINK) sess->LINK->FlexNetLink = FALSE;
                sess->LINK = NL;
                NL->FlexNetLink = TRUE;
                sess->reap_strikes = 0;
                if (FLEXNET_DEBUG)
                {
                    char mnbr[20] = {0};
                    ConvFromAX25(NL->LINKCALL, mnbr);
                    { int sl = strlen(mnbr);
                      while (sl > 0 && mnbr[sl-1] == ' ') mnbr[--sl] = '\0'; }
                    FlexNet_Info("FlexNet: migrated session slot %d to "
                                  "new LINK for %s (BPQ recycled the old "
                                  "LINKTABLE slot)", i, mnbr);
                }
                goto next_session;
            }
        }

        if (!hard)
        {
            sess->reap_strikes++;
            if (sess->reap_strikes < FLEXNET_REAP_STRIKES)
            {
                if (FLEXNET_DEBUG)
                    FlexNet_Info("FlexNet: session slot %d bad-state "
                                  "strike %d/%d (L2STATE=%d LINKCALL[0]=%d)",
                                  i, sess->reap_strikes,
                                  FLEXNET_REAP_STRIKES,
                                  sess->LINK ? sess->LINK->L2STATE : -1,
                                  sess->LINK ? sess->LINK->LINKCALL[0] : -1);
                continue;
            }
        }
        if (FLEXNET_DEBUG)
            FlexNet_Info("FlexNet: reaping ghost session slot %d "
                          "(LINK=%p, strikes=%d, hard=%d)",
                          i, (void *)sess->LINK, sess->reap_strikes, hard);
        if (sess->LINK) sess->LINK->FlexNetLink = FALSE;
        /* Demote dest entries that pointed at this slot so
           cost-based attribution can re-elect a live session. */
        for (int d = 0; d < FlexNetDestCount; d++)
            if (FlexNetDests[d].via_session_idx == i)
                FlexNetDests[d].via_session_idx = -1;

        /* Stamp the learned table: the reaper is the path peers usually
           die on, so without this a reaped peer could never be adopted
           back and every ghost reap would rebuild its table. */
        FlexNetLearned[i].died_at = time(NULL);

        /* §5.7 — withdraw what we learned here. This is the path a
           peer actually dies on: FlexNet_CloseSession needs an
           explicit DISC and is rarely reached, so without this hook
           poison-reverse never fires at all. Deactivate first, so
           flex_expected_rtt stops counting this session as a source
           and the alternate-path check is honest; the stashed
           peer_callsign is used for the log because sess->LINK may
           already point at a slot BPQ has zeroed. */
        {
            char reap_call[20] = {0};
            if (sess->peer_callsign[0])
                flex_normalize_callsign(sess->peer_callsign,
                                        reap_call, sizeof(reap_call));
            sess->active = FALSE;
            flex_advertise_poison_session(i, reap_call);
        }

        memset(sess, 0, sizeof(*sess));
        next_session: ;
    }

    /* v2.x #3 — proactive CE init. Scan connected L2 links for any
       FlexNet-mapped peer that doesn't have a FlexNet session yet
       and bootstrap one. Without this, two FlexNet peers can both
       wait for the other to send the first CE frame and never
       converge. Throttle to once per FLEXNET_PROACTIVE_INIT_INTERVAL
       to bound the scan cost.

       v2.1.7 — only auto-init the dedicated peer-to-peer AXIP-tunnel
       LINK, not user pass-through sessions that happen to terminate
       at the FlexNet peer call. A user session "IW7CFD → IW2OHX-12"
       (e.g. a telnet user issuing C IW2OHX-12) reaches L2STATE=5
       with LINKCALL=IW2OHX-12 — the same condition the original
       scan matched on — but its OURCALL is the user's call, not our
       node call. Auto-initing it pushed CE INIT+KA into the user
       session, which PC/Flexnet then refused to deliver replies on.
       Two filters express the "real peer session" shape:
         (a) OURCALL base (6 bytes, SSID ignored) == node MYCALL base;
         (b) no L2 digi chain (DIGIS[0] == 0).
       Both hold for direct FlexNet AXIP peer sessions and neither
       holds for user pass-through sessions. */
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
            /* Only consider links whose local-side call is our node
               call — peer-to-peer FlexNet, not user pass-through. */
            if (memcmp(L->OURCALL, MYCALL, 6) != 0) continue;
            /* And only direct (no L2 digi) sessions — the AXIP
               peer tunnel is always digi-less. */
            if (L->DIGIS[0] != 0) continue;
            if (FlexNet_IsPeerFlexNetMapped(L->LINKCALL,
                                            L->LINKPORT->PORTNUMBER))
            {
                char pcall[20] = {0};
                ConvFromAX25(L->LINKCALL, pcall);
                { int sl = (int)strlen(pcall);
                  while (sl > 0 && pcall[sl-1] == ' ') pcall[--sl] = '\0'; }
                FlexNet_Info("FlexNet: proactive CE init to %s on port %d",
                              pcall, L->LINKPORT->PORTNUMBER);
                FlexNet_InitSession(L, L->LINKPORT->PORTNUMBER);
            }
        }
    }

    for (int i = 0; i < FLEXNET_MAX_SESSIONS; i++)
    {
        struct FLEXNET_SESSION * sess = &FlexNetSessions[i];
        if (!sess->active || !sess->LINK) continue;

        /* v2.1.24 — per-peer-type proactive keepalive cadence.

           Wire-trace evidence on iw2ohx-bpq (PCF host), 2026-05-29:
           captured all UDP traffic to/from PC/Flexnet (PID 9840 =
           flexkrnl.exe) and observed PCF maintains very different
           per-peer cadence. IW2OHX-4 on UDP port 95 exchanges
           227-byte CE KAs with PCF every 16-32 s and sends compact
           route bursts every ~21 s — heavy continuous activity.
           IR2UFV on UDP port 10075 had ~5 min CE KA cadence and
           pure RR-only stretches up to 80 s. PCF cycles IR2UFV's
           L2 link every ~2-2.5 h while keeping IW2OHX-4 connected
           for 16+ h.

           Hypothesis: PCF tracks per-link activity and AXIP-cycles
           peers that go too quiet. Sending KAs at the same cadence
           as xnet peers (~20-30 s vs flexnetd-default 300 s) should
           keep PCF satisfied without affecting xnet links (xnet's
           189 s native KA still preempts our proactive timer on
           xnet sessions because they're more frequent than the
           threshold for that peer flavour).

           Per-peer threshold:
             PCF  (peer_ka_term == '\r') → 30 s
             xnet (peer_ka_term == ' ')  → 300 s
             unknown (peer_ka_term == 0) → 300 s (safe default until
                                            first peer KA shapes us) */
        /* v2.1.28 — PCF threshold 30 → 29 s to land the KA tx ~1 tick
           past PCF's link.ts boundary (= (FLEXNET_WIRE_LT + 4) * 32
           ticks = 288 ticks = 28.8 s for LT=5), which produces
           sample = ~2 ticks in PCF's L * row instead of the
           ~12 ticks we'd get with cadence=30. (X)Net stays at 300. */
        int ka_threshold = 300;
        if (sess->peer_ka_term == '\r')
            ka_threshold = 29;
        if (now - sess->last_keepalive >= ka_threshold)
        {
            char tnbr[20] = {0};
            ConvFromAX25(sess->LINK->LINKCALL, tnbr);
            { int sl = strlen(tnbr); while (sl > 0 && tnbr[sl-1] == ' ') tnbr[--sl] = '\0'; }
            if (FLEXNET_DEBUG) FlexNet_Info("FlexNet: sending keepalive + LT=%d to %s",
                        sess->our_link_time, tnbr);

            unsigned char ka[FLEXNET_KEEPALIVE_LEN];
            int klen = flex_build_keepalive(ka, sizeof(ka), sess);
            if (klen > 0)
                flex_send_frame(sess->LINK, FLEXNET_PID_CE, ka, klen);

            /* Proactive LT (stamps lt_tx_tick). */
            flex_send_link_time(sess->LINK, sess);

            sess->last_keepalive = now;
        }

        /* §5.5 — the 120 s tick is no longer a table sweep. It
           re-emits our own record (the peer's route to US, which xnet
           ages out like any other) and force-refreshes the direct-
           neighbour set. Learned destinations beyond that set are not
           touched here: they go out as change events, which is the
           whole point of rc4.

           The gate on g_flexnet_transit_enabled is load-bearing and
           predates rc4. v2.1.32 (2026-06-01) measured that on the
           IR2UFV ↔ IW2OHX-12 link, even a transit-OFF node emitting
           just its own record + `3-` every 120 s made PC/Flexnet
           cycle the L2 session every 1-10 minutes; the flexnetd
           M6.9.4 wire study had already recorded that PCF DMs the
           link within 10-15 ms of processing a compact record on an
           otherwise quiet link. So a leaf stays silent after its
           initial advertisement and sits at the stable v2.1.28
           baseline, and only a node that has opted into transit pays
           PCF's periodic L2 cycle. Do not widen this gate. */
        if (g_flexnet_transit_enabled &&
            sess->sent_routes &&
            (now - FlexNetLearned[i].last_advert) >= FLEXNET_ADVERT_INTERVAL)
        {
            FlexNetLearned[i].last_advert = now;
            flex_send_own_routes(sess->LINK, FALSE);
            flex_advertise_neighbours(i);
        }

        /* §5.4 — meter whatever the change triggers queued onto the
           wire at this peer's family rate. Per-second resolution is
           enough: the buckets refill in whole seconds. */
        flex_advertise_drain(i);
    }

    /* Prune learned[] entries no peer has refreshed. Throttled: the
       walk is over every session's table and FlexNet_Timer ticks
       several times a second. */
    if ((now - g_last_learned_age_scan) >= FLEXNET_LEARNED_AGE_SCAN)
    {
        g_last_learned_age_scan = now;
        flex_learned_age_scan(now);
    }

    /* Expire timed-out L3RTT probes */
    for (int p = 0; p < FLEXNET_MAX_PROBES; p++)
    {
        if (FlexNetProbes[p].active &&
            (now - FlexNetProbes[p].sent_time) > FLEXNET_PROBE_TIMEOUT)
        {
            if (FLEXNET_DEBUG) FlexNet_Info("FlexNet: L3RTT probe to %s timed out",
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
            if (FLEXNET_DEBUG) FlexNet_Info("FlexNet: PATH-REQ to %s qso=%d timed out",
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

    if (FLEXNET_DEBUG) FlexNet_Info("FlexNet: L3RTT probe sent for %s", target_full);
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

        if (FLEXNET_DEBUG) FlexNet_Info("FlexNet: L3RTT reply for %s, %d hops",
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

/* Is `target` one of our own direct FlexNet session peers? Only then is
 * "us, then the target" a truthful two-hop chain. Compared on the
 * normalised human form so SSIDs are honoured: IW2OHX-4 is a direct
 * neighbour, IW2OHX-3 is not, and they share a base call.
 */
static BOOL flex_target_is_direct_peer(const char * target)
{
    if (!target || !target[0]) return FALSE;

    for (int si = 0; si < FLEXNET_MAX_SESSIONS; si++)
    {
        if (!FlexNetSessions[si].active || !FlexNetSessions[si].LINK)
            continue;
        char peer[20] = {0};
        flex_sess_peer_call(&FlexNetSessions[si], peer, sizeof(peer));
        if (peer[0] && strcasecmp(peer, target) == 0) return TRUE;
    }
    return FALSE;
}

/* Resolve a PATH_REQ target ("CALL" or "CALL-n") to a FlexNetDests row
 * we can actually reach. Matches the SSID-in-range rule the transit
 * CREQ hook uses, so path answers and forwarding decisions agree.
 * Returns the index, or -1.
 */
static int flex_find_dest_for_target(const char * target)
{
    if (!target || !target[0]) return -1;

    char base[20] = {0};
    int  ssid = -1;
    strncpy(base, target, sizeof(base) - 1);
    char * dash = strchr(base, '-');
    if (dash) { ssid = atoi(dash + 1); *dash = '\0'; }

    for (int i = 0; i < FlexNetDestCount; i++)
    {
        struct FLEXNET_DEST_ENTRY * d = &FlexNetDests[i];
        if (d->rtt >= FLEXNET_RTT_INFINITY) continue;
        if (strcasecmp(d->callsign, base) != 0) continue;
        if (ssid >= 0 && (ssid < d->ssid_lo || ssid > d->ssid_hi)) continue;
        return i;
    }
    return -1;
}

/* ── On-demand path probing ─────────────────────────────────────────────
 *
 * The background probe walks the destination table round-robin, one per
 * FLEXNET_PATH_PROBE_INTERVAL (60s), so a cold node with ~190
 * destinations needs over three hours to cache every chain. Until a
 * chain is cached we answer nothing, and a peer with no answer cannot
 * render `D <dest>` — observed on IW2OHX-4 2026-09-17: `D DB0FAA`
 * printed `T=19` with no `route:` line while our background probe was
 * still at index 13 of 190. The connect itself worked, because
 * hop-by-hop L2 forwarding needs no full path. So this is a visibility
 * gap, not a routing one.
 *
 * When a peer asks about a destination we have not cached, probe it now
 * so the next query can be answered. Two rate limits, because a sysop
 * sweeping `D` across the table would otherwise turn one keystroke into
 * ~190 probes into the live mesh: at most one on-demand probe every
 * FLEXNET_ONDEMAND_PROBE_GAP seconds, and never the same target twice
 * within FLEXNET_ONDEMAND_PROBE_REPEAT.
 *
 * The recent-target table is deliberately separate from
 * FLEXNET_DEST_ENTRY: route updates memcpy whole entries from the
 * incoming record, which would silently reset a timestamp stored there
 * and defeat the per-target limit. */
#define FLEXNET_ONDEMAND_PROBE_GAP      3
#define FLEXNET_ONDEMAND_PROBE_REPEAT 120
#define FLEXNET_ONDEMAND_RECENT        32

static struct
{
    char   call[20];        /* normalised; ConvFromAX25 can exceed 10 chars */
    time_t probed;
} g_ondemand_recent[FLEXNET_ONDEMAND_RECENT];
static int    g_ondemand_next       = 0;
static time_t g_last_ondemand_probe = 0;

/* Probe `target` now if both rate limits allow. Returns TRUE if a probe
 * was actually sent. Never fails the caller: a declined probe just means
 * the answer arrives via the background walk instead. */
static BOOL flex_ondemand_probe(int dest_idx, const char * target)
{
    time_t now = time(NULL);

    if ((now - g_last_ondemand_probe) < FLEXNET_ONDEMAND_PROBE_GAP)
        return FALSE;

    for (int i = 0; i < FLEXNET_ONDEMAND_RECENT; i++)
    {
        if (!g_ondemand_recent[i].call[0]) continue;
        if (strcasecmp(g_ondemand_recent[i].call, target) != 0) continue;
        if ((now - g_ondemand_recent[i].probed) < FLEXNET_ONDEMAND_PROBE_REPEAT)
            return FALSE;
        break;
    }

    if (dest_idx < 0 || dest_idx >= FlexNetDestCount) return FALSE;

    struct FLEXNET_DEST_ENTRY * d = &FlexNetDests[dest_idx];
    if (flex_send_path_req(dest_idx, d->callsign, d->ssid_lo) != 0)
        return FALSE;       /* no free probe slot — background walk covers it */

    g_last_ondemand_probe = now;
    strncpy(g_ondemand_recent[g_ondemand_next].call, target,
            sizeof(g_ondemand_recent[0].call) - 1);
    g_ondemand_recent[g_ondemand_next].call[
        sizeof(g_ondemand_recent[0].call) - 1] = '\0';
    g_ondemand_recent[g_ondemand_next].probed = now;
    g_ondemand_next = (g_ondemand_next + 1) % FLEXNET_ONDEMAND_RECENT;

    FlexNet_Log("PATH-PROBE-DEMAND: target=%s (peer asked, cache cold)",
                target);
    return TRUE;
}

/* ── Deferred path answers ──────────────────────────────────────────────
 *
 * The on-demand probe fixed "the cache is cold for three hours", but not
 * the request that triggered it. Measured on IW2OHX-4, 2026-09-18:
 *
 *   11:01:32 PATH-PROBE-DEMAND: target=DB0DLG-6 (peer asked, cache cold)
 *   11:01:32 PATH-REQ-NOANSWER: ... [probing now]
 *   11:01:32 PATH-REP-RX: hops=9 target=DB0DLG (0s elapsed)
 *
 * The answer arrived in the same second — just after we had already
 * answered the peer with silence. So `D <dest>` showed no route on the
 * first try and would have worked on the second, while `C <dest>`
 * succeeded either way (hop-by-hop forwarding needs no path). That split
 * is exactly the inconsistency an operator sees.
 *
 * So park the request, and when the reply lands, replay it. Replaying
 * the ORIGINAL frame through flex_handle_path_req() rather than
 * factoring the answer out keeps one code path for both cases: a
 * deferred answer cannot drift from a direct one.
 */
#define FLEXNET_MAX_DEFERRED_REQ   8
#define FLEXNET_DEFERRED_REQ_TTL  20   /* s; probes reply in ~0s or not at all */
#define FLEXNET_DEFERRED_FRAME_MAX 96

struct FLEXNET_DEFERRED_REQ
{
    BOOL          in_use;
    int           peer_idx;
    char          target_base[20];      /* base call, no SSID */
    unsigned char frame[FLEXNET_DEFERRED_FRAME_MAX];
    int           frame_len;
    time_t        parked_at;
};
static struct FLEXNET_DEFERRED_REQ FlexNetDeferred[FLEXNET_MAX_DEFERRED_REQ];

/* Set while replaying, so a replay that still cannot answer parks
   nothing and cannot feed itself. */
static BOOL g_path_replaying = FALSE;

/* Strip "-n" so "DB0DLG-6" matches a probe's base "DB0DLG". */
static void flex_base_call(const char * call, char * out, size_t outsz)
{
    if (!out || outsz == 0) return;
    out[0] = '\0';
    if (!call) return;
    size_t n = 0;
    while (call[n] && call[n] != '-' && n + 1 < outsz) { out[n] = call[n]; n++; }
    out[n] = '\0';
}

static void flex_defer_path_req(int peer_idx, const char * target,
                                const unsigned char * data, int len)
{
    if (g_path_replaying) return;
    if (len <= 0 || len > FLEXNET_DEFERRED_FRAME_MAX) return;
    if (peer_idx < 0 || peer_idx >= FLEXNET_MAX_SESSIONS) return;

    time_t now = time(NULL);
    int slot = -1;
    for (int i = 0; i < FLEXNET_MAX_DEFERRED_REQ; i++)
    {
        if (!FlexNetDeferred[i].in_use ||
            (now - FlexNetDeferred[i].parked_at) > FLEXNET_DEFERRED_REQ_TTL)
        {
            slot = i;
            break;
        }
    }
    if (slot < 0) return;               /* all parked and fresh — drop it */

    struct FLEXNET_DEFERRED_REQ * d = &FlexNetDeferred[slot];
    d->in_use    = TRUE;
    d->peer_idx  = peer_idx;
    d->frame_len = len;
    memcpy(d->frame, data, (size_t)len);
    flex_base_call(target, d->target_base, sizeof(d->target_base));
    d->parked_at = now;

    FlexNet_Log("PATH-DEFER: parked %s for peer slot %d (slot %d) — will "
                "answer when the probe replies", d->target_base, peer_idx,
                slot);
}

/* ── CE type-6 traversal forwarding ─────────────────────────────────────
 *
 * A type-6 is not a question put to one node; it is a chain under
 * construction. Captured from PC/Flexnet IW2OHX-12 on 2026-09-18
 * (research/path_query_2026-09-18/TYPE6_IS_A_TRAVERSAL.md):
 *
 *   in   '6' 0x21 "    0" "IW2OHX-4 IW2OHX-12 IR3UGM"
 *   out  '6' 0x22 "    0" "IW2OHX-4 IW2OHX-12 IW2OHX-14 IR3UGM"
 *
 * The node that cannot finish the chain INSERTS its own next hop toward
 * the target immediately before the target, bumps the byte after the
 * type, and passes the type-6 on. The node finally adjacent to the
 * target replies type-7, which travels back down the chain.
 *
 * Why this matters here: answering from our own path cache is a
 * shortcut, and it fails in two ways that forwarding does not. A cached
 * chain longer than 8 digis cannot be answered at all (DB0LHR at 13,
 * DB0ACA-15 at 9), and our own probe times out about 11% of the time
 * (261 replies / 31 timeouts in one process). Forwarding removes both,
 * because then no single node has to know or express the whole path.
 *
 * On the header: we copy the inbound bytes and add 1 to the byte after
 * the type, which is precisely the transformation the capture shows.
 * That byte's exact meaning is NOT settled — `flex_build_path_rep()`
 * treats it as CE_PATH_HOP_BYTE_BASE + n_hops, and the replies we
 * receive do not all fit that reading. Reproducing the observed delta
 * needs no theory, so no theory is assumed. The 5-char QSO field is
 * passed through untouched; the originator uses it to match the reply.
 */
static BOOL flex_forward_path_req(int asker_idx, const char * target,
                                  int dest_idx,
                                  const char * origin,
                                  char hops[][FLEXNET_MAX_CALLSIGN],
                                  int n_hops,
                                  const unsigned char * data, int len)
{
    if (!g_flexnet_path_forward_enabled) return FALSE;

    const int hdr = 2 + CE_PATH_QSO_FIELD_LEN;
    if (len < hdr || n_hops < 1) { g_path_fwd_declined++; return FALSE; }

    /* Which neighbour do we reach the target through? */
    if (dest_idx < 0 || dest_idx >= FlexNetDestCount)
    {
        g_path_fwd_declined++;
        FlexNet_Log("PATH-FWD-DECLINE: target=%s not in our dest table",
                    target);
        return FALSE;
    }
    struct FLEXNET_DEST_ENTRY * d = &FlexNetDests[dest_idx];
    int via = d->via_session_idx;
    if (via < 0 || via >= FLEXNET_MAX_SESSIONS || !FlexNetSessions[via].active)
        via = flex_session_for_call(d->via_callsign);
    if (via < 0 || via >= FLEXNET_MAX_SESSIONS ||
        !FlexNetSessions[via].active || !FlexNetSessions[via].LINK)
    {
        g_path_fwd_declined++;
        FlexNet_Log("PATH-FWD-DECLINE: target=%s no live session via %s",
                    target, d->via_callsign[0] ? d->via_callsign : "?");
        return FALSE;
    }

    /* Never hand it back to the peer that asked. */
    if (via == asker_idx)
    {
        g_path_fwd_declined++;
        FlexNet_Log("PATH-FWD-DECLINE: target=%s next hop IS the asker",
                    target);
        return FALSE;
    }

    char nexthop[20] = {0};
    flex_sess_peer_call(&FlexNetSessions[via], nexthop, sizeof(nexthop));
    if (!nexthop[0]) { g_path_fwd_declined++; return FALSE; }

    /* Loop guard: if the next hop is already in the chain, this
       traversal has been there. The chain is the only loop information
       the wire carries. */
    if (strcasecmp(origin, nexthop) == 0)
    {
        g_path_fwd_declined++;
        FlexNet_Log("PATH-FWD-DECLINE: target=%s next hop %s is the origin",
                    target, nexthop);
        return FALSE;
    }
    for (int i = 0; i < n_hops; i++)
        if (strcasecmp(hops[i], nexthop) == 0)
        {
            g_path_fwd_declined++;
            FlexNet_Log("PATH-FWD-DECLINE: target=%s next hop %s already in "
                        "chain — loop", target, nexthop);
            return FALSE;
        }

    /* Bound the traversal. The chain length is the only hop limit the
       protocol offers, exactly as with L2 forwarding. */
    if (n_hops + 1 >= FLEXNET_MAX_PATH_HOPS)
    {
        g_path_fwd_declined++;
        FlexNet_Log("PATH-FWD-DECLINE: target=%s chain full (%d hops)",
                    target, n_hops);
        return FALSE;
    }

    /* Rebuild: origin, hops[0..n-2], nexthop, target. */
    unsigned char out[256];
    if ((int)sizeof(out) < hdr) { g_path_fwd_declined++; return FALSE; }
    memcpy(out, data, (size_t)hdr);
    out[1] = (unsigned char)(data[1] + 1);      /* observed delta */

    int pos = hdr;
    const char * parts[FLEXNET_MAX_PATH_HOPS + 2];
    int np = 0;
    parts[np++] = origin;
    for (int i = 0; i < n_hops - 1; i++) parts[np++] = hops[i];
    parts[np++] = nexthop;
    parts[np++] = target;

    for (int i = 0; i < np; i++)
    {
        int hl = (int)strlen(parts[i]);
        int need = (i == 0 ? 0 : 1) + hl;
        if (pos + need + 1 >= (int)sizeof(out))
        {
            g_path_fwd_declined++;
            FlexNet_Log("PATH-FWD-DECLINE: target=%s rebuilt frame too long",
                        target);
            return FALSE;
        }
        if (i > 0) out[pos++] = ' ';
        memcpy(out + pos, parts[i], (size_t)hl);
        pos += hl;
    }
    out[pos++] = '\r';

    flex_send_frame(FlexNetSessions[via].LINK, FLEXNET_PID_CE, out, pos);
    g_path_fwd_sent++;

    FlexNet_Log("PATH-FWD: target=%s -> next=%s (chain %d -> %d hops, "
                "hopbyte 0x%02x -> 0x%02x, %d bytes)",
                target, nexthop, n_hops, np - 1,
                (unsigned)data[1], (unsigned)out[1], pos);
    if (FLEXNET_DEBUG)
        FlexNet_Info("FlexNet: PATH-FWD target=%s via %s (traversal "
                      "forwarded, not answered)", target, nexthop);
    return TRUE;
}

/* Incoming PATH_REQ: reply with type-7 for us AND for transit
 * destinations; drop only when we genuinely cannot answer. */
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

    if (FLEXNET_DEBUG) FlexNet_Info("FlexNet: PATH-REQ qso=%d origin=%s target=%s "
                "hop_count=%d (us=%d)",
                qso, origin, target, hop_count, flex_target_is_us(target));

    char mycall_norm[20] = {0};
    ConvFromAX25((unsigned char *)MYCALL, (unsigned char *)mycall_norm);
    { int sl = (int)strlen(mycall_norm);
      while (sl > 0 && mycall_norm[sl-1] == ' ') mycall_norm[--sl] = '\0'; }

    /* Chain we are about to advertise, and the FIRST element is the
       ASKER, not us.
     *
     * Every working type-7 we receive is anchored that way. Frames off
     * the wire, replying to probes WE originated:
     *
     *   '7' 'C' "   51" "IR2UFV IW2OHX-14 IW2OHX-12 IQ2LB-6"
     *   '7' '$' "   41" "IR2UFV IW2OHX-14 HB9ON-15 HB9ON-10"
     *
     * — the chain starts with IR2UFV, the originator of the request. It
     * is also how PC/Flexnet's answers render on IW2OHX-4:
     * `IW2OHX-4 IW2OHX-12 IW2OHX-14 HB9ON-15 VE3MCH-8 VA3BAL-8`, asker
     * first.
     *
     * We used to start at mycall. IW2OHX-4 then received a chain that
     * did not begin with itself, could not anchor it, rendered no
     * `route:` line, and bounced a type-7 straight back at us (logged as
     * PATH-REP-DROP: unsolicited) — observed 2026-09-17 22:07 for
     * target=IR3UHU-1 with a chain well inside the digi limit, so
     * length was not the problem.
     *
     * Our own cache stores the chain with the originator stripped
     * (path_hops = first-hop..target, see flex_handle_path_rep), so the
     * full answer is: asker, us, then the cached chain. */
    const char * reply_hops[FLEXNET_MAX_PATH_HOPS] = { origin, mycall_norm };
    int n_reply = 2;

    if (!flex_target_is_us(target))
    {
        /* TRANSIT PATH ANSWER.
         *
         * This used to drop, and dropping is what broke transit end to
         * end: a peer that has installed a route via us asks us for the
         * hop chain, gets silence, and so can neither render `D <dest>`
         * nor build a connect — observed on IW2OHX-14 2026-09-17 as
         * `D IW2OHX-4` showing `T=3` with no `route:` line, then
         * `C IW2OHX-4` → `link setup (14)... *** link failure`. Only
         * destinations WE announce were affected, because every other
         * destination's request is answered by somebody else. The route
         * advertisement (G1) was fine; the path query was not.
         *
         * We answer from our own resolved path rather than relaying the
         * request, because we already probe destinations ourselves and
         * cache the chain (`path_hops[]`, populated by type-7 replies).
         * Relaying would mean correlating QSO ids across two sessions —
         * real M6 forwarding, worth doing only if a destination we
         * cannot answer for turns up in practice.
         */
        if (!g_flexnet_transit_enabled)
        {
            FlexNet_Log("PATH-REQ-DROP: target=%s not us, transit disabled",
                        target);
            return;
        }

        int di = flex_find_dest_for_target(target);
        if (di < 0)
        {
            FlexNet_Log("PATH-REQ-DROP: target=%s not us and not in our "
                        "dest table", target);
            return;
        }

        struct FLEXNET_DEST_ENTRY * d = &FlexNetDests[di];
        /* path_updated is 0 until a type-7 reply lands, and `now - 0` is
           the epoch — which is how this log came to report
           `age=1789673536s`. Report -1 for "never" instead of a number
           that looks like data. */
        time_t age = d->path_updated ? time(NULL) - d->path_updated : -1;

        if (d->path_len > 0 && d->path_updated &&
            age <= FLEXNET_PATH_CACHE_TTL)
        {
            for (int h = 0; h < d->path_len &&
                            n_reply < FLEXNET_MAX_PATH_HOPS; h++)
            {
                if (!d->path_hops[h][0]) continue;
                reply_hops[n_reply++] = d->path_hops[h];
            }
        }
        else if (flex_target_is_direct_peer(target))
        {
            /* Genuinely one hop beyond us, so "us, then it" is the
               whole truth and the peer's two-digi chain will work. */
            reply_hops[n_reply++] = target;
        }
        else
        {
            /* NEVER answer with a truncated chain.
             *
             * A `via_callsign` fallback was tried here and is actively
             * harmful: on 2026-09-17 it answered IW2OHX-4's query for
             * IR8CSB — four hops away by our own earlier probe — with
             * `hops=2 [IR2UFV IW2OHX-14]`. -4 concluded IR8CSB was one
             * hop beyond us, built the AX.25 two-digi chain
             * `IW2OHX-4* IR2UFV`, we dutifully repeated it, and the
             * fully-repeated frame was then addressed to a station that
             * is not an AXIP peer of ours and went nowhere:
             * `*** link failure with IR8CSB`.
             *
             * Staying silent is strictly better. The peer cannot
             * conclude it may digipeat, so it falls back to a NetROM
             * CREQ, which is the mechanism §6 exists to forward. Our
             * own background probe fills the cache within a probe cycle
             * and the next query is answered in full. A wrong chain is
             * worse than no chain. */
            /* Prefer forwarding: it answers the peer from the node that
               actually knows, so a probe timeout here stops mattering.
               Only fall back to probe-and-park when forwarding is off or
               declines. */
            BOOL fwd = flex_forward_path_req((int)(sess - FlexNetSessions),
                                             target, di, origin, hops,
                                             n_hops, data, len);
            BOOL probed = FALSE;
            if (!fwd)
            {
                probed = flex_ondemand_probe(di, target);
                if (probed)
                    flex_defer_path_req((int)(sess - FlexNetSessions), target,
                                        data, len);
            }
            FlexNet_Log("PATH-REQ-NOANSWER: target=%s reachable via %s but "
                        "no cached chain (path_len=%d age=%lds) — staying "
                        "silent rather than answering a truncated path"
                        "%s",
                        target,
                        d->via_callsign[0] ? d->via_callsign : "?",
                        d->path_len, (long)age,
                        fwd ? " [traversal forwarded]"
                            : (probed ? " [probing now]" : ""));
            return;
        }
    }

    /* A chain that is TOO LONG is as wrong as a truncated one.
     *
     * reply_hops is [us, ...path..., target]; the asking peer turns all
     * but the final entry into digipeaters, so it needs n_reply-1 of
     * them. AX.25 holds 8. On 2026-09-17 we answered IW2OHX-4's query
     * for DB0ALG with hops=11 — ten digis — and -4, unable to express
     * that address field, rendered no `route:` line and failed the
     * connect: `link setup (4)... *** link failure with DB0ALG`. The
     * identical connect succeeded eighteen minutes later, when an empty
     * cache made us stay silent and -4 fell back to the two-digi chain
     * `IW2OHX-4* IR2UFV` that FlexNet_L2Transit() then extended one hop
     * at a time — across ten hops.
     *
     * That is the whole point of hop-by-hop rewriting: no node needs the
     * full path, so supplying an unusable one replaces a mechanism that
     * works with one that cannot. Our own background probe is what fills
     * the cache and triggers this, so the failure appears per
     * destination, after each is probed, on a node that still looks
     * healthy.
     *
     * Cap on the port we would answer over, as FlexNet_L2Transit() does.
     * We cannot know the ASKING peer's PORTMAXDIGIS, so the AX.25
     * ceiling is the only defensible bound. */
    /* Endpoints are not digipeaters: reply_hops[0] is the asker and the
       last entry is the target, so the chain it must repeat through is
       everything between them. */
    int reply_digis = n_reply - 2;
    int digi_cap = (LINK->LINKPORT && LINK->LINKPORT->PORTMAXDIGIS)
                       ? LINK->LINKPORT->PORTMAXDIGIS
                       : FLEXNET_L2_MAX_DIGIS;
    if (digi_cap > FLEXNET_L2_MAX_DIGIS) digi_cap = FLEXNET_L2_MAX_DIGIS;

    if (reply_digis > digi_cap)
    {
        FlexNet_Log("PATH-REQ-TOOLONG: target=%s chain needs %d digis > "
                    "cap %d — staying silent so the peer falls back to "
                    "hop-by-hop L2 forwarding, which carries it",
                    target, reply_digis, digi_cap);
        if (FLEXNET_DEBUG)
            FlexNet_Info("FlexNet: PATH-REQ-TOOLONG target=%s digis=%d "
                         "cap=%d — silent (L2 transit carries it)",
                         target, reply_digis, digi_cap);

        /* We cannot express this chain, but we do not have to: hand the
           traversal on and let the node adjacent to the target answer.
           That is the whole point of forwarding — no single node has to
           express a path it cannot carry. */
        (void)flex_forward_path_req((int)(sess - FlexNetSessions), target,
                                    flex_find_dest_for_target(target),
                                    origin, hops, n_hops, data, len);
        return;
    }

    unsigned char reply[256];
    int rlen = flex_build_path_rep(reply, sizeof(reply), qso, trace,
                                   reply_hops, n_reply);
    if (rlen <= 0)
    {
        FlexNet_Log("PATH-REQ-DROP: build_rep failed (n_hops=%d)", n_reply);
        return;
    }
    flex_send_frame(LINK, FLEXNET_PID_CE, reply, rlen);
    {
        char chain[160] = {0};
        for (int h = 0; h < n_reply; h++)
        {
            if (h) strncat(chain, " ", sizeof(chain) - strlen(chain) - 1);
            strncat(chain, reply_hops[h], sizeof(chain) - strlen(chain) - 1);
        }
        FlexNet_Log("PATH-REP-TX: -> origin=%s qso=%d trace=%d target=%s "
                    "hops=%d [%s] (%d bytes)",
                    origin, qso, trace, target, n_reply, chain, rlen);
    }
}

/* Relay a type-7 answer one hop back toward the station that asked.
 *
 * Returns TRUE if the frame was passed on. The chain identifies the path
 * completely, so "who asked" is whoever sits immediately before us in
 * it — no per-traversal state. If we are the first element the answer is
 * ours to keep, and if we are absent the frame is not ours at all. */
static BOOL flex_relay_path_rep(const char * origin,
                                char hops[][FLEXNET_MAX_CALLSIGN],
                                int n_hops,
                                const unsigned char * data, int len)
{
    if (!g_flexnet_path_forward_enabled) return FALSE;
    if (n_hops < 1 || len <= 0) return FALSE;

    char mycall[20] = {0};
    ConvFromAX25((unsigned char *)MYCALL, (unsigned char *)mycall);
    { int sl = (int)strlen(mycall);
      while (sl > 0 && mycall[sl-1] == ' ') mycall[--sl] = '\0'; }
    if (!mycall[0]) return FALSE;

    /* Walk the chain as [origin, hops[0..n-1]] and locate ourselves. */
    if (strcasecmp(origin, mycall) == 0)
        return FALSE;                   /* we are the originator — keep it */

    int me = -1;
    for (int i = 0; i < n_hops; i++)
        if (strcasecmp(hops[i], mycall) == 0) { me = i; break; }
    if (me < 0) return FALSE;           /* not in this chain */

    const char * prev = (me == 0) ? origin : hops[me - 1];
    int via = flex_session_for_call(prev);
    if (via < 0 || !FlexNetSessions[via].active || !FlexNetSessions[via].LINK)
    {
        FlexNet_Log("PATH-REP-RELAY-DECLINE: prev hop %s has no live "
                    "session", prev);
        return FALSE;
    }

    flex_send_frame(FlexNetSessions[via].LINK, FLEXNET_PID_CE,
                    (unsigned char *)data, len);
    g_path_rep_relayed++;
    FlexNet_Log("PATH-REP-RELAY: answer for chain origin=%s passed back to "
                "%s (we are hop %d of %d, %d bytes)",
                origin, prev, me + 1, n_hops, len);
    return TRUE;
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
        /* Not a reply to a probe of ours. It may still be the answer to
           a traversal we FORWARDED, on its way back to whoever asked —
           so relay it before calling it unsolicited.
         *
         * The frame is self-describing: the chain is
         * [origin, ...hops..., target], so find our own callsign in it
         * and pass the frame to the element before us. No reverse-path
         * state is needed, which is the nice property of this design.
         *
         * Forwarded unchanged. The capture shows the ANSWERING node
         * bumping the byte after the type once; there is no evidence of
         * relays bumping it again, so nothing is invented here. */
        if (flex_relay_path_rep(origin, hops, n_hops, data, len))
            return;

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

    /* The cache for this target is now warm. If a peer asked about it
       while it was cold, answer that original request now by replaying
       its frame — see the Deferred path answers block. Without this the
       peer's first `D <dest>` renders no route even though the answer
       arrived milliseconds later. */
    {
        char base[20] = {0};
        flex_base_call(probe->target_call, base, sizeof(base));
        time_t now = time(NULL);

        for (int i = 0; i < FLEXNET_MAX_DEFERRED_REQ; i++)
        {
            struct FLEXNET_DEFERRED_REQ * d = &FlexNetDeferred[i];
            if (!d->in_use) continue;
            if ((now - d->parked_at) > FLEXNET_DEFERRED_REQ_TTL)
            {
                d->in_use = FALSE;      /* stale — the probe never answered */
                continue;
            }
            if (strcasecmp(d->target_base, base) != 0) continue;

            struct FLEXNET_SESSION * psess = &FlexNetSessions[d->peer_idx];
            /* Consume the entry BEFORE replaying: the replay must not be
               able to re-park or re-enter this one. */
            d->in_use = FALSE;
            if (!psess->active || !psess->LINK) continue;

            unsigned char copy[FLEXNET_DEFERRED_FRAME_MAX];
            int clen = d->frame_len;
            memcpy(copy, d->frame, (size_t)clen);

            FlexNet_Log("PATH-DEFER-REPLAY: answering parked request for %s "
                        "to peer slot %d", base, d->peer_idx);

            g_path_replaying = TRUE;
            flex_handle_path_req(psess->LINK, psess, copy, clen);
            g_path_replaying = FALSE;
        }
    }
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
    if (FLEXNET_DEBUG) FlexNet_Info("FlexNet: PATH-REQ-TX next=%s target=%s qso=%d",
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
        FlexNet_Info("FlexNet: path cache saved (%d entries)", written);
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
        FlexNet_Info("FlexNet: path cache loaded (%d entries, %d stale, %d bad)",
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

           This is a PARTIAL path, and it is DISPLAY ONLY — never
           answer a peer with it (see flex_handle_path_req).

           An earlier version of this comment claimed our background
           PATH_REQ probes receive no replies. That was false: the log
           holds 185 `PATH_REP from IW2OHX-14` and 39 from IW2OHX-12.
           Replies arrive in volume, and the cached-path branch above
           does populate — which is exactly how we came to answer a
           peer with an 11-hop chain it could not carry. */
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

/* qsort comparator state — sort by via-cost / callsign / age. */
static int g_dest_sort_mode_qs = 0;
static time_t g_dest_sort_now_qs = 0;

static int flex_dest_cmp_qs(const void * a, const void * b)
{
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    const struct FLEXNET_DEST_ENTRY * ea = &FlexNetDests[ia];
    const struct FLEXNET_DEST_ENTRY * eb = &FlexNetDests[ib];

    switch (g_dest_sort_mode_qs)
    {
    case 1: /* COST — ascending RTT, ties broken by callsign */
        if (ea->rtt != eb->rtt) return ea->rtt - eb->rtt;
        return strcasecmp(ea->callsign, eb->callsign);
    case 2: /* CALL — alphabetical, ties broken by ssid_lo */
    {
        int c = strcasecmp(ea->callsign, eb->callsign);
        if (c != 0) return c;
        return ea->ssid_lo - eb->ssid_lo;
    }
    case 3: /* AGE — freshest path_updated first; uncached last */
    {
        time_t aa = (ea->path_len > 0) ? ea->path_updated : 0;
        time_t bb = (eb->path_len > 0) ? eb->path_updated : 0;
        if (aa != bb) return (bb > aa) ? 1 : -1;  /* newer first */
        return strcasecmp(ea->callsign, eb->callsign);
    }
    default:
        return ia - ib;
    }
}

void FlexNet_CmdDest(TRANSPORTENTRY * Session, char * Bufferptr,
                     char * CmdTail, struct CMDX * CMD)
{
    char callsign_filter[FLEXNET_MAX_CALLSIGN] = {0};
    int  have_filter = 0;

    /* Optional modifiers — parsed in any order:
         <neigh>     route-via filter (neighbour callsign, optional -SSID)
         !           cached-path only (path_len>0 and within TTL)
         ?           uncached only
         /COST       sort by RTT ascending
         /CALL       sort by callsign alphabetically
         /AGE        sort by path_updated (freshest first)
       Plus the existing callsign-filter token (may include wildcards
       or be a bare "*" meaning show-all). */
    char via_filter[FLEXNET_MAX_CALLSIGN] = {0};
    int  have_via_filter = 0;
    int  cache_filter = 0;  /* 0=none, 1=cached-only, 2=uncached-only */
    int  sort_mode = 0;     /* 0=none, 1=COST, 2=CALL, 3=AGE */

    if (CmdTail && *CmdTail && *CmdTail != '\r' && *CmdTail != '\n')
    {
        while (*CmdTail == ' ') CmdTail++;
        while (*CmdTail && *CmdTail != '\r' && *CmdTail != '\n')
        {
            while (*CmdTail == ' ') CmdTail++;
            if (!*CmdTail || *CmdTail == '\r' || *CmdTail == '\n') break;

            char tok[FLEXNET_MAX_CALLSIGN] = {0};
            int  ti = 0;
            while (*CmdTail && *CmdTail != ' ' &&
                   *CmdTail != '\r' && *CmdTail != '\n' &&
                   ti < (int)sizeof(tok) - 1)
            {
                tok[ti++] = (char)toupper((unsigned char)*CmdTail++);
            }
            tok[ti] = '\0';
            if (ti == 0) continue;

            if (tok[0] == '<')
            {
                /* "<NEIGH" or "< NEIGH" — neighbour-via filter. */
                char * src = (tok[1]) ? &tok[1] : NULL;
                if (!src)
                {
                    while (*CmdTail == ' ') CmdTail++;
                    ti = 0;
                    while (*CmdTail && *CmdTail != ' ' &&
                           *CmdTail != '\r' && *CmdTail != '\n' &&
                           ti < (int)sizeof(tok) - 1)
                    {
                        tok[ti++] = (char)toupper((unsigned char)*CmdTail++);
                    }
                    tok[ti] = '\0';
                    if (ti > 0) src = tok;
                }
                if (src && *src)
                {
                    strncpy(via_filter, src, sizeof(via_filter) - 1);
                    have_via_filter = 1;
                }
            }
            else if (tok[0] == '/' && ti > 1)
            {
                if      (strcmp(&tok[1], "COST") == 0) sort_mode = 1;
                else if (strcmp(&tok[1], "CALL") == 0) sort_mode = 2;
                else if (strcmp(&tok[1], "AGE")  == 0) sort_mode = 3;
            }
            else if (tok[0] == '!' && tok[1] == '\0')
            {
                cache_filter = 1;
            }
            else if (tok[0] == '?' && tok[1] == '\0')
            {
                cache_filter = 2;
            }
            else if (!have_filter)
            {
                strncpy(callsign_filter, tok, sizeof(callsign_filter) - 1);
                have_filter = 1;
            }
            /* Subsequent unrecognised tokens are ignored — keeps the
               grammar forward-compatible. */
        }
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

    time_t now_list = time(NULL);
    int shown = 0;

    /* Pre-extract the via-filter callsign and optional SSID once, so
       the per-row check is just two comparisons. */
    char vf_call[FLEXNET_MAX_CALLSIGN] = {0};
    int  vf_ssid = -1;
    if (have_via_filter)
    {
        char * dash = strchr(via_filter, '-');
        if (dash)
        {
            int clen = (int)(dash - via_filter);
            if (clen > 0 && clen < FLEXNET_MAX_CALLSIGN)
            {
                strncpy(vf_call, via_filter, clen);
                vf_call[clen] = '\0';
                vf_ssid = atoi(dash + 1);
            }
        }
        else
        {
            strncpy(vf_call, via_filter, sizeof(vf_call) - 1);
        }
    }

    /* Build a list of candidate indices (so sorting is applied to a
       small int array, not to FlexNetDests[] itself). */
    int order[FLEXNET_MAX_DESTS];
    int order_len = 0;
    for (int i = 0; i < FlexNetDestCount; i++) order[order_len++] = i;

    if (sort_mode != 0)
    {
        g_dest_sort_mode_qs = sort_mode;
        g_dest_sort_now_qs  = now_list;
        qsort(order, order_len, sizeof(int), flex_dest_cmp_qs);
    }

    /* v1.9.5+ cosmetic: emit dests in 3 columns, xnet-style.
       Each cell is 24 chars: "%-6s %-5s %5d%-2s " ' '*remainder.
       3 columns × 24 chars = 72 chars per line. */
    char line[256];
    int  line_pos = 0;
    int  col = 0;
    const int CELL_WIDTH = 24;

    for (int oi = 0; oi < order_len; oi++)
    {
        int i = order[oi];
        struct FLEXNET_DEST_ENTRY * e = &FlexNetDests[i];
        if (e->rtt >= FLEXNET_RTT_INFINITY) continue;
        if (e->callsign[0] == '\0') continue;

        /* Via-neighbour filter ("D < IW2OHX-14"). The route's chosen
           neighbour is stored verbatim in via_callsign, possibly with
           SSID embedded (e.g. "IW2OHX-14"). Compare base and SSID
           separately so the operator can write "D < IW2OHX" to match
           any SSID of that base call. */
        if (have_via_filter)
        {
            char ec[FLEXNET_MAX_CALLSIGN] = {0};
            int  es = -1;
            const char * vcall = e->via_callsign;
            if (vcall[0])
            {
                const char * ed = strchr(vcall, '-');
                if (ed)
                {
                    int clen = (int)(ed - vcall);
                    if (clen > 0 && clen < FLEXNET_MAX_CALLSIGN)
                    {
                        strncpy(ec, vcall, clen);
                        ec[clen] = '\0';
                        es = atoi(ed + 1);
                    }
                }
                else
                {
                    strncpy(ec, vcall, sizeof(ec) - 1);
                }
            }
            if (ec[0] == '\0' || strcasecmp(ec, vf_call) != 0) continue;
            if (vf_ssid >= 0 && es != vf_ssid) continue;
        }

        /* Cached-path filter — '!' = only fresh-path entries,
           '?' = only entries with no fresh cached path. */
        if (cache_filter)
        {
            int has_path =
                (e->path_len > 0 &&
                 (now_list - e->path_updated) < FLEXNET_PATH_CACHE_TTL);
            if (cache_filter == 1 && !has_path) continue;
            if (cache_filter == 2 &&  has_path) continue;
        }

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

        /* Build the cell — fixed width, padded with trailing spaces
           so columns line up regardless of SSID-range or RTT width. */
        char cell[40];
        int  clen = snprintf(cell, sizeof(cell),
            "%-6s %-5s %5d%-2s",
            e->callsign, ssid_range, e->rtt,
            path_mark[0] ? "! " : "  ");
        if (clen < 0) clen = 0;
        if (clen > (int)sizeof(cell) - 1) clen = (int)sizeof(cell) - 1;
        while (clen < CELL_WIDTH && clen < (int)sizeof(cell) - 1)
            cell[clen++] = ' ';
        cell[clen] = '\0';

        line_pos += snprintf(line + line_pos,
                             sizeof(line) - line_pos, "%s", cell);
        col++;

        if (col >= 3)
        {
            Bufferptr = Cmdprintf(Session, Bufferptr, "%s\r", line);
            line[0] = '\0';
            line_pos = 0;
            col = 0;
        }
        shown++;
    }

    /* Flush any partial last line */
    if (col > 0 && line_pos > 0)
        Bufferptr = Cmdprintf(Session, Bufferptr, "%s\r", line);

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
        if (flex_is_established(sess) && sess->sent_routes)
            status = "CONNECTED";
        else if (flex_is_established(sess))
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

    /* v2.2 rc4 — transit state, so a Phase 1 soak can be read off the
       node instead of only out of the console log. */
    if (g_flexnet_transit_enabled)
    {
        Bufferptr = Cmdprintf(Session, Bufferptr, "\r");
        Bufferptr = Cmdprintf(Session, Bufferptr,
            "FlexNet Transit (FLEXNETTRANSIT YES)  rtt0-skips=%lu\r",
            g_flexnet_rtt0_skips);
        if (g_flexnet_l2_transit_enabled)
            Bufferptr = Cmdprintf(Session, Bufferptr,
                "L2 forwarding ON: extended=%lu contracted=%lu "
                "declined=%lu\r",
                g_l2_fwd_extended, g_l2_fwd_contracted, g_l2_fwd_declined);
        if (g_flexnet_path_forward_enabled)
            Bufferptr = Cmdprintf(Session, Bufferptr,
                "Path forwarding ON: forwarded=%lu declined=%lu "
                "replies-relayed=%lu\r",
                g_path_fwd_sent, g_path_fwd_declined, g_path_rep_relayed);
        Bufferptr = Cmdprintf(Session, Bufferptr,
            "Peer         Family  Learned  Direct  Advert  Queued  Tokens\r");
        Bufferptr = Cmdprintf(Session, Bufferptr,
            "------------ ------  -------  ------  ------  ------  ------\r");

        for (int i = 0; i < FLEXNET_MAX_SESSIONS; i++)
        {
            struct FLEXNET_SESSION * sess = &FlexNetSessions[i];
            if (!sess->active || !sess->LINK) continue;
            if (sess->LINK->LINKCALL[0] == 0) continue;

            char tcall[20] = {0};
            flex_sess_peer_call(sess, tcall, sizeof(tcall));

            int direct = 0;
            for (int ri = 0; ri < FlexNetLearned[i].count; ri++)
                if (FlexNetLearned[i].routes[ri].is_direct_neighbour)
                    direct++;

            int qd = 0;
            for (int ai = 0; ai < FlexNetAdvertised[i].count; ai++)
                if (FlexNetAdvertised[i].advs[ai].pending) qd++;

            Bufferptr = Cmdprintf(Session, Bufferptr,
                "%-12s %-6s  %7d  %6d  %6d  %6d  %6.2f\r",
                tcall, flex_peer_is_pcf(sess) ? "PCF" : "xnet",
                FlexNetLearned[i].count, direct,
                FlexNetAdvertised[i].count, qd,
                FlexNetAdvertised[i].tokens);
        }
    }

    SendCommandReply(Session, REPLYBUFFER,
        (int)(Bufferptr - (char *)REPLYBUFFER));
}

/* ── CE Frame Classifier ─────────────────────────────────────────────── */

static int flex_parse_ce_frame(unsigned char * data, int len)
{
    if (len <= 0) return -1;

    /* Keepalive: '2' prefix followed by any number of spaces.
       (X)Net emits 241-byte frames ('2' + 240 spaces); PC/Flexnet
       emits 201-byte frames ('2' + 200 spaces). Per the FlexNet
       protocol spec a receiver should accept any length >= 2 whose
       body after the leading '2' is whitespace. Match on prefix +
       second-byte-space; the size discrepancy alone shouldn't
       reclassify a PC/Flexnet keepalive as UNKNOWN. */
    if (len >= 2 && data[0] == '2' && data[1] == ' ')
        return CE_FRAME_KEEPALIVE;

    /* A 3-byte "1n\r" is a LINK_TIME carrying a single-digit value, not
       a status frame. Checked before the status block, which would
       otherwise claim it. See g_flexnet_lt3byte_enabled. */
    if (g_flexnet_lt3byte_enabled && len == 3 &&
        data[0] == '1' && data[1] >= '0' && data[1] <= '9' &&
        data[2] == '\r')
        return CE_FRAME_LINK_TIME;

    /* 3-byte status frames */
    if (len == 3)
    {
        if (data[0]=='3' && data[1]=='+' && data[2]=='\r')
            return CE_FRAME_STATUS_POS;
        if (data[0]=='3' && data[1]=='-' && data[2]=='\r')
            return CE_FRAME_STATUS_NEG;
        if (data[0]=='1' && data[1]=='0' && data[2]=='\r')
            return CE_FRAME_STATUS_10;
        /* "1n\r" status family for n=1..9 — sibling of STATUS_10.
           Observed shapes so far: "12\r" from xnet peers. Semantic
           not yet documented; treated as benign status notification.
           See ROADMAP.md GA item #1 and project memory. */
        if (data[0]=='1' && data[1] >= '1' && data[1] <= '9' && data[2]=='\r')
            return CE_FRAME_STATUS_1N;
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

/* Public wrapper around flex_parse_ce_frame for L2Code's PID=F0 bypass.
   Returns the CE_FRAME_* type code if `data` is recognised as a FlexNet
   CE-shaped INFO body, or -1 otherwise.

   Caller passes the INFO bytes only (no AX.25 PID byte), exactly as
   FlexNet_ProcessCE sees them after stripping the leading PID. See
   research/ir2ufv-pcf-v2.1.35-capture-analysis-2026-06-02.md for the
   PCF PID=0xF0 demotion the L2 bypass works around. */
int FlexNet_ClassifyCEShape(unsigned char * data, int len)
{
    return flex_parse_ce_frame(data, len);
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

static void flex_learned_add(int sess_idx,
                             const struct FLEXNET_DEST_ENTRY * route)
{
    if (sess_idx < 0 || sess_idx >= FLEXNET_MAX_SESSIONS) return;
    if (!route || !route->callsign[0]) return;
    /* Withdrawal records (RTT=60000) still go into learned[] so the
       next periodic re-advert can poison-reverse the destination
       downstream. */
    struct FLEXNET_LEARNED_STATE * st = &FlexNetLearned[sess_idx];

    /* Find existing entry by (callsign, ssid_lo, ssid_hi) */
    for (int i = 0; i < st->count; i++)
    {
        struct FLEXNET_LEARNED_ROUTE * r = &st->routes[i];
        if (r->ssid_lo == route->ssid_lo && r->ssid_hi == route->ssid_hi &&
            strncmp(r->dest_call, route->callsign,
                    FLEXNET_MAX_CALLSIGN) == 0)
        {
            if (r->rtt_at_neighbour != route->rtt)
            {
                r->rtt_at_neighbour = route->rtt;
                st->dirty = TRUE;
                /* RFC §5.2/§5.3 trigger (a) — the RTT this peer reports
                   moved, so what we'd tell everyone else about this
                   destination moved with it. Split-horizon is inside
                   flex_advertise_check's source walk; the drain call is
                   what lets a change on a quiet link go out at once on
                   accumulated bucket credit. */
                for (int pi = 0; pi < FLEXNET_MAX_SESSIONS; pi++)
                {
                    if (pi == sess_idx) continue;
                    if (!FlexNetSessions[pi].active) continue;
                    flex_advertise_check(pi, r->dest_call,
                                         r->ssid_lo, r->ssid_hi, FALSE);
                    flex_advertise_drain(pi);
                }
            }
            r->last_heard = time(NULL);
            return;
        }
    }
    /* New entry — append if there's room */
    if (st->count >= FLEXNET_MAX_LEARNED_PER_NEIGHBOUR)
    {
        if (FLEXNET_DEBUG) FlexNet_Info("FlexNet: learned[] full for session %d "
                    "— dropping new route %s (%d-%d)",
                    sess_idx, route->callsign,
                    route->ssid_lo, route->ssid_hi);
        return;
    }
    struct FLEXNET_LEARNED_ROUTE * r = &st->routes[st->count++];
    strncpy(r->dest_call, route->callsign, FLEXNET_MAX_CALLSIGN - 1);
    r->dest_call[FLEXNET_MAX_CALLSIGN - 1] = '\0';
    r->ssid_lo = route->ssid_lo;
    r->ssid_hi = route->ssid_hi;
    r->rtt_at_neighbour = route->rtt;
    r->last_heard = time(NULL);
    st->dirty = TRUE;

    /* Trigger (a) for a brand-new destination — a change from nothing. */
    for (int pi = 0; pi < FLEXNET_MAX_SESSIONS; pi++)
    {
        if (pi == sess_idx) continue;
        if (!FlexNetSessions[pi].active) continue;
        flex_advertise_check(pi, r->dest_call, r->ssid_lo, r->ssid_hi, FALSE);
        flex_advertise_drain(pi);
    }

    if (FLEXNET_DEBUG) FlexNet_Info("FlexNet: learned %s (%d-%d) RTT=%d "
                "from session %d (total learned=%d)",
                r->dest_call, r->ssid_lo, r->ssid_hi,
                r->rtt_at_neighbour, sess_idx, st->count);
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
        g_flexnet_rtt0_skips++;
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
        /* v2.2 — also record into the per-session learned[] table,
           so the periodic transit re-advertiser can see it. */
        if (g_flexnet_transit_enabled)
            flex_learned_add(sess_idx, incoming);
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
        /* v2.2 — record into per-session learned[] table */
        if (g_flexnet_transit_enabled)
            flex_learned_add(sess_idx, incoming);
        return 1;
    }

    return 0;  /* Table full */
}

/* ── Frame Builders ──────────────────────────────────────────────────── */

/* Emit a CE keepalive that mirrors the peer's last KA shape.
 *
 * Wire observation (2026-05-25, IR2UFV↔IW2OHX-12 24-min capture):
 *   - (X)Net peers emit 241 B: '2' + 240 spaces, no terminator.
 *   - PC/Flexnet peers emit 201 B: '2' + 199 spaces + CR (0x0d).
 * PC/Flexnet silently discards inbound KAs whose final byte isn't CR.
 * That caused IR2UFV's echoed-241-B KAs to be ignored by IW2OHX-12 →
 * link-time decayed to infinity → DISC every ~5 min.
 *
 * Fix: echo the same length & terminator we last accepted from this peer.
 * Before any KA from the peer (initial outbound KA after SABM), or when
 * sess is NULL, fall back to the (X)Net default. The receive-side parser
 * is already permissive (flex_parse_ce_frame: any length, terminator
 * optional), so PC/Flexnet's 201-B-with-CR shape still classifies as KA
 * when fed back here.
 */
static int flex_build_keepalive(unsigned char * buf, int buflen,
                                const struct FLEXNET_SESSION * sess)
{
    if (buflen < FLEXNET_KEEPALIVE_LEN) return -1;

    /* v2.1.13 — universal 241 B + trailing space (no CR), matching
       flexnetd's ce_proto.c:84-86 and PROTOCOL_SPEC.md §2.5. Drops
       v2.1.10's per-session peer_ka shape mirror: empirical evidence
       2026-05-27 showed PCF's link-table saturation isn't caused by
       KA shape, but by negative-delta wrap on rate-limit-violating
       LT replies (see flex_send_link_time below). flexnetd hardcodes
       this single shape regardless of peer flavour and gets correctly
       probed by PCF on IW2OHX-4, so the per-session mirror was
       chasing the wrong variable. (void)sess kept for signature. */
    (void)sess;
    int len            = FLEXNET_KEEPALIVE_LEN;  /* 241 B */
    unsigned char term = ' ';                    /* trailing space, no CR */

    buf[0] = '2';
    memset(buf + 1, ' ', (size_t)(len - 1));
    buf[len - 1] = term;
    return len;
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
   healthy T=1-3 range) and skews link-quality routing.

   v2.1.28 — wire LT bumped from 2 → 5 to match the v2.1.24 30 s
   KA cadence. PC/Flexnet computes its `link.ts` window from our
   advertised LT value as `(smoothed + 4) * 32` × 100 ms ticks =
   (5+4)*32 = 288 ticks = 28.8 s for LT=5. Our KA at 30 s lands
   ~1.2 s past `link.ts`, so PCF's per-sample measurement
   becomes ~12 ticks instead of the ~100 ticks we observed
   pre-v2.1.28 (which was 30 s − 19.2 s for LT=2's 19.2 s
   `link.ts`). Coupled with the v2.1.24 KA threshold dropped from
   30 → 29 s (see FlexNet_Timer), samples land at the link.ts
   boundary + ~2 ticks → cost row on PCF's L * settles at the
   single-digit value xnet peers display. */
#define FLEXNET_WIRE_LT 5

/* v2.1.13 — LT-reply rate-limit gate.
 * PCFlexnet's internal expected-reply timestamp is:
 *     link.ts = now + (smoothed + 4) * 32         (smoothed < 96, 100 ms ticks)
 *     link.ts = now + 3200                        (otherwise)
 * = 12.8 s to 320 s. A type-1 LT reply that arrives BEFORE link.ts
 * makes PCF compute `delta = now - link.ts < 0`, which wraps and clamps
 * the sample to 4095 (12-bit RTT field cap). That's precisely the
 * IR2UFV ↔ IW2OHX-12 saturation we've been chasing — found in flexnetd's
 * poll_cycle.c:508-522 commentary on 2026-05-27.
 *
 * Rate-limit windows below mirror flexnetd's per-port-flavor defaults:
 *   PCF peers      (peer_ka_term == '\r')  : 320 s
 *   (X)Net + unknown                       :  20 s
 *
 * Initial LT during the session-start handshake is unrestricted —
 * last_lt_tx is zero on first call, so `now - 0 >= interval` is always
 * true. Subsequent replies are dropped silently until the window opens
 * again; the peer's KA cadence is unaffected. */
/* v2.1.31 — revert FLEXNET_LT_INTERVAL_PCF back to 320 s.
   The v2.1.29 reduction to 25 s was based on a wrong model of
   PCF's sample math. Wire study on 2026-05-31 confirmed the
   original v2.1.13 rationale: PCF's link.ts = now + (smoothed+4)*32
   ticks (100 ms). When smoothed is high (e.g. 4095 right after
   a fresh INIT or L2-cycle), link.ts is +21.86 min from each PCF
   LT TX. A reply arriving before link.ts produces a NEGATIVE
   delta that wraps to 4095 and pins smoothed at the cap — the
   wire trace at 23:01-23:04 confirmed PCF reporting `14095\r`
   continuously under v2.1.29's 25 s rate-limit.
   320 s sits at the high end of the link.ts window for any
   reasonable smoothed value (PCF clamps at smoothed=92 → link.ts
   = 320 s exactly), so our LT lands at-or-after link.ts, delta
   is positive small, and PCF's IIR converges downward over a
   few cycles. */
#define FLEXNET_LT_INTERVAL_PCF     320  /* seconds */
#define FLEXNET_LT_INTERVAL_XNET     20  /* seconds */

static int flex_send_link_time(LINKTABLE * LINK,
                               struct FLEXNET_SESSION * sess)
{
    time_t now      = time(NULL);
    int interval   = (sess != NULL && sess->peer_ka_term == '\r')
                     ? FLEXNET_LT_INTERVAL_PCF
                     : FLEXNET_LT_INTERVAL_XNET;
    if (sess != NULL && sess->last_lt_tx != 0 &&
        (now - sess->last_lt_tx) < interval)
    {
        /* Within the suppression window — drop silently. The wire-level
           LT cycle should drive against link.ts not against our peer's
           every poke. */
        return 0;
    }

    unsigned char lt[16];
    int ltlen = flex_build_link_time(lt, sizeof(lt), FLEXNET_WIRE_LT);
    if (ltlen <= 0) return -1;

    flex_send_frame(LINK, FLEXNET_PID_CE, lt, ltlen);

    if (sess != NULL)
        sess->last_lt_tx = now;
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

    /* Trigger (b) + §15 Q6 dampening. Every route learned from this
       peer is advertised as learned_rtt + our link RTT to it, so a
       real move in that link RTT invalidates what we told the other
       peers. Gated on >= 1 wire tick (100 ms) from the last anchor:
       re-walking 200+ learned routes on each IIR wiggle would be
       pathological, and the per-record jitter floor in
       flex_advertise_check would suppress almost all of it anyway. */
    if (g_flexnet_transit_enabled)
    {
        int lt_idx = (int)(sess - FlexNetSessions);
        if (lt_idx >= 0 && lt_idx < FLEXNET_MAX_SESSIONS)
        {
            struct FLEXNET_LEARNED_STATE * lst = &FlexNetLearned[lt_idx];
            int anchor = lst->lt_anchor;
            int moved  = (sess->our_link_time > anchor)
                         ? sess->our_link_time - anchor
                         : anchor - sess->our_link_time;
            if (moved >= FLEXNET_REFRESH_THRESHOLD_ABS && lst->count > 0)
            {
                lst->lt_anchor = sess->our_link_time;
                for (int pi = 0; pi < FLEXNET_MAX_SESSIONS; pi++)
                {
                    if (pi == lt_idx) continue;
                    if (!FlexNetSessions[pi].active) continue;
                    for (int ri = 0; ri < lst->count; ri++)
                        flex_advertise_check(pi, lst->routes[ri].dest_call,
                                             lst->routes[ri].ssid_lo,
                                             lst->routes[ri].ssid_hi, FALSE);
                    flex_advertise_drain(pi);
                }
            }
            else if (moved >= FLEXNET_REFRESH_THRESHOLD_ABS)
            {
                lst->lt_anchor = sess->our_link_time;
            }
        }
    }

    if (FLEXNET_DEBUG)
    {
        char nbr[20] = {0};
        ConvFromAX25(sess->LINK->LINKCALL, nbr);
        { int sl = (int)strlen(nbr);
          while (sl > 0 && nbr[sl-1] == ' ') nbr[--sl] = '\0'; }
        FlexNet_Info("FlexNet: lt_sample peer=%s sample=%u smoothed=%u "
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

/* One compact record, WITHOUT the '3' frame prefix and without the
 * terminating '\r'. Several of these pack into one frame; see
 * FLEXNET_ADVERT_FRAME_BYTES. Returns bytes written, or -1 if the
 * record does not fit in `buflen`.
 */
static int flex_build_route_rec(unsigned char * buf, int buflen,
    const char * callsign, int ssid_lo, int ssid_hi, int rtt)
{
    char tmp[32];
    int len = snprintf(tmp, sizeof(tmp), "%-6.6s%c%c%d ",
        callsign,
        (char)(FLEXNET_SSID_BASE + ssid_lo),
        (char)(FLEXNET_SSID_BASE + ssid_hi),
        rtt);
    if (len < 0 || len >= buflen) return -1;
    memcpy(buf, tmp, len);
    return len;
}

/* A complete single-record frame: '3' + one record + '\r'. Kept for the
 * call sites that legitimately emit exactly one destination.
 */
static int flex_build_route(unsigned char * buf, int buflen,
    const char * callsign, int ssid_lo, int ssid_hi, int rtt)
{
    if (buflen < 3) return -1;
    buf[0] = '3';
    int rl = flex_build_route_rec(buf + 1, buflen - 2,
                                  callsign, ssid_lo, ssid_hi, rtt);
    if (rl < 0) return -1;
    buf[1 + rl] = '\r';
    return rl + 2;
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

/* ── v2.2 rc4 — Event-Driven Route Re-Advertisement (RFC §5) ────────── */
/*
 * The rc1-rc3 model advertised on a clock: every N seconds, push a
 * capped slice of learned[] at each peer and rotate a cursor so the
 * rest went out on later cycles. It failed three times. rc1 had no cap
 * and put 326-403 back-to-back records into PC/Flexnet, which saturated
 * its RTT at 4095 and left it in a broken state that does not
 * self-recover. rc2 capped at 8 records per 120 s, which was clean on
 * the wire but made the cursor take ~480 s to revisit a session —
 * slower than xnet's per-destination ageing window, so routes flapped.
 * rc3 fixed CREQ forwarding but the cursor still starved the stream.
 *
 * The error common to all three was one global emission policy for two
 * peer families with very different ingestion rates. Captures show xnet
 * does not sweep a table at all: it emits compact records as a near-1:1
 * mapping of its own table mutations, and its rate to a PC/Flexnet peer
 * settles around 1 record / 50 s. rc4 replicates that — emission is
 * driven by change events, each peer has its own token bucket sized to
 * its family, and the small stable set of direct neighbours gets a
 * timer-driven refresh so it cannot age out between changes.
 */

/* Our own base callsign, without SSID. Returns the SSID separately.
 * ConvFromAX25 writes more than 10 chars, so buf must be >= 20 —
 * a FLEXNET_MAX_CALLSIGN-sized buffer here was a real overflow.
 */
static void flex_own_base_call(char * buf, int buflen, int * ssid_out)
{
    char raw[20] = {0};
    buf[0] = '\0';
    if (buflen < 20) return;
    ConvFromAX25((unsigned char *)MYCALL, (unsigned char *)raw);
    int slen = (int)strlen(raw);
    while (slen > 0 && raw[slen - 1] == ' ') raw[--slen] = '\0';
    char * dash = strchr(raw, '-');
    if (dash)
    {
        if (ssid_out) *ssid_out = atoi(dash + 1);
        *dash = '\0';
    }
    else if (ssid_out) *ssid_out = 0;
    strncpy(buf, raw, (size_t)buflen - 1);
    buf[buflen - 1] = '\0';
}

/* Peer callsign for a session, trimmed. buf must be >= 20 (see above). */
static void flex_sess_peer_call(const struct FLEXNET_SESSION * sess,
                                char * buf, int buflen)
{
    buf[0] = '\0';
    if (!sess || !sess->LINK || buflen < 20) return;
    ConvFromAX25((unsigned char *)sess->LINK->LINKCALL,
                 (unsigned char *)buf);
    int slen = (int)strlen(buf);
    while (slen > 0 && buf[slen - 1] == ' ') buf[--slen] = '\0';
}

/* PC/Flexnet or (X)Net-like?
 *
 * RFC §5.4 keys this off the AXIP MAP `B` flag (PCF = F without B).
 * We use the peer's own keepalive shape instead: PC/Flexnet emits a
 * 201-byte KA terminated with CR, (X)Net a 241-byte KA ending in a
 * space (2026-05-25 IR2UFV↔IW2OHX-12 capture). That is protocol
 * evidence rather than a local config convention — a MAP line edited
 * to add `B` to a PCF peer would silently mis-size its bucket — and it
 * is already what the KA cadence and the old per-emit cap key off.
 * Cost: the family is unknown until the first peer KA arrives, so we
 * answer PCF while unknown, which is the slower and safer bucket.
 */
static BOOL flex_peer_is_pcf(const struct FLEXNET_SESSION * sess)
{
    if (!sess) return TRUE;
    return (sess->peer_ka_term != ' ');
}

/* Cheapest RTT we could advertise to `peer_idx` for one destination,
 * over every OTHER session that has learned it. Split-horizon is
 * structural here: session peer_idx is never considered as a source, so
 * no caller can forget it.
 *
 * Returns FLEXNET_RTT_INFINITY with *src_idx_out = -1 when no session
 * offers a finite path — that is the poison-reverse value, and the
 * caller decides whether this peer has ever been told about the route.
 */
static int flex_expected_rtt(int peer_idx, const char * dest_call,
                             int ssid_lo, int ssid_hi, int * src_idx_out,
                             BOOL * src_is_direct_out)
{
    int  best        = FLEXNET_RTT_INFINITY;
    int  best_src    = -1;
    BOOL best_direct = FALSE;

    for (int si = 0; si < FLEXNET_MAX_SESSIONS; si++)
    {
        if (si == peer_idx) continue;                    /* split-horizon */
        if (!FlexNetSessions[si].active) continue;

        struct FLEXNET_LEARNED_STATE * st = &FlexNetLearned[si];
        for (int ri = 0; ri < st->count; ri++)
        {
            struct FLEXNET_LEARNED_ROUTE * lr = &st->routes[ri];
            if (lr->ssid_lo != ssid_lo || lr->ssid_hi != ssid_hi) continue;
            if (strncmp(lr->dest_call, dest_call, FLEXNET_MAX_CALLSIGN) != 0)
                continue;
            /* learned[] is keyed by (call, ssid_lo, ssid_hi), so this
               session has nothing further to offer for this key. */
            if (lr->rtt_at_neighbour <= 0) break;   /* §15 Q5 RTT=0 skip */
            if (lr->rtt_at_neighbour >= FLEXNET_RTT_INFINITY) break;

            int link_rtt = FlexNetSessions[si].our_link_time;
            if (link_rtt < 1) link_rtt = 1;
            int cand = lr->rtt_at_neighbour + link_rtt;
            if (cand >= FLEXNET_RTT_INFINITY) cand = FLEXNET_RTT_INFINITY - 1;
            if (cand < best)
            {
                best        = cand;
                best_src    = si;
                best_direct = lr->is_direct_neighbour;
            }
            break;
        }
    }

    if (src_idx_out)        *src_idx_out        = best_src;
    if (src_is_direct_out)  *src_is_direct_out  = best_direct;
    return (best_src < 0) ? FLEXNET_RTT_INFINITY : best;
}

static struct FLEXNET_ADVERTISED_ROUTE *
flex_adv_find(int peer_idx, const char * dest_call,
              int ssid_lo, int ssid_hi, BOOL create)
{
    struct FLEXNET_ADVERTISED_STATE * st = &FlexNetAdvertised[peer_idx];

    for (int i = 0; i < st->count; i++)
    {
        struct FLEXNET_ADVERTISED_ROUTE * a = &st->advs[i];
        if (a->ssid_lo == ssid_lo && a->ssid_hi == ssid_hi &&
            strncmp(a->dest_call, dest_call, FLEXNET_MAX_CALLSIGN) == 0)
            return a;
    }
    if (!create) return NULL;

    /* §15 Q4 — reject on overflow and warn; never silently overwrite,
       which was rc1's failure mode. Operator raises the bound and
       rebuilds if a real network ever gets here. */
    if (st->count >= FLEXNET_MAX_ADVERTISED_PER_PEER)
    {
        if (!st->warned_full)
        {
            st->warned_full = TRUE;
            Consoleprintf("FlexNet: advertised[] full for session %d "
                          "(%d entries) — dropping %s (%d-%d). Raise "
                          "FLEXNET_MAX_ADVERTISED_PER_PEER and rebuild.",
                          peer_idx, st->count, dest_call, ssid_lo, ssid_hi);
        }
        return NULL;
    }

    struct FLEXNET_ADVERTISED_ROUTE * a = &st->advs[st->count++];
    memset(a, 0, sizeof(*a));
    strncpy(a->dest_call, dest_call, FLEXNET_MAX_CALLSIGN - 1);
    a->dest_call[FLEXNET_MAX_CALLSIGN - 1] = '\0';
    a->ssid_lo = ssid_lo;
    a->ssid_hi = ssid_hi;
    a->last_advertised_rtt = -1;          /* never advertised */
    return a;
}

/* RFC §5.3 — the decision rule. Recompute what we'd tell `peer_idx`
 * about one destination; queue a record only if that has moved far
 * enough from what we last told it to be worth a frame.
 */
/* Is this destination KEY one of our own active peers?
 *
 * Destinations are keyed (base call, ssid_lo, ssid_hi) while a session
 * carries a full "CALL-n" callsign, so comparing the two as strings
 * never matches. Getting that wrong made the direct-neighbour exemption
 * in flex_advertise_check() dead on arrival: IW2OHX-12 is a peer of
 * ours, yet `dest=IW2OHX-12/12` was suppressed to the other two peers,
 * withholding real adjacency information that RFC §5.5 wants refreshed.
 */
static BOOL flex_dest_is_our_peer(const char * dest_call,
                                  int ssid_lo, int ssid_hi)
{
    if (!dest_call || !dest_call[0]) return FALSE;

    for (int si = 0; si < FLEXNET_MAX_SESSIONS; si++)
    {
        if (!FlexNetSessions[si].active || !FlexNetSessions[si].LINK)
            continue;

        char peer[20] = {0};
        flex_sess_peer_call(&FlexNetSessions[si], peer, sizeof(peer));
        if (!peer[0]) continue;

        /* Split "CALL-n" into base and SSID; a bare "CALL" is SSID 0. */
        char base[20] = {0};
        int  ssid = 0;
        char * dash = strrchr(peer, '-');
        if (dash)
        {
            size_t blen = (size_t)(dash - peer);
            if (blen >= sizeof(base)) blen = sizeof(base) - 1;
            memcpy(base, peer, blen);
            base[blen] = '\0';
            ssid = atoi(dash + 1);
        }
        else
        {
            strncpy(base, peer, sizeof(base) - 1);
        }

        if (strcasecmp(base, dest_call) != 0) continue;
        if (ssid >= ssid_lo && ssid <= ssid_hi) return TRUE;
    }
    return FALSE;
}

/* Did this peer itself tell us about this destination?
 *
 * Keyed exactly as learned[] is, on (call, ssid_lo, ssid_hi), so a
 * partial SSID-range overlap is intentionally not a match — those are
 * different routes to FlexNet. */
static BOOL flex_learned_has(int peer_idx, const char * dest_call,
                             int ssid_lo, int ssid_hi)
{
    if (peer_idx < 0 || peer_idx >= FLEXNET_MAX_SESSIONS) return FALSE;
    if (!dest_call || !dest_call[0]) return FALSE;

    struct FLEXNET_LEARNED_STATE * st = &FlexNetLearned[peer_idx];
    for (int ri = 0; ri < st->count; ri++)
    {
        struct FLEXNET_LEARNED_ROUTE * lr = &st->routes[ri];
        if (lr->ssid_lo != ssid_lo || lr->ssid_hi != ssid_hi) continue;
        if (strncmp(lr->dest_call, dest_call, FLEXNET_MAX_CALLSIGN) == 0)
            return TRUE;
    }
    return FALSE;
}

static void flex_advertise_check(int peer_idx, const char * dest_call,
                                 int ssid_lo, int ssid_hi, BOOL force)
{
    if (!g_flexnet_transit_enabled) return;
    if (peer_idx < 0 || peer_idx >= FLEXNET_MAX_SESSIONS) return;
    if (!FlexNetSessions[peer_idx].active || !FlexNetSessions[peer_idx].LINK)
        return;
    if (!dest_call || !dest_call[0]) return;

    /* Our own record is flex_send_own_routes' job — it carries the
       configured SSID range, which a transit record would flatten. */
    {
        char own[20] = {0};
        flex_own_base_call(own, sizeof(own), NULL);
        if (own[0] && strncmp(dest_call, own, FLEXNET_MAX_CALLSIGN) == 0)
            return;
    }

    int  src_idx   = -1;
    BOOL src_direct = FALSE;
    int  expected  = flex_expected_rtt(peer_idx, dest_call, ssid_lo, ssid_hi,
                                       &src_idx, &src_direct);

    struct FLEXNET_ADVERTISED_ROUTE * adv =
        flex_adv_find(peer_idx, dest_call, ssid_lo, ssid_hi, FALSE);

    /* Split horizon at DESTINATION level.
     *
     * flex_expected_rtt() already refuses peer_idx as a SOURCE, and
     * that is not enough. When a third peer echoes this peer's own
     * routes back to us, the destination IS reachable "via someone
     * else", so we advertised it straight back to the peer that taught
     * it to us. Measured 2026-09-18: 184 records pushed at IW2OHX-14,
     * every one a route -14 had taught us, of which -14 installed 2.
     * quad-watch flagged it 88 times as A3.
     *
     * It is also how count-to-infinity closes here: while -14's session
     * is down, -4's echo of -14's routes becomes our only source, we
     * advertise them back to -14, and -14 has a route to its own
     * destinations via us.
     *
     * Exception: a destination that is one of OUR OWN direct peers.
     * That is our adjacency, not a relayed route, and RFC §5.5 wants
     * neighbours refreshed on a timer — suppressing those would stop us
     * telling -14 that we can reach -12, which is real information.
     *
     * Retract once if we already advertised it, then stop for good;
     * leaving a stale finite route behind would be the loop we are
     * closing. Same shape as the GA scope gate below.
     */
    if (flex_learned_has(peer_idx, dest_call, ssid_lo, ssid_hi) &&
        !flex_dest_is_our_peer(dest_call, ssid_lo, ssid_hi))
    {
        BOOL told_before = (adv && adv->last_advertised_rtt >= 0 &&
                            adv->last_advertised_rtt < FLEXNET_RTT_INFINITY);
        if (!told_before)
        {
            if (FLEXNET_DEBUG)
            {
                char speer[20] = {0};
                flex_sess_peer_call(&FlexNetSessions[peer_idx],
                                    speer, sizeof(speer));
                FlexNet_Trace("FlexNet: SPLIT-HORIZON peer=%s dest=%s-%d/%d "
                              "— it taught us this route, not advertising "
                              "it back", speer, dest_call, ssid_lo, ssid_hi);
            }
            return;
        }
        {
            char speer[20] = {0};
            flex_sess_peer_call(&FlexNetSessions[peer_idx],
                                speer, sizeof(speer));
            /* Operator-visible: this corrects something we should never
               have advertised. */
            FlexNet_Info("FlexNet: SPLIT-RETRACT peer=%s dest=%s-%d/%d "
                         "last=%d — withdrawing, it is that peer's own "
                         "route", speer, dest_call, ssid_lo, ssid_hi,
                         adv->last_advertised_rtt);
        }
        expected = FLEXNET_RTT_INFINITY;
    }

    /* GA scope gate — see FLEXNET_ADVERTISE_DIRECT_ONLY. Placed here,
       in the one decision point, so every trigger site inherits it and
       none can forget: the walkers may still sweep the whole learned
       table and this is what makes that safe.
       A destination we have ALREADY told this peer about is deliberately
       let through with expected forced to infinity, so enabling this
       retracts whatever a previous build advertised instead of stranding
       it. After that single withdrawal last_advertised_rtt is infinity
       and the entry stops here for good. */
    if (flex_advertise_direct_only() && !src_direct)
    {
        BOOL told_before = (adv && adv->last_advertised_rtt >= 0 &&
                            adv->last_advertised_rtt < FLEXNET_RTT_INFINITY);
        if (!told_before)
        {
            if (FLEXNET_DEBUG)
            {
                char gpeer[20] = {0};
                flex_sess_peer_call(&FlexNetSessions[peer_idx],
                                    gpeer, sizeof(gpeer));
                FlexNet_Trace("FlexNet: NOT-DIRECT peer=%s dest=%s-%d/%d "
                              "exp=%d — not advertised (GA scope: we "
                              "cannot carry multi-hop)",
                              gpeer, dest_call, ssid_lo, ssid_hi, expected);
            }
            return;
        }
        {
            char rpeer[20] = {0};
            flex_sess_peer_call(&FlexNetSessions[peer_idx],
                                rpeer, sizeof(rpeer));
            /* Not FLEXNET_DEBUG-gated: a retraction is an operator-
               visible correction of something we should not have
               advertised. */
            FlexNet_Info("FlexNet: RETRACT peer=%s dest=%s-%d/%d last=%d "
                         "— withdrawing, not a direct neighbour",
                         rpeer, dest_call, ssid_lo, ssid_hi,
                         adv->last_advertised_rtt);
        }
        expected = FLEXNET_RTT_INFINITY;
    }

    /* Poison hold-down. We told this peer the destination was gone;
       do not contradict that on the strength of a finite path that
       reappeared within seconds — through the mesh, that is our own
       withdrawal coming back to us. `force` does not bypass this: the
       120 s neighbour refresh must not resurrect a poisoned route
       either. A direct neighbour IS exempt, because its return is
       proven by our own session re-establishing rather than by
       hearsay, and delaying a real neighbour's recovery by up to
       FLEXNET_POISON_HOLDDOWN would be a worse trade. */
    if (adv && !src_direct &&
        adv->last_advertised_rtt >= FLEXNET_RTT_INFINITY &&
        expected < FLEXNET_RTT_INFINITY)
    {
        time_t held = time(NULL) - adv->last_advertised_at;
        if (held < FLEXNET_POISON_HOLDDOWN)
        {
            if (FLEXNET_DEBUG)
            {
                char hpeer[20] = {0};
                flex_sess_peer_call(&FlexNetSessions[peer_idx],
                                    hpeer, sizeof(hpeer));
                FlexNet_Trace("FlexNet: HOLDDOWN peer=%s dest=%s-%d/%d "
                              "exp=%d held=%lds of %ds — suppressed",
                              hpeer, dest_call, ssid_lo, ssid_hi, expected,
                              (long)held, FLEXNET_POISON_HOLDDOWN);
            }
            /* Drop any queued finite value for the same reason. */
            adv->pending = FALSE;
            return;
        }
    }

    /* Poison only what this peer was actually told about. Otherwise a
       session-down walk announces RTT=60000 for destinations the peer
       never heard from us, which is noise it has to age out. */
    if (src_idx < 0)
    {
        if (!adv || adv->last_advertised_rtt < 0 ||
            adv->last_advertised_rtt >= FLEXNET_RTT_INFINITY)
            return;
    }

    if (!adv)
    {
        adv = flex_adv_find(peer_idx, dest_call, ssid_lo, ssid_hi, TRUE);
        if (!adv) return;                          /* advs[] full — warned */
    }

    int  last  = adv->last_advertised_rtt;
    int  delta = (last < 0) ? expected
                            : (expected > last ? expected - last
                                               : last - expected);
    BOOL fired = TRUE;

    if (last >= 0 && !force)
    {
        /* 10 % relative with a 1-tick absolute floor. RTTs live in
           100 ms wire units and are typically single-digit, so the
           floor is what actually gates most of the traffic: it drops
           the sub-tick IIR wiggle that would otherwise put a frame on
           the wire for no routing change. */
        int thresh = (last * FLEXNET_REFRESH_THRESHOLD_PCT) / 100;
        if (thresh < FLEXNET_REFRESH_THRESHOLD_ABS)
            thresh = FLEXNET_REFRESH_THRESHOLD_ABS;
        if (delta < thresh) fired = FALSE;
    }

    if (FLEXNET_DEBUG)
    {
        char peer[20] = {0};
        flex_sess_peer_call(&FlexNetSessions[peer_idx], peer, sizeof(peer));
        FlexNet_Trace("FlexNet: ADVERT-CHECK peer=%s dest=%s-%d/%d exp=%d "
                     "last=%d delta=%d %s%s",
                     peer, dest_call, ssid_lo, ssid_hi, expected, last, delta,
                     fired ? "FIRED" : "SUPPRESSED",
                     force ? " (force)" : "");
    }

    if (!fired)
    {
        /* Already queued behind a dry bucket: keep the slot but carry
           the freshest value, so what finally goes out is current. */
        if (adv->pending) adv->pending_rtt = expected;
        return;
    }

    adv->pending     = TRUE;
    adv->pending_rtt = expected;
}

/* RFC §5.4 — drain one peer's queue through its token bucket. Called
 * every timer tick and again right after a check queues, so a change
 * arriving on a quiet link goes out immediately on accumulated credit.
 */
static void flex_advertise_drain(int peer_idx)
{
    if (!g_flexnet_transit_enabled) return;
    if (peer_idx < 0 || peer_idx >= FLEXNET_MAX_SESSIONS) return;

    struct FLEXNET_SESSION * sess = &FlexNetSessions[peer_idx];
    if (!sess->active || !sess->LINK) return;

    struct FLEXNET_ADVERTISED_STATE * st = &FlexNetAdvertised[peer_idx];

    int queued = 0;
    for (int i = 0; i < st->count; i++)
        if (st->advs[i].pending) queued++;
    if (queued == 0 && !st->eob_pending) return;

    BOOL   is_pcf     = flex_peer_is_pcf(sess);
    int    refill_s   = is_pcf ? FLEXNET_BUCKET_REFILL_PCF_S
                               : FLEXNET_BUCKET_REFILL_XNET_S;
    double bucket_max = is_pcf ? (double)FLEXNET_BUCKET_SIZE_PCF
                               : (double)FLEXNET_BUCKET_SIZE_XNET;

    time_t now = time(NULL);
    if (st->last_tokens_refill == 0)
    {
        /* First use: start full, so the init burst isn't held back. */
        st->tokens             = bucket_max;
        st->last_tokens_refill = now;
    }
    else if (now > st->last_tokens_refill)
    {
        st->tokens += (double)(now - st->last_tokens_refill)
                      / (double)refill_s;
        if (st->tokens > bucket_max) st->tokens = bucket_max;
        st->last_tokens_refill = now;
    }

    /* One token buys one FRAME, packed with as many queued records as
       the byte budget allows. See FLEXNET_ADVERT_FRAME_BYTES: this
       leaves the I-frame rate to the peer exactly where the bucket put
       it, and only stops wasting 94 % of every frame. */
    int max_recs = is_pcf ? FLEXNET_ADVERT_RECS_PCF
                          : FLEXNET_ADVERT_RECS_XNET;
    int emitted = 0;                     /* records on the wire */
    int frames  = 0;                     /* I-frames used for them */

    while (st->tokens >= 1.0)
    {
        unsigned char frame[FLEXNET_ADVERT_FRAME_BYTES];
        int flen     = 0;
        int in_frame = 0;

        frame[flen++] = '3';

        while (in_frame < max_recs)
        {
            struct FLEXNET_ADVERTISED_ROUTE * pick = NULL;
            for (int i = 0; i < st->count; i++)
                if (st->advs[i].pending) { pick = &st->advs[i]; break; }
            if (!pick) break;

            /* -1 reserves the terminating '\r'. */
            int rl = flex_build_route_rec(frame + flen,
                                          (int)sizeof(frame) - flen - 1,
                                          pick->dest_call, pick->ssid_lo,
                                          pick->ssid_hi, pick->pending_rtt);
            if (rl < 0)
            {
                /* Out of room. Send what we have and let the next token
                   carry this one; do NOT clear pending, or the record is
                   silently lost. */
                if (in_frame == 0) pick->pending = FALSE;  /* unencodable */
                break;
            }

            flen += rl;
            /* §5.3 step 4 — commit on emission, not on queueing, so
               repeated changes while the bucket is dry collapse into
               one record carrying the final value. */
            pick->last_advertised_rtt = pick->pending_rtt;
            pick->last_advertised_at  = now;
            pick->pending             = FALSE;
            in_frame++;
            emitted++;
        }

        if (in_frame == 0) break;        /* nothing left to send */

        frame[flen++] = '\r';
        flex_send_frame(sess->LINK, FLEXNET_PID_CE, frame, flen);
        frames++;
        st->tokens -= 1.0;
    }

    /* The '3-' owed after a `3+` walk goes out once the records it
       queued have all drained — end-of-batch has to mean it. */
    if (st->eob_pending)
    {
        BOOL still_queued = FALSE;
        for (int i = 0; i < st->count; i++)
            if (st->advs[i].pending) { still_queued = TRUE; break; }
        if (!still_queued)
        {
            unsigned char rel[3] = { '3', '-', '\r' };
            flex_send_frame(sess->LINK, FLEXNET_PID_CE, rel, 3);
            st->eob_pending = FALSE;
        }
    }

    /* Log only when a record actually went out. FlexNet_Timer ticks
       several times a second, so logging a non-empty queue would emit
       three lines a second per peer for the whole of a cold-start
       drain — which on a ~190-destination table is ~16 minutes to a
       PCF peer. The queue depth is still in the line, and `FL` shows
       it live. */
    if (FLEXNET_DEBUG && emitted > 0)
    {
        char peer[20] = {0};
        flex_sess_peer_call(sess, peer, sizeof(peer));
        FlexNet_Trace("FlexNet: BUCKET peer=%s tokens=%.2f queue=%d "
                     "emit=%d frames=%d family=%s",
                     peer, st->tokens, queued, emitted, frames,
                     is_pcf ? "PCF" : "xnet");
    }
}

/* Walk every route learned from OTHER sessions through the decision
 * rule for `peer_idx`. `direct_only` restricts the walk to direct
 * neighbours (the §5.5 keepalive); otherwise it is the full view (the
 * §5.6 `3+` response).
 */
static void flex_advertise_walk_for_peer(int peer_idx, BOOL direct_only,
                                         BOOL force, int * walked,
                                         int * queued)
{
    int n_walked = 0, n_queued = 0;

    for (int si = 0; si < FLEXNET_MAX_SESSIONS; si++)
    {
        if (si == peer_idx) continue;                    /* split-horizon */
        if (!FlexNetSessions[si].active) continue;

        struct FLEXNET_LEARNED_STATE * st = &FlexNetLearned[si];
        for (int ri = 0; ri < st->count; ri++)
        {
            struct FLEXNET_LEARNED_ROUTE * lr = &st->routes[ri];
            if (!lr->dest_call[0]) continue;
            if (direct_only && !lr->is_direct_neighbour) continue;

            struct FLEXNET_ADVERTISED_ROUTE * before =
                flex_adv_find(peer_idx, lr->dest_call,
                              lr->ssid_lo, lr->ssid_hi, FALSE);
            BOOL was_pending = (before && before->pending);

            n_walked++;
            flex_advertise_check(peer_idx, lr->dest_call,
                                 lr->ssid_lo, lr->ssid_hi, force);

            struct FLEXNET_ADVERTISED_ROUTE * after =
                flex_adv_find(peer_idx, lr->dest_call,
                              lr->ssid_lo, lr->ssid_hi, FALSE);
            if (after && after->pending && !was_pending) n_queued++;
        }
    }

    if (walked) *walked = n_walked;
    if (queued) *queued = n_queued;
}

/* Prune learned[] entries nobody has refreshed, and withdraw them.
 *
 * (X)Net does not send a withdrawal for a destination it has aged out
 * of its own table — it simply stops mentioning it. Without this pass
 * the entry lives until the session dies, so we keep advertising a
 * route our own source gave up on. Measured 2026-09-17: IW2OHX-14
 * silently dropped IR2UFX, our learned[] kept it, `FL` still showed a
 * path through -14 that -14 no longer had, and only a process restart
 * cleared it.
 *
 * FLEXNET_LEARNED_MAX_AGE must stay above xnet's own ageing window
 * (< 480 s observed) or we would withdraw routes a peer still holds.
 * Direct neighbours are never pruned: their entry is ours, added at
 * session init, and it goes away with the session via
 * flex_advertise_poison_session().
 */
static void flex_learned_age_scan(time_t now)
{
    if (!g_flexnet_transit_enabled) return;

    for (int si = 0; si < FLEXNET_MAX_SESSIONS; si++)
    {
        if (!FlexNetSessions[si].active) continue;
        struct FLEXNET_LEARNED_STATE * st = &FlexNetLearned[si];

        for (int ri = 0; ri < st->count; /* no ++ — see swap below */)
        {
            struct FLEXNET_LEARNED_ROUTE * lr = &st->routes[ri];

            if (lr->is_direct_neighbour || !lr->dest_call[0] ||
                (now - lr->last_heard) <= FLEXNET_LEARNED_MAX_AGE)
            {
                ri++;
                continue;
            }

            /* Copy the key before removing: the walk below needs it and
               the slot is about to be overwritten. */
            char stale_call[FLEXNET_MAX_CALLSIGN];
            int  stale_lo = lr->ssid_lo, stale_hi = lr->ssid_hi;
            long age      = (long)(now - lr->last_heard);
            strncpy(stale_call, lr->dest_call, sizeof(stale_call) - 1);
            stale_call[sizeof(stale_call) - 1] = '\0';

            /* Remove by swapping the last entry down, so the scan stays
               O(count). Do NOT advance ri — the swapped-in entry has
               not been examined yet. */
            st->count--;
            if (ri != st->count)
                st->routes[ri] = st->routes[st->count];
            memset(&st->routes[st->count], 0, sizeof(st->routes[0]));
            st->dirty = TRUE;

            if (FLEXNET_DEBUG)
            {
                char apeer[20] = {0};
                flex_sess_peer_call(&FlexNetSessions[si],
                                    apeer, sizeof(apeer));
                FlexNet_Trace("FlexNet: LEARNED-AGE from=%s dest=%s-%d/%d "
                              "age=%lds — pruned",
                              apeer, stale_call, stale_lo, stale_hi, age);
            }

            /* Removal is a change event: with this source gone the
               expected RTT for every other peer has moved, possibly to
               infinity. Runs after the removal so flex_expected_rtt
               cannot still see the entry we just dropped. */
            for (int pi = 0; pi < FLEXNET_MAX_SESSIONS; pi++)
            {
                if (pi == si) continue;
                if (!FlexNetSessions[pi].active) continue;
                flex_advertise_check(pi, stale_call, stale_lo, stale_hi,
                                     FALSE);
                flex_advertise_drain(pi);
            }
        }
    }
}

/* RFC §5.7 — poison-reverse for a session that has just gone away.
 *
 * For everything we learned from the dead peer, ask whether any
 * surviving session still offers it. If one does, stay quiet: that
 * session's next change event re-advertises the better path naturally.
 * If none does, flex_advertise_check now computes RTT=60000 and queues
 * the withdrawal to every other peer, which is what stops them
 * black-holing traffic through us.
 *
 * The dead peer's OWN entry is included, though §5.7 step 1 says to
 * skip direct neighbours: it is advertised to the others as
 * learned_rtt + link_rtt like any destination, so leaving it out would
 * leave them routing to a dead node through us.
 *
 * THE CALLER MUST HAVE CLEARED sess->active FIRST. flex_expected_rtt
 * only counts active sessions as sources, and that is exactly what
 * makes the alternate-path check below see the network as it is now
 * rather than as it was.
 *
 * Called from both paths a session can die on, which is not obvious
 * and is why this is a function: FlexNet_CloseSession (an explicit
 * DISC) and FlexNet_Timer's ghost reaper. On the live network the
 * reaper is the common one by a wide margin — a 25-minute IR2UFV
 * window on 2026-09-17 saw IW2OHX-4 cycle with no CloseSession call at
 * all, so hooking only CloseSession left G4 as dead code.
 */
static void flex_advertise_poison_session(int dead_idx, const char * dead_call)
{
    if (!g_flexnet_transit_enabled) return;
    if (dead_idx < 0 || dead_idx >= FLEXNET_MAX_SESSIONS) return;

    struct FLEXNET_LEARNED_STATE * dst = &FlexNetLearned[dead_idx];
    int poisoned = 0, covered = 0;

    for (int ri = 0; ri < dst->count; ri++)
    {
        struct FLEXNET_LEARNED_ROUTE * lr = &dst->routes[ri];
        if (!lr->dest_call[0]) continue;

        int alt_idx = -1;
        (void)flex_expected_rtt(-1, lr->dest_call, lr->ssid_lo,
                                lr->ssid_hi, &alt_idx, NULL);
        if (alt_idx >= 0) covered++; else poisoned++;

        if (FLEXNET_DEBUG)
        {
            char alt_call[20] = {0};
            if (alt_idx >= 0)
                flex_sess_peer_call(&FlexNetSessions[alt_idx],
                                    alt_call, sizeof(alt_call));
            FlexNet_Trace("FlexNet: POISON peer-down=%s dest=%s-%d/%d alt=%s",
                          dead_call && dead_call[0] ? dead_call : "?",
                          lr->dest_call, lr->ssid_lo, lr->ssid_hi,
                          alt_idx >= 0 ? alt_call : "(none, poisoning)");
        }

        for (int pi = 0; pi < FLEXNET_MAX_SESSIONS; pi++)
        {
            if (pi == dead_idx) continue;
            if (!FlexNetSessions[pi].active) continue;
            flex_advertise_check(pi, lr->dest_call,
                                 lr->ssid_lo, lr->ssid_hi, FALSE);
        }
    }

    for (int pi = 0; pi < FLEXNET_MAX_SESSIONS; pi++)
        if (FlexNetSessions[pi].active) flex_advertise_drain(pi);

    if (dst->count > 0)
        FlexNet_Info("FlexNet: peer %s down — %d learned routes, "
                     "%d withdrawn, %d covered by another peer",
                     dead_call && dead_call[0] ? dead_call : "?",
                     dst->count, poisoned, covered);

    /* advertised[] is cleared so that if the peer comes back we
       re-advertise from scratch rather than assuming it still remembers
       what we told it.
     *
     * learned[] is deliberately KEPT. Clearing it here is what made
     * flex_learned_adopt() unreachable: this function runs on both
     * death paths, so by the time a returning peer reached InitSession
     * there was never anything left to adopt, and every reconnect
     * rebuilt the table and re-advertised it. The adoption diagnostic
     * showed it plainly — the fresh slot was absent from the dump
     * because its table was already zeroed.
     *
     * Keeping it is safe: flex_expected_rtt() only counts ACTIVE
     * sessions as sources, so a dead peer's routes cannot be used for
     * routing or advertised onward; flex_learned_age_scan() prunes them
     * on age; the array is fixed-size so nothing grows; and a DIFFERENT
     * peer landing in this slot gets a clean table, because adoption
     * matches on peer_call and InitSession memsets when it fails.
     *
     * died_at is stamped here too. Both callers already do it, but this
     * is the one point both of them pass through, so a future third
     * caller cannot forget. */
    dst->died_at = time(NULL);
    memset(&FlexNetAdvertised[dead_idx], 0, sizeof(FlexNetAdvertised[0]));
}

/* Seed a peer that has just come up with our full transit view.
 *
 * Not in the §5 text, and it is not optional. Trigger (a) fires on
 * *change*, so a peer that joins a node whose table has already
 * converged sees nothing: on IR2UFV 2026-09-17 the PCF peer came up
 * after the two xnet sessions had populated ~190 destinations and its
 * advertised[] sat at 2 — the two direct neighbours — while the other
 * peers had 119 and 126. A peer that never sends `3+` (PC/Flexnet does
 * not) would stay at that forever, so transit toward it would be
 * silently dead. Test B2 only reads as "self + direct neighbours"
 * because it assumes a cold start, where the full view IS the direct
 * set.
 *
 * force=FALSE deliberately: a fresh session's advertised[] is empty, so
 * every entry fires on the never-advertised sentinel anyway, and
 * leaving the jitter rule in play keeps one decision path. The queue
 * drains at the peer's family rate — ~16 min for a 190-entry table to
 * a PCF peer — which is the bucket doing its job, not a burst.
 */
static void flex_advertise_seed_peer(int peer_idx)
{
    if (!g_flexnet_transit_enabled) return;
    if (peer_idx < 0 || peer_idx >= FLEXNET_MAX_SESSIONS) return;

    int walked = 0, queued = 0;
    flex_advertise_walk_for_peer(peer_idx, FALSE, FALSE, &walked, &queued);
    if (queued > 0) flex_advertise_drain(peer_idx);

    /* Anchor the 120 s timer — this walk covered the direct set too. */
    FlexNetLearned[peer_idx].last_advert = time(NULL);

    if (FLEXNET_DEBUG)
    {
        char peer[20] = {0};
        flex_sess_peer_call(&FlexNetSessions[peer_idx], peer, sizeof(peer));
        FlexNet_Trace("FlexNet: SEED peer=%s entries=%d queued=%d",
                     peer, walked, queued);
    }
}

/* Re-offer the direct-neighbour set to one peer (RFC §5.5 / test B2).
 * Used both for a peer that has just come up — which otherwise learns
 * nothing from us until something changes — and for the 120 s
 * keepalive that stops xnet ageing those entries out between changes.
 * force=TRUE bypasses only the jitter floor; the records still ride
 * the peer's token bucket, so this cannot burst however often it runs.
 */
static void flex_advertise_neighbours(int peer_idx)
{
    if (!g_flexnet_transit_enabled) return;
    if (peer_idx < 0 || peer_idx >= FLEXNET_MAX_SESSIONS) return;

    int walked = 0, queued = 0;
    flex_advertise_walk_for_peer(peer_idx, TRUE, TRUE, &walked, &queued);
    if (walked > 0) flex_advertise_drain(peer_idx);

    /* Anchor the 120 s timer here, not only in FlexNet_Timer. Every
       path that refreshes the direct set has now done so, and
       last_advert starting at the epoch would otherwise make the
       timer's first tick fire a duplicate refresh seconds after the
       session-init one — observed on IR2UFV 2026-09-17. (rc2 kept
       this assignment inside flex_send_own_routes for the same
       reason; it went with the cap+cursor block.) */
    FlexNetLearned[peer_idx].last_advert = time(NULL);

    if (FLEXNET_DEBUG && walked > 0)
    {
        char peer[20] = {0};
        flex_sess_peer_call(&FlexNetSessions[peer_idx], peer, sizeof(peer));
        FlexNet_Trace("FlexNet: NBR-REFRESH peer=%s direct=%d queued=%d",
                     peer, walked, queued);
    }
}

static void flex_send_own_routes(LINKTABLE * LINK, BOOL defer_eob)
{
    /* v2.1.6: removed the leading "3+\r" emit. Per the protocol
       spec §2.6 and confirmed by direct observation of the
       xnet-14 ↔ IW2OHX-12 (PC/Flexnet) link, "3+\r" means
       "PLEASE SEND ME YOUR ROUTES" — a REQUEST sent to the peer.
       Sending it as a preamble to our OWN record stream is a
       protocol violation: we're asking the peer for routes while
       immediately dumping ours, which PC/Flexnet treats as a
       malformed exchange and DISCs the session. xnet emits
       compact records spontaneously (no leading token) and replies
       with "3-\r" only when the peer's "3+\r" request arrives.
       The trailing "3-\r" below is kept — it correctly signals
       "end of OUR batch" per the spec. */

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

    /* v1.10.0: advertise the configured SSID range if set, otherwise
       fall back to the node's own SSID only (pre-v1.10.0 behaviour).
       The range must include `my_ssid` so the node itself remains
       reachable on the FlexNet cloud — clamp accordingly if the
       operator's config left it out. */
    int ssid_lo = my_ssid, ssid_hi = my_ssid;
    if (g_flexnet_ssid_lo >= 0)
    {
        ssid_lo = g_flexnet_ssid_lo;
        ssid_hi = g_flexnet_ssid_hi;
        if (my_ssid < ssid_lo) ssid_lo = my_ssid;
        if (my_ssid > ssid_hi) ssid_hi = my_ssid;
    }

    /* Single compact record carrying the configured (ssid_lo,ssid_hi)
       range. xnet and other FlexNet implementations display the
       range as e.g. "IR2UFV 0-8" when the wire record encodes both
       SSID bytes distinctly. */
    if (mycall[0])
    {
        unsigned char route[32];
        int rlen = flex_build_route(route, sizeof(route),
                                    mycall, ssid_lo, ssid_hi, 1);
        if (rlen > 0)
        {
            /* Debug: log the exact bytes we're about to put on the
               wire so we can compare with what xnet actually parses. */
            char hex[3*32+1] = {0};
            char asc[32+1]   = {0};
            int n = rlen > 32 ? 32 : rlen;
            for (int i = 0; i < n; i++) {
                snprintf(&hex[i*3], 4, "%02X ", route[i]);
                asc[i] = (route[i] >= 0x20 && route[i] < 0x7F) ? route[i] : '.';
            }
            FlexNet_Log("ROUTE-TX: len=%d hex=[%s] ascii=[%s]", rlen, hex, asc);
            flex_send_frame(LINK, FLEXNET_PID_CE, route, rlen);
        }
    }

    /* v2.2 rc4 — transit re-advertisement no longer happens here.
       This function emits our OWN record only; learned routes are
       emitted by flex_advertise_check/flex_advertise_drain as table
       mutations happen (RFC §5). The cap + rotating-cursor block that
       used to sit here is what rc1-rc3 failed on three times: see
       RFC §16/§17 before re-introducing anything clock-driven. */

    /* Release token. Deferred for a `3+` response — there the '3-'
       is queued behind the transit records, so end-of-batch actually
       means it (see flex_advertise_drain). */
    if (!defer_eob)
    {
        unsigned char rel[] = { '3', '-', '\r' };
        flex_send_frame(LINK, FLEXNET_PID_CE, rel, 3);
    }

    /* Decode neighbor for logging */
    char nbr[20] = {0};
    ConvFromAX25(LINK->LINKCALL, nbr);
    slen = strlen(nbr);
    while (slen > 0 && nbr[slen - 1] == ' ') nbr[--slen] = '\0';

    FlexNet_Info("FlexNet: advertising %s (%d-%d) RTT=1 to %s",
                mycall, ssid_lo, ssid_hi, nbr);
}

/* ── FlexNet L2 forwarding (post-GA milestone) ───────────────────────── */
/*
 * Symmetric digi-chain rewriting: what the real routers do, captured on
 * PC/Flexnet IW2OHX-12 on 2026-09-17 while it forwarded a user session
 * from IW2OHX-4 to IGATE two hops beyond it —
 *
 *   in   IW7EAS-2->IGATE   IW2OHX-4* IW2OHX-12                SABM
 *   out  IW7EAS-2->IGATE   IW2OHX-4* IW2OHX-12* IW2OHX-14     SABM
 *   in   IGATE->IW7EAS-2   IW2OHX-14* IW2OHX-12 IW2OHX-4      UA
 *   out  IGATE->IW7EAS-2   IW2OHX-12* IW2OHX-4                UA
 *
 * Forward: set our H-bit, APPEND the next hop as a new unrepeated digi.
 * Reverse: REMOVE the entry we appended, set our H-bit. The originator
 * only ever sees the chain it sent, so AX.25 V2's "the UA's digi list is
 * the reverse of the SABM's" invariant holds.
 *
 * PROTOCOL_SPEC §5.1 used to call this illegal, which is why it was
 * never built: extension alone does break V2, and v1.9.4 did only the
 * extension. Contraction is the other half. See §5.2 and
 * research/l2_forwarding_2026-09-17/.
 *
 * The reverse path is the dangerous part. "The digi I appended" and "a
 * digi the originator supplied" are indistinguishable by inspection, so
 * removing one on a guess would corrupt a stranger's session rather than
 * ours. Hence FlexNetL2Transit[] below: we only ever remove a hop we
 * recorded appending, for that exact (user, dest, port) triple.
 */

/* FLEXNET_L2_MAX_DIGIS is defined with the path-cache constants near the
   top: the PATH_REQ answer needs the same ceiling, and one definition
   cannot drift from the other. */
#define FLEXNET_MAX_L2_TRANSIT     64
#define FLEXNET_L2_TRANSIT_IDLE   900    /* s before a slot is reusable */
/* Frame bytes from DEST onward that we are willing to grow into. The
   buffer is BUFFLEN with the trailing bookkeeping fields at the end, so
   this stays well clear rather than computing to the byte. */
#define FLEXNET_L2_MAX_FRAME      330

struct FLEXNET_L2_TRANSIT
{
    BOOL    active;
    UCHAR   user[7];        /* ORIGIN of the forward frame */
    UCHAR   dest[7];        /* DEST of the forward frame   */
    UCHAR   appended[7];    /* the next-hop digi WE added  */
    int     port;
    time_t  last_used;
};

static struct FLEXNET_L2_TRANSIT FlexNetL2Transit[FLEXNET_MAX_L2_TRANSIT];


/* Compare two AX.25 addresses ignoring the H (repeated) and E (end)
   bits, which change as a frame travels. */
static BOOL flex_l2_same_call(const UCHAR * a, const UCHAR * b)
{
    if (memcmp(a, b, 6) != 0) return FALSE;
    return ((a[6] & 0x1E) == (b[6] & 0x1E));
}

/* Last address entry (the one with the E bit set), or NULL if the
   address field is malformed. */
static UCHAR * flex_l2_addr_last(MESSAGE * Buffer)
{
    UCHAR * a = (UCHAR *)Buffer->DEST;
    for (int n = 0; n < 2 + FLEXNET_L2_MAX_DIGIS; n++)
    {
        if (a[6] & 0x01) return a;
        a += 7;
    }
    return NULL;
}

static int flex_l2_digi_count(MESSAGE * Buffer)
{
    UCHAR * base = (UCHAR *)Buffer->DEST;
    if (base[13] & 0x01) return 0;          /* E bit on ORIGIN: no digis */
    UCHAR * d = base + 14;
    int n = 0;
    while (n < FLEXNET_L2_MAX_DIGIS)
    {
        n++;
        if (d[6] & 0x01) break;
        d += 7;
    }
    return n;
}

static BOOL flex_l2_call_in_chain(MESSAGE * Buffer, const UCHAR * call)
{
    UCHAR * base = (UCHAR *)Buffer->DEST;
    if (flex_l2_same_call(base, call) || flex_l2_same_call(base + 7, call))
        return TRUE;
    if (base[13] & 0x01) return FALSE;
    UCHAR * d = base + 14;
    for (int n = 0; n < FLEXNET_L2_MAX_DIGIS; n++)
    {
        if (flex_l2_same_call(d, call)) return TRUE;
        if (d[6] & 0x01) break;
        d += 7;
    }
    return FALSE;
}

/* Insert `call` as a new final digi: unrepeated, E bit set. The tail
   (control, PID, info) shifts up by 7. */
static BOOL flex_l2_append_digi(MESSAGE * Buffer, const UCHAR * call)
{
    UCHAR * base = (UCHAR *)Buffer->DEST;
    UCHAR * last = flex_l2_addr_last(Buffer);
    if (!last) return FALSE;

    int flen     = (int)Buffer->LENGTH - MSGHDDRLEN;   /* DEST onward */
    int addr_len = (int)(last + 7 - base);
    if (flen < addr_len || flen + 7 > FLEXNET_L2_MAX_FRAME) return FALSE;

    memmove(base + addr_len + 7, base + addr_len,
            (size_t)(flen - addr_len));
    last[6] &= (UCHAR)~0x01;                       /* no longer the end */
    memcpy(base + addr_len, call, 6);
    /* Keep SSID and the two reserved bits, clear H, set E. */
    base[addr_len + 6] = (UCHAR)((call[6] & 0x7E) | 0x01);
    Buffer->LENGTH = (USHORT)(Buffer->LENGTH + 7);
    return TRUE;
}

/* Remove one digi entry; the tail shifts down by 7. */
static BOOL flex_l2_remove_digi(MESSAGE * Buffer, UCHAR * ent)
{
    UCHAR * base = (UCHAR *)Buffer->DEST;
    int flen = (int)Buffer->LENGTH - MSGHDDRLEN;
    int off  = (int)(ent - base);

    if (off < 14 || off + 7 > flen) return FALSE;
    BOOL was_last = (ent[6] & 0x01) != 0;

    memmove(ent, ent + 7, (size_t)(flen - off - 7));
    Buffer->LENGTH = (USHORT)(Buffer->LENGTH - 7);

    if (was_last && off >= 21)          /* a digi still precedes it */
        ent[-1] |= 0x01;                /* byte 6 of the previous entry */
    else if (was_last)
        base[13] |= 0x01;               /* no digis left: end on ORIGIN */
    return TRUE;
}

static struct FLEXNET_L2_TRANSIT *
flex_l2_find(const UCHAR * user, const UCHAR * dest, int port, BOOL create)
{
    struct FLEXNET_L2_TRANSIT * spare = NULL;
    time_t now = time(NULL);

    for (int i = 0; i < FLEXNET_MAX_L2_TRANSIT; i++)
    {
        struct FLEXNET_L2_TRANSIT * e = &FlexNetL2Transit[i];
        if (e->active && (now - e->last_used) > FLEXNET_L2_TRANSIT_IDLE)
            e->active = FALSE;
        if (e->active && e->port == port &&
            flex_l2_same_call(e->user, user) &&
            flex_l2_same_call(e->dest, dest))
            return e;
        if (!e->active && !spare) spare = e;
    }
    if (!create || !spare) return NULL;

    memset(spare, 0, sizeof(*spare));
    spare->active = TRUE;
    memcpy(spare->user, user, 7);
    memcpy(spare->dest, dest, 7);
    spare->port = port;
    spare->last_used = now;
    return spare;
}

/* Map a normalised neighbour callsign to an active session index, or -1.
 *
 * A destination restored from the on-disk cache — or simply not yet
 * refreshed by a CE compact batch — carries via_session_idx = -1 while
 * its via_callsign is perfectly good. L2 forwarding used to decline
 * those outright, so transit was unavailable for the whole
 * post-restart window: 55 `no live session` declines landed during the
 * 21:14 DB0ALG failure on 2026-09-17, alongside the over-long path
 * reply. Two independent causes, one symptom, and fixing only the
 * visible one would have left this in place.
 *
 * Resolving by callsign lets the index heal itself on first use. */
static int flex_session_for_call(const char * call)
{
    if (!call || !call[0]) return -1;

    for (int i = 0; i < FLEXNET_MAX_SESSIONS; i++)
    {
        if (!FlexNetSessions[i].active) continue;
        if (!FlexNetSessions[i].peer_callsign[0]) continue;

        /* char[20]: ConvFromAX25 writes more than 10 bytes, and sizing
           this by FLEXNET_MAX_CALLSIGN has overflowed here before. */
        char peer[20] = {0};
        ConvFromAX25((unsigned char *)FlexNetSessions[i].peer_callsign,
                     (unsigned char *)peer);
        int sl = (int)strlen(peer);
        while (sl > 0 && peer[sl - 1] == ' ') peer[--sl] = '\0';

        if (strcasecmp(peer, call) == 0) return i;
    }
    return -1;
}

/*
 * Rewrite the digi chain of a frame that lists us as the next digi, so a
 * destination which is NOT adjacent to us can still be reached.
 *
 * Returns the (possibly moved) pointer to our own digi entry, which the
 * caller passes to Digipeat() — Digipeat sets the H bit and transmits.
 * Returns the pointer unchanged when we decline, so the caller's normal
 * digipeat behaviour is preserved and the 1-hop case is untouched.
 * Returns NULL only when the frame must be dropped.
 */
UCHAR * FlexNet_L2Transit(struct PORTCONTROL * PORT, MESSAGE * Buffer,
                          UCHAR * ourdigi)
{
    if (!g_flexnet_transit_enabled || !g_flexnet_l2_transit_enabled)
        return ourdigi;
    if (!PORT || !Buffer || !ourdigi) return ourdigi;

    UCHAR * base = (UCHAR *)Buffer->DEST;
    int our_off  = (int)(ourdigi - base);
    if (our_off < 14 || ((our_off - 14) % 7) != 0) return ourdigi;

    /* Only ever on a port carrying FlexNet peers. */
    if (!FlexNet_IsPeerFlexNetMapped(Buffer->ORIGIN, PORT->PORTNUMBER) &&
        flex_l2_digi_count(Buffer) < 2)
        return ourdigi;

    char dest_s[20] = {0}, user_s[20] = {0};
    flex_normalize_callsign(base,     dest_s, sizeof(dest_s));
    flex_normalize_callsign(base + 7, user_s, sizeof(user_s));

    /* ── reverse path ────────────────────────────────────────────────
       The hop we appended comes back as the digi immediately BEFORE us
       in the reversed chain, already marked repeated. Only remove it if
       our own table says we put it there for this exact conversation —
       an originator-supplied digi looks identical here, and removing one
       of those would break a session that is nothing to do with us. */
    if (our_off >= 21)
    {
        UCHAR * prev = ourdigi - 7;
        struct FLEXNET_L2_TRANSIT * e =
            flex_l2_find(base, base + 7, PORT->PORTNUMBER, FALSE);
        if (e && (prev[6] & 0x80) && flex_l2_same_call(prev, e->appended))
        {
            if (flex_l2_remove_digi(Buffer, prev))
            {
                e->last_used = time(NULL);
                g_l2_fwd_contracted++;
                FlexNet_Log("L2FWD-CONTRACT: %s->%s removed %s "
                            "(port %d, digis now %d)",
                            user_s, dest_s, "appended-hop",
                            PORT->PORTNUMBER, flex_l2_digi_count(Buffer));
                return ourdigi - 7;        /* our entry moved down */
            }
        }
        /* Not ours to touch — fall through; a plain digipeat is correct
           when the originator built the whole chain itself. */
    }

    /* ── forward path ────────────────────────────────────────────────
       Append the next hop toward DEST, but only if DEST is genuinely
       beyond us. If it is adjacent, plain digipeating already works and
       is what the captures show the real routers doing. */
    if (ourdigi[6] & 0x01)                 /* we are the last digi */
    {
        /* Every decline below says why. A silent decline here is
           indistinguishable from the hook not running at all, which
           cost a test cycle to work out the first time. */
        int di = flex_find_dest_for_target(dest_s);
        if (di < 0)
        {
            g_l2_fwd_declined++;
            FlexNet_Log("L2FWD-DECLINE: %s->%s dest not in our table",
                        user_s, dest_s);
            return ourdigi;
        }

        int via = FlexNetDests[di].via_session_idx;
        if (via < 0 || via >= FLEXNET_MAX_SESSIONS ||
            !FlexNetSessions[via].active)
        {
            /* Heal an unresolved index from via_callsign instead of
               declining — see flex_session_for_call(). */
            int healed = flex_session_for_call(FlexNetDests[di].via_callsign);
            if (healed >= 0)
            {
                FlexNetDests[di].via_session_idx = healed;
                FlexNet_Log("L2FWD-HEAL: %s->%s via_session_idx %d -> %d "
                            "(resolved from via=%s)", user_s, dest_s, via,
                            healed, FlexNetDests[di].via_callsign);
                via = healed;
            }
        }

        if (via < 0 || via >= FLEXNET_MAX_SESSIONS ||
            !FlexNetSessions[via].active)
        {
            g_l2_fwd_declined++;
            FlexNet_Log("L2FWD-DECLINE: %s->%s no live session for it "
                        "(via_session_idx=%d via=%s)", user_s, dest_s, via,
                        FlexNetDests[di].via_callsign[0]
                            ? FlexNetDests[di].via_callsign : "?");
            return ourdigi;
        }

        UCHAR * nexthop = (UCHAR *)FlexNetSessions[via].peer_callsign;
        if (!nexthop[0])
        {
            g_l2_fwd_declined++;
            FlexNet_Log("L2FWD-DECLINE: %s->%s session %d has no stashed "
                        "peer callsign", user_s, dest_s, via);
            return ourdigi;
        }

        /* Adjacent: the next hop IS the destination, so there is nothing
           to add and stock digipeating delivers it. */
        if (flex_l2_same_call(nexthop, base))
        {
            FlexNet_Log("L2FWD-ADJACENT: %s->%s is our own neighbour — "
                        "plain digipeat is correct here", user_s, dest_s);
            return ourdigi;
        }

        /* Loop guard, and it doubles as split-horizon: if the next hop is
           already in the chain the frame has been there. */
        if (flex_l2_call_in_chain(Buffer, nexthop))
        {
            g_l2_fwd_declined++;
            FlexNet_Log("L2FWD-DECLINE: %s->%s next hop already in chain",
                        user_s, dest_s);
            return ourdigi;
        }

        int ndigis = flex_l2_digi_count(Buffer);
        int cap = PORT->PORTMAXDIGIS ? PORT->PORTMAXDIGIS
                                     : FLEXNET_L2_MAX_DIGIS;
        if (cap > FLEXNET_L2_MAX_DIGIS) cap = FLEXNET_L2_MAX_DIGIS;
        if (ndigis >= cap)
        {
            g_l2_fwd_declined++;
            FlexNet_Log("L2FWD-DECLINE: %s->%s digi chain full (%d/%d) — "
                        "this is FlexNet's hop limit, AX.25 has no TTL",
                        user_s, dest_s, ndigis, cap);
            return ourdigi;
        }

        struct FLEXNET_L2_TRANSIT * e =
            flex_l2_find(base + 7, base, PORT->PORTNUMBER, TRUE);
        if (!e)
        {
            g_l2_fwd_declined++;
            FlexNet_Log("L2FWD-DECLINE: %s->%s transit table full",
                        user_s, dest_s);
            return ourdigi;
        }

        if (!flex_l2_append_digi(Buffer, nexthop))
        {
            e->active = FALSE;
            g_l2_fwd_declined++;
            FlexNet_Log("L2FWD-DECLINE: %s->%s append failed (len %d)",
                        user_s, dest_s, (int)Buffer->LENGTH);
            return ourdigi;
        }

        memcpy(e->appended, nexthop, 7);
        e->last_used = time(NULL);
        g_l2_fwd_extended++;

        char nh_s[20] = {0};
        flex_normalize_callsign(nexthop, nh_s, sizeof(nh_s));
        FlexNet_Info("FlexNet: L2FWD %s->%s via %s (appended, digis now %d)",
                     user_s, dest_s, nh_s, flex_l2_digi_count(Buffer));
    }

    return ourdigi;
}

/* ── PCF L2-Cycle Adoption Hook ──────────────────────────────────────── */
/*
 * v2.1.25 — adopt an existing FlexNet session onto a new LINKTABLE
 * pointer on the SABM-accept path.
 *
 * Wire evidence (pcap on iw2ohx-gw, 2026-05-29/30) shows PC/Flexnet
 * IW2OHX-12 actively DISC's the L2 link on a ~2 h interval, then
 * immediately re-SABMs with a fresh CID. The DISC causes BPQ to
 * CLEAROUTLINK the LINKTABLE entry; the SABM then triggers BPQ to
 * allocate a fresh LINKTABLE slot, hit the L2Code.c FlexNet
 * SABM-accept hook, and call FlexNet_InitSession() which sends a
 * fresh CE-INIT. PC/Flexnet treats every received INIT as "this
 * is a new peer, reseed the link-cost ring" — and we get the
 * `600 4095 …` outliers in PCF's L * row that v2.1.13 takes ~5
 * min to converge back.
 *
 * This function lets the SABM-accept hook recognise the cycle and
 * just MIGRATE the existing FlexNetSessions[] entry to the new
 * LINK pointer, preserving `got_peer_init` / `sent_routes` /
 * `peer_ka_term` / `peer_max_ssid`. No fresh INIT means PC/Flexnet
 * doesn't reseed.
 *
 * Returns TRUE if a session was adopted (caller should skip
 * FlexNet_InitSession), FALSE if no matching session exists and
 * the caller should fall through to the normal new-session path.
 */
BOOL FlexNet_TryAdoptSession(struct _LINKTABLE * new_link, int bpq_port)
{
    if (!new_link) return FALSE;
    if (new_link->LINKCALL[0] == 0) return FALSE;

    char new_call_str[12];
    flex_normalize_callsign(new_link->LINKCALL, new_call_str,
                            sizeof(new_call_str));
    if (new_call_str[0] == 0) return FALSE;

    for (int i = 0; i < FLEXNET_MAX_SESSIONS; i++)
    {
        struct FLEXNET_SESSION * sess = &FlexNetSessions[i];
        if (!sess->active) continue;
        if (sess->port != bpq_port) continue;
        if (sess->peer_callsign[0] == 0) continue;
        /* v2.1.26 — do NOT skip when sess->LINK == new_link. BPQ's
           CLEAROUTLINK zeroes the LINKTABLE entry in place but does not
           free the memory slot, so the next SABM often re-allocates the
           SAME slot (same pointer). Our session is the right one to
           adopt — the migrate code below is a no-op in the same-pointer
           case (FlexNetLink toggles FALSE→TRUE; peer_callsign overwrite
           is harmless) — and skipping it forced new-slot path + fresh
           INIT, which is exactly the cost-ring reseed we're fixing. */

        char existing_str[12];
        flex_normalize_callsign(sess->peer_callsign, existing_str,
                                sizeof(existing_str));
        if (existing_str[0] == 0) continue;
        if (strcmp(existing_str, new_call_str) != 0) continue;

        /* Match. Migrate the session to the new LINK pointer without
           re-INITing. Demote the old LINK's FlexNetLink flag only if
           it's a different LINKTABLE entry — otherwise we'd clobber
           the flag the SABM-accept caller just set. */
        if (sess->LINK && sess->LINK != new_link)
            sess->LINK->FlexNetLink = FALSE;
        BOOL same_slot = (sess->LINK == new_link);
        sess->LINK = new_link;
        new_link->FlexNetLink = TRUE;
        sess->reap_strikes = 0;
        memcpy(sess->peer_callsign, new_link->LINKCALL, 7);

        FlexNet_Info("FlexNet: adopted existing session slot %d for %s "
                      "on port %d (PCF L2-cycle continuation, %s, no fresh INIT)",
                      i, new_call_str, bpq_port,
                      same_slot ? "BPQ reused LINKTABLE slot"
                                : "new LINKTABLE slot");
        return TRUE;
    }

    return FALSE;
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
    FlexNet_Info("FlexNet: accepting incoming L2 to %s on port %d",
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

    if (FLEXNET_DEBUG) FlexNet_Info("FlexNet: FindRoute called for '%s' base='%s' "
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
        FlexNet_Info("FlexNet: routing C %s via FlexNet port %d "
                      "(RTT=%d, via session %d)",
                      target, e->port, e->rtt, e->via_session_idx);
        return e->port;
    }

    if (FLEXNET_DEBUG) FlexNet_Info("FlexNet: FindRoute — '%s' not found in dest table",
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
