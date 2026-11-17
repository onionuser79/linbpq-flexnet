/* v2.11 `D <call> *`: one row per FlexNet neighbour.
 *
 * The first two columns must read like (X)Net's own `D <call> *`. The
 * expected prefixes below are rows captured from (X)Net 1.39 on
 * 2026-11-17 (research/xnet_capture_2026-11-17/LIVE_TEST.md); what
 * follows them is linbpq-flexnet's own detail (next hop, rx, tx, ages).
 *
 * Extracted verbatim from FlexNetCode.c by tools/unit/extract.sh.
 */
#include <stdio.h>
#include <string.h>

typedef int BOOL;
#define TRUE  1
#define FALSE 0

#include "extracted_dest_alts.inc"

static int failures = 0, checks = 0;
static void ok(int cond, const char * what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static void prefix(const char * row, const char * want, const char * what)
{
    int good = strncmp(row, want, strlen(want)) == 0;
    ok(good, what);
    if (!good) printf("    got  [%s]\n    want [%s...]\n", row, want);
}

static void test_xnet_columns(void)
{
    char r[96];
    /* Offers (positive). */
    flex_dest_alt_row(r, sizeof(r), "HB9ON-15", 19, 5, -1, 0, TRUE);
    prefix(r, "    HB9ON-15     19", "offer: HB9ON-15 19");
    flex_dest_alt_row(r, sizeof(r), "IR2UFV", 2001, 5, -1, 0, FALSE);
    prefix(r, "    IR2UFV     2001", "penalised offer: IR2UFV 2001");
    flex_dest_alt_row(r, sizeof(r), "IR3UHU-2", 357, 5, 2, 9, FALSE);
    prefix(r, "    IR3UHU-2    357", "offer wins over tx: IR3UHU-2 357");
    /* No offer: what we advertised, negative. */
    flex_dest_alt_row(r, sizeof(r), "IR1UAW-10", -1, 0, 331, 40, FALSE);
    prefix(r, "    IR1UAW-10  -331", "tx: 9-char call -331");
    flex_dest_alt_row(r, sizeof(r), "IW2OHX-4", -1, 0, 22, 40, FALSE);
    prefix(r, "    IW2OHX-4    -22", "tx: IW2OHX-4 -22");
    flex_dest_alt_row(r, sizeof(r), "IW2OHX-13", -1, 0, 2, 40, FALSE);
    prefix(r, "    IW2OHX-13    -2", "tx: IW2OHX-13 -2");
}

static void test_detail_columns(void)
{
    char r[96];
    flex_dest_alt_row(r, sizeof(r), "NODEA", 19, 125, 22, 40, TRUE);
    ok(strcmp(r, "    NODEA        19 * rx 19 2m05s       tx 22 40s") == 0,
       "next hop with both directions");
    if (strcmp(r, "    NODEA        19 * rx 19 2m05s       tx 22 40s") != 0)
        printf("    got [%s]\n", r);
    flex_dest_alt_row(r, sizeof(r), "NODEB", -1, 0, -1, 0, FALSE);
    ok(strcmp(r, "    NODEB         -   rx -              tx -") == 0,
       "neither direction: '-'");
    if (strcmp(r, "    NODEB         -   rx -              tx -") != 0)
        printf("    got [%s]\n", r);
    flex_dest_alt_row(r, sizeof(r), "NODEC", 0 + 1, 0, 0, 3, FALSE);
    ok(strstr(r, "tx 0 3s") != NULL, "tx of 0 is shown, not hidden");
}

static void test_tiny_buffer(void)
{
    char r[8];
    memset(r, 'X', sizeof(r));
    flex_dest_alt_row(r, sizeof(r), "IR1UAW-10", 357, 5, 22, 9, TRUE);
    ok(r[sizeof(r) - 1] == '\0', "truncates, stays terminated");
}

static void test_age(void)
{
    char a[16];
    flex_dest_age(0, a, sizeof(a));          ok(strcmp(a, "0s") == 0, "0s");
    flex_dest_age(59, a, sizeof(a));         ok(strcmp(a, "59s") == 0, "59s");
    flex_dest_age(60, a, sizeof(a));         ok(strcmp(a, "1m00s") == 0, "1m00s");
    flex_dest_age(3599, a, sizeof(a));       ok(strcmp(a, "59m59s") == 0, "59m59s");
    flex_dest_age(3600, a, sizeof(a));       ok(strcmp(a, "1h00m") == 0, "1h00m");
    flex_dest_age(86399, a, sizeof(a));      ok(strcmp(a, "23h59m") == 0, "23h59m");
    flex_dest_age(86400, a, sizeof(a));      ok(strcmp(a, "1d00h") == 0, "1d00h");
    flex_dest_age(-5, a, sizeof(a));         ok(strcmp(a, "0s") == 0, "clock step back: 0s");
}

int main(void)
{
    test_xnet_columns();
    test_detail_columns();
    test_tiny_buffer();
    test_age();
    printf("test_dest_alts: %d/%d passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
