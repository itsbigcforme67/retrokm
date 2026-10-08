#!/bin/sh
# Builds rkm-win32.exe on Linux with Open Watcom v2, whose output runs on
# Windows 98 SE and 2000 (current MinGW output does not start on 98).
#   WATCOM=~/.local/watcom agents/win32/build-watcom.sh
# Open Watcom: https://github.com/open-watcom/open-watcom-v2/releases
# (ow-snapshot.tar.xz, unpacked into $WATCOM).
set -e
: "${WATCOM:=$HOME/.local/watcom}"
export WATCOM PATH="$WATCOM/binl64:$WATCOM/binl:$PATH" INCLUDE="$WATCOM/h:$WATCOM/h/nt"
cd "$(dirname "$0")/../.."
mkdir -p build
wcl386 -q -w4 -l=nt_win -bt=nt -i=common -fo=build/ -fe=build/rkm-win32.exe \
    agents/win32/rkm_win32.c common/rkm_proto.c user32.lib shell32.lib wsock32.lib
echo "built build/rkm-win32.exe"
