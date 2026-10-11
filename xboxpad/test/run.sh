#!/bin/sh
# Report format and pad mapping tests on the PC: test/run.sh
set -e
cd "$(dirname "$0")"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc -std=c99 -Wall -Wextra -Werror -I../src -I../../common -o "$out/xidpad_test" xidpad_test.c ../src/xidpad.c
"$out/xidpad_test"
