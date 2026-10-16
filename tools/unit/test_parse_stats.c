/* FL "Parser:" line (v2.7): counts frames a peer sent that the CE
 * dispatcher could not classify (unknown) or that failed to parse
 * (malformed), with the last one, on every build.
 *
 * Extracted verbatim from FlexNetCode.c by tools/unit/extract.sh.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "extracted_parse_stats.inc"

static int failures = 0, checks = 0;
static void ok(int cond, const char * what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

static struct tm fixed_tm(void)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_mday = 16; t.tm_hour = 8; t.tm_min = 13; t.tm_sec = 28;
    return t;
}

static void test_empty_footer(void)
{
    struct FLEX_PARSE_STATS st;
    memset(&st, 0, sizeof(st));
    char buf[192];
    int n = flex_parse_footer(buf, sizeof(buf), &st, NULL);
    ok(strcmp(buf, "Parser: unknown=0 malformed=0\r") == 0, "zeroed stats: counters only");
    ok(n == (int)strlen(buf), "returns the length written");
    struct tm t = fixed_tm();
    flex_parse_footer(buf, sizeof(buf), &st, &t);
    ok(strstr(buf, "last:") == NULL, "no last: while nothing was seen, even with a tm");
}

static void test_counts_and_last(void)
{
    struct FLEX_PARSE_STATS st;
    memset(&st, 0, sizeof(st));
    const unsigned char ce[] = {'8', 'x', 'y'};
    const unsigned char rep[] = {'7', '!'};
    flex_parse_note(&st, FLEX_PARSE_UNKNOWN, "NODEB-2", "CE", ce, 3, 100);
    flex_parse_note(&st, FLEX_PARSE_UNKNOWN, "NODEB-2", "CE", ce, 3, 101);
    flex_parse_note(&st, FLEX_PARSE_MALFORMED, "NODEC", "PATH-REP", rep, 2, 102);
    ok(st.unknown == 2 && st.malformed == 1, "counted per kind");
    ok(st.last_t == 102 && st.last_byte0 == '7' && st.last_len == 2, "last is the newest");

    struct tm t = fixed_tm();
    char buf[192];
    flex_parse_footer(buf, sizeof(buf), &st, &t);
    ok(strcmp(buf, "Parser: unknown=2 malformed=1  last: 16 08:13:28 NODEC "
                   "PATH-REP malformed byte0=0x37 len=2\r") == 0, "full line");
}

static void test_edges(void)
{
    struct FLEX_PARSE_STATS st;
    memset(&st, 0, sizeof(st));
    flex_parse_note(&st, FLEX_PARSE_UNKNOWN, NULL, NULL, NULL, 0, 5);
    ok(st.last_byte0 == -1 && strcmp(st.last_peer, "?") == 0 &&
       strcmp(st.last_what, "?") == 0, "NULL peer/what/data tolerated");
    struct tm t = fixed_tm();
    char buf[192];
    flex_parse_footer(buf, sizeof(buf), &st, &t);
    ok(strstr(buf, "byte0=- len=0\r") != NULL, "empty frame shows byte0=-");

    flex_parse_note(&st, FLEX_PARSE_UNKNOWN, "ABCDEFGHIJKLMNOP", "VERYLONGFAMILY",
                    (const unsigned char *)"\xff", 1, 6);
    ok(strlen(st.last_peer) == sizeof(st.last_peer) - 1, "long peer truncated, terminated");
    ok(strlen(st.last_what) == sizeof(st.last_what) - 1, "long family truncated, terminated");
    flex_parse_footer(buf, sizeof(buf), &st, &t);
    ok(strstr(buf, "byte0=0xFF") != NULL, "high byte rendered unsigned");

    char small[16];
    int n = flex_parse_footer(small, sizeof(small), &st, &t);
    ok(n > (int)sizeof(small) && strlen(small) == sizeof(small) - 1,
       "small buffer: truncated, terminated, wanted length returned");
    ok(flex_parse_footer(NULL, 10, &st, &t) == -1, "NULL buffer");
    ok(flex_parse_footer(buf, 0, &st, &t) == -1, "zero-length buffer");
    flex_parse_note(NULL, FLEX_PARSE_UNKNOWN, "X", "CE", NULL, 0, 7);
    ok(1, "NULL stats does not crash");
}

static void test_counter_no_wrap_at_int_max(void)
{
    struct FLEX_PARSE_STATS st;
    memset(&st, 0, sizeof(st));
    st.unknown = 2147483647UL;
    flex_parse_note(&st, FLEX_PARSE_UNKNOWN, "X", "CE", NULL, 0, 1);
    ok(st.unknown == 2147483648UL, "unsigned long past INT_MAX");
}

int main(void)
{
    test_empty_footer();
    test_counts_and_last();
    test_edges();
    test_counter_no_wrap_at_int_max();
    printf("test_parse_stats: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
