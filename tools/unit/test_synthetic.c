/* B-3 (v2.6.1): the neighbour entry FlexNet_InitSession invents (link
 * SSID, cost 1) is local knowledge only.
 *
 * Pins:
 *   - an invented entry is never offered to another peer;
 *   - once the neighbour advertises itself under a different range
 *     (IR2UFV: link SSID 0, own record 0-8), the invented one is dropped
 *     and the real record carries the direct-neighbour flag;
 *   - the D-table row of the invented key loses its source;
 *   - a record for some OTHER callsign, or one not covering the
 *     neighbour's SSID, retires nothing.
 *
 * Extracted verbatim from FlexNetCode.c by tools/unit/extract.sh.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
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
    BOOL active;
    int  our_link_time;
    int  port;
};
static struct FLEXNET_SESSION FlexNetSessions[FLEXNET_MAX_SESSIONS];
static LINKTABLE links[FLEXNET_MAX_SESSIONS];
static int n_log = 0;
#define FlexNet_Log(...) (n_log++)

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

#include "extracted_synthetic.inc"

static int failures = 0, checks = 0;
static void ok(int cond, const char * what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static void add_session(int si, const char * call, int ssid)
{
    size_t n = strlen(call);
    for (size_t i = 0; i < 6; i++)
        links[si].LINKCALL[i] = (UCHAR)((i < n ? call[i] : ' ') << 1);
    links[si].LINKCALL[6] = (UCHAR)(0x60 | (ssid << 1));
    FlexNetSessions[si].LINK = &links[si];
    FlexNetSessions[si].active = TRUE;
    FlexNetSessions[si].our_link_time = 2;
    FlexNetSessions[si].port = 2;
}

static int learn(int si, const char * call, int lo, int hi, int rtt,
                 BOOL synthetic, BOOL direct)
{
    struct FLEXNET_LEARNED_STATE * st = &FlexNetLearned[si];
    struct FLEXNET_LEARNED_ROUTE * r = &st->routes[st->count];
    memset(r, 0, sizeof(*r));
    snprintf(r->dest_call, sizeof(r->dest_call), "%s", call);
    r->ssid_lo = lo;
    r->ssid_hi = hi;
    r->rtt_at_neighbour = rtt;
    r->synthetic = synthetic;
    r->is_direct_neighbour = direct;
    return st->count++;
}

static void dest(const char * call, int lo, int hi, int rtt, int via)
{
    struct FLEXNET_DEST_ENTRY * d = &FlexNetDests[FlexNetDestCount++];
    snprintf(d->callsign, sizeof(d->callsign), "%s", call);
    d->ssid_lo = lo;
    d->ssid_hi = hi;
    d->rtt = rtt;
    d->via_session_idx = via;
}

static void reset(void)
{
    memset(FlexNetSessions, 0, sizeof(FlexNetSessions));
    memset(FlexNetLearned, 0, sizeof(FlexNetLearned));
    memset(FlexNetDests, 0, sizeof(FlexNetDests));
    FlexNetDestCount = 0;
    /* Session 0 = the peer we advertise TO; session 1 = IR2UFV (SSID 0). */
    add_session(0, "NODEA", 2);
    add_session(1, "IR2UFV", 0);
}

static void test_invented_entry_never_offered(void)
{
    reset();
    learn(1, "IR2UFV", 0, 0, 1, TRUE, TRUE);
    ok(flex_expected_rtt(0, "IR2UFV", 0, 0, NULL, NULL, NULL)
       == FLEXNET_RTT_INFINITY, "invented 0-0 is not offered");
}

static void test_real_same_key_is_offered(void)
{
    reset();
    learn(1, "IW2OHX", 12, 12, 1, FALSE, TRUE);
    add_session(1, "IW2OHX", 12);
    ok(flex_expected_rtt(0, "IW2OHX", 12, 12, NULL, NULL, NULL) == 3,
       "real own record offered at 1 + link 2");
}

static void test_wider_own_record_retires_invented(void)
{
    reset();
    learn(1, "IR2UFV", 0, 0, 1, TRUE, TRUE);
    int real = learn(1, "IR2UFV", 0, 8, 1, FALSE, FALSE);
    dest("IR2UFV", 0, 0, 1, 1);
    dest("IR2UFV", 0, 8, 1, 1);
    flex_learned_retire_synthetic(1, real);
    struct FLEXNET_LEARNED_STATE * st = &FlexNetLearned[1];
    ok(st->count == 1, "invented entry dropped");
    ok(st->routes[0].ssid_hi == 8 && st->routes[0].is_direct_neighbour,
       "real record carries the direct-neighbour flag");
    ok(FlexNetDests[0].rtt == FLEXNET_RTT_INFINITY &&
       FlexNetDests[0].via_session_idx == -1, "D row of 0-0 lost its source");
    ok(FlexNetDests[1].rtt == 1, "D row of 0-8 untouched");
    ok(flex_expected_rtt(0, "IR2UFV", 0, 8, NULL, NULL, NULL) == 3,
       "0-8 offered");
}

static void test_real_before_invented_in_table(void)
{
    reset();
    int real = learn(1, "IR2UFV", 0, 8, 1, FALSE, TRUE);   /* adopted */
    learn(1, "IR2UFV", 0, 0, 1, TRUE, TRUE);               /* re-invented */
    flex_learned_retire_synthetic(1, real);
    ok(FlexNetLearned[1].count == 1 &&
       FlexNetLearned[1].routes[0].ssid_hi == 8,
       "adopted table: invented entry after the real one is dropped too");
}

static void test_unrelated_records_retire_nothing(void)
{
    reset();
    learn(1, "IR2UFV", 0, 0, 1, TRUE, TRUE);
    int other = learn(1, "DB0AAT", 0, 9, 10, FALSE, FALSE);
    flex_learned_retire_synthetic(1, other);
    ok(FlexNetLearned[1].count == 2, "other callsign: nothing retired");

    int notcover = learn(1, "IR2UFV", 3, 8, 1, FALSE, FALSE);
    flex_learned_retire_synthetic(1, notcover);
    ok(FlexNetLearned[1].count == 3,
       "range not covering the link SSID: nothing retired");

    int withdrawn = learn(1, "IR2UFV", 0, 8, FLEXNET_RTT_INFINITY,
                          FALSE, FALSE);
    flex_learned_retire_synthetic(1, withdrawn);
    ok(FlexNetLearned[1].count == 4, "withdrawn record: nothing retired");

    int self = 0;
    flex_learned_retire_synthetic(1, self);
    ok(FlexNetLearned[1].count == 4, "invented entry does not retire itself");
}

int main(void)
{
    test_invented_entry_never_offered();
    test_real_same_key_is_offered();
    test_wider_own_record_retires_invented();
    test_real_before_invented_in_table();
    test_unrelated_records_retire_nothing();
    printf("test_synthetic: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
