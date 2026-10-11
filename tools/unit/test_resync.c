/* B-1 (v2.6.1): learned routes have no lifetime; a peer that silently
 * forgets one is caught by a full-table request ('3+', own records, '3-')
 * and a sweep of what the answer left out.
 *
 * Pins the places this can go wrong on the live network:
 *   - (X)Net never closes its answer with '3-': the quiet window must end it;
 *   - LinBPQ peers send '3-' after every pushed batch: a '3-' that arrives
 *     before the answer must not trigger a sweep;
 *   - a short or missing answer must drop NOTHING;
 *   - withdrawn routes and the neighbour's own entry are never counted
 *     against the answer and never swept;
 *   - a generation counter wrap must not make never-named routes look named.
 *
 * Extracted verbatim from FlexNetCode.c by tools/unit/extract.sh.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define FLEXNET_MAX_CALLSIGN               10
#define FLEXNET_RTT_INFINITY               60000
#define FLEXNET_MAX_LEARNED_PER_NEIGHBOUR  64

typedef int BOOL;
#define TRUE  1
#define FALSE 0

static int g_info_calls = 0;
#define FlexNet_Info(...) (g_info_calls++)

#include "extracted_resync.inc"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #c); } } while (0)

static struct FLEXNET_LEARNED_ROUTE * add(struct FLEXNET_LEARNED_STATE * st,
                                          const char * call, int rtt)
{
    struct FLEXNET_LEARNED_ROUTE * r = &st->routes[st->count++];
    memset(r, 0, sizeof(*r));
    strncpy(r->dest_call, call, FLEXNET_MAX_CALLSIGN - 1);
    r->rtt_at_neighbour = rtt;
    return r;
}

/* 4 finite routes, 1 withdrawn, the neighbour's own entry. */
static void setup(struct FLEXNET_LEARNED_STATE * st)
{
    memset(st, 0, sizeof(*st));
    add(st, "DB0AAT", 10);
    add(st, "DB0LHR", 20);
    add(st, "HB9AK", 30);
    add(st, "IK6IHL", 40);
    add(st, "GONE", FLEXNET_RTT_INFINITY);
    add(st, "IW2OHX", 1)->is_direct_neighbour = TRUE;
}

static int count_stale(const struct FLEXNET_LEARNED_STATE * st)
{
    int n = 0;
    for (int i = 0; i < st->count; i++)
        if (flex_resync_is_stale(st, &st->routes[i])) n++;
    return n;
}

static void test_note_outside_request_is_ignored(void)
{
    struct FLEXNET_LEARNED_STATE st;
    setup(&st);
    flex_resync_note(&st, &st.routes[0], 100);
    CHECK(st.routes[0].resync_seen == 0);
    CHECK(st.resync_last_rec == 0);
    CHECK(flex_resync_verdict(&st, 100) == FLEX_RESYNC_WAIT);
}

static void test_xnet_answer_ends_on_quiet(void)
{
    struct FLEXNET_LEARNED_STATE st;
    setup(&st);
    flex_resync_begin(&st, 1000);
    for (int i = 0; i < 3; i++)                 /* HB9AK... all but IK6IHL */
        flex_resync_note(&st, &st.routes[i], 1002);
    CHECK(flex_resync_verdict(&st, 1002 + FLEXNET_RESYNC_QUIET - 1)
          == FLEX_RESYNC_WAIT);
    CHECK(flex_resync_verdict(&st, 1002 + FLEXNET_RESYNC_QUIET)
          == FLEX_RESYNC_SWEEP);
    /* IK6IHL and the withdrawn GONE are stale; the neighbour never is. */
    CHECK(count_stale(&st) == 2);
    CHECK(!flex_resync_is_stale(&st, &st.routes[5]));
    CHECK(flex_resync_is_stale(&st, &st.routes[3]));
}

static void test_early_eob_does_not_sweep(void)
{
    struct FLEXNET_LEARNED_STATE st;
    setup(&st);
    flex_resync_begin(&st, 1000);
    flex_resync_note(&st, &st.routes[0], 1001); /* one pushed record ... */
    st.resync_eob = TRUE;                       /* ... and its batch '3-' */
    CHECK(flex_resync_verdict(&st, 1001) == FLEX_RESYNC_WAIT);
    st.resync_eob = FALSE;                      /* what service() does */
    for (int i = 0; i < 4; i++)                 /* the real answer */
        flex_resync_note(&st, &st.routes[i], 1003);
    st.resync_eob = TRUE;
    CHECK(flex_resync_verdict(&st, 1003) == FLEX_RESYNC_SWEEP);
    CHECK(count_stale(&st) == 1);               /* only GONE */
}

static void test_short_answer_aborts(void)
{
    struct FLEXNET_LEARNED_STATE st;
    setup(&st);
    flex_resync_begin(&st, 1000);
    flex_resync_note(&st, &st.routes[0], 1001); /* 1 of 4 < 50 % */
    CHECK(flex_resync_verdict(&st, 1001 + FLEXNET_RESYNC_QUIET)
          == FLEX_RESYNC_ABORT);
}

static void test_threshold_boundary(void)
{
    struct FLEXNET_LEARNED_STATE st;
    setup(&st);
    flex_resync_begin(&st, 1000);
    flex_resync_note(&st, &st.routes[0], 1001);
    flex_resync_note(&st, &st.routes[1], 1001); /* 2 of 4 = 50 % */
    st.resync_eob = TRUE;
    CHECK(flex_resync_verdict(&st, 1001) == FLEX_RESYNC_SWEEP);
}

static void test_no_answer_times_out(void)
{
    struct FLEXNET_LEARNED_STATE st;
    setup(&st);
    flex_resync_begin(&st, 1000);
    CHECK(flex_resync_verdict(&st, 1000 + FLEXNET_RESYNC_TIMEOUT - 1)
          == FLEX_RESYNC_WAIT);
    CHECK(flex_resync_verdict(&st, 1000 + FLEXNET_RESYNC_TIMEOUT)
          == FLEX_RESYNC_ABORT);
}

static void test_withdrawn_and_neighbour_not_counted(void)
{
    struct FLEXNET_LEARNED_STATE st;
    setup(&st);
    int e = -1, n = -1;
    flex_resync_begin(&st, 1000);
    flex_resync_tally(&st, &e, &n);
    CHECK(e == 4);
    CHECK(n == 0);
}

static void test_empty_table(void)
{
    struct FLEXNET_LEARNED_STATE st;
    memset(&st, 0, sizeof(st));
    add(&st, "IW2OHX", 1)->is_direct_neighbour = TRUE;
    flex_resync_begin(&st, 1000);
    flex_resync_note(&st, &st.routes[0], 1001);
    CHECK(flex_resync_verdict(&st, 1001 + FLEXNET_RESYNC_QUIET)
          == FLEX_RESYNC_SWEEP);
    CHECK(count_stale(&st) == 0);
}

static void test_generation_wrap(void)
{
    struct FLEXNET_LEARNED_STATE st;
    setup(&st);
    st.resync_gen = 0xFFFFFFFFu;
    flex_resync_begin(&st, 1000);
    CHECK(st.resync_gen == 1);
    /* routes never named carry 0, which must not equal the new gen */
    CHECK(count_stale(&st) == 5);
}

static void test_previous_request_marks_do_not_count(void)
{
    struct FLEXNET_LEARNED_STATE st;
    setup(&st);
    flex_resync_begin(&st, 1000);
    for (int i = 0; i < 4; i++)
        flex_resync_note(&st, &st.routes[i], 1001);
    st.resync_active = FALSE;
    flex_resync_begin(&st, 5000);               /* next hour */
    int e, n;
    flex_resync_tally(&st, &e, &n);
    CHECK(n == 0);
}

static void test_parse_int_line(void)
{
    int v = 7;
    CHECK(flex_parse_int_line("FLEXNETRESYNC 60\r\n", "FLEXNETRESYNC",
                              0, 1440, &v) == 1 && v == 60);
    CHECK(flex_parse_int_line("  flexnetresync=0", "FLEXNETRESYNC",
                              0, 1440, &v) == 1 && v == 0);
    CHECK(flex_parse_int_line("FLEXNETRESYNC 120 ; every 2 h",
                              "FLEXNETRESYNC", 0, 1440, &v) == 1 && v == 120);
    v = 7;
    int before = g_info_calls;
    CHECK(flex_parse_int_line("FLEXNETRESYNC 9999", "FLEXNETRESYNC",
                              0, 1440, &v) == 1 && v == 7);
    CHECK(flex_parse_int_line("FLEXNETRESYNC abc", "FLEXNETRESYNC",
                              0, 1440, &v) == 1 && v == 7);
    CHECK(flex_parse_int_line("FLEXNETRESYNC -5", "FLEXNETRESYNC",
                              0, 1440, &v) == 1 && v == 7);
    CHECK(g_info_calls == before + 3);
    CHECK(flex_parse_int_line("FLEXNETRESYNCX 5", "FLEXNETRESYNC",
                              0, 1440, &v) == 0 && v == 7);
    CHECK(flex_parse_int_line("; FLEXNETRESYNC 5", "FLEXNETRESYNC",
                              0, 1440, &v) == 0 && v == 7);
    CHECK(flex_parse_int_line("", "FLEXNETRESYNC", 0, 1440, &v) == 0);
}

int main(void)
{
    test_note_outside_request_is_ignored();
    test_xnet_answer_ends_on_quiet();
    test_early_eob_does_not_sweep();
    test_short_answer_aborts();
    test_threshold_boundary();
    test_no_answer_times_out();
    test_withdrawn_and_neighbour_not_counted();
    test_empty_table();
    test_generation_wrap();
    test_previous_request_marks_do_not_count();
    test_parse_int_line();
    if (failures) { printf("test_resync: %d FAILED\n", failures); return 1; }
    printf("test_resync: all passed\n");
    return 0;
}
