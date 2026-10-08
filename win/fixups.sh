#!/usr/bin/env bash
#
# Source fixes for C that MSVC accepts and GCC rejects. Run inside an
# exported upstream tree (never in this repository): upstream and the overlay
# stay untouched.
#
# Every fix must match exactly once. After an upstream rebase a pattern that
# no longer matches stops the build here instead of resurfacing as a
# compiler error far from its cause. Upstream files are CRLF, so no pattern
# anchors on end of line.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# fix FILE PERL_SUBSTITUTION — applied to the whole file, must change it
# exactly once (counted with the substitution's return value).
fix() {
    local file="$1" expr="$2" n
    n=$(perl -0777 -i -pe "\$n = ($expr); END { print STDERR \$n + 0 }" "$file" 2>&1 >/dev/null) \
        || { echo "fixups: perl failed on $file" >&2; exit 1; }
    if [ "$n" != "1" ]; then
        echo "fixups: $file: expected 1 match, got $n for: $expr" >&2
        exit 1
    fi
}

# "static declaration follows non-static declaration": the forward
# declaration lacks the static its definition has. Make the declaration
# static — dropping static from the definition instead exports a second
# ReleaseTNC / GetAddress and breaks the link.
fix V4.c        's/^(VOID ReleaseTNC\(struct TNCINFO \* TNC\);)/static $1/mg'
fix bpqether.c  's/^(FARPROCX GetAddress\(char \* Proc\);)/static $1/mg'
fix bpqvkiss.c  's/^(int\s+kissencode\(UCHAR \* inbuff, UCHAR \* outbuff, int len\);)/static $1/mg'
fix bpqvkiss.c  's/^(int GetRXMessage\(int port, PMESSAGE buff\);)/static $1/mg'
fix bpqvkiss.c  's/^(void CheckReceivedData\(PVCOMINFO\s+pVCOMInfo\);)/static $1/mg'
fix bpqvkiss.c  's/^(PVCOMINFO CreateInfo\(\s*int port,int speed, int bpqport\s*\)\s*;)/static $1/mg'

# bpqmail.h declares `extern char *month[]`, defined in BBSHTMLConfig.c;
# ChatHTMLConfig.c then defines a static one with the same contents.
fix ChatHTMLConfig.c 's/^static char \*month\[\] = \{"Jan",[^\n]*\n//mg'

# cheaders.h declares OpenCOMPort(VOID * pPort, ...); the WIN32 definition
# takes char *.
fix CommonCode.c 's/^HANDLE OpenCOMPort\(char \* pPort,/HANDLE OpenCOMPort(VOID * pPort,/mg'

# The crash handler dumps the stack with MSVC __asm. Sources include it as
# "StdExcept.c": on a case-insensitive file system that finds the root
# stdexcept.c, on a case-sensitive one Win32bits/StdExcept.c. Replace both.
cp "$HERE/stdexcept.c" stdexcept.c
cp "$HERE/stdexcept.c" Win32bits/StdExcept.c
