#!/usr/bin/env python3
"""Download one public Google Drive folder with official gdown, in parallel.

The official gdown folder command is authoritative for recursive enumeration.
This helper asks gdown 6.1+ for its JSON manifest and then downloads those same
resolved file URLs concurrently with separate gdown processes.  It never trusts
Drive paths blindly and never writes outside the caller-provided staging root.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import sys
import time


def safe_relative_path(raw: object) -> Path:
    if not isinstance(raw, str) or not raw:
        raise ValueError(f"invalid gdown manifest path: {raw!r}")
    posix = PurePosixPath(raw.replace("\\", "/"))
    if posix.is_absolute() or not posix.parts:
        raise ValueError(f"absolute/empty gdown path refused: {raw!r}")
    if any(part in ("", ".", "..") for part in posix.parts):
        raise ValueError(f"unsafe gdown path refused: {raw!r}")
    if ":" in posix.parts[0]:
        raise ValueError(f"drive-qualified gdown path refused: {raw!r}")
    return Path(*posix.parts)


def enumerate_folder(python: str, folder_url: str) -> list[tuple[str, Path]]:
    cmd = [python, "-m", "gdown", folder_url, "--folder", "--json", "--quiet"]
    result = subprocess.run(cmd, check=True, text=True, capture_output=True)
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"gdown JSON manifest was invalid: {exc}") from exc
    if not isinstance(payload, list) or not payload:
        raise RuntimeError("gdown returned an empty/non-list folder manifest")

    items: list[tuple[str, Path]] = []
    exact: set[str] = set()
    folded: set[str] = set()
    for entry in payload:
        if not isinstance(entry, dict):
            raise RuntimeError(f"invalid gdown manifest entry: {entry!r}")
        url = entry.get("url")
        if not isinstance(url, str) or not url.startswith("https://"):
            raise RuntimeError(f"invalid gdown manifest URL: {url!r}")
        rel = safe_relative_path(entry.get("path"))
        key = rel.as_posix()
        folded_key = key.casefold()
        if key in exact:
            raise RuntimeError(f"duplicate gdown manifest path: {key}")
        if folded_key in folded:
            raise RuntimeError(f"case-colliding gdown manifest path: {key}")
        exact.add(key)
        folded.add(folded_key)
        items.append((url, rel))
    return items


def write_manifest(items: list[tuple[str, Path]], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(
            [{"url": url, "path": rel.as_posix()} for url, rel in items],
            indent=2,
            ensure_ascii=False,
        )
        + "\n",
        encoding="utf-8",
    )


def download_one(python: str, url: str, output: Path, log_dir: Path) -> tuple[bool, str]:
    output.parent.mkdir(parents=True, exist_ok=True)
    log_dir.mkdir(parents=True, exist_ok=True)
    safe_name = f"{abs(hash(output.as_posix())):x}.log"
    log_path = log_dir / safe_name
    cmd = [python, "-m", "gdown", url, "--continue", "--quiet", "-O", str(output)]
    with log_path.open("ab") as log:
        proc = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT)
    if proc.returncode == 0 and output.is_file():
        return True, ""
    return False, f"{output} (exit {proc.returncode}; log {log_path})"


def run_download(
    python: str,
    folder_url: str,
    staging: Path,
    workers: int,
    attempts: int,
    backoff: float,
    diagnostics: Path,
) -> None:
    staging.mkdir(parents=True, exist_ok=True)
    diagnostics.mkdir(parents=True, exist_ok=True)
    items = enumerate_folder(python, folder_url)
    write_manifest(items, diagnostics / "gdown-folder-manifest.json")
    print(f"gdown manifest entries: {len(items)}")

    pending = list(items)
    for attempt in range(1, attempts + 1):
        failures: list[tuple[str, Path]] = []
        print(f"download attempt {attempt}/{attempts}: {len(pending)} pending file(s)")
        with ThreadPoolExecutor(max_workers=workers, thread_name_prefix="gdown") as pool:
            futures = {
                pool.submit(download_one, python, url, staging / rel, diagnostics / "files"): (url, rel)
                for url, rel in pending
            }
            completed = 0
            for future in as_completed(futures):
                url, rel = futures[future]
                try:
                    ok, message = future.result()
                except Exception as exc:  # keep all other transfers alive
                    ok, message = False, f"{rel}: {type(exc).__name__}: {exc}"
                completed += 1
                if ok:
                    if completed % 50 == 0 or completed == len(futures):
                        print(f"  completed {completed}/{len(futures)}")
                else:
                    print(f"  failed: {message}", file=sys.stderr)
                    failures.append((url, rel))
        if not failures:
            break
        pending = failures
        if attempt != attempts:
            delay = backoff * attempt
            print(f"retrying {len(pending)} failed file(s) after {delay:.0f}s", file=sys.stderr)
            time.sleep(delay)
    else:
        failed_path = diagnostics / "failed-files.json"
        failed_path.write_text(
            json.dumps([{"url": u, "path": p.as_posix()} for u, p in pending], indent=2) + "\n",
            encoding="utf-8",
        )
        raise RuntimeError(f"{len(pending)} Drive file(s) still failed; see {failed_path}")

    missing = [rel.as_posix() for _, rel in items if not (staging / rel).is_file()]
    if missing:
        raise RuntimeError(f"download reported success but {len(missing)} manifest file(s) are missing")
    files = sum(1 for p in staging.rglob("*") if p.is_file())
    total = sum(p.stat().st_size for p in staging.rglob("*") if p.is_file())
    summary = {
        "schema_version": 1,
        "folder_url": folder_url,
        "manifest_files": len(items),
        "materialized_files": files,
        "materialized_bytes": total,
        "workers": workers,
        "attempts": attempts,
    }
    (diagnostics / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"materialized: {files} files, {total} bytes")


def self_test() -> None:
    good = {
        "a.txt": "a.txt",
        "nested/b.txt": "nested/b.txt",
        "unicode/áudio.ogg": "unicode/áudio.ogg",
    }
    for raw, expected in good.items():
        assert safe_relative_path(raw).as_posix() == expected
    for raw in ("", "/etc/passwd", "../x", "a/../../x", "C:/x", "./x"):
        try:
            safe_relative_path(raw)
        except ValueError:
            pass
        else:
            raise AssertionError(f"unsafe path accepted: {raw!r}")
    print("parallel gdown helper self-test passed")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--python", default=sys.executable, help="Python executable containing the pinned gdown")
    parser.add_argument("--folder-url")
    parser.add_argument("--staging", type=Path)
    parser.add_argument("--diagnostics", type=Path)
    parser.add_argument("--workers", type=int, default=3)
    parser.add_argument("--attempts", type=int, default=5)
    parser.add_argument("--backoff-seconds", type=float, default=20.0)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if not args.folder_url or args.staging is None or args.diagnostics is None:
        parser.error("--folder-url, --staging and --diagnostics are required")
    if not 1 <= args.workers <= 8:
        parser.error("--workers must be 1..8")
    if not 1 <= args.attempts <= 10:
        parser.error("--attempts must be 1..10")
    run_download(
        args.python,
        args.folder_url,
        args.staging,
        args.workers,
        args.attempts,
        args.backoff_seconds,
        args.diagnostics,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
