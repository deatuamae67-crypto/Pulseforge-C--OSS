#!/usr/bin/env python3
"""Reconcile PulseForge 1.0.0 Complete testing drafts into one cumulative draft.

The canonical Complete draft is the historical draft that already carries the
engine/platform release artifacts. Mod/content testing assets are additive to
that draft and must never create or replace it with a mods-only draft.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from typing import Any

TITLE = "PulseForge 1.0.0 Complete"
TAG = "v1.0.0-complete"
ENGINE_ASSET_MARKERS = (
    "PulseForge-v1.0.0-complete-Windows-x86_64.zip",
    "PulseForge-v1.0.0-complete-Linux-x86_64.tar.gz",
    "PulseForge-v1.0.0-complete-Android-arm64-test-signed.apk",
    "PulseForge-v1.0.0-complete-macOS-arm64.tar.gz",
    "PulseForge-v1.0.0-complete-macOS-x86_64.tar.gz",
    "COMPLETE_CONTENT_1.0.0.json",
)


def run(args: list[str], *, capture: bool = False) -> str:
    result = subprocess.run(
        args,
        check=True,
        text=True,
        stdout=subprocess.PIPE if capture else None,
    )
    return result.stdout.strip() if capture and result.stdout else ""


def paginated(repo: str, endpoint: str) -> list[dict[str, Any]]:
    raw = run(
        ["gh", "api", "--paginate", "--slurp", f"repos/{repo}/{endpoint}"],
        capture=True,
    )
    pages = json.loads(raw)
    out: list[dict[str, Any]] = []
    for page in pages:
        if isinstance(page, list):
            out.extend(item for item in page if isinstance(item, dict))
        elif isinstance(page, dict):
            out.append(page)
    return out


def releases(repo: str) -> list[dict[str, Any]]:
    return paginated(repo, "releases?per_page=100")


def assets(repo: str, release_id: int) -> list[dict[str, Any]]:
    return paginated(repo, f"releases/{release_id}/assets?per_page=100")


def engine_score(items: list[dict[str, Any]]) -> int:
    names = {str(item.get("name", "")) for item in items}
    return sum(marker in names for marker in ENGINE_ASSET_MARKERS)


def choose_canonical(repo: str, candidates: list[dict[str, Any]]) -> dict[str, Any]:
    ranked: list[tuple[int, str, int, dict[str, Any]]] = []
    for release in candidates:
        release_id = int(release["id"])
        score = engine_score(assets(repo, release_id))
        created = str(release.get("created_at", ""))
        ranked.append((score, created, release_id, release))
        print(f"Complete draft candidate id={release_id}: engine-score={score}, created={created}")

    best_score = max(item[0] for item in ranked)
    tied = [item for item in ranked if item[0] == best_score]
    tied.sort(key=lambda item: (item[1], item[2]))
    chosen = tied[0][3]
    print(f"Canonical cumulative Complete draft: {chosen['id']}")
    return chosen


def ensure_tag(repo: str, head: str) -> None:
    probe = subprocess.run(
        ["gh", "api", f"repos/{repo}/git/ref/tags/{TAG}"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if probe.returncode == 0:
        run([
            "gh", "api", "--method", "PATCH",
            f"repos/{repo}/git/refs/tags/{TAG}",
            "-f", f"sha={head}", "-F", "force=true",
        ])
    else:
        run([
            "gh", "api", "--method", "POST", f"repos/{repo}/git/refs",
            "-f", f"ref=refs/tags/{TAG}", "-f", f"sha={head}",
        ])


def patch_canonical(repo: str, release_id: int, head: str) -> None:
    run([
        "gh", "api", "--method", "PATCH",
        f"repos/{repo}/releases/{release_id}",
        "-f", f"tag_name={TAG}",
        "-f", f"target_commitish={head}",
        "-f", f"name={TITLE}",
        "-F", "draft=true", "-F", "prerelease=false",
    ])


def download_asset(repo: str, asset_id: int, destination: Path) -> None:
    with destination.open("wb") as handle:
        subprocess.run(
            [
                "gh", "api", "-H", "Accept: application/octet-stream",
                f"repos/{repo}/releases/assets/{asset_id}",
            ],
            check=True,
            stdout=handle,
        )


def migrate_duplicate_assets(
    repo: str,
    canonical_id: int,
    duplicates: list[dict[str, Any]],
) -> None:
    existing = {str(item.get("name", "")) for item in assets(repo, canonical_id)}
    with tempfile.TemporaryDirectory(prefix="pulseforge-complete-draft-") as raw_tmp:
        tmp = Path(raw_tmp)
        for duplicate in duplicates:
            duplicate_id = int(duplicate["id"])
            for item in assets(repo, duplicate_id):
                name = str(item.get("name", ""))
                asset_id = item.get("id")
                if not name or not isinstance(asset_id, int):
                    continue
                if name in existing:
                    print(f"Already present on cumulative draft: {name}")
                    continue
                destination = tmp / name
                print(f"Migrating {name} from duplicate draft {duplicate_id}")
                download_asset(repo, asset_id, destination)
                run(["gh", "release", "upload", TAG, "--repo", repo, str(destination)])
                existing.add(name)


def delete_duplicates(repo: str, duplicates: list[dict[str, Any]]) -> None:
    for duplicate in duplicates:
        duplicate_id = int(duplicate["id"])
        print(f"Deleting duplicate mods-only Complete draft {duplicate_id}")
        run(["gh", "api", "--method", "DELETE", f"repos/{repo}/releases/{duplicate_id}"])


def verify(repo: str, canonical_id: int, head: str) -> None:
    matching = [
        item for item in releases(repo)
        if item.get("draft") is True and item.get("name") == TITLE
    ]
    if len(matching) != 1:
        raise SystemExit(f"expected exactly one Complete draft, found {len(matching)}")
    current = matching[0]
    if int(current["id"]) != canonical_id:
        raise SystemExit("canonical Complete draft identity changed unexpectedly")
    if current.get("tag_name") != TAG:
        raise SystemExit(f"Complete draft tag mismatch: {current.get('tag_name')!r}")
    if current.get("target_commitish") != head:
        raise SystemExit("Complete draft target_commitish does not track current main")
    current_assets = assets(repo, canonical_id)
    score = engine_score(current_assets)
    if score < 3:
        raise SystemExit(f"cumulative Complete draft lost engine/platform artifacts (score={score})")
    print(
        f"Verified one cumulative Complete draft: id={canonical_id}, "
        f"engine-score={score}, assets={len(current_assets)}, head={head}"
    )


def main() -> None:
    if shutil.which("gh") is None:
        raise SystemExit("gh CLI is required")
    repo = os.environ.get("GITHUB_REPOSITORY", "").strip()
    head = os.environ.get("GITHUB_SHA", "").strip()
    if not repo or not head:
        raise SystemExit("GITHUB_REPOSITORY and GITHUB_SHA are required")

    candidates = [
        item for item in releases(repo)
        if item.get("draft") is True and item.get("name") == TITLE
    ]
    if not candidates:
        print("No existing Complete draft; normal helper may create the first cumulative draft.")
        return

    canonical = choose_canonical(repo, candidates)
    canonical_id = int(canonical["id"])
    duplicates = [item for item in candidates if int(item["id"]) != canonical_id]

    ensure_tag(repo, head)
    patch_canonical(repo, canonical_id, head)
    migrate_duplicate_assets(repo, canonical_id, duplicates)
    delete_duplicates(repo, duplicates)
    verify(repo, canonical_id, head)


if __name__ == "__main__":
    main()
