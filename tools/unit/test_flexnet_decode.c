/* Monitor decode of FlexNet PID 0xCE frames and of PIDs nothing else decodes
 * (Moncode.c DISPLAYFLEXNET / DISPLAYOTHERPID, v2.11.7).
 *
 * Frames are the PROTOCOL_SPEC.md Appendix A examples plus boundary cases.
 * Extracted verbatim from Moncode.c by tools/unit/extract.sh. Each frame is
 * copied into a heap block of exactly `len` bytes, so ASan catches any read
 * past what the frame carries.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char UCHAR;

#include "extracted_flexnet_decode.inc"

static int failures = 0, checks = 0;

static void ok(int cond, const char * what, const char * got)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n    got: %s\n", what, got); }
}

static const char * last;
static size_t last_len;

/* The telnet monitor frames blocks with 0xFE/0xFF: never let one through. */
static void check_clean(const char * what)
{
    size_t i;
    for (i = 0; i < last_len; i++)
        if ((UCHAR)last[i] == 0xFE || (UCHAR)last[i] == 0xFF || (UCHAR)last[i] == 0)
            break;
    ok(i == last_len, what, last);
}

static UCHAR * heap_copy(const UCHAR * src, int len)
{
    UCHAR * copy = malloc(len > 0 ? (size_t)len : 1);
    if (!copy) { perror("malloc"); exit(2); }
    if (len > 0)
        memcpy(copy, src, (size_t)len);
    return copy;
}

static const char * ce(const void * frame, int len)
{
    static char out[4096];
    UCHAR * copy = heap_copy(frame, len);
    UCHAR * end;
    memset(out, 0, sizeof(out));
    end = DISPLAYFLEXNET(copy, len, (UCHAR *)out);
    *end = 0;
    free(copy);
    last = out;
    last_len = (size_t)(end - (UCHAR *)out);
    check_clean("no NUL/0xFE/0xFF in output");
    return out;
}

static const char * other(int pid, const void * data, int len)
{
    static char out[4096];
    UCHAR * copy = heap_copy(data, len);
    UCHAR * end;
    memset(out, 0, sizeof(out));
    end = DISPLAYOTHERPID(pid, copy, len, (UCHAR *)out);
    *end = 0;
    free(copy);
    last = out;
    last_len = (size_t)(end - (UCHAR *)out);
    check_clean("no NUL/0xFE/0xFF in output");
    return out;
}

#define CE(lit) ce(lit, (int)sizeof(lit) - 1)
#define HAS(s, sub) ok(strstr((s), (sub)) != NULL, "contains \"" sub "\"", (s))
#define LACKS(s, sub) ok(strstr((s), (sub)) == NULL, "lacks \"" sub "\"", (s))

static void test_init(void)
{
    const char * r = CE("08%!\r");                     /* A.1, (X)Net form */
    HAS(r, " <FlexNet> init, max SSID 8");
    r = CE("08  !\r");                                 /* PC/Flexnet 6-byte form */
    HAS(r, "init, max SSID 8");
    r = CE("0<%!\r");                                  /* §5.2: '<' = SSID 12 */
    HAS(r, "max SSID 12");
    r = CE("0");                                       /* no MAXSSID: falls to raw */
    HAS(r, "type '0', 1 bytes");
}

static void test_link_time(void)
{
    const char * r = CE("15\r");                       /* A.3 */
    HAS(r, " <FlexNet> link time 5");
    r = CE("10\r");
    HAS(r, "link time 0");
    r = CE("1600\r");
    HAS(r, "link time 600");
    r = CE("1\r");                                     /* no digits */
    HAS(r, "type '1'");
}

static void test_keepalive(void)
{
    UCHAR xnet[241], pcf[201];
    const char * r;
    memset(xnet, ' ', sizeof(xnet)); xnet[0] = '2';    /* A.2: '2' + 240 spaces */
    memset(pcf, ' ', sizeof(pcf)); pcf[0] = '2'; pcf[200] = '\r';
    r = ce(xnet, 241);
    HAS(r, " <FlexNet> keepalive (241 bytes)");
    LACKS(r, "CR");
    r = ce(pcf, 201);
    HAS(r, "keepalive (201 bytes, CR)");
}

static void test_tokens(void)
{
    const char * r = CE("3+\r");                       /* A.7 */
    HAS(r, " <FlexNet> route request (3+)");
    r = CE("3-\r");
    HAS(r, " <FlexNet> end of routes (3-)");
    r = CE("3+");
    HAS(r, "route request (3+)");
}

static void test_records(void)
{
    const char * r = CE("3NODEA 081 \r");              /* A.4 */
    HAS(r, " <FlexNet> 1 route:");
    HAS(r, "\r  NODEA  0-8       1");

    /* A.5: three records in one frame */
    r = CE("3NODEB 223 NODEC 0017 DEST  0042 \r");
    HAS(r, " <FlexNet> 3 routes:");
    HAS(r, "NODEB  2         3");
    HAS(r, "NODEC  0        17");
    HAS(r, "DEST   0        42");
    LACKS(r, "unparsed");

    /* A.6: '-' before CR withdraws every record */
    r = CE("3NODEC 0060000 DEST  0060000 -\r");
    HAS(r, "2 routes withdrawn:");
    HAS(r, "NODEC  0       inf");
    LACKS(r, "60000");

    /* cost 60000 without the frame flag is infinity too */
    r = CE("3NODEC 0060000 \r");
    HAS(r, "1 route:");
    HAS(r, "inf");

    /* six-letter call, SSID range 4-12 ('<' = 12), §5.2 */
    r = CE("3NODEAB4<1 \r");
    HAS(r, "NODEAB 4-12      1");

    /* §5.6.6 '?' before the cost */
    r = CE("3NODEA 00?42 \r");
    HAS(r, "?42");

    /* cost 0 refresh marker (§5.6.5) is shown as 0, not as inf */
    r = CE("3NODEA 000 \r");
    HAS(r, "NODEA  0         0");

    /* last record without the trailing space, no CR */
    r = CE("3NODEA 0012");
    HAS(r, "1 route:");
    LACKS(r, "unparsed");
}

static void test_records_bad(void)
{
    const char * r = CE("3NODEA 0");                   /* truncated record */
    HAS(r, "0 routes:");
    HAS(r, "unparsed at byte 1:");

    r = CE("3NODEA 001 nodeb 001 \r");                 /* lower case: not a call */
    HAS(r, "1 route:");
    HAS(r, "unparsed at byte 11:");
    HAS(r, "Data: nodeb 001");

    r = CE("3NO DEA001 \r");                           /* space inside the call */
    HAS(r, "0 routes:");

    r = CE("3NODEA 0Z1 \r");                           /* SSID byte out of range */
    HAS(r, "0 routes:");

    r = CE("3NODEA 00123456 \r");                      /* > 5 cost digits */
    HAS(r, "0 routes:");
}

/* The output budget holds for a full PACLEN frame and for a bogus huge one. */
static void test_budget(void)
{
    static UCHAR f[2000];
    const char * r;
    int len = 1, n;

    f[0] = '3';
    while (len + 11 <= 255)
    {
        memcpy(f + len, "NODEAB0<9 ", 10);         /* 10-byte records, the shortest */
        len += 10;
    }
    f[len++] = '\r';
    r = ce(f, len);
    HAS(r, "25 routes:");
    ok(last_len < MON_CE_BUDGET, "full frame within MON_CE_BUDGET", r);

    len = 1;
    while (len + 11 <= (int)sizeof(f))
    {
        memcpy(f + len, "NODEAB0<9 ", 10);
        len += 10;
    }
    r = ce(f, len);
    HAS(r, "199 routes:");
    HAS(r, " more");
    ok(last_len < MON_CE_BUDGET, "huge frame within MON_CE_BUDGET", r);
    n = 0;
    for (r = last; (r = strstr(r, "NODEAB")) != NULL; r++)
        n++;
    ok(n > 10 && n < 30, "huge frame shows some but not all records", last);
}

static void test_path(void)
{
    /* A.8: request, HOP 0x21, QSO "   12" */
    const char * r = CE("6!   12NODEA NODEB DEST");
    HAS(r, " <FlexNet> path request hop 1 qso 12:");
    HAS(r, "\r  NODEA NODEB DEST");
    LACKS(r, "trace");

    r = CE("7\"   12NODEA NODEB NODEC DEST\r");
    HAS(r, "path reply hop 2 qso 12:");
    HAS(r, "NODEA NODEB NODEC DEST");

    /* §5.9: bit 0x40 of the first QSO byte is a trace flag */
    r = CE("6!`  12NODEA DEST");
    HAS(r, "qso 12 trace:");

    r = CE("6!  ");                                    /* short */
    HAS(r, "path request (short, 4 bytes)");

    {
        static UCHAR f[600];
        memcpy(f, "6!    7", 7);
        memset(f + 7, 'A', sizeof(f) - 7);
        f[100] = 0xFF;
        f[101] = 0xFE;
        r = ce(f, (int)sizeof(f));
        HAS(r, "...");
        HAS(r, "..");
        ok(last_len < MON_CE_BUDGET, "long chain within MON_CE_BUDGET", r);
    }
}

static void test_other_types(void)
{
    const char * r = CE("412\r");                      /* §5.7, (X)Net form */
    HAS(r, " <FlexNet> sequence 12");
    LACKS(r, "\r");

    r = CE("412A\r");                                  /* PC/Flexnet form */
    HAS(r, "sequence 12A");

    r = CE("5xyz");                                    /* §5.8 reserved */
    HAS(r, " <FlexNet> type '5', 4 bytes");
    HAS(r, "Data: 5xyz");

    {
        UCHAR f[3] = { 0x01, 0xFF, 0x41 };
        r = ce(f, 3);
        HAS(r, "type 0x01, 3 bytes");
        HAS(r, "Data: ..A");
    }

    r = ce("", 0);
    HAS(r, " <FlexNet> (empty)");
}

static void test_other_pid(void)
{
    const char * r = other(0x01, "hello", 5);
    HAS(r, " pid=01 len 5");
    HAS(r, "\r  Data: hello");

    r = other(0xC3, "", 0);
    HAS(r, " pid=C3 len 0");
    LACKS(r, "Data");

    r = other(0x08, "x", -7);                          /* negative length clamps */
    HAS(r, "len 0");

    {
        static UCHAR big[300];
        memset(big, 'B', sizeof(big));
        r = other(0xCB, big, (int)sizeof(big));
        HAS(r, "pid=CB len 300");
        HAS(r, "...");
        ok(last_len < 200, "preview capped", r);
    }
}

int main(void)
{
    test_init();
    test_link_time();
    test_keepalive();
    test_tokens();
    test_records();
    test_records_bad();
    test_budget();
    test_path();
    test_other_types();
    test_other_pid();
    printf("flexnet monitor decode: %d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
