/* Round-trip test for the compact CE route encoding.
 *
 * The defect this guards: the emitter used to write one record per AX.25
 * I-frame ('3' + record + CR), using 15 bytes of a 236-byte PACLEN, while
 * every peer in the mesh packs many records behind a single '3'. These
 * tests pin the packed shape against bytes captured from real (X)Net and
 * PC/Flexnet peers on 2026-09-19, and assert that what we build parses
 * back to what we put in.
 *
 * The functions under test are extracted verbatim from FlexNetCode.c by
 * tools/unit/extract.sh, so the test cannot drift from shipped code.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>

#define FLEXNET_MAX_CALLSIGN   10
#define FLEXNET_SSID_BASE      0x30
#define FLEXNET_RTT_INFINITY   60000
#define FLEXNET_MAX_PATH_HOPS  8
#define FLEXNET_ADVERT_FRAME_BYTES 200
#define FLEXNET_ADVERT_RECS_PCF    16
#define FLEXNET_RTT_WIRE_MAX    4095
#define FLEXNET_CLIMB_RATIO        4
#define FLEXNET_CLIMB_MIN_STEPS    3
#define FLEX_CLIMB_OK        0
#define FLEX_CLIMB_WITHDRAW  1
#define FLEX_CLIMB_SUPPRESS  2

typedef int BOOL;
#define TRUE  1
#define FALSE 0

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

#include "extracted.inc"

static int failures = 0, checks = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

/* Mirror of the drain loop's framing: '3' + N records + CR. */
static int pack(unsigned char *frame, int cap,
                const char calls[][8], const int *lo, const int *hi,
                const int *rtt, int n, int max_recs)
{
    int flen = 0, in_frame = 0;
    frame[flen++] = '3';
    for (int i = 0; i < n && in_frame < max_recs; i++) {
        int rl = flex_build_route_rec(frame + flen, cap - flen - 1,
                                      calls[i], lo[i], hi[i], rtt[i]);
        if (rl < 0) break;
        flen += rl; in_frame++;
    }
    if (in_frame == 0) return -1;
    frame[flen++] = '\r';
    return flen;
}

int main(void)
{
    /* extract.sh emits one shared .inc for every test here; this one
       exercises the wire-format half. Reference the decision-rule
       function so the shared include stays warning-free. */
    (void)flex_climb_is_loop;

    struct FLEXNET_DEST_ENTRY out[64];
    unsigned char frame[FLEXNET_ADVERT_FRAME_BYTES];

    /* ---- 1. single-record frame keeps the exact legacy bytes ---- */
    {
        unsigned char b[64];
        int n = flex_build_route(b, sizeof(b), "IW2OHX", 4, 4, 7);
        ok(n == 12 && memcmp(b, "3IW2OHX447 \r", 12) == 0,
           "single-record frame is byte-identical to the pre-pack format");
        printf("  single : %.*s\n", n - 1, b);
    }

    /* ---- 2. round-trip a packed frame ---- */
    {
        const char calls[][8] = {"CQ0PAX","CS5LX","DB0AAT","IR2UFV","IW2OHX"};
        int lo[]  = {8, 6, 0, 0, 3};
        int hi[]  = {8, 6, 9, 8, 3};
        int rtt[] = {24, 186, 122, 1, 5};
        int flen = pack(frame, sizeof(frame), calls, lo, hi, rtt, 5,
                        FLEXNET_ADVERT_RECS_PCF);
        printf("  packed : %.*s  (%d bytes, 5 records)\n", flen - 1, frame, flen);
        int got = flex_parse_compact_records(frame, flen, out, 64);
        ok(got == 5, "packed frame parses back to 5 records");
        for (int i = 0; i < got && i < 5; i++) {
            ok(strcmp(out[i].callsign, calls[i]) == 0, "callsign round-trips");
            ok(out[i].ssid_lo == lo[i] && out[i].ssid_hi == hi[i],
               "ssid range round-trips");
            ok(out[i].rtt == rtt[i], "rtt round-trips");
        }
    }

    /* ---- 3. real captured (X)Net frame (IW2OHX-14, 2026-09-19) ---- */
    {
        char cap[] = "3CQ0PAX880 CQ0PCR880 CQ0PPA550 CQ0PPA890 CQ0PSX890 "
                     "CQ0UFL890 CS0RCL550 \r";
        int got = flex_parse_compact_records((unsigned char *)cap,
                                             (int)strlen(cap), out, 64);
        ok(got == 7, "captured (X)Net packed frame yields 7 records");
        ok(strcmp(out[0].callsign, "CQ0PAX") == 0 && out[0].ssid_lo == 8 &&
           out[0].ssid_hi == 8 && out[0].rtt == 0, "first (X)Net record decodes");
        ok(strcmp(out[6].callsign, "CS0RCL") == 0 && out[6].ssid_lo == 5,
           "last (X)Net record decodes");
    }

    /* ---- 4. real captured PC/Flexnet frame (IW2OHX-12, 2026-09-19) ---- */
    {
        char cap[] = "3CS0RCL89131 CS5LX 66186 CS5LX 8?129 CS5NRA88147 "
                     "DB0AAT09122 \r";
        int got = flex_parse_compact_records((unsigned char *)cap,
                                             (int)strlen(cap), out, 64);
        ok(got == 5, "captured PC/Flexnet packed frame yields 5 records");
        ok(strcmp(out[1].callsign, "CS5LX") == 0 && out[1].rtt == 186,
           "space-padded callsign decodes");
        ok(out[2].ssid_hi == 15, "'?' decodes as SSID 15");
    }

    /* ---- 5. our packed output is shaped like the peers' ---- */
    {
        const char calls[][8] = {"CS0RCL","CS5LX","DB0AAT"};
        int lo[] = {8, 6, 0}, hi[] = {9, 6, 9}, rtt[] = {131, 186, 122};
        int flen = pack(frame, sizeof(frame), calls, lo, hi, rtt, 3, 16);
        ok(flen > 0 &&
           memcmp(frame, "3CS0RCL89131 CS5LX 66186 DB0AAT09122 \r",
                  (size_t)flen) == 0,
           "our packed bytes match the captured peer shape exactly");
    }

    /* ---- 6. withdrawal (RTT infinity) survives packing ---- */
    {
        const char calls[][8] = {"DB0ZB","IQ2LB"};
        int lo[] = {0, 0}, hi[] = {15, 15};
        int rtt[] = {FLEXNET_RTT_INFINITY, 42};
        int flen = pack(frame, sizeof(frame), calls, lo, hi, rtt, 2, 16);
        int got = flex_parse_compact_records(frame, flen, out, 64);
        ok(got == 2, "withdrawal packs alongside a live route");
        ok(out[0].is_infinity == 1 && out[0].rtt >= FLEXNET_RTT_INFINITY,
           "withdrawn record decodes as infinity");
        ok(out[1].is_infinity == 0 && out[1].rtt == 42,
           "live record beside a withdrawal is unaffected");
    }

    /* ---- 7. byte budget: never overruns, never truncates a record ---- */
    {
        const char calls[][8] = {"DB0AAA"};
        int lo[] = {0}, hi[] = {15}, rtt[] = {60000};
        int n = 0;
        int flen = 0, in_frame = 0;
        frame[flen++] = '3';
        while (in_frame < 64) {
            int rl = flex_build_route_rec(frame + flen,
                                          (int)sizeof(frame) - flen - 1,
                                          calls[0], lo[0], hi[0], rtt[0]);
            if (rl < 0) break;
            flen += rl; in_frame++; n++;
        }
        frame[flen++] = '\r';
        ok(flen <= FLEXNET_ADVERT_FRAME_BYTES,
           "packed frame never exceeds the byte budget");
        int got = flex_parse_compact_records(frame, flen, out, 64);
        ok(got == n, "every record that was packed parses back out");
        printf("  budget : %d records of 14 B fit in %d bytes (cap %d)\n",
               n, flen, FLEXNET_ADVERT_FRAME_BYTES);
    }

    /* ---- 8. a record that cannot fit is rejected, not truncated ---- */
    {
        unsigned char tiny[8];
        ok(flex_build_route_rec(tiny, 4, "DB0AAA", 0, 15, 60000) == -1,
           "record too large for the remaining budget returns -1");
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
