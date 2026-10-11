#!/usr/bin/env bash
#
# Build a standalone (release) APK of the tracker app.
#
# The release build embeds the JS bundle, so the app runs with no Mac, no USB
# cable and no Metro - which is what you want for an actual range test in the
# field. Only the debug build depends on Metro.
#
# Usage:
#   tools/build_release.sh            build
#   tools/build_release.sh --install build, then install on the connected phone
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="$ROOT/CatTrackApp"
OUT="$APP/android/app/build/outputs/apk/release/app-release.apk"
DEST="$ROOT/catracker-release.apk"

INSTALL=0
TARGET=""
if [[ "${1:-}" == "--install" ]]; then
  INSTALL=1
  # Optional serial, for when more than one phone is plugged in.
  TARGET="${2:-}"
fi

if [[ ! -f "$APP/android/keystore.properties" ]]; then
  cat >&2 <<'EOF'
error: android/keystore.properties is missing, so this would be signed with the
       debug key. That build installs, but Android will refuse to upgrade it
       once you switch to a real key.

       Regenerate with:
         tools/make_release_keystore.sh
EOF
  exit 1
fi

echo "==> building release APK"
(cd "$APP/android" && ./gradlew assembleRelease)

[[ -f "$OUT" ]] || { echo "error: expected APK at $OUT" >&2; exit 1; }

cp "$OUT" "$DEST"
echo
echo "==> APK: $DEST"
ls -lh "$DEST" | awk '{print "    size: " $5}'

if [[ "$INSTALL" == "1" ]]; then
  # Skipped rather than used: emulators have a virtual Bluetooth radio and
  # cannot see the handheld, so an install that lands on one looks broken for
  # reasons unrelated to the build. Bash 3.2 compatible - no mapfile.
  CANDIDATES=()
  while IFS= read -r line; do
    [[ -n "$line" ]] && CANDIDATES+=("$line")
  done < <(adb devices | awk 'NR>1 && $2=="device" {print $1}' | grep -v '^emulator-')

  case "${#CANDIDATES[@]}" in
    0)
      echo "error: no physical device connected (emulators are skipped)." >&2
      echo "       Plug in the phone and enable USB debugging." >&2
      exit 1
      ;;
    1)
      DEV="${CANDIDATES[0]}"
      ;;
    *)
      echo "error: several physical devices connected:" >&2
      printf '         %s\n' "${CANDIDATES[@]}" >&2
      echo "       Re-run with: tools/build_release.sh --install <serial>" >&2
      exit 1
      ;;
  esac

  # An explicit serial wins over auto-detection, but still has to be real.
  if [[ -n "$TARGET" ]]; then
    if ! printf '%s\n' "${CANDIDATES[@]}" | grep -qx "$TARGET"; then
      echo "error: '$TARGET' is not a connected physical device." >&2
      exit 1
    fi
    DEV="$TARGET"
  fi

  echo "==> installing on $DEV"
  adb -s "$DEV" install -r "$DEST"
  # Release builds carry the JS bundle, so no Metro tunnel is needed. Removing
  # any stale one keeps that honest.
  adb -s "$DEV" reverse --remove tcp:8081 2>/dev/null || true
fi

echo
echo "Remember: the keystore and keystore.properties are what make this APK"
echo "upgradable. Back both up somewhere outside this repo - losing them means"
echo "uninstalling before you can install a newer build."