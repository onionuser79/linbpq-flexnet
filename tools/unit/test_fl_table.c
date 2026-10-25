/* FL link table: rows laid out as (X)Net's L command prints them.
 *
 * The reference rows are live (X)Net 1.39 L output with the callsigns
 * replaced; every one must come out of flex_fl_format_row byte for byte.
 *
 * Extracted verbatim from FlexNetCode.c by tools/unit/extract.sh.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int BOOL;
#define TRUE 1
#define FALSE 0

#include "extracted_fl_table.inc"

static int failures = 0, checks = 0;
static void ok(int cond, const char * what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static void same(const char * got, const char * want, const char * what)
{
    checks++;
    if (strcmp(got, want) != 0)
    {
        failures++;
        printf("  FAIL: %s\n    got  '%s'\n    want '%s'\n", what, got, want);
    }
}

static struct flex_fl_row up_row(int port, const char * call, int dst,
                                 char type, int qt, int own, int peer,
                                 long con, long long tx, long long rx,
                                 int txq, int rxq, int rr, long long bps)
{
    struct flex_fl_row r;
    memset(&r, 0, sizeof(r));
    r.port = port;
    snprintf(r.call, sizeof(r.call), "%s", call);
    r.dst = dst;
    r.type = type;
    r.qt = qt;
    r.rtt_own = own;
    r.rtt_peer = peer;
    r.up = TRUE;
    r.connect_s = con;
    r.tx_bytes = tx;
    r.rx_bytes = rx;
    r.txq = txq;
    r.rxq = rxq;
    r.rr_tenths = rr;
    r.bps = bps;
    return r;
}

static void test_reference_rows(void)
{
    char line[128];
    struct flex_fl_row r;

    r = up_row(14, "NODEA", 2, 'Q', 254, -1, -1, 2 * 3600 + 24 * 60,
               44 * 1024, 24 * 1024, 100, 100, 0, 64);
    flex_fl_format_row(line, sizeof(line), &r);
    same(line, "14:NODEA        2 Q 254   -/-     0  2h 24m   44K  24K  100/100"
               "   0.0     64", "NET/ROM row");

    r = up_row(16, "NODEB-15", 104, 'I', 0, 0, 0, 15 * 3600 + 60,
               304 * 1024, 330 * 1024, 100, 100, 10, 94);
    flex_fl_format_row(line, sizeof(line), &r);
    same(line, "16:NODEB-15   104 I   0   0/0     0 15h 01m  304K 330K  100/100"
               "   1.0     94", "INP3 row, 3-digit dst and 7-char connect");

    r = up_row(5, "NODEE-15", 18, 'I', 1, 1, -1, 4 * 60 + 11,
               6861, 745, 100, 100, 711, 238);
    flex_fl_format_row(line, sizeof(line), &r);
    same(line, " 5:NODEE-15    18 I   1   1/-     0  4m 11s  6.7K 745   100/100"
               "  71.1    238", "peer time unknown, sub-1000 rx bytes");

    r = up_row(13, "NODEC-10", 2, 'F', 302, 3, 600, 3600 + 6 * 60,
               17 * 1024, 3174, 97, 4, 838, 42);
    flex_fl_format_row(line, sizeof(line), &r);
    same(line, "13:NODEC-10     2 F 302   3/600   0  1h 06m   17K 3.1K   97/4"
               "    83.8     42", "FlexNet row, 3-digit peer time");

    memset(&r, 0, sizeof(r));
    r.port = 7;
    snprintf(r.call, sizeof(r.call), "NODED");
    r.dst = -1;
    r.type = 'F';
    r.qt = r.rtt_own = r.rtt_peer = -1;
    flex_fl_format_row(line, sizeof(line), &r);
    same(line, " 7:NODED          F   -   -/-     -  - - -     -    -     -/-"
               "     -.-      -", "configured link that is down");
}

static void test_bytes(void)
{
    char b[16];
    flex_fl_bytes(b, sizeof(b), 0);           same(b, "  0 ", "0 bytes");
    flex_fl_bytes(b, sizeof(b), 999);         same(b, "999 ", "999 bytes stay plain");
    flex_fl_bytes(b, sizeof(b), 1000);        same(b, "1.0K", "1000 bytes go to K");
    flex_fl_bytes(b, sizeof(b), 10188);       same(b, "9.9K", "below 9.95K keeps a decimal");
    flex_fl_bytes(b, sizeof(b), 10240);       same(b, " 10K", "10K drops the decimal");
    flex_fl_bytes(b, sizeof(b), 1023 * 1024); same(b, "1.0M", "999.5K and up is M");
    flex_fl_bytes(b, sizeof(b), -5);          same(b, "  0 ", "negative clamps to 0");
    flex_fl_bytes(b, sizeof(b), 2147483647);  same(b, "2.0G", "INT_MAX fits the cell");
}

static void test_connect(void)
{
    char b[16];
    flex_fl_connect(b, sizeof(b), 0);      same(b, "0m 00s", "zero");
    flex_fl_connect(b, sizeof(b), 3599);   same(b, "59m 59s", "just under an hour");
    flex_fl_connect(b, sizeof(b), 3600);   same(b, "1h 00m", "one hour");
    flex_fl_connect(b, sizeof(b), 86399);  same(b, "23h 59m", "just under a day");
    flex_fl_connect(b, sizeof(b), 86400);  same(b, "1d 00h", "one day");
    flex_fl_connect(b, sizeof(b), 6 * 86400 + 8 * 3600); same(b, "6d 08h", "days");
    flex_fl_connect(b, sizeof(b), -10);    same(b, "0m 00s", "clock step back clamps");
}

static void test_ratios(void)
{
    ok(flex_fl_pct(0, 0) == 100, "nothing sent: no loss");
    ok(flex_fl_pct(999, 1) == 99, "any loss shows below 100");
    ok(flex_fl_pct(1, 24) == 4, "1 of 25 good = 4");
    ok(flex_fl_pct(0, 5) == 0, "all bad = 0");
    ok(flex_fl_pct(-1, -1) == 100, "negative counters clamp");
    ok(flex_fl_rr_tenths(0, 0) == 0, "no frames: 0.0");
    ok(flex_fl_rr_tenths(1, 999) == 1, "1 poll in 1000 frames = 0.1");
    ok(flex_fl_rr_tenths(838, 162) == 838, "83.8");
    ok(flex_fl_rr_tenths(5, 0) == 1000, "only polls = 100.0");
    ok(flex_fl_rr_tenths(2147483647, 2147483647) == 500, "INT_MAX counters, no overflow");
}

static void test_link_times(void)
{
    ok(flex_fl_ticks_10ms(0) == -1, "SRTT 0 = not measured");
    ok(flex_fl_ticks_10ms(4) == 0, "40 ms rounds to 0 ticks");
    ok(flex_fl_ticks_10ms(5) == 1, "50 ms rounds to 1 tick");
    ok(flex_fl_ticks_10ms(48) == 5, "480 ms = 5 ticks");

    /* (own, peer) -> Q/T pairs read off live (X)Net L output. */
    ok(flex_fl_qt_avg(3, 600, 1) == 302, "3/600 -> 302");
    ok(flex_fl_qt_avg(0, 5, 1) == 3, "0/5 -> 3");
    ok(flex_fl_qt_avg(1, 2, 1) == 2, "1/2 -> 2");
    ok(flex_fl_qt_avg(1, 0, 1) == 1, "1/0 -> 1");
    ok(flex_fl_qt_avg(0, 0, 1) == 1, "F link never below 1");
    ok(flex_fl_qt_avg(0, 0, 0) == 0, "INP3 link may be 0");
    ok(flex_fl_qt_avg(0, 1, 0) == 1, "INP3 0/1 -> 1");
    ok(flex_fl_qt_avg(1, -1, 0) == 1, "peer unknown: own alone");
    ok(flex_fl_qt_avg(-1, 7, 1) == 7, "own unknown: peer alone");
    ok(flex_fl_qt_avg(-1, -1, 1) == -1, "neither known: '-'");
}

static void test_truncation(void)
{
    struct flex_fl_row r = up_row(99, "LONGCALL-15", 9999, 'F', 4095, 4095,
                                  99999, 99 * 86400, 2147483647, 2147483647,
                                  100, 100, 1000, 999999);
    char line[128];
    flex_fl_format_row(line, sizeof(line), &r);
    ok(strncmp(line, "99:LONGCALL- ", 13) == 0, "call cut to (X)Net's 9 chars");
    char tiny[16];
    flex_fl_format_row(tiny, sizeof(tiny), &r);
    ok(strlen(tiny) == sizeof(tiny) - 1, "short buffer is truncated and terminated");
}

static void test_detail_switch(void)
{
    ok(!flex_fl_wants_detail(NULL), "no tail: overview only");
    ok(!flex_fl_wants_detail(""), "FL alone: overview only");
    ok(!flex_fl_wants_detail("\r"), "FL<CR>: overview only");
    ok(!flex_fl_wants_detail("  X"), "unknown argument: overview only");
    ok(flex_fl_wants_detail("+"), "FL+: detail");
    ok(flex_fl_wants_detail(" +\r"), "FL +: detail");
    ok(flex_fl_wants_detail("\t +"), "leading blanks skipped");
}

int main(void)
{
    test_reference_rows();
    test_bytes();
    test_connect();
    test_ratios();
    test_link_times();
    test_truncation();
    test_detail_switch();
    printf("test_fl_table: %d/%d passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
