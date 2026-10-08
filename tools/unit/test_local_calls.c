/* v2.3 local APPLICATION calls as FlexNet destinations (GitHub issue #1).
 *
 * Pins the four places the feature can half-work:
 *   - config: FLEXNETLOCAL parsing, and FLEXNETLOCALAPPS NOT being eaten
 *     by it (the keyword is FLEXNETLOCAL's prefix);
 *   - black holes: an entry no APPLICATION answers, or one on NODECALL's
 *     base, is never advertised, answered for, or delivered;
 *   - the wire: node record + every local call in ONE compact frame, and
 *     with no locals the frame is byte-identical to v2.2.4's;
 *   - the answers: flex_target_is_us() must know the local list, or a
 *     type-6 for a call we advertise gets no type-7 at all.
 *
 * Extracted verbatim from FlexNetCode.c by tools/unit/extract.sh.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>

#define FLEXNET_MAX_CALLSIGN        10
#define FLEXNET_SSID_BASE           0x30
#define FLEXNET_RTT_INFINITY        60000
#define FLEXNET_RTT_WIRE_MAX        4095
#define FLEXNET_MAX_PATH_HOPS       8
#define FLEXNET_ADVERT_FRAME_BYTES  200
#define FLEXNET_MAX_LOCAL_CALLS     16
#define FLEXNET_LOCAL_BASE_MAX      6
#define FLEXNET_LOCAL_REC_BYTES     10
#define FLEX_LOCAL_UNCHECKED        0
#define FLEX_LOCAL_BOUND            1
#define FLEX_LOCAL_UNBOUND          2
#define FLEX_LOCAL_NODECALL         3
#define FLEX_LOCAL_NOPORT           4
#define FLEX_LOCAL_NOFWD            5
#define FLEX_TARGET_NODE            1
#define FLEX_TARGET_LOCAL           2

typedef int BOOL;
typedef unsigned char UCHAR;
#define TRUE  1
#define FALSE 0

#define FlexNet_Info(...) ((void)0)

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

/* AX.25 address field: 6 shifted chars + SSID byte. BPQ's own
   ConvFromAX25 writes "CALL-N", omitting "-0". */
static void ax25(unsigned char * out, const char * call, int ssid)
{
    size_t n = strlen(call);
    for (size_t i = 0; i < 6; i++)
        out[i] = (unsigned char)((i < n ? call[i] : ' ') << 1);
    out[6] = (unsigned char)(0x60 | (ssid << 1));
}

static int conv_from_ax25(const void * in, void * out)
{
    const unsigned char * a = in;
    char * o = out;
    int n = 0;
    for (int i = 0; i < 6; i++)
    {
        char c = (char)(a[i] >> 1);
        if (c != ' ') o[n++] = c;
    }
    int ssid = (a[6] >> 1) & 0x0F;
    if (ssid) n += snprintf(o + n, 8, "-%d", ssid);
    o[n] = '\0';
    return n;
}
#define ConvFromAX25(a, b) conv_from_ax25((a), (b))

/* BPQ's CompareCalls: 6 call bytes + the SSID bits, ignoring C/H/E. */
static BOOL CompareCalls(const UCHAR * a, const UCHAR * b)
{
    return memcmp(a, b, 6) == 0 && (a[6] & 0x1e) == (b[6] & 0x1e);
}

static char MYCALL[7];

#include "extracted_local.inc"

static int failures = 0, checks = 0;

static void ok(int cond, const char * what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static void reset(void)
{
    memset(FlexNetLocalCalls, 0, sizeof(FlexNetLocalCalls));
    FlexNetLocalCount = 0;
}

static const char APPLS[][FLEXNET_MAX_CALLSIGN] =
    { "SR4BBX", "SR4DXC-2", "SR4DON-8" };

static void test_parse(void)
{
    reset();
    ok(flex_parse_local_line("FLEXNETLOCAL SR4BBX\r\n") == 1,
       "FLEXNETLOCAL line is consumed");
    ok(flex_parse_local_line("flexnetlocal sr4dxc-2, SR4XYZ-15 ; two\n") == 1,
       "lower case, comma list, trailing comment");
    ok(FlexNetLocalCount == 3, "three calls parsed");
    ok(strcmp(FlexNetLocalCalls[1].base, "SR4DXC") == 0 &&
       FlexNetLocalCalls[1].ssid == 2, "base upper-cased, SSID split");
    ok(FlexNetLocalCalls[2].ssid == 15, "SSID 15 accepted");

    ok(flex_parse_local_line("FLEXNETLOCALAPPS YES\n") == 0,
       "FLEXNETLOCALAPPS is NOT eaten by the FLEXNETLOCAL prefix");
    ok(flex_parse_local_line("  ; FLEXNETLOCAL SR4AAA\n") == 0,
       "commented line ignored");
    ok(flex_parse_local_line("FLEXNETSSIDRANGE 0-8\n") == 0,
       "unrelated directive ignored");

    int before = FlexNetLocalCount;
    flex_parse_local_line("FLEXNETLOCAL SR4BBX SR4BBX-0\n");
    ok(FlexNetLocalCount == before, "duplicates (incl. -0 == bare) dropped");
    flex_parse_local_line("FLEXNETLOCAL TOOLONG1 SR4BBX-16 SR4BBX- -3 SR4/X\n");
    ok(FlexNetLocalCount == before, "invalid calls all rejected");

    reset();
    for (int i = 0; i < FLEXNET_MAX_LOCAL_CALLS + 3; i++)
    {
        char line[40];
        snprintf(line, sizeof(line), "FLEXNETLOCAL LC%04d\n", i);
        flex_parse_local_line(line);
    }
    ok(FlexNetLocalCount == FLEXNET_MAX_LOCAL_CALLS, "table caps at 16");
}

static void test_resolve(void)
{
    reset();
    flex_parse_local_line("FLEXNETLOCAL SR4BBX SR4DXC-2 SR4XYZ SR4DON-3\n");
    flex_local_resolve("SR4DON", APPLS, 3);
    ok(FlexNetLocalCalls[0].state == FLEX_LOCAL_BOUND, "bound app call");
    ok(FlexNetLocalCalls[1].state == FLEX_LOCAL_BOUND, "bound with SSID");
    ok(FlexNetLocalCalls[2].state == FLEX_LOCAL_UNBOUND,
       "no APPLICATION answers it -> not advertised (black hole guard)");
    ok(FlexNetLocalCalls[3].state == FLEX_LOCAL_NODECALL,
       "NODECALL base -> refused, FLEXNETSSIDRANGE's job");

    reset();
    flex_parse_local_line("FLEXNETLOCAL SR4DXC-3\n");
    flex_local_resolve("SR4DON", APPLS, 3);
    ok(FlexNetLocalCalls[0].state == FLEX_LOCAL_UNBOUND,
       "SSID must match exactly: SR4DXC-3 is not SR4DXC-2");

    reset();
    flex_parse_local_line("FLEXNETLOCAL SR4BBX\n");
    flex_local_collect_apps("SR4DON", APPLS, 3);
    flex_local_resolve("SR4DON", APPLS, 3);
    ok(FlexNetLocalCount == 2, "auto-walk adds SR4DXC-2, dedupes SR4BBX, "
                               "skips NODECALL base SR4DON-8");
    ok(!FlexNetLocalCalls[0].from_apps && FlexNetLocalCalls[1].from_apps,
       "explicit vs auto provenance kept");
    ok(FlexNetLocalCalls[1].state == FLEX_LOCAL_BOUND, "auto entry bound");
}

static void test_find_and_covers(void)
{
    reset();
    flex_parse_local_line("FLEXNETLOCAL SR4BBX SR4DXC-2 SR4XYZ\n");
    flex_local_resolve("SR4DON", APPLS, 3);

    ok(flex_local_find("SR4BBX") == 0, "bare call found");
    ok(flex_local_find("sr4bbx-0") == 0, "-0 and case normalised");
    ok(flex_local_find("SR4BBX-1") < 0, "other SSID of a local call is not it");
    ok(flex_local_find("SR4DXC-2") == 1, "SSID entry found");
    ok(flex_local_find("SR4XYZ") < 0, "unbound entry is never found");
    ok(flex_local_find("") < 0 && flex_local_find(NULL) < 0, "empty/NULL");

    ok(flex_local_covers("SR4DXC", 0, 15), "range covering the SSID");
    ok(flex_local_covers("sr4bbx", 0, 0), "exact, case-insensitive");
    ok(!flex_local_covers("SR4DXC", 3, 9), "range missing the SSID");
    ok(!flex_local_covers("SR4XYZ", 0, 15), "unbound: a peer may own it");
}

static void test_own_frame(void)
{
    unsigned char b[FLEXNET_ADVERT_FRAME_BYTES];
    struct FLEXNET_DEST_ENTRY out[64];

    reset();
    int n = flex_build_own_frame(b, sizeof(b), "IR2UFV", 0, 8,
                                 FlexNetLocalCalls, FlexNetLocalCount);
    ok(n == 12 && memcmp(b, "3IR2UFV081 \r", 12) == 0,
       "no locals: byte-identical to the v2.2.4 own record");

    flex_parse_local_line("FLEXNETLOCAL SR4BBX SR4XYZ SR4DXC-2\n");
    flex_local_resolve("SR4DON", APPLS, 3);
    n = flex_build_own_frame(b, sizeof(b), "SR4DON", 0, 0,
                             FlexNetLocalCalls, FlexNetLocalCount);
    printf("  frame  : %.*s  (%d bytes)\n", n - 1, b, n);
    ok(n > 0 && b[0] == '3' && b[n - 1] == '\r', "one '3', one CR");
    int threes = 0;
    for (int i = 0; i < n; i++) if (b[i] == '3' && i == 0) threes++;
    ok(threes == 1 && memchr(b + 1, '\r', (size_t)(n - 2)) == NULL,
       "no inner frame boundary");
    int got = flex_parse_compact_records(b, n, out, 64);
    ok(got == 3, "node + 2 bound locals; unbound SR4XYZ left out");
    ok(got == 3 && strcmp(out[1].callsign, "SR4BBX") == 0 &&
       out[1].ssid_lo == 0 && out[1].ssid_hi == 0 && out[1].rtt == 1,
       "SR4BBX at cost 1, single SSID");
    ok(got == 3 && strcmp(out[2].callsign, "SR4DXC") == 0 &&
       out[2].ssid_lo == 2 && out[2].ssid_hi == 2, "SR4DXC-2 as 2-2");

    /* Worst case the _Static_assert promises: 16 six-char locals. */
    reset();
    static char appls[FLEXNET_MAX_LOCAL_CALLS][FLEXNET_MAX_CALLSIGN];
    for (int i = 0; i < FLEXNET_MAX_LOCAL_CALLS; i++)
    {
        snprintf(appls[i], sizeof(appls[i]), "LC%04d-15", i);
        char line[40];
        snprintf(line, sizeof(line), "FLEXNETLOCAL %s\n", appls[i]);
        flex_parse_local_line(line);
    }
    flex_local_resolve("SR4DON", (const char (*)[FLEXNET_MAX_CALLSIGN])appls,
                       FLEXNET_MAX_LOCAL_CALLS);
    n = flex_build_own_frame(b, sizeof(b), "SR4DON", 0, 15,
                             FlexNetLocalCalls, FlexNetLocalCount);
    ok(n == 2 + (FLEXNET_MAX_LOCAL_CALLS + 1) * FLEXNET_LOCAL_REC_BYTES,
       "full table: exact worst-case size");
    ok(n <= FLEXNET_ADVERT_FRAME_BYTES, "full table fits ONE frame");
    got = flex_parse_compact_records(b, n, out, 64);
    ok(got == FLEXNET_MAX_LOCAL_CALLS + 1, "full table parses back 17");

    ok(flex_build_own_frame(b, 40, "SR4DON", 0, 15, FlexNetLocalCalls,
                            FlexNetLocalCount) == -1,
       "too small a buffer fails, never truncates silently");
    ok(flex_build_own_frame(b, 2, "SR4DON", 0, 0, NULL, 0) == -1,
       "degenerate buffer");
}

static void test_target_and_l2(void)
{
    reset();
    flex_parse_local_line("FLEXNETLOCAL SR4BBX SR4XYZ\n");
    flex_local_resolve("SR4DON", APPLS, 3);
    ax25((unsigned char *)MYCALL, "SR4DON", 0);

    ok(flex_target_is_us("SR4DON") == FLEX_TARGET_NODE, "node call");
    ok(flex_target_is_us("SR4DON-8") == FLEX_TARGET_NODE, "node base, SSID");
    ok(flex_target_is_us("SR4BBX") == FLEX_TARGET_LOCAL,
       "advertised local call is ours to answer");
    ok(flex_target_is_us("SR4XYZ") == 0, "unbound local is not answered");
    ok(flex_target_is_us("SR6DWH-11") == 0, "stranger");

    unsigned char src[7], digi[7];
    ax25(src, "SR4BBX", 0);
    ax25(digi, "SR4DON", 0);
    digi[6] |= 0x01;                         /* last address, as sent */
    ok(FlexNet_IsLocalCall(src), "IsLocalCall on an AX.25 address");
    FlexNet_MarkLocalDigi(src, digi);
    ok((digi[6] & 0x80) && (digi[6] & 0x01),
       "reply as local call via MYCALL: H-bit set, E-bit kept");

    ax25(digi, "SR6DWH", 11);
    FlexNet_MarkLocalDigi(src, digi);
    ok(!(digi[6] & 0x80), "a real digi other than us is left alone");

    ax25(src, "SR4XYZ", 0);
    ax25(digi, "SR4DON", 0);
    FlexNet_MarkLocalDigi(src, digi);
    ok(!(digi[6] & 0x80) && !FlexNet_IsLocalCall(src),
       "unbound source: untouched");

    ax25(src, "SR4DON", 3);
    FlexNet_MarkLocalDigi(src, digi);
    ok(!(digi[6] & 0x80), "node's own SSID is not a local call");

    unsigned char empty[7] = {0};
    ax25(src, "SR4BBX", 0);
    FlexNet_MarkLocalDigi(src, empty);
    FlexNet_MarkLocalDigi(NULL, digi);
    ok(empty[6] == 0, "no digi list: no write");

    reset();
    ax25(src, "SR4BBX", 0);
    ok(!FlexNet_IsLocalCall(src), "empty table: nothing is local");
}

int main(void)
{
    (void)flex_local_state_name;
    (void)flex_local_format;
    printf("test_local_calls\n");
    test_parse();
    test_resolve();
    test_find_and_covers();
    test_own_frame();
    test_target_and_l2();
    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
