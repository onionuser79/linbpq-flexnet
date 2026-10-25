/* The value our type-1 (link time) frames carry: our own smoothed link
 * time to (X)Net and LinBPQ peers, the fixed FLEXNET_WIRE_LT to
 * PC/Flexnet peers and before the first measurement.
 *
 * Extracted verbatim from FlexNetCode.c by tools/unit/extract.sh.
 */
#include <stdint.h>
#include <stdio.h>

typedef int BOOL;
#define TRUE 1
#define FALSE 0

#include "extracted_wire_lt.inc"

static int failures = 0, checks = 0;
static void ok(int cond, const char * what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static void test_value(void)
{
    ok(flex_wire_lt_value(FALSE, 3, 1) == 1, "(X)Net peer, measured 1: send 1");
    ok(flex_wire_lt_value(FALSE, 3, 133) == 133, "RF link measured 13.3 s: send 133");
    ok(flex_wire_lt_value(TRUE, 3, 133) == FLEXNET_WIRE_LT, "PC/Flexnet peer: constant");
    ok(flex_wire_lt_value(FALSE, 0, 2) == FLEXNET_WIRE_LT,
       "no round trip yet: constant, not the InitSession seed");
    ok(flex_wire_lt_value(FALSE, 1, 0) == 1, "never below 1");
    ok(flex_wire_lt_value(FALSE, 1, -7) == 1, "negative clamps to 1");
    ok(flex_wire_lt_value(FALSE, 1, 9999) == 4095, "12-bit cap");
    ok(FLEXNET_WIRE_LT == 5, "PC/Flexnet constant stays 5 (v2.1.28 link.ts tuning)");
}

int main(void)
{
    test_value();
    printf("test_wire_lt: %d/%d passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
