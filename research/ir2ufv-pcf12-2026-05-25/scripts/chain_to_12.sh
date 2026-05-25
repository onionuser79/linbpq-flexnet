#!/bin/bash
# Chain from iw2ohx-gw → telnet IW2OHX-14 → C IW2OHX-12 → L * → BYE
# Output: raw transcript on stdout. Run on iw2ohx-gw.
set -uo pipefail

{
    printf 'iw7eas-1\r\n'
    sleep 1
    printf 'sherwood\r\n'
    sleep 2
    printf 'C IW2OHX-12\r\n'
    sleep 6
    printf 'L *\r\n'
    sleep 4
    printf 'BYE\r\n'
    sleep 2
    printf 'BYE\r\n'
    sleep 1
} | timeout 25 telnet 44.134.24.2 23 2>&1
