#!/usr/bin/env bash
set -euo pipefail

# Android-only Discord Social SDK hydration. The authorized AAR may be supplied
# directly or wrapped in a ZIP/TAR archive. The private source URL is never
# printed and the original SDK input is never published as an artifact.
url="${PULSEFORGE_DISCORD_SDK_ARCHIVE_URL:-}"
if [[ -z "$url" ]]; then
  echo 'PULSEFORGE_DISCORD_SDK_ARCHIVE_URL is required for Discord-enabled Android builds.' >&2
  exit 1
fi

project_root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
work_root="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/pulseforge-discord-android-sdk"
rm -rf -- "$work_root"
mkdir -p -- "$work_root"
raw="$work_root/sdk.download"
staged="$work_root/discord_partner_sdk.aar"

curl --fail --location --silent --show-error --retry 4 --retry-delay 2 \
  "$url" --output "$raw"
[[ -s "$raw" ]] || { echo 'Downloaded Discord Android SDK input is empty.' >&2; exit 1; }

python3 - "$raw" "$staged" <<'PY'
from __future__ import annotations

import io
import pathlib
import shutil
import sys
import tarfile
import zipfile

source = pathlib.Path(sys.argv[1])
destination = pathlib.Path(sys.argv[2])


def normalized(name: str) -> str:
    return name.replace('\\', '/').lstrip('./')


def aar_is_usable(raw: bytes) -> tuple[bool, str]:
    try:
        with zipfile.ZipFile(io.BytesIO(raw), 'r') as archive:
            names = [normalized(name) for name in archive.namelist() if not name.endswith('/')]
    except zipfile.BadZipFile:
        return False, 'input is not a valid AAR/ZIP'

    lower = [name.lower() for name in names]
    if 'androidmanifest.xml' not in lower:
        return False, 'AAR is missing AndroidManifest.xml'

    prefab_prefix = 'prefab/modules/discord_partner_sdk/'
    prefab_names = [name for name in lower if name.startswith(prefab_prefix)]
    if not prefab_names:
        return False, 'AAR is missing the discord_partner_sdk Prefab module'

    if not any(name.endswith('/discordpp.h') for name in prefab_names):
        return False, 'AAR Prefab module is missing discordpp.h'
    if not any(name.endswith('/cdiscord.h') for name in prefab_names):
        return False, 'AAR Prefab module is missing cdiscord.h'

    native = [
        name for name in lower
        if name.endswith('/libdiscord_partner_sdk.so')
        or name.endswith('/discord_partner_sdk.so')
    ]
    if not native:
        return False, 'AAR contains no Discord native shared library'
    if not any(
        '/arm64-v8a/' in name or '/android.arm64-v8a/' in name
        for name in native
    ):
        return False, 'AAR contains no arm64-v8a Discord runtime'

    return True, ''


payload = source.read_bytes()
direct_ok, _ = aar_is_usable(payload)
if direct_ok:
    destination.write_bytes(payload)
else:
    extracted: bytes | None = None
    if zipfile.is_zipfile(source):
        with zipfile.ZipFile(source, 'r') as archive:
            candidates = [
                info for info in archive.infolist()
                if not info.is_dir()
                and pathlib.PurePosixPath(normalized(info.filename)).name.lower()
                    == 'discord_partner_sdk.aar'
            ]
            if candidates:
                candidate = min(
                    candidates,
                    key=lambda info: (
                        normalized(info.filename).count('/'),
                        len(normalized(info.filename)),
                        normalized(info.filename).lower(),
                    ),
                )
                extracted = archive.read(candidate)
    else:
        try:
            is_tar = tarfile.is_tarfile(source)
        except OSError:
            is_tar = False
        if is_tar:
            with tarfile.open(source, 'r:*') as archive:
                candidates = [
                    member for member in archive.getmembers()
                    if member.isfile()
                    and pathlib.PurePosixPath(normalized(member.name)).name.lower()
                        == 'discord_partner_sdk.aar'
                ]
                if candidates:
                    candidate = min(
                        candidates,
                        key=lambda member: (
                            normalized(member.name).count('/'),
                            len(normalized(member.name)),
                            normalized(member.name).lower(),
                        ),
                    )
                    stream = archive.extractfile(candidate)
                    if stream is not None:
                        extracted = stream.read()

    if extracted is None:
        raise SystemExit(
            'Downloaded Android SDK input is neither a usable direct AAR nor an archive containing discord_partner_sdk.aar.'
        )
    usable, reason = aar_is_usable(extracted)
    if not usable:
        raise SystemExit(f'Nested discord_partner_sdk.aar is unusable: {reason}')
    destination.write_bytes(extracted)

usable, reason = aar_is_usable(destination.read_bytes())
if not usable:
    raise SystemExit(f'Staged discord_partner_sdk.aar is unusable: {reason}')
PY

sdk="$project_root/third_party/discord_social_sdk"
mkdir -p -- "$sdk/android"
install -m 0644 -- "$staged" "$sdk/android/discord_partner_sdk.aar"
test -s "$sdk/android/discord_partner_sdk.aar"

echo 'Discord Social SDK Android AAR staged and validated (Prefab + arm64-v8a).'
