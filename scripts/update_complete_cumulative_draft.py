#!/usr/bin/env python3
"""Update the single cumulative PulseForge 1.0.0 Complete draft by release ID.

Draft releases are not reliably addressable through ``gh release upload <tag>``.
This helper therefore keeps the historical engine-bearing Complete draft as the
canonical release and uploads testing assets directly to its numeric release ID.
"""

from __future__ import annotations

import argparse
import http.client
import json
import os
from pathlib import Path
import shutil
import sys
from urllib.parse import quote

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import reconcile_complete_testing_drafts as reconcile  # noqa: E402
import update_complete_testing_draft as legacy  # noqa: E402

TITLE = "PulseForge 1.0.0 Complete"
TAG = "v1.0.0-complete"


def upload_asset(repo: str, release_id: int, path: Path) -> None:
    token = os.environ.get("GH_TOKEN", "").strip()
    if not token:
        raise SystemExit("GH_TOKEN is required for release asset upload")
    owner, name = repo.split("/", 1)
    target = (
        f"/repos/{quote(owner, safe='')}/{quote(name, safe='')}/releases/"
        f"{release_id}/assets?name={quote(path.name, safe='')}"
    )
    conn = http.client.HTTPSConnection("uploads.github.com", timeout=300)
    conn.putrequest("POST", target)
    conn.putheader("Accept", "application/vnd.github+json")
    conn.putheader("Authorization", f"Bearer {token}")
    conn.putheader("X-GitHub-Api-Version", "2022-11-28")
    conn.putheader("Content-Type", "application/octet-stream")
    conn.putheader("Content-Length", str(path.stat().st_size))
    conn.endheaders()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(8 * 1024 * 1024), b""):
            conn.send(chunk)
    response = conn.getresponse()
    payload = response.read()
    conn.close()
    if response.status != 201:
        detail = payload.decode("utf-8", errors="replace")[:2000]
        raise SystemExit(
            f"release asset upload failed for {path.name}: HTTP {response.status}: {detail}"
        )
    print(f"Uploaded to cumulative draft {release_id}: {path.name}")


def patch_draft(repo: str, release_id: int, head: str) -> None:
    legacy.run(
        [
            "gh", "api", "--method", "PATCH",
            f"repos/{repo}/releases/{release_id}",
            "-f", f"name={TITLE}",
            "-f", f"target_commitish={head}",
            "-f", f"body={legacy.release_body(head)}",
            "-F", "draft=true",
            "-F", "prerelease=false",
        ]
    )


def migrate_duplicate_assets(
    repo: str,
    canonical_id: int,
    duplicates: list[dict[str, object]],
) -> None:
    existing = {
        str(item.get("name", ""))
        for item in reconcile.assets(repo, canonical_id)
    }
    import tempfile

    with tempfile.TemporaryDirectory(prefix="pulseforge-complete-draft-") as tmp_raw:
        tmp = Path(tmp_raw)
        for duplicate in duplicates:
            duplicate_id = int(duplicate["id"])
            for item in reconcile.assets(repo, duplicate_id):
                asset_name = str(item.get("name", ""))
                asset_id = item.get("id")
                if not asset_name or not isinstance(asset_id, int) or asset_name in existing:
                    continue
                destination = tmp / asset_name
                reconcile.download_asset(repo, asset_id, destination)
                upload_asset(repo, canonical_id, destination)
                existing.add(asset_name)
            legacy.run([
                "gh", "api", "--method", "DELETE",
                f"repos/{repo}/releases/{duplicate_id}",
            ])
            print(f"Deleted duplicate Complete draft {duplicate_id}")


def verify(repo: str, release_id: int, head: str) -> None:
    release = legacy.gh_json(["api", f"repos/{repo}/releases/{release_id}"])
    if not isinstance(release, dict):
        raise SystemExit("invalid cumulative Complete draft response")
    if release.get("draft") is not True:
        raise SystemExit("cumulative Complete release unexpectedly became public")
    if release.get("name") != TITLE:
        raise SystemExit("cumulative Complete draft title mismatch")
    if release.get("target_commitish") != head:
        raise SystemExit("cumulative Complete draft does not target current main")
    body = release.get("body")
    if not isinstance(body, str) or f"**Private testing snapshot:** `{head}`" not in body:
        raise SystemExit("cumulative Complete draft notes do not track current main")
    current_assets = reconcile.assets(repo, release_id)
    score = reconcile.engine_score(current_assets)
    if score < 3:
        raise SystemExit(
            f"cumulative Complete draft lost engine/platform artifacts (score={score})"
        )
    tag = legacy.gh_json(["api", f"repos/{repo}/git/ref/tags/{TAG}"])
    try:
        tag_sha = tag["object"]["sha"]  # type: ignore[index]
    except (KeyError, TypeError):
        raise SystemExit("invalid Complete testing tag response") from None
    if tag_sha != head:
        raise SystemExit(f"Complete testing tag mismatch: {tag_sha} != {head}")
    print(
        f"Verified cumulative Complete draft {release_id}: "
        f"engine-score={score}, assets={len(current_assets)}, head={head}"
    )


def update(before: str | None) -> None:
    descriptors = legacy.validate_contract()
    if shutil.which("gh") is None or shutil.which("git") is None:
        raise SystemExit("gh and git are required")
    repo = os.environ.get("GITHUB_REPOSITORY", "").strip()
    if not repo:
        raise SystemExit("GITHUB_REPOSITORY is required")
    head = legacy.git_head()

    candidates = [
        item
        for item in reconcile.releases(repo)
        if item.get("draft") is True and item.get("name") == TITLE
    ]
    if not candidates:
        raise SystemExit("no existing engine-bearing PulseForge 1.0.0 Complete draft found")
    canonical = reconcile.choose_canonical(repo, candidates)
    release_id = int(canonical["id"])
    duplicates = [item for item in candidates if int(item["id"]) != release_id]

    reconcile.ensure_tag(repo, head)
    patch_draft(repo, release_id, head)
    migrate_duplicate_assets(repo, release_id, duplicates)

    prior = legacy.resolve_before(before)
    changed = legacy.changed_complete_mods(descriptors, prior, head)
    if not changed:
        print("No Complete mod changed in this push; cumulative draft metadata is current.")
        verify(repo, release_id, head)
        return

    output_dir = Path(os.environ.get("RUNNER_TEMP", "/tmp")) / "pulseforge-complete-testing"
    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True)

    generated = legacy.package_changed_mods(changed, head, output_dir)
    generated.append(legacy.write_snapshot(descriptors, changed, head, output_dir))
    legacy.delete_stale_assets(repo, release_id, changed)
    for path in generated:
        upload_asset(repo, release_id, path)
    verify(repo, release_id, head)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--before")
    args = parser.parse_args()
    update(args.before)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
