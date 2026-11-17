/* v2.11: a cost-0 compact record is the sender withdrawing its offer.
 *
 * Captured 2026-11-17 (research/xnet_capture_2026-11-17): an (X)Net node
 * that lost a neighbour sent its other neighbours `3IR2UFV080 ` (cost 0)
 * about 10 s after the DISC, and never 60000. Skipping the record kept
 * the destination installed via the dead path.
 *
 * Failover after the rewrite (next-best neighbour, else infinity) is the
 * withdrawal path test_reelect already pins.
 *
 * Extracted verbatim from FlexNetCode.c by tools/unit/extract.sh.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#define FLEXNET_MAX_CALLSIGN   10
#define FLEXNET_RTT_INFINITY   60000
#define FLEXNET_MAX_PATH_HOPS  8

typedef int BOOL;
#define TRUE  1
#define FALSE 0

#include "extracted_rtt0.inc"

static int failures = 0, checks = 0;
static void ok(int cond, const char * what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static struct FLEXNET_DEST_ENTRY rec(const char * call, int lo, int hi,
                                     int rtt, int inf)
{
    struct FLEXNET_DEST_ENTRY r;
    memset(&r, 0, sizeof(r));
    snprintf(r.callsign, sizeof(r.callsign), "%s", call);
    r.ssid_lo = lo;
    r.ssid_hi = hi;
    r.rtt = rtt;
    r.is_infinity = inf;
    return r;
}

static void test_known_destination_withdrawn(void)
{
    /* The captured record: IR2UFV 0-8, cost 0, destination held. */
    struct FLEXNET_DEST_ENTRY r = rec("IR2UFV", 0, 8, 0, 0);
    ok(flex_rtt0_rewrite(&r, TRUE) == FLEX_RTT0_WITHDRAW, "known: withdraw");
    ok(r.rtt == FLEXNET_RTT_INFINITY, "known: cost becomes infinity");
    ok(r.is_infinity, "known: flagged infinite");
    ok(strcmp(r.callsign, "IR2UFV") == 0 && r.ssid_lo == 0 && r.ssid_hi == 8,
       "known: call and range untouched");
}

static void test_unknown_destination_dropped(void)
{
    struct FLEXNET_DEST_ENTRY r = rec("I0OJJ", 3, 3, 0, 0);
    ok(flex_rtt0_rewrite(&r, FALSE) == FLEX_RTT0_DROP, "unknown: drop");
    ok(r.rtt == 0 && !r.is_infinity, "unknown: record not rewritten");
}

static void test_other_costs_untouched(void)
{
    /* Boundaries either side of 0, and both withdrawal spellings. */
    static const int costs[] = { 1, 2, 2001, FLEXNET_RTT_INFINITY - 1 };
    for (size_t i = 0; i < sizeof(costs) / sizeof(costs[0]); i++)
    {
        struct FLEXNET_DEST_ENTRY r = rec("DEST", 0, 0, costs[i], 0);
        ok(flex_rtt0_rewrite(&r, TRUE) == FLEX_RTT0_NONE, "finite: none");
        ok(r.rtt == costs[i] && !r.is_infinity, "finite: unchanged");
    }
    struct FLEXNET_DEST_ENTRY w = rec("DEST", 0, 0, FLEXNET_RTT_INFINITY, 1);
    ok(flex_rtt0_rewrite(&w, TRUE) == FLEX_RTT0_NONE, "60000: none");
    /* A '-' frame sets is_infinity; the parser forces 60000, but a 0
       carrying the flag is already a withdrawal and stays as it is. */
    struct FLEXNET_DEST_ENTRY m = rec("DEST", 0, 0, 0, 1);
    ok(flex_rtt0_rewrite(&m, FALSE) == FLEX_RTT0_NONE, "0 + infinity flag: none");
    ok(m.rtt == 0, "0 + infinity flag: unchanged");
}

static void test_idempotent(void)
{
    struct FLEXNET_DEST_ENTRY r = rec("DEST", 0, 15, 0, 0);
    ok(flex_rtt0_rewrite(&r, TRUE) == FLEX_RTT0_WITHDRAW, "first pass");
    ok(flex_rtt0_rewrite(&r, TRUE) == FLEX_RTT0_NONE, "second pass: no-op");
}

int main(void)
{
    test_known_destination_withdrawn();
    test_unknown_destination_dropped();
    test_other_costs_untouched();
    test_idempotent();
    printf("test_rtt0: %d/%d passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
