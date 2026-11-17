/* Slow-poller detection and the type-1 interval that follows from it.
 *
 * WAMPES polls with PC/Flexnet's keepalive bytes ('2' + 199 spaces + CR)
 * every 300 s and times each poll against the next type-1 it receives;
 * PC/Flexnet sends the same bytes every ~16 s and must not get a type-1
 * sooner than 320 s apart. Captured 2026-11-17 (WAMPES <-> IR2UFV) and
 * 2026-06-02 (research/xnet14-pcf-2026-06-02.pcapng).
 *
 * Extracted verbatim from FlexNetCode.c by tools/unit/extract.sh.
 */
#include <stdio.h>
#include <time.h>

typedef int BOOL;
#define TRUE 1
#define FALSE 0

#include "extracted_slow_poll.inc"

static int failures = 0, checks = 0;
static void ok(int cond, const char * what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static void test_gap(void)
{
    const time_t t = 1000000;

    ok(flex_ka_gap_is_slow('\r', t, t + 300), "WAMPES: CR keepalive 300 s apart");
    ok(flex_ka_gap_is_slow('\r', t, t + FLEXNET_SLOW_POLL_GAP), "boundary: exactly the gap");
    ok(!flex_ka_gap_is_slow('\r', t, t + FLEXNET_SLOW_POLL_GAP - 1), "one second short");
    ok(!flex_ka_gap_is_slow('\r', t, t + 16), "PC/Flexnet: CR keepalive 16 s apart");
    ok(!flex_ka_gap_is_slow('\r', t, t + 59), "PC/Flexnet after a 59 s link stall");
    ok(!flex_ka_gap_is_slow(' ', t, t + 300), "(X)Net shape is never a slow poller");
    ok(!flex_ka_gap_is_slow(0, t, t + 300), "no shape seen yet");
    ok(!flex_ka_gap_is_slow('\r', 0, t), "first keepalive of a session: no gap");
    ok(!flex_ka_gap_is_slow('\r', t, t - 5), "clock stepped back");
    ok(FLEXNET_SLOW_POLL_GAP > 59 && FLEXNET_SLOW_POLL_GAP < 300,
       "gap sits between PC/Flexnet's worst stall and WAMPES's poll");
}

static void test_interval(void)
{
    ok(flex_lt_interval('\r', TRUE) == 0, "slow poller: every poll answered");
    ok(flex_lt_interval('\r', FALSE) == FLEXNET_LT_INTERVAL_PCF, "PC/Flexnet: 320 s");
    ok(flex_lt_interval(' ', FALSE) == FLEXNET_LT_INTERVAL_XNET, "(X)Net: 20 s");
    ok(flex_lt_interval(0, FALSE) == FLEXNET_LT_INTERVAL_XNET, "unknown: 20 s");
    ok(FLEXNET_LT_INTERVAL_PCF == 320, "PC/Flexnet window stays 320 s (v2.1.31)");
    ok(FLEXNET_LT_INTERVAL_PCF > 300,
       "the window that starved WAMPES's 300 s poll is still there for PC/Flexnet");
}

int main(void)
{
    test_gap();
    test_interval();
    printf("test_slow_poll: %d/%d passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
