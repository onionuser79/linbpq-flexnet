/* flex_replace_file(): the path-cache save's final step.
 *
 * Pins the defect fixed for the Windows build: rename() there refuses an
 * existing target, so the cache was written once and never refreshed. The
 * second case below is the one that failed. Built for POSIX and, by
 * run_replace_file.sh, cross-compiled for Windows (the MoveFileExA branch).
 *
 * Extracted verbatim from FlexNetCode.c by tools/unit/extract.sh.
 */
#ifdef WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "extracted_replace.inc"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
    else printf("ok:   %s\n", msg); } while (0)

static int write_file(const char * path, const char * text)
{
    FILE * fp = fopen(path, "w");
    if (!fp)
        return -1;
    if (fputs(text, fp) < 0) {
        fclose(fp);
        return -1;
    }
    return fclose(fp) == 0 ? 0 : -1;
}

static int file_is(const char * path, const char * text)
{
    char buf[64] = {0};
    FILE * fp = fopen(path, "r");
    if (!fp)
        return 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    return n == strlen(text) && memcmp(buf, text, n) == 0;
}

static int exists(const char * path)
{
    FILE * fp = fopen(path, "r");
    if (!fp)
        return 0;
    fclose(fp);
    return 1;
}

int main(void)
{
    const char * tmp = "test_replace_file.dat.tmp";
    const char * dst = "test_replace_file.dat";
    remove(tmp);
    remove(dst);

    CHECK(write_file(tmp, "first\n") == 0, "setup: write first tmp");
    CHECK(flex_replace_file(tmp, dst) == 0, "no target: move succeeds");
    CHECK(file_is(dst, "first\n"), "no target: target has new content");
    CHECK(!exists(tmp), "no target: source is gone");

    CHECK(write_file(tmp, "second\n") == 0, "setup: write second tmp");
    CHECK(flex_replace_file(tmp, dst) == 0, "existing target: replace succeeds");
    CHECK(file_is(dst, "second\n"), "existing target: target has new content");
    CHECK(!exists(tmp), "existing target: source is gone");

    int rc = flex_replace_file(tmp, dst);
    CHECK(rc < 0, "missing source: negative error code");
    CHECK(file_is(dst, "second\n"), "missing source: target untouched");

    remove(dst);
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
