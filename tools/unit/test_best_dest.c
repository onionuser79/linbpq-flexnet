/* B-4 (v2.6.1): `C <call>` and `D <call>` use the cheapest D-table row
 * that covers the target, counted from here — not the first row in table
 * order. Overlapping rows for one callsign are normal: (X)Net itself
 * advertises IQ2LB 0-0 and IQ2LB 0-5.
 *
 * Extracted verbatim from FlexNetCode.c by tools/unit/extract.sh.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <time.h>

#define FLEXNET_MAX_CALLSIGN   10
#define FLEXNET_RTT_INFINITY   60000
#define FLEXNET_MAX_SESSIONS   8
#define FLEXNET_MAX_PATH_HOPS  8

typedef int BOOL;
#define TRUE  1
#define FALSE 0

struct FLEXNET_SESSION { BOOL active; int our_link_time; };
static struct FLEXNET_SESSION FlexNetSessions[FLEXNET_MAX_SESSIONS];

#include "extracted_best_dest.inc"

static int failures = 0, checks = 0;
static void ok(int cond, const char * what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static int row(const char * call, int lo, int hi, int rtt, int via)
{
    struct FLEXNET_DEST_ENTRY * d = &FlexNetDests[FlexNetDestCount];
    memset(d, 0, sizeof(*d));
    snprintf(d->callsign, sizeof(d->callsign), "%s", call);
    d->ssid_lo = lo;
    d->ssid_hi = hi;
    d->rtt = rtt;
    d->via_session_idx = via;
    return FlexNetDestCount++;
}

static void reset(void)
{
    memset(FlexNetSessions, 0, sizeof(FlexNetSessions));
    FlexNetDestCount = 0;
    FlexNetSessions[1].active = TRUE;  FlexNetSessions[1].our_link_time = 2;
    FlexNetSessions[2].active = TRUE;  FlexNetSessions[2].our_link_time = 2;
}

static void test_cheapest_overlapping_row(void)
{
    reset();
    row("IQ2LB", 0, 5, 2254, 1);               /* arrived first */
    int cheap = row("IQ2LB", 0, 0, 19, 1);
    ok(flex_find_dest_for_target("IQ2LB") == cheap, "IQ2LB: 0-0 at 19, not 0-5");
    ok(flex_find_dest_for_target("IQ2LB-0") == cheap, "IQ2LB-0 likewise");
    ok(flex_find_best_dest("IQ2LB", 3) == 0, "IQ2LB-3: only 0-5 covers it");
}

static void test_ssid_and_infinity(void)
{
    reset();
    row("IR2UFV", 0, 0, 1, 1);
    int wide = row("IR2UFV", 0, 8, 4, 2);
    row("IR2UFV", 0, 8, FLEXNET_RTT_INFINITY, 1);
    ok(flex_find_dest_for_target("IR2UFV-8") == wide, "SSID 8: only 0-8 covers");
    ok(flex_find_dest_for_target("IR2UFV-9") == -1, "SSID 9: no row covers");
    ok(flex_find_dest_for_target("NOSUCH") == -1, "unknown call");
    ok(flex_find_dest_for_target("") == -1, "empty target");
}

static void test_cost_from_here(void)
{
    reset();
    FlexNetSessions[1].our_link_time = 50;      /* slow RF neighbour */
    row("DEST", 0, 15, 10, 1);                  /* 60 from here */
    int fast = row("DEST", 0, 15, 20, 2);       /* 22 from here */
    ok(flex_find_dest_for_target("DEST") == fast, "20+2 beats 10+50");
}

static void test_tie_keeps_table_order(void)
{
    reset();
    int first = row("DEST", 0, 15, 10, 1);
    row("DEST", 0, 15, 10, 2);
    ok(flex_find_dest_for_target("DEST") == first, "tie: first row kept");
}

int main(void)
{
    test_cheapest_overlapping_row();
    test_ssid_and_infinity();
    test_cost_from_here();
    test_tie_keeps_table_order();
    printf("test_best_dest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
