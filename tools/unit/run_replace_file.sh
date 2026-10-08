#!/bin/bash
# flex_replace_file: extract + build (-Werror, ASan/UBSan) + run on this host.
# With WIN_HOST=<ssh alias> it also cross-compiles the Windows branch with
# mingw-w64 and runs it there in a scratch directory that is removed after.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
U=tools/unit

bash $U/extract.sh FlexNetCode.c flex_replace_file > $U/extracted_replace.inc

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
gcc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Werror -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I $U -o "$work/test_replace_file" $U/test_replace_file.c
(cd "$work" && ./test_replace_file)

if [ -n "${WIN_HOST:-}" ]; then
    i686-w64-mingw32-gcc -std=c11 -DWIN32 -Wall -Wextra -Wpedantic -Wshadow -Werror \
        -I $U -o "$work/test_replace_file.exe" $U/test_replace_file.c
    winps() { ssh "$WIN_HOST" "powershell -NoProfile -EncodedCommand \
        $(printf '%s' "\$ProgressPreference='SilentlyContinue'; $1" | iconv -t UTF-16LE | base64 | tr -d '\n')"; }
    winps "New-Item -ItemType Directory -Force C:\\Temp\\flexnet-unit | Out-Null" >/dev/null
    scp -q "$work/test_replace_file.exe" "$WIN_HOST:C:/Temp/flexnet-unit/"
    winps 'Set-Location C:\Temp\flexnet-unit; .\test_replace_file.exe; $rc = $LASTEXITCODE;
           Set-Location C:\; Remove-Item -Recurse -Force C:\Temp\flexnet-unit; exit $rc'
fi
