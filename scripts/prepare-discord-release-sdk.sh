#!/usr/bin/env bash
set -euo pipefail

# Release-only Discord Social SDK hydration. The proprietary SDK archive is
# supplied through an authenticated/short-lived URL and never committed or
# uploaded as a standalone artifact by PulseForge.
url="${PULSEFORGE_DISCORD_SDK_ARCHIVE_URL:-}"
require="${PULSEFORGE_DISCORD_SDK_REQUIRE:-}"
if [[ -z "$url" ]]; then
  echo 'PULSEFORGE_DISCORD_SDK_ARCHIVE_URL is required for Discord-enabled release builds.' >&2
  exit 1
fi
if [[ -z "$require" ]]; then
  case "$(uname -s)" in
    Darwin) require=macos ;;
    Linux) require=linux ;;
    *) require=all ;;
  esac
fi
case "$require" in
  windows|linux|macos|android|all) ;;
  *)
    echo 'PULSEFORGE_DISCORD_SDK_REQUIRE must be windows, linux, macos, android or all.' >&2
    exit 2
    ;;
esac

project_root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
work_root="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/pulseforge-discord-sdk"
rm -rf -- "$work_root"
mkdir -p -- "$work_root"
raw="$work_root/sdk.download"

# Never print the private URL. curl only writes provider diagnostics on failure.
curl --fail --location --silent --show-error --retry 4 --retry-delay 2 \
  "$url" --output "$raw"
[[ -s "$raw" ]] || { echo 'Downloaded Discord SDK archive is empty.' >&2; exit 1; }

# An Android-specific secret may point directly to the official AAR, or to a
# small wrapper archive containing only that AAR. Detect that shape before the
# desktop SDK installer (which correctly expects desktop headers/layout).
if python3 - "$raw" <<'PY'
from __future__ import annotations
import io
import pathlib
import sys
import tarfile
import zipfile

path = pathlib.Path(sys.argv[1])

def norm(value: str) -> str:
    return value.replace('\\', '/').lstrip('./').lower()


def direct_aar(raw: bytes) -> bool:
    try:
        with zipfile.ZipFile(io.BytesIO(raw), 'r') as archive:
            names = [norm(name) for name in archive.namelist() if not name.endswith('/')]
    except zipfile.BadZipFile:
        return False
    return (
        'androidmanifest.xml' in names
        and any(name.startswith('prefab/modules/discord_partner_sdk/') for name in names)
    )

raw = path.read_bytes()
if direct_aar(raw):
    raise SystemExit(0)

names: list[str] = []
if zipfile.is_zipfile(path):
    with zipfile.ZipFile(path, 'r') as archive:
        names = [norm(info.filename) for info in archive.infolist() if not info.is_dir()]
else:
    try:
        is_tar = tarfile.is_tarfile(path)
    except OSError:
        is_tar = False
    if is_tar:
        with tarfile.open(path, 'r:*') as archive:
            names = [norm(member.name) for member in archive.getmembers() if member.isfile()]

has_aar = any(pathlib.PurePosixPath(name).name == 'discord_partner_sdk.aar' for name in names)
has_desktop_headers = any(name.endswith('/include/discordpp.h') or name == 'include/discordpp.h' for name in names)
raise SystemExit(0 if has_aar and not has_desktop_headers else 1)
PY
then
  PULSEFORGE_DISCORD_SDK_LOCAL_INPUT="$raw" \
    bash "$project_root/scripts/prepare-discord-android-sdk.sh"
  exit 0
fi

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
  --sdk "$archive" --require "$require" --deep

"$project_root/scripts/setup-discord-social-sdk.sh" \
  --sdk "$archive" \
  --destination "$project_root/third_party/discord_social_sdk" \
  --force

sdk="$project_root/third_party/discord_social_sdk"

# Desktop integrations compile against the public C/C++ headers. Android-only
# private builds use the dedicated AAR path above when no desktop bundle exists.
if [[ "$require" != android ]]; then
  test -f "$sdk/include/discordpp.h"
  test -f "$sdk/include/cdiscord.h"
fi

case "$require" in
  windows)
    test -f "$sdk/lib/release/discord_partner_sdk.lib"
    test -f "$sdk/bin/release/discord_partner_sdk.dll"
    ;;
  linux)
    test -f "$sdk/lib/release/libdiscord_partner_sdk.so"
    ;;
  macos)
    find "$sdk" -type d -name 'discord_partner_sdk.framework' -print -quit | grep -q .
    ;;
  android)
    test -f "$sdk/android/discord_partner_sdk.aar"
    ;;
  all)
    test -f "$sdk/include/discordpp.h"
    test -f "$sdk/include/cdiscord.h"
    test -f "$sdk/lib/release/discord_partner_sdk.lib"
    test -f "$sdk/bin/release/discord_partner_sdk.dll"
    test -f "$sdk/lib/release/libdiscord_partner_sdk.so"
    test -f "$sdk/android/discord_partner_sdk.aar"
    find "$sdk" -type d -name 'discord_partner_sdk.framework' -print -quit | grep -q .
    ;;
esac

echo "Discord Social SDK release input staged and validated for target: $require."
