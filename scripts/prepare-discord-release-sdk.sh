#!/usr/bin/env bash
set -euo pipefail

# Release-only Discord Social SDK hydration. The proprietary SDK archive is
# supplied through an authenticated/short-lived URL and never committed or
# uploaded as a standalone artifact by PulseForge.
url="${PULSEFORGE_DISCORD_SDK_ARCHIVE_URL:-}"
if [[ -z "$url" ]]; then
  echo 'PULSEFORGE_DISCORD_SDK_ARCHIVE_URL is required for Discord-enabled release builds.' >&2
  exit 1
fi

project_root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
work_root="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/pulseforge-discord-sdk"
rm -rf -- "$work_root"
mkdir -p -- "$work_root"
raw="$work_root/sdk.download"

# Never print the private URL. curl only writes provider diagnostics on failure.
curl --fail --location --silent --show-error --retry 4 --retry-delay 2 \
  "$url" --output "$raw"
[[ -s "$raw" ]] || { echo 'Downloaded Discord SDK archive is empty.' >&2; exit 1; }

archive="$work_root/discord-social-sdk.zip"
if python3 - "$raw" <<'PY'
import sys, zipfile
raise SystemExit(0 if zipfile.is_zipfile(sys.argv[1]) else 1)
PY
then
  mv -- "$raw" "$archive"
elif python3 - "$raw" <<'PY'
import sys, tarfile
try:
    ok = tarfile.is_tarfile(sys.argv[1])
except OSError:
    ok = False
raise SystemExit(0 if ok else 1)
PY
then
  archive="$work_root/discord-social-sdk.tar"
  mv -- "$raw" "$archive"
else
  echo 'Downloaded Discord SDK input is neither a ZIP nor a TAR archive.' >&2
  exit 1
fi

python3 "$project_root/scripts/inspect-discord-social-sdk.py" \
  --sdk "$archive" --require all --deep

"$project_root/scripts/setup-discord-social-sdk.sh" \
  --sdk "$archive" \
  --destination "$project_root/third_party/discord_social_sdk" \
  --force

sdk="$project_root/third_party/discord_social_sdk"
test -f "$sdk/include/discordpp.h"
test -f "$sdk/include/cdiscord.h"
test -f "$sdk/lib/release/discord_partner_sdk.lib"
test -f "$sdk/bin/release/discord_partner_sdk.dll"
test -f "$sdk/lib/release/libdiscord_partner_sdk.so"
test -f "$sdk/android/discord_partner_sdk.aar"
find "$sdk" -type d -name 'discord_partner_sdk.framework' -print -quit | grep -q .

echo 'Discord Social SDK release input staged and validated for Windows, Linux, macOS and Android.'
