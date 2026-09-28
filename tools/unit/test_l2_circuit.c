/* The L2 transit circuit table — the hardening of FlexNet_L2Transit().
 *
 * The defect this guards: up to v2.2.3 the forward path re-resolved the
 * next hop for EVERY frame and overwrote the circuit's `appended` hop. If
 * the route to DEST changed mid-circuit, frames still returning over the
 * old hop no longer matched, were not contracted, and reached the
 * originator carrying a digi it never sent — a stranger's session broken
 * by us. v2.2.4 pins the hop per circuit, keeps the replaced pin
 * contractible, and ties a slot's life to the AX.25 teardown instead of
 * a flat 900 s idle timer.
 *
 * Everything under test is extracted verbatim from FlexNetCode.c by
 * tools/unit/extract.sh, so the test cannot drift from shipped code.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <stddef.h>

typedef int BOOL;
#define TRUE  1
#define FALSE 0
typedef unsigned char  UCHAR;
typedef unsigned short USHORT;
typedef void VOID;

/* The BPQ link-level buffer, reduced to the fields the helpers touch.
   The layout is what matters: DEST, ORIGIN, then up to 56 bytes of digis
   run on into CTL/PID/L2DATA as one contiguous frame. */
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

#include "extracted_l2_types.inc"

static struct FLEXNET_L2_TRANSIT FlexNetL2Transit[FLEXNET_MAX_L2_TRANSIT];
static unsigned long g_l2_fwd_evicted = 0;
static void FlexNet_Log(const char * fmt, ...) { (void)fmt; }

#include "extracted_l2.inc"

static int failures = 0, checks = 0;

static void ok(int cond, const char * what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static void reset(void)
{
    memset(FlexNetL2Transit, 0, sizeof(FlexNetL2Transit));
    g_l2_fwd_evicted = 0;
}

/* AX.25 address: shifted ASCII, SSID byte 0x60 | ssid<<1 | H | E. */
static void ax(UCHAR * out, const char * call, int ssid, int h, int e)
{
    memset(out, ' ' << 1, 6);
    for (int i = 0; i < 6 && call[i]; i++) out[i] = (UCHAR)(call[i] << 1);
    out[6] = (UCHAR)(0x60 | (ssid << 1) | (h ? 0x80 : 0) | (e ? 0x01 : 0));
}

/* Through a byte pointer, never m->DEST[n]: the frame deliberately runs
   past DEST[7] into the digi area, which BPQ overlays on the struct. */
static UCHAR * frame_base(MESSAGE * m)
{
    return (UCHAR *)m + offsetof(MESSAGE, DEST);
}

struct Hop { const char * call; int ssid; int h; };

/* Build DEST, ORIGIN, digis..., CTL, PID, then `info`. */
static void frame(MESSAGE * m, const char * dest, int dssid,
                  const char * orig, int ossid,
                  const struct Hop * digis, int n, UCHAR ctl, const char * info)
{
    memset(m, 0, sizeof(*m));
    UCHAR * p = frame_base(m);
    ax(p, dest, dssid, 0, 0);        p += 7;
    ax(p, orig, ossid, 0, n == 0);   p += 7;
    for (int i = 0; i < n; i++, p += 7)
        ax(p, digis[i].call, digis[i].ssid, digis[i].h, i == n - 1);
    *p++ = ctl;
    *p++ = 0xF0;
    size_t il = strlen(info);
    memcpy(p, info, il);
    p += il;
    m->LENGTH = (USHORT)(MSGHDDRLEN + (p - frame_base(m)));
}

static UCHAR * digi_at(MESSAGE * m, int i) { return frame_base(m) + 14 + 7 * i; }

static void test_ctl_kind(void)
{
    ok(flex_l2_ctl_kind(0x3F) == FLEX_L2K_SABM, "SABM with P");
    ok(flex_l2_ctl_kind(0x2F) == FLEX_L2K_SABM, "SABM without P");
    ok(flex_l2_ctl_kind(0x6F) == FLEX_L2K_SABM, "SABME counts as SABM");
    ok(flex_l2_ctl_kind(0x7F) == FLEX_L2K_SABM, "SABME with P");
    ok(flex_l2_ctl_kind(0x53) == FLEX_L2K_DISC, "DISC with P");
    ok(flex_l2_ctl_kind(0x43) == FLEX_L2K_DISC, "DISC without P");
    ok(flex_l2_ctl_kind(0x73) == FLEX_L2K_UA,   "UA with F");
    ok(flex_l2_ctl_kind(0x63) == FLEX_L2K_UA,   "UA without F");
    ok(flex_l2_ctl_kind(0x1F) == FLEX_L2K_DM,   "DM with F");
    ok(flex_l2_ctl_kind(0x0F) == FLEX_L2K_DM,   "DM without F");
    /* Everything that carries or supervises a live connection is OTHER —
       an I-frame whose N(S)/N(R) bits happen to spell 0x43 must never
       read as a DISC. */
    ok(flex_l2_ctl_kind(0x00) == FLEX_L2K_OTHER, "I frame");
    ok(flex_l2_ctl_kind(0x42) == FLEX_L2K_OTHER, "I frame shaped like DISC");
    ok(flex_l2_ctl_kind(0x62) == FLEX_L2K_OTHER, "I frame shaped like UA");
    ok(flex_l2_ctl_kind(0x01) == FLEX_L2K_OTHER, "RR");
    ok(flex_l2_ctl_kind(0x21) == FLEX_L2K_OTHER, "RR N(R)=1");
    ok(flex_l2_ctl_kind(0x05) == FLEX_L2K_OTHER, "RNR");
    ok(flex_l2_ctl_kind(0x09) == FLEX_L2K_OTHER, "REJ");
    ok(flex_l2_ctl_kind(0x03) == FLEX_L2K_OTHER, "UI");
    ok(flex_l2_ctl_kind(0x87) == FLEX_L2K_OTHER, "FRMR");
    ok(flex_l2_ctl_kind(0xAF) == FLEX_L2K_OTHER, "XID");
}

static void test_slot_state_boundaries(void)
{
    time_t t = 1000000;
    ok(flex_l2_slot_state(t, t, 0) == FLEX_L2S_LIVE, "fresh open circuit live");
    ok(flex_l2_slot_state(t + FLEXNET_L2_TRANSIT_EVICT, t, 0) == FLEX_L2S_LIVE,
       "open, idle == EVICT still live");
    ok(flex_l2_slot_state(t + FLEXNET_L2_TRANSIT_EVICT + 1, t, 0)
           == FLEX_L2S_EVICTABLE, "open, idle > EVICT evictable");
    ok(flex_l2_slot_state(t + FLEXNET_L2_TRANSIT_IDLE, t, 0)
           == FLEX_L2S_EVICTABLE, "open, idle == IDLE not yet expired");
    ok(flex_l2_slot_state(t + FLEXNET_L2_TRANSIT_IDLE + 1, t, 0)
           == FLEX_L2S_EXPIRED, "open, idle > IDLE expired");
    /* The v2.2.3 hazard: 900 s of silence on an open circuit used to free
       the slot. It must survive now unless the table is under pressure. */
    ok(flex_l2_slot_state(t + 1800, t, 0) != FLEX_L2S_EXPIRED,
       "open circuit silent 30 min keeps its contraction");
    ok(flex_l2_slot_state(t, t, t) == FLEX_L2S_EVICTABLE,
       "just-closed circuit evictable, not expired");
    ok(flex_l2_slot_state(t + FLEXNET_L2_TRANSIT_LINGER, t, t)
           == FLEX_L2S_EVICTABLE, "closed, == LINGER still contracts retries");
    ok(flex_l2_slot_state(t + FLEXNET_L2_TRANSIT_LINGER + 1, t, t)
           == FLEX_L2S_EXPIRED, "closed, > LINGER expired");
}

static void test_must_repin(void)
{
    ok(flex_l2_must_repin(0, FLEX_L2K_OTHER, 1) == 0, "open, hop live: keep pin");
    ok(flex_l2_must_repin(0, FLEX_L2K_SABM, 1) == 0,
       "SABM on an OPEN circuit (reset/retry) keeps the pin");
    ok(flex_l2_must_repin(1, FLEX_L2K_DISC, 1) == 0,
       "retransmitted DISC after close keeps the pin");
    ok(flex_l2_must_repin(1, FLEX_L2K_SABM, 1) == 1,
       "SABM after completed teardown = new connection, re-resolve");
    ok(flex_l2_must_repin(0, FLEX_L2K_OTHER, 0) == 1, "pinned hop dead: re-pin");
    ok(flex_l2_must_repin(1, FLEX_L2K_UA, 0) == 1, "dead hop re-pins even closed");
}

static void test_lifecycle(void)
{
    struct FLEXNET_L2_TRANSIT e;
    memset(&e, 0, sizeof(e));
    flex_l2_note_ctl(&e, FLEX_L2K_UA, 10);
    ok(e.closed_at == 0, "UA to a SABM does not close");
    flex_l2_note_ctl(&e, FLEX_L2K_OTHER, 11);
    ok(e.closed_at == 0 && !e.closing, "I/S traffic changes nothing");
    flex_l2_note_ctl(&e, FLEX_L2K_DISC, 12);
    ok(e.closing && e.closed_at == 0, "DISC opens the teardown only");
    flex_l2_note_ctl(&e, FLEX_L2K_UA, 13);
    ok(e.closed_at == 13, "UA after DISC completes it");
    flex_l2_note_ctl(&e, FLEX_L2K_UA, 20);
    ok(e.closed_at == 13, "a retransmitted UA does not extend the linger");
    flex_l2_note_ctl(&e, FLEX_L2K_SABM, 30);
    ok(e.closed_at == 0 && !e.closing, "SABM reopens");
    flex_l2_note_ctl(&e, FLEX_L2K_DM, 40);
    ok(e.closed_at == 40, "DM closes without a DISC (refused SABM)");
}

static void test_is_our_hop(void)
{
    struct FLEXNET_L2_TRANSIT e;
    UCHAR h1[7], h2[7], h1_rep[7], h1_ssid[7], other_rep[7];
    memset(&e, 0, sizeof(e));
    ax(h1, "IW2OHX", 14, 0, 0);
    ax(h2, "IW2OHX", 12, 0, 0);
    ax(h1_rep, "IW2OHX", 14, 1, 0);
    ax(h1_ssid, "IW2OHX", 15, 1, 0);
    ax(other_rep, "DB0FHN", 0, 1, 0);
    memcpy(e.appended, h1, 7);

    ok(flex_l2_is_our_hop(&e, h1_rep) == 1, "pinned hop, repeated: ours");
    ok(flex_l2_is_our_hop(&e, h1) == 0,
       "pinned hop NOT yet repeated: not ours (never remove a pending digi)");
    ok(flex_l2_is_our_hop(&e, h1_ssid) == 0, "same base call, other SSID: not ours");
    ok(flex_l2_is_our_hop(&e, other_rep) == 0, "originator-supplied digi: not ours");

    memcpy(e.prev_appended, h1, 7);
    memcpy(e.appended, h2, 7);
    ok(flex_l2_is_our_hop(&e, h1_rep) == 1,
       "after re-pin the OLD hop is still contracted");
}

/* The defect, end to end on real frame bytes. A user connects through us;
   we pin H1. The route to DEST moves to H2 mid-circuit — v2.2.3 would
   have re-resolved and overwritten `appended`. The UA coming back over
   H1 must still be contracted to exactly the chain the user sent. */
static void test_route_change_mid_circuit(void)
{
    reset();
    const struct Hop fwd[] = { {"IW2OHX", 4, 1}, {"IR2UFV", 0, 0} };
    MESSAGE m;
    frame(&m, "IQ2LB", 6, "IW7EAS", 2, fwd, 2, 0x3F, "");
    USHORT len0 = m.LENGTH;

    UCHAR h1[7], h2[7];
    ax(h1, "IW2OHX", 14, 0, 0);
    ax(h2, "IW2OHX", 12, 0, 0);

    struct FLEXNET_L2_TRANSIT * e = flex_l2_find(m.ORIGIN, m.DEST, 2, TRUE);
    ok(e != NULL, "circuit created");
    ok(flex_l2_append_digi(&m, h1) == TRUE, "append H1");
    memcpy(e->appended, h1, 7);
    ok(flex_l2_digi_count(&m) == 3, "forward SABM now carries 3 digis");
    ok(m.LENGTH == len0 + 7, "length grew by one address");
    ok((digi_at(&m, 1)[6] & 0x01) == 0, "our entry lost its E bit");
    ok((digi_at(&m, 2)[6] & 0x81) == 0x01, "H1 appended unrepeated, E set");
    ok(*(digi_at(&m, 3)) == 0x3F, "control byte shifted intact");

    /* Route changes. Pinning keeps H1 while its session is live... */
    ok(flex_l2_must_repin(e->closed_at != 0, FLEX_L2K_OTHER, 1) == 0,
       "route change with H1 still live: pin held");
    /* ...and even if we are later forced to re-pin to H2, H1 stays ours. */
    memcpy(e->prev_appended, e->appended, 7);
    memcpy(e->appended, h2, 7);

    /* UA returns over H1:  IQ2LB-6 > IW7EAS-2  H1* IR2UFV IW2OHX-4 */
    const struct Hop rev[] = { {"IW2OHX", 14, 1}, {"IR2UFV", 0, 0},
                               {"IW2OHX", 4, 0} };
    MESSAGE r;
    frame(&r, "IW7EAS", 2, "IQ2LB", 6, rev, 3, 0x73, "");
    struct FLEXNET_L2_TRANSIT * re = flex_l2_find(r.DEST, r.ORIGIN, 2, FALSE);
    ok(re == e, "reverse frame finds the same circuit");
    UCHAR * ours = digi_at(&r, 1);
    ok(re && flex_l2_is_our_hop(re, ours - 7), "old-path hop recognised");
    ok(flex_l2_remove_digi(&r, ours - 7) == TRUE, "contract");
    ok(flex_l2_digi_count(&r) == 2, "originator sees 2 digis again");
    UCHAR us[7];
    ax(us, "IR2UFV", 0, 0, 0);
    ok(flex_l2_same_call(digi_at(&r, 0), us), "first digi is now us (IR2UFV)");
    ok((digi_at(&r, 1)[6] & 0x01) == 0x01, "IW2OHX-4 keeps the E bit");
    ok(*(digi_at(&r, 2)) == 0x73, "UA control byte shifted back intact");
}

static void test_find_eviction(void)
{
    reset();
    UCHAR u[7], d[7];
    time_t now = time(NULL);
    for (int i = 0; i < FLEXNET_MAX_L2_TRANSIT; i++)
    {
        ax(u, "USER", i % 16, 0, 0);
        ax(d, "DEST", i / 16, 0, 0);
        ok(flex_l2_find(u, d, 1, TRUE) != NULL, "fill slot");
    }
    ok(flex_l2_active_circuits() == FLEXNET_MAX_L2_TRANSIT, "table full");

    ax(u, "NEWBIE", 0, 0, 0);
    ax(d, "DEST", 0, 0, 0);
    ok(flex_l2_find(u, d, 1, TRUE) == NULL,
       "full of LIVE circuits: decline, never evict a live one");
    ok(g_l2_fwd_evicted == 0, "nothing evicted");

    FlexNetL2Transit[5].last_used = now - FLEXNET_L2_TRANSIT_EVICT - 60;
    FlexNetL2Transit[9].last_used = now - FLEXNET_L2_TRANSIT_EVICT - 600;
    FlexNetL2Transit[20].closed_at = now - 5;
    struct FLEXNET_L2_TRANSIT * s = flex_l2_find(u, d, 1, TRUE);
    ok(s == &FlexNetL2Transit[20], "closed circuit reclaimed before silent ones");
    ok(g_l2_fwd_evicted == 1, "eviction counted");

    ax(u, "NEWBIE", 1, 0, 0);
    s = flex_l2_find(u, d, 1, TRUE);
    ok(s == &FlexNetL2Transit[9], "then the longest-silent open circuit");

    ax(u, "USER", 3, 0, 0);
    ax(d, "DEST", 0, 0, 0);
    ok(flex_l2_find(u, d, 2, FALSE) == NULL, "port is part of the key");
    ok(flex_l2_find(u, d, 1, FALSE) == &FlexNetL2Transit[3], "exact key found");
}

static void test_find_expiry(void)
{
    reset();
    UCHAR u[7], d[7];
    ax(u, "IW7EAS", 2, 0, 0);
    ax(d, "IGATE", 0, 0, 0);
    struct FLEXNET_L2_TRANSIT * e = flex_l2_find(u, d, 1, TRUE);
    e->closed_at = time(NULL) - FLEXNET_L2_TRANSIT_LINGER - 1;
    ok(flex_l2_find(u, d, 1, FALSE) == NULL, "lingered-out circuit is gone");
    ok(flex_l2_active_circuits() == 0, "and not counted");
}

static void test_self_repeated(void)
{
    const struct Hop loop[] = { {"IR2UFV", 0, 1}, {"IW2OHX", 14, 1},
                                {"IR2UFV", 0, 0} };
    MESSAGE m;
    frame(&m, "IQ2LB", 6, "IW7EAS", 2, loop, 3, 0x00, "hello");
    ok(flex_l2_self_repeated(&m, digi_at(&m, 2)) == TRUE,
       "our call repeated earlier: loop");

    const struct Hop fine[] = { {"IW2OHX", 4, 1}, {"IR2UFV", 0, 0} };
    frame(&m, "IQ2LB", 6, "IW7EAS", 2, fine, 2, 0x00, "hello");
    ok(flex_l2_self_repeated(&m, digi_at(&m, 1)) == FALSE, "normal chain: no loop");

    const struct Hop pend[] = { {"IR2UFV", 0, 0}, {"IR2UFV", 0, 0} };
    frame(&m, "IQ2LB", 6, "IW7EAS", 2, pend, 2, 0x00, "hello");
    ok(flex_l2_self_repeated(&m, digi_at(&m, 1)) == FALSE,
       "an UNREPEATED earlier copy of us is not a loop");
}

static void test_append_limits(void)
{
    const struct Hop full[] = {
        {"A", 0, 1}, {"B", 0, 1}, {"C", 0, 1}, {"D", 0, 1},
        {"E", 0, 1}, {"F", 0, 1}, {"G", 0, 1}, {"IR2UFV", 0, 0} };
    MESSAGE m;
    frame(&m, "IQ2LB", 6, "IW7EAS", 2, full, 8, 0x00, "x");
    ok(flex_l2_digi_count(&m) == 8, "8 digis parsed");
    UCHAR ta[7];
    ax(ta, "IW2OHX", 14, 0, 0);
    ok(flex_l2_call_in_chain(&m, ta) == FALSE, "hop not in chain");

    char big[FLEXNET_L2_MAX_FRAME];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    frame(&m, "IQ2LB", 6, "IW7EAS", 2, full + 6, 2, 0x00, big);
    USHORT l0 = m.LENGTH;
    ok(flex_l2_append_digi(&m, ta) == FALSE, "refuse to grow past MAX_FRAME");
    ok(m.LENGTH == l0, "and leave the frame untouched");
}

int main(void)
{
    printf("test_l2_circuit\n");
    test_ctl_kind();
    test_slot_state_boundaries();
    test_must_repin();
    test_lifecycle();
    test_is_our_hop();
    test_route_change_mid_circuit();
    test_find_eviction();
    test_find_expiry();
    test_self_repeated();
    test_append_limits();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
