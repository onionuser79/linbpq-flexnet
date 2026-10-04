/* v2.5 FlexNet over KISS ports.
 *
 * Pins the places the feature can half-work:
 *   - the PORT block parser: FLEXNET= and FLEXNETLINK= are read only
 *     inside a block, matched exactly (FLEXNET must not swallow
 *     FLEXNETLINK or the global FLEXNETTRANSIT), numbered the way
 *     config.c numbers ports, and never from a driver's CONFIG section;
 *   - resolution: a link is only usable on an existing KISS port that
 *     also has FLEXNET=YES;
 *   - the lookup every FlexNet gate goes through, matched like the AXIP
 *     MAP lookup (C/H bits ignored, SSID significant);
 *   - the link keeper: opens once due, doubles its back-off while
 *     unanswered, starts the CE session on a link it opened, and reopens
 *     promptly after a live link drops;
 *   - next-hop choice counts our own link time to each neighbour;
 *   - the KA echo gate that stops two LinBPQ nodes echoing forever.
 *
 * Extracted verbatim from FlexNetCode.c by tools/unit/extract.sh.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stddef.h>
#include <stdarg.h>
#include <time.h>

#define FLEXNET_RTT_INFINITY   60000
#define FLEXNET_MAX_SESSIONS   8

typedef int BOOL;
typedef unsigned char UCHAR;
#define TRUE  1
#define FALSE 0
#define VOID  void

struct PORTCONTROL
{
    int   PORTNUMBER;
    char  PORTTYPE;
    UCHAR PORTT1;
    UCHAR PORTWINDOW;
};

struct _LINKTABLE
{
    UCHAR LINKCALL[7];
    UCHAR OURCALL[7];
    UCHAR DIGIS[56];
    struct PORTCONTROL * LINKPORT;
    UCHAR LINKTYPE;
    UCHAR LINKWINDOW;
    UCHAR L2STATE;
    int   L2TIME;
    BOOL  FlexNetLink;
};

struct FLEXNET_SESSION
{
    BOOL active;
    int  our_link_time;
};
static struct FLEXNET_SESSION FlexNetSessions[FLEXNET_MAX_SESSIONS];

static char MYCALL[7];

/* ── stubs for the LinBPQ side ───────────────────────────────────────── */

static int n_warn = 0;
static char last_warn[200];
#define flex_port_warn(port, ...) do {                                   \
        n_warn++;                                                        \
        snprintf(last_warn, sizeof(last_warn), __VA_ARGS__);             \
        (void)(port);                                                    \
    } while (0)
#define FlexNet_Info(...) do { } while (0)

static BOOL ConvToAX25(unsigned char * call, unsigned char * ax)
{
    memset(ax, 0x40, 6);
    int i = 0;
    for (; call[i] && call[i] != '-'; i++)
    {
        if (i >= 6 || !isalnum(call[i])) return FALSE;
        ax[i] = (UCHAR)(call[i] << 1);
    }
    if (i == 0) return FALSE;
    int ssid = call[i] == '-' ? atoi((char *)call + i + 1) : 0;
    if (ssid < 0 || ssid > 15) return FALSE;
    ax[6] = (UCHAR)(0x60 | (ssid << 1));
    return TRUE;
}

static struct PORTCONTROL ports[4] = {
    { 1, 0,  30, 4 },     /* ASYNC  */
    { 2, 16, 30, 4 },     /* EXTERNAL (AXIP, telnet...) */
    { 5, 0,  30, 5 },     /* ASYNC  */
    { 7, 22, 40, 3 },     /* I2C    */
};
static struct PORTCONTROL * GetPortTableEntryFromPortNum(int n)
{
    for (size_t i = 0; i < sizeof(ports) / sizeof(ports[0]); i++)
        if (ports[i].PORTNUMBER == n) return &ports[i];
    return NULL;
}

/* One LINKTABLE slot is enough for the keeper. `link_exists` says
   whether FindLink finds the session; `slot_free` whether it can hand
   out a slot when it does not. */
static struct _LINKTABLE the_link;
static BOOL link_exists = FALSE, slot_free = TRUE;
static int  n_sabm = 0, n_init = 0;

static BOOL FindLink(UCHAR * LinkCall, UCHAR * OurCall, int Port,
                     struct _LINKTABLE ** REQLINK)
{
    (void)LinkCall; (void)OurCall; (void)Port;
    if (link_exists) { *REQLINK = &the_link; return TRUE; }
    *REQLINK = slot_free ? &the_link : NULL;
    return FALSE;
}
static VOID SENDSABM(struct _LINKTABLE * LINK) { (void)LINK; n_sabm++; }
static void FlexNet_InitSession(struct _LINKTABLE * LINK, int Port)
{
    (void)Port;
    LINK->FlexNetLink = TRUE;
    n_init++;
}

#include "extracted_kiss.inc"

/* ── harness ─────────────────────────────────────────────────────────── */

static int failures = 0, checks = 0;

static void ok(int cond, const char * what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static void reset_cfg(void)
{
    memset(g_port_links, 0, sizeof(g_port_links));
    g_port_link_count = 0;
    g_flex_port_count = 0;
    n_warn = 0;
}

/* The parser keeps its block state in statics, as flex_load_config()
   reads one file once. Each test feeds a whole file, so it always ends
   outside a block — what the next test needs. */
static void feed(const char * const * lines)
{
    for (; *lines; lines++) flex_parse_port_block_line(*lines);
}

static void ax25(UCHAR * out, const char * call)
{
    ConvToAX25((unsigned char *)call, out);
}

/* ── tests ───────────────────────────────────────────────────────────── */

static void test_port_key(void)
{
    printf("flex_port_key\n");
    ok(flex_port_key("FLEXNET=YES", "FLEXNET") != NULL, "exact key");
    ok(strcmp(flex_port_key("  flexnet = yes", "FLEXNET"), "yes") == 0,
       "case and blanks");
    ok(flex_port_key("FLEXNETLINK=X", "FLEXNET") == NULL,
       "FLEXNET does not match FLEXNETLINK");
    ok(flex_port_key("FLEXNETTRANSIT YES", "FLEXNET") == NULL,
       "FLEXNET does not match FLEXNETTRANSIT");
    ok(flex_port_key("FLEXNET YES", "FLEXNET") == NULL, "'=' required");
    ok(flex_port_key("; FLEXNET=YES", "FLEXNET") == NULL, "comment");
    ok(flex_port_key("", "FLEXNET") == NULL, "empty line");
}

static void test_parse_blocks(void)
{
    printf("PORT block parsing\n");
    reset_cfg();
    static const char * const cfg[] = {
        "FLEXNETLINK=OUTSIDE\n",            /* not in a block: ignored */
        "PORT\n",                           /* ordinal 1 */
        " TYPE=ASYNC\n",
        " FLEXNET=YES\n",
        " FLEXNETLINK=NODEA ; comment\n",
        "ENDPORT\n",
        "PORT\n",                           /* ordinal 2, AXIP */
        " CONFIG\n",
        " FLEXNETLINK=INDRIVER\n",          /* driver config: ignored */
        " MAP NODEX 1.2.3.4 UDP 10093 F\n",
        "ENDPORT\n",
        "PORT\r\n",                         /* PORTNUM after the link */
        " FLEXNETLINK=nodeb-2 F+)\r\n",
        " FLEXNET=on\r\n",
        " PORTNUM=5\r\n",
        "ENDPORT\r\n",
        "PORT\n",                           /* FLEXNET=NO wins last */
        " PORTNUM=7\n",
        " FLEXNET=YES\n",
        " FLEXNET=NO\n",
        " FLEXNETLINK=NODEC\n",
        "ENDPORT\n",
        "PORTS_NOT_A_BLOCK=1\n",
        NULL };
    feed(cfg);

    ok(g_port_link_count == 3, "three links (outside + CONFIG ignored)");
    ok(strcmp(g_port_links[0].call, "NODEA") == 0 &&
       g_port_links[0].port == 1 && g_port_links[0].opts == 0,
       "NODEA on ordinal port 1, no options, comment dropped");
    ok(strcmp(g_port_links[1].call, "NODEB-2") == 0 &&
       g_port_links[1].port == 5,
       "NODEB-2 upper-cased and renumbered to a later PORTNUM=5");
    ok(g_port_links[1].opts == (FLEX_LOPT_PENALTY | FLEX_LOPT_HIDDEN),
       "F+) parsed as penalty + hidden");
    ok(g_port_links[2].port == 7, "NODEC on port 7");
    ok(flex_port_is_flexnet(1) && flex_port_is_flexnet(5),
       "FLEXNET=YES / on recorded");
    ok(!flex_port_is_flexnet(2), "AXIP port has no FLEXNET");
    ok(!flex_port_is_flexnet(7), "FLEXNET=NO after YES disables");
    ok(n_warn == 0, "no warnings on a valid file");

    reset_cfg();
    static const char * const bad[] = {
        "PORT\n",
        " FLEXNET=MAYBE\n",
        " FLEXNETLINK=\n",
        " FLEXNETLINK=TOOLONGCALL\n",
        " FLEXNETLINK=NODEA Fx\n",
        "ENDPORT\n",
        NULL };
    feed(bad);
    ok(n_warn == 4, "invalid value, empty, bad call, bad option all warn");
    ok(g_port_link_count == 1 && g_port_links[0].opts == 0,
       "unknown option keeps the link with default policy");
    ok(!flex_port_is_flexnet(1), "invalid FLEXNET value does not enable");

    reset_cfg();
    static const char * const many[] = { "PORT\n", " FLEXNET=YES\n", NULL };
    feed(many);
    for (int i = 0; i < FLEXNET_MAX_PORT_LINKS + 2; i++)
    {
        char l[40];
        snprintf(l, sizeof(l), " FLEXNETLINK=NODE%d\n", i % 10);
        flex_parse_port_block_line(l);
    }
    flex_parse_port_block_line("ENDPORT\n");
    ok(g_port_link_count == FLEXNET_MAX_PORT_LINKS, "table capped");
    ok(n_warn == 2, "each overflow warns");
}

static void test_resolve_and_find(void)
{
    printf("resolution and lookup\n");
    reset_cfg();
    static const char * const cfg[] = {
        "PORT\n PORTNUM=1\n FLEXNET=YES\n FLEXNETLINK=NODEA-3 F+\nENDPORT\n",
        "PORT\n PORTNUM=2\n FLEXNET=YES\n FLEXNETLINK=NODEB\nENDPORT\n",
        "PORT\n PORTNUM=5\n FLEXNETLINK=NODEC\nENDPORT\n",
        "PORT\n PORTNUM=9\n FLEXNET=YES\n FLEXNETLINK=NODED\nENDPORT\n",
        "PORT\n PORTNUM=7\n FLEXNET=YES\n FLEXNETLINK=NODEE\nENDPORT\n",
        NULL };
    /* One string per block; split them into lines for the parser. */
    for (const char * const * b = cfg; *b; b++)
    {
        char buf[200];
        snprintf(buf, sizeof(buf), "%s", *b);
        for (char * l = strtok(buf, "\n"); l; l = strtok(NULL, "\n"))
            flex_parse_port_block_line(l);
    }
    ok(g_port_link_count == 5, "five declared");

    time_t t0 = time(NULL);
    flex_port_links_resolve();
    ok(g_port_links[0].usable, "ASYNC + FLEXNET=YES usable");
    ok(!g_port_links[1].usable, "non-KISS port refused");
    ok(!g_port_links[2].usable, "KISS port without FLEXNET=YES refused");
    ok(!g_port_links[3].usable, "missing port refused");
    ok(g_port_links[4].usable, "I2C KISS usable");
    ok(n_warn == 3, "each refusal warns");
    ok(g_port_links[0].next_try >= t0 + FLEXNET_PORTLINK_FIRST &&
       g_port_links[0].next_try <= time(NULL) + FLEXNET_PORTLINK_FIRST + 9,
       "first attempt delayed by FIRST plus 0-9 s of jitter");

    UCHAR a[7];
    ax25(a, "NODEA-3");
    ok(FlexNet_PortLinkOpts(a, 1) == FLEX_LOPT_PENALTY, "found with options");
    a[6] |= 0x80;       /* C/H bit, as on LINKCALL */
    a[6] |= 0x01;       /* end-of-address */
    ok(FlexNet_PortLinkOpts(a, 1) == FLEX_LOPT_PENALTY, "C/H and EA ignored");
    ok(FlexNet_PortLinkOpts(a, 5) == -1, "other port: not declared");
    ax25(a, "NODEA-4");
    ok(FlexNet_PortLinkOpts(a, 1) == -1, "SSID is significant");
    ax25(a, "NODEB");
    ok(FlexNet_PortLinkOpts(a, 2) == -1, "unusable link is not a peer");
    ok(FlexNet_PortLinkOpts(NULL, 1) == -1, "NULL call");
}

static void test_keeper(void)
{
    printf("link keeper\n");
    reset_cfg();
    flex_parse_port_block_line("PORT");
    flex_parse_port_block_line(" PORTNUM=5");
    flex_parse_port_block_line(" FLEXNET=YES");
    flex_parse_port_block_line(" FLEXNETLINK=NODEB-2");
    flex_parse_port_block_line("ENDPORT");
    flex_port_links_resolve();
    ax25((UCHAR *)MYCALL, "MYNODE");

    struct FLEXNET_PORT_LINK * pl = &g_port_links[0];
    time_t now = pl->next_try - 1;
    link_exists = FALSE; slot_free = TRUE; n_sabm = 0; n_init = 0;
    g_port_link_scanned = 0;

    flex_port_links_keep(now);
    ok(n_sabm == 0, "not before next_try");

    now += 1;
    flex_port_links_keep(now);
    ok(n_sabm == 1, "opens when due");
    ok(the_link.L2STATE == 2 && the_link.LINKTYPE == 2,
       "SABM state, downlink with no circuit");
    ok(the_link.LINKPORT == &ports[2] && the_link.L2TIME == ports[2].PORTT1 &&
       the_link.LINKWINDOW == ports[2].PORTWINDOW, "port timers applied");
    ok(memcmp(the_link.OURCALL, MYCALL, 7) == 0 &&
       memcmp(the_link.LINKCALL, pl->axcall, 7) == 0, "node call to peer");
    ok(pl->backoff == FLEXNET_PORTLINK_RETRY &&
       pl->next_try == now + FLEXNET_PORTLINK_RETRY, "first back-off 60 s");

    flex_port_links_keep(now);
    ok(n_sabm == 1, "rate-limited to one pass per second");

    /* Unanswered: the SABM run retries out, FindLink finds nothing. */
    int expect[] = { 120, 240, 480, 900, 900 };
    for (int i = 0; i < 5; i++)
    {
        now = pl->next_try;
        flex_port_links_keep(now);
        ok(pl->backoff == expect[i], "back-off doubles to the cap");
    }
    ok(n_sabm == 6, "one SABM run per attempt");

    /* Answered: UA brings the link to state 5. */
    link_exists = TRUE;
    the_link.L2STATE = 5;
    the_link.FlexNetLink = FALSE;
    now += 1;
    flex_port_links_keep(now);
    ok(n_init == 1 && the_link.FlexNetLink, "CE session started on UA");
    ok(pl->backoff == 0 && pl->was_up, "back-off cleared while up");
    now += 1;
    flex_port_links_keep(now);
    ok(n_init == 1, "not started twice");

    the_link.DIGIS[0] = 0x40;
    the_link.FlexNetLink = FALSE;
    now += 1;
    flex_port_links_keep(now);
    ok(n_init == 1, "a digipeated link is not our peer link");
    the_link.DIGIS[0] = 0;

    the_link.L2STATE = 4;
    now += 1;
    flex_port_links_keep(now);
    ok(n_sabm == 6, "closing link left alone");

    /* The neighbour went away. */
    link_exists = FALSE;
    now += 1;
    flex_port_links_keep(now);
    ok(n_sabm == 6 && pl->next_try == now + FLEXNET_PORTLINK_REOPEN,
       "reopen scheduled 10 s after a live link drops");
    now = pl->next_try;
    flex_port_links_keep(now);
    ok(n_sabm == 7 && pl->backoff == FLEXNET_PORTLINK_RETRY,
       "reopens with a fresh back-off");

    slot_free = FALSE;
    now = pl->next_try;
    flex_port_links_keep(now);
    ok(n_sabm == 7 && pl->next_try == now + FLEXNET_PORTLINK_RETRY,
       "no free LINK slot: retry later, no SABM");
    slot_free = TRUE;
}

static void test_cost_here(void)
{
    printf("next-hop cost\n");
    memset(FlexNetSessions, 0, sizeof(FlexNetSessions));
    FlexNetSessions[0].active = TRUE; FlexNetSessions[0].our_link_time = 1;
    FlexNetSessions[1].active = TRUE; FlexNetSessions[1].our_link_time = 40;
    ok(flex_dest_cost_here(10, 0) == 11, "adds our link time");
    ok(flex_dest_cost_here(5, 1) == 45, "slow link costs more");
    ok(flex_dest_cost_here(5, 1) > flex_dest_cost_here(10, 0),
       "a cheaper report over a slow link loses");
    ok(flex_dest_cost_here(FLEXNET_RTT_INFINITY, 0) == FLEXNET_RTT_INFINITY,
       "infinity stays infinity");
    ok(flex_dest_cost_here(FLEXNET_RTT_INFINITY - 1, 1) ==
       FLEXNET_RTT_INFINITY - 1, "a finite route never becomes infinity");
    ok(flex_dest_cost_here(10, 2) == 10, "inactive session adds nothing");
    ok(flex_dest_cost_here(10, -1) == 10 &&
       flex_dest_cost_here(10, FLEXNET_MAX_SESSIONS) == 10, "bad index");
}

static void test_ka_echo(void)
{
    printf("KA echo gate\n");
    memset(g_ka_echo_at, 0, sizeof(g_ka_echo_at));
    time_t t = 1000000;
    ok(flex_ka_should_echo(0, FALSE, t), "first KA echoed");
    ok(!flex_ka_should_echo(0, FALSE, t), "the peer's echo is not echoed");
    ok(!flex_ka_should_echo(0, FALSE, t + FLEXNET_KA_ECHO_GAP - 1),
       "still inside the gap");
    ok(flex_ka_should_echo(0, FALSE, t + FLEXNET_KA_ECHO_GAP),
       "next cycle echoed");
    ok(flex_ka_should_echo(1, FALSE, t), "per session");
    ok(flex_ka_should_echo(2, TRUE, t) && flex_ka_should_echo(2, TRUE, t),
       "PC/Flexnet always echoed");
    ok(!flex_ka_should_echo(-1, FALSE, t) &&
       !flex_ka_should_echo(FLEXNET_MAX_SESSIONS, FALSE, t), "bad index");

    /* Two LinBPQ nodes, A opens: A's KA -> B echoes -> A must not. */
    memset(g_ka_echo_at, 0, sizeof(g_ka_echo_at));
    int frames = 1;                         /* A's proactive KA */
    BOOL b_turn = TRUE;
    for (int hop = 0; hop < 10; hop++)
    {
        if (!flex_ka_should_echo(b_turn ? 1 : 0, FALSE, t)) break;
        frames++;
        b_turn = !b_turn;
    }
    ok(frames == 3, "loop stops after one echo each way");
}

int main(void)
{
    test_port_key();
    test_parse_blocks();
    test_resolve_and_find();
    test_keeper();
    test_cost_here();
    test_ka_echo();
    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
