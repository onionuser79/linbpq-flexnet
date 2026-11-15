/* v2.10 range relay: an external inside our own SSID range.
 *
 * A peer holding our range record addresses an SSID inside it as one of
 * ours, `USER -> NODEA-5 via PEER*`, with no slot for us in the chain. The
 * relay must:
 *   - going out (rc2, single-digi leg): the station's leg carries only
 *     `via NODEA*`; the originator's chain is kept in the circuit;
 *   - coming back: our single entry out, the kept chain back in reverse and
 *     unrepeated, sent on the arrival port: the peer sees exactly
 *     `NODEA-5 -> USER via PEER`;
 *   - leave alone what is not its business: a plain external (own record),
 *     a frame the originator already routed through us, a reply with no
 *     circuit, the node call, an uncovered SSID.
 *
 * Extracted verbatim from FlexNetCode.c by tools/unit/extract.sh.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#include <stddef.h>

typedef int BOOL;
#define TRUE  1
#define FALSE 0
typedef unsigned char  UCHAR;
typedef unsigned short USHORT;
typedef void VOID;

typedef struct _MESSAGE
{
    struct _MESSAGE * CHAIN;
    UCHAR   PORT;
    USHORT  LENGTH;
    UCHAR   DEST[7];
    UCHAR   ORIGIN[7];
    UCHAR   CTL;
    UCHAR   PID;
    UCHAR   L2DATA[400];
} MESSAGE;
#define MSGHDDRLEN (USHORT)(sizeof(VOID *) + sizeof(UCHAR) + sizeof(USHORT))

struct PORTCONTROL
{
    int  PORTNUMBER;
    char DIGIFLAG;
};

#define FLEXNET_MAX_CALLSIGN        10
#define FLEXNET_MAX_LOCAL_CALLS     16
#define FLEXNET_LOCAL_BASE_MAX      6
#define FlexNet_Info(...) ((void)0)
static void FlexNet_Log(const char * fmt, ...) { (void)fmt; }

static char MYCALL[7];
static int g_flexnet_ssid_lo = -1;
static int g_flexnet_ssid_hi = -1;

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

/* Port table: ports 2 and 3 exist, DIGIFLAG on both unless a test says. */
static struct PORTCONTROL g_ports[4] = {
    {0, 0}, {1, 1}, {2, 1}, {3, 1},
};
static struct PORTCONTROL * GetPortTableEntryFromPortNum(int port)
{
    return (port >= 1 && port <= 3) ? &g_ports[port] : NULL;
}

/* What left on which port, and what was released. */
static int     g_sent_port = 0, g_sent = 0, g_released = 0;
static MESSAGE g_sent_copy;
static void PUT_ON_PORT_Q(struct PORTCONTROL * P, MESSAGE * m)
{
    g_sent++;
    g_sent_port = P->PORTNUMBER;
    memcpy(&g_sent_copy, m, sizeof(*m));
}
static void ReleaseBuffer(void * m) { (void)m; g_released++; }

#include "extracted_range_relay.inc"

static int failures = 0, checks = 0;

static void ok(int cond, const char * what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static void ax(UCHAR * out, const char * call, int ssid, int h, int e)
{
    memset(out, ' ' << 1, 6);
    for (int i = 0; i < 6 && call[i]; i++) out[i] = (UCHAR)(call[i] << 1);
    out[6] = (UCHAR)(0x60 | (ssid << 1) | (h ? 0x80 : 0) | (e ? 0x01 : 0));
}

static UCHAR * frame_base(MESSAGE * m)
{
    return (UCHAR *)m + offsetof(MESSAGE, DEST);
}

/* Build `src -> dst via digis` + control byte `ctl`. digis: "CALL-S*" form. */
static void build(MESSAGE * m, const char * dst, int dssid, const char * src,
                  int sssid, const char ** digis, const int * dssids,
                  const int * reps, int nd, UCHAR ctl)
{
    memset(m, 0, sizeof(*m));
    UCHAR * b = frame_base(m);
    ax(b, dst, dssid, 1, 0);
    ax(b + 7, src, sssid, 0, nd == 0);
    for (int i = 0; i < nd; i++)
        ax(b + 14 + 7 * i, digis[i], dssids[i], reps[i], i == nd - 1);
    b[14 + 7 * nd] = ctl;
    m->LENGTH = (USHORT)(MSGHDDRLEN + 14 + 7 * nd + 1);
}

static int digis_of(MESSAGE * m, char out[][20], int * h)
{
    UCHAR * b = frame_base(m);
    if (b[13] & 1) return 0;
    int n = 0;
    UCHAR * d = b + 14;
    for (;;)
    {
        conv_from_ax25(d, out[n]);
        h[n] = (d[6] & 0x80) != 0;
        n++;
        if (d[6] & 1) break;
        d += 7;
    }
    return n;
}

static void settle(void)
{
    int my_ssid = (MYCALL[6] >> 1) & 0x0F;
    int lo = my_ssid, hi = my_ssid;
    flex_own_ssid_range(my_ssid, &lo, &hi);
    static const char appls[][FLEXNET_MAX_CALLSIGN] = { "NODEA-8" };
    flex_local_resolve("NODEA", lo, hi, appls, 1);
}

static void setup(void)
{
    ax((UCHAR *)MYCALL, "NODEA", 0, 0, 0);
    g_flexnet_ssid_lo = 0;
    g_flexnet_ssid_hi = 8;
    memset(FlexNetLocalCalls, 0, sizeof(FlexNetLocalCalls));
    FlexNetLocalCount = 0;
    memset(g_relay, 0, sizeof(g_relay));
    g_relay_out = g_relay_back = g_relay_drop = 0;
    flex_parse_external_line("FLEXNETEXTERNAL NODEA-5 3");    /* covered */
    flex_parse_external_line("FLEXNETEXTERNAL DXCLU-6 3");    /* own record */
    settle();
    g_sent = g_released = g_sent_port = 0;
}

static void test_states(void)
{
    setup();
    ok(FlexNetLocalCalls[0].state == FLEX_LOCAL_COVERED, "NODEA-5 covered");
    ok(FlexNetLocalCalls[1].state == FLEX_LOCAL_BOUND, "DXCLU-6 bound");
    UCHAR a[7];
    ax(a, "NODEA", 5, 0, 0);
    ok(flex_relay_covered_port(a) == 3, "covered port 3");
    ax(a, "DXCLU", 6, 0, 0);
    ok(flex_relay_covered_port(a) == 0, "plain external is not relayed");
    ax(a, "NODEA", 4, 0, 0);
    ok(flex_relay_covered_port(a) == 0, "uncovered SSID is not relayed");
}

/* The live shape: USER -> NODEA-5 via PEER* on port 2, back on port 3. */
static void test_round_trip(void)
{
    setup();
    MESSAGE m;
    const char * d1[] = { "PEER" };
    const int s1[] = { 0 }, h1[] = { 1 };
    build(&m, "NODEA", 5, "USER", 1, d1, s1, h1, 1, 0x3F);
    ok(FlexNet_RangeRelayOut(&g_ports[2], &m) == TRUE, "out: taken");
    ok(g_sent == 1 && g_sent_port == 3, "out: sent on the external's port");
    char d[8][20]; int h[8];
    int n = digis_of(&g_sent_copy, d, h);
    ok(n == 1 && strcmp(d[0], "NODEA") == 0 && h[0],
       "out: single-digi leg, via NODEA* only");
    ok(frame_base(&g_sent_copy)[14 + 7] == 0x3F, "out: control byte intact");
    ok(g_relay_out == 1 && flex_relay_live() == 1, "out: circuit recorded");

    /* The station answers through us: NODEA-5 -> USER via NODEA. */
    const char * d2[] = { "NODEA" };
    const int s2[] = { 0 }, h2[] = { 0 };
    build(&m, "USER", 1, "NODEA", 5, d2, s2, h2, 1, 0x73);
    g_sent = 0;
    UCHAR * ours = frame_base(&m) + 14;
    ok(FlexNet_RangeRelayBack(&g_ports[3], &m, ours) == TRUE, "back: taken");
    ok(g_sent == 1 && g_sent_port == 2, "back: sent on the arrival port");
    n = digis_of(&g_sent_copy, d, h);
    ok(n == 1 && strcmp(d[0], "PEER") == 0 && !h[0],
       "back: via PEER only - exactly what the peer expects");
    ok(frame_base(&g_sent_copy)[14 + 7] == 0x73, "back: control byte intact");
    ok(g_sent_copy.LENGTH == m.LENGTH, "back: same length (ours out, PEER in)");
    ok(g_relay_back == 1, "back: counted");
}

/* A user on our own port with no digis at all. */
static void test_no_digis(void)
{
    setup();
    MESSAGE m;
    build(&m, "NODEA", 5, "USER", 0, NULL, NULL, NULL, 0, 0x3F);
    ok(FlexNet_RangeRelayOut(&g_ports[2], &m) == TRUE, "out, direct user");
    char d[8][20]; int h[8];
    int n = digis_of(&g_sent_copy, d, h);
    ok(n == 1 && strcmp(d[0], "NODEA") == 0 && h[0], "out: via NODEA*");

    const char * d2[] = { "NODEA" };
    const int s2[] = { 0 }, h2[] = { 0 };
    build(&m, "USER", 0, "NODEA", 5, d2, s2, h2, 1, 0x73);
    ok(FlexNet_RangeRelayBack(&g_ports[3], &m, frame_base(&m) + 14) == TRUE,
       "back, direct user");
    ok(digis_of(&g_sent_copy, d, h) == 0 && (frame_base(&g_sent_copy)[13] & 1),
       "back: no digis left, E bit on ORIGIN");
    ok(g_sent_port == 2, "back: arrival port");
}

/* Two digis upstream: kept in order, restored reversed on the reply. */
static void test_two_digis(void)
{
    setup();
    MESSAGE m;
    const char * d1[] = { "PEERA", "PEERB" };
    const int s1[] = { 0, 3 }, h1[] = { 1, 1 };
    build(&m, "NODEA", 5, "USER", 1, d1, s1, h1, 2, 0x3F);
    ok(FlexNet_RangeRelayOut(&g_ports[2], &m) == TRUE, "2 digis: out");
    char d[8][20]; int h[8];
    ok(digis_of(&g_sent_copy, d, h) == 1 && strcmp(d[0], "NODEA") == 0 && h[0],
       "2 digis: leg via NODEA* only");
    const char * d2[] = { "NODEA" };
    const int s2[] = { 0 }, h2[] = { 0 };
    build(&m, "USER", 1, "NODEA", 5, d2, s2, h2, 1, 0x73);
    ok(FlexNet_RangeRelayBack(&g_ports[3], &m, frame_base(&m) + 14) == TRUE,
       "2 digis: back");
    int n = digis_of(&g_sent_copy, d, h);
    ok(n == 2 && strcmp(d[0], "PEERB-3") == 0 && !h[0] &&
       strcmp(d[1], "PEERA") == 0 && !h[1],
       "2 digis: reply path PEERB-3, PEERA (reversed, unrepeated)");
    ok((frame_base(&g_sent_copy)[14 + 13] & 1) && !(frame_base(&g_sent_copy)[14 + 6] & 1),
       "2 digis: E bit only on the last");
}

/* Eight digis upstream (the AX.25 maximum) still fit. */
static void test_eight_digis(void)
{
    setup();
    MESSAGE m;
    const char * d8[] = { "A", "B", "C", "D", "E", "F", "G", "H" };
    const int s8[8] = { 0 }, h8[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    build(&m, "NODEA", 5, "USER", 1, d8, s8, h8, 8, 0x3F);
    ok(FlexNet_RangeRelayOut(&g_ports[2], &m) == TRUE && g_sent == 1,
       "8 digis: relayed, not dropped");
    const char * d2[] = { "NODEA" };
    const int s2[] = { 0 }, h2[] = { 0 };
    build(&m, "USER", 1, "NODEA", 5, d2, s2, h2, 1, 0x73);
    ok(FlexNet_RangeRelayBack(&g_ports[3], &m, frame_base(&m) + 14) == TRUE,
       "8 digis: back");
    char d[8][20]; int h[8];
    int n = digis_of(&g_sent_copy, d, h);
    ok(n == 8 && strcmp(d[0], "H") == 0 && strcmp(d[7], "A") == 0,
       "8 digis: all restored, reversed");
}

/* What the relay must NOT touch. */
static void test_declines(void)
{
    MESSAGE m;
    const char * d1[] = { "PEER" };
    const int s1[] = { 0 }, h1[] = { 1 };

    setup();
    build(&m, "DXCLU", 6, "USER", 1, d1, s1, h1, 1, 0x3F);
    ok(FlexNet_RangeRelayOut(&g_ports[2], &m) == FALSE && g_sent == 0,
       "plain external: left to the digipeat path");

    build(&m, "NODEA", 4, "USER", 1, d1, s1, h1, 1, 0x3F);
    ok(FlexNet_RangeRelayOut(&g_ports[2], &m) == FALSE, "uncovered SSID");

    build(&m, "NODEA", 0, "USER", 1, d1, s1, h1, 1, 0x3F);
    ok(FlexNet_RangeRelayOut(&g_ports[2], &m) == FALSE, "the node itself");

    g_ports[2].DIGIFLAG = 0;
    build(&m, "NODEA", 5, "USER", 1, d1, s1, h1, 1, 0x3F);
    ok(FlexNet_RangeRelayOut(&g_ports[2], &m) == FALSE,
       "arrival port does not digipeat: no relay");
    g_ports[2].DIGIFLAG = 1;

    /* Already carries us: loop guard, dropped not resent. */
    const char * d2[] = { "PEER", "NODEA" };
    const int s2[] = { 0, 0 }, h2[] = { 1, 1 };
    build(&m, "NODEA", 5, "USER", 1, d2, s2, h2, 2, 0x3F);
    g_released = 0;
    ok(FlexNet_RangeRelayOut(&g_ports[2], &m) == TRUE && g_sent == 0 &&
       g_released == 1 && g_relay_drop == 1, "our call in chain: dropped");

    /* Reply with no circuit (originator routed through us itself): the
       stock digipeat keeps us in the chain - not ours to contract. */
    setup();
    const char * d3[] = { "NODEA", "PEER" };
    const int s3[] = { 0, 0 }, h3[] = { 0, 0 };
    build(&m, "USER", 1, "NODEA", 5, d3, s3, h3, 2, 0x73);
    ok(FlexNet_RangeRelayBack(&g_ports[3], &m, frame_base(&m) + 14) == FALSE,
       "reply without a circuit: left alone");

    /* Circuit exists, but the reply arrives on another port than the
       external's: not from that station. */
    build(&m, "NODEA", 5, "USER", 1, d1, s1, h1, 1, 0x3F);
    FlexNet_RangeRelayOut(&g_ports[2], &m);
    build(&m, "USER", 1, "NODEA", 5, d3, s3, h3, 2, 0x73);
    ok(FlexNet_RangeRelayBack(&g_ports[2], &m, frame_base(&m) + 14) == FALSE,
       "reply on the wrong port: left alone");

    /* Our entry first but more digis behind it: not our leg's shape. */
    build(&m, "USER", 1, "NODEA", 5, d3, s3, h3, 2, 0x73);
    ok(FlexNet_RangeRelayBack(&g_ports[3], &m, frame_base(&m) + 14) == FALSE,
       "reply with extra digis after ours: left alone");

    /* Our entry is not the first digi: not one we added. */
    const char * d4[] = { "OTHER", "NODEA", "PEER" };
    const int s4[] = { 0, 0, 0 }, h4[] = { 1, 0, 0 };
    build(&m, "USER", 1, "NODEA", 5, d4, s4, h4, 3, 0x73);
    ok(FlexNet_RangeRelayBack(&g_ports[3], &m, frame_base(&m) + 21) == FALSE,
       "our entry not first: left alone");
}

int main(void)
{
    test_states();
    test_round_trip();
    test_no_digis();
    test_two_digis();
    test_eight_digis();
    test_declines();
    printf("test_range_relay: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
