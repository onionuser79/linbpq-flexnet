/* v2.6 cross-port L2 forwarding, end to end through FlexNet_L2Transit().
 *
 * Up to v2.5 a forwarded frame always left on the port it arrived on, so
 * a route learned from an RF neighbour could not be offered to an AXUDP
 * one. These tests push real AX.25 frames through the shipped transit
 * code, forward and back, and check both the rewritten digi chain and
 * the port the frame leaves on:
 *
 *   - FLEXNETCROSSPORT off: nothing changes, and a next hop on another
 *     port is declined rather than sent out of the wrong one;
 *   - AXUDP neighbour -> RF destination and back (append, contract);
 *   - an RF user digipeating "via NODE" to an AXUDP destination, whose
 *     replies can only find the user through the circuit table;
 *   - an adjacent destination on another port (nothing to append);
 *   - FLEXNETEXTERNAL stations, which cross ports even with
 *     FLEXNETCROSSPORT off, and stay put when on the arrival port;
 *   - a neighbour with links on two ports, and DIGIFLAG=0.
 *
 * Everything but the BPQ stubs below is extracted verbatim from
 * FlexNetCode.c by tools/unit/extract.sh.
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

#define FLEXNET_MAX_SESSIONS     8
#define FLEXNET_MAX_CALLSIGN     10
#define FLEXNET_RTT_INFINITY     60000
#define FLEXNET_MAX_LOCAL_CALLS  16
#define FLEXNET_LOCAL_BASE_MAX   6

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

/* ── BPQ / FlexNet stubs: only the fields the transit code reads ── */
struct PORTCONTROL
{
    int  PORTNUMBER;
    int  PORTMAXDIGIS;
    char DIGIFLAG;
};
static struct PORTCONTROL Ports[5];
static struct PORTCONTROL * GetPortTableEntryFromPortNum(int n)
{
    return (n >= 1 && n <= 4) ? &Ports[n] : NULL;
}

struct FLEXNET_SESSION
{
    BOOL active;
    int  port;
    char peer_callsign[7];
    int  our_link_time;
};
static struct FLEXNET_SESSION FlexNetSessions[FLEXNET_MAX_SESSIONS];

struct FLEXNET_DEST_ENTRY
{
    char callsign[FLEXNET_MAX_CALLSIGN];
    int  via_session_idx;
    char via_callsign[FLEXNET_MAX_CALLSIGN];
};
static struct FLEXNET_DEST_ENTRY FlexNetDests[8];
static int FlexNetDestCount = 0;
static int flex_find_dest_for_target(const char * target)
{
    for (int i = 0; i < FlexNetDestCount; i++)
        if (strcasecmp(FlexNetDests[i].callsign, target) == 0) return i;
    return -1;
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

static BOOL g_flexnet_transit_enabled    = TRUE;
static BOOL g_flexnet_l2_transit_enabled = TRUE;
static BOOL g_flexnet_crossport_enabled  = FALSE;
static unsigned long g_l2_fwd_extended, g_l2_fwd_contracted,
    g_l2_fwd_declined, g_l2_fwd_repinned, g_l2_fwd_looped,
    g_l2_fwd_evicted, g_l2_fwd_crossport;
static void FlexNet_Log(const char * fmt, ...)  { (void)fmt; }
static void FlexNet_Info(const char * fmt, ...) { (void)fmt; }

static BOOL flex_port_is_flexnet(int port) { return port == 3 || port == 4; }

/* A FlexNet peer = a session with that call on that port. */
static BOOL FlexNet_IsPeerFlexNetMapped(UCHAR * call, int port)
{
    for (int i = 0; i < FLEXNET_MAX_SESSIONS; i++)
        if (FlexNetSessions[i].active && FlexNetSessions[i].port == port &&
            memcmp(FlexNetSessions[i].peer_callsign, call, 6) == 0 &&
            (FlexNetSessions[i].peer_callsign[6] & 0x1E) == (call[6] & 0x1E))
            return TRUE;
    return FALSE;
}

#include "extracted_l2_types.inc"
#include "extracted_xport_types.inc"
static struct FLEXNET_L2_TRANSIT FlexNetL2Transit[FLEXNET_MAX_L2_TRANSIT];
static struct FLEXNET_LOCAL_CALL FlexNetLocalCalls[FLEXNET_MAX_LOCAL_CALLS];
static int FlexNetLocalCount = 0;

#include "extracted_xport.inc"

/* ── test helpers ── */
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

/* Build DEST, ORIGIN and digis from "CALL" / "CALL*" tokens, then CTL. */
static void build(MESSAGE * m, const char * dest, const char * origin,
                  const char * digis, UCHAR ctl)
{
    memset(m, 0, sizeof(*m));
    UCHAR * b = frame_base(m);
    char tok[16];
    int n = 0;
    const char * list[9];
    char store[9][16];
    if (digis)
    {
        const char * p = digis;
        while (*p && n < 8)
        {
            int k = 0;
            while (*p == ' ') p++;
            while (*p && *p != ' ' && k < 15) store[n][k++] = *p++;
            store[n][k] = '\0';
            if (k) { list[n] = store[n]; n++; }
        }
    }
    snprintf(tok, sizeof(tok), "%s", dest);
    char * dash = strchr(tok, '-');
    int ssid = dash ? atoi(dash + 1) : 0;
    if (dash) *dash = '\0';
    ax(b, tok, ssid, 0, 0);
    snprintf(tok, sizeof(tok), "%s", origin);
    dash = strchr(tok, '-');
    ssid = dash ? atoi(dash + 1) : 0;
    if (dash) *dash = '\0';
    ax(b + 7, tok, ssid, 0, n == 0);
    for (int i = 0; i < n; i++)
    {
        snprintf(tok, sizeof(tok), "%s", list[i]);
        size_t l = strlen(tok);
        int h = (l && tok[l - 1] == '*');
        if (h) tok[l - 1] = '\0';
        dash = strchr(tok, '-');
        ssid = dash ? atoi(dash + 1) : 0;
        if (dash) *dash = '\0';
        ax(b + 14 + 7 * i, tok, ssid, h, i == n - 1);
    }
    b[14 + 7 * n] = ctl;
    m->LENGTH = (USHORT)(MSGHDDRLEN + 14 + 7 * n + 1);
}

/* "DEST<ORIGIN via D1* D2" rendering of the address field. */
static void render(MESSAGE * m, char * out, size_t outlen)
{
    UCHAR * b = frame_base(m);
    char c[20];
    size_t pos = 0;
    conv_from_ax25(b + 7, c);
    pos += (size_t)snprintf(out + pos, outlen - pos, "%s>", c);
    conv_from_ax25(b, c);
    pos += (size_t)snprintf(out + pos, outlen - pos, "%s", c);
    if (b[13] & 0x01) return;
    pos += (size_t)snprintf(out + pos, outlen - pos, " via");
    for (UCHAR * d = b + 14; ; d += 7)
    {
        conv_from_ax25(d, c);
        pos += (size_t)snprintf(out + pos, outlen - pos, " %s%s", c,
                                (d[6] & 0x80) ? "*" : "");
        if (d[6] & 0x01) break;
    }
}

/* What L2Code.c does: find the first unrepeated digi (ours), call the
   hook, then set our H-bit as Digipeat() would. Returns the out port
   (the arrival port when the hook says 0), -1 when the frame is dropped. */
static int carry(MESSAGE * m, int arrive, char * chain, size_t chainlen)
{
    UCHAR * b = frame_base(m);
    UCHAR * our = b + 14;
    while (our[6] & 0x80) our += 7;
    int to = 0;
    UCHAR * r = FlexNet_L2Transit(&Ports[arrive], m, our, &to);
    if (!r) return -1;
    r[6] |= 0x80;
    render(m, chain, chainlen);
    return to ? to : arrive;
}

static void session(int i, const char * call, int port, int lt)
{
    FlexNetSessions[i].active = TRUE;
    FlexNetSessions[i].port = port;
    FlexNetSessions[i].our_link_time = lt;
    ax((UCHAR *)FlexNetSessions[i].peer_callsign, call, 0, 0, 0);
}

static void dest(const char * call, int via)
{
    struct FLEXNET_DEST_ENTRY * d = &FlexNetDests[FlexNetDestCount++];
    snprintf(d->callsign, sizeof(d->callsign), "%s", call);
    d->via_session_idx = via;
    conv_from_ax25(FlexNetSessions[via].peer_callsign, d->via_callsign);
}

/* Port 1 telnet, 2 AXUDP, 3 and 4 RF. XNET14 and PEERC on AXUDP; NODEB has
   links on both RF ports, the 4 one cheaper. DXCLU-6 is an external on 2. */
static void world(BOOL crossport)
{
    memset(Ports, 0, sizeof(Ports));
    for (int p = 1; p <= 4; p++) { Ports[p].PORTNUMBER = p; Ports[p].DIGIFLAG = 1; }
    Ports[1].DIGIFLAG = 0;
    memset(FlexNetSessions, 0, sizeof(FlexNetSessions));
    memset(FlexNetDests, 0, sizeof(FlexNetDests));
    memset(FlexNetL2Transit, 0, sizeof(FlexNetL2Transit));
    memset(FlexNetLocalCalls, 0, sizeof(FlexNetLocalCalls));
    FlexNetDestCount = 0;
    session(0, "XNET14", 2, 30);
    session(1, "NODEB", 3, 50);
    session(2, "NODEB", 4, 20);
    session(3, "PEERC", 2, 40);
    dest("FARDST", 2);           /* beyond NODEB, via its port-4 link */
    dest("FARAX", 0);            /* beyond XNET14 */
    dest("FARAX2", 3);           /* beyond PEERC, same port as XNET14 */
    dest("NODEB", 2);            /* the neighbour itself */
    FlexNetLocalCount = 1;
    snprintf(FlexNetLocalCalls[0].base, sizeof(FlexNetLocalCalls[0].base),
             "DXCLU");
    FlexNetLocalCalls[0].ssid = 6;
    FlexNetLocalCalls[0].state = FLEX_LOCAL_BOUND;
    FlexNetLocalCalls[0].ext_port = 2;
    g_flexnet_crossport_enabled = crossport;
}

#define SABM 0x3F
#define UA   0x73
#define IFR  0x10

static void test_active_truth_table(void)
{
    g_flexnet_transit_enabled = g_flexnet_l2_transit_enabled = TRUE;
    g_flexnet_crossport_enabled = TRUE;
    ok(flex_crossport_active(), "all three on");
    g_flexnet_l2_transit_enabled = FALSE;
    ok(!flex_crossport_active(), "needs FLEXNETL2TRANSIT");
    g_flexnet_l2_transit_enabled = TRUE;
    g_flexnet_transit_enabled = FALSE;
    ok(!flex_crossport_active(), "needs FLEXNETTRANSIT");
    g_flexnet_transit_enabled = TRUE;
    g_flexnet_crossport_enabled = FALSE;
    ok(!flex_crossport_active(), "off by default");
}

static void test_out_port_precedence(void)
{
    ok(flex_l2_out_port(2, 0, 0, 0, 0) == 2, "nothing known: stay");
    ok(flex_l2_out_port(2, 3, 4, 1, 1) == 3, "returning circuit first");
    ok(flex_l2_out_port(2, 0, 4, 1, 3) == 4, "then the forward circuit");
    ok(flex_l2_out_port(2, 0, 0, 1, 3) == 1, "then an external");
    ok(flex_l2_out_port(2, 0, 0, 0, 3) == 3, "then a neighbour");
}

static void test_off_unchanged(void)
{
    MESSAGE m;
    char c[160];
    world(FALSE);
    build(&m, "FARDST", "USER", "XNET14* NODEA", SABM);
    int out = carry(&m, 2, c, sizeof(c));
    ok(out == 2, "off: RF destination not carried across");
    ok(strcmp(c, "USER>FARDST via XNET14* NODEA*") == 0,
       "off: chain left alone (declined, not sent out of the wrong port)");

    build(&m, "FARAX2", "USER", "XNET14* NODEA", SABM);
    out = carry(&m, 2, c, sizeof(c));
    ok(out == 2 && strcmp(c, "USER>FARAX2 via XNET14* NODEA* PEERC") == 0,
       "off: same-port transit exactly as v2.5");
    build(&m, "USER", "FARAX2", "PEERC* NODEA XNET14", UA);
    out = carry(&m, 2, c, sizeof(c));
    ok(out == 2 && strcmp(c, "FARAX2>USER via NODEA* XNET14") == 0,
       "off: same-port contraction exactly as v2.5");
}

static void test_axudp_to_rf_and_back(void)
{
    MESSAGE m;
    char c[160];
    world(TRUE);
    build(&m, "FARDST", "USER", "XNET14* NODEA", SABM);
    int out = carry(&m, 2, c, sizeof(c));
    ok(out == 4, "forward leaves on NODEB's cheaper port");
    ok(strcmp(c, "USER>FARDST via XNET14* NODEA* NODEB") == 0,
       "forward appends the RF next hop");

    build(&m, "USER", "FARDST", "NODEB* NODEA XNET14", UA);
    out = carry(&m, 4, c, sizeof(c));
    ok(out == 2, "reply goes back to the AXUDP side");
    ok(strcmp(c, "FARDST>USER via NODEA* XNET14") == 0,
       "reply contracted to the chain the user sent");

    build(&m, "FARDST", "USER", "XNET14* NODEA", IFR);
    out = carry(&m, 2, c, sizeof(c));
    ok(out == 4 && strcmp(c, "USER>FARDST via XNET14* NODEA* NODEB") == 0,
       "I frame follows the pinned hop and port");
    ok(g_l2_fwd_crossport >= 3, "cross-port frames counted");
}

static void test_rf_user_to_axudp(void)
{
    MESSAGE m;
    char c[160];
    world(TRUE);
    build(&m, "FARAX", "RFUSER", "NODEA", SABM);
    int out = carry(&m, 3, c, sizeof(c));
    ok(out == 2, "RF user via NODE routed to AXUDP");
    ok(strcmp(c, "RFUSER>FARAX via NODEA* XNET14") == 0,
       "AXUDP next hop appended");

    build(&m, "RFUSER", "FARAX", "XNET14* NODEA", UA);
    out = carry(&m, 2, c, sizeof(c));
    ok(out == 3, "reply finds the RF user through the circuit table");
    ok(strcmp(c, "FARAX>RFUSER via NODEA*") == 0, "reply contracted");

    world(FALSE);
    build(&m, "FARAX", "RFUSER", "NODEA", SABM);
    out = carry(&m, 3, c, sizeof(c));
    ok(out == 3 && strcmp(c, "RFUSER>FARAX via NODEA*") == 0,
       "off: an RF user's digipeat is not rewritten (v2.5 gate)");
}

static void test_adjacent_other_port(void)
{
    MESSAGE m;
    char c[160];
    world(TRUE);
    build(&m, "NODEB", "USER", "XNET14* NODEA", SABM);
    int out = carry(&m, 2, c, sizeof(c));
    ok(out == 4, "adjacent neighbour on another port: crosses");
    ok(strcmp(c, "USER>NODEB via XNET14* NODEA*") == 0, "nothing appended");

    build(&m, "USER", "NODEB", "NODEA XNET14", UA);
    out = carry(&m, 4, c, sizeof(c));
    ok(out == 2 && strcmp(c, "NODEB>USER via NODEA* XNET14") == 0,
       "reply crosses back, chain untouched");
}

static void test_external(void)
{
    MESSAGE m;
    char c[160];
    world(FALSE);                       /* externals do not need it */
    build(&m, "DXCLU-6", "RFUSER", "NODEA", SABM);
    int out = carry(&m, 3, c, sizeof(c));
    ok(out == 2, "RF user to external: out on the external's port");
    ok(strcmp(c, "RFUSER>DXCLU-6 via NODEA*") == 0, "no digi appended");

    build(&m, "RFUSER", "DXCLU-6", "NODEA", UA);
    out = carry(&m, 2, c, sizeof(c));
    ok(out == 3, "external's reply back to the RF user");

    build(&m, "DXCLU-6", "USER", "XNET14* NODEA", SABM);
    out = carry(&m, 2, c, sizeof(c));
    ok(out == 2 && strcmp(c, "USER>DXCLU-6 via XNET14* NODEA*") == 0,
       "external on the arrival port: plain digipeat");
    build(&m, "USER", "DXCLU-6", "NODEA XNET14", UA);
    out = carry(&m, 2, c, sizeof(c));
    ok(out == 2, "its reply stays too");
}

static void test_two_port_neighbour_and_digiflag(void)
{
    MESSAGE m;
    char c[160];
    world(TRUE);
    /* An originator-built chain through us to NODEB: no circuit, so the
       neighbour lookup decides. */
    build(&m, "FARDST", "USER", "XNET14* NODEA NODEB", SABM);
    ok(carry(&m, 2, c, sizeof(c)) == 4, "two-port neighbour: cheaper link");
    build(&m, "FARDST", "USER", "NODEA NODEB", SABM);
    ok(carry(&m, 3, c, sizeof(c)) == 3,
       "two-port neighbour: stays when it is on the arrival port");

    Ports[2].DIGIFLAG = 0;
    build(&m, "FARDST", "USER", "XNET14* NODEA", SABM);
    ok(carry(&m, 2, c, sizeof(c)) == 2, "DIGIFLAG=0 port does not cross");
}

int main(void)
{
    test_active_truth_table();
    test_out_port_precedence();
    test_off_unchanged();
    test_axudp_to_rf_and_back();
    test_rf_user_to_axudp();
    test_adjacent_other_port();
    test_external();
    test_two_port_neighbour_and_digiflag();
    printf("test_crossport: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
