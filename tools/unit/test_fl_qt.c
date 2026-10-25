/* FL "Q/T" column: our own smoothed link time to a FlexNet peer, in the
 * unit (X)Net's L prints under Q/T (100 ms ticks), "-" until measured.
 *
 * Extracted verbatim from FlexNetCode.c by tools/unit/extract.sh.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "extracted_fl_qt.inc"

static int failures = 0, checks = 0;
static void ok(int cond, const char * what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static void test_unmeasured(void)
{
    char buf[8];
    flex_fl_qt_cell(buf, sizeof(buf), 0, 2);
    ok(strcmp(buf, "-") == 0, "no sample yet: '-', not the InitSession seed");
    flex_fl_qt_cell(buf, sizeof(buf), 0, 0);
    ok(strcmp(buf, "-") == 0, "no sample, zero value: still '-'");
}

static void test_ticks(void)
{
    char buf[8];
    flex_fl_qt_cell(buf, sizeof(buf), 1, 1);
    ok(strcmp(buf, "1") == 0, "floor value 1 = 100 ms, printed as a bare tick count");
    flex_fl_qt_cell(buf, sizeof(buf), 7, 3);
    ok(strcmp(buf, "3") == 0, "3 ticks prints 3, as (X)Net Q/T would");
    flex_fl_qt_cell(buf, sizeof(buf), UINT32_MAX, 4095);
    ok(strcmp(buf, "4095") == 0, "12-bit cap fits the cell");
}

static void test_fits_column(void)
{
    char buf[8];
    flex_fl_qt_cell(buf, sizeof(buf), 1, 4095);
    ok(strlen(buf) <= 5, "widest value fits the 5-char FL column");
    char tiny[2];
    flex_fl_qt_cell(tiny, sizeof(tiny), 1, 4095);
    ok(tiny[1] == '\0', "short buffer is truncated and terminated");
}

int main(void)
{
    test_unmeasured();
    test_ticks();
    test_fits_column();
    printf("test_fl_qt: %d/%d passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
