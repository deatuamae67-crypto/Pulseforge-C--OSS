#!/usr/bin/env python3
"""Audit a locally supplied Discord Social SDK archive/directory without redistributing it.

The auditor reads the SDK in place, locates the development/runtime inputs PulseForge
needs on each platform, and can inspect binary architecture metadata. It never writes
SDK contents unless a caller explicitly redirects its textual report.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import pathlib
import re
import struct
import sys
import tarfile
import zipfile
from dataclasses import dataclass
from typing import BinaryIO, Iterable

VERSION_RE = re.compile(r"DiscordSocialSdk[-_](\d+\.\d+\.\d+)", re.IGNORECASE)


def normalize_member(name: str) -> str:
    return name.replace("\\", "/").lstrip("./")


@dataclass
class Member:
    name: str
    size: int | None = None


class SdkSource:
    kind: str

    def __init__(self, display_name: str) -> None:
        self.display_name = display_name

    def members(self) -> list[Member]:
        raise NotImplementedError

    def open_member(self, name: str) -> BinaryIO:
        raise NotImplementedError

    def read_member(self, name: str, limit: int | None = None) -> bytes:
        with self.open_member(name) as stream:
            return stream.read() if limit is None else stream.read(limit)

    def hash_member(self, name: str) -> str:
        digest = hashlib.sha256()
        with self.open_member(name) as stream:
            while True:
                chunk = stream.read(1024 * 1024)
                if not chunk:
                    break
                digest.update(chunk)
        return digest.hexdigest()

    def close(self) -> None:
        return None


class DirectorySource(SdkSource):
    kind = "directory"

    def __init__(self, root: pathlib.Path) -> None:
        super().__init__(root.name or str(root))
        self.root = root.resolve()
        self._members: list[Member] | None = None

    def members(self) -> list[Member]:
        if self._members is None:
            out: list[Member] = []
            for path in sorted(self.root.rglob("*")):
                if path.is_file():
                    rel = normalize_member(path.relative_to(self.root).as_posix())
                    out.append(Member(rel, path.stat().st_size))
            self._members = out
        return self._members

    def open_member(self, name: str) -> BinaryIO:
        path = (self.root / pathlib.PurePosixPath(name)).resolve()
        try:
            path.relative_to(self.root)
        except ValueError as exc:
            raise ValueError(f"member escapes SDK root: {name}") from exc
        return path.open("rb")


class ZipSource(SdkSource):
    kind = "zip"

    def __init__(self, path: pathlib.Path) -> None:
        super().__init__(path.name)
        self.path = path
        self.archive = zipfile.ZipFile(path, "r")
        self._original_by_normalized: dict[str, str] = {}
        self._members: list[Member] = []
        for info in self.archive.infolist():
            if info.is_dir():
                continue
            normalized = normalize_member(info.filename)
            if normalized in self._original_by_normalized:
                raise ValueError(f"duplicate normalized archive member: {normalized}")
            self._original_by_normalized[normalized] = info.filename
            self._members.append(Member(normalized, info.file_size))

    def members(self) -> list[Member]:
        return self._members

    def open_member(self, name: str) -> BinaryIO:
        return self.archive.open(self._original_by_normalized[name], "r")

    def close(self) -> None:
        self.archive.close()


class TarSource(SdkSource):
    kind = "tar"

    def __init__(self, path: pathlib.Path) -> None:
        super().__init__(path.name)
        self.path = path
        self.archive = tarfile.open(path, "r:*")
        self._original_by_normalized: dict[str, tarfile.TarInfo] = {}
        self._members: list[Member] = []
        for info in self.archive.getmembers():
            if not info.isfile():
                continue
            normalized = normalize_member(info.name)
            if normalized in self._original_by_normalized:
                raise ValueError(f"duplicate normalized archive member: {normalized}")
            self._original_by_normalized[normalized] = info
            self._members.append(Member(normalized, info.size))

    def members(self) -> list[Member]:
        return self._members

    def open_member(self, name: str) -> BinaryIO:
        stream = self.archive.extractfile(self._original_by_normalized[name])
        if stream is None:
            raise FileNotFoundError(name)
        return stream

    def close(self) -> None:
        self.archive.close()


def open_source(path: pathlib.Path) -> SdkSource:
    if path.is_dir():
        return DirectorySource(path)
    if not path.is_file():
        raise FileNotFoundError(path)
    lower = path.name.lower()
    if lower.endswith(".zip"):
        return ZipSource(path)
    if lower.endswith((".tar", ".tar.gz", ".tgz", ".tar.xz", ".txz")):
        return TarSource(path)
    raise ValueError(
        "SDK input must be a directory, .zip, .tar, .tar.gz, .tgz, .tar.xz or .txz"
    )


def shortest_match(
    names: Iterable[str],
    *,
    suffix: str | None = None,
    contains: str | None = None,
    basename: str | None = None,
) -> str | None:
    candidates: list[str] = []
    suffix_l = suffix.lower() if suffix else None
    contains_l = contains.lower() if contains else None
    basename_l = basename.lower() if basename else None
    for name in names:
        lower = name.lower()
        if suffix_l is not None and not lower.endswith(suffix_l):
            continue
        if contains_l is not None and contains_l not in lower:
            continue
        if basename_l is not None and pathlib.PurePosixPath(lower).name != basename_l:
            continue
        candidates.append(name)
    if not candidates:
        return None
    return min(candidates, key=lambda value: (value.count("/"), len(value), value.lower()))


def exact_match(names: Iterable[str], expected: str) -> str | None:
    expected_l = expected.lower()
    for name in names:
        if name.lower() == expected_l:
            return name
    return None


def detect_version(display_name: str, names: Iterable[str]) -> str | None:
    versions: set[str] = set()
    for text in (display_name, *names):
        versions.update(match.group(1) for match in VERSION_RE.finditer(text))
    if len(versions) > 1:
        raise ValueError(
            "conflicting SDK versions detected: " + ", ".join(sorted(versions))
        )
    return next(iter(versions), None)


_STANDARD_ROOT_SUFFIXES = (
    "include/discordpp.h",
    "include/cdiscord.h",
    "lib/release/discord_partner_sdk.lib",
    "bin/release/discord_partner_sdk.dll",
    "lib/release/libdiscord_partner_sdk.so",
    "lib/release/libdiscord_partner_sdk.dylib",
    "lib/release/discord_partner_sdk.aar",
)
_FRAMEWORK_ROOT_TOKENS = (
    "lib/release/discord_partner_sdk.framework/",
    "lib/release/discord_partner_sdk.xcframework/",
)


def detect_sdk_root(names: Iterable[str]) -> str:
    roots: dict[str, str] = {}
    for name in names:
        lower = name.lower()
        for suffix in _STANDARD_ROOT_SUFFIXES:
            if lower.endswith(suffix):
                prefix = name[: len(name) - len(suffix)]
                roots.setdefault(prefix.lower(), prefix)
        for token in _FRAMEWORK_ROOT_TOKENS:
            index = lower.find(token)
            if index >= 0:
                prefix = name[:index]
                roots.setdefault(prefix.lower(), prefix)

    if len(roots) > 1:
        rendered = ", ".join(repr(value or ".") for value in sorted(roots.values()))
        raise ValueError(f"multiple SDK roots/layouts detected: {rendered}")
    return next(iter(roots.values()), "")


def scope_to_root(names: Iterable[str], root: str) -> list[str]:
    if not root:
        return list(names)
    root_l = root.lower()
    return [name for name in names if name.lower().startswith(root_l)]


def framework_binary_match(
    names: Iterable[str], sizes: dict[str, int | None], root: str
) -> str | None:
    expected = f"{root}lib/release/discord_partner_sdk.framework/Versions/A/discord_partner_sdk"
    exact = exact_match(names, expected)
    if exact:
        return exact

    token = f"{root}lib/release/discord_partner_sdk.framework/".lower()
    candidates = [
        name
        for name in names
        if token in name.lower()
        and pathlib.PurePosixPath(name.lower()).name == "discord_partner_sdk"
    ]
    if not candidates:
        return None
    # Framework archives may preserve symlinks as tiny members. Prefer the real
    # executable payload rather than a shallow symlink placeholder.
    return max(candidates, key=lambda value: ((sizes.get(value) or 0), -len(value)))


def parse_binary_header(data: bytes) -> dict[str, object]:
    if len(data) >= 64 and data[:2] == b"MZ":
        pe_offset = struct.unpack_from("<I", data, 0x3C)[0] if len(data) >= 0x40 else 0
        if pe_offset + 6 <= len(data) and data[pe_offset : pe_offset + 4] == b"PE\0\0":
            machine = struct.unpack_from("<H", data, pe_offset + 4)[0]
            machine_map = {
                0x014C: "x86",
                0x8664: "x86_64",
                0xAA64: "arm64",
                0x01C4: "arm",
            }
            return {
                "format": "PE",
                "architectures": [machine_map.get(machine, f"machine-0x{machine:04x}")],
            }

    if len(data) >= 20 and data[:4] == b"\x7fELF":
        endian = "<" if data[5] == 1 else ">" if data[5] == 2 else None
        if endian:
            machine = struct.unpack_from(endian + "H", data, 18)[0]
            machine_map = {3: "x86", 40: "arm", 62: "x86_64", 183: "arm64"}
            return {
                "format": "ELF",
                "architectures": [machine_map.get(machine, f"machine-{machine}")],
            }

    if len(data) >= 8:
        magic = data[:4]
        thin_magics = {
            b"\xfe\xed\xfa\xce": (">", "Mach-O"),
            b"\xce\xfa\xed\xfe": ("<", "Mach-O"),
            b"\xfe\xed\xfa\xcf": (">", "Mach-O"),
            b"\xcf\xfa\xed\xfe": ("<", "Mach-O"),
        }
        cpu_map = {
            7: "x86",
            12: "arm",
            0x01000007: "x86_64",
            0x0100000C: "arm64",
        }
        if magic in thin_magics:
            endian, fmt = thin_magics[magic]
            cpu = struct.unpack_from(endian + "I", data, 4)[0]
            return {
                "format": fmt,
                "architectures": [cpu_map.get(cpu, f"cpu-0x{cpu:08x}")],
            }

        fat_magics = {
            b"\xca\xfe\xba\xbe": (">", False),
            b"\xbe\xba\xfe\xca": ("<", False),
            b"\xca\xfe\xba\xbf": (">", True),
            b"\xbf\xba\xfe\xca": ("<", True),
        }
        if magic in fat_magics:
            endian, fat64 = fat_magics[magic]
            count = struct.unpack_from(endian + "I", data, 4)[0]
            entry_size = 32 if fat64 else 20
            architectures: list[str] = []
            for index in range(min(count, 32)):
                offset = 8 + index * entry_size
                if offset + 4 > len(data):
                    break
                cpu = struct.unpack_from(endian + "I", data, offset)[0]
                arch = cpu_map.get(cpu, f"cpu-0x{cpu:08x}")
                if arch not in architectures:
                    architectures.append(arch)
            return {"format": "Mach-O universal", "architectures": architectures}

    if data.startswith(b"!<arch>\n"):
        return {"format": "COFF/ar archive", "architectures": []}
    return {"format": "unknown", "architectures": []}


def inspect_aar(raw: bytes) -> dict[str, object]:
    result: dict[str, object] = {
        "valid_zip": False,
        "prefab": False,
        "abis": [],
        "native_libraries": [],
        "usable": False,
    }
    try:
        with zipfile.ZipFile(io.BytesIO(raw), "r") as archive:
            names = [
                normalize_member(name)
                for name in archive.namelist()
                if not name.endswith("/")
            ]
    except zipfile.BadZipFile:
        return result

    result["valid_zip"] = True
    lowered = [name.lower() for name in names]
    result["prefab"] = any(
        "prefab/modules/discord_partner_sdk/" in name for name in lowered
    )
    native: list[str] = []
    abis: set[str] = set()
    for name in names:
        lower = name.lower()
        if not lower.endswith(".so") or "discord_partner_sdk" not in lower:
            continue
        native.append(name)
        match = re.search(r"(?:^|/)jni/([^/]+)/", name)
        if match:
            abis.add(match.group(1))
        match = re.search(r"(?:^|/)libs/android\.([^/]+)/", name)
        if match:
            abis.add(match.group(1))

    result["native_libraries"] = sorted(native)
    result["abis"] = sorted(abis)
    result["usable"] = bool(result["valid_zip"] and result["prefab"] and native and abis)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Audit an authorized Discord Social SDK archive/directory in place "
            "without redistributing it."
        )
    )
    parser.add_argument(
        "--sdk", required=True, help="Path to the authorized SDK directory or archive"
    )
    parser.add_argument(
        "--expect-version",
        help="Require an exact SDK version inferred from the archive/path name",
    )
    parser.add_argument(
        "--require",
        action="append",
        choices=("windows", "linux", "macos", "android", "all"),
        default=[],
        help="Fail if the requested platform input set is incomplete; repeat as needed",
    )
    parser.add_argument(
        "--deep",
        action="store_true",
        help="Inspect binary headers and Android AAR contents",
    )
    parser.add_argument(
        "--hash",
        action="store_true",
        dest="hash_candidates",
        help="SHA-256 hash matched SDK artifacts",
    )
    parser.add_argument("--json", action="store_true", help="Emit machine-readable JSON only")
    args = parser.parse_args()

    sdk_path = pathlib.Path(args.sdk).expanduser()
    source: SdkSource | None = None
    try:
        source = open_source(sdk_path)
        members = source.members()
        names = [member.name for member in members]
        sizes = {member.name: member.size for member in members}
        version = detect_version(source.display_name, names)
        sdk_root = detect_sdk_root(names)
        scoped_names = scope_to_root(names, sdk_root)

        discordpp = exact_match(scoped_names, f"{sdk_root}include/discordpp.h")
        cdiscord = exact_match(scoped_names, f"{sdk_root}include/cdiscord.h")
        windows_lib = exact_match(
            scoped_names, f"{sdk_root}lib/release/discord_partner_sdk.lib"
        )
        windows_dll = exact_match(
            scoped_names, f"{sdk_root}bin/release/discord_partner_sdk.dll"
        ) or shortest_match(scoped_names, suffix="/discord_partner_sdk.dll")
        linux_so = exact_match(
            scoped_names, f"{sdk_root}lib/release/libdiscord_partner_sdk.so"
        ) or shortest_match(scoped_names, suffix="/libdiscord_partner_sdk.so")
        mac_dylib = exact_match(
            scoped_names, f"{sdk_root}lib/release/libdiscord_partner_sdk.dylib"
        ) or shortest_match(scoped_names, suffix="/libdiscord_partner_sdk.dylib")
        mac_framework_binary = framework_binary_match(scoped_names, sizes, sdk_root)
        mac_framework_member = shortest_match(
            scoped_names,
            contains=f"{sdk_root}lib/release/discord_partner_sdk.framework/",
        )
        mac_xcframework_member = shortest_match(
            scoped_names,
            contains=f"{sdk_root}lib/release/discord_partner_sdk.xcframework/",
        )
        android_aar = exact_match(
            scoped_names, f"{sdk_root}lib/release/discord_partner_sdk.aar"
        ) or shortest_match(scoped_names, suffix="/discord_partner_sdk.aar")

        headers_ready = bool(discordpp and cdiscord)
        platforms: dict[str, dict[str, object]] = {
            "windows": {
                "ready_input": bool(headers_ready and windows_lib and windows_dll),
                "import_library": windows_lib,
                "runtime": windows_dll,
            },
            "linux": {
                "ready_input": bool(headers_ready and linux_so),
                "runtime": linux_so,
            },
            "macos": {
                "framework_input_present": bool(headers_ready and mac_framework_binary),
                "framework_binary": mac_framework_binary,
                "framework_member": mac_framework_member,
                "xcframework_member": mac_xcframework_member,
                "dylib_input_present": bool(headers_ready and mac_dylib),
                "dylib": mac_dylib,
                "ready_input": bool(headers_ready and (mac_framework_binary or mac_dylib)),
                "pulseforge_framework_private_validation_required": bool(
                    mac_framework_binary
                ),
            },
            "android": {
                "ready_input": bool(android_aar),
                "aar": android_aar,
            },
        }

        report: dict[str, object] = {
            "source": {
                "name": source.display_name,
                "kind": source.kind,
                "version": version,
                "sdk_root": sdk_root or ".",
                "member_count": len(members),
            },
            "headers": {
                "ready": headers_ready,
                "discordpp_h": discordpp,
                "cdiscord_h": cdiscord,
            },
            "platforms": platforms,
        }

        candidates = {
            "windows_import_library": windows_lib,
            "windows_runtime": windows_dll,
            "linux_runtime": linux_so,
            "macos_framework_binary": mac_framework_binary,
            "macos_dylib_runtime": mac_dylib,
            "android_aar": android_aar,
        }

        if args.deep:
            deep: dict[str, object] = {}
            for key in (
                "windows_runtime",
                "linux_runtime",
                "macos_framework_binary",
                "macos_dylib_runtime",
            ):
                member_name = candidates[key]
                if member_name:
                    deep[key] = parse_binary_header(
                        source.read_member(member_name, 65536)
                    )

            if android_aar:
                aar_size = sizes.get(android_aar)
                if aar_size is None or aar_size <= 256 * 1024 * 1024:
                    aar_analysis = inspect_aar(source.read_member(android_aar))
                    deep["android_aar"] = aar_analysis
                    platforms["android"]["ready_input"] = bool(
                        aar_analysis.get("usable")
                    )
                else:
                    deep["android_aar"] = {
                        "skipped": True,
                        "reason": (
                            f"AAR is {aar_size} bytes; deep nested inspection "
                            "limit is 256 MiB"
                        ),
                    }
                    platforms["android"]["ready_input"] = False

            report["deep"] = deep

        if args.hash_candidates:
            hashes: dict[str, str] = {}
            for key, member_name in candidates.items():
                if member_name:
                    hashes[key] = source.hash_member(member_name)
            report["sha256"] = hashes

        requested = set(args.require)
        if "all" in requested:
            requested = {"windows", "linux", "macos", "android"}
        missing = [
            platform
            for platform in sorted(requested)
            if not bool(platforms[platform]["ready_input"])
        ]
        version_ok = args.expect_version is None or version == args.expect_version
        requirements = {
            "requested_platforms": sorted(requested),
            "missing_platforms": missing,
            "expected_version": args.expect_version,
            "version_ok": version_ok,
            "satisfied": not missing and version_ok,
        }
        report["requirements"] = requirements

        if args.json:
            print(json.dumps(report, indent=2, sort_keys=True))
        else:
            print("Discord Social SDK private input audit")
            print(
                f"  Source:  {source.display_name} "
                f"({source.kind}, {len(members)} files)"
            )
            print(f"  Version: {version or 'unknown'}")
            print(f"  SDK root: {sdk_root or '.'}")
            print(f"  Headers: {'READY' if headers_ready else 'MISSING'}")
            for platform in ("windows", "linux", "macos", "android"):
                ready = bool(platforms[platform]["ready_input"])
                print(
                    f"  {platform.capitalize():8}: "
                    f"{'READY INPUT' if ready else 'MISSING INPUT'}"
                )
            if mac_framework_binary:
                print(
                    "  macOS:   framework executable detected; "
                    "framework-native PulseForge packaging still requires "
                    "its dedicated validation path"
                )
            if args.deep and "deep" in report:
                print("\nBinary/package inspection:")
                for key, value in report["deep"].items():
                    print(f"  {key}: {value}")
            if args.hash_candidates and report.get("sha256"):
                print("\nCandidate SHA-256:")
                for key, value in sorted(report["sha256"].items()):
                    print(f"  {key}: {value}")
            if args.expect_version and not version_ok:
                print(
                    f"\nVersion mismatch: expected {args.expect_version}, "
                    f"detected {version or 'unknown'}",
                    file=sys.stderr,
                )
            if missing:
                print(
                    f"\nMissing required platform inputs: {', '.join(missing)}",
                    file=sys.stderr,
                )

        return 0 if requirements["satisfied"] else 3
    except (
        OSError,
        ValueError,
        KeyError,
        EOFError,
        zipfile.BadZipFile,
        tarfile.TarError,
    ) as exc:
        print(f"Discord SDK audit input error: {exc}", file=sys.stderr)
        return 2
    finally:
        if source is not None:
            try:
                source.close()
            except (OSError, zipfile.BadZipFile, tarfile.TarError):
                pass


if __name__ == "__main__":
    raise SystemExit(main())
