/* v2.11 FLSTAT: now/min/max gauges and per-neighbour session counters.
 *
 * Pins the two things that make the numbers worth reading:
 *   - min counts from the first non-zero sample, else every gauge would
 *     report min 0 from the seconds before the first link came up;
 *   - ups - downs stays 0 or 1 per neighbour even when an end is missed
 *     (a new session over one we never saw close) or reported twice
 *     (closed by L2, then reaped).
 *
 * Extracted verbatim from FlexNetCode.c by tools/unit/extract.sh.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

typedef int BOOL;
#define TRUE  1
#define FALSE 0

#include "extracted_stats.inc"

static int failures = 0, checks = 0;
static void ok(int cond, const char * what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static void test_gauge(void)
{
    struct FLEX_STAT_GAUGE g;
    memset(&g, 0, sizeof(g));
    flex_stat_gauge(&g, 0, 100);
    flex_stat_gauge(&g, 0, 110);
    ok(!g.seen && g.now == 0, "zeros before convergence: not seen");
    flex_stat_gauge(&g, 127, 120);
    ok(g.seen && g.min == 127 && g.max == 127 && g.max_at == 120,
       "first non-zero sets min = max");
    flex_stat_gauge(&g, 129, 130);
    flex_stat_gauge(&g, 2, 140);
    flex_stat_gauge(&g, 128, 150);
    ok(g.now == 128, "now = last sample");
    ok(g.min == 2, "min after convergence (link lost: 2)");
    ok(g.max == 129 && g.max_at == 130, "max and when");
    flex_stat_gauge(&g, 0, 160);
    ok(g.min == 0, "zero after convergence does count");
    flex_stat_gauge(&g, 129, 170);
    ok(g.max_at == 130, "equal max keeps the first time");
}

static void test_link_events(void)
{
    struct FLEX_STAT_NBR t[3];
    memset(t, 0, sizeof(t));
    flex_stat_link_event(t, 3, "NODEA", TRUE, 10);
    flex_stat_link_event(t, 3, "NODEA", FALSE, 20);
    flex_stat_link_event(t, 3, "NODEA", FALSE, 21);   /* closed, then reaped */
    struct FLEX_STAT_NBR * a = flex_stat_nbr(t, 3, "nodea", FALSE);
    ok(a != NULL, "lookup is case-insensitive");
    ok(a && a->ups == 1 && a->downs == 1, "double end counted once");
    ok(a && a->last_down == 20 && !a->up, "first end's time kept");

    flex_stat_link_event(t, 3, "NODEA", TRUE, 30);
    flex_stat_link_event(t, 3, "NODEA", TRUE, 40);    /* end never seen */
    ok(a && a->ups == 3 && a->downs == 2, "missed end inferred");
    ok(a && a->up && a->last_up == 40 && a->last_down == 40,
       "inferred end at the new start");
}

static void test_down_unknown_ignored(void)
{
    struct FLEX_STAT_NBR t[2];
    memset(t, 0, sizeof(t));
    flex_stat_link_event(t, 2, "NODEB", FALSE, 5);
    ok(flex_stat_nbr(t, 2, "NODEB", FALSE) == NULL,
       "end for an unknown neighbour creates no row");
    flex_stat_link_event(t, 2, NULL, TRUE, 5);
    flex_stat_link_event(t, 2, "", TRUE, 5);
    ok(!t[0].call[0] && !t[1].call[0], "empty call ignored");
}

static void test_table_full(void)
{
    struct FLEX_STAT_NBR t[2];
    memset(t, 0, sizeof(t));
    flex_stat_link_event(t, 2, "NODEA", TRUE, 1);
    flex_stat_link_event(t, 2, "NODEB", TRUE, 1);
    flex_stat_link_event(t, 2, "NODEC", TRUE, 1);
    ok(flex_stat_nbr(t, 2, "NODEC", FALSE) == NULL, "full: third not counted");
    ok(strcmp(t[0].call, "NODEA") == 0 && strcmp(t[1].call, "NODEB") == 0,
       "full: existing rows not overwritten");
}

static void test_long_call_truncated(void)
{
    struct FLEX_STAT_NBR t[1];
    memset(t, 0, sizeof(t));
    flex_stat_link_event(t, 1, "ABCDEFGHIJKLMNOP", TRUE, 1);
    ok(t[0].call[sizeof(t[0].call) - 1] == '\0', "call stays terminated");
}

int main(void)
{
    test_gauge();
    test_link_events();
    test_down_unknown_ignored();
    test_table_full();
    test_long_call_truncated();
    printf("test_stats: %d/%d passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
