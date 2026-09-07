#!/usr/bin/env python3
"""Maintain the owner-only PulseForge Complete testing draft.

The public Complete distribution remains embedded-content.  This helper only
keeps a movable *draft* tag/source snapshot and TEST-ONLY overlays for Complete
mods that have just landed in ``main`` so the repository owner can validate the
integrated content before the final release.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
CONTENT_SPEC = ROOT / "docs/COMPLETE_CONTENT_1.0.0.json"
DESCRIPTOR_DIR = ROOT / "docs/complete/mods"
RELEASE_NOTES = ROOT / "docs/RELEASE_NOTES_1.0.0_COMPLETE.md"
COMPLETE_TAG = "v1.0.0-complete"
RELEASE_TITLE = "PulseForge 1.0.0 Complete"
SNAPSHOT_NAME = "PulseForge-v1.0.0-complete-TESTING-SNAPSHOT.json"
PART_BYTES = 1800 * 1024 * 1024
LFS_POINTER_PREFIX = b"version https://git-lfs.github.com/spec/v1\n"


def run(args: list[str], *, capture: bool = False, check: bool = True) -> str:
    result = subprocess.run(
        args,
        cwd=ROOT,
        check=check,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
    )
    return result.stdout.strip() if capture and result.stdout else ""


def gh_json(args: list[str]) -> object:
    output = run(["gh", *args], capture=True)
    return json.loads(output)


def load_descriptors() -> dict[str, dict[str, object]]:
    spec = json.loads(CONTENT_SPEC.read_text(encoding="utf-8"))
    if spec.get("schema_version") != 2:
        raise SystemExit("Complete content spec schema_version must be 2")
    if spec.get("edition") != "1.0.0-complete":
        raise SystemExit("Complete content spec edition mismatch")
    if spec.get("target_mod_count") != 30:
        raise SystemExit("Complete target_mod_count must be 30")

    descriptors: dict[str, dict[str, object]] = {}
    slugs: set[str] = set()
    casefold_names: set[str] = set()
    paths = sorted(DESCRIPTOR_DIR.glob("*.json"))
    if len(paths) != 30:
        raise SystemExit(f"expected 30 Complete descriptors, found {len(paths)}")
    for path in paths:
        item = json.loads(path.read_text(encoding="utf-8"))
        name = item.get("name")
        slug = item.get("slug")
        if not isinstance(name, str) or not name or "/" in name or "\\" in name:
            raise SystemExit(f"invalid Complete mod name in {path}: {name!r}")
        if not isinstance(slug, str) or not slug:
            raise SystemExit(f"invalid Complete slug in {path}: {slug!r}")
        folded = name.casefold()
        if folded in casefold_names:
            raise SystemExit(f"duplicate Complete mod name: {name}")
        if slug in slugs:
            raise SystemExit(f"duplicate Complete slug: {slug}")
        casefold_names.add(folded)
        slugs.add(slug)
        item["_descriptor_path"] = str(path.relative_to(ROOT))
        descriptors[name] = item
    return descriptors


def validate_contract() -> dict[str, dict[str, object]]:
    for path in (CONTENT_SPEC, RELEASE_NOTES):
        if not path.is_file() or path.stat().st_size == 0:
            raise SystemExit(f"required Complete file missing/empty: {path.relative_to(ROOT)}")
    descriptors = load_descriptors()
    print(f"Complete testing-draft contract valid: {len(descriptors)} descriptors")
    return descriptors


def git_head() -> str:
    return run(["git", "rev-parse", "HEAD"], capture=True)


def resolve_before(value: str | None) -> str:
    if value and value.strip("0"):
        # Ensure the event's before SHA is locally available before diffing.
        result = subprocess.run(
            ["git", "cat-file", "-e", f"{value}^{{commit}}"], cwd=ROOT
        )
        if result.returncode == 0:
            return value
    return run(["git", "rev-parse", "HEAD^"], capture=True)


def changed_complete_mods(
    descriptors: dict[str, dict[str, object]], before: str, head: str
) -> list[tuple[str, str]]:
    changed = run(
        ["git", "diff", "--name-only", before, head, "--", "mods"], capture=True
    ).splitlines()
    names: set[str] = set()
    for item in changed:
        parts = PurePosixPath(item).parts
        if len(parts) < 3 or parts[0] != "mods":
            continue
        name = parts[1]
        if name in descriptors and (ROOT / "mods" / name).is_dir():
            names.add(name)
    resolved = [(name, str(descriptors[name]["slug"])) for name in sorted(names, key=str.casefold)]
    print(f"Changed Complete mods: {len(resolved)}")
    for name, slug in resolved:
        print(f" - {slug}: {name}")
    return resolved


def hydrate_mod(name: str) -> Path:
    target = ROOT / "mods" / name
    if not target.is_dir():
        raise SystemExit(f"Complete mod directory missing: {target}")
    run(["git", "lfs", "pull", f"--include=mods/{name}/**", "--exclude="])
    for path in target.rglob("*"):
        if not path.is_file():
            continue
        with path.open("rb") as handle:
            head = handle.read(256)
        if head.startswith(LFS_POINTER_PREFIX) and b"\noid sha256:" in head:
            raise SystemExit(f"unhydrated LFS pointer: {path.relative_to(ROOT)}")
    return target


def write_zip(target: Path, archive: Path) -> tuple[int, int]:
    files = sorted((p for p in target.rglob("*") if p.is_file()), key=lambda p: p.as_posix().casefold())
    total = sum(p.stat().st_size for p in files)
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_STORED, allowZip64=True) as zf:
        for path in files:
            zf.write(path, path.relative_to(ROOT / "mods"))
    return len(files), total


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def split_if_needed(archive: Path) -> list[Path]:
    if archive.stat().st_size <= PART_BYTES:
        return [archive]
    parts: list[Path] = []
    with archive.open("rb") as source:
        index = 0
        while True:
            chunk = source.read(PART_BYTES)
            if not chunk:
                break
            part = archive.with_name(f"{archive.name}.part{index:03d}")
            part.write_bytes(chunk)
            parts.append(part)
            index += 1
    archive.unlink()
    return parts


def package_changed_mods(
    changed: list[tuple[str, str]], head: str, output_dir: Path
) -> list[Path]:
    output_dir.mkdir(parents=True, exist_ok=True)
    upload: list[Path] = []
    for name, slug in changed:
        target = hydrate_mod(name)
        base = f"PulseForge-v1.0.0-complete-TEST-ONLY-mod-{slug}"
        archive = output_dir / f"{base}.zip"
        file_count, total_bytes = write_zip(target, archive)
        digest = sha256_file(archive)
        checksum = output_dir / f"{base}.zip.SHA256SUMS.txt"
        checksum.write_text(f"{digest}  {base}.zip\n", encoding="ascii")
        manifest = output_dir / f"{base}.manifest.json"
        manifest.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "kind": "complete-testing-overlay",
                    "testing_only": True,
                    "commit": head,
                    "mod_name": name,
                    "slug": slug,
                    "file_count": file_count,
                    "total_bytes": total_bytes,
                    "archive_sha256": digest,
                    "extract_into": "mods/",
                },
                indent=2,
                ensure_ascii=False,
            )
            + "\n",
            encoding="utf-8",
        )
        upload.extend(split_if_needed(archive))
        upload.extend([checksum, manifest])
    return upload


def write_snapshot(
    descriptors: dict[str, dict[str, object]], changed: list[tuple[str, str]], head: str, output_dir: Path
) -> Path:
    integrated = [
        {"name": name, "slug": str(item["slug"])}
        for name, item in descriptors.items()
        if (ROOT / "mods" / name).is_dir()
    ]
    integrated.sort(key=lambda item: item["name"].casefold())
    snapshot = output_dir / SNAPSHOT_NAME
    snapshot.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "edition": "1.0.0-complete",
                "testing_only": True,
                "commit": head,
                "merged_complete_mod_count": len(integrated),
                "merged_complete_mods": integrated,
                "changed_in_this_snapshot": [
                    {"name": name, "slug": slug} for name, slug in changed
                ],
            },
            indent=2,
            ensure_ascii=False,
        )
        + "\n",
        encoding="utf-8",
    )
    return snapshot


def paginated(endpoint: str) -> list[dict[str, object]]:
    payload = gh_json(["api", "--paginate", "--slurp", endpoint])
    if not isinstance(payload, list):
        raise SystemExit(f"unexpected GitHub API pagination payload for {endpoint}")
    flattened: list[dict[str, object]] = []
    for page in payload:
        if isinstance(page, list):
            flattened.extend(item for item in page if isinstance(item, dict))
        elif isinstance(page, dict):
            flattened.append(page)
    return flattened


def find_release(repo: str) -> dict[str, object] | None:
    for item in paginated(f"repos/{repo}/releases?per_page=100"):
        if item.get("tag_name") == COMPLETE_TAG:
            return item
    return None


def ensure_tag(repo: str, head: str) -> None:
    probe = subprocess.run(
        ["gh", "api", f"repos/{repo}/git/ref/tags/{COMPLETE_TAG}"],
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if probe.returncode == 0:
        run(
            [
                "gh", "api", "--method", "PATCH",
                f"repos/{repo}/git/refs/tags/{COMPLETE_TAG}",
                "-f", f"sha={head}", "-F", "force=true",
            ]
        )
    else:
        run(
            [
                "gh", "api", "--method", "POST", f"repos/{repo}/git/refs",
                "-f", f"ref=refs/tags/{COMPLETE_TAG}", "-f", f"sha={head}",
            ]
        )


def release_body(head: str) -> str:
    return (
        RELEASE_NOTES.read_text(encoding="utf-8").rstrip()
        + "\n\n---\n\n"
        + f"**Private testing snapshot:** `{head}`\n\n"
        + "The Git tag and TEST-ONLY mod overlay assets track the merged testing source. "
        + "These overlays are owner-validation artifacts only; the final public Complete "
        + "release remains an embedded-content distribution.\n"
    )


def ensure_draft(repo: str, head: str) -> int:
    current = find_release(repo)
    if current is not None and current.get("draft") is not True:
        raise SystemExit(f"{COMPLETE_TAG} is public; refusing to mutate a public release")
    ensure_tag(repo, head)
    body = release_body(head)
    if current is None:
        created = gh_json(
            [
                "api", "--method", "POST", f"repos/{repo}/releases",
                "-f", f"tag_name={COMPLETE_TAG}",
                "-f", f"target_commitish={head}",
                "-f", f"name={RELEASE_TITLE}",
                "-f", f"body={body}",
                "-F", "draft=true", "-F", "prerelease=false",
            ]
        )
        if not isinstance(created, dict) or not isinstance(created.get("id"), int):
            raise SystemExit("GitHub did not return a valid Complete draft release")
        return int(created["id"])
    release_id = int(current["id"])
    run(
        [
            "gh", "api", "--method", "PATCH", f"repos/{repo}/releases/{release_id}",
            "-f", f"name={RELEASE_TITLE}",
            "-f", f"target_commitish={head}",
            "-f", f"body={body}",
            "-F", "draft=true", "-F", "prerelease=false",
        ]
    )
    return release_id


def release_assets(repo: str, release_id: int) -> list[dict[str, object]]:
    return paginated(f"repos/{repo}/releases/{release_id}/assets?per_page=100")


def delete_stale_assets(
    repo: str, release_id: int, changed: list[tuple[str, str]]
) -> None:
    prefixes = [f"PulseForge-v1.0.0-complete-TEST-ONLY-mod-{slug}" for _, slug in changed]
    for item in release_assets(repo, release_id):
        name = item.get("name")
        asset_id = item.get("id")
        if not isinstance(name, str) or not isinstance(asset_id, int):
            continue
        stale = name == SNAPSHOT_NAME or any(name.startswith(prefix) for prefix in prefixes)
        if stale:
            run(["gh", "api", "--method", "DELETE", f"repos/{repo}/releases/assets/{asset_id}"])


def upload_assets(repo: str, paths: list[Path]) -> None:
    if not paths:
        raise SystemExit("no Complete testing assets were generated")
    run(["gh", "release", "upload", COMPLETE_TAG, "--repo", repo, *map(str, paths)])


def verify(repo: str, release_id: int, head: str) -> None:
    release = gh_json(["api", f"repos/{repo}/releases/{release_id}"])
    if not isinstance(release, dict):
        raise SystemExit("invalid Complete draft release response")
    if release.get("draft") is not True:
        raise SystemExit("Complete testing release unexpectedly became public")
    if release.get("tag_name") != COMPLETE_TAG:
        raise SystemExit("Complete testing release tag_name mismatch")
    if release.get("target_commitish") != head:
        raise SystemExit("Complete testing release target_commitish mismatch")
    tag = gh_json(["api", f"repos/{repo}/git/ref/tags/{COMPLETE_TAG}"])
    try:
        tag_sha = tag["object"]["sha"]  # type: ignore[index]
    except (KeyError, TypeError):
        raise SystemExit("invalid Complete testing tag response") from None
    if tag_sha != head:
        raise SystemExit(f"Complete testing tag mismatch: {tag_sha} != {head}")
    print(f"Complete private testing draft now tracks {head} exactly")


def update(before: str | None) -> None:
    descriptors = validate_contract()
    if shutil.which("gh") is None or shutil.which("git") is None:
        raise SystemExit("gh and git are required")
    repo = os.environ.get("GITHUB_REPOSITORY")
    if not repo:
        raise SystemExit("GITHUB_REPOSITORY is required")
    head = git_head()
    prior = resolve_before(before)
    changed = changed_complete_mods(descriptors, prior, head)
    if not changed:
        raise SystemExit("testing-draft update was triggered without a changed Complete mod")
    output_dir = Path(os.environ.get("RUNNER_TEMP", "/tmp")) / "pulseforge-complete-testing"
    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True)
    assets = package_changed_mods(changed, head, output_dir)
    snapshot = write_snapshot(descriptors, changed, head, output_dir)
    assets.append(snapshot)
    release_id = ensure_draft(repo, head)
    delete_stale_assets(repo, release_id, changed)
    upload_assets(repo, assets)
    verify(repo, release_id, head)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--validate-only", action="store_true")
    parser.add_argument("--update", action="store_true")
    parser.add_argument("--before")
    args = parser.parse_args()
    if args.validate_only == args.update:
        parser.error("choose exactly one of --validate-only or --update")
    if args.validate_only:
        validate_contract()
    else:
        update(args.before)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
