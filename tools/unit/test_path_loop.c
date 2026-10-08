/* v2.6 path answers that loop back through this node.
 *
 * Found at the IW2OHX-12 go-live (2026-10-08): IW2OHX-14 rendered
 * `route: IW2OHX-14 IW2OHX-12 IW2OHX-12 IQ2LB` for IQ2LB, a direct RF
 * neighbour of -12. -12 had cached [IW2OHX-12 IQ2LB] from -14's answer
 * to its own probe (-14 reaches IQ2LB through -12) and served it with
 * itself prepended again. Pins:
 *   - flex_chain_has_call(): a chain containing our call is a loop;
 *   - flex_target_is_direct_peer(): "CALL-0" is "CALL", so a direct
 *     neighbour is recognised and answered [asker, us, it].
 *
 * Extracted verbatim from FlexNetCode.c by tools/unit/extract.sh.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#define FLEXNET_MAX_CALLSIGN   10
#define FLEXNET_MAX_PATH_HOPS  8
#define FLEXNET_MAX_SESSIONS   8

typedef int BOOL;
typedef unsigned char UCHAR;
#define TRUE  1
#define FALSE 0

typedef struct { UCHAR LINKCALL[7]; } LINKTABLE;
struct FLEXNET_SESSION
{
    LINKTABLE * LINK;
    BOOL active;
};
static struct FLEXNET_SESSION FlexNetSessions[FLEXNET_MAX_SESSIONS];
static LINKTABLE links[FLEXNET_MAX_SESSIONS];

/* BPQ's ConvFromAX25: space-fills 10 bytes, no terminator, returns len. */
static int ConvFromAX25(UCHAR * in, UCHAR * out)
{
    char * o = (char *)out;
    memset(o, ' ', 10);
    int n = 0;
    for (int i = 0; i < 6 && in[i] != 0x40; i++) o[n++] = (char)(in[i] >> 1);
    int ssid = (in[6] >> 1) & 0x0F;
    if (ssid) n += snprintf(o + n, 4, "-%d", ssid);
    if (n < 10) o[n] = ' ';
    return n;
}

#include "extracted_path_loop.inc"

static int failures = 0, checks = 0;
static void ok(int cond, const char * what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static void ax(UCHAR * out, const char * call, int ssid)
{
    memset(out, 0x40, 6);
    for (int i = 0; i < 6 && call[i]; i++) out[i] = (UCHAR)(call[i] << 1);
    out[6] = (UCHAR)(0x60 | (ssid << 1));
}

int main(void)
{
    char chain[FLEXNET_MAX_PATH_HOPS][FLEXNET_MAX_CALLSIGN] = {{0}};
    strcpy(chain[0], "IW2OHX-12");
    strcpy(chain[1], "IQ2LB");
    ok(flex_chain_has_call((const char (*)[FLEXNET_MAX_CALLSIGN])chain, 2,
                           "IW2OHX-12"), "chain through us is a loop");
    ok(flex_chain_has_call((const char (*)[FLEXNET_MAX_CALLSIGN])chain, 2,
                           "iw2ohx-12"), "case-insensitive");
    ok(!flex_chain_has_call((const char (*)[FLEXNET_MAX_CALLSIGN])chain, 2,
                            "IW2OHX-1"), "another SSID is not us");
    ok(!flex_chain_has_call((const char (*)[FLEXNET_MAX_CALLSIGN])chain, 1,
                            "IQ2LB"), "only the first n entries count");
    ok(!flex_chain_has_call((const char (*)[FLEXNET_MAX_CALLSIGN])chain, 2,
                            ""), "empty call never matches");

    FlexNetSessions[0].active = TRUE;
    FlexNetSessions[0].LINK = &links[0];
    ax(links[0].LINKCALL, "IQ2LB", 0);
    FlexNetSessions[1].active = TRUE;
    FlexNetSessions[1].LINK = &links[1];
    ax(links[1].LINKCALL, "NODEB", 4);
    ok(flex_target_is_direct_peer("IQ2LB"), "direct peer");
    ok(flex_target_is_direct_peer("IQ2LB-0"), "CALL-0 is CALL");
    ok(flex_target_is_direct_peer("NODEB-4"), "SSID peer");
    ok(!flex_target_is_direct_peer("NODEB"), "wrong SSID is not the peer");
    ok(!flex_target_is_direct_peer("IQ2LB-6"), "other SSID of a peer's base");
    ok(!flex_target_is_direct_peer(""), "empty target");

    printf("test_path_loop: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
