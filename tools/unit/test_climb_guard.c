/* Count-to-infinity containment for the advertisement decision rule.
 *
 * The defect this guards: the decision rule fires on a 10 % relative
 * move, which is correct for a cost that wanders and wrong for one that
 * is looping. A distance-vector loop multiplies the cost by a roughly
 * constant factor each lap, so every rung of the ladder clears 10 % and
 * buys a frame.
 *
 * Measured on IR2UFV -> IW2OHX-12, 8.6 h, 2026-09-20: 43 of 204
 * destinations climbed geometrically and produced 35.7 % of the 6701
 * records we pushed at a peer that sent us 481. The series in
 * test_real_k1ymi_ladder() are the costs we actually put on the wire.
 *
 * The function under test is extracted verbatim from FlexNetCode.c by
 * tools/unit/extract.sh, so the test cannot drift from shipped code.
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

#include "extracted.inc"

static int failures = 0, checks = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

/* Drive a whole cost series through the guard, as flex_advertise_check
 * does: `last` is what we advertised previously, and a withdrawal is
 * counted but does not become the next `last`. Returns the number of
 * times the guard said "withdraw". */
static int run_series(const int *vals, int n, int *out_first_trip)
{
    int floor_ = -1, steps = 0, last = -1, trips = 0;
    if (out_first_trip) *out_first_trip = -1;
    for (int i = 0; i < n; i++)
    {
        if (flex_climb_is_loop(&floor_, &steps, last, vals[i]))
        {
            trips++;
            if (out_first_trip && *out_first_trip < 0) *out_first_trip = i;
            last = -1;              /* withdrawn; nothing advertised now */
        }
        else
        {
            last = vals[i];
        }
    }
    return trips;
}

/* A steady route must never trip the guard, however long it runs. */
static void test_steady_route_never_trips(void)
{
    int vals[64];
    for (int i = 0; i < 64; i++) vals[i] = 17;
    ok(run_series(vals, 64, NULL) == 0, "steady cost 17 never withdrawn");

    /* Our own direct neighbours, the two most-advertised destinations
       in the capture: IR2UFV/08 cost 1 (258x) and IW2OHX/>> cost 2. */
    for (int i = 0; i < 64; i++) vals[i] = 1;
    ok(run_series(vals, 64, NULL) == 0, "direct neighbour cost 1 never withdrawn");
}

/* Ordinary jitter around a mean is not a climb: the falls re-floor it. */
static void test_jitter_never_trips(void)
{
    const int vals[] = { 100, 118, 96, 131, 104, 88, 127, 99, 140, 91,
                         112, 103, 135, 97, 120, 108, 92, 129, 101, 115 };
    ok(run_series(vals, (int)(sizeof vals / sizeof *vals), NULL) == 0,
       "jitter within ~1.6x of the floor never withdrawn");
}

/* One honest re-route onto a worse path is a single rise, not a loop —
 * MIN_STEPS exists precisely so this survives. */
static void test_single_rerouting_survives(void)
{
    /* 20 -> 200 in one step is 10x the floor, but only one rise. */
    const int vals[] = { 20, 20, 20, 200, 200, 200, 200 };
    ok(run_series(vals, (int)(sizeof vals / sizeof *vals), NULL) == 0,
       "single 10x re-route not treated as a loop");

    /* Two rises still under MIN_STEPS. */
    const int two[] = { 20, 60, 200, 200, 200 };
    ok(run_series(two, (int)(sizeof two / sizeof *two), NULL) == 0,
       "two consecutive rises do not trip MIN_STEPS=3");
}

/* The ladder: three consecutive rises reaching RATIO x floor. */
static void test_geometric_climb_trips(void)
{
    /* floor=100, rises at every step, but the series tops out at 384 —
       an integer ratio of 3. Both conditions must hold, so a ladder
       that never reaches 4x the floor is left alone. This is the
       boundary, and it is deliberate: a route that settles at 3x its
       best is expensive, not looping. */
    int first = -1;
    const int vals[] = { 100, 140, 196, 274, 384 };
    ok(run_series(vals, (int)(sizeof vals / sizeof *vals), &first) == 0,
       "climb that stays under RATIO x floor is not withdrawn");

    /* A ladder that clearly passes 4x. */
    int f2 = -1;
    const int steep[] = { 100, 150, 225, 340, 510 };
    ok(run_series(steep, (int)(sizeof steep / sizeof *steep), &f2) == 1,
       "steeper ladder withdrawn once");
    ok(f2 == 4 && steep[f2] == 510, "trips at the first rung >= 4x floor");
}

/* The real K1YMI/0? series from the capture. It must be contained, and
 * the guard must still let the route settle when it genuinely falls. */
static void test_real_k1ymi_ladder(void)
{
    const int vals[] = {
        270, 2660, 270, 342, 385, 433, 548, 781, 1113, 2257, 1783, 798,
        1136, 1618, 2304, 173, 137, 173, 137, 154, 220, 314, 248, 279,
        447, 637, 807, 459, 315, 355, 506, 577, 720, 1040, 1481, 1642,
        2078, 2338, 2959, 3329, 1898, 2135, 2702, 3848, 4329
    };
    int n = (int)(sizeof vals / sizeof *vals);
    int trips = run_series(vals, n, NULL);
    ok(trips >= 3, "real K1YMI ladder is caught repeatedly");
    /* Containment is the point: without the guard all 45 rungs go on the
       wire. Each trip replaces a ladder with one withdrawal. */
    printf("  note: K1YMI series of %d rungs produced %d withdrawals\n",
           n, trips);
}

/* A withdrawal must never be fed back into the detector as a cost. */
static void test_infinity_is_not_a_cost(void)
{
    int floor_ = 10, steps = 0;
    ok(flex_climb_is_loop(&floor_, &steps, 10, FLEXNET_RTT_INFINITY) == FALSE,
       "infinity never trips the guard");
    ok(floor_ == 10 && steps == 0, "infinity leaves the state untouched");
}

/* After a trip the state is clean, so a genuinely returning route is
 * judged on its own merits rather than the loop that preceded it. */
static void test_state_resets_after_trip(void)
{
    int floor_ = -1, steps = 0;
    const int climb[] = { 50, 100, 200, 400 };
    int last = -1;
    for (int i = 0; i < 4; i++)
    {
        if (flex_climb_is_loop(&floor_, &steps, last, climb[i])) break;
        last = climb[i];
    }
    ok(floor_ == -1 && steps == 0, "floor and step count reset on trip");
    /* The route comes back cheap and steady: must not trip again. */
    int vals[16];
    for (int i = 0; i < 16; i++) vals[i] = 12;
    int f2 = floor_, s2 = steps, l2 = -1, trips = 0;
    for (int i = 0; i < 16; i++)
    {
        if (flex_climb_is_loop(&f2, &s2, l2, vals[i])) trips++;
        else l2 = vals[i];
    }
    ok(trips == 0, "a recovered steady route is not withdrawn again");
}

/* The wire clamp: no finite cost may leave as more than the peer can
 * represent, and the withdrawal sentinel must pass through intact. */
static void test_wire_clamp(void)
{
    unsigned char buf[64];
    int n = flex_build_route_rec(buf, sizeof buf, "K1YMI", 0, 15, 4910);
    ok(n > 0, "over-max record still builds");
    ok(memcmp(buf, "K1YMI 0?4095 ", (size_t)n) == 0,
       "finite cost above the 12-bit field is clamped to 4095");

    n = flex_build_route_rec(buf, sizeof buf, "K1YMI", 0, 15,
                             FLEXNET_RTT_INFINITY);
    ok(n > 0 && memcmp(buf, "K1YMI 0?60000 ", (size_t)n) == 0,
       "withdrawal sentinel passes through unclamped");

    n = flex_build_route_rec(buf, sizeof buf, "IR2UFV", 0, 8, 1);
    ok(n > 0 && memcmp(buf, "IR2UFV081 ", (size_t)n) == 0,
       "ordinary cost unchanged");
}

int main(void)
{
    /* extract.sh emits one shared .inc for every test here; this one
       exercises the decision-rule half. Reference the wire-format
       functions so the shared include stays warning-free without
       giving extract.sh a per-test mode. */
    (void)flex_build_route;
    (void)flex_parse_compact_records;

    printf("test_climb_guard\n");
    test_steady_route_never_trips();
    test_jitter_never_trips();
    test_single_rerouting_survives();
    test_geometric_climb_trips();
    test_real_k1ymi_ladder();
    test_infinity_is_not_a_cost();
    test_state_resets_after_trip();
    test_wire_clamp();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
