/* FLEXPROBE (v2.7): the sysop path-query command. Pins the argument
 * parser and the result ring that lets a production build - which logs
 * nothing - show what became of a query answered after the command
 * returned.
 *
 * Extracted verbatim from FlexNetCode.c by tools/unit/extract.sh.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef int BOOL;
#define TRUE  1
#define FALSE 0

#include "extracted_flexprobe.inc"

static int failures = 0, checks = 0;
static void ok(int cond, const char * what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static int parse(const char * in, char * call, int * ssid)
{
    return flex_parse_probe_target(in, call, 8, ssid);
}

static void test_parse_empty_lists(void)
{
    char c[8];
    int s = 99;
    ok(parse(NULL, c, &s) == -1, "NULL tail lists");
    ok(parse("", c, &s) == -1 && s == -1, "empty tail lists, ssid reset");
    ok(parse("   \r", c, &s) == -1, "blanks + CR list");
    ok(parse("\n", c, &s) == -1, "bare LF lists");
}

static void test_parse_valid(void)
{
    char c[8];
    int s;
    ok(parse("db0faa\r", c, &s) == 0 && strcmp(c, "DB0FAA") == 0 && s == -1,
       "lower case upper-cased, no ssid");
    ok(parse("  DB0FAA-3\r", c, &s) == 0 && s == 3, "leading blanks, ssid 3");
    ok(parse("DB0FAA-0", c, &s) == 0 && s == 0, "ssid 0 kept as 0, not none");
    ok(parse("DB0FAA-15", c, &s) == 0 && s == 15, "ssid 15 is the maximum");
    ok(parse("K1A", c, &s) == 0 && strcmp(c, "K1A") == 0, "short call");
    ok(parse("ABCDEF", c, &s) == 0, "6 characters is the AX.25 maximum");
    ok(parse("DB0FAA-3 extra", c, &s) == 0 && s == 3, "trailing token ignored");
}

static void test_parse_malformed(void)
{
    char c[8];
    int s;
    ok(parse("ABCDEFG", c, &s) == -2, "7 characters rejected");
    ok(parse("DB0FAA-16", c, &s) == -2, "ssid 16 rejected");
    ok(parse("DB0FAA-", c, &s) == -2, "dash without digits");
    ok(parse("DB0FAA-007", c, &s) == -2, "three ssid digits");
    ok(parse("DB0FAA-3x", c, &s) == -2, "junk after ssid");
    ok(parse("DB*", c, &s) == -2, "wildcard is not a callsign");
    ok(parse("-3", c, &s) == -2, "ssid with no call");
    ok(flex_parse_probe_target("DB0FAA", c, 6, &s) == -2,
       "buffer too small for 6 chars + NUL");
}

static void test_ring_add_wraps(void)
{
    memset(FlexNetSysopProbes, 0, sizeof(FlexNetSysopProbes));
    g_sysop_probe_next = 0;
    int first = flex_sysop_probe_add("A-1", "N", 1, 100);
    ok(first == 1, "first entry is ring 1 (0 means none)");
    for (int i = 2; i <= FLEXNET_SYSOP_PROBE_RING; i++)
        flex_sysop_probe_add("A-1", "N", i, 100);
    ok(flex_sysop_probe_add("B-2", "N", 99, 100) == 1, "9th overwrites the oldest");
    ok(FlexNetSysopProbes[0].qso == 99 && strcmp(FlexNetSysopProbes[0].target, "B-2") == 0,
       "overwritten entry holds the new probe");
    ok(FlexNetSysopProbes[0].state == FLEX_SPROBE_PENDING, "new entry pending");
}

static void test_ring_close(void)
{
    char hops[3][FLEXNET_MAX_CALLSIGN] = {"NB-1", "NC", "DEST"};
    memset(FlexNetSysopProbes, 0, sizeof(FlexNetSysopProbes));
    g_sysop_probe_next = 0;
    int r = flex_sysop_probe_add("DEST", "NB-1", 42, 1000);

    flex_sysop_probe_close(0, 42, FLEX_SPROBE_ANSWERED, hops, 3, 1002);
    ok(FlexNetSysopProbes[0].state == FLEX_SPROBE_PENDING, "ring 0 is a background probe: no-op");
    flex_sysop_probe_close(r, 41, FLEX_SPROBE_ANSWERED, hops, 3, 1002);
    ok(FlexNetSysopProbes[0].state == FLEX_SPROBE_PENDING, "QSO mismatch (row reused): no-op");
    flex_sysop_probe_close(FLEXNET_SYSOP_PROBE_RING + 1, 42, FLEX_SPROBE_ANSWERED, hops, 3, 1002);
    ok(FlexNetSysopProbes[0].state == FLEX_SPROBE_PENDING, "out-of-range ring: no-op");

    flex_sysop_probe_close(r, 42, FLEX_SPROBE_ANSWERED, hops, 3, 1002);
    const struct FLEXNET_SYSOP_PROBE * e = &FlexNetSysopProbes[0];
    ok(e->state == FLEX_SPROBE_ANSWERED && e->elapsed == 2, "answered after 2 s");
    ok(e->n_hops == 3 && strcmp(e->hops[2], "DEST") == 0, "chain copied");

    flex_sysop_probe_close(r, 42, FLEX_SPROBE_TIMEOUT, NULL, 0, 1020);
    ok(e->state == FLEX_SPROBE_ANSWERED && e->n_hops == 3,
       "a closed entry is not reopened by a later timeout");
}

static void test_ring_close_timeout_and_cap(void)
{
    char many[FLEXNET_MAX_PATH_HOPS + 4][FLEXNET_MAX_CALLSIGN];
    for (int i = 0; i < FLEXNET_MAX_PATH_HOPS + 4; i++)
        snprintf(many[i], sizeof(many[i]), "H%d", i);
    memset(FlexNetSysopProbes, 0, sizeof(FlexNetSysopProbes));
    g_sysop_probe_next = 0;
    int a = flex_sysop_probe_add("X", "N", 7, 500);
    int b = flex_sysop_probe_add("Y", "N", 8, 500);

    flex_sysop_probe_close(a, 7, FLEX_SPROBE_TIMEOUT, NULL, 0, 516);
    ok(FlexNetSysopProbes[0].state == FLEX_SPROBE_TIMEOUT &&
       FlexNetSysopProbes[0].n_hops == 0, "timeout: no chain");
    flex_sysop_probe_close(b, 8, FLEX_SPROBE_LOOP,
                           (const char (*)[FLEXNET_MAX_CALLSIGN])many,
                           FLEXNET_MAX_PATH_HOPS + 4, 501);
    ok(FlexNetSysopProbes[1].n_hops == FLEXNET_MAX_PATH_HOPS,
       "over-long chain capped at FLEXNET_MAX_PATH_HOPS");
    ok(FlexNetSysopProbes[1].state == FLEX_SPROBE_LOOP, "loop recorded");
}

int main(void)
{
    test_parse_empty_lists();
    test_parse_valid();
    test_parse_malformed();
    test_ring_add_wraps();
    test_ring_close();
    test_ring_close_timeout_and_cap();
    printf("test_flexprobe: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
