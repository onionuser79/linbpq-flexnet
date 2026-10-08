/* v2.4 per-link routing options (the F suffix of an AXUDP MAP entry).
 *
 * Pins the places the feature can half-work:
 *   - parsing: every option, combinations, and an unknown character
 *     failing cleanly so the link falls back to default policy;
 *   - the source filter: '-', '!', '>' and '=' remove a link as a source
 *     in flex_expected_rtt() — and a destination ALSO reachable over an
 *     unrestricted link must still be offered, at that link's cost;
 *   - the neighbour test: "the neighbour itself" must match the peer's
 *     own range record, not only the entry InitSession flagged;
 *   - '+': added once, never to the withdrawal sentinel or the RTT=0
 *     marker, and reported so the climb guard can judge the path;
 *   - the climb guard: a failover onto a penalised link is one honest
 *     re-route, and must not be latched as a loop.
 *
 * Extracted verbatim from FlexNetCode.c by tools/unit/extract.sh.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <time.h>

#define FLEXNET_MAX_CALLSIGN               10
#define FLEXNET_RTT_INFINITY               60000
#define FLEXNET_MAX_SESSIONS               8
#define FLEXNET_MAX_LEARNED_PER_NEIGHBOUR  256
#define FLEXNET_CLIMB_RATIO                4
#define FLEXNET_CLIMB_MIN_STEPS            3
#define FLEX_CLIMB_OK                      0
#define FLEX_CLIMB_WITHDRAW                1
#define FLEX_CLIMB_SUPPRESS                2

typedef int BOOL;
typedef unsigned char UCHAR;
#define TRUE  1
#define FALSE 0

typedef struct { UCHAR LINKCALL[7]; } LINKTABLE;

/* Only the fields the extracted code touches. */
struct FLEXNET_SESSION
{
    LINKTABLE * LINK;
    BOOL active;
    int  our_link_time;
    int  port;
};
static struct FLEXNET_SESSION FlexNetSessions[FLEXNET_MAX_SESSIONS];
static LINKTABLE links[FLEXNET_MAX_SESSIONS];

/* BPQ's ConvFromAX25 writes "CALL-N", omitting "-0". */
static int ConvFromAX25(UCHAR * in, UCHAR * out)
{
    char * o = (char *)out;
    int n = 0;
    for (int i = 0; i < 6; i++)
    {
        char c = (char)(in[i] >> 1);
        if (c != ' ') o[n++] = c;
    }
    int ssid = (in[6] >> 1) & 0x0F;
    if (ssid) n += snprintf(o + n, 8, "-%d", ssid);
    o[n] = '\0';
    return n;
}

#include "extracted_link_opts.inc"

static int failures = 0, checks = 0;

static void ok(int cond, const char * what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static void ax25(UCHAR * out, const char * call, int ssid)
{
    size_t n = strlen(call);
    for (size_t i = 0; i < 6; i++)
        out[i] = (UCHAR)((i < n ? call[i] : ' ') << 1);
    out[6] = (UCHAR)(0x60 | (ssid << 1));
}

static void add_session(int si, const char * call, int ssid)
{
    ax25(links[si].LINKCALL, call, ssid);
    FlexNetSessions[si].LINK = &links[si];
    FlexNetSessions[si].active = TRUE;
    FlexNetSessions[si].our_link_time = 2;
}

static void learn(int si, const char * call, int lo, int hi, int rtt,
                  BOOL direct)
{
    struct FLEXNET_LEARNED_STATE * st = &FlexNetLearned[si];
    struct FLEXNET_LEARNED_ROUTE * r = &st->routes[st->count++];
    memset(r, 0, sizeof(*r));
    snprintf(r->dest_call, sizeof(r->dest_call), "%s", call);
    r->ssid_lo = lo;
    r->ssid_hi = hi;
    r->rtt_at_neighbour = rtt;
    r->is_direct_neighbour = direct;
}

/* Session 0 = NODEA-2, the peer we advertise TO.
   Session 1 = NODEB, direct, with DEST behind it at 10.
   Session 2 = NODEC-1, direct, with DEST behind it at 30.
   Every link time is 2, so DEST costs 12 via NODEB and 32 via NODEC. */
static void reset(void)
{
    memset(FlexNetSessions, 0, sizeof(FlexNetSessions));
    memset(FlexNetLearned, 0, sizeof(FlexNetLearned));
    memset(g_link_opts, 0, sizeof(g_link_opts));
    add_session(0, "NODEA", 2);
    add_session(1, "NODEB", 0);
    add_session(2, "NODEC", 1);
    learn(1, "NODEB", 0, 0, 1, TRUE);
    learn(1, "DEST", 0, 0, 10, FALSE);
    learn(2, "NODEC", 1, 1, 1, TRUE);
    learn(2, "DEST", 0, 0, 30, FALSE);
}

static int opts_of(const char * s)
{
    int o = -99;
    int rc = FlexNet_ParseLinkOpts(s, &o);
    return rc == 0 ? o : -1;
}

static void test_parse(void)
{
    ok(opts_of("") == 0, "bare F parses to no options");
    ok(opts_of("-") == FLEX_LOPT_NO_NBR, "'-' = no neighbour");
    ok(opts_of("!") == FLEX_LOPT_NO_BEHIND, "'!' = nothing behind");
    ok(opts_of(">") == (FLEX_LOPT_NO_NBR | FLEX_LOPT_NO_BEHIND),
       "'>' = neither");
    ok(opts_of("=") == (FLEX_LOPT_NO_BEHIND | FLEX_LOPT_OWN_ONLY),
       "'=' = '!' plus own-only");
    ok(opts_of("+") == FLEX_LOPT_PENALTY, "'+' = penalty");
    ok(opts_of(")") == FLEX_LOPT_HIDDEN, "')' = hidden");
    ok(opts_of("+)") == (FLEX_LOPT_PENALTY | FLEX_LOPT_HIDDEN),
       "combination '+)'");
    ok(opts_of("-!") == opts_of(">"), "'-!' composes to '>'");
    ok(opts_of(">>") == opts_of(">"), "repeated option is idempotent");

    int o = 77;
    ok(FlexNet_ParseLinkOpts("x", &o) == -1 && o == 0,
       "unknown option fails and zeroes the result");
    o = 77;
    ok(FlexNet_ParseLinkOpts("+*", &o) == -1 && o == 0,
       "a valid option does not survive an invalid one");
    o = 77;
    ok(FlexNet_ParseLinkOpts(NULL, &o) == -1 && o == 0, "NULL suffix");
    ok(FlexNet_ParseLinkOpts("+", NULL) == -1, "NULL out-param");
}

static void test_format(void)
{
    const char * canon[] = { "F", "F-", "F!", "F>", "F=", "F-=", "F+",
                             "F)", "F>+)", "F=+" };
    char buf[8];
    for (size_t i = 0; i < sizeof(canon) / sizeof(canon[0]); i++)
    {
        int o = opts_of(canon[i] + 1);
        flex_link_opts_format(o, buf, sizeof(buf));
        char what[48];
        snprintf(what, sizeof(what), "format round-trips %s", canon[i]);
        ok(strcmp(buf, canon[i]) == 0, what);
    }
    flex_link_opts_format(opts_of("-!"), buf, sizeof(buf));
    ok(strcmp(buf, "F>") == 0, "'-!' is shown as F>");
    flex_link_opts_format(opts_of(">+)"), buf, 3);
    ok(strcmp(buf, "F>") == 0, "format truncates to the buffer");
}

static void test_cost(void)
{
    int p = FLEX_LOPT_PENALTY;
    ok(flex_link_opts_cost(0, 12) == 12, "no '+' leaves the cost alone");
    ok(flex_link_opts_cost(p, 12) == 12 + FLEXNET_LINK_PENALTY,
       "'+' adds the penalty once");
    ok(flex_link_opts_cost(p, FLEXNET_RTT_INFINITY) == FLEXNET_RTT_INFINITY,
       "withdrawal sentinel passes through");
    ok(flex_link_opts_cost(p, 0) == 0, "RTT=0 marker passes through");
    ok(flex_link_opts_cost(p, FLEXNET_RTT_INFINITY - 1) ==
           FLEXNET_RTT_INFINITY - 1,
       "a penalised cost never becomes the sentinel");
    ok(flex_link_opts_cost(p, FLEXNET_RTT_INFINITY - FLEXNET_LINK_PENALTY) ==
           FLEXNET_RTT_INFINITY - 1,
       "boundary clamps below the sentinel");
}

static void test_source_allows(void)
{
    ok(flex_link_opts_source_allows(0, TRUE), "default: neighbour offered");
    ok(flex_link_opts_source_allows(0, FALSE), "default: behind offered");
    ok(!flex_link_opts_source_allows(opts_of("-"), TRUE), "'-' drops nbr");
    ok(flex_link_opts_source_allows(opts_of("-"), FALSE), "'-' keeps behind");
    ok(flex_link_opts_source_allows(opts_of("!"), TRUE), "'!' keeps nbr");
    ok(!flex_link_opts_source_allows(opts_of("!"), FALSE), "'!' drops behind");
    ok(!flex_link_opts_source_allows(opts_of(">"), TRUE), "'>' drops nbr");
    ok(!flex_link_opts_source_allows(opts_of(">"), FALSE), "'>' drops behind");
    ok(flex_link_opts_source_allows(opts_of("="), TRUE), "'=' keeps nbr");
    ok(!flex_link_opts_source_allows(opts_of("="), FALSE), "'=' drops behind");
    ok(flex_link_opts_source_allows(opts_of("+)"), FALSE),
       "'+' and ')' never filter");
}

static int exp_rtt(int peer, const char * call, int lo, int hi,
                   int * src, int * pen)
{
    BOOL direct = FALSE;
    return flex_expected_rtt(peer, call, lo, hi, src, &direct, pen);
}

static void test_expected_default(void)
{
    int src = -9, pen = -9;
    reset();
    ok(exp_rtt(0, "DEST", 0, 0, &src, &pen) == 12 && src == 1 && pen == 0,
       "default: DEST via NODEB at 12");
    ok(exp_rtt(0, "NODEB", 0, 0, &src, &pen) == 3 && src == 1,
       "default: NODEB itself at 3");
    ok(exp_rtt(1, "DEST", 0, 0, &src, &pen) == 32 && src == 2,
       "split horizon: NODEB is never its own source");
}

/* v2.5 — sessions on another port are not sources (L2 forwarding stays
   on the arrival port). All three sessions start on port 0. */
static void test_expected_cross_port(void)
{
    int src = -9, pen = -9;
    reset();
    FlexNetSessions[1].port = 5;          /* NODEB moves to a KISS port */
    ok(exp_rtt(0, "DEST", 0, 0, &src, &pen) == 32 && src == 2,
       "cross-port: DEST falls back to the same-port NODEC");
    ok(exp_rtt(0, "NODEB", 0, 0, &src, &pen) == FLEXNET_RTT_INFINITY &&
           src == -1,
       "cross-port: the other port's neighbour is not offered");
    ok(exp_rtt(1, "DEST", 0, 0, &src, &pen) == FLEXNET_RTT_INFINITY,
       "cross-port: nothing from port 0 is offered to NODEB");
    FlexNetSessions[2].port = 5;
    ok(exp_rtt(1, "DEST", 0, 0, &src, &pen) == 32 && src == 2,
       "same port again: offered");
    ok(exp_rtt(-1, "DEST", 0, 0, &src, &pen) == 12 && src == 1,
       "no target peer (climb guard view): every port counts");
}

/* v2.6 — with FLEXNETCROSSPORT the other port's sessions are sources
   again: L2 forwarding can now carry the frames across. */
static void test_expected_cross_port_on(void)
{
    int src = -9, pen = -9;
    reset();
    g_flexnet_crossport_enabled = TRUE;
    FlexNetSessions[1].port = 5;          /* NODEB on a KISS port */
    ok(exp_rtt(0, "DEST", 0, 0, &src, &pen) == 12 && src == 1,
       "crossport on: the cheaper route via the other port is offered");
    ok(exp_rtt(0, "NODEB", 0, 0, &src, &pen) < FLEXNET_RTT_INFINITY &&
           src == 1,
       "crossport on: the other port's neighbour is offered");
    ok(exp_rtt(1, "DEST", 0, 0, &src, &pen) == 32 && src == 2,
       "crossport on: port 0's routes are offered to NODEB");
    g_flexnet_l2_transit_enabled = FALSE;
    ok(exp_rtt(0, "DEST", 0, 0, &src, &pen) == 32 && src == 2,
       "crossport without L2 forwarding: back to same-port only");
    g_flexnet_l2_transit_enabled = TRUE;
    g_flexnet_crossport_enabled = FALSE;
}

/* v2.6 — a neighbour is never offered a route to itself, even when a
   third neighbour knows one (found live: IR2UFV offered IW2OHX-13 its own
   call across ports). (X)Net does not send a node its own record. */
static void test_expected_not_to_itself(void)
{
    int src = -9, pen = -9;
    for (int xp = 0; xp <= 1; xp++)
    {
        reset();
        g_flexnet_crossport_enabled = xp;
        learn(2, "NODEB", 0, 0, 5, FALSE);     /* NODEC knows a way to NODEB */
        ok(exp_rtt(1, "NODEB", 0, 0, &src, &pen) == FLEXNET_RTT_INFINITY &&
               src == -1,
           xp ? "crossport on: NODEB is not offered NODEB"
              : "crossport off: NODEB is not offered NODEB");
        ok(exp_rtt(0, "NODEB", 0, 0, &src, &pen) < FLEXNET_RTT_INFINITY,
           "NODEA is still offered NODEB");
        learn(2, "NODEB", 0, 15, 7, FALSE);    /* a range covering it */
        ok(exp_rtt(1, "NODEB", 0, 15, &src, &pen) == FLEXNET_RTT_INFINITY,
           "nor a range record covering its own SSID");
    }
    g_flexnet_crossport_enabled = FALSE;
}

static void test_expected_no_behind(void)
{
    int src = -9, pen = -9;
    reset();
    g_link_opts[1] = opts_of("!");
    ok(exp_rtt(0, "DEST", 0, 0, &src, &pen) == 32 && src == 2,
       "'!' on NODEB: DEST falls back to NODEC's 32, not withdrawn");
    ok(exp_rtt(0, "NODEB", 0, 0, &src, &pen) == 3 && src == 1,
       "'!' on NODEB: NODEB itself still offered");

    FlexNetSessions[2].active = FALSE;
    ok(exp_rtt(0, "DEST", 0, 0, &src, &pen) == FLEXNET_RTT_INFINITY &&
           src == -1,
       "'!' with no other source: DEST is withdrawn");
}

static void test_expected_no_nbr(void)
{
    int src = -9, pen = -9;
    reset();
    g_link_opts[1] = opts_of("-");
    ok(exp_rtt(0, "NODEB", 0, 0, &src, &pen) == FLEXNET_RTT_INFINITY &&
           src == -1,
       "'-' on NODEB: NODEB itself withdrawn");
    ok(exp_rtt(0, "DEST", 0, 0, &src, &pen) == 12 && src == 1,
       "'-' on NODEB: what is behind it still offered");

    /* The peer's own RANGE record, not just InitSession's entry. */
    learn(1, "NODEB", 0, 15, 1, FALSE);
    ok(exp_rtt(0, "NODEB", 0, 15, &src, &pen) == FLEXNET_RTT_INFINITY,
       "'-' also hides the neighbour's own range record");
}

static void test_expected_private(void)
{
    int src = -9, pen = -9;
    reset();
    g_link_opts[1] = opts_of(">");
    ok(exp_rtt(0, "NODEB", 0, 0, &src, &pen) == FLEXNET_RTT_INFINITY,
       "'>' on NODEB: NODEB withdrawn");
    ok(exp_rtt(0, "DEST", 0, 0, &src, &pen) == 32 && src == 2,
       "'>' on NODEB: DEST only via NODEC");
    ok(exp_rtt(0, "NODEC", 1, 1, &src, &pen) == 3 && src == 2,
       "'>' on NODEB leaves NODEC's link alone");
}

static void test_expected_penalty(void)
{
    int src = -9, pen = -9;
    reset();
    g_link_opts[1] = opts_of("+");
    ok(exp_rtt(0, "DEST", 0, 0, &src, &pen) == 32 && src == 2 && pen == 0,
       "'+' on NODEB: the unpenalised NODEC path now wins");
    ok(exp_rtt(0, "NODEB", 0, 0, &src, &pen) ==
           3 + FLEXNET_LINK_PENALTY && pen == FLEXNET_LINK_PENALTY,
       "'+' on NODEB: the neighbour itself is degraded too");

    FlexNetSessions[2].active = FALSE;
    ok(exp_rtt(0, "DEST", 0, 0, &src, &pen) ==
           12 + FLEXNET_LINK_PENALTY && src == 1 &&
           pen == FLEXNET_LINK_PENALTY,
       "'+' sole path: offered at cost + penalty, penalty reported");

    ok(flex_expected_rtt(0, "DEST", 0, 0, NULL, NULL, NULL) ==
           12 + FLEXNET_LINK_PENALTY,
       "NULL out-params are allowed");
}

static void test_expected_own_only_as_source(void)
{
    int src = -9, pen = -9;
    reset();
    g_link_opts[1] = opts_of("=");
    ok(exp_rtt(0, "DEST", 0, 0, &src, &pen) == 32 && src == 2,
       "'=' on NODEB as a source behaves as '!'");
    ok(exp_rtt(0, "NODEB", 0, 0, &src, &pen) == 3,
       "'=' on NODEB: NODEB itself still offered");
}

static void test_session_peer(void)
{
    reset();
    ok(flex_dest_is_session_peer(2, "NODEC", 0, 15), "range covers SSID");
    ok(flex_dest_is_session_peer(2, "nodec", 1, 1), "case-insensitive");
    ok(!flex_dest_is_session_peer(2, "NODEC", 2, 15), "range misses SSID");
    ok(!flex_dest_is_session_peer(2, "NODEB", 0, 15), "other call");
    ok(!flex_dest_is_session_peer(-1, "NODEC", 1, 1), "index -1");
    ok(!flex_dest_is_session_peer(FLEXNET_MAX_SESSIONS, "NODEC", 1, 1),
       "index == MAX");
    FlexNetSessions[2].active = FALSE;
    ok(!flex_dest_is_session_peer(2, "NODEC", 1, 1), "inactive session");
}

/* Drive a cost series through the guard the way flex_advertise_check()
   does: last is what went on the wire, expected what we would send. */
static int climb_series(const int * cost, const int * pen, int n,
                        BOOL subtract_penalty)
{
    int floor_ = -1, steps = 0, last = -1, last_pen = 0, withdrawn = 0;
    BOOL looping = FALSE;
    for (int i = 0; i < n; i++)
    {
        int lc = last, ec = cost[i];
        if (subtract_penalty)
        {
            if (lc >= 0) lc -= last_pen;
            ec -= pen[i];
        }
        int v = flex_climb_is_loop(&floor_, &steps, &looping, lc, ec);
        if (v != FLEX_CLIMB_OK) withdrawn++;
        last = cost[i];
        last_pen = pen[i];
    }
    return withdrawn;
}

static void test_climb_failover_to_penalised(void)
{
    /* 12 via the direct path, which dies; the '+' tunnel then carries
       it at 12+2000 and wiggles upward by a tick three times. */
    const int P = FLEXNET_LINK_PENALTY;
    const int cost[] = { 12, 13, 12, P + 14, P + 15, P + 16, P + 17 };
    const int pen[]  = { 0,  0,  0,  P,      P,      P,      P      };
    int n = (int)(sizeof(cost) / sizeof(cost[0]));

    ok(climb_series(cost, pen, n, FALSE) > 0,
       "control: on raw costs the failover reads as a loop");
    ok(climb_series(cost, pen, n, TRUE) == 0,
       "with the penalty removed it is one honest re-route");
}

int main(void)
{
    printf("test_link_opts\n");
    test_parse();
    test_format();
    test_cost();
    test_source_allows();
    test_expected_default();
    test_expected_cross_port();
    test_expected_cross_port_on();
    test_expected_not_to_itself();
    test_expected_no_behind();
    test_expected_no_nbr();
    test_expected_private();
    test_expected_penalty();
    test_expected_own_only_as_source();
    test_session_peer();
    test_climb_failover_to_penalised();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
