/* v2.11.3 LOCALS and direct APPLICATION connects.
 *
 * Pins:
 *   - flex_local_reach(): an APPLICATION on NODECALL's base inside the own
 *     SSID range reads "range" (the bug: FL + listed no such call), the node
 *     SSID itself and SSIDs outside the range do not, and the
 *     FlexNetLocalCalls[] states map to advertised / range / "-";
 *   - FlexNet_StripStayFlag(): only a stand-alone S after the first word is
 *     removed from a connect tail, so a caller that connected straight to an
 *     APPLCALL is disconnected when the application ends (no stay).
 *
 * Extracted verbatim from FlexNetCode.c by tools/unit/extract.sh.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define FLEXNET_LOCAL_BASE_MAX 6
#define FLEX_LOCAL_BOUND       1
#define FLEX_LOCAL_UNBOUND     2
#define FLEX_LOCAL_NODECALL    3
#define FLEX_LOCAL_NOPORT      4
#define FLEX_LOCAL_COVERED     7

#include "extracted_locals.inc"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
    failures++; } } while (0)

static void test_reach(void)
{
    /* Node IW2OHX-4, FLEXNETSSIDRANGE 4-12 (the live -4 layout). */
    CHECK(strcmp(flex_local_reach("IW2OHX-8", -1, "IW2OHX", 4, 4, 12), "range") == 0);
    CHECK(strcmp(flex_local_reach("IW2OHX-5", -1, "IW2OHX", 4, 4, 12), "range") == 0);
    CHECK(strcmp(flex_local_reach("IW2OHX-12", -1, "IW2OHX", 4, 4, 12), "range") == 0);
    CHECK(strcmp(flex_local_reach("iw2ohx-7", -1, "IW2OHX", 4, 4, 12), "range") == 0);
    /* Boundaries: node SSID, just outside the range. */
    CHECK(strcmp(flex_local_reach("IW2OHX-4", -1, "IW2OHX", 4, 4, 12), "-") == 0);
    CHECK(strcmp(flex_local_reach("IW2OHX-13", -1, "IW2OHX", 4, 4, 12), "-") == 0);
    CHECK(strcmp(flex_local_reach("IW2OHX-3", -1, "IW2OHX", 4, 4, 12), "-") == 0);
    CHECK(strcmp(flex_local_reach("IW2OHX", -1, "IW2OHX", 4, 4, 12), "-") == 0);
    /* No range: only the node SSID, so nothing else on the base is reached. */
    CHECK(strcmp(flex_local_reach("IW2OHX-5", -1, "IW2OHX", 4, 4, 4), "-") == 0);
    /* Own-base entry listed by FLEXNETLOCAL (NODECALL state) but in range: range. */
    CHECK(strcmp(flex_local_reach("IW2OHX-8", FLEX_LOCAL_NODECALL, "IW2OHX", 4, 4, 12),
                 "range") == 0);
    /* Other bases: the FlexNetLocalCalls[] state decides. */
    CHECK(strcmp(flex_local_reach("IQ2LB-6", FLEX_LOCAL_BOUND, "IW2OHX", 4, 4, 12),
                 "advertised") == 0);
    CHECK(strcmp(flex_local_reach("IW2OHX-6", FLEX_LOCAL_COVERED, "IW2OHX", 13, 13, 13),
                 "range") == 0);
    CHECK(strcmp(flex_local_reach("SR4BBX", FLEX_LOCAL_UNBOUND, "IW2OHX", 4, 4, 12),
                 "-") == 0);
    CHECK(strcmp(flex_local_reach("SR4BBX", FLEX_LOCAL_NOPORT, "IW2OHX", 4, 4, 12),
                 "-") == 0);
    CHECK(strcmp(flex_local_reach("SR4BBX", -1, "IW2OHX", 4, 4, 12), "-") == 0);
    /* Invalid input. */
    CHECK(strcmp(flex_local_reach(NULL, -1, "IW2OHX", 4, 4, 12), "-") == 0);
    CHECK(strcmp(flex_local_reach("", -1, "IW2OHX", 4, 4, 12), "-") == 0);
    CHECK(strcmp(flex_local_reach("IW2OHX-16", -1, "IW2OHX", 4, 4, 12), "-") == 0);
    CHECK(strcmp(flex_local_reach("TOOLONGX-5", -1, "IW2OHX", 4, 4, 12), "-") == 0);
    CHECK(strcmp(flex_local_reach("IW2OHX-8", -1, NULL, 4, 4, 12), "-") == 0);
}

static int strip_is(const char *in, const char *want)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", in);
    FlexNet_StripStayFlag(buf);
    if (strcmp(buf, want) != 0) {
        printf("  strip(\"%s\") = \"%s\", want \"%s\"\n", in, buf, want);
        return 0;
    }
    return 1;
}

static void test_strip(void)
{
    /* The live -4 aliases: C 1 HOST n S -> cmdCopy "HOST n S". */
    CHECK(strip_is("HOST 1 S", "HOST 1 "));
    CHECK(strip_is("HOST 0 S K", "HOST 0 K"));
    CHECK(strip_is("HOST 2 K S NOCALL", "HOST 2 K NOCALL"));
    CHECK(strip_is("HOST 1 s", "HOST 1 "));
    CHECK(strip_is("HOST 1 S\r", "HOST 1 \r"));
    /* Not a stand-alone S, or the first word: untouched. */
    CHECK(strip_is("HOST 1", "HOST 1"));
    CHECK(strip_is("S", "S"));
    CHECK(strip_is("S 1", "S 1"));
    CHECK(strip_is("HOST 1 SS", "HOST 1 SS"));
    CHECK(strip_is("HOST 1 STAY", "HOST 1 STAY"));
    CHECK(strip_is("IW2OHX-8 S", "IW2OHX-8 "));
    CHECK(strip_is("", ""));
    CHECK(strip_is("   ", "   "));
    /* Space-padded command buffer: padding kept, S gone. */
    CHECK(strip_is("HOST 1 S      ", "HOST 1 "));
    FlexNet_StripStayFlag(NULL);
}

int main(void)
{
    test_reach();
    test_strip();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("test_locals: all passed\n");
    return 0;
}
