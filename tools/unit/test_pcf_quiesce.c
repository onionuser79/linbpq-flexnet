/* The record-emission gate that fixes the IW2OHX-12 teardown.
 *
 * The defect this guards: PC/Flexnet tolerates our unsolicited compact
 * records until it has done a '3+' exchange, and treats them as a
 * protocol error afterwards. After the '3-' closing our answer it
 * accepts at most TWO more record frames and then DISCs — measured
 * 30/30 over the 20.9 h quiet capture of 2026-09-21, 10 transactions
 * dying on the 1st frame and 20 on the 2nd, none on the 0th and none
 * reaching a 3rd.
 *
 * flex_records_allowed() is the single predicate behind all three
 * emission paths (queueing, bucket drain, 120 s tick). It exists as one
 * function precisely because the first draft of the fix gated two of
 * the three and left the tick pushing our own record every 120 s.
 *
 * Extracted verbatim from FlexNetCode.c by tools/unit/extract.sh, so
 * the test cannot drift from shipped code.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>

#define FLEXNET_MAX_CALLSIGN   10
#define FLEXNET_SSID_BASE      0x30
#define FLEXNET_RTT_INFINITY   60000
#define FLEXNET_MAX_PATH_HOPS  8
#define FLEXNET_ADVERT_FRAME_BYTES 200
#define FLEXNET_ADVERT_RECS_PCF    16
#define FLEXNET_RTT_WIRE_MAX    4095
#define FLEXNET_CLIMB_RATIO        4
#define FLEXNET_CLIMB_MIN_STEPS    3
#define FLEX_CLIMB_OK        0
#define FLEX_CLIMB_WITHDRAW  1
#define FLEX_CLIMB_SUPPRESS  2
#define FLEXNET_MAX_SESSIONS   8

typedef int BOOL;
#define TRUE  1
#define FALSE 0

struct FLEXNET_DEST_ENTRY
{
    char callsign[FLEXNET_MAX_CALLSIGN];
    int  ssid_lo, ssid_hi, rtt, is_infinity;
    char via_callsign[FLEXNET_MAX_CALLSIGN];
    int  port, via_session_idx;
    time_t last_updated;
    char path_hops[FLEXNET_MAX_PATH_HOPS][FLEXNET_MAX_CALLSIGN];
    int  path_len;
    time_t path_updated;
};

/* Only the fields the predicate touches. */
struct FLEXNET_SESSION { BOOL active; BOOL pcf_quiesced; };
static struct FLEXNET_SESSION FlexNetSessions[FLEXNET_MAX_SESSIONS];
static BOOL g_flexnet_pcf_quiesce = TRUE;

#include "extracted_quiesce.inc"

static int failures = 0, checks = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static void reset(void)
{
    memset(FlexNetSessions, 0, sizeof(FlexNetSessions));
    g_flexnet_pcf_quiesce = TRUE;
}

/* A fresh session may advertise: this is the seed dump, and PC/Flexnet
   accepted 1601-3259 records per session that way without complaint. */
static void test_fresh_session_may_advertise(void)
{
    reset();
    ok(flex_records_allowed(0) == 1, "fresh session may emit records");
}

/* The whole point: once the '3+' answer is closed, stay silent. */
static void test_quiesced_session_is_silent(void)
{
    reset();
    FlexNetSessions[0].pcf_quiesced = TRUE;
    ok(flex_records_allowed(0) == 0, "quiesced session emits nothing");
}

/* The gate is per session. (X)Net sent no '3+' at all in 20.9 h, so it
   must never be armed, and arming the PCF peer must not silence it. */
static void test_gate_is_per_session(void)
{
    reset();
    FlexNetSessions[1].pcf_quiesced = TRUE;   /* the PCF peer */
    ok(flex_records_allowed(0) == 1, "xnet peer unaffected by PCF quiesce");
    ok(flex_records_allowed(1) == 0, "PCF peer silenced");
}

/* FLEXNETPCFQUIESCE NO restores pre-v2.2.2 behaviour exactly, so the
   change can be backed out on a live node without a rebuild. */
static void test_switch_off_restores_old_behaviour(void)
{
    reset();
    g_flexnet_pcf_quiesce = FALSE;
    FlexNetSessions[0].pcf_quiesced = TRUE;
    ok(flex_records_allowed(0) == 1, "FLEXNETPCFQUIESCE NO ignores the flag");
}

/* Out of range is not a session; emitting to it would index off the
   array. Callers pass (sess - FlexNetSessions), which is -1 on failure. */
static void test_out_of_range_is_denied(void)
{
    reset();
    ok(flex_records_allowed(-1) == 0, "negative index denied");
    ok(flex_records_allowed(FLEXNET_MAX_SESSIONS) == 0, "index == MAX denied");
    ok(flex_records_allowed(FLEXNET_MAX_SESSIONS + 99) == 0, "far index denied");
}

/* A reconnect must clear the flag. FlexNet_InitSession memsets the
   session, so the zeroed state has to mean "may advertise" — if the
   sense of this flag is ever inverted, this test fails. */
static void test_zeroed_session_may_advertise(void)
{
    reset();
    memset(&FlexNetSessions[2], 0, sizeof(FlexNetSessions[2]));
    ok(flex_records_allowed(2) == 1, "memset session may emit (reconnect)");
}

int main(void)
{
    printf("test_pcf_quiesce\n");
    test_fresh_session_may_advertise();
    test_quiesced_session_is_silent();
    test_gate_is_per_session();
    test_switch_off_restores_old_behaviour();
    test_out_of_range_is_denied();
    test_zeroed_session_may_advertise();
    printf("  %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
