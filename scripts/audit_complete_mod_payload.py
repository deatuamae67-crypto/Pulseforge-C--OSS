#!/usr/bin/env python3
"""PulseForge Complete payload gate entrypoint.

Keeps the v4 runtime/archival implementation intact while making static-archive
PR validation aware of canonical, unhydrated Git LFS JSON pointers. Runtime
payload auditing remains fail-closed and unchanged.
"""
from __future__ import annotations

import importlib.util
import json
import re
import tempfile
from pathlib import Path

_IMPL_PATH = Path(__file__).with_name("audit_complete_mod_payload_v4_impl.py")
_SPEC = importlib.util.spec_from_file_location("pulseforge_complete_audit_v4_impl", _IMPL_PATH)
if _SPEC is None or _SPEC.loader is None:
    raise RuntimeError("cannot load Complete payload audit v4 implementation")
impl = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(impl)

_LFS_MARKER = "version https://git-lfs.github.com/spec/v1\n"
_LFS_POINTER_RE = re.compile(
    r"\Aversion https://git-lfs.github.com/spec/v1\n"
    r"oid sha256:[0-9a-f]{64}\n"
    r"size ([0-9]+)\n?\Z"
)


def _lfs_pointer_size(path: Path) -> int | None:
    """Return LFS object size, None for an ordinary file, or -1 if malformed."""
    try:
        if path.stat().st_size > 1024:
            return None
        text = path.read_text(encoding="utf-8", errors="strict")
    except (OSError, UnicodeError):
        return None
    if not text.startswith(_LFS_MARKER):
        return None
    match = _LFS_POINTER_RE.fullmatch(text)
    if match is None:
        return -1
    return int(match.group(1))


def _static_archive_report(project, mod, desc, override, files, total, errors):
    basis = override.get("audit_mode_basis")
    if not isinstance(basis, str) or not basis.strip():
        errors.append("static_archive audit mode requires a non-empty audit_mode_basis")

    warnings: list[str] = []
    unreadable: list[str] = []
    lfs_pointers = 0

    for path in files:
        if path.suffix.lower() != ".json":
            continue
        rel = path.relative_to(mod).as_posix()
        pointer_size = _lfs_pointer_size(path)
        if pointer_size == -1:
            errors.append(f"static archive JSON has malformed Git LFS pointer: {rel}")
            continue
        if pointer_size is not None:
            lfs_pointers += 1
            continue

        try:
            size = path.stat().st_size
        except OSError:
            errors.append(f"cannot stat JSON file in static archive: {rel}")
            continue
        if size > 16 * 1024 * 1024:
            warnings.append(f"{rel}: JSON exceeds bounded structural parse limit")
            continue
        try:
            json.loads(path.read_text(encoding="utf-8", errors="strict"))
        except Exception:
            unreadable.append(rel)

    if unreadable:
        errors.extend(f"static archive JSON is invalid: {path}" for path in unreadable)
    if lfs_pointers:
        warnings.append(
            f"{lfs_pointers} static archive JSON files are canonical unhydrated Git LFS pointers; "
            "their object identity/size remain content-addressed while JSON parsing is deferred to hydrated staging audits"
        )
    if isinstance(basis, str) and basis.strip():
        warnings.append(
            "runtime dependency closure intentionally skipped for verified static archive: "
            + basis.strip()
        )

    report = {
        "format": "pulseforge-complete-mod-audit-v4",
        "slug": desc.get("slug"),
        "auditMode": "static_archive",
        "files": len(files),
        "bytes": total,
        "materializedMinimumBytes": int(
            override.get("materialized_min_bytes", desc.get("min_bytes", 0))
        ),
        "materializationOverride": bool(override),
        "charts": [],
        "archivalCharts": [],
        "charactersChecked": [],
        "luaReferencesChecked": 0,
        "lfsPointersChecked": lfs_pointers,
        "warnings": warnings,
        "errors": errors,
        "ok": not errors,
    }
    return report, errors


_original_selftest = impl.selftest
impl._static_archive_report = _static_archive_report


def selftest() -> int:
    if _original_selftest() != 0:
        return 1

    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        mod = root / "mods" / "archive-lfs"
        mod.mkdir(parents=True)
        pointer = (
            "version https://git-lfs.github.com/spec/v1\n"
            + "oid sha256:" + ("0" * 64) + "\n"
            + "size 123\n"
        )
        (mod / "data.json").write_text(pointer, encoding="utf-8")
        registry = root / "docs" / "complete"
        registry.mkdir(parents=True)
        registry.joinpath("materialization_overrides.json").write_text(
            json.dumps({
                "schema_version": 1,
                "overrides": {
                    "archive-lfs": {
                        "source_min_bytes": 123,
                        "materialized_min_bytes": 123,
                        "audit_mode": "static_archive",
                        "audit_mode_basis": "self-test unhydrated LFS archive",
                    }
                },
            }),
            encoding="utf-8",
        )
        descriptor = {
            "slug": "archive-lfs",
            "min_files": 1,
            "min_bytes": 123,
        }
        report, errors = impl.audit(root, mod, descriptor)
        if errors or report.get("lfsPointersChecked") != 1:
            return 1

        (mod / "data.json").write_text(
            "version https://git-lfs.github.com/spec/v1\n"
            "oid sha256:not-a-valid-object-id\n"
            "size 123\n",
            encoding="utf-8",
        )
        _, errors = impl.audit(root, mod, descriptor)
        if not errors:
            return 1

    return 0


impl.selftest = selftest

audit = impl.audit
main = impl.main
core = impl.core

if __name__ == "__main__":
    raise SystemExit(impl.main())
