#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
flasher="$script_dir/../wch-ch56x-isp/wch-ch56x-isp"

if (( $# < 1 )); then
    printf 'Usage: %s <firmware.bin>\n' "$(basename -- "$0")" >&2
    exit 1
fi

if [[ ! -x "$flasher" ]]; then
    printf 'Flasher not found or not executable: %s\n' "$flasher" >&2
    exit 1
fi

firmware="$1"
if [[ ! -f "$firmware" ]]; then
    printf 'Firmware file not found: %s\n' "$firmware" >&2
    exit 1
fi

printf 'Flashing %s\n' "$(basename -- "$firmware")"
sudo "$flasher" -d=off
sudo "$flasher" -vr -f="$firmware"
