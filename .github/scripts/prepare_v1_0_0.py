from pathlib import Path
import json


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(
            f"{path}: expected exactly one match, found {count}: {old!r}"
        )
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


# Version identity.
replace_once("CMakeLists.txt", "    VERSION 0.9.7\n", "    VERSION 1.0.0\n")
replace_once(
    "CMakeLists.txt",
    '            MACOSX_BUNDLE_BUNDLE_VERSION "90700"\n',
    '            MACOSX_BUNDLE_BUNDLE_VERSION "100000"\n',
)
replace_once(
    "platform/android/app/build.gradle",
    "        versionCode = 90700\n        versionName = '0.9.7'\n",
    "        versionCode = 100000\n        versionName = '1.0.0'\n",
)
replace_once(
    "README.md", "# PulseForge C++ OSS 0.9.7\n", "# PulseForge C++ OSS 1.0.0\n"
)
replace_once(
    "platform/android/README.md",
    "current delivery is `0.9.7` (`versionCode 90700`).",
    "current delivery is `1.0.0` (`versionCode 100000`).",
)
replace_once(
    ".github/ISSUE_TEMPLATE/bug_report.yml",
    'placeholder: "0.9.7 / commit SHA"',
    'placeholder: "1.0.0 / commit SHA"',
)

# SC:R fix: executable Lua discovery must be scoped to the selected content/mod.
roots = Path("src/app/psych_content_roots.hpp")
text = roots.read_text(encoding="utf-8")
marker = "// Generic Psych/FNF layout expansion.\n"
if text.count(marker) != 1:
    raise SystemExit("psych_content_roots.hpp insertion marker mismatch")
helper = r'''// PULSEFORGE_1_0_0_SELECTED_SCRIPT_ROOT_ISOLATION_V1
// Executable script discovery is deliberately narrower than visual/content
// fallback discovery. When the launcher selected a concrete mod or content
// root, only that selection may contribute Lua. This prevents an unrelated
// sibling mod from returning Function_Stop from onStartCountdown() and holding
// the active chart at t=0. Direct CLI launches without a catalog selection keep
// the legacy caller-provided root set.
[[nodiscard]] inline std::vector<std::filesystem::path>
select_psych_executable_roots(
    const std::span<const std::filesystem::path> content_roots,
    const std::filesystem::path& selected_content_root,
    const std::filesystem::path& selected_mod_root
) {
    using namespace psych_content_roots_detail;

    std::vector<std::filesystem::path> result;
    result.reserve(content_roots.size() + 2U);
    std::unordered_set<std::string> keys;
    keys.reserve(content_roots.size() * 2U + 5U);

    const auto append = [&](const std::filesystem::path& candidate) {
        if (candidate.empty()) return;
        const auto normalized = normalize_path(candidate);
        const auto candidate_key = path_key(normalized);
        if (candidate_key.empty() || keys.contains(candidate_key)) return;
        keys.insert(candidate_key);
        result.push_back(normalized);
    };

    if (!selected_mod_root.empty()) {
        append(selected_content_root);
        append(selected_mod_root);
        return result;
    }
    if (!selected_content_root.empty()) {
        append(selected_content_root);
        return result;
    }

    for (const auto& root : content_roots) append(root);
    return result;
}

'''
roots.write_text(text.replace(marker, helper + marker, 1), encoding="utf-8")

app = Path("src/app/application.cpp")
text = app.read_text(encoding="utf-8")
old = '''        for (const auto& root : options_.content_roots) {
            append_explicit_script_root(root);
        }
        append_explicit_script_root(options_.selected_content_root);
        append_explicit_script_root(options_.selected_mod_root);'''
new = '''        for (const auto& root : detail::select_psych_executable_roots(
                 options_.content_roots,
                 options_.selected_content_root,
                 options_.selected_mod_root
             )) {
            append_explicit_script_root(root);
        }'''
if text.count(old) != 1:
    raise SystemExit(f"application.cpp SC:R root block mismatch: {text.count(old)}")
app.write_text(text.replace(old, new, 1), encoding="utf-8")

test = Path("tests/psych_content_roots_test.cpp")
text = test.read_text(encoding="utf-8")
function_marker = "void test_stock_provider_tie_break_is_deterministic() {\n"
if text.count(function_marker) != 1:
    raise SystemExit("psych_content_roots_test.cpp function marker mismatch")
regression = r'''void test_selected_mod_isolates_executable_scripts() {
    const auto root = temp_root();
    const auto base_assets = root / "assets";
    const auto mods = root / "mods";
    const auto sibling = mods / "unrelated-mod";
    const auto selected_mod = mods / "drive-pack-screboot-demo";
    const auto selected_content = selected_mod
        / "SCReboot_Demo" / "bin" / "assets" / "shared" / "data";

    mkdir(base_assets);
    mkdir(sibling / "scripts");
    mkdir(selected_content);

    const std::vector content_roots{base_assets, mods, sibling};
    const auto executable = pulseforge::detail::select_psych_executable_roots(
        content_roots,
        selected_content,
        selected_mod
    );

    require(
        executable.size() == 2U,
        "selected content + mod are the only executable roots"
    );
    require(
        key(executable[0]) == key(selected_content),
        "selected content keeps precedence order"
    );
    require(
        key(executable[1]) == key(selected_mod),
        "selected mod remains highest precedence"
    );
    for (const auto& path : executable) {
        require(
            key(path) != key(sibling),
            "unrelated sibling Lua cannot enter selected SC:R runtime"
        );
    }

    const auto resolved = pulseforge::detail::resolve_psych_content_roots(
        executable,
        64U,
        false
    );
    require(!resolved.roots.empty(), "selected SC:R executable roots resolve");
    for (const auto& path : resolved.roots) {
        require(
            pulseforge::detail::psych_content_roots_detail::path_is_within(
                path,
                selected_mod
            ),
            "script-code fallback stays inside the selected SC:R mod"
        );
    }
    remove_tree(root);
}

void test_direct_launch_keeps_explicit_script_roots() {
    const auto root = temp_root();
    const auto first = root / "first";
    const auto second = root / "second";
    mkdir(first);
    mkdir(second);
    const std::vector content_roots{first, second};
    const auto executable = pulseforge::detail::select_psych_executable_roots(
        content_roots,
        {},
        {}
    );
    require(executable.size() == 2U, "direct CLI launch retains explicit roots");
    require(key(executable[0]) == key(first), "direct root order first");
    require(key(executable[1]) == key(second), "direct root order second");
    remove_tree(root);
}

'''
text = text.replace(function_marker, regression + function_marker, 1)
call_marker = "        test_stock_provider_tie_break_is_deterministic();\n"
if text.count(call_marker) != 1:
    raise SystemExit("psych_content_roots_test.cpp call marker mismatch")
text = text.replace(
    call_marker,
    "        test_selected_mod_isolates_executable_scripts();\n"
    "        test_direct_launch_keeps_explicit_script_roots();\n"
    + call_marker,
    1,
)
test.write_text(text, encoding="utf-8")

changelog = Path("CHANGELOG.md")
text = changelog.read_text(encoding="utf-8")
marker = "## [0.9.7] - 2026-09-04\n"
if text.count(marker) != 1:
    raise SystemExit("CHANGELOG 0.9.7 marker mismatch")
section = '''## [1.0.0] - 2026-09-05

### Definitive release

- Promotes PulseForge to the stable `1.0.0` release line; `0.9.7` is retained as the preceding pre-release.
- Distributes the complete authoritative 27-pack engine mod corpus as separately verifiable release assets instead of bloating Git history with approximately 14.4 GB of static content.
- Adds a versioned mod-corpus manifest with Drive provenance, default enablement, file counts, unpacked byte counts and deterministic path/size inventory hashes.

### Fixed

- Isolates executable Psych/Denpa/SC:R Lua discovery to the selected content/mod roots. Unrelated sibling mods can no longer inject `onStartCountdown()` / `Function_Stop` into the active SC:R chart and freeze gameplay at `t=0`.
- Keeps broad fallback discovery for non-executable content while explicitly disabling sibling stock-provider injection for executable script discovery.

### Validation

- Adds a deterministic regression that models the nested `drive-pack-screboot-demo/SCReboot_Demo/bin/assets/shared/data` layout and proves sibling script roots are excluded.
- Direct CLI launches without a catalog-selected mod retain the existing explicit-root behavior.

'''
changelog.write_text(text.replace(marker, section + marker, 1), encoding="utf-8")

entries = [
    ("drive-library-2026-04-30", True, "1PTa0ldHNt0reXMOhnz6vTJVt5RkokHu0", 592, 389579119, "7ad585b809f48a670eb79d502604a9c2765abda0fc4cfcd107d1acd75920cff1"),
    ("mr-shash-recovered", True, "1JXQZUnuOo9klZb9vg9OReaVIn55VGMQL", 16, 149594831, "d8b75b78c343e4e0963d5a3828e0fa810548df9d4803728b07e5d8ed325a96db"),
    ("js-engine-dorklysonic", True, "1ELZMQUVXuAe-V43QeUheKgPY5CxAPPrW", 41, 3492318, "a4ff0f3cc5cdbe4ab868dd31acf3626f17587b29f12f9977414b6ebe70cf8a87"),
    ("pulseforge-created", True, "1d-a6q9C9AkxJUITKtZsk0wlNpqhXDYa-", 12, 388710226, "66aeaf366bb79cedfe97fe6be75fbe690211e4ca291b60a173b8877d08b47d43"),
    ("drive-loose-charts-2026-08-01", True, "1sl3Dyd56Co6nuykyXx1Eb3MKGi_-3fZ7", 34, 721246229, "c46d65b195eab985437dc46a593599fc109831eaf9cd165ed3036955a0896f29"),
    ("drive-batata-static", True, "1NgkaKdmunL9J81RnS1nE5HHUngDs63iV", 17, 1703938152, "d5a701c582746e753a44619069f6cf0e4643065d09da0e80fd226c273a3ed4b4"),
    ("drive-pack-denpaex", True, "1jfE3oKE_onhN4OAKX7rQPxwtXBSKtRfL", 1199, 254631623, "fd9807ca1a9ece73d11f39e7d7c52cfe986effd82bd7734c219ce0e5f1d5ce30"),
    ("drive-pack-fnf-js-engine-windows", True, "1spqt0yKLzmPyHRm5khs-w2lEor_89cQT", 1351, 481992046, "acc708778fb1aefafbecfafedbf6024a697c88a407bff24fdb0b3288eefba301"),
    ("drive-pack-h-slice-js-0-2-4-windows", True, "1QOCRFk5iJzmNCudLEX4JRyFUw3nZk_7m", 1929, 593871715, "3ca7d937be543dd46f1a9d9cfd9195814389720ee089b431dbc2b2e79b1789ed"),
    ("drive-pack-h-slice-0-2-3-windows", True, "1g0oPwr9ey4vFgjlH701N-fETz5yeD_3c", 1928, 593846242, "14ae5395d5cb52f88c1799ea713889b9a5107ee2ad8b7ab5f90fdb49a987ff9c"),
    ("drive-pack-christmasred-edition", True, "1_Z0ZzstG7rhDkqOfdQOvaEva96ssAQ3N", 1317, 851092276, "55b92aa2d710c6b73a40ac159e54f078cf88b2662e5d6d211b0e1ed4f65be90f"),
    ("drive-pack-vs-dinnerbambi-9e405", True, "1k5TR9OSyS3m5R2iiJfmvKwraiLjmcLkE", 731, 346161509, "af3c40d2be45277f0f7526ef1dbcb7bb3efb9b03fecc810d19a9344a681995ff"),
    ("drive-pack-hortas-edition-v3-16d89", True, "1x0tlkEQM7vbc8XQnNFyUvcaMP0N3c6LX", 1482, 1256632059, "0a9d312919927c44502c9de794bb5ac7a5d482d643943edee7bdd0995aa88d4b"),
    ("drive-pack-files", True, "1_ZYbZRVLr2TQpTy-yPnOWsS323PcBASG", 51, 2390312362, "b639908bbffa2a7ad8818824ca7eac92a29c08b343828dfcde7b68a6571dcc77"),
    ("drive-pack-disable-sustains-legacy", True, "1nmrnSGijljg_u8Se9xZ9V9KItVJ1gMtx", 712, 177125667, "b56932f5d454bcde40437104b16c672fee4664b8bfa052580a7973cbbe612c80"),
    ("drive-pack-screboot-demo", True, "1s40Q-khRW33tvM9RsEdQ0gOgB6rTONkg", 1288, 592464073, "5fd24bdfb3a5544cb22ce67486efe39b7e4e140d90fa56a7a493cb67a10d4131"),
    ("drive-2026-08-22-ez-edition", True, "1b7PQfq2p97xSJJEQfloCb2EHr8laBSPX", 674, 279964464, "e0192ee89be2aefc0cf609b9e0401677e770670a0df18ba2f44bff1045e2b047"),
    ("drive-2026-08-22-dek-v1", True, "1W0eTRv8bJgNke38YA0iJKoZTErkqksGD", 417, 241118272, "e269bf55e990179d0f95490e6b16af9898c724c4010990813f3b2b7befe16ef7"),
    ("drive-2026-08-22-dek-v2-finale", True, "1boHh8HtatjRrPYCjn9xMI0QFCbDxoYtA", 951, 1153345941, "28bb4ac838a3ec17a029906538e2e23ea0a222e25873057ba1a9224b22bbdabc"),
    ("drive-2026-08-22-dek-v2-demo", False, "18OLGtIwI20KP0ZRrvtlaD_sFIlfDIq7Z", 558, 337381878, "61ff890177d4bc0a5cd4a6b9c1944b37dccfcb8d9e4b4d4b46216859db022bef"),
    ("drive-2026-08-22-dek-v2-dev-unverified", False, "1Yla_a-mYRA5vDBOeS5Me2jTnuHJGvcau", 575, 344260382, "ea7e5f9fa761ed7221be85107d57706c42da54ba9e3b64dc02c15888a3118f7e"),
    ("drive-2026-08-22-vs-yerep-v2", True, "1DgQeakx07MucA13_2juqnuAgqFb8766X", 1379, 504322079, "ab68ad7edf9f60114d021ed752e367ee8b5f5be590ca89dd5b94b4c619025fac"),
    ("drive-2026-08-22-vs-everamii", True, "1Xns4TsZdDCKhClqbGwGU702nk8ciwrjP", 1092, 254502864, "2da89d98f01663a3dd8ce3954e4db2a7c938553402c5b030698c5410e3078989"),
    ("drive-2026-08-22-vs-bambi-b-side-9k", True, "1rAiZ5Ry5ISyJ_dPQqGSsDIzDqQqhdvN9", 396, 137192805, "3090037b01d34f333601d690af4d4888a3a83afc1187b1a3747883703f732296"),
    ("drive-2026-08-22-evil-unfairness", True, "1tOD7BIA6jbeeFC1zKcDJQh4YTiC_QNzw", 405, 144705828, "34d91a65835e91ab593e74a006a58767665673e10b15509aff47ca328f4ac642"),
    ("drive-2026-08-22-evil-tsukareta-b-side", True, "1kun7MefPcpLqDchsm8j0zKWkpRoiTMt-", 393, 134693528, "d8eb2dab3133b8516faaecd9d46a3b909f71b2ee725fb4fd7aaf911353e18871"),
    ("m-r-vs-dave-bambi-fantrack-reupload", True, "1WD3k9iuF6QNVUmy4UyJRtsNuJorHqbFL", 7, 3402847, "c07f14d4c7cc05421b57615224d4bbdc5d6e3b9c0834aab3b2faaf2941a34294"),
]
corpus = {
    "schema_version": 1,
    "pulseforge_version": "1.0.0",
    "source": {
        "drive_mods_folder_id": "1pAnpn__0pfFe1pMtS-XyenPP5peqIU46",
        "mods_list_sha256": "5eafeac677f4e70082db88a5f71f950107fbede576a20acb37ccbe1f40d8e2bb",
        "engine_full_filelist_sha256": "37476e5fa488c6bc3b24c511d114dd95b0acb56c31a77217d51154a87515a549",
    },
    "mod_count": len(entries),
    "enabled_by_default_count": sum(1 for e in entries if e[1]),
    "file_count": sum(e[3] for e in entries),
    "unpacked_bytes": sum(e[4] for e in entries),
    "mods": [
        {
            "id": e[0],
            "enabled_by_default": e[1],
            "drive_folder_id": e[2],
            "file_count": e[3],
            "unpacked_bytes": e[4],
            "path_size_inventory_sha256": e[5],
        }
        for e in entries
    ],
}
Path("docs/MOD_CORPUS_1.0.0.json").write_text(
    json.dumps(corpus, indent=2) + "\n", encoding="utf-8"
)
Path("docs/MOD_CORPUS_1.0.0.md").write_text(
    "# PulseForge 1.0.0 mod corpus\n\n"
    "The definitive 1.0.0 distribution tracks **all 27 entries** in the engine's "
    "authoritative `modsList.txt`, including the two entries that are disabled by "
    "default. The corpus contains **19,547 mod files** and **14,429,581,335 unpacked "
    "bytes**; `modsList.txt` itself is distributed alongside the packs.\n\n"
    "The public Git repository stores this deterministic inventory rather than "
    "committing roughly 14.4 GB of binary/static mod content into Git history. "
    "The GitHub 1.0.0 release packages every listed mod as a separately checksummed "
    "content asset sourced from the public read-only Drive corpus. `.autochart-staging` "
    "and auxiliary FLP/source folders are not engine mods and are not part of "
    "authoritative `modsList.txt`.\n\n"
    "`docs/MOD_CORPUS_1.0.0.json` records the Drive folder ID, default enablement, "
    "file count, unpacked byte count and SHA-256 of the sorted `path<TAB>size` "
    "inventory for every pack. Release-package SHA-256 values are generated from "
    "the actual archives and are independent of these inventory hashes.\n\n"
    "SCReboot is included as `drive-pack-screboot-demo`; executable Lua is scoped "
    "to the selected mod in 1.0.0 so sibling mods cannot block its countdown.\n",
    encoding="utf-8",
)
Path("docs/RELEASE_NOTES_1.0.0.md").write_text(
    "# PulseForge 1.0.0\n\n"
    "PulseForge 1.0.0 is the definitive stable release following the 0.9.7 "
    "pre-release.\n\n"
    "## Highlights\n\n"
    "- Complete 27-pack authoritative engine mod corpus distributed as checksummed GitHub release content assets.\n"
    "- SC:R / SCReboot gameplay-start freeze fixed by isolating executable script discovery to the selected mod/content roots.\n"
    "- Existing cross-platform engine targets retained: Windows x86_64, Linux x86_64, macOS x86_64, macOS arm64 and Android arm64.\n"
    "- Huge-chart PFC1/streaming behavior retained; large source charts are not forced through materializing loaders or Git blobs.\n"
    "- Discord Social SDK remains an optional private dependency; proprietary SDK files are not redistributed in the OSS repository or public no-SDK builds.\n\n"
    "## Distribution\n\n"
    "The core platform packages and all mod-content packages are built/packaged from "
    "the final 1.0.0 source/provenance chain. Android CI output remains test-signed "
    "and macOS CI output remains ad-hoc signed unless separate production identities "
    "are supplied.\n",
    encoding="utf-8",
)
Path("mods").mkdir(exist_ok=True)
Path("mods/README.md").write_text(
    "# PulseForge 1.0.0 content packs\n\n"
    "The definitive release contains the complete engine mod corpus, but the ~14.4 GB "
    "payload is intentionally not committed into Git history. See "
    "`../docs/MOD_CORPUS_1.0.0.json` for the exact 27-pack inventory and the GitHub "
    "`v1.0.0` release for the checksummed downloadable content archives.\n",
    encoding="utf-8",
)

assert corpus["mod_count"] == 27
assert corpus["enabled_by_default_count"] == 25
assert corpus["file_count"] == 19547
assert corpus["unpacked_bytes"] == 14429581335
assert any(e["id"] == "drive-pack-screboot-demo" for e in corpus["mods"])
