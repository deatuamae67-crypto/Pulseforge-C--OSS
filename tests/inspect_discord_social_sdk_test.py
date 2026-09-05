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


def write_complete_sdk(
    archive: zipfile.ZipFile,
    root: str = "DiscordSocialSdk-1.10.19337",
    *,
    include_android: bool = True,
    framework_binary: bool = True,
) -> None:
    archive.writestr(f"{root}/include/discordpp.h", "#pragma once\n")
    archive.writestr(f"{root}/include/cdiscord.h", "#pragma once\n")
    archive.writestr(f"{root}/lib/release/discord_partner_sdk.lib", b"!<arch>\n")
    archive.writestr(f"{root}/bin/release/discord_partner_sdk.dll", pe_x64())
    archive.writestr(f"{root}/lib/release/libdiscord_partner_sdk.so", elf_x64())
    if framework_binary:
        archive.writestr(
            f"{root}/lib/release/discord_partner_sdk.framework/Versions/A/discord_partner_sdk",
            macho_arm64(),
        )
    else:
        archive.writestr(
            f"{root}/lib/release/discord_partner_sdk.framework/Versions/A/Resources/Info.plist",
            "<plist/>",
        )
    if include_android:
        archive.writestr(f"{root}/lib/release/discord_partner_sdk.aar", aar_arm64())


def make_sdk(path: pathlib.Path, include_android: bool = True) -> None:
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        write_complete_sdk(archive, include_android=include_android)


def corrupt_stored_member(path: pathlib.Path, member_name: str) -> None:
    with zipfile.ZipFile(path, "r") as archive:
        info = archive.getinfo(member_name)
        header_offset = info.header_offset

    raw = bytearray(path.read_bytes())
    # Local file header: signature (4), fixed fields (26), filename length and
    # extra-field length. The selected test archive uses ZIP_STORED so changing
    # one payload byte preserves structure while guaranteeing a CRC failure.
    name_length = struct.unpack_from("<H", raw, header_offset + 26)[0]
    extra_length = struct.unpack_from("<H", raw, header_offset + 28)[0]
    data_offset = header_offset + 30 + name_length + extra_length
    raw[data_offset] ^= 0x01
    path.write_bytes(raw)


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
            self.assertTrue(report["deep"]["android_aar"]["usable"])
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

    def test_malformed_aar_fails_deep_android_gate(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            archive = pathlib.Path(temp_dir) / "DiscordSocialSdk-1.10.19337.zip"
            root = "DiscordSocialSdk-1.10.19337"
            with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as sdk:
                write_complete_sdk(sdk, include_android=False)
                sdk.writestr(f"{root}/lib/release/discord_partner_sdk.aar", b"not-an-aar")
            result = self.run_inspector(
                archive, "--require", "android", "--deep", "--json"
            )
            self.assertEqual(result.returncode, 3, result.stderr)
            report = json.loads(result.stdout)
            self.assertFalse(report["deep"]["android_aar"]["usable"])
            self.assertEqual(report["requirements"]["missing_platforms"], ["android"])

    def test_framework_without_executable_fails_macos_gate(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            archive = pathlib.Path(temp_dir) / "DiscordSocialSdk-1.10.19337.zip"
            with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as sdk:
                write_complete_sdk(sdk, framework_binary=False)
            result = self.run_inspector(archive, "--require", "macos", "--json")
            self.assertEqual(result.returncode, 3, result.stderr)
            report = json.loads(result.stdout)
            self.assertFalse(report["platforms"]["macos"]["framework_input_present"])
            self.assertEqual(report["requirements"]["missing_platforms"], ["macos"])

    def test_multiple_sdk_roots_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            archive = pathlib.Path(temp_dir) / "DiscordSocialSdk-1.10.19337.zip"
            with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as sdk:
                sdk.writestr("sdk-a/include/discordpp.h", "#pragma once\n")
                sdk.writestr("sdk-a/include/cdiscord.h", "#pragma once\n")
                sdk.writestr("sdk-b/lib/release/libdiscord_partner_sdk.so", elf_x64())
            result = self.run_inspector(archive, "--require", "linux", "--json")
            self.assertEqual(result.returncode, 2)
            self.assertIn("multiple SDK roots/layouts detected", result.stderr)
            self.assertNotIn("Traceback", result.stderr)

    def test_conflicting_versions_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            archive = pathlib.Path(temp_dir) / "sdk-bundle.zip"
            with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as sdk:
                write_complete_sdk(sdk, root="DiscordSocialSdk-1.10.19337")
                sdk.writestr("DiscordSocialSdk-1.10.18687/README.txt", "older release")
            result = self.run_inspector(archive, "--json")
            self.assertEqual(result.returncode, 2)
            self.assertIn("conflicting SDK versions detected", result.stderr)
            self.assertNotIn("Traceback", result.stderr)

    def test_corrupt_member_is_controlled_input_error(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            archive = pathlib.Path(temp_dir) / "DiscordSocialSdk-1.10.19337.zip"
            root = "DiscordSocialSdk-1.10.19337"
            with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_STORED) as sdk:
                write_complete_sdk(sdk)
            target = f"{root}/bin/release/discord_partner_sdk.dll"
            corrupt_stored_member(archive, target)
            result = self.run_inspector(archive, "--deep", "--json")
            self.assertEqual(result.returncode, 2)
            self.assertIn("Discord SDK audit input error", result.stderr)
            self.assertNotIn("Traceback", result.stderr)


if __name__ == "__main__":
    unittest.main()
