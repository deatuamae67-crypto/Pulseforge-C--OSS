#include "pulseforge/content_catalog.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void write_text(
    const std::filesystem::path& path,
    const std::string_view text
) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("failed to create catalog fixture");
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!output) {
        throw std::runtime_error("failed to write catalog fixture");
    }
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto nonce = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
            / ("pulseforge-catalog-test-" + std::to_string(nonce));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] const pulseforge::SongCatalogEntry* entry_for_mod(
    const pulseforge::ContentCatalog& catalog,
    const std::string_view mod_id
) {
    const auto entries = catalog.entries();
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
        return entry.mod_id == mod_id;
    });
    return found == entries.end() ? nullptr : &*found;
}

[[nodiscard]] bool has_diagnostic(
    const pulseforge::ContentCatalog& catalog,
    const std::string_view text
) {
    const auto diagnostics = catalog.diagnostics();
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& diagnostic) {
        return diagnostic.message.find(text) != std::string::npos;
    });
}

void test_basic_catalog(const std::filesystem::path& root) {
    const auto mod = root / "mods" / "test-mod";
    write_text(mod / "pack.json", R"json({"name":"Test Mod"})json");
    write_text(
        mod / "weeks" / "week-test.json",
        R"json({"weekName":"Test Week","songs":[["Cool Song","dad",[1,2,3]]]})json"
    );
    write_text(
        mod / "data" / "cool-song" / "cool-song-hard.json",
        R"json({"song":{"song":"Cool Song","bpm":120,"notes":[{"mustHitSection":true,"sectionNotes":[[1000,0,0]]}]}})json"
    );
    write_text(
        mod / "data" / "cool-song" / "script.lua",
        "function onCreate() end\n"
    );
    write_text(
        mod / "data" / "cool-song" / "events.json",
        R"json({"song":{"events":[]}})json"
    );

    const auto native = root / "native-mod";
    write_text(
        native / "mod.json",
        R"json({"id":"native.demo","name":"Native Demo","entryChart":"chart.json","scripts":["script.lua","extra.lua"]})json"
    );
    write_text(
        native / "chart.json",
        R"json({"format":"pulseforge-chart","version":1,"song":{"title":"Native","keyCount":4},"tempos":[{"time":0,"bpm":120}],"notes":[]})json"
    );
    write_text(native / "script.lua", "function onCreate() end\n");
    write_text(native / "extra.lua", "function onCreatePost() end\n");

    pulseforge::ContentCatalogOptions options;
    options.roots = {root / "mods", native};
    const auto catalog = pulseforge::ContentCatalog::scan(options);
    require(!catalog.truncated(), "small catalog must not truncate");
    require(catalog.entries().size() == 2, "catalog discovers two charts");

    const auto* psych = catalog.find("cool-song", "hard");
    require(psych != nullptr, "Psych song and difficulty are selectable");
    require(psych->mod_name == "Test Mod", "pack.json mod name is applied");
    require(psych->week == "Test Week", "week metadata is applied");
    require(psych->title == "Cool Song", "week song title is preserved");
    require(psych->script_path.has_value(), "Psych song script is discovered");
    require(psych->script_paths.size() == 1, "Psych script vector is populated");
    require(
        psych->script_path == psych->script_paths.front(),
        "legacy script path aliases the first script"
    );
    require(
        psych->profile == pulseforge::ContentProfile::psych,
        "pack.json selects the Psych profile"
    );
    require(
        psych->provenance == pulseforge::ContentProvenance::convention,
        "Psych data charts retain convention provenance"
    );
    require(
        psych->layout == pulseforge::ContentLayout::psych,
        "Psych layout is detected"
    );

    const auto* native_entry = catalog.find("NATIVE.DEMO:NATIVE-MOD:NORMAL");
    require(native_entry != nullptr, "stable ids are case-insensitive");
    require(
        native_entry->script_path.has_value(),
        "native manifest script is discovered"
    );
    require(
        native_entry->script_paths.size() == 2,
        "all safe manifest scripts are retained and de-duplicated"
    );
    require(
        native_entry->profile == pulseforge::ContentProfile::pulseforge,
        "native manifest selects the PulseForge profile"
    );
    require(
        native_entry->provenance == pulseforge::ContentProvenance::manifest,
        "entryChart retains manifest provenance"
    );
    require(
        native_entry->manifest_path.has_value(),
        "song provenance includes its manifest path"
    );
}

void write_scoped_mod(
    const std::filesystem::path& mods,
    const std::string_view id,
    const std::string_view week_name,
    const std::string_view display_title
) {
    const auto mod = mods / std::string(id);
    write_text(mod / "pack.json", "{\"name\":\"" + std::string(id) + "\"}");
    write_text(
        mod / "weeks" / "shared-week.json",
        "{\"weekName\":\"" + std::string(week_name)
            + "\",\"songs\":[[\"" + std::string(display_title)
            + "\",\"dad\",[1,2,3]]]}"
    );
    write_text(
        mod / "data" / "shared-song" / "shared-song.json",
        R"json({"song":{"song":"Shared Song","bpm":120,"notes":[{"mustHitSection":true,"sectionNotes":[]}]}})json"
    );
}

void test_mods_list_and_scoped_weeks(const std::filesystem::path& root) {
    const auto engine = root / "ordered-engine";
    const auto mods = engine / "mods";
    write_text(engine / "modsList.txt", "mod-b|1\nmod-a|0\n");
    write_scoped_mod(mods, "mod-a", "Disabled Week", "Shared Song");
    write_scoped_mod(mods, "mod-b", "Beta Week", "Shared Song");
    write_scoped_mod(mods, "mod-c", "Gamma Week", "Shared & Song");

    pulseforge::ContentCatalogOptions options;
    options.roots = {mods};
    const auto catalog = pulseforge::ContentCatalog::scan(options);
    require(catalog.entries().size() == 2, "disabled mods do not publish songs");
    require(catalog.mods().size() == 3, "enabled and disabled mods are described");
    require(catalog.mods()[0].id == "mod-b", "modsList first entry keeps priority");
    require(catalog.mods()[0].enabled, "enabled modsList state is retained");
    require(catalog.mods()[1].id == "mod-a", "disabled mod keeps list position");
    require(!catalog.mods()[1].enabled, "disabled modsList state is retained");
    require(catalog.mods()[2].id == "mod-c", "unlisted mods follow listed mods");
    require(catalog.mods()[2].enabled, "unlisted mods default to enabled");

    const auto* beta = entry_for_mod(catalog, "mod-b");
    const auto* gamma = entry_for_mod(catalog, "mod-c");
    require(beta != nullptr && gamma != nullptr, "enabled scoped songs exist");
    require(beta->week == "Beta Week", "mod B uses only its own week metadata");
    require(beta->title == "Shared Song", "mod B uses its scoped display title");
    require(gamma->week == "Gamma Week", "mod C uses only its own week metadata");
    require(gamma->title == "Shared & Song", "mod C uses its scoped display title");
}

void test_manifest_containment(const std::filesystem::path& root) {
    const auto outside_chart = root / "outside-chart.json";
    const auto outside_script = root / "outside.lua";
    write_text(
        outside_chart,
        R"json({"format":"pulseforge-chart","version":1,"song":{"title":"Outside","keyCount":4},"tempos":[{"time":0,"bpm":120}],"notes":[]})json"
    );
    write_text(outside_script, "function outside() end\n");

    const auto traversal = root / "traversal-mod";
    write_text(
        traversal / "mod.json",
        R"json({"id":"traversal","entryChart":"../outside-chart.json"})json"
    );

    const auto absolute = root / "absolute-mod";
    write_text(
        absolute / "mod.json",
        R"json({"id":"absolute","entryChart":"C:/outside-chart.json"})json"
    );

    const auto safe = root / "safe-mod";
    write_text(
        safe / "mod.json",
        R"json({"id":"safe","entryChart":"chart.json","scripts":["safe.lua","../outside.lua","C:/outside.lua"]})json"
    );
    write_text(
        safe / "chart.json",
        R"json({"format":"pulseforge-chart","version":1,"song":{"title":"Safe","keyCount":4},"tempos":[{"time":0,"bpm":120}],"notes":[]})json"
    );
    write_text(safe / "safe.lua", "function safe() end\n");

    const auto linked = root / "linked-mod";
    write_text(
        linked / "mod.json",
        R"json({"id":"linked","entryChart":"linked-chart.json"})json"
    );
    std::error_code symlink_error;
    std::filesystem::create_symlink(
        outside_chart,
        linked / "linked-chart.json",
        symlink_error
    );

    pulseforge::ContentCatalogOptions options;
    options.roots = {traversal, absolute, safe, linked};
    const auto catalog = pulseforge::ContentCatalog::scan(options);
    require(catalog.entries().size() == 1, "only the contained entry chart is accepted");
    const auto* safe_entry = catalog.find("safe:safe-mod:normal");
    require(safe_entry != nullptr, "safe manifest entry remains playable");
    require(safe_entry->script_paths.size() == 1, "unsafe scripts are rejected");
    require(
        safe_entry->script_paths.front().filename() == "safe.lua",
        "the contained script remains available"
    );
    require(
        has_diagnostic(catalog, "absolute or contains '..'"),
        "absolute and parent traversal paths produce diagnostics"
    );
    if (!symlink_error) {
        require(
            has_diagnostic(catalog, "escapes the mod root"),
            "a symlink escape is rejected after canonicalization"
        );
    }
}

void write_duplicate_mod(const std::filesystem::path& root) {
    write_text(
        root / "mod.json",
        R"json({"id":"duplicate","entryChart":"data/same/same.json"})json"
    );
    write_text(
        root / "data" / "same" / "same.json",
        R"json({"song":{"song":"Same","bpm":120,"notes":[{"mustHitSection":true,"sectionNotes":[]}]}})json"
    );
}

void test_deterministic_ids(const std::filesystem::path& root) {
    const auto first = root / "first";
    const auto second = root / "second";
    write_duplicate_mod(first);
    write_duplicate_mod(second);

    pulseforge::ContentCatalogOptions options;
    options.roots = {second, first};
    const auto first_scan = pulseforge::ContentCatalog::scan(options);
    const auto second_scan = pulseforge::ContentCatalog::scan(options);
    require(first_scan.entries().size() == 2, "duplicate logical ids keep both charts");
    require(
        first_scan.entries()[0].id == "duplicate:same:normal",
        "the first deterministic collision keeps the base id"
    );
    require(
        first_scan.entries()[1].id == "duplicate:same:normal#2",
        "the second deterministic collision gets a stable suffix"
    );
    require(
        first_scan.entries()[0].title == "Same-1"
            && first_scan.entries()[1].title == "Same-2",
        "duplicate chart display names keep every version with stable suffixes"
    );
    require(
        first_scan.entries().size() == second_scan.entries().size(),
        "repeat scans return the same entry count"
    );
    for (std::size_t index = 0; index < first_scan.entries().size(); ++index) {
        require(
            first_scan.entries()[index].id == second_scan.entries()[index].id,
            "repeat scans return identical ids"
        );
        require(
            first_scan.entries()[index].chart_path
                == second_scan.entries()[index].chart_path,
            "repeat scans return identical path order"
        );
    }
}

void test_vslice_metadata_variants(const std::filesystem::path& root) {
    const auto song = root / "data" / "universal-test";
    write_text(
        song / "universal-test-chart.json",
        R"json({"version":"2.2.4","notes":{"easy":[],"normal":[],"hard":[]},"events":[]})json"
    );
    write_text(
        song / "metadata.json",
        R"json({"version":"2.2.4","songName":"Universal Test","artist":"Test","charter":"Test","playData":{"difficulties":["easy","normal","hard"],"stage":"mainStage","noteStyle":"funkin","characters":{"player":"bf","girlfriend":"gf","opponent":"dad"}}})json"
    );

    pulseforge::ContentCatalogOptions options;
    options.roots = {root};
    const auto catalog = pulseforge::ContentCatalog::scan(options);
    require(catalog.entries().size() == 3U, "V-Slice metadata expands difficulties");
    for (const auto difficulty : {"easy", "normal", "hard"}) {
        const auto* entry = catalog.find("universal-test", difficulty);
        require(entry != nullptr, "each V-Slice difficulty is selectable");
        require(entry->title == "Universal Test", "V-Slice display name is retained");
        require(entry->metadata_path.has_value(), "V-Slice metadata path is retained");
    }
}

void test_loose_root_chart(const std::filesystem::path& root) {
    write_text(root / "pack.json", R"json({"name":"Recovered Charts"})json");
    write_text(
        root / "loose-hard.json",
        R"json({"song":{"song":"Loose","bpm":120,"notes":[{"mustHitSection":true,"sectionNotes":[[100,0,0]]}]}})json"
    );
    write_text(
        root / "character-descriptor.json",
        R"json({"image":"characters/test","animations":[{"anim":"idle"}]})json"
    );
    pulseforge::ContentCatalogOptions options;
    options.roots = {root};
    const auto catalog = pulseforge::ContentCatalog::scan(options);
    require(catalog.entries().size() == 1U, "loose root charts are sniffed safely");
    require(catalog.find("loose", "hard") != nullptr, "root chart keeps difficulty");
}

void test_nested_packaged_engine_content_root(
    const std::filesystem::path& root
) {
    const auto pack = root / "mods" / "packaged-engine";
    write_text(pack / "mod.json", R"json({"id":"packaged"})json");
    write_text(
        pack / "bin" / "assets" / "data" / "charts" / "nested" / "nested.json",
        R"json({"song":{"song":"Nested","bpm":120,"notes":[{"mustHitSection":true,"sectionNotes":[]}]}})json"
    );
    write_text(
        pack / "bin" / "assets" / "songs" / "nested" / "Inst.ogg",
        "fixture"
    );

    pulseforge::ContentCatalogOptions options;
    options.roots = {root / "mods"};
    const auto catalog = pulseforge::ContentCatalog::scan(options);
    const auto* entry = catalog.find("nested");
    require(entry != nullptr, "nested packaged chart is discovered");
    const auto expected_content_root = std::filesystem::weakly_canonical(
        pack / "bin" / "assets"
    );
    require(
        entry->content_root == expected_content_root,
        "nested packaged chart resolves its sibling asset root"
    );
    require(
        entry->mod_root == std::filesystem::weakly_canonical(pack),
        "nested packaged chart retains the outer provenance boundary"
    );
}

void test_persistent_layout_cache_and_staging_ignore(
    const std::filesystem::path& root
) {
    const auto chart = root / "data" / "cached" / "cached.json";
    write_text(
        chart,
        R"json({"song":{"song":"Cached","bpm":120,"notes":[{"mustHitSection":true,"sectionNotes":[]}]}})json"
    );
    write_text(
        root / ".pulseforge-import-stale" / "data" / "hidden"
            / "hidden.json",
        R"json({"song":{"song":"Hidden","bpm":120,"notes":[{"mustHitSection":true,"sectionNotes":[]}]}})json"
    );

    pulseforge::ContentCatalogOptions options;
    options.roots = {root};
    options.cache_path = root / ".state" / "catalog.json";

    const auto cold = pulseforge::ContentCatalog::scan(options);
    require(cold.entries().size() == 1U, "staging directory is never indexed");
    require(cold.find("cached") != nullptr, "cold scan discovers cached chart");
    require(cold.cache_hits() == 0U, "cold scan has no persistent hits");
    require(cold.cache_misses() >= 1U, "cold scan records a layout miss");
    require(
        std::filesystem::is_regular_file(options.cache_path),
        "cold scan writes the persistent layout cache"
    );

    const auto warm = pulseforge::ContentCatalog::scan(options);
    require(warm.entries().size() == 1U, "warm scan retains the chart");
    require(warm.cache_hits() >= 1U, "warm scan reuses cached layout metadata");
    require(warm.cache_misses() == 0U, "unchanged chart avoids a prefix read");

    write_text(
        chart,
        R"json({"notAChart":true,"changedSizeForInvalidation":"yes"})json"
    );
    const auto changed = pulseforge::ContentCatalog::scan(options);
    require(changed.entries().empty(), "changed chart is not served stale");
    require(changed.cache_hits() == 0U, "changed size invalidates cache hit");
    require(changed.cache_misses() >= 1U, "changed size triggers a new probe");

    write_text(
        chart,
        R"json({"song":{"song":"Cached","bpm":120,"notes":[{"mustHitSection":true,"sectionNotes":[[10,0,0]]}]}})json"
    );
    options.probe_bytes *= 2U;
    const auto reprobed = pulseforge::ContentCatalog::scan(options);
    require(reprobed.entries().size() == 1U, "probe change keeps valid chart");
    require(
        reprobed.cache_hits() == 0U && reprobed.cache_misses() >= 1U,
        "probe-size change invalidates the cache schema"
    );
}

}  // namespace

int main() {
    try {
        TemporaryDirectory temporary;
        test_basic_catalog(temporary.path() / "basic");
        test_mods_list_and_scoped_weeks(temporary.path() / "scoped");
        test_manifest_containment(temporary.path() / "containment");
        test_deterministic_ids(temporary.path() / "determinism");
        test_vslice_metadata_variants(temporary.path() / "vslice");
        test_loose_root_chart(temporary.path() / "loose");
        test_nested_packaged_engine_content_root(temporary.path() / "nested");
        test_persistent_layout_cache_and_staging_ignore(
            temporary.path() / "cache"
        );

        std::cout << "Content catalog tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "Content catalog tests failed: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
