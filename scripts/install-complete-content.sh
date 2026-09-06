#!/usr/bin/env bash
set -euo pipefail

REPO="${PULSEFORGE_REPO:-deatuamae67-crypto/Pulseforge-C--OSS}"
TAG="${PULSEFORGE_COMPLETE_TAG:-v1.0.0-complete}"
ENGINE_ROOT="${1:-$(pwd)}"

if [[ -d "$ENGINE_ROOT/bin" ]]; then
  TARGET="$ENGINE_ROOT/bin"
else
  TARGET="$ENGINE_ROOT"
fi
mkdir -p "$TARGET/mods" "$TARGET/assets/menu"

for tool in curl python3 unzip; do
  command -v "$tool" >/dev/null 2>&1 || { echo "Missing required tool: $tool" >&2; exit 1; }
done

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
API="https://api.github.com/repos/$REPO/releases/tags/$TAG"
curl -fsSL -H 'Accept: application/vnd.github+json' "$API" -o "$TMP/release.json"

python3 - "$TMP/release.json" "$TMP/assets.tsv" <<'PY'
import json, sys
release=json.load(open(sys.argv[1], encoding='utf-8'))
if release.get('draft') or release.get('tag_name') != 'v1.0.0-complete':
    raise SystemExit('Complete release is not public or has an unexpected tag')
with open(sys.argv[2], 'w', encoding='utf-8') as out:
    for asset in release.get('assets', []):
        out.write(f"{asset['name']}\t{asset['browser_download_url']}\n")
PY

asset_url() {
  local wanted="$1"
  awk -F '\t' -v n="$wanted" '$1==n {print $2; exit}' "$TMP/assets.tsv"
}

download_asset() {
  local name="$1" url
  url="$(asset_url "$name")"
  [[ -n "$url" ]] || { echo "Release asset not found: $name" >&2; exit 1; }
  curl -fL --retry 4 --retry-delay 2 "$url" -o "$TMP/$name"
}

verify_sha_file() {
  local checksum_file="$1" archive="$2" expected actual
  expected="$(awk 'NF {print $1; exit}' "$checksum_file")"
  if command -v sha256sum >/dev/null 2>&1; then
    actual="$(sha256sum "$archive" | awk '{print $1}')"
  else
    actual="$(shasum -a 256 "$archive" | awk '{print $1}')"
  fi
  [[ "$actual" == "$expected" ]] || { echo "SHA-256 mismatch: $(basename "$archive")" >&2; exit 1; }
}

MENU="PulseForge-v1.0.0-complete-menu-music.zip"
download_asset "$MENU"
download_asset "$MENU.SHA256SUMS.txt"
verify_sha_file "$TMP/$MENU.SHA256SUMS.txt" "$TMP/$MENU"
unzip -oq "$TMP/$MENU" -d "$TARGET"

MODSLIST="PulseForge-v1.0.0-complete-modsList.txt"
download_asset "$MODSLIST"
cp "$TMP/$MODSLIST" "$TARGET/mods/modsList.txt"

mapfile -t manifests < <(awk -F '\t' '$1 ~ /^PulseForge-v1\.0\.0-complete-mod-.*\.manifest\.json$/ {print $1}' "$TMP/assets.tsv" | sort)
[[ ${#manifests[@]} -gt 0 ]] || { echo 'No Complete mod manifests were found.' >&2; exit 1; }

for manifest in "${manifests[@]}"; do
  base="${manifest%.manifest.json}"
  zip_name="$base.zip"
  checksum_name="$zip_name.SHA256SUMS.txt"
  echo "Installing ${base#PulseForge-v1.0.0-complete-mod-}..."
  download_asset "$checksum_name"

  if [[ -n "$(asset_url "$zip_name")" ]]; then
    download_asset "$zip_name"
  else
    mapfile -t parts < <(awk -F '\t' -v p="$zip_name.part" 'index($1,p)==1 {print $1}' "$TMP/assets.tsv" | sort)
    [[ ${#parts[@]} -gt 0 ]] || { echo "Missing archive or split parts for $base" >&2; exit 1; }
    : > "$TMP/$zip_name"
    for part in "${parts[@]}"; do
      download_asset "$part"
      cat "$TMP/$part" >> "$TMP/$zip_name"
      rm -f "$TMP/$part"
    done
  fi

  verify_sha_file "$TMP/$checksum_name" "$TMP/$zip_name"
  unzip -oq "$TMP/$zip_name" -d "$TARGET"
  rm -f "$TMP/$zip_name" "$TMP/$checksum_name"
done

echo "PulseForge Complete content installed into: $TARGET"
