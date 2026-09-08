#!/usr/bin/env python3
"""Transport PulseForge Complete compiled stages through draft release assets.

This intentionally avoids GitHub Actions artifacts: their repository quota can
reject otherwise-valid builds before Complete packaging starts.
"""
from __future__ import annotations

import argparse
import http.client
import json
import os
from pathlib import Path
import shutil
import sys
import tarfile
import tempfile
import urllib.error
import urllib.parse
import urllib.request

API_VERSION = "2022-11-28"
STAGE_PREFIX = "STAGE-"
CHUNK = 1024 * 1024


def token() -> str:
    value = os.environ.get("GH_TOKEN", "").strip()
    if not value:
        raise SystemExit("GH_TOKEN is required")
    return value


def api_headers(*, octet_stream: bool = False) -> dict[str, str]:
    return {
        "Accept": "application/octet-stream" if octet_stream else "application/vnd.github+json",
        "Authorization": f"Bearer {token()}",
        "X-GitHub-Api-Version": API_VERSION,
        "User-Agent": "PulseForge-Complete-stage-transport",
    }


def api_json(url: str, *, method: str = "GET"):
    request = urllib.request.Request(url, method=method, headers=api_headers())
    try:
        with urllib.request.urlopen(request, timeout=120) as response:
            raw = response.read()
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", "replace")
        raise SystemExit(f"GitHub API {method} {url} failed: HTTP {exc.code}: {body[:1000]}") from exc
    return json.loads(raw) if raw else None


def list_assets(repo: str, release_id: int) -> list[dict]:
    return api_json(
        f"https://api.github.com/repos/{repo}/releases/{release_id}/assets?per_page=100"
    )


def delete_asset(repo: str, asset_id: int) -> None:
    url = f"https://api.github.com/repos/{repo}/releases/assets/{asset_id}"
    request = urllib.request.Request(url, method="DELETE", headers=api_headers())
    try:
        with urllib.request.urlopen(request, timeout=120) as response:
            if response.status != 204:
                raise SystemExit(f"delete asset {asset_id}: expected HTTP 204, got {response.status}")
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", "replace")
        raise SystemExit(f"delete asset {asset_id} failed: HTTP {exc.code}: {body[:1000]}") from exc


def archive_stage(stage_dir: Path, archive: Path) -> None:
    if not stage_dir.is_dir():
        raise SystemExit(f"missing stage directory: {stage_dir}")
    marker = stage_dir / "PULSEFORGE_SOURCE_SHA.txt"
    if not marker.is_file():
        raise SystemExit(f"missing stage source marker: {marker}")
    archive.parent.mkdir(parents=True, exist_ok=True)
    archive.unlink(missing_ok=True)
    with tarfile.open(archive, "w:gz", compresslevel=1, dereference=False) as output:
        for child in sorted(stage_dir.iterdir(), key=lambda p: p.name.casefold()):
            output.add(child, arcname=child.name, recursive=True)
    if archive.stat().st_size <= 0:
        raise SystemExit(f"empty stage archive: {archive}")


def upload_archive(repo: str, release_id: int, archive: Path) -> None:
    name = archive.name
    for asset in list_assets(repo, release_id):
        if asset.get("name") == name:
            delete_asset(repo, int(asset["id"]))

    path = (
        f"/repos/{repo}/releases/{release_id}/assets?"
        + urllib.parse.urlencode({"name": name})
    )
    connection = http.client.HTTPSConnection("uploads.github.com", timeout=300)
    connection.putrequest("POST", path)
    for key, value in api_headers().items():
        connection.putheader(key, value)
    connection.putheader("Content-Type", "application/octet-stream")
    connection.putheader("Content-Length", str(archive.stat().st_size))
    connection.endheaders()
    with archive.open("rb") as source:
        while chunk := source.read(CHUNK):
            connection.send(chunk)
    response = connection.getresponse()
    body = response.read()
    connection.close()
    if response.status != 201:
        raise SystemExit(
            f"stage upload failed for {name}: HTTP {response.status}: "
            + body.decode("utf-8", "replace")[:1000]
        )
    metadata = json.loads(body)
    if metadata.get("name") != name:
        raise SystemExit(f"uploaded asset name mismatch: {metadata.get('name')!r} != {name!r}")
    print(f"Uploaded release stage: {name} ({archive.stat().st_size} bytes)")


def download_asset(repo: str, asset_id: int, destination: Path) -> None:
    url = f"https://api.github.com/repos/{repo}/releases/assets/{asset_id}"
    request = urllib.request.Request(url, headers=api_headers(octet_stream=True))
    try:
        with urllib.request.urlopen(request, timeout=300) as response, destination.open("wb") as out:
            shutil.copyfileobj(response, out, length=CHUNK)
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", "replace")
        raise SystemExit(f"download asset {asset_id} failed: HTTP {exc.code}: {body[:1000]}") from exc
    if not destination.is_file() or destination.stat().st_size <= 0:
        raise SystemExit(f"downloaded stage is empty: {destination}")


def safe_extract(archive: Path, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, "r:gz") as source:
        try:
            source.extractall(destination, filter="data")
        except TypeError:  # Python < 3.12 fallback; runners are currently newer.
            source.extractall(destination)


def upload(args: argparse.Namespace) -> None:
    expected_sha = args.sha
    stage_dir = Path(args.stage_dir).resolve()
    marker = (stage_dir / "PULSEFORGE_SOURCE_SHA.txt").read_text(
        encoding="utf-8", errors="replace"
    )
    if expected_sha not in marker:
        raise SystemExit(f"stage source marker does not contain {expected_sha}")
    name = f"{STAGE_PREFIX}{expected_sha}--complete-engine-stage-{args.stage_key}.tar.gz"
    archive = Path(args.archive_dir).resolve() / name
    archive_stage(stage_dir, archive)
    upload_archive(args.repo, args.release_id, archive)


def download_all(args: argparse.Namespace) -> None:
    expected_sha = args.sha
    assets = {asset["name"]: asset for asset in list_assets(args.repo, args.release_id)}
    archive_dir = Path(args.archive_dir).resolve()
    output_root = Path(args.output_root).resolve()
    shutil.rmtree(archive_dir, ignore_errors=True)
    shutil.rmtree(output_root, ignore_errors=True)
    archive_dir.mkdir(parents=True, exist_ok=True)
    output_root.mkdir(parents=True, exist_ok=True)

    keys = [item.strip() for item in args.stage_keys.split(",") if item.strip()]
    if not keys:
        raise SystemExit("no stage keys supplied")
    for key in keys:
        name = f"{STAGE_PREFIX}{expected_sha}--complete-engine-stage-{key}.tar.gz"
        asset = assets.get(name)
        if asset is None:
            raise SystemExit(f"missing release stage asset: {name}")
        archive = archive_dir / name
        download_asset(args.repo, int(asset["id"]), archive)
        destination = output_root / f"complete-engine-stage-{key}"
        safe_extract(archive, destination)
        marker = destination / "PULSEFORGE_SOURCE_SHA.txt"
        if not marker.is_file() or expected_sha not in marker.read_text(
            encoding="utf-8", errors="replace"
        ):
            raise SystemExit(f"stage {key} source marker does not match {expected_sha}")
        print(f"Recovered release stage: {key}")


def self_test() -> None:
    with tempfile.TemporaryDirectory() as raw:
        root = Path(raw)
        stage = root / "stage"
        stage.mkdir()
        (stage / "PULSEFORGE_SOURCE_SHA.txt").write_text("abc123\n", encoding="utf-8")
        executable = stage / "pulseforge"
        executable.write_text("binary-placeholder\n", encoding="utf-8")
        executable.chmod(0o755)
        nested = stage / "nested"
        nested.mkdir()
        (nested / "data.txt").write_text("data\n", encoding="utf-8")
        if os.name != "nt":
            (stage / "data-link").symlink_to("nested/data.txt")
        archive = root / "stage.tar.gz"
        archive_stage(stage, archive)
        extracted = root / "extracted"
        safe_extract(archive, extracted)
        assert (extracted / "PULSEFORGE_SOURCE_SHA.txt").read_text() == "abc123\n"
        assert (extracted / "nested/data.txt").read_text() == "data\n"
        if os.name != "nt":
            assert (extracted / "data-link").is_symlink()
            assert os.access(extracted / "pulseforge", os.X_OK)
    print("Complete release stage transport self-test: PASS")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    sub = result.add_subparsers(dest="command", required=True)
    sub.add_parser("self-test")

    up = sub.add_parser("upload")
    up.add_argument("--repo", required=True)
    up.add_argument("--release-id", required=True, type=int)
    up.add_argument("--sha", required=True)
    up.add_argument("--stage-key", required=True)
    up.add_argument("--stage-dir", required=True)
    up.add_argument("--archive-dir", required=True)

    down = sub.add_parser("download-all")
    down.add_argument("--repo", required=True)
    down.add_argument("--release-id", required=True, type=int)
    down.add_argument("--sha", required=True)
    down.add_argument("--stage-keys", required=True)
    down.add_argument("--archive-dir", required=True)
    down.add_argument("--output-root", required=True)
    return result


def main() -> int:
    args = parser().parse_args()
    if args.command == "self-test":
        self_test()
    elif args.command == "upload":
        upload(args)
    elif args.command == "download-all":
        download_all(args)
    else:
        raise AssertionError(args.command)
    return 0


if __name__ == "__main__":
    sys.exit(main())
