#!/usr/bin/env bash
#
# Cross-compile LinBPQ.exe (Windows, 32-bit console) with FlexNet, using
# mingw-w64. Upstream builds the same program with MSVC from MailNode.vcxproj;
# this follows that project's source list and defines.
#
# usage: win/build-win.sh [UPSTREAM_REF]
#   UPSTREAM_REF  LinBPQ revision to build on (default: upstream/master).
#                 Must match the version this overlay is based on.
#
# Environment:
#   EXTRA_CFLAGS  appended to the compiler flags, as with the makefile:
#                 -DFLEXNET_PROD=1 (silent) or -DFLEXNET_DEBUG=1 (debug)
#   BUILD_DIR     work and output directory (default: win/build)
#   JOBS          parallel compiles (default: CPU count)
#
# Every run starts from a fresh export, so a flavour change needs no clean.
# Output: $BUILD_DIR/LinBPQ.exe
set -euo pipefail

WIN="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(dirname "$WIN")"
REF="${1:-upstream/master}"
BUILD="${BUILD_DIR:-$WIN/build}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN)}"
CC=i686-w64-mingw32-gcc
STRIP=i686-w64-mingw32-strip

command -v "$CC" >/dev/null || {
    echo "build-win: $CC not found (macOS: brew install mingw-w64;" \
         "Debian: apt install gcc-mingw-w64-i686)" >&2
    exit 1
}

OVERLAY=(FlexNetCode.c flexnet_l3.c flexnet_l3.h Cmd.c L2Code.c asmstrucs.h bpqaxip.c)

echo ">> dependencies"
"$WIN/build-deps.sh" "$BUILD/deps"

echo ">> export $REF + overlay"
SRC="$BUILD/src"
rm -rf "$SRC" && mkdir -p "$SRC/obj"
git -C "$REPO" archive "$REF" | tar -x -C "$SRC"
for f in "${OVERLAY[@]}"; do cp "$REPO/$f" "$SRC/"; done
(cd "$SRC" && "$WIN/fixups.sh")

# The source list is MailNode.vcxproj's, so it follows upstream; the FlexNet
# files are ours.
SOURCES=$(grep -o 'ClCompile Include="[^"]*\.c"' "$SRC/MailNode.vcxproj" \
          | sed 's/.*Include="//; s/"$//')
SOURCES="$SOURCES FlexNetCode.c flexnet_l3.c"

# MailNode.vcxproj's defines. -O0 -g matches the Linux makefile, which is
# what the FlexNet code is run and tested with. GCC 14+ makes C23 and
# pointer-type mismatches errors; the upstream code predates both and MSVC
# accepts it, hence gnu17 and -fpermissive. -w: upstream code is not ours to
# warn about; check FlexNet's own warnings with the Linux build.
CFLAGS="-std=gnu17 -fpermissive -w -O0 -g -fcommon -include $WIN/winshim.h \
 -DWIN32 -DNDEBUG -D_CONSOLE -DLINBPQ -D_USE_32BIT_TIME_T -DNOMQTT \
 -I. -IWin32bits ${EXTRA_CFLAGS:-}"

echo ">> compile ($(echo "$SOURCES" | wc -w | tr -d ' ') files, $JOBS jobs)"
cd "$SRC"
export CC CFLAGS
# shellcheck disable=SC2016  # expanded by the inner shell
echo "$SOURCES" | tr ' ' '\n' | grep . | xargs -P "$JOBS" -I{} sh -c \
    '$CC -c $CFLAGS "$1" -o "obj/${1%.c}.o" 2>"obj/${1%.c}.err" \
     || { echo "FAILED: $1"; cat "obj/${1%.c}.err"; exit 255; }' _ {}

echo ">> link"
DEPS="$BUILD/deps/lib"
"$CC" -static -o LinBPQ.exe obj/*.o \
    "$DEPS/libconfig.a" "$DEPS/libminiupnpc.a" "$DEPS/libz.a" \
    -lws2_32 -liphlpapi -ldbghelp -lsetupapi -lpsapi -lwinmm -lcomctl32 \
    -lcomdlg32 -lgdi32 -lshlwapi -lhid
"$STRIP" -o "$BUILD/LinBPQ.exe" LinBPQ.exe

EXE="$BUILD/LinBPQ.exe"
echo ">> $EXE ($(wc -c < "$EXE" | tr -d ' ') bytes)"
echo "   LinBPQ  $(strings "$EXE" | grep -m1 -E '^6\.0\.[0-9]+\.[0-9]+$')"
echo "   FlexNet $(strings "$EXE" | grep -m1 -E '^v[0-9]+\.[0-9]+\.[0-9]+$')"
echo "   'FlexNet: ' strings: $(strings "$EXE" | grep -c 'FlexNet: ') (silent build: <= 5)"
