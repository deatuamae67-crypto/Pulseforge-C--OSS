#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK=''
FORCE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --sdk) SDK="$2"; shift 2;;
    --force) FORCE=1; shift;;
    *) echo "Unknown argument: $1" >&2; exit 2;;
  esac
done
[ -n "$SDK" ] || { echo 'Usage: setup-discord-social-sdk.sh --sdk <archive-or-directory> [--force]' >&2; exit 2; }
DEST="$ROOT/third_party/discord_social_sdk"
if [ -e "$DEST" ] && [ "$FORCE" -ne 1 ]; then
  echo "$DEST already exists; use --force" >&2; exit 3
fi
rm -rf "$DEST"; mkdir -p "$DEST"
if [ -d "$SDK" ]; then cp -R "$SDK"/. "$DEST"/; else
  case "$SDK" in
    *.zip) unzip -q "$SDK" -d "$DEST";;
    *.tar.gz|*.tgz) tar -xzf "$SDK" -C "$DEST";;
    *.tar.xz) tar -xJf "$SDK" -C "$DEST";;
    *) echo "Unsupported SDK archive: $SDK" >&2; exit 4;;
  esac
fi
shopt -s nullglob dotglob
entries=("$DEST"/*)
if [ ${#entries[@]} -eq 1 ] && [ -d "${entries[0]}" ]; then
  tmp="$DEST.__flat"; rm -rf "$tmp"; mv "${entries[0]}" "$tmp"; rmdir "$DEST"; mv "$tmp" "$DEST"
fi
echo "Discord Social SDK staged at $DEST (gitignored)."
