#!/usr/bin/env bash
#
# Generate the release signing key used by tools/build_release.sh.
#
# Run once. The keystore and keystore.properties are both excluded from git, so
# neither is ever committed - which also means there is no way to recover them
# if you lose them. Back them up somewhere safe before you need them.
#
# Losing the key is not fatal: you can regenerate and install the resulting APK,
# but Android will only accept it after uninstalling the old one, losing app
# data. Keeping one key avoids that entirely.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AND="$ROOT/CatTrackApp/android"
KS="$AND/cattracker-release.keystore"
PROPS="$AND/keystore.properties"

if [[ -f "$KS" || -f "$PROPS" ]]; then
  echo "error: $KS or $PROPS already exists - refusing to overwrite." >&2
  echo "       An existing key must not be replaced, or every already-installed" >&2
  echo "       build becomes uninstallable rather than upgradable." >&2
  exit 1
fi

if ! command -v keytool >/dev/null; then
  echo "error: keytool not found - install a JDK (brew install openjdk)" >&2
  exit 1
fi

PW="$(python3 -c "import secrets,string;a=string.ascii_letters+string.digits;print(''.join(secrets.choice(a) for _ in range(24)))")"

keytool -genkeypair -v -storetype PKCS12 \
  -keystore "$KS" \
  -alias cattracker \
  -keyalg RSA -keysize 2048 -validity 10000 \
  -storepass "$PW" -keypass "$PW" \
  -dname "CN=Cat Tracker, OU=Personal Project, O=Cat Tracker, C=DK"

printf 'CATTRACKER_STORE_FILE=cattracker-release.keystore\nCATTRACKER_KEY_ALIAS=cattracker\nCATTRACKER_STORE_PASSWORD=%s\nCATTRACKER_KEY_PASSWORD=%s\n' \
  "$PW" "$PW" > "$PROPS"
chmod 600 "$PROPS"

echo
echo "created:"
echo "  $KS"
echo "  $PROPS   (holds the password)"
echo
echo "Back both up now. They are gitignored, so git will not save them for you."