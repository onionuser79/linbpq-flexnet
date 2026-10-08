/* v2.6 own-record guard in flex_advertise_check().
 *
 * Found in the IW2OHX-12 neighbour QA (2026-10-08): -12 never advertised
 * IW2OHX-4, whose only FlexNet link is -12, so no other node could route
 * to it. The guard that keeps transit records off our own call compared
 * the base call only, and IW2OHX-4 / -14 / -13 share IW2OHX with us.
 * Pins:
 *   - flex_record_is_ours(): same base AND overlapping SSID range;
 *   - flex_own_ssid_range(): FLEXNETSSIDRANGE widened to the node SSID.
 *
 * Extracted verbatim from FlexNetCode.c by tools/unit/extract.sh.
 */
#include <stdio.h>
#include <string.h>

#define FLEXNET_MAX_CALLSIGN 10

typedef int BOOL;
#define TRUE  1
#define FALSE 0

static int g_flexnet_ssid_lo = -1;
static int g_flexnet_ssid_hi = -1;

#include "extracted_own_record.inc"

static int failures = 0, checks = 0;
static void ok(int cond, const char * what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static void range_cases(void)
{
    int lo = -1, hi = -1;

    g_flexnet_ssid_lo = g_flexnet_ssid_hi = -1;
    flex_own_ssid_range(12, &lo, &hi);
    ok(lo == 12 && hi == 12, "unconfigured: node SSID only");

    g_flexnet_ssid_lo = 0; g_flexnet_ssid_hi = 8;
    flex_own_ssid_range(0, &lo, &hi);
    ok(lo == 0 && hi == 8, "configured range used as is");

    g_flexnet_ssid_lo = 0; g_flexnet_ssid_hi = 8;
    flex_own_ssid_range(12, &lo, &hi);
    ok(lo == 0 && hi == 12, "range widened up to the node SSID");

    g_flexnet_ssid_lo = 13; g_flexnet_ssid_hi = 15;
    flex_own_ssid_range(12, &lo, &hi);
    ok(lo == 12 && hi == 15, "range widened down to the node SSID");

    g_flexnet_ssid_lo = g_flexnet_ssid_hi = -1;
}

static void match_cases(void)
{
    /* IW2OHX-12, own record 12-12. */
    ok(flex_record_is_ours("IW2OHX", 12, 12, "IW2OHX", 12, 12),
       "our own SSID is ours");
    ok(!flex_record_is_ours("IW2OHX", 4, 4, "IW2OHX", 12, 12),
       "IW2OHX-4 next to IW2OHX-12 is NOT ours (the bug)");
    ok(!flex_record_is_ours("IW2OHX", 14, 14, "IW2OHX", 12, 12),
       "IW2OHX-14 is NOT ours");
    ok(!flex_record_is_ours("IW2OHX", 13, 15, "IW2OHX", 12, 12),
       "adjacent range above is not ours");
    ok(!flex_record_is_ours("IW2OHX", 0, 11, "IW2OHX", 12, 12),
       "adjacent range below is not ours");
    ok(flex_record_is_ours("IW2OHX", 0, 15, "IW2OHX", 12, 12),
       "a range covering us overlaps: stays suppressed");
    ok(flex_record_is_ours("IW2OHX", 11, 12, "IW2OHX", 12, 12),
       "boundary overlap at hi");
    ok(flex_record_is_ours("IW2OHX", 12, 13, "IW2OHX", 12, 12),
       "boundary overlap at lo");

    /* IR2UFV, own record 0-8. */
    ok(flex_record_is_ours("IR2UFV", 0, 0, "IR2UFV", 0, 8),
       "inside a configured range");
    ok(!flex_record_is_ours("IR2UFV", 9, 9, "IR2UFV", 0, 8),
       "just outside a configured range");

    ok(!flex_record_is_ours("IQ2LB", 0, 0, "IW2OHX", 0, 15),
       "another base call is never ours");
    ok(!flex_record_is_ours("IW2OHX1", 12, 12, "IW2OHX", 12, 12),
       "longer base call with our prefix is not ours");
    ok(!flex_record_is_ours("IW2OH", 12, 12, "IW2OHX", 12, 12),
       "shorter base call is not ours");
    ok(!flex_record_is_ours("IW2OHX", 12, 12, "", 12, 12),
       "unknown own call: nothing is ours");
    ok(!flex_record_is_ours(NULL, 12, 12, "IW2OHX", 12, 12), "NULL dest");
    ok(!flex_record_is_ours("IW2OHX", 12, 12, NULL, 12, 12), "NULL own");
}

int main(void)
{
    range_cases();
    match_cases();
    printf("test_own_record: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
