/* v2.11 FLOG: the in-memory route-event ring.
 *
 * Pins: the ring keeps the newest FLEX_RLOG_SIZE events across wrap;
 * a row change is an event only when reachability or the next hop's
 * callsign changes (a cost change on the same next hop is not); the
 * line text; and the callsign filter.
 *
 * Extracted verbatim from FlexNetCode.c by tools/unit/extract.sh.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

typedef int BOOL;
#define TRUE  1
#define FALSE 0

#include "extracted_rlog.inc"

static int failures = 0, checks = 0;
static void ok(int cond, const char * what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}
static void eq(const char * got, const char * want, const char * what)
{
    int good = strcmp(got, want) == 0;
    ok(good, what);
    if (!good) printf("    got  [%s]\n    want [%s]\n", got, want);
}

static struct FLEX_RLOG log_;

static void put_cost(int cost)
{
    struct FLEX_ROUTE_EVENT e;
    memset(&e, 0, sizeof(e));
    e.type = FLEV_REACHABLE;
    e.cost = cost;
    flex_rlog_put(&log_, &e);
}

static void test_ring(void)
{
    memset(&log_, 0, sizeof(log_));
    ok(flex_rlog_nth(&log_, 0) == NULL, "empty: no newest");
    put_cost(1); put_cost(2); put_cost(3);
    ok(flex_rlog_nth(&log_, 0)->cost == 3, "newest first");
    ok(flex_rlog_nth(&log_, 2)->cost == 1, "oldest at k=2");
    ok(flex_rlog_nth(&log_, 3) == NULL, "past the oldest: NULL");

    memset(&log_, 0, sizeof(log_));
    for (int i = 1; i <= FLEX_RLOG_SIZE + 5; i++) put_cost(i);
    ok(log_.total == FLEX_RLOG_SIZE + 5, "total counts every event");
    ok(flex_rlog_nth(&log_, 0)->cost == FLEX_RLOG_SIZE + 5, "wrap: newest");
    ok(flex_rlog_nth(&log_, FLEX_RLOG_SIZE - 1)->cost == 6,
       "wrap: oldest kept is the 6th");
    ok(flex_rlog_nth(&log_, FLEX_RLOG_SIZE) == NULL, "wrap: no more than SIZE");
}

static void test_classify(void)
{
    ok(flex_rlog_classify(FALSE, "", TRUE, "NODEB") == FLEV_REACHABLE,
       "new / unreachable -> reachable");
    ok(flex_rlog_classify(TRUE, "NODEB", FALSE, "NODEB") == FLEV_UNREACHABLE,
       "reachable -> unreachable");
    ok(flex_rlog_classify(TRUE, "NODEB", TRUE, "NODEC") == FLEV_NEXTHOP,
       "next hop changed");
    ok(flex_rlog_classify(TRUE, "NODEB", TRUE, "nodeb") == 0,
       "same next hop, any case: no event (cost change only)");
    ok(flex_rlog_classify(FALSE, "NODEB", FALSE, "NODEC") == 0,
       "unreachable stays unreachable: no event");
    ok(flex_rlog_classify(TRUE, NULL, TRUE, "NODEC") == FLEV_NEXTHOP,
       "NULL previous next hop handled");
}

static void test_lines(void)
{
    struct FLEX_ROUTE_EVENT e;
    char b[96];
    memset(&e, 0, sizeof(e));
    snprintf(e.dest, sizeof(e.dest), "IR0EQ"); e.lo = 2; e.hi = 2;

    e.type = FLEV_UNREACHABLE;
    snprintf(e.from, sizeof(e.from), "IW2OHX-14");
    flex_rlog_line(&e, b, sizeof(b));
    eq(b, "IR0EQ 2-2      unreachable (was via IW2OHX-14)", "unreachable");

    e.type = FLEV_REACHABLE; e.from[0] = '\0';
    snprintf(e.to, sizeof(e.to), "IW2OHX-14"); e.cost = 305;
    flex_rlog_line(&e, b, sizeof(b));
    eq(b, "IR0EQ 2-2      reachable via IW2OHX-14, cost 305", "reachable");

    e.type = FLEV_NEXTHOP;
    snprintf(e.from, sizeof(e.from), "IW2OHX-2");
    flex_rlog_line(&e, b, sizeof(b));
    eq(b, "IR0EQ 2-2      next hop IW2OHX-2 -> IW2OHX-14, cost 305", "next hop");

    memset(&e, 0, sizeof(e));
    snprintf(e.from, sizeof(e.from), "IW2OHX-14");
    e.type = FLEV_LINK_UP;   flex_rlog_line(&e, b, sizeof(b));
    eq(b, "IW2OHX-14      link up", "link up");
    e.type = FLEV_LINK_DOWN; flex_rlog_line(&e, b, sizeof(b));
    eq(b, "IW2OHX-14      link down", "link down");
    e.type = FLEV_TABLE_REQ; flex_rlog_line(&e, b, sizeof(b));
    eq(b, "IW2OHX-14      full-table request (3+)", "3+");

    char tiny[6];
    flex_rlog_line(&e, tiny, sizeof(tiny));
    ok(tiny[sizeof(tiny) - 1] == '\0', "tiny buffer stays terminated");
}

static void test_match(void)
{
    struct FLEX_ROUTE_EVENT e;
    memset(&e, 0, sizeof(e));
    snprintf(e.dest, sizeof(e.dest), "DB0RES");
    snprintf(e.to, sizeof(e.to), "IW2OHX-14");
    ok(flex_rlog_match(&e, ""), "no filter: all");
    ok(flex_rlog_match(&e, NULL), "NULL filter: all");
    ok(flex_rlog_match(&e, "DB0"), "destination prefix");
    ok(flex_rlog_match(&e, "iw2ohx"), "neighbour base call, any case");
    ok(flex_rlog_match(&e, "IW2OHX-14"), "neighbour exact");
    ok(!flex_rlog_match(&e, "IW2OHX-2"), "other neighbour: no");
    ok(!flex_rlog_match(&e, "DK0"), "other destination: no");
}

int main(void)
{
    test_ring();
    test_classify();
    test_lines();
    test_match();
    printf("test_rlog: %d/%d passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
