#!/usr/bin/env python3
"""PulseForge Complete payload gate wrapper.

Keeps the v3 functional-closure auditor fail-closed for runtime content while
allowing narrowly documented archival content and incomplete chart/audio pairs
to remain distributable without pretending that missing source assets exist.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import tempfile
from pathlib import Path

_CORE_PATH = Path(__file__).with_name("audit_complete_mod_payload_core.py")
_SPEC = importlib.util.spec_from_file_location("pulseforge_complete_audit_core", _CORE_PATH)
if _SPEC is None or _SPEC.loader is None:
    raise RuntimeError("cannot load Complete payload audit core")
core = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(core)


def _safe_relative(value: object) -> str | None:
    if not isinstance(value, str) or not value.strip():
        return None
    raw = value.replace("\\", "/").strip().strip("/")
    path = Path(raw)
    if path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
        return None
    return path.as_posix()


def _override(project: Path, desc: dict, errors: list[str]) -> dict:
    return core.materialization_override(project, desc, errors)


def _structure(project: Path, mod: Path, desc: dict, override: dict) -> tuple[list[Path], int, list[str]]:
    errors: list[str] = []
    if not mod.is_dir():
        return [], 0, [f"missing mod root: {mod}"]
    files = [p for p in mod.rglob("*") if p.is_file()]
    if len(files) > 600000:
        return files, 0, ["payload exceeds 600000-file audit bound"]
    total = sum(core.lsize(p) for p in files)
    try:
        materialized_min = int(override.get("materialized_min_bytes", desc.get("min_bytes", 0)))
        minimum_files = int(desc.get("min_files", 0))
    except (TypeError, ValueError):
        return files, total, ["Complete structural thresholds are invalid"]
    if len(files) < minimum_files:
        errors.append(f"{len(files)} files < descriptor minimum {minimum_files}")
    if total < materialized_min:
        errors.append(f"{total} bytes < materialized descriptor minimum {materialized_min}")
    return files, total, errors


def _validated_paths(mod: Path, override: dict, key: str, errors: list[str]) -> list[str]:
    raw = override.get(key, [])
    if raw is None:
        return []
    if not isinstance(raw, list):
        errors.append(f"Complete materialization override {key} must be an array")
        return []
    result: list[str] = []
    for value in raw:
        rel = _safe_relative(value)
        if rel is None:
            errors.append(f"Complete materialization override {key} contains an unsafe path")
            continue
        if rel in result:
            errors.append(f"Complete materialization override {key} contains duplicate path {rel}")
            continue
        path = mod / rel
        if not path.is_file():
            errors.append(f"Complete materialization override {key} path is missing: {rel}")
            continue
        result.append(rel)
    return result


def _allow_partial_chart_audio(report: dict, errors: list[str]) -> list[str]:
    """Downgrade only missing chart/audio counterparts to explicit warnings.

    Complete staging mirrors the authoritative source as-is. A chart whose
    Inst/Voices asset is absent, or a chart-only recovery payload, is therefore
    publishable and remains visibly incomplete for later asset restoration.
    All unrelated dependency and structural failures remain blocking.
    """
    remaining: list[str] = []
    partial: list[str] = []
    for error in errors:
        if (
            ": missing Inst for '" in error
            or "needsVoices=true but Voices missing for '" in error
            or error == "chart-bearing payload is metadata/charts only; functional assets are missing"
        ):
            partial.append(error)
        else:
            remaining.append(error)
    if partial:
        warnings = list(report.get("warnings", []))
        warnings.extend(
            error + " (partial authoritative source accepted; missing counterpart may be supplied later)"
            for error in partial
        )
        report["warnings"] = warnings
    report["partialChartAudioGaps"] = partial
    report["errors"] = remaining
    report["ok"] = not remaining
    return remaining


def _static_archive_report(project: Path, mod: Path, desc: dict, override: dict, files: list[Path], total: int, errors: list[str]) -> tuple[dict, list[str]]:
    basis = override.get("audit_mode_basis")
    if not isinstance(basis, str) or not basis.strip():
        errors.append("static_archive audit mode requires a non-empty audit_mode_basis")
    warnings: list[str] = []
    unreadable: list[str] = []
    for path in files:
        if path.suffix.lower() != ".json":
            continue
        try:
            size = path.stat().st_size
        except OSError:
            errors.append(f"cannot stat JSON file in static archive: {path.relative_to(mod)}")
            continue
        if size > 16 * 1024 * 1024:
            warnings.append(f"{path.relative_to(mod)}: JSON exceeds bounded structural parse limit")
            continue
        try:
            json.loads(path.read_text(encoding="utf-8", errors="strict"))
        except Exception:
            unreadable.append(path.relative_to(mod).as_posix())
    if unreadable:
        errors.extend(f"static archive JSON is invalid: {path}" for path in unreadable)
    if isinstance(basis, str) and basis.strip():
        warnings.append("runtime dependency closure intentionally skipped for verified static archive: " + basis.strip())
    report = {
        "format": "pulseforge-complete-mod-audit-v4",
        "slug": desc.get("slug"),
        "auditMode": "static_archive",
        "files": len(files),
        "bytes": total,
        "materializedMinimumBytes": int(override.get("materialized_min_bytes", desc.get("min_bytes", 0))),
        "materializationOverride": bool(override),
        "charts": [],
        "archivalCharts": [],
        "charactersChecked": [],
        "luaReferencesChecked": 0,
        "warnings": warnings,
        "errors": errors,
        "ok": not errors,
    }
    return report, errors


def audit(project: Path, mod: Path, desc: dict) -> tuple[dict, list[str]]:
    project = project.resolve()
    mod = mod.resolve()
    registry_errors: list[str] = []
    override = _override(project, desc, registry_errors)
    files, total, structural_errors = _structure(project, mod, desc, override)
    errors = registry_errors + structural_errors

    mode = override.get("audit_mode", "runtime")
    if mode not in ("runtime", "static_archive"):
        errors.append(f"unsupported Complete audit_mode: {mode!r}")
    if mode == "static_archive":
        return _static_archive_report(project, mod, desc, override, files, total, errors)

    archival = _validated_paths(mod, override, "archival_charts", errors)
    folder_fallback = _validated_paths(mod, override, "chart_folder_audio_fallback", errors)
    if archival and not isinstance(override.get("archival_charts_basis"), str):
        errors.append("archival_charts requires archival_charts_basis")
    if folder_fallback and not isinstance(override.get("chart_folder_audio_fallback_basis"), str):
        errors.append("chart_folder_audio_fallback requires chart_folder_audio_fallback_basis")

    # Fast path: ordinary runtime mods retain v3 dependency semantics, except
    # that missing chart/audio counterparts are accepted as partial source.
    if not archival and not folder_fallback:
        report, core_errors = core.audit(project, mod, desc)
        core_errors = _allow_partial_chart_audio(report, list(core_errors))
        if errors:
            combined = list(core_errors) + errors
            report["errors"] = combined
            report["ok"] = False
            return report, combined
        report["format"] = "pulseforge-complete-mod-audit-v4"
        report["auditMode"] = "runtime"
        return report, core_errors

    saved: dict[str, bytes] = {}
    moved: list[tuple[Path, Path]] = []
    temp = tempfile.TemporaryDirectory()
    temp_root = Path(temp.name)
    try:
        # Historical recovery charts remain byte-for-byte in the pack, but are
        # removed only from the runtime dependency-closure view.
        for rel in archival:
            source = mod / rel
            destination = temp_root / "archival" / rel
            destination.parent.mkdir(parents=True, exist_ok=True)
            os.replace(source, destination)
            moved.append((source, destination))

        # Some recovered Psych charts carry a stale internal song id while the
        # runtime deliberately falls back to the chart folder for conventional
        # audio. Normalize only the temporary audit view to that runtime rule.
        for rel in folder_fallback:
            path = mod / rel
            saved[rel] = path.read_bytes()
            root = json.loads(saved[rel].decode("utf-8"))
            song = root.get("song", root) if isinstance(root, dict) else None
            if not isinstance(song, dict):
                errors.append(f"chart_folder_audio_fallback target has no song object: {rel}")
                continue
            song["song"] = path.parent.name
            path.write_text(json.dumps(root, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")

        reduced = dict(desc)
        reduced["slug"] = "__runtime_subset__"
        reduced["min_files"] = 0
        reduced["min_bytes"] = 0
        reduced["materialized_min_bytes"] = 0
        reduced["known_missing_voices"] = override.get("known_missing_voices", [])
        report, core_errors = core.audit(project, mod, reduced)
    finally:
        for rel, data in saved.items():
            (mod / rel).write_bytes(data)
        for source, destination in reversed(moved):
            source.parent.mkdir(parents=True, exist_ok=True)
            os.replace(destination, source)
        temp.cleanup()

    core_errors = _allow_partial_chart_audio(report, list(core_errors))
    warnings = list(report.get("warnings", []))
    if archival:
        warnings.append(
            "runtime dependency closure skipped only for explicitly archived recovery charts: "
            + ", ".join(archival)
        )
    if folder_fallback:
        warnings.append(
            "runtime chart-folder audio fallback mirrored for audit only: "
            + ", ".join(folder_fallback)
        )
    combined_errors = errors + list(core_errors)
    report.update({
        "format": "pulseforge-complete-mod-audit-v4",
        "slug": desc.get("slug"),
        "auditMode": "runtime",
        "files": len(files),
        "bytes": total,
        "materializedMinimumBytes": int(override.get("materialized_min_bytes", desc.get("min_bytes", 0))),
        "materializationOverride": bool(override),
        "archivalCharts": archival,
        "warnings": warnings,
        "errors": combined_errors,
        "ok": not combined_errors,
    })
    return report, combined_errors


def selftest() -> int:
    if core.selftest() != 0:
        return 1
    policy_report = {
        "warnings": [],
        "errors": [
            "mods/x/data/foo/foo.json: missing Inst for 'foo'",
            "mods/x/data/bar/bar.json: needsVoices=true but Voices missing for 'bar'",
            "chart-bearing payload is metadata/charts only; functional assets are missing",
            "custom stage 'broken' missing",
        ],
        "ok": False,
    }
    policy_errors = _allow_partial_chart_audio(policy_report, list(policy_report["errors"]))
    if policy_errors != ["custom stage 'broken' missing"]:
        return 1
    if len(policy_report.get("partialChartAudioGaps", [])) != 3:
        return 1
    if len(policy_report.get("warnings", [])) != 3:
        return 1
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        mod = root / "mods" / "archive"
        mod.mkdir(parents=True)
        (mod / "data.json").write_text('{"ok":true}', encoding="utf-8")
        registry = root / "docs" / "complete"
        registry.mkdir(parents=True)
        registry.joinpath("materialization_overrides.json").write_text(json.dumps({
            "schema_version": 1,
            "overrides": {
                "archive": {
                    "source_min_bytes": 1,
                    "materialized_min_bytes": 1,
                    "audit_mode": "static_archive",
                    "audit_mode_basis": "self-test archive"
                }
            }
        }), encoding="utf-8")
        report, errors = audit(root, mod, {"slug": "archive", "min_files": 1, "min_bytes": 1})
        if errors or report.get("auditMode") != "static_archive":
            return 1
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", nargs="?", default=".")
    parser.add_argument("--mod-root")
    parser.add_argument("--descriptor")
    parser.add_argument("--output")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        rc = selftest()
        print("PulseForge Complete mod auditor v4 self-test:", "PASS" if rc == 0 else "FAIL")
        return rc
    if not args.mod_root or not args.descriptor:
        parser.error("--mod-root and --descriptor required")
    root = Path(args.root).resolve()
    mod = Path(args.mod_root)
    mod = mod if mod.is_absolute() else root / mod
    descriptor_path = Path(args.descriptor)
    descriptor_path = descriptor_path if descriptor_path.is_absolute() else root / descriptor_path
    desc = json.loads(descriptor_path.read_text(encoding="utf-8"))
    report, errors = audit(root, mod, desc)
    if args.output:
        output = Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    print(
        f"PulseForge Complete mod audit v4: {report.get('files', 0)} files; "
        f"{len(report.get('charts', []))} runtime charts; "
        f"{len(report.get('archivalCharts', []))} archival charts; "
        f"{len(report.get('errors', []))} errors; {len(report.get('warnings', []))} warnings"
    )
    for warning in report.get("warnings", []):
        print("WARNING:", warning)
    for error in report.get("errors", []):
        print("ERROR:", error, file=os.sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
