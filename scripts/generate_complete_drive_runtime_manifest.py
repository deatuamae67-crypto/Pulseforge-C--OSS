#!/usr/bin/env python3
"""Generate the lightweight PulseForge Complete runtime download manifest.

The heavy mod corpus stays in Google Drive. This script asks gdown only for
folder metadata (JSON paths + stable Drive download URLs); it never downloads
mod payload bytes. The resulting manifest can therefore be embedded in every
platform runtime without duplicating the Complete corpus in Git/LFS or release
packages.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tempfile
import time
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
DESCRIPTORS = ROOT / "docs" / "complete" / "mods"
SLUG_RE = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
DRIVE_URL_PREFIXES = (
    "https://drive.google.com/",
    "https://drive.usercontent.google.com/",
)


def safe_relative_path(raw: object) -> PurePosixPath:
    if not isinstance(raw, str) or not raw or "\x00" in raw:
        raise ValueError(f"invalid gdown manifest path: {raw!r}")
    path = PurePosixPath(raw.replace("\\", "/"))
    if path.is_absolute() or not path.parts:
        raise ValueError(f"absolute/empty gdown path refused: {raw!r}")
    if any(part in ("", ".", "..") for part in path.parts):
        raise ValueError(f"unsafe gdown path refused: {raw!r}")
    if ":" in path.parts[0]:
        raise ValueError(f"drive-qualified gdown path refused: {raw!r}")
    return path


def _enumerate_folder_once(python: str, drive_id: str) -> list[dict[str, str]]:
    folder_url = f"https://drive.google.com/drive/folders/{drive_id}"
    command = [python, "-m", "gdown", folder_url, "--folder", "--json", "--quiet"]
    result = subprocess.run(
        command,
        check=True,
        text=True,
        capture_output=True,
        timeout=300,
    )
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"gdown JSON manifest was invalid: {exc}") from exc
    if not isinstance(payload, list) or not payload:
        raise RuntimeError(f"gdown returned an empty/non-list manifest for {drive_id}")

    files: list[dict[str, str]] = []
    exact: set[str] = set()
    folded: set[str] = set()
    for entry in payload:
        if not isinstance(entry, dict):
            raise RuntimeError(f"invalid gdown manifest entry: {entry!r}")
        url = entry.get("url")
        if not isinstance(url, str) or not url.startswith(DRIVE_URL_PREFIXES):
            raise RuntimeError(f"invalid/non-Drive gdown manifest URL: {url!r}")
        path = safe_relative_path(entry.get("path")).as_posix()
        folded_path = path.casefold()
        if path in exact:
            raise RuntimeError(f"duplicate gdown manifest path: {path}")
        if folded_path in folded:
            raise RuntimeError(f"case-colliding gdown manifest path: {path}")
        exact.add(path)
        folded.add(folded_path)
        files.append({"path": path, "url": url})
    files.sort(key=lambda item: item["path"].casefold())
    return files


def enumerate_folder(
    python: str,
    drive_id: str,
    *,
    attempts: int = 4,
) -> list[dict[str, str]]:
    """Enumerate one Drive folder with bounded retries for transient gdown failures.

    Retrying the individual folder avoids throwing away the metadata already
    collected for earlier Complete descriptors when Drive/gdown has a temporary
    request failure. Descriptor threshold failures remain non-retryable in
    ``build_manifest`` because they represent a real inventory mismatch.
    """
    last_error: BaseException | None = None
    for attempt in range(1, attempts + 1):
        try:
            return _enumerate_folder_once(python, drive_id)
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired, RuntimeError) as exc:
            last_error = exc
            if attempt >= attempts:
                break
            delay = min(30, attempt * 5)
            detail = str(exc)
            if isinstance(exc, subprocess.CalledProcessError) and exc.stderr:
                detail = exc.stderr.strip() or detail
            print(
                f"Drive enumeration retry {attempt}/{attempts - 1} for {drive_id} "
                f"after: {detail}; sleeping {delay}s",
                file=sys.stderr,
                flush=True,
            )
            time.sleep(delay)
    raise RuntimeError(
        f"failed to enumerate Google Drive folder {drive_id} after {attempts} attempts"
    ) from last_error


def load_descriptors(directory: Path) -> list[dict[str, Any]]:
    descriptors: list[dict[str, Any]] = []
    for path in sorted(directory.glob("*.json"), key=lambda item: item.name.casefold()):
        payload = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(payload, dict):
            raise RuntimeError(f"descriptor is not an object: {path}")
        required = ("slug", "name", "drive_id", "enabled_by_default")
        missing = [key for key in required if key not in payload]
        if missing:
            raise RuntimeError(f"descriptor {path.name} misses {', '.join(missing)}")
        slug = payload["slug"]
        name = payload["name"]
        drive_id = payload["drive_id"]
        enabled = payload["enabled_by_default"]
        if not all(isinstance(value, str) and value for value in (slug, name, drive_id)):
            raise RuntimeError(f"descriptor {path.name} has invalid identity fields")
        if not SLUG_RE.fullmatch(slug) or len(slug) > 96:
            raise RuntimeError(f"descriptor {path.name} has a non-portable slug")
        if not isinstance(enabled, bool):
            raise RuntimeError(f"descriptor {path.name} has invalid enabled_by_default")
        descriptors.append(payload)
    if not descriptors:
        raise RuntimeError(f"no Complete descriptors found under {directory}")
    slugs = [item["slug"] for item in descriptors]
    if len(slugs) != len(set(slugs)):
        raise RuntimeError("duplicate Complete descriptor slug")
    return descriptors


def compute_revision(value: Any) -> str:
    canonical = json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def build_manifest(
    descriptors: list[dict[str, Any]],
    python: str,
    source_commit: str,
) -> dict[str, Any]:
    mods: list[dict[str, Any]] = []
    for index, descriptor in enumerate(descriptors, 1):
        slug = descriptor["slug"]
        print(
            f"[{index}/{len(descriptors)}] enumerating {slug} ({descriptor['drive_id']})",
            file=sys.stderr,
            flush=True,
        )
        files = enumerate_folder(python, descriptor["drive_id"])
        minimum_files = int(descriptor.get("min_files", 1))
        if len(files) < minimum_files:
            raise RuntimeError(
                f"{slug}: Drive manifest has {len(files)} files, expected >= {minimum_files}"
            )
        revision_basis = {
            "slug": slug,
            "name": descriptor["name"],
            "drive_id": descriptor["drive_id"],
            "enabled_by_default": descriptor["enabled_by_default"],
            "expected_min_files": minimum_files,
            "expected_min_bytes": int(descriptor.get("min_bytes", 0)),
            "files": files,
        }
        mods.append({**revision_basis, "revision": compute_revision(revision_basis)})
    revision = compute_revision(mods)
    return {
        "schema_version": 1,
        "edition": "1.0.0-complete",
        "source_commit": source_commit,
        "content_revision": revision,
        "mod_count": len(mods),
        "transport": "google-drive-gdown-resolved-https",
        "mods": mods,
    }


def self_test() -> None:
    assert safe_relative_path("Wrapper/data/song/chart.json").as_posix() == (
        "Wrapper/data/song/chart.json"
    )
    for bad in ("", "/absolute", "../escape", "a/../b", "C:/drive", "a\x00b"):
        try:
            safe_relative_path(bad)
        except ValueError:
            pass
        else:
            raise AssertionError(f"unsafe path accepted: {bad!r}")

    first = {"slug": "a", "files": [{"path": "x", "url": "https://drive.google.com/uc?id=x"}]}
    second = json.loads(json.dumps(first))
    assert compute_revision(first) == compute_revision(second)
    second["files"][0]["path"] = "y"
    assert compute_revision(first) != compute_revision(second)
    assert SLUG_RE.fullmatch("drive-pack-example-9k")
    assert not SLUG_RE.fullmatch("Bad Slug")

    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        descriptor = {
            "schema_version": 1,
            "edition": "1.0.0-complete",
            "name": "Example",
            "slug": "example",
            "drive_id": "drive-id",
            "enabled_by_default": True,
            "min_files": 1,
            "min_bytes": 5,
        }
        (root / "example.json").write_text(json.dumps(descriptor), encoding="utf-8")
        loaded = load_descriptors(root)
        assert loaded[0]["slug"] == "example"
        assert loaded[0]["enabled_by_default"] is True
    print("Complete Drive runtime manifest generator self-test: PASS")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--descriptors", type=Path, default=DESCRIPTORS)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--source-commit", default=os.environ.get("GITHUB_SHA", "development"))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        self_test()
        return 0
    if args.output is None:
        parser.error("--output is required unless --self-test is used")

    descriptors = load_descriptors(args.descriptors)
    manifest = build_manifest(descriptors, args.python, args.source_commit)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(
        f"wrote {args.output}: {manifest['mod_count']} mods, "
        f"{sum(len(item['files']) for item in manifest['mods'])} files, "
        f"revision={manifest['content_revision']}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
