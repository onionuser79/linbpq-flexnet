/* B-5 (v2.6.1): the FlexNet session reaper must not kill a session whose
 * L2 link is merely in REJ-sent state, and must allow seconds - not
 * 100 ms timer ticks - for a link to recover.
 *
 * BPQ's L2STATE values: 0 free, 1 XID sent, 2 SABM sent, 3 FRMR,
 * 4 disconnecting, 5 connected, 6 connected with REJ outstanding.
 * BPQ itself treats >= 5 as connected.
 *
 * Extracted verbatim from FlexNetCode.c by tools/unit/extract.sh.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef int BOOL;
typedef unsigned char UCHAR;
#define TRUE  1
#define FALSE 0

typedef struct { UCHAR LINKCALL[7]; int L2STATE; } LINKTABLE;

#include "extracted_reaper.inc"

static int failures = 0, checks = 0;
static void ok(int cond, const char * what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static LINKTABLE link_in(int state)
{
    LINKTABLE L;
    memset(&L, 0, sizeof(L));
    L.LINKCALL[0] = (UCHAR)('I' << 1);
    L.L2STATE = state;
    return L;
}

static void test_connected_states(void)
{
    LINKTABLE l5 = link_in(5), l6 = link_in(6), l4 = link_in(4), l2 = link_in(2);
    ok(flex_l2_connected(&l5), "5 is connected");
    ok(flex_l2_connected(&l6), "6 (REJ outstanding) is connected");
    ok(!flex_l2_connected(&l4), "4 (disconnecting) is not");
    ok(!flex_l2_connected(&l2), "2 (SABM sent) is not");
    ok(!flex_l2_connected(NULL), "no LINK is not");
}

static void test_rej_state_never_reaped(void)
{
    LINKTABLE L = link_in(6);
    time_t bad = 0;
    for (time_t t = 1000; t < 1000 + 600; t++)
        if (flex_reap_decide(&L, &bad, t) != FLEX_REAP_KEEP)
        {
            ok(0, "REJ state reaped");
            return;
        }
    ok(bad == 0, "REJ state: no bad clock running");
}

static void test_grace_is_seconds(void)
{
    LINKTABLE L = link_in(4);
    time_t bad = 0;
    /* 100 ms ticks within one second: the old 3-strike rule fired here */
    ok(flex_reap_decide(&L, &bad, 1000) == FLEX_REAP_WAIT, "t+0 waits");
    ok(flex_reap_decide(&L, &bad, 1000) == FLEX_REAP_WAIT, "t+0.1 waits");
    ok(flex_reap_decide(&L, &bad, 1000) == FLEX_REAP_WAIT, "t+0.2 waits");
    ok(flex_reap_decide(&L, &bad, 1000 + FLEXNET_REAP_GRACE - 1)
       == FLEX_REAP_WAIT, "just inside grace waits");
    ok(flex_reap_decide(&L, &bad, 1000 + FLEXNET_REAP_GRACE)
       == FLEX_REAP_NOW, "grace over: reap");
}

static void test_recovery_resets_clock(void)
{
    LINKTABLE L = link_in(4);
    time_t bad = 0;
    flex_reap_decide(&L, &bad, 1000);
    L.L2STATE = 5;
    ok(flex_reap_decide(&L, &bad, 1003) == FLEX_REAP_KEEP, "recovered: keep");
    ok(bad == 0, "recovered: clock cleared");
    L.L2STATE = 4;
    ok(flex_reap_decide(&L, &bad, 1004) == FLEX_REAP_WAIT,
       "second blip starts its own grace");
    ok(flex_reap_decide(&L, &bad, 1004 + FLEXNET_REAP_GRACE - 1)
       == FLEX_REAP_WAIT, "second blip: still inside its grace");
}

static void test_no_link_is_immediate(void)
{
    time_t bad = 0;
    ok(flex_reap_decide(NULL, &bad, 1000) == FLEX_REAP_NOW, "no LINK: at once");
}

static void test_cleared_linkcall_is_bad(void)
{
    LINKTABLE L = link_in(5);
    L.LINKCALL[0] = 0;                          /* BPQ zeroed the slot */
    time_t bad = 0;
    ok(flex_reap_decide(&L, &bad, 1000) == FLEX_REAP_WAIT,
       "zeroed slot: grace for the migration to find the new one");
    ok(flex_reap_decide(&L, &bad, 1000 + FLEXNET_REAP_GRACE) == FLEX_REAP_NOW,
       "zeroed slot: reaped after grace");
}

int main(void)
{
    test_connected_states();
    test_rej_state_never_reaped();
    test_grace_is_seconds();
    test_recovery_resets_clock();
    test_no_link_is_immediate();
    test_cleared_linkcall_is_bad();
    printf("test_reaper: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
