/* B-2 (v2.6.1): the D table re-chooses a destination's next hop from what
 * the active sessions offer whenever the current source goes away.
 *
 * Pins:
 *   - a withdrawal from the current next hop fails over to another
 *     neighbour's finite offer instead of going to infinity;
 *   - a dead session's rows fail over, or go to infinity — never stay
 *     finite and pointing at no session (the IW2OHX-13 "80 of 213" state);
 *   - orphans left by older builds are healed by the same walk;
 *   - choice is by cost FROM HERE (reported + our link time, '+' penalty
 *     included) and the incumbent wins a tie (§7.2);
 *   - without learned[] (FLEXNETTRANSIT NO) the row goes to infinity.
 *
 * Extracted verbatim from FlexNetCode.c by tools/unit/extract.sh.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define FLEXNET_MAX_CALLSIGN               10
#define FLEXNET_RTT_INFINITY               60000
#define FLEXNET_MAX_SESSIONS               8
#define FLEXNET_MAX_LEARNED_PER_NEIGHBOUR  32
#define FLEXNET_MAX_PATH_HOPS              8

typedef int BOOL;
typedef unsigned char UCHAR;
#define TRUE  1
#define FALSE 0

typedef struct { UCHAR LINKCALL[7]; } LINKTABLE;
struct FLEXNET_SESSION
{
    LINKTABLE * LINK;
    BOOL active;
    int  our_link_time;
    int  port;
};
static struct FLEXNET_SESSION FlexNetSessions[FLEXNET_MAX_SESSIONS];
static LINKTABLE links[FLEXNET_MAX_SESSIONS];

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

#include "extracted_reelect.inc"

static int failures = 0, checks = 0;
static void ok(int cond, const char * what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static void reset(void)
{
    memset(FlexNetSessions, 0, sizeof(FlexNetSessions));
    memset(FlexNetLearned, 0, sizeof(FlexNetLearned));
    memset(FlexNetDests, 0, sizeof(FlexNetDests));
    memset(g_link_opts, 0, sizeof(g_link_opts));
    FlexNetDestCount = 0;
    g_flexnet_transit_enabled = TRUE;
}

static void add_session(int si, const char * call, int link_time, int port)
{
    size_t n = strlen(call);
    for (size_t i = 0; i < 6; i++)
        links[si].LINKCALL[i] = (UCHAR)((i < n ? call[i] : ' ') << 1);
    links[si].LINKCALL[6] = 0x60;
    FlexNetSessions[si].LINK = &links[si];
    FlexNetSessions[si].active = TRUE;
    FlexNetSessions[si].our_link_time = link_time;
    FlexNetSessions[si].port = port;
}

static struct FLEXNET_LEARNED_ROUTE * learn(int si, const char * call, int rtt)
{
    struct FLEXNET_LEARNED_STATE * st = &FlexNetLearned[si];
    struct FLEXNET_LEARNED_ROUTE * r = &st->routes[st->count++];
    memset(r, 0, sizeof(*r));
    snprintf(r->dest_call, sizeof(r->dest_call), "%s", call);
    r->ssid_lo = 0;
    r->ssid_hi = 15;
    r->rtt_at_neighbour = rtt;
    return r;
}

static int dest(const char * call, int rtt, int via)
{
    struct FLEXNET_DEST_ENTRY * d = &FlexNetDests[FlexNetDestCount];
    snprintf(d->callsign, sizeof(d->callsign), "%s", call);
    d->ssid_lo = 0;
    d->ssid_hi = 15;
    d->rtt = rtt;
    d->is_infinity = rtt >= FLEXNET_RTT_INFINITY;
    d->via_session_idx = via;
    return FlexNetDestCount++;
}

/* Session 1 = NODEB (port 2, link 2), session 2 = NODEC (port 3, link 2). */
static void two_neighbours(int rtt_b, int rtt_c)
{
    reset();
    add_session(1, "NODEB", 2, 2);
    add_session(2, "NODEC", 2, 3);
    learn(1, "DEST", rtt_b);
    learn(2, "DEST", rtt_c);
}

static void test_withdrawal_fails_over(void)
{
    two_neighbours(10, 30);
    int i = dest("DEST", 10, 1);
    FlexNetLearned[1].routes[0].rtt_at_neighbour = FLEXNET_RTT_INFINITY;
    flex_dest_reelect(i);
    ok(FlexNetDests[i].via_session_idx == 2, "withdrawal: fails over to NODEC");
    ok(FlexNetDests[i].rtt == 30, "withdrawal: takes NODEC's reported cost");
    ok(!FlexNetDests[i].is_infinity, "withdrawal: stays finite");
    ok(FlexNetDests[i].port == 3, "withdrawal: port follows the next hop");
    ok(strcmp(FlexNetDests[i].via_callsign, "NODEC") == 0,
       "withdrawal: via_callsign follows (D '<' filter and FL agree)");
}

static void test_no_alternative_goes_infinite(void)
{
    two_neighbours(10, FLEXNET_RTT_INFINITY);
    int i = dest("DEST", 10, 1);
    FlexNetLearned[1].routes[0].rtt_at_neighbour = FLEXNET_RTT_INFINITY;
    flex_dest_reelect(i);
    ok(FlexNetDests[i].rtt == FLEXNET_RTT_INFINITY, "no alt: infinity");
    ok(FlexNetDests[i].is_infinity == 1, "no alt: flagged infinite");
    ok(FlexNetDests[i].via_session_idx == -1, "no alt: no next hop");
}

static void test_dead_session_rows_fail_over(void)
{
    two_neighbours(10, 30);
    int i = dest("DEST", 10, 1);
    int j = dest("ONLYB", 5, 1);
    learn(1, "ONLYB", 5);
    FlexNetSessions[1].active = FALSE;          /* reaped / closed */
    flex_dest_reelect_via(1);
    ok(FlexNetDests[i].via_session_idx == 2, "dead: DEST moves to NODEC");
    ok(FlexNetDests[j].via_session_idx == -1 &&
       FlexNetDests[j].rtt == FLEXNET_RTT_INFINITY,
       "dead: ONLYB goes to infinity, not a finite orphan");
}

static void test_orphan_healed(void)
{
    two_neighbours(10, 30);
    int i = dest("DEST", 4392, -1);             /* the CQ0PPA state */
    flex_dest_reelect_via(5);                   /* any session's death */
    ok(FlexNetDests[i].via_session_idx == 1 && FlexNetDests[i].rtt == 10,
       "orphan: re-attached to the cheapest live source");
}

static void test_cost_from_here(void)
{
    two_neighbours(10, 20);
    FlexNetSessions[1].our_link_time = 50;      /* slow RF to NODEB */
    int i = dest("DEST", FLEXNET_RTT_INFINITY, -1);
    flex_dest_reelect(i);
    ok(FlexNetDests[i].via_session_idx == 2, "from here: 20+2 beats 10+50");
}

static void test_penalty_link_loses(void)
{
    two_neighbours(30, 10);
    g_link_opts[2] = FLEX_LOPT_PENALTY;
    int i = dest("DEST", FLEXNET_RTT_INFINITY, -1);
    flex_dest_reelect(i);
    ok(FlexNetDests[i].via_session_idx == 1, "'+': 10+2000 loses to 30");
    FlexNetLearned[1].routes[0].rtt_at_neighbour = FLEXNET_RTT_INFINITY;
    flex_dest_reelect(i);
    ok(FlexNetDests[i].via_session_idx == 2 &&
       FlexNetDests[i].rtt == 10 + FLEXNET_LINK_PENALTY,
       "'+': only source left, stored with its penalty as merge does");
}

static void test_tie_keeps_incumbent(void)
{
    two_neighbours(10, 10);
    int i = dest("DEST", 10, 2);
    flex_dest_reelect(i);
    ok(FlexNetDests[i].via_session_idx == 2, "tie: incumbent NODEC kept");
}

static void test_rtt0_and_other_rows_ignored(void)
{
    two_neighbours(0, 30);                      /* refresh marker only */
    int i = dest("DEST", FLEXNET_RTT_INFINITY, -1);
    int k = dest("OTHER", 7, 2);
    learn(2, "OTHER", 7);
    flex_dest_reelect(i);
    ok(FlexNetDests[i].via_session_idx == 2, "rtt 0 is not an offer");
    flex_dest_reelect_via(1);
    ok(FlexNetDests[k].via_session_idx == 2 && FlexNetDests[k].rtt == 7,
       "rows via a live other session untouched");
}

static void test_transit_off(void)
{
    two_neighbours(10, 30);
    g_flexnet_transit_enabled = FALSE;
    int i = dest("DEST", 10, 1);
    FlexNetSessions[1].active = FALSE;
    flex_dest_reelect_via(1);
    ok(FlexNetDests[i].rtt == FLEXNET_RTT_INFINITY &&
       FlexNetDests[i].via_session_idx == -1,
       "transit off: nothing to choose from, infinity");
}

int main(void)
{
    test_withdrawal_fails_over();
    test_no_alternative_goes_infinite();
    test_dead_session_rows_fail_over();
    test_orphan_healed();
    test_cost_from_here();
    test_penalty_link_loses();
    test_tie_keeps_incumbent();
    test_rtt0_and_other_rows_ignored();
    test_transit_off();
    printf("test_reelect: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
