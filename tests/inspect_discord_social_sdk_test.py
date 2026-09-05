#!/usr/bin/env python3
from __future__ import annotations

import io
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "scripts" / "inspect-discord-social-sdk.py"


def pe_x64() -> bytes:
    data = bytearray(512)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", data, 0x84, 0x8664)
    return bytes(data)


def elf_x64() -> bytes:
    data = bytearray(64)
    data[:4] = b"\x7fELF"
    data[4] = 2
    data[5] = 1
    struct.pack_into("<H", data, 18, 62)
    return bytes(data)


def macho_arm64() -> bytes:
    return b"\xcf\xfa\xed\xfe" + struct.pack("<I", 0x0100000C) + b"\0" * 56


def aar_arm64() -> bytes:
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("AndroidManifest.xml", "<manifest package='com.discord.socialsdk'/>")
        archive.writestr("prefab/modules/discord_partner_sdk/module.json", "{}")
        archive.writestr("jni/arm64-v8a/libdiscord_partner_sdk.so", elf_x64())
    return buffer.getvalue()


def make_sdk(path: pathlib.Path, include_android: bool = True) -> None:
    root = "DiscordSocialSdk-1.10.19337"
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(f"{root}/include/discordpp.h", "#pragma once\n")
        archive.writestr(f"{root}/include/cdiscord.h", "#pragma once\n")
        archive.writestr(f"{root}/lib/release/discord_partner_sdk.lib", b"!<arch>\n")
        archive.writestr(f"{root}/bin/release/discord_partner_sdk.dll", pe_x64())
        archive.writestr(f"{root}/lib/release/libdiscord_partner_sdk.so", elf_x64())
        archive.writestr(
            f"{root}/lib/release/discord_partner_sdk.framework/Versions/A/discord_partner_sdk",
            macho_arm64(),
        )
        if include_android:
            archive.writestr(f"{root}/lib/release/discord_partner_sdk.aar", aar_arm64())


class InspectorTest(unittest.TestCase):
    def run_inspector(self, archive: pathlib.Path, *extra: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--sdk", str(archive), *extra],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

    def test_complete_multiplatform_archive(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            archive = pathlib.Path(temp_dir) / "DiscordSocialSdk-1.10.19337.zip"
            make_sdk(archive)
            result = self.run_inspector(
                archive,
                "--expect-version", "1.10.19337",
                "--require", "all",
                "--deep",
                "--hash",
                "--json",
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(result.stdout)
            self.assertTrue(report["requirements"]["satisfied"])
            self.assertTrue(report["headers"]["ready"])
            self.assertTrue(report["platforms"]["windows"]["ready_input"])
            self.assertTrue(report["platforms"]["linux"]["ready_input"])
            self.assertTrue(report["platforms"]["macos"]["framework_input_present"])
            self.assertTrue(report["platforms"]["android"]["ready_input"])
            self.assertEqual(report["deep"]["windows_runtime"]["architectures"], ["x86_64"])
            self.assertEqual(report["deep"]["linux_runtime"]["architectures"], ["x86_64"])
            self.assertEqual(report["deep"]["macos_framework_binary"]["architectures"], ["arm64"])
            self.assertTrue(report["deep"]["android_aar"]["prefab"])
            self.assertEqual(report["deep"]["android_aar"]["abis"], ["arm64-v8a"])
            self.assertIn("windows_runtime", report["sha256"])

    def test_missing_required_android_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            archive = pathlib.Path(temp_dir) / "DiscordSocialSdk-1.10.19337.zip"
            make_sdk(archive, include_android=False)
            result = self.run_inspector(archive, "--require", "android", "--json")
            self.assertEqual(result.returncode, 3)
            report = json.loads(result.stdout)
            self.assertEqual(report["requirements"]["missing_platforms"], ["android"])


if __name__ == "__main__":
    unittest.main()
