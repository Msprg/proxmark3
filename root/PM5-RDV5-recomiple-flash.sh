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

# The FPGA and BWM images are not part of the firmware flash. After it, compare what
# the device runs with the images bundled in client/resources:
#  - FPGA: written to the FPGA flash automatically when the usercode differs
#  - BWM:  only reported at the end, with the command to run by hand
FPGA_IMG=client/resources/fpga_pm5.bin
BWM_IMG=client/resources/proxmark5_ble_wifi_module.bin
RESET_WAIT=15   # seconds to leave the PM5 alone after the flash reset
NOTES=()
TMO=$(command -v timeout || command -v gtimeout || true)

# run_pm3 <timeout s, 0 = none> <client commands>: output without colour codes, never fails
# --foreground: the client polls the keyboard, in a background group the terminal would stop it
run_pm3() {
    local pre=()
    if [ -n "$TMO" ] && [ "$1" -gt 0 ]; then
        pre=("$TMO" --foreground "$1")
    fi
    ${pre[@]+"${pre[@]}"} env -u DISPLAY QT_QPA_PLATFORM=offscreen ./pm3 -c "$2" 2>&1 \
        | sed $'s/\033\\[[0-9;]*m//g' || true
}

# first 0x........ value on stdin, hex digits in upper case; empty when there is none
first_hex32() {
    grep -Eio '0x[0-9a-f]{8}' | head -n1 | tr 'a-f' 'A-F' || true
}

# first FPGA usercode in client output; empty when absent or "unknown"
parse_usercode() {
    grep -Eio 'usercode[. ]+0x[0-9a-f]{8}' | head -n1 | first_hex32 || true
}

# BWM version from the `hw status` line "Firmware.... <ver> [on <chip>], link <baud>"
parse_bwm_version() {
    sed -En 's/.*Firmware\.+ +(.*), link .*/\1/p' | head -n1 | sed -E 's/ on .*//' || true
}

# version[32] of the ESP app descriptor, at 0x30 of the image
bwm_image_version() {
    dd if="$1" bs=1 skip=48 count=32 2>/dev/null | tr -d '\0' || true
}

# same rule as bwm_ver_tag() in client/src/cmdbwm.c: equal, or image is <describe>-g<running hash>
bwm_versions_match() {
    [ "$1" = "$2" ] || [[ "$2" == *"-g$1" ]]
}

check_device_images() {
    local left=$((RESET_WAIT - (SECONDS - FLASH_DONE)))
    if [ "$left" -gt 0 ]; then
        echo "waiting ${left}s for the PM5 to settle after the flash"
        sleep "$left"
    fi

    local out dev_uc img_uc dev_bwm img_bwm fout
    out=$(run_pm3 40 "hw version; hw status")

    # FPGA
    dev_uc=$(printf '%s\n' "$out" | parse_usercode)
    img_uc=$(python3 fpga/common/gowin_usercode.py "$FPGA_IMG" 2>/dev/null | first_hex32)
    if [ -z "$dev_uc" ] || [ -z "$img_uc" ]; then
        echo "FPGA: not checked (device usercode '${dev_uc:-unknown}', image usercode '${img_uc:-unknown}')"
    elif [ "$dev_uc" = "$img_uc" ]; then
        echo "FPGA: up to date (usercode $dev_uc)"
    else
        echo "FPGA: device runs $dev_uc, bundled image is $img_uc -> writing it to the FPGA flash"
        # no timeout: a write killed half way leaves a broken image in the FPGA flash
        fout=$(run_pm3 0 "hw fpga config -f $PWD/$FPGA_IMG; hf 14a read --drop")
        printf '%s\n' "$fout"
        if printf '%s\n' "$fout" | grep -q 'FPGA configuration successfully'; then
            NOTES+=("FPGA image written ($dev_uc -> $img_uc). Power-cycle the PM5 to run it; until then every run of this script writes it again.")
        else
            NOTES+=("FPGA update FAILED (device $dev_uc, image $img_uc). Retry from ./pm3:  hw fpga config -f fpga_pm5.bin")
        fi
    fi

    # BWM
    dev_bwm=$(printf '%s\n' "$out" | parse_bwm_version)
    img_bwm=$(bwm_image_version "$BWM_IMG")
    if [ -z "$dev_bwm" ] || [ "$dev_bwm" = "unknown" ] || [ -z "$img_bwm" ]; then
        echo "BWM: not checked (module version '${dev_bwm:-not reported}', image version '${img_bwm:-unknown}')"
    elif bwm_versions_match "$dev_bwm" "$img_bwm"; then
        echo "BWM: up to date ($dev_bwm)"
    else
        echo "BWM: module runs $dev_bwm, bundled image is $img_bwm"
        NOTES+=("BWM firmware differs: module runs $dev_bwm, bundled image is $img_bwm. To upgrade, run in an interactive ./pm3 session:  hw bwm upgrade -f proxmark5_ble_wifi_module.bin")
    fi
}

step "firmware + host tools"
make -j $(printf '%s/all ' $FW_TOOLS) PLATFORM_FILE=$PLTFRM

step "client (cmake + ninja)"
if [ ! -f "$CLIENT_BUILD/build.ninja" ]; then
    cmake -G Ninja -DPLATFORM=$PLATFORM \
          -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
          -S client -B "$CLIENT_BUILD"
fi
# regenerate version_pm3.c from current git (CMake only tracks default_version_pm3.c,
# so ninja alone keeps a stale hash and the flash aborts on version mismatch)
rm -f "$CLIENT_BUILD/version_pm3.c"
ninja -C "$CLIENT_BUILD"
# pm3-flash-all and `make install` use client/proxmark3
cp -p "$CLIENT_BUILD/proxmark3" client/proxmark3

step "flash"
./pm3-flash-all
FLASH_DONE=$SECONDS

step "install"
make $(printf '%s/install ' $FW_TOOLS) common/install PLATFORM_FILE=$PLTFRM
# client install recipe only: -o all = do not rebuild the Makefile client, ninja just built it
make -C client install -o all PLATFORM_FILE=../$PLTFRM

step "FPGA + BWM images"
check_device_images

step "done"
# actionable leftovers last, so they are not lost in the build output
for n in ${NOTES[@]+"${NOTES[@]}"}; do
    printf '[!] %s\n' "$n"
done
