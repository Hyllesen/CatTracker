#!/usr/bin/env bash
#
# Build and flash the handheld (ESP32-C3 + RA-02).
#
# Builds the BLE sketch, handheld_ble/, because that is the one the CatTrackApp
# talks to. There is no second handheld sketch: the serial-only one was deleted
# (see multi-collar.md).
#
# Usage:
#   tools/build_handheld.sh                    build + flash the BLE firmware
#   tools/build_handheld.sh --build-only       compile only, do not flash
#   tools/build_handheld.sh --port <device>    pick the port yourself
#
# ─────────────────────────────────────────────────────────────────────────────
# WHY --board-options CDCOnBoot=cdc IS NOT OPTIONAL
#
# The ESP32-C3 board definition defaults to:
#
#     esp32c3.menu.CDCOnBoot.default=Disabled
#     esp32c3.menu.CDCOnBoot.default.build.cdc_on_boot=0
#
# With cdc_on_boot=0, `Serial` is bound to UART0 on GPIO20/GPIO21, NOT to the
# native USB. The USB port still enumerates, so nothing looks wrong - but no
# byte is ever exchanged, and the firmware's whole diagnostic interface ('v'
# for verbose logging, banner, packet log) is dead. The failure is completely
# silent, which is how it survives: BLE keeps working either way, because the
# Bluetooth controller is separate hardware from the CDC peripheral.
#
# Always pass CDCOnBoot=cdc. If the serial monitor goes quiet after a flash,
# check this before suspecting the sketch.
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

FQBN="esp32:esp32:esp32c3"
BOARD_OPTIONS="CDCOnBoot=cdc"

# Vendor ID of the Espressif USB JTAG/serial debug unit. Used to tell the
# handheld apart from the nRF52840 collar, which is also plugged in.
ESP32_VID="0x303a"

SKETCH_DIR="handheld_ble"
PORT=""
BUILD_ONLY=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-only) BUILD_ONLY=1; shift ;;
    --port) PORT="${2:-}"; shift 2 ;;
    -h|--help) sed -n '2,12p' "${BASH_SOURCE[0]}"; exit 0 ;;
    -*) echo "unknown option: $1" >&2; exit 1 ;;
    *) echo "unexpected argument: $1 (only --build-only and --port are accepted)" >&2; exit 1 ;;
  esac
done

# ---- locate arduino-cli ----
CLI="${ARDUINO_CLI:-}"
if [[ -z "$CLI" ]]; then
  if command -v arduino-cli >/dev/null 2>&1; then
    CLI="$(command -v arduino-cli)"
  fi
fi
if [[ -z "$CLI" ]]; then
  cat >&2 <<EOF
error: arduino-cli not found.

Install it with either:
    brew install arduino-cli
or download the binary and point ARDUINO_CLI at it:
    curl -fsSL -o /tmp/acli.tar.gz \\
      https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_macOS_64bit.tar.gz
    tar xzf /tmp/acli.tar.gz -C /tmp arduino-cli
    export ARDUINO_CLI=/tmp/arduino-cli

The esp32 core is required too; install it from the Boards Manager if the first
compile reports the platform is unknown.
EOF
  exit 1
fi

SKETCH="$ROOT/$SKETCH_DIR"
[[ -d "$SKETCH" ]] || { echo "error: no such sketch directory: $SKETCH" >&2; exit 1; }

echo "==> building $SKETCH_DIR for $FQBN ($BOARD_OPTIONS)"
"$CLI" compile --fqbn "$FQBN" --board-options "$BOARD_OPTIONS" "$SKETCH"

if [[ "$BUILD_ONLY" == "1" ]]; then
  echo "==> build only, not flashing"
  exit 0
fi

# ---- find the port ----
if [[ -z "$PORT" ]]; then
  PORT="$(python3 - <<PY
from serial.tools import list_ports
for p in list_ports.comports():
    if p.vid == int(${ESP32_VID}):
        print(p.device)
        break
PY
)"
fi

if [[ -z "$PORT" ]]; then
  cat >&2 <<'EOF'
error: no ESP32-C3 found.

Plug the handheld in over USB, or name the port explicitly:
    tools/build_handheld.sh --port /dev/cu.usbmodem11001

If several boards are attached, note the collar is an Adafruit nRF52840
(0x239a) and the handheld is an Espressif USB JTAG/serial unit (0x303a).
EOF
  exit 1
fi

echo "==> flashing $PORT"
"$CLI" upload -p "$PORT" --fqbn "$FQBN" --board-options "$BOARD_OPTIONS" "$SKETCH"

echo
echo "Done. Open the serial monitor at 115200 and press 'v' for diagnostics."
echo "Expected banner:"
echo "    Cat Tracker - Handheld + BLE"
echo "    [radio] online  433.0 MHz  SF9  sync=0x12"
echo
echo "If nothing comes back, the 'Serial' above is bound to UART0, not USB -"
echo "check that CDCOnBoot=cdc was applied."