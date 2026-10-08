/* v2.6 FLEXNETEXTERNAL: a plain AX.25 station advertised as ours.
 *
 * Pins the places where an external must behave like a local call and
 * the one where it must not:
 *   - config: `FLEXNETEXTERNAL CALL[-SSID] PORT`, one per line, port
 *     required; listed as FLEXNETLOCAL as well, the external wins;
 *   - advertised: in our own compact frame at cost 1, echoes of it are
 *     not learned, a type-6 for it is answered [asker, us, it];
 *   - NOT delivered here: FlexNet_IsLocalCall() is what makes L2Code.c
 *     deliver a frame to an APPLICATION instead of repeating it, and an
 *     external must be forwarded to its port instead.
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
#define FLEX_TARGET_NODE            1
#define FLEX_TARGET_LOCAL           2

typedef int BOOL;
typedef unsigned char UCHAR;
#define TRUE  1
#define FALSE 0

#define FlexNet_Info(...) ((void)0)

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

static char MYCALL[7];

/* flex_target_is_us reads the own SSID range (FLEXNETSSIDRANGE unset). */
static int g_flexnet_ssid_lo = -1;
static int g_flexnet_ssid_hi = -1;

#include "extracted_external.inc"

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

static const char APPLS[][FLEXNET_MAX_CALLSIGN] = { "SR4BBX" };

static void settle(void)
{
    flex_local_resolve("NODEA", APPLS, 1);
}

static void test_parse(void)
{
    reset();
    ok(flex_parse_external_line("FLEXNETEXTERNAL DXCLU-6 2") == 1, "keyword");
    ok(FlexNetLocalCount == 1, "one entry");
    ok(strcmp(FlexNetLocalCalls[0].base, "DXCLU") == 0 &&
       FlexNetLocalCalls[0].ssid == 6 && FlexNetLocalCalls[0].ext_port == 2,
       "call, SSID and port");

    ok(flex_parse_external_line("  flexnetexternal bbsx 3 ; the BBS") == 1 &&
       FlexNetLocalCount == 2 && FlexNetLocalCalls[1].ext_port == 3 &&
       strcmp(FlexNetLocalCalls[1].base, "BBSX") == 0,
       "lower case, indented, trailing comment");

    int n = FlexNetLocalCount;
    ok(flex_parse_external_line("FLEXNETEXTERNAL DXCLU-7") == 1 &&
       FlexNetLocalCount == n, "no port: matched, ignored");
    ok(flex_parse_external_line("FLEXNETEXTERNAL DXCLU-7 0") == 1 &&
       FlexNetLocalCount == n, "port 0 ignored");
    ok(flex_parse_external_line("FLEXNETEXTERNAL DXCLU-7 65") == 1 &&
       FlexNetLocalCount == n, "port 65 ignored");
    ok(flex_parse_external_line("FLEXNETEXTERNAL DXCLU-7 2 junk") == 1 &&
       FlexNetLocalCount == n, "trailing junk ignored");
    ok(flex_parse_external_line("FLEXNETEXTERNAL TOOLONG1 2") == 1 &&
       FlexNetLocalCount == n, "invalid call ignored");
    ok(flex_parse_external_line("FLEXNETEXTERNALS X 2") == 0,
       "longer keyword is not ours");
    ok(flex_parse_external_line("FLEXNETLOCAL X") == 0,
       "FLEXNETLOCAL is not ours");
}

static void test_external_wins_over_local(void)
{
    reset();
    flex_parse_local_line("FLEXNETLOCAL DXCLU-6");
    flex_parse_external_line("FLEXNETEXTERNAL DXCLU-6 2");
    ok(FlexNetLocalCount == 1 && FlexNetLocalCalls[0].ext_port == 2,
       "listed as both: one entry, external");
}

static void test_resolve_and_lookup(void)
{
    reset();
    flex_parse_external_line("FLEXNETEXTERNAL DXCLU-6 2");
    flex_parse_external_line("FLEXNETEXTERNAL NODEA-7 2");   /* our base */
    flex_parse_local_line("FLEXNETLOCAL SR4BBX");
    settle();

    ok(FlexNetLocalCalls[0].state == FLEX_LOCAL_BOUND,
       "external bound without an APPLICATION");
    ok(FlexNetLocalCalls[1].state == FLEX_LOCAL_NODECALL,
       "external on NODECALL's base refused, as a local call is");
    ok(FlexNetLocalCalls[2].state == FLEX_LOCAL_BOUND, "local app bound");

    ok(flex_external_port("DXCLU-6") == 2, "external port");
    ok(flex_external_port("DXCLU-5") == 0, "other SSID is not it");
    ok(flex_external_port("SR4BBX") == 0, "local app has no port");
    ok(flex_external_port("NODEA-7") == 0, "refused entry has no port");
    ok(flex_target_is_us("DXCLU-6") == FLEX_TARGET_LOCAL,
       "type-6 for an external answered as ours");
    ok(flex_local_covers("DXCLU", 0, 15), "echo of the external skipped");
}

static void test_not_delivered_here(void)
{
    reset();
    flex_parse_external_line("FLEXNETEXTERNAL DXCLU-6 2");
    flex_parse_local_line("FLEXNETLOCAL SR4BBX");
    settle();

    unsigned char a[7];
    ax25(a, "DXCLU", 6);
    ok(!FlexNet_IsLocalCall(a), "external is NOT a local call for L2Code");
    ax25(a, "SR4BBX", 0);
    ok(FlexNet_IsLocalCall(a), "local app still is");
}

static void test_advertised(void)
{
    reset();
    flex_parse_external_line("FLEXNETEXTERNAL DXCLU-6 2");
    settle();

    unsigned char buf[FLEXNET_ADVERT_FRAME_BYTES];
    int n = flex_build_own_frame(buf, sizeof(buf), "NODEA", 0, 0,
                                 FlexNetLocalCalls, FlexNetLocalCount);
    ok(n > 0, "own frame built");
    buf[n > 0 ? n : 0] = '\0';
    ok(n > 0 && strstr((char *)buf, "DXCLU ") != NULL,
       "external rides in our own frame");

    FlexNetLocalCalls[0].state = FLEX_LOCAL_NOFWD;
    n = flex_build_own_frame(buf, sizeof(buf), "NODEA", 0, 0,
                             FlexNetLocalCalls, FlexNetLocalCount);
    buf[n > 0 ? n : 0] = '\0';
    ok(n > 0 && strstr((char *)buf, "DXCLU") == NULL,
       "not advertised when it cannot be forwarded");
    ok(flex_external_port("DXCLU-6") == 0,
       "and then not routed to either");
}

int main(void)
{
    ax25((unsigned char *)MYCALL, "NODEA", 0);
    test_parse();
    test_external_wins_over_local();
    test_resolve_and_lookup();
    test_not_delivered_here();
    test_advertised();
    printf("test_external: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
