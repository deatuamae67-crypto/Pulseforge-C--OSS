#!/usr/bin/env bash
set -euo pipefail

: "${GITHUB_REPOSITORY:?}"
: "${GITHUB_SHA:?}"
: "${GITHUB_WORKSPACE:?}"
: "${RUNNER_TEMP:?}"
: "${GH_TOKEN:?}"
: "${RELEASE_ID:?}"
: "${COMPLETE_TAG:?}"
: "${RELEASE_TITLE:?}"
: "${PACKAGE_VERSION:?}"
: "${RELEASE_PART_LIMIT:?}"

upload_candidate() {
  local file="$1"
  local stable candidate encoded
  stable="$(basename "$file")"
  candidate="CANDIDATE-${GITHUB_SHA}--${stable}"
  encoded="$(python3 - "$candidate" <<'PY'
import sys, urllib.parse
print(urllib.parse.quote(sys.argv[1], safe=''))
PY
)"
  echo "Uploading Complete candidate: $stable"
  curl --fail-with-body --silent --show-error \
    -X POST \
    -H "Accept: application/vnd.github+json" \
    -H "Authorization: Bearer $GH_TOKEN" \
    -H "X-GitHub-Api-Version: 2022-11-28" \
    -H "Content-Type: application/octet-stream" \
    --data-binary "@$file" \
    "https://uploads.github.com/repos/$GITHUB_REPOSITORY/releases/$RELEASE_ID/assets?name=$encoded" \
    >/dev/null
}

checksum_and_upload_parts() {
  local output_dir="$1"
  local checksum_name="$2"
  shift 2
  local files=("$@")
  (( ${#files[@]} >= 1 ))
  local checksums="$output_dir/$checksum_name"
  : > "$checksums"
  for file in "${files[@]}"; do
    local size
    size="$(stat -c '%s' "$file")"
    (( size < 2147483648 )) || {
      echo "release asset exceeds GitHub 2 GiB limit: $file ($size bytes)" >&2
      exit 1
    }
    (cd "$output_dir" && sha256sum "$(basename "$file")") >> "$checksums"
    upload_candidate "$file"
    rm -f "$file"
  done
  upload_candidate "$checksums"
  rm -f "$checksums"
}

package_windows() {
  local package="PulseForge-v${PACKAGE_VERSION}-Windows-x86_64"
  local root="$RUNNER_TEMP/package-windows"
  local out="$RUNNER_TEMP/release-windows"
  rm -rf "$root" "$out"
  mkdir -p "$root/$package/bin" "$out"
  cp -a "$RUNNER_TEMP/compiled/complete-engine-stage-windows-x86_64/." "$root/$package/"
  rm -rf "$root/$package/bin/assets" "$root/$package/bin/mods"
  ln -s "$GITHUB_WORKSPACE/assets" "$root/$package/bin/assets"
  ln -s "$GITHUB_WORKSPACE/mods" "$root/$package/bin/mods"
  test -f "$root/$package/bin/pulseforge.exe"
  test -f "$root/$package/bin/mods/modsList.txt"
  bsdtar -L --format zip -cf - -C "$root" "$package" \
    | split -b "$RELEASE_PART_LIMIT" -d -a 3 - "$out/$package.zip.part-"
  mapfile -t parts < <(find "$out" -maxdepth 1 -type f -name "$package.zip.part-*" -print | sort)
  checksum_and_upload_parts "$out" "$package.SHA256SUMS.txt" "${parts[@]}"
  rm -rf "$root"
}

package_unix_desktop() {
  local platform="$1" stage_key="$2"
  local package="PulseForge-v${PACKAGE_VERSION}-${platform}"
  local root="$RUNNER_TEMP/package-${stage_key}"
  local out="$RUNNER_TEMP/release-${stage_key}"
  rm -rf "$root" "$out"
  mkdir -p "$root/$package/bin" "$out"
  cp -a "$RUNNER_TEMP/compiled/complete-engine-stage-${stage_key}/." "$root/$package/"
  rm -rf "$root/$package/bin/assets" "$root/$package/bin/mods"
  ln -s "$GITHUB_WORKSPACE/assets" "$root/$package/bin/assets"
  ln -s "$GITHUB_WORKSPACE/mods" "$root/$package/bin/mods"
  if [[ "$stage_key" == linux-* ]]; then
    test -x "$root/$package/bin/pulseforge"
  else
    local app="$root/$package/PulseForge.app"
    rm -rf "$app/Contents/MacOS/assets"
    ln -s "$GITHUB_WORKSPACE/assets" "$app/Contents/MacOS/assets"
    test -x "$app/Contents/MacOS/pulseforge"
  fi
  test -f "$root/$package/bin/mods/modsList.txt"
  tar --dereference -C "$root" -cf - "$package" \
    | gzip -1 \
    | split -b "$RELEASE_PART_LIMIT" -d -a 3 - "$out/$package.tar.gz.part-"
  mapfile -t parts < <(find "$out" -maxdepth 1 -type f -name "$package.tar.gz.part-*" -print | sort)
  checksum_and_upload_parts "$out" "$package.SHA256SUMS.txt" "${parts[@]}"
  rm -rf "$root"
}

package_android() {
  local package="PulseForge-v${PACKAGE_VERSION}-Android-arm64"
  local root="$RUNNER_TEMP/package-android"
  local out="$RUNNER_TEMP/release-android"
  rm -rf "$root" "$out"
  mkdir -p "$root/$package" "$out"
  cp -a "$RUNNER_TEMP/compiled/complete-engine-stage-android-arm64/." "$root/$package/"
  ln -s "$GITHUB_WORKSPACE/mods" "$root/$package/mods"

  cat > "$root/$package/install-with-adb.sh" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
apk="$(find "$here" -maxdepth 1 -type f -name '*.apk' -print -quit)"
test -n "$apk"
adb install -r "$apk"
adb shell mkdir -p /sdcard/Android/data/org.pulseforge.engine/files/mods
adb push "$here/mods/." /sdcard/Android/data/org.pulseforge.engine/files/mods/
echo 'PulseForge Complete installed with the bundled mod tree.'
SH
  chmod +x "$root/$package/install-with-adb.sh"

  cat > "$root/$package/install-with-adb.ps1" <<'PS1'
$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Apk = Get-ChildItem -LiteralPath $Here -Filter '*.apk' -File | Select-Object -First 1
if ($null -eq $Apk) { throw 'PulseForge APK not found.' }
& adb install -r $Apk.FullName
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& adb shell mkdir -p /sdcard/Android/data/org.pulseforge.engine/files/mods
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& adb push (Join-Path $Here 'mods/.') /sdcard/Android/data/org.pulseforge.engine/files/mods/
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host 'PulseForge Complete installed with the bundled mod tree.'
PS1

  cat > "$root/$package/README-INSTALL.txt" <<EOF
PulseForge 1.0.0 Complete
Source commit: $GITHUB_SHA

This is one Complete Android distribution: the compiled/test-signed APK
and the same cumulative built-in mods tree used by the desktop packages.

Use install-with-adb.sh or install-with-adb.ps1 with Android platform-tools.
EOF

  test -f "$root/$package/mods/modsList.txt"
  tar --dereference -C "$root" -cf - "$package" \
    | gzip -1 \
    | split -b "$RELEASE_PART_LIMIT" -d -a 3 - "$out/$package.tar.gz.part-"
  mapfile -t parts < <(find "$out" -maxdepth 1 -type f -name "$package.tar.gz.part-*" -print | sort)
  checksum_and_upload_parts "$out" "$package.SHA256SUMS.txt" "${parts[@]}"
  rm -rf "$root"
}

verify_candidates() {
  local prefix="CANDIDATE-${GITHUB_SHA}--"
  local names="$RUNNER_TEMP/candidate-names.txt"
  gh api "repos/$GITHUB_REPOSITORY/releases/$RELEASE_ID/assets?per_page=100" --paginate \
    --jq ".[] | select(.name | startswith(\"$prefix\")) | .name" \
    | sed "s/^$prefix//" | sort > "$names"
  for platform in Windows-x86_64 Linux-x86_64 macOS-arm64 macOS-x86_64 Android-arm64; do
    grep -Fxq "PulseForge-v${PACKAGE_VERSION}-${platform}.SHA256SUMS.txt" "$names"
    grep -Eq "^PulseForge-v${PACKAGE_VERSION}-${platform}.*\\.part-[0-9]{3}$" "$names"
  done
  ! grep -Fq 'TEST-ONLY' "$names"
  ! grep -Fq -- '-mod-' "$names"
  echo 'Verified candidate assets for all five full compiled platform distributions.'
}

finalize_release() {
  verify_candidates
  local prefix="CANDIDATE-${GITHUB_SHA}--"
  mapfile -t old_ids < <(
    gh api "repos/$GITHUB_REPOSITORY/releases/$RELEASE_ID/assets?per_page=100" --paginate \
      --jq ".[] | select(.name | startswith(\"$prefix\") | not) | .id"
  )
  for id in "${old_ids[@]}"; do
    gh api --method DELETE "repos/$GITHUB_REPOSITORY/releases/assets/$id" >/dev/null
  done

  gh api "repos/$GITHUB_REPOSITORY/releases/$RELEASE_ID/assets?per_page=100" --paginate \
    --jq ".[] | select(.name | startswith(\"$prefix\")) | [.id, .name] | @tsv" \
    > "$RUNNER_TEMP/candidates.tsv"
  while IFS=$'\t' read -r id name; do
    local stable="${name#"$prefix"}"
    gh api --method PATCH "repos/$GITHUB_REPOSITORY/releases/assets/$id" -f name="$stable" >/dev/null
  done < "$RUNNER_TEMP/candidates.tsv"

  if gh api "repos/$GITHUB_REPOSITORY/git/ref/tags/$COMPLETE_TAG" >/dev/null 2>&1; then
    gh api --method PATCH "repos/$GITHUB_REPOSITORY/git/refs/tags/$COMPLETE_TAG" \
      -f sha="$GITHUB_SHA" -F force=true >/dev/null
  else
    gh api --method POST "repos/$GITHUB_REPOSITORY/git/refs" \
      -f ref="refs/tags/$COMPLETE_TAG" -f sha="$GITHUB_SHA" >/dev/null
  fi

  local body
  body="$(
    cat docs/RELEASE_NOTES_1.0.0_COMPLETE.md
    cat <<EOF

---

**Compiled Complete snapshot:** \`$GITHUB_SHA\`

Every platform package in this draft was rebuilt from this exact commit.
Each distribution contains the cumulative Complete content present in the
engine tree at that commit. There are no per-mod release assets.

Platform archives are split only when needed to remain below GitHub's
per-asset upload limit; the accompanying SHA256SUMS file authenticates
every part.
EOF
  )"
  gh api --method PATCH "repos/$GITHUB_REPOSITORY/releases/$RELEASE_ID" \
    -f tag_name="$COMPLETE_TAG" \
    -f target_commitish="$GITHUB_SHA" \
    -f name="$RELEASE_TITLE" \
    -f body="$body" \
    -F draft=true \
    -F prerelease=false >/dev/null

  test "$(gh api "repos/$GITHUB_REPOSITORY/releases/$RELEASE_ID" --jq '.draft')" = 'true'
  test "$(gh api "repos/$GITHUB_REPOSITORY/releases/$RELEASE_ID" --jq '.target_commitish')" = "$GITHUB_SHA"
  test "$(gh api "repos/$GITHUB_REPOSITORY/releases/$RELEASE_ID" --jq '.tag_name')" = "$COMPLETE_TAG"
  test "$(gh api "repos/$GITHUB_REPOSITORY/git/ref/tags/$COMPLETE_TAG" --jq '.object.sha')" = "$GITHUB_SHA"
  local assets="$RUNNER_TEMP/final-assets.txt"
  gh api "repos/$GITHUB_REPOSITORY/releases/$RELEASE_ID/assets?per_page=100" --paginate --jq '.[].name' | sort > "$assets"
  ! grep -Fq 'CANDIDATE-' "$assets"
  ! grep -Fq 'STAGE-' "$assets"
  ! grep -Fq 'TEST-ONLY' "$assets"
  ! grep -Fq -- '-mod-' "$assets"
  for platform in Windows-x86_64 Linux-x86_64 macOS-arm64 macOS-x86_64 Android-arm64; do
    grep -Fxq "PulseForge-v${PACKAGE_VERSION}-${platform}.SHA256SUMS.txt" "$assets"
    grep -Eq "^PulseForge-v${PACKAGE_VERSION}-${platform}.*\\.part-[0-9]{3}$" "$assets"
  done
  printf 'PulseForge Complete draft now represents compiled engine commit %s\n' "$GITHUB_SHA"
}

case "${1:-}" in
  package)
    package_windows
    package_unix_desktop Linux-x86_64 linux-x86_64
    package_unix_desktop macOS-arm64 macos-arm64
    package_unix_desktop macOS-x86_64 macos-x86_64
    package_android
    ;;
  finalize)
    finalize_release
    ;;
  *)
    echo "usage: $0 {package|finalize}" >&2
    exit 2
    ;;
esac
