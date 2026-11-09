#!/usr/bin/env bash
# Build (and optionally flash) base3, the 915 MHz gateway.
#
#   ./gateway/build.sh                 # compile to /tmp/gw_base3
#   ./gateway/build.sh --flash         # compile and upload to the C3 on the bench
#   ./gateway/build.sh --no433         # no RA-02 fitted: node 0 rows are not published
  ./gateway/build.sh --bat=3 --chrg=8  # divider on GPIO3/A3, TP5000 CHRG on GPIO8
#   ./gateway/build.sh --ip=233        # different static address
#
# Credentials are read from the files that already hold them and passed as build
# flags. They are never typed into a prompt, a README, or a commit:
#
#   homebase/esphome/cat-base2.yaml   TP-Link_9E19 (first) and HUAWEI (second)
#   gateway/secrets.local            Stefan_Office, cached from the ESPHome
#                                    dashboard's secrets.yaml on m710q
#
# Each build goes to its own --output-dir, because `arduino-cli upload` sends the
# artifacts of the LAST build of that sketch: a stray compile can put the wrong base
# id, node id, or role on a board with no error at all.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
YAML="$REPO/homebase/esphome/cat-base2.yaml"
SECRETS="$REPO/gateway/secrets.local"
OUT=/tmp/gw_base3
FQBN="esp32:esp32:esp32c3:CDCOnBoot=cdc"
BASE_ID=base3
STATIC_IP_LAST=232
CHECK=0
HAS_433=1
# Battery: -1 means nothing fitted, and the base then publishes JSON null, which Home
# Assistant renders as unknown. That is the honest state, and it is not the same as 0.
BAT_ADC=-1
BAT_DIV=2
CHRG_PIN=-1
PORT=""
FLASH=0

for a in "$@"; do
  case "$a" in
    --flash) FLASH=1 ;;
    --no433) HAS_433=0 ;;
    --bat=*) BAT_ADC="${a#--bat=}" ;;
    --chrg=*) CHRG_PIN="${a#--chrg=}" ;;
    --port=*) PORT="${a#--port=}" ;;
    --ip=*) STATIC_IP_LAST="${a#--ip=}" ;;
    --check) CHECK=1 ;;
    *) echo "unknown option: $a" >&2; exit 2 ;;
  esac
done

[ -f "$YAML" ] || { echo "$YAML not found - it is the house's copy of the passphrases" >&2; exit 1; }

# Pair them by file order: the networks: block is ssid then password, and the
# fallback AP entry is a third pair that is not a network.
# Pair ssid/password in file order. The AP fallback entry is a third pair, and it is
# not a network, so the first two pairs are taken and the rest ignored.
eval "$(awk -F'"' '
  /ssid:/     { s=$2 }
  /password:/ { n++;
                if (s != "" && n <= 2) printf "S%d=%s\nP%d=%s\n", n, s, n, $2;
                s="" }
' "$YAML")"

if [ -z "${S1:-}" ] || [ -z "${S2:-}" ]; then
  echo "could not read both networks from $YAML" >&2
  exit 1
fi

# Stefan_Office is the third network; it lives in the ESPHome dashboard's
# secrets.yaml on m710q. Cache it locally so a build with no SSH works.
if [ ! -f "$SECRETS" ]; then
  ssh stefan@m710q 'cat /home/stefan/homeassistant/esphome-config/secrets.yaml' \
    > "$SECRETS" 2>/dev/null || echo "warning: could not fetch secrets.yaml, Stefan_Office skipped"
fi
S3=$(sed -n 's/^wifi_ssid_office: \([^ ]*\).*/\1/p' "$SECRETS" 2>/dev/null | head -1)
P3=$(sed -n 's/^wifi_password_stefan_office: \([^ ]*\).*/\1/p' "$SECRETS" 2>/dev/null | head -1)

FLAGS="-DBASE_ID=$BASE_ID -DSTATIC_IP_LAST=$STATIC_IP_LAST -DHAS_433=$HAS_433"
FLAGS="$FLAGS -DBAT_ADC=$BAT_ADC -DBAT_DIV=$BAT_DIV -DCHRG_PIN=$CHRG_PIN"
FLAGS="$FLAGS -DWIFI_SSID=\"$S1\" -DWIFI_PASS=\"$P1\""
FLAGS="$FLAGS -DWIFI_SSID2=\"$S2\" -DWIFI_PASS2=\"$P2\""
FLAGS="$FLAGS -I$REPO/poc915 -I$REPO/homebase/esphome"   # rak_event.h, collar_frame.h
[ -n "$S3" ] && FLAGS="$FLAGS -DWIFI_SSID3=\"$S3\" -DWIFI_PASS3=\"$P3\""

# --check builds the minimal WiFi probe in gateway/wifi_check with the same
# credentials, for "can this board associate at all", not "is the gateway right".
if [ "$CHECK" = 1 ]; then
  OUT=/tmp/wifi_check
  SKETCH="$REPO/gateway/wifi_check"
else
  SKETCH="$REPO/gateway"
fi

echo "== building $BASE_ID (HAS_433=$HAS_433, static 192.168.100.$STATIC_IP_LAST, BAT_ADC=$BAT_ADC CHRG_PIN=$CHRG_PIN) sketch=$SKETCH"
echo "   networks: $S1, $S2, ${S3:-<none>}"

# Run the JSON test first (the gateway build; the probe has no JSON). A truncated discovery record publishes, Home Assistant
# logs "Unable to parse JSON", and the dashboard stays empty with no error on the
# device - so the test is part of the build, not an optional extra.
if [ "$CHECK" != 1 ]; then
  c++ -std=c++17 -Wall -Wextra -o /tmp/gwtest "$REPO/gateway/test_state_json.cpp"
  /tmp/gwtest > /dev/null
  echo "   state_json tests pass"
fi

arduino-cli compile --fqbn "$FQBN" \
  --build-property "compiler.cpp.extra_flags=$FLAGS" \
  --output-dir "$OUT" "$SKETCH"

if [ "$FLASH" = 1 ]; then
  # Identify the port by USB vendor id 0x303a, not by the usbmodem suffix, which is
  # shuffled between reboots. pyserial's serial_number is the board MAC, and reading
  # it does not open the port, so it does not reset the board.
  if [ -z "$PORT" ]; then
    PORT=$(python3 -c "
import serial.tools.list_ports as lp
for p in lp.comports():
    if p.vid == 0x303a:
        print(p.device); break
")
    [ -z "$PORT" ] && { echo "no ESP32-C3 (VID 0x303a) plugged in" >&2; exit 1; }
  fi
  echo "== uploading to $PORT"
  arduino-cli upload --input-dir "$OUT" --fqbn "$FQBN" --port "$PORT"
fi
