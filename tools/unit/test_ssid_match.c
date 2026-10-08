/* v2.6 path queries between nodes that share a base callsign.
 *
 * Found in the IW2OHX-12 neighbour QA (2026-10-08): IR2UFV showed
 * `route: IR2UFV IW2OHX-13` for its own neighbour IW2OHX-12. Three faults:
 *   - the probe for a destination whose neighbour index had been cleared
 *     went to the first active session (IW2OHX-13), not to IW2OHX-12;
 *   - IW2OHX-13 took any IW2OHX-n target for itself and answered [IW2OHX-13];
 *   - IR2UFV cached a chain that does not end at the destination.
 * Pins flex_call_in_range(), flex_chain_ends_at(), flex_target_is_us()
 * and flex_probe_session().
 *
 * Extracted verbatim from FlexNetCode.c by tools/unit/extract.sh.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#define FLEXNET_MAX_CALLSIGN 10
#define FLEXNET_MAX_SESSIONS 8
#define FLEXNET_MAX_PATH_HOPS 8

typedef int BOOL;
typedef unsigned char UCHAR;
#define TRUE  1
#define FALSE 0

typedef struct { UCHAR LINKCALL[7]; } LINKTABLE;
struct FLEXNET_SESSION
{
    LINKTABLE * LINK;
    BOOL active;
    UCHAR peer_callsign[7];
};
static struct FLEXNET_SESSION FlexNetSessions[FLEXNET_MAX_SESSIONS];
static LINKTABLE links[FLEXNET_MAX_SESSIONS];

struct FLEXNET_DEST_ENTRY
{
    char callsign[FLEXNET_MAX_CALLSIGN];
    int  ssid_lo, ssid_hi;
    int  via_session_idx;
    char via_callsign[FLEXNET_MAX_CALLSIGN];
};
static struct FLEXNET_DEST_ENTRY FlexNetDests[4];
static int FlexNetDestCount = 0;

static UCHAR MYCALL[7];
static int g_flexnet_ssid_lo = -1;
static int g_flexnet_ssid_hi = -1;

static int local_hit = 0;
static int flex_local_find(const char * call)
{
    return (local_hit && strcmp(call, "IW2OHX-9") == 0) ? 0 : -1;
}

/* BPQ's ConvFromAX25: space-fills 10 bytes, no terminator, returns len.
   `void *` out: the extracted callers pass both char and UCHAR buffers. */
static int ConvFromAX25(UCHAR * in, void * out)
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

#include "extracted_ssid_match.inc"

static int failures = 0, checks = 0;
static void ok(int cond, const char * what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static void ax(UCHAR * out, const char * call, int ssid)
{
    memset(out, 0x40, 6);
    for (int i = 0; call[i] && i < 6; i++) out[i] = (UCHAR)(call[i] << 1);
    out[6] = (UCHAR)(0x60 | (ssid << 1));
}

static void session(int i, const char * call, int ssid)
{
    FlexNetSessions[i].active = TRUE;
    FlexNetSessions[i].LINK = &links[i];
    ax(links[i].LINKCALL, call, ssid);
    memcpy(FlexNetSessions[i].peer_callsign, links[i].LINKCALL, 7);
}

static void range_cases(void)
{
    ok(flex_call_in_range("IW2OHX-12", "IW2OHX", 12, 12), "exact SSID");
    ok(!flex_call_in_range("IW2OHX-13", "IW2OHX", 12, 12), "other SSID");
    ok(flex_call_in_range("IW2OHX", "IW2OHX", 0, 0), "no SSID is -0");
    ok(flex_call_in_range("iw2ohx-4", "IW2OHX", 0, 8), "case-insensitive");
    ok(!flex_call_in_range("IW2OHX-9", "IW2OHX", 0, 8), "past hi");
    ok(!flex_call_in_range("IW2OHX1-4", "IW2OHX", 0, 15), "longer base");
    ok(!flex_call_in_range("IW2OHX-", "IW2OHX", 0, 15), "dangling dash");
    ok(!flex_call_in_range("IW2OHX-16", "IW2OHX", 0, 15), "SSID > 15");
    ok(!flex_call_in_range(NULL, "IW2OHX", 0, 15), "NULL call");
    ok(!flex_call_in_range("IW2OHX", "", 0, 15), "empty base");

    char chain[FLEXNET_MAX_PATH_HOPS][FLEXNET_MAX_CALLSIGN] = {{0}};
    strcpy(chain[0], "IW2OHX-13");
    ok(!flex_chain_ends_at((const char (*)[FLEXNET_MAX_CALLSIGN])chain, 1,
                           "IW2OHX", 12, 12),
       "[IW2OHX-13] is not a path to IW2OHX-12 (the bug)");
    strcpy(chain[0], "IW2OHX-14");
    strcpy(chain[1], "HB9ON-15");
    strcpy(chain[2], "DB0RES");
    ok(flex_chain_ends_at((const char (*)[FLEXNET_MAX_CALLSIGN])chain, 3,
                          "DB0RES", 0, 9), "ends at a range destination");
    ok(!flex_chain_ends_at((const char (*)[FLEXNET_MAX_CALLSIGN])chain, 2,
                           "DB0RES", 0, 9), "truncated chain");
    ok(!flex_chain_ends_at((const char (*)[FLEXNET_MAX_CALLSIGN])chain, 0,
                           "DB0RES", 0, 9), "empty chain");
}

static void target_cases(void)
{
    ax(MYCALL, "IW2OHX", 13);
    g_flexnet_ssid_lo = g_flexnet_ssid_hi = -1;
    ok(flex_target_is_us("IW2OHX-13") == FLEX_TARGET_NODE, "our own call");
    ok(flex_target_is_us("iw2ohx-13") == FLEX_TARGET_NODE, "case-insensitive");
    ok(flex_target_is_us("IW2OHX-12") == 0,
       "IW2OHX-12 is not IW2OHX-13 (the bug)");
    ok(flex_target_is_us("IW2OHX") == 0, "bare base is -0, not us");
    ok(flex_target_is_us("IQ2LB") == 0, "another call");

    ax(MYCALL, "IR2UFV", 0);
    g_flexnet_ssid_lo = 0; g_flexnet_ssid_hi = 8;
    ok(flex_target_is_us("IR2UFV-5") == FLEX_TARGET_NODE,
       "inside FLEXNETSSIDRANGE");
    ok(flex_target_is_us("IR2UFV") == FLEX_TARGET_NODE, "bare base in range");
    ok(flex_target_is_us("IR2UFV-9") == 0, "outside FLEXNETSSIDRANGE");
    g_flexnet_ssid_lo = g_flexnet_ssid_hi = -1;

    ax(MYCALL, "IW2OHX", 12);
    local_hit = 1;
    ok(flex_target_is_us("IW2OHX-9") == FLEX_TARGET_LOCAL,
       "local call still wins");
    local_hit = 0;
}

static void probe_cases(void)
{
    /* IR2UFV's sessions: -13 first, -14, then -12. */
    session(0, "IW2OHX", 13);
    session(1, "IW2OHX", 14);
    session(2, "IW2OHX", 12);

    FlexNetDestCount = 2;
    strcpy(FlexNetDests[0].callsign, "IW2OHX");
    FlexNetDests[0].ssid_lo = FlexNetDests[0].ssid_hi = 12;
    FlexNetDests[0].via_session_idx = -1;      /* cleared by a peer death */
    FlexNetDests[0].via_callsign[0] = '\0';
    ok(flex_probe_session(0, "IW2OHX-12") == 2,
       "cleared index, direct neighbour: its own session, not the first");

    FlexNetDests[0].via_session_idx = 1;
    ok(flex_probe_session(0, "IW2OHX-12") == 1, "a live index is used");

    FlexNetSessions[1].active = FALSE;
    strcpy(FlexNetDests[0].via_callsign, "IW2OHX-12");
    ok(flex_probe_session(0, "IW2OHX-12") == 2, "healed from via_callsign");
    ok(FlexNetDests[0].via_session_idx == 2, "heal is written back");
    FlexNetSessions[1].active = TRUE;

    strcpy(FlexNetDests[1].callsign, "DB0RES");
    FlexNetDests[1].ssid_lo = 0; FlexNetDests[1].ssid_hi = 9;
    FlexNetDests[1].via_session_idx = -1;
    FlexNetDests[1].via_callsign[0] = '\0';
    ok(flex_probe_session(1, "DB0RES-0") == -1,
       "no neighbour known: no probe rather than a guess");

    session(3, "IQ2LB", 0);
    ok(flex_probe_session(-1, "IQ2LB-0") == 3, "CALL-0 finds the CALL peer");
    ok(flex_probe_session(-1, NULL) == -1, "NULL target");
}

int main(void)
{
    range_cases();
    target_cases();
    probe_cases();
    printf("test_ssid_match: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
