#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="$(sed -nE 's/^[[:space:]]*VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+).*$/\1/p' "$ROOT/CMakeLists.txt" | head -n1)"
JOBS="${PULSEFORGE_BUILD_JOBS:-2}"
BUILD="${PULSEFORGE_BUILD_ROOT:-$ROOT/out/build/linux-release}"
STAGE="$ROOT/Release/Linux/PulseForge-v${VERSION}-Linux-x86_64"
OUT="$ROOT/Release/Linux"
rm -rf "$BUILD" "$STAGE"
mkdir -p "$BUILD" "$STAGE" "$OUT"
cmake -S "$ROOT" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DPULSEFORGE_BUILD_APP=ON \
  -DPULSEFORGE_BUILD_TESTS=OFF \
  -DPULSEFORGE_ENABLE_LUA=ON \
  -DPULSEFORGE_BUILD_AUTOCHART=OFF \
  -DPULSEFORGE_BUNDLE_FFMPEG=OFF \
  -DPULSEFORGE_WARNINGS_AS_ERRORS=OFF
cmake --build "$BUILD" --config Release --parallel "$JOBS"
cmake --install "$BUILD" --config Release --prefix "$STAGE"
TARBALL="$OUT/PulseForge-v${VERSION}-Linux-x86_64.tar.gz"
tar -C "$OUT" -czf "$TARBALL" "$(basename "$STAGE")"
( cd "$OUT" && sha256sum "$(basename "$TARBALL")" > SHA256SUMS.txt )
printf 'Created %s\n' "$TARBALL"
