#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: scripts/setup-discord-social-sdk.sh --sdk PATH [--android-aar FILE] [--destination DIR] [--force]

PATH may be an extracted Discord Social SDK directory, a .zip, or a .tar/.tar.gz/.tgz archive.
The script never downloads the SDK: obtain it from the Discord Developer Portal first.
EOF
}

project_root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
sdk_path=""
android_aar=""
destination="$project_root/third_party/discord_social_sdk"
force=0

while (($#)); do
  case "$1" in
    --sdk) sdk_path="${2:-}"; shift 2 ;;
    --android-aar) android_aar="${2:-}"; shift 2 ;;
    --destination) destination="${2:-}"; shift 2 ;;
    --force) force=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ -n "$sdk_path" ]] || { usage >&2; exit 2; }
[[ -e "$sdk_path" ]] || { echo "SDK path does not exist: $sdk_path" >&2; exit 1; }

tmp=""
cleanup() { [[ -z "$tmp" ]] || rm -rf -- "$tmp"; }
trap cleanup EXIT

source_root="$sdk_path"
if [[ -f "$sdk_path" ]]; then
  tmp="$(mktemp -d)"
  case "$sdk_path" in
    *.zip)
      command -v unzip >/dev/null || { echo 'unzip is required for .zip archives' >&2; exit 1; }
      unzip -q "$sdk_path" -d "$tmp"
      ;;
    *.tar|*.tar.gz|*.tgz)
      tar -xf "$sdk_path" -C "$tmp"
      ;;
    *)
      echo 'Desktop SDK archive must be .zip, .tar, .tar.gz or .tgz.' >&2
      exit 1
      ;;
  esac
  source_root="$tmp"
fi

header="$(find "$source_root" -type f -path '*/include/discordpp.h' -print -quit)"
[[ -n "$header" ]] || { echo "discordpp.h not found under $source_root" >&2; exit 1; }
sdk_root="$(dirname "$(dirname "$header")")"
[[ -f "$sdk_root/include/cdiscord.h" ]] || { echo "cdiscord.h missing under $sdk_root/include" >&2; exit 1; }

if [[ -e "$destination" ]] && [[ -n "$(find "$destination" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null)" ]]; then
  if (( ! force )); then
    echo "Destination already contains files: $destination (use --force to replace it)" >&2
    exit 1
  fi
  rm -rf -- "$destination"
fi
mkdir -p -- "$destination"
cp -a "$sdk_root"/. "$destination"/

if [[ -z "$android_aar" && -f "$destination/lib/release/discord_partner_sdk.aar" ]]; then
  android_aar="$destination/lib/release/discord_partner_sdk.aar"
fi
if [[ -n "$android_aar" ]]; then
  [[ -f "$android_aar" ]] || { echo "Android AAR does not exist: $android_aar" >&2; exit 1; }
  case "$android_aar" in *.aar) ;; *) echo 'Android SDK file must end in .aar' >&2; exit 1;; esac
  mkdir -p "$destination/android"
  cp -f -- "$android_aar" "$destination/android/discord_partner_sdk.aar"
fi

mac_framework="$(find "$destination" -type d -name 'discord_partner_sdk.framework' -print -quit 2>/dev/null || true)"

echo
echo "Discord Social SDK installed: $destination"
echo "  Windows import library: $([[ -f "$destination/lib/release/discord_partner_sdk.lib" ]] && echo yes || echo no)"
echo "  Windows runtime DLL:   $([[ -f "$destination/bin/release/discord_partner_sdk.dll" ]] && echo yes || echo no)"
echo "  Linux shared library:  $([[ -f "$destination/lib/release/libdiscord_partner_sdk.so" ]] && echo yes || echo no)"
echo "  macOS 1.10+ framework: $([[ -n "$mac_framework" ]] && echo yes || echo no)"
echo "  macOS legacy dylib:    $([[ -f "$destination/lib/release/libdiscord_partner_sdk.dylib" ]] && echo yes || echo no)"
echo "  Android AAR:            $([[ -f "$destination/android/discord_partner_sdk.aar" ]] && echo yes || echo no)"
if [[ -n "$mac_framework" ]]; then
  echo "  macOS framework path:   ${mac_framework#"$destination"/}"
fi
echo
if command -v sha256sum >/dev/null; then
  find "$destination" -type f \( -name 'discordpp.h' -o -name 'cdiscord.h' -o -name 'discord_partner_sdk.*' -o -name 'libdiscord_partner_sdk.*' \) -print0 \
    | sort -z | xargs -0 -r sha256sum
fi
echo
echo 'Next: set a real Discord Application ID in assets/settings.json or configure with:'
echo '  cmake -S . -B build -DPULSEFORGE_DISCORD_APPLICATION_ID=123456789012345678'
if [[ -n "$mac_framework" && ! -f "$destination/lib/release/libdiscord_partner_sdk.dylib" ]]; then
  echo 'Note: the Social SDK 1.10 macOS framework was staged successfully, but PulseForge framework linking/embedding must be validated before using this package for a Discord-enabled macOS build.'
else
  echo 'PulseForge auto-detects the currently supported desktop library layout on the next CMake configure.'
fi
