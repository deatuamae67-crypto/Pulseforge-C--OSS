#!/usr/bin/env python3
"""Download one public Google Drive folder with official gdown, in parallel.

The official gdown folder command is authoritative for recursive enumeration.
This helper asks gdown 6.1+ for its JSON manifest and then downloads those same
resolved file URLs concurrently with separate gdown processes. It never trusts
Drive paths blindly and never writes outside the caller-provided staging root.

Completed downloads may be resumed across fresh runners by persisting a small
success ledger alongside the staging directory. A cached file is skipped only
when its path and resolved URL still match the current authoritative manifest
and the file is still present on disk.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
from pathlib import Path, PurePosixPath
import subprocess
import sys
import tempfile
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
    result = subprocess.run(cmd, check=True, text=True, capture_output=True, timeout=300)
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


def load_completed_state(
    state_path: Path,
    folder_url: str,
    staging: Path,
    items: list[tuple[str, Path]],
) -> set[str]:
    if not state_path.is_file():
        return set()
    try:
        payload = json.loads(state_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"warning: ignoring unreadable download state {state_path}: {exc}", file=sys.stderr)
        return set()
    if not isinstance(payload, dict):
        print(f"warning: ignoring non-object download state {state_path}", file=sys.stderr)
        return set()
    if payload.get("schema_version") != 1 or payload.get("folder_url") != folder_url:
        print(f"warning: ignoring stale/incompatible download state {state_path}", file=sys.stderr)
        return set()

    expected = {rel.as_posix(): url for url, rel in items}
    entries = payload.get("completed")
    if not isinstance(entries, list):
        print(f"warning: ignoring download state with invalid completed list {state_path}", file=sys.stderr)
        return set()

    completed: set[str] = set()
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        raw_path = entry.get("path")
        url = entry.get("url")
        if not isinstance(raw_path, str) or expected.get(raw_path) != url:
            continue
        try:
            rel = safe_relative_path(raw_path)
        except ValueError:
            continue
        if (staging / rel).is_file():
            completed.add(raw_path)
    return completed


def write_completed_state(
    state_path: Path,
    folder_url: str,
    items: list[tuple[str, Path]],
    completed: set[str],
) -> None:
    state_path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "schema_version": 1,
        "folder_url": folder_url,
        "completed": [
            {"url": url, "path": rel.as_posix()}
            for url, rel in items
            if rel.as_posix() in completed
        ],
    }
    tmp = state_path.with_name(state_path.name + ".tmp")
    tmp.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    tmp.replace(state_path)


def download_one(
    python: str,
    url: str,
    output: Path,
    log_dir: Path,
    file_timeout: float,
) -> tuple[bool, str]:
    output.parent.mkdir(parents=True, exist_ok=True)
    log_dir.mkdir(parents=True, exist_ok=True)
    safe_name = f"{abs(hash(output.as_posix())):x}.log"
    log_path = log_dir / safe_name
    cmd = [python, "-m", "gdown", url, "--continue", "--quiet", "-O", str(output)]
    try:
        with log_path.open("ab") as log:
            proc = subprocess.run(
                cmd,
                stdout=log,
                stderr=subprocess.STDOUT,
                timeout=file_timeout,
            )
    except subprocess.TimeoutExpired:
        with log_path.open("ab") as log:
            log.write(f"\nPulseForge: gdown timed out after {file_timeout:.0f}s\n".encode("utf-8"))
        return False, f"{output} (timed out after {file_timeout:.0f}s; log {log_path})"
    if proc.returncode == 0 and output.is_file():
        return True, ""
    return False, f"{output} (exit {proc.returncode}; log {log_path})"


def run_download(
    python: str,
    folder_url: str,
    staging: Path,
    state_path: Path,
    workers: int,
    attempts: int,
    backoff: float,
    diagnostics: Path,
    file_timeout: float,
) -> None:
    staging.mkdir(parents=True, exist_ok=True)
    diagnostics.mkdir(parents=True, exist_ok=True)
    items = enumerate_folder(python, folder_url)
    write_manifest(items, diagnostics / "gdown-folder-manifest.json")
    print(f"gdown manifest entries: {len(items)}")

    completed = load_completed_state(state_path, folder_url, staging, items)
    restored_files = len(completed)
    pending = [(url, rel) for url, rel in items if rel.as_posix() not in completed]
    print(f"restored completed files: {restored_files}; pending: {len(pending)}")
    write_completed_state(state_path, folder_url, items, completed)

    for attempt in range(1, attempts + 1):
        if not pending:
            break
        failures: list[tuple[str, Path]] = []
        print(f"download attempt {attempt}/{attempts}: {len(pending)} pending file(s)")
        with ThreadPoolExecutor(max_workers=workers, thread_name_prefix="gdown") as pool:
            futures = {
                pool.submit(
                    download_one,
                    python,
                    url,
                    staging / rel,
                    diagnostics / "files",
                    file_timeout,
                ): (url, rel)
                for url, rel in pending
            }
            processed = 0
            success_this_attempt = 0
            for future in as_completed(futures):
                url, rel = futures[future]
                try:
                    ok, message = future.result()
                except Exception as exc:
                    ok, message = False, f"{rel}: {type(exc).__name__}: {exc}"
                processed += 1
                if ok:
                    completed.add(rel.as_posix())
                    success_this_attempt += 1
                    write_completed_state(state_path, folder_url, items, completed)
                    if processed % 50 == 0 or processed == len(futures):
                        print(
                            f"  processed {processed}/{len(futures)}; "
                            f"completed total {len(completed)}/{len(items)}"
                        )
                else:
                    print(f"  failed: {message}", file=sys.stderr)
                    failures.append((url, rel))
        print(
            f"attempt {attempt} added {success_this_attempt} completed file(s); "
            f"{len(failures)} remain"
        )
        if not failures:
            pending = []
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
        write_completed_state(state_path, folder_url, items, completed)
        raise RuntimeError(
            f"{len(pending)} Drive file(s) still failed; "
            f"{len(completed)} completed file(s) were preserved in {state_path}; "
            f"see {failed_path}"
        )

    missing = [rel.as_posix() for _, rel in items if not (staging / rel).is_file()]
    if missing:
        raise RuntimeError(f"download reported success but {len(missing)} manifest file(s) are missing")
    write_completed_state(state_path, folder_url, items, {rel.as_posix() for _, rel in items})
    files = sum(1 for p in staging.rglob("*") if p.is_file())
    total = sum(p.stat().st_size for p in staging.rglob("*") if p.is_file())
    summary = {
        "schema_version": 1,
        "folder_url": folder_url,
        "manifest_files": len(items),
        "restored_files": restored_files,
        "materialized_files": files,
        "materialized_bytes": total,
        "workers": workers,
        "attempts": attempts,
        "file_timeout_seconds": file_timeout,
    }
    (diagnostics / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"materialized: {files} files, {total} bytes")


def self_test() -> None:
    good = {
        "a.txt": "a.txt",
        "nested/b.txt": "nested/b.txt",
        "unicode/áudio.ogg": "unicode/áudio.ogg",
        "./x": "x",
    }
    for raw, expected in good.items():
        assert safe_relative_path(raw).as_posix() == expected
    for raw in ("", "/etc/passwd", "../x", "a/../../x", "C:/x"):
        try:
            safe_relative_path(raw)
        except ValueError:
            pass
        else:
            raise AssertionError(f"unsafe path accepted: {raw!r}")

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        staging = root / "staging"
        state = root / "state.json"
        staging.mkdir()
        items = [
            ("https://drive.google.com/uc?id=one", Path("a.txt")),
            ("https://drive.google.com/uc?id=two", Path("nested/b.txt")),
        ]
        (staging / "a.txt").write_text("ok", encoding="utf-8")
        completed = {"a.txt"}
        write_completed_state(state, "https://drive.google.com/drive/folders/test", items, completed)
        restored = load_completed_state(
            state,
            "https://drive.google.com/drive/folders/test",
            staging,
            items,
        )
        assert restored == {"a.txt"}
        stale_items = [
            ("https://drive.google.com/uc?id=changed", Path("a.txt")),
            items[1],
        ]
        assert load_completed_state(
            state,
            "https://drive.google.com/drive/folders/test",
            staging,
            stale_items,
        ) == set()
    print("parallel gdown helper self-test passed")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--python", default=sys.executable, help="Python executable containing the pinned gdown")
    parser.add_argument("--folder-url")
    parser.add_argument("--staging", type=Path)
    parser.add_argument("--state", type=Path)
    parser.add_argument("--diagnostics", type=Path)
    parser.add_argument("--workers", type=int, default=3)
    parser.add_argument("--attempts", type=int, default=5)
    parser.add_argument("--backoff-seconds", type=float, default=20.0)
    parser.add_argument("--file-timeout-seconds", type=float, default=300.0)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if (
        not args.folder_url
        or args.staging is None
        or args.state is None
        or args.diagnostics is None
    ):
        parser.error("--folder-url, --staging, --state and --diagnostics are required")
    if not 1 <= args.workers <= 8:
        parser.error("--workers must be 1..8")
    if not 1 <= args.attempts <= 10:
        parser.error("--attempts must be 1..10")
    if not 30 <= args.file_timeout_seconds <= 3600:
        parser.error("--file-timeout-seconds must be 30..3600")
    run_download(
        args.python,
        args.folder_url,
        args.staging,
        args.state,
        args.workers,
        args.attempts,
        args.backoff_seconds,
        args.diagnostics,
        args.file_timeout_seconds,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
