#!/usr/bin/env bash
# Rebuild + flash + install for the Proxmark5 (RDV5).
#
# Fast path: nothing is cleaned.
#  - firmware and host tools: make (ccache is enabled by the platform file, and the
#    root Makefile cleans bootrom/armsrc/recovery itself when the platform changes)
#  - client: CMake + Ninja + ccache in a per-platform build dir, so switching
#    between PM5 and PM3 Easy never invalidates the other platform's objects
set -euo pipefail
cd "$(dirname "$0")"

PLTFRM=Makefile.pm5.platform
PLATFORM=PM5
CLIENT_BUILD=client/build-pm5
# TARGETS of the root Makefile, minus client
FW_TOOLS="bootrom armsrc recovery mfc_card_only mfc_card_reader mfd_aes_brute mfulc_des_brute fpga_compress cryptorf"

step() { printf '\n[%s] %s (t=%ds)\n' "$(basename "$0")" "$1" "$SECONDS"; }

step "firmware + host tools"
make -j $(printf '%s/all ' $FW_TOOLS) PLATFORM_FILE=$PLTFRM

step "client (cmake + ninja)"
if [ ! -f "$CLIENT_BUILD/build.ninja" ]; then
    cmake -G Ninja -DPLATFORM=$PLATFORM \
          -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
          -S client -B "$CLIENT_BUILD"
fi
ninja -C "$CLIENT_BUILD"
# pm3-flash-all and `make install` use client/proxmark3
cp -p "$CLIENT_BUILD/proxmark3" client/proxmark3

step "flash"
./pm3-flash-all

step "install"
make $(printf '%s/install ' $FW_TOOLS) common/install PLATFORM_FILE=$PLTFRM
# client install recipe only: -o all = do not rebuild the Makefile client, ninja just built it
make -C client install -o all PLATFORM_FILE=../$PLTFRM

step "done"
