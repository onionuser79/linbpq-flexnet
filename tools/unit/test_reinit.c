/* v2.6.2: a type-0 INIT that arrives on an already-established session
 * means the peer restarted its FlexNet layer under a live L2 link (it
 * re-SABMed an existing link). Captures show (X)Net then repeats its
 * INIT every ~190 s and withholds keepalives and route records until it
 * receives ours, so the INIT must be answered.
 *
 * The guard keeps two LinBPQ peers from answering each other's answer:
 * one that sent an INIT within FLEXNET_REINIT_GUARD seconds stays quiet.
 *
 * Extracted verbatim from FlexNetCode.c by tools/unit/extract.sh.
 */
#include <stdio.h>
#include <time.h>

typedef int BOOL;
#define TRUE  1
#define FALSE 0

#include "extracted_reinit.inc"

static int failures = 0, checks = 0;
static void ok(int cond, const char * what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static void test_first_init_not_answered(void)
{
    /* Session start: we sent our INIT with the KA, the peer's INIT is the
       other half of the handshake, not a restart. */
    ok(!flex_reinit_answer(FALSE, 1000, 1000), "first INIT, same second");
    ok(!flex_reinit_answer(FALSE, 1000, 5000), "first INIT, late");
    ok(!flex_reinit_answer(FALSE, 0, 5000), "first INIT, never sent ours");
}

static void test_reinit_answered(void)
{
    /* 2026-10-12 capture: our last INIT 13 h before the peer's re-INIT. */
    ok(flex_reinit_answer(TRUE, 1000, 1000 + 13 * 3600), "re-INIT hours later");
    ok(flex_reinit_answer(TRUE, 0, 5000), "re-INIT, ours never stamped");
    ok(flex_reinit_answer(TRUE, 1000, 1000 + FLEXNET_REINIT_GUARD),
       "re-INIT exactly at the guard");
}

static void test_guard_stops_ping_pong(void)
{
    ok(!flex_reinit_answer(TRUE, 1000, 1000), "INIT answered this second");
    ok(!flex_reinit_answer(TRUE, 1000, 1000 + FLEXNET_REINIT_GUARD - 1),
       "one second inside the guard");
    /* The capture's second (X)Net INIT came 16 s after the first: one
       answer covers both. */
    ok(!flex_reinit_answer(TRUE, 1000, 1016), "peer's 16 s repeat");
}

static void test_clock_step_back(void)
{
    /* Wall clock stepped backwards (NTP): never wedge the answer off. */
    ok(flex_reinit_answer(TRUE, 5000, 1000), "now before last_init_tx");
}

int main(void)
{
    test_first_init_not_answered();
    test_reinit_answered();
    test_guard_stops_ping_pong();
    test_clock_step_back();
    printf("test_reinit: %d/%d passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
