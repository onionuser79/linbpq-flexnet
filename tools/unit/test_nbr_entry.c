/* B-3 (v2.6.1): a neighbour enters the D table as (X)Net enters it -
 * <link SSID>-<INIT max SSID> - once the session is established, not as
 * <link SSID>-<link SSID> before the INIT has arrived.
 *
 * Captured 2026-10-11: IW2OHX-14 holds IR2UFV 0-8 (link 0, INIT max 8)
 * and IW2OHX 13-13 (link 13, INIT max 13).
 *
 * Extracted verbatim from FlexNetCode.c by tools/unit/extract.sh;
 * flex_dtable_merge is stubbed to record the entry and add it to learned[].
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define FLEXNET_MAX_CALLSIGN               10
#define FLEXNET_RTT_INFINITY               60000
#define FLEXNET_MAX_SESSIONS               8
#define FLEXNET_MAX_LEARNED_PER_NEIGHBOUR  32
#define FLEXNET_MAX_PATH_HOPS              8

typedef int BOOL;
typedef unsigned char UCHAR;
#define TRUE  1
#define FALSE 0

typedef struct { UCHAR LINKCALL[7]; } LINKTABLE;
struct FLEXNET_SESSION
{
    LINKTABLE * LINK;
    int  port;
    BOOL got_peer_init;
    int  peer_max_ssid;
};
static struct FLEXNET_SESSION FlexNetSessions[FLEXNET_MAX_SESSIONS];
static LINKTABLE links[FLEXNET_MAX_SESSIONS];
#define FlexNet_Info(...) ((void)0)

static int ConvFromAX25(UCHAR * in, UCHAR * out)
{
    char * o = (char *)out;
    int n = 0;
    for (int i = 0; i < 6; i++)
    {
        char c = (char)(in[i] >> 1);
        if (c != ' ') o[n++] = c;
    }
    int ssid = (in[6] >> 1) & 0x0F;
    if (ssid) n += snprintf(o + n, 8, "-%d", ssid);
    o[n] = '\0';
    return n;
}

#include "extracted_nbr_entry.inc"

static int n_merge = 0;
static struct FLEXNET_DEST_ENTRY last_merge;
static int flex_dtable_merge(struct FLEXNET_DEST_ENTRY * in,
                             struct FLEXNET_SESSION * sess)
{
    n_merge++;
    last_merge = *in;
    int si = (int)(sess - FlexNetSessions);
    struct FLEXNET_LEARNED_STATE * st = &FlexNetLearned[si];
    for (int i = 0; i < st->count; i++)
        if (st->routes[i].ssid_lo == in->ssid_lo &&
            st->routes[i].ssid_hi == in->ssid_hi &&
            strcmp(st->routes[i].dest_call, in->callsign) == 0)
            return 2;
    struct FLEXNET_LEARNED_ROUTE * r = &st->routes[st->count++];
    memset(r, 0, sizeof(*r));
    snprintf(r->dest_call, sizeof(r->dest_call), "%s", in->callsign);
    r->ssid_lo = in->ssid_lo;
    r->ssid_hi = in->ssid_hi;
    r->rtt_at_neighbour = in->rtt;
    return 1;
}

static int failures = 0, checks = 0;
static void ok(int cond, const char * what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static struct FLEXNET_SESSION * session(const char * call, int ssid,
                                        BOOL init, int max)
{
    memset(FlexNetSessions, 0, sizeof(FlexNetSessions));
    memset(FlexNetLearned, 0, sizeof(FlexNetLearned));
    memset(g_nbr_entered, 0, sizeof(g_nbr_entered));
    n_merge = 0;
    size_t n = strlen(call);
    for (size_t i = 0; i < 6; i++)
        links[1].LINKCALL[i] = (UCHAR)((i < n ? call[i] : ' ') << 1);
    links[1].LINKCALL[6] = (UCHAR)(0x60 | (ssid << 1));
    FlexNetSessions[1].LINK = &links[1];
    FlexNetSessions[1].port = 2;
    FlexNetSessions[1].got_peer_init = init;
    FlexNetSessions[1].peer_max_ssid = max;
    return &FlexNetSessions[1];
}

static void test_init_range(void)
{
    flex_add_neighbour_entry(session("IR2UFV", 0, TRUE, 8));
    ok(last_merge.ssid_lo == 0 && last_merge.ssid_hi == 8, "IR2UFV: 0-8");
    ok(strcmp(last_merge.callsign, "IR2UFV") == 0 && last_merge.rtt == 1,
       "IR2UFV: base call, cost 1");
    ok(FlexNetLearned[1].routes[0].is_direct_neighbour, "0-8 flagged direct");

    flex_add_neighbour_entry(session("IW2OHX", 13, TRUE, 13));
    ok(last_merge.ssid_lo == 13 && last_merge.ssid_hi == 13, "IW2OHX-13: 13-13");

    flex_add_neighbour_entry(session("IW2OHX", 14, TRUE, 3));
    ok(last_merge.ssid_lo == 14 && last_merge.ssid_hi == 14,
       "INIT max below link SSID: link SSID only");
}

static void test_inferred_keeps_adopted_range(void)
{
    struct FLEXNET_SESSION * s = session("IR2UFV", 0, FALSE, 0);
    struct FLEXNET_LEARNED_ROUTE * r = &FlexNetLearned[1].routes[0];
    snprintf(r->dest_call, sizeof(r->dest_call), "IR2UFV");
    r->ssid_lo = 0; r->ssid_hi = 8; r->rtt_at_neighbour = 1;
    r->is_direct_neighbour = TRUE;
    FlexNetLearned[1].count = 1;
    flex_add_neighbour_entry(s);
    ok(last_merge.ssid_hi == 8, "no INIT, adopted 0-8: stays 0-8");
    ok(FlexNetLearned[1].count == 1, "no second, narrower entry");
}

static void test_inferred_without_history(void)
{
    flex_add_neighbour_entry(session("IR2UFV", 0, FALSE, 0));
    ok(last_merge.ssid_lo == 0 && last_merge.ssid_hi == 0,
       "no INIT, nothing adopted: link SSID");
}

static void test_once_per_session(void)
{
    struct FLEXNET_SESSION * s = session("IR2UFV", 0, TRUE, 8);
    flex_add_neighbour_entry(s);
    flex_add_neighbour_entry(s);
    ok(n_merge == 1, "entered once");
}

int main(void)
{
    test_init_range();
    test_inferred_keeps_adopted_range();
    test_inferred_without_history();
    test_once_per_session();
    printf("test_nbr_entry: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
