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

upload_candidate() {
  local file="$1"
  local stable candidate encoded
  stable="$(basename "$file")"
  candidate="CANDIDATE-${GITHUB_SHA}--${stable}"

  # Make a failed package/finalize attempt safely retryable without requiring
  # another prepare job first.
  mapfile -t duplicates < <(
    gh api "repos/$GITHUB_REPOSITORY/releases/$RELEASE_ID/assets?per_page=100" --paginate \
      --jq ".[] | select(.name == \"$candidate\") | .id"
  )
  for id in "${duplicates[@]}"; do
    gh api --method DELETE "repos/$GITHUB_REPOSITORY/releases/assets/$id" >/dev/null
  done

  encoded="$(python3 - "$candidate" <<'PY'
import sys, urllib.parse
print(urllib.parse.quote(sys.argv[1], safe=''))
PY
)"
  echo "Uploading Complete runtime candidate: $stable"
  curl --fail-with-body --silent --show-error \
    --retry 5 --retry-all-errors --retry-delay 5 \
    -X POST \
    -H "Accept: application/vnd.github+json" \
    -H "Authorization: Bearer $GH_TOKEN" \
    -H "X-GitHub-Api-Version: 2022-11-28" \
    -H "Content-Type: application/octet-stream" \
    --upload-file "$file" \
    "https://uploads.github.com/repos/$GITHUB_REPOSITORY/releases/$RELEASE_ID/assets?name=$encoded" \
    >/dev/null
}

checksum_and_upload_files() {
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

require_runtime_manifest() {
  local manifest="$GITHUB_WORKSPACE/assets/complete-runtime-manifest.json"
  test -s "$manifest"
  jq -e '
    .schema_version == 1
    and .edition == "1.0.0-complete"
    and .mod_count == 30
    and (.mods | length) == 30
  ' "$manifest" >/dev/null
}

package_windows() {
  local package="PulseForge-v${PACKAGE_VERSION}-Windows-x86_64"
  local root="$RUNNER_TEMP/package-windows"
  local out="$RUNNER_TEMP/release-windows"
  local archive="$out/$package.zip"
  rm -rf "$root" "$out"
  mkdir -p "$root/$package/bin" "$out"
  cp -a "$RUNNER_TEMP/compiled/complete-engine-stage-windows-x86_64/." "$root/$package/"
  rm -rf "$root/$package/bin/assets" "$root/$package/bin/mods"
  ln -s "$GITHUB_WORKSPACE/assets" "$root/$package/bin/assets"
  test -f "$root/$package/bin/pulseforge.exe"
  test -f "$root/$package/bin/assets/complete-runtime-manifest.json"
  test ! -e "$root/$package/bin/mods"
  bsdtar -L --format zip -cf "$archive" -C "$root" "$package"
  checksum_and_upload_files "$out" "$package.SHA256SUMS.txt" "$archive"
  rm -rf "$root"
}

package_unix_desktop() {
  local platform="$1" stage_key="$2"
  local package="PulseForge-v${PACKAGE_VERSION}-${platform}"
  local root="$RUNNER_TEMP/package-${stage_key}"
  local out="$RUNNER_TEMP/release-${stage_key}"
  local archive="$out/$package.tar.gz"
  rm -rf "$root" "$out"
  mkdir -p "$root/$package/bin" "$out"
  cp -a "$RUNNER_TEMP/compiled/complete-engine-stage-${stage_key}/." "$root/$package/"
  rm -rf "$root/$package/bin/assets" "$root/$package/bin/mods"
  ln -s "$GITHUB_WORKSPACE/assets" "$root/$package/bin/assets"
  if [[ "$stage_key" == linux-* ]]; then
    test -x "$root/$package/bin/pulseforge"
  else
    local app="$root/$package/pulseforge.app"
    test -d "$app/Contents/MacOS"
    test -f "$app/Contents/Info.plist"
    test -x "$app/Contents/MacOS/pulseforge"
    rm -rf "$app/Contents/MacOS/assets"
    ln -s "$GITHUB_WORKSPACE/assets" "$app/Contents/MacOS/assets"
    test -f "$app/Contents/MacOS/assets/complete-runtime-manifest.json"
  fi
  test -f "$root/$package/bin/assets/complete-runtime-manifest.json"
  test ! -e "$root/$package/bin/mods"
  tar --dereference -C "$root" -czf "$archive" "$package"
  checksum_and_upload_files "$out" "$package.SHA256SUMS.txt" "$archive"
  rm -rf "$root"
}

package_android() {
  local package="PulseForge-v${PACKAGE_VERSION}-Android-arm64"
  local stage="$RUNNER_TEMP/compiled/complete-engine-stage-android-arm64"
  local out="$RUNNER_TEMP/release-android"
  rm -rf "$out"
  mkdir -p "$out"

  local source_apk
  source_apk="$(find "$stage" -maxdepth 1 -type f -name '*.apk' -print -quit)"
  test -n "$source_apk"
  local apk="$out/${package}-test-signed.apk"
  cp -a "$source_apk" "$apk"

  unzip -l "$apk" > "$out/${package}.contents.txt"
  grep -q 'assets/pulseforge/assets/complete-runtime-manifest.json' "$out/${package}.contents.txt"
  grep -q 'lib/arm64-v8a/libmain.so' "$out/${package}.contents.txt"
  grep -q 'lib/arm64-v8a/libSDL3.so' "$out/${package}.contents.txt"
  ! grep -Eq '(^|[ /])mods/' "$out/${package}.contents.txt"

  # Android Complete is the installable runtime itself. The 30-mod corpus is
  # downloaded on demand from Drive after the user accepts the startup prompt.
  checksum_and_upload_files "$out" "$package.SHA256SUMS.txt" "$apk"
  rm -f "$out/${package}.contents.txt"
}

verify_candidates() {
  local prefix="CANDIDATE-${GITHUB_SHA}--"
  local names="$RUNNER_TEMP/candidate-names.txt"
  gh api "repos/$GITHUB_REPOSITORY/releases/$RELEASE_ID/assets?per_page=100" --paginate \
    --jq ".[] | select(.name | startswith(\"$prefix\")) | .name" \
    | sed "s/^$prefix//" | sort > "$names"

  grep -Fxq "PulseForge-v${PACKAGE_VERSION}-Windows-x86_64.zip" "$names"
  grep -Fxq "PulseForge-v${PACKAGE_VERSION}-Windows-x86_64.SHA256SUMS.txt" "$names"
  for platform in Linux-x86_64 macOS-arm64 macOS-x86_64; do
    grep -Fxq "PulseForge-v${PACKAGE_VERSION}-${platform}.tar.gz" "$names"
    grep -Fxq "PulseForge-v${PACKAGE_VERSION}-${platform}.SHA256SUMS.txt" "$names"
  done
  grep -Fxq "PulseForge-v${PACKAGE_VERSION}-Android-arm64.SHA256SUMS.txt" "$names"
  grep -Fxq "PulseForge-v${PACKAGE_VERSION}-Android-arm64-test-signed.apk" "$names"
  ! grep -Eq '\.part-[0-9]{3}$' "$names"
  ! grep -Fq 'TEST-ONLY' "$names"
  ! grep -Fq -- '-mod-' "$names"
  test "$(wc -l < "$names")" -eq 10
  echo 'Verified five lightweight Complete runtime candidate sets in normal 1.0.0 package formats.'
}

finalize_release() {
  verify_candidates
  local prefix="CANDIDATE-${GITHUB_SHA}--"

  # Candidates coexist with the current stable assets until every runtime has
  # been uploaded and verified. Only then are the previous stable assets removed.
  mapfile -t old_ids < <(
    gh api "repos/$GITHUB_REPOSITORY/releases/$RELEASE_ID/assets?per_page=100" --paginate \
      --jq ".[] | select((.name | startswith(\"$prefix\")) | not) | .id"
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

**Compiled Complete runtime snapshot:** \`$GITHUB_SHA\`

The five platform downloads are lightweight engine runtimes built from this
exact tested `main` commit. They do **not** bundle the 30-mod corpus. Each
runtime carries the small `complete-runtime-manifest.json`; when content is
missing or outdated, PulseForge asks the user whether to download the required
mods directly from the canonical Google Drive folders.

The public downloads use the same practical formats as the normal 1.0.0 engine
release: a Windows `.zip`, Linux/macOS `.tar.gz` archives and the installable
Android `.apk`, each accompanied by a SHA-256 checksum file.
EOF
  )"
  gh api --method PATCH "repos/$GITHUB_REPOSITORY/releases/$RELEASE_ID" \
    -f tag_name="$COMPLETE_TAG" \
    -f target_commitish="$GITHUB_SHA" \
    -f name="$RELEASE_TITLE" \
    -f body="$body" \
    -F draft=false \
    -F prerelease=false >/dev/null

  test "$(gh api "repos/$GITHUB_REPOSITORY/releases/$RELEASE_ID" --jq '.draft')" = false
  test "$(gh api "repos/$GITHUB_REPOSITORY/releases/$RELEASE_ID" --jq '.target_commitish')" = "$GITHUB_SHA"
  test "$(gh api "repos/$GITHUB_REPOSITORY/releases/$RELEASE_ID" --jq '.tag_name')" = "$COMPLETE_TAG"
  test "$(gh api "repos/$GITHUB_REPOSITORY/git/ref/tags/$COMPLETE_TAG" --jq '.object.sha')" = "$GITHUB_SHA"

  local assets="$RUNNER_TEMP/final-assets.txt"
  gh api "repos/$GITHUB_REPOSITORY/releases/$RELEASE_ID/assets?per_page=100" --paginate --jq '.[].name' | sort > "$assets"
  ! grep -Fq 'CANDIDATE-' "$assets"
  ! grep -Fq 'STAGE-' "$assets"
  ! grep -Fq 'TEST-ONLY' "$assets"
  ! grep -Fq -- '-mod-' "$assets"
  ! grep -Eq '\.part-[0-9]{3}$' "$assets"
  grep -Fxq "PulseForge-v${PACKAGE_VERSION}-Windows-x86_64.zip" "$assets"
  grep -Fxq "PulseForge-v${PACKAGE_VERSION}-Windows-x86_64.SHA256SUMS.txt" "$assets"
  for platform in Linux-x86_64 macOS-arm64 macOS-x86_64; do
    grep -Fxq "PulseForge-v${PACKAGE_VERSION}-${platform}.tar.gz" "$assets"
    grep -Fxq "PulseForge-v${PACKAGE_VERSION}-${platform}.SHA256SUMS.txt" "$assets"
  done
  grep -Fxq "PulseForge-v${PACKAGE_VERSION}-Android-arm64.SHA256SUMS.txt" "$assets"
  grep -Fxq "PulseForge-v${PACKAGE_VERSION}-Android-arm64-test-signed.apk" "$assets"
  test "$(wc -l < "$assets")" -eq 10
  printf 'PulseForge Complete release now represents lightweight runtime commit %s\n' "$GITHUB_SHA"
}

case "${1:-}" in
  package)
    require_runtime_manifest
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
