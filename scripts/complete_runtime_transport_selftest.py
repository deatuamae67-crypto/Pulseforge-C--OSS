#!/usr/bin/env python3
"""Fast source-level invariants for the optional Complete runtime transport."""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TRANSPORT = ROOT / "src/app/complete_content_transport.hpp"
GATE = ROOT / "src/app/complete_content_gate.hpp"
ANDROID = (
    ROOT
    / "platform/android/app/src/main/java/org/pulseforge/engine/CompleteDownloadBridge.java"
)


def require(text: str, needle: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing required Complete transport invariant: {needle}")


def forbid(text: str, needle: str) -> None:
    if needle in text:
        raise AssertionError(f"forbidden Complete transport dependency/pattern: {needle}")


def main() -> int:
    transport = TRANSPORT.read_text(encoding="utf-8")
    gate = GATE.read_text(encoding="utf-8")
    android = ANDROID.read_text(encoding="utf-8")

    for needle in (
        "WinHttpOpen",
        "libcurl.so.4",
        "/usr/lib/libcurl.4.dylib",
        "drive_confirmation_url",
        '".part"',
        "install_complete_runtime_staging",
    ):
        require(transport, needle)
    for needle in (
        "Pretende instalar os mods agora?",
        "PULSEFORGE_COMPLETE_MANIFEST",
        "PULSEFORGE_MOD_ROOT",
        "run_progress_window",
    ):
        require(gate, needle)
    for needle in (
        "HttpURLConnection",
        "CookieManager",
        "Content-Disposition",
        'status == 416',
        'new File(parent, destination.getName() + ".part")',
    ):
        require(android, needle)

    combined = transport + gate + android
    for needle in (
        "python -m gdown",
        "Runtime.getRuntime().exec",
        "ProcessBuilder(",
        "system(\"curl",
        "popen(\"curl",
    ):
        forbid(combined, needle)

    print("Complete runtime transport self-test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
