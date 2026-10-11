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
if [[ "${1:-}" == "--install" ]]; then
  INSTALL=1
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
  DEV="$(adb devices | awk 'NR>1 && $2=="device" {print $1; exit}')"
  if [[ -z "$DEV" ]]; then
    echo "error: no adb device connected" >&2
    exit 1
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