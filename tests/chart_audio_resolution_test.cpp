#include "pulseforge/chart_loader.hpp"

#include <chrono>
#include <cstdlib>
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
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(static_cast<bool>(output), "fixture JSON can be written");
}

void touch(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.put('\0');
    require(static_cast<bool>(output), "fixture audio can be written");
}

[[nodiscard]] bool same_path(
    const std::filesystem::path& left,
    const std::filesystem::path& right
) {
    // PULSEFORGE_P1_2_0B_AUDIO_TEST_FILESYSTEM_EQUIVALENCE_V1
    // Stock-provider discovery intentionally normalizes/canonicalizes paths.
    // On Windows two spellings can therefore identify the same file while
    // comparing unequal as path strings (case, short/long names, junctions).
    // Test the filesystem identity first so the regression checks the selected
    // audio file rather than the spelling used to reach it.
    if (!left.empty() && !right.empty()) {
        std::error_code error;
        const bool equivalent = std::filesystem::equivalent(left, right, error);
        if (!error) {
            return equivalent;
        }
    }
    return left.lexically_normal() == right.lexically_normal();
}

void test_psych_mixed_stems(const std::filesystem::path& root) {
    const auto chart = root / "psych-mod" / "data" / "test-song"
        / "test-song-hard.json";
    const auto song = root / "psych-mod" / "songs" / "test-song";
    write_text(
        chart,
        R"json({"song":{"song":"test-song","bpm":180,"needsVoices":true,"notes":[{"mustHitSection":true,"sectionNotes":[[100,0,0]]}]}})json"
    );
    touch(song / "Inst.ogg");
    touch(song / "Voices.wav");
    touch(song / "Voices-dad.flac");
    touch(song / "not-this-song.mp3");

    const auto loaded = pulseforge::ChartLoader::load(chart);
    require(static_cast<bool>(loaded), "Psych fixture loads");
    require(
        same_path(loaded.chart->audio.instrumental, song / "Inst.ogg"),
        "Psych Inst is selected from the matching songs directory"
    );
    require(
        loaded.chart->audio.vocals.size() == 2U,
        "all conventional vocal stems are selected across extensions"
    );
    require(
        same_path(loaded.chart->audio.vocals[0], song / "Voices-dad.flac")
            || same_path(loaded.chart->audio.vocals[1], song / "Voices-dad.flac"),
        "character-specific V-Slice/Psych vocal stem is retained"
    );
}

void test_vslice_assets_layout(const std::filesystem::path& root) {
    const auto assets = root / "funkin-assets";
    const auto data = assets / "data" / "songs" / "fresh-test";
    const auto chart = data / "fresh-test-chart.json";
    const auto metadata = data / "fresh-test-metadata.json";
    const auto song = assets / "songs" / "fresh-test";
    write_text(
        chart,
        R"json({"version":"2.2.4","notes":{"normal":[]},"events":[]})json"
    );
    write_text(
        metadata,
        R"json({"version":"2.2.4","songName":"Fresh Test","timeChanges":[{"t":0,"bpm":210}],"playData":{"difficulties":["normal"],"stage":"mainStage","noteStyle":"funkin","characters":{"player":"bf","girlfriend":"gf","opponent":"dad"}}})json"
    );
    touch(song / "Inst-normal.mp3");
    touch(song / "Voices-bf.ogg");
    touch(song / "Voices-dad.wav");

    pulseforge::ChartLoadOptions options;
    options.metadata_path = metadata;
    const auto loaded = pulseforge::ChartLoader::load(chart, options);
    require(static_cast<bool>(loaded), "V-Slice fixture loads");
    require(
        same_path(loaded.chart->audio.instrumental, song / "Inst-normal.mp3"),
        "V-Slice difficulty Inst is found under assets/songs"
    );
    require(
        loaded.chart->audio.vocals.size() == 2U,
        "V-Slice split character vocals are discovered"
    );
}

void test_difficulty_mix_is_isolated(const std::filesystem::path& root) {
    const auto data = root / "difficulty-mod" / "data" / "blammed";
    const auto song = root / "difficulty-mod" / "songs" / "blammed";
    const auto normal_chart = data / "blammed.json";
    const auto erect_chart = data / "blammed-erect.json";
    const auto chart_json =
        R"json({"song":{"song":"blammed","bpm":120,"needsVoices":true,"notes":[{"mustHitSection":true,"sectionNotes":[]}]}})json";
    write_text(normal_chart, chart_json);
    write_text(erect_chart, chart_json);
    touch(song / "Inst.ogg");
    touch(song / "Inst-erect.ogg");
    touch(song / "Voices-bf.ogg");
    touch(song / "Voices-pico.ogg");
    touch(song / "Voices-bf-erect.ogg");
    touch(song / "Voices-pico-erect.ogg");

    pulseforge::ChartLoadOptions normal_options;
    normal_options.difficulty = "normal";
    normal_options.difficulty_explicit = true;
    const auto normal = pulseforge::ChartLoader::load(
        normal_chart,
        normal_options
    );
    require(static_cast<bool>(normal), "normal mix fixture loads");
    require(
        same_path(normal.chart->audio.instrumental, song / "Inst.ogg"),
        "normal difficulty selects the unsuffixed instrumental"
    );
    require(
        normal.chart->audio.vocals.size() == 2U,
        "normal difficulty excludes erect vocal stems"
    );
    for (const auto& vocal : normal.chart->audio.vocals) {
        require(
            vocal.stem().string().find("erect") == std::string::npos,
            "normal mix contains no erect voice"
        );
    }

    pulseforge::ChartLoadOptions erect_options;
    erect_options.difficulty = "erect";
    erect_options.difficulty_explicit = true;
    const auto erect = pulseforge::ChartLoader::load(
        erect_chart,
        erect_options
    );
    require(static_cast<bool>(erect), "erect mix fixture loads");
    require(
        same_path(erect.chart->audio.instrumental, song / "Inst-erect.ogg"),
        "erect difficulty selects its matching instrumental"
    );
    require(
        erect.chart->audio.vocals.size() == 2U,
        "erect difficulty selects only its matching split vocals"
    );
    for (const auto& vocal : erect.chart->audio.vocals) {
        require(
            vocal.stem().string().find("erect") != std::string::npos,
            "erect mix contains no normal voice"
        );
    }
}

void test_loose_named_audio_is_not_global_fallback(
    const std::filesystem::path& root
) {
    const auto loose = root / "loose-pack";
    const auto matching_chart = loose / "ntuzongere_kugaruka.json";
    const auto other_chart = loose / "different-song.json";
    const auto named_audio = loose
        / "Ntuzongere_Kugaruka_my_fastest_song_ever.ogg";
    write_text(
        matching_chart,
        R"json({"song":{"song":"Ntuzongere Kugaruka","bpm":522,"notes":[{"mustHitSection":true,"sectionNotes":[]}]}})json"
    );
    write_text(
        other_chart,
        R"json({"song":{"song":"Different Song","bpm":120,"notes":[{"mustHitSection":true,"sectionNotes":[]}]}})json"
    );
    touch(named_audio);

    const auto matching = pulseforge::ChartLoader::load(matching_chart);
    require(static_cast<bool>(matching), "matching loose chart loads");
    require(
        same_path(matching.chart->audio.instrumental, named_audio),
        "song-prefixed loose audio is associated with its chart"
    );

    const auto other = pulseforge::ChartLoader::load(other_chart);
    require(static_cast<bool>(other), "unrelated loose chart loads");
    require(
        other.chart->audio.instrumental.empty()
            && other.chart->audio.vocals.empty(),
        "unrelated loose audio is never used as a global fallback"
    );
}

void test_native_explicit_audio_wins(const std::filesystem::path& root) {
    const auto directory = root / "native";
    const auto chart = directory / "chart.json";
    const auto explicit_inst = directory / "custom-mix.wav";
    write_text(
        chart,
        R"json({"format":"pulseforge-chart","song":{"title":"Native"},"audio":{"instrumental":"custom-mix.wav","vocals":[]},"tempos":[{"time":0,"bpm":333}],"notes":[]})json"
    );
    touch(explicit_inst);
    touch(directory / "Inst.ogg");

    const auto loaded = pulseforge::ChartLoader::load(chart);
    require(static_cast<bool>(loaded), "native fixture loads");
    require(
        same_path(loaded.chart->audio.instrumental, explicit_inst),
        "an explicit manifest path always wins over convention discovery"
    );
}


void create_stock_provider(
    const std::filesystem::path& assets
) {
    for (const auto* song : {"tutorial", "bopeebo", "fresh", "dad-battle"}) {
        std::filesystem::create_directories(assets / "songs" / song);
    }
    std::filesystem::create_directories(assets / "images");
    std::filesystem::create_directories(assets / "sounds");
    std::filesystem::create_directories(assets / "music");
    std::filesystem::create_directories(assets / "shared");
    std::filesystem::create_directories(assets / "data" / "characters");
}

void test_stock_song_audio_uses_complete_sibling_provider(
    const std::filesystem::path& root
) {
    const auto install = root / "stock-fallback-install";
    std::filesystem::create_directories(install / "assets");
    const auto chart = install / "mods" / "thin-mod" / "data" / "bopeebo"
        / "bopeebo.json";
    write_text(
        chart,
        R"json({"song":{"song":"bopeebo","bpm":100,"needsVoices":true,"notes":[{"mustHitSection":true,"sectionNotes":[]}]}})json"
    );

    const auto provider_assets = install / "mods" / "stock-engine" / "assets";
    create_stock_provider(provider_assets);
    touch(provider_assets / "songs" / "bopeebo" / "Inst.ogg");
    touch(provider_assets / "songs" / "bopeebo" / "Voices.ogg");

    const auto loaded = pulseforge::ChartLoader::load(chart);
    require(static_cast<bool>(loaded), "stock fallback chart loads");
    require(
        same_path(
            loaded.chart->audio.instrumental,
            provider_assets / "songs" / "bopeebo" / "Inst.ogg"
        ),
        "stock base-game Inst resolves from one complete sibling provider"
    );
    require(
        loaded.chart->audio.vocals.size() == 1U
            && same_path(
                loaded.chart->audio.vocals.front(),
                provider_assets / "songs" / "bopeebo" / "Voices.ogg"
            ),
        "stock base-game Voices resolves from the same provider"
    );
}

void test_custom_song_never_bleeds_from_sibling_provider(
    const std::filesystem::path& root
) {
    const auto install = root / "custom-isolation-install";
    std::filesystem::create_directories(install / "assets");
    const auto chart = install / "mods" / "thin-mod" / "data" / "custom-song"
        / "custom-song.json";
    write_text(
        chart,
        R"json({"song":{"song":"custom-song","bpm":120,"needsVoices":true,"notes":[{"mustHitSection":true,"sectionNotes":[]}]}})json"
    );
    const auto provider_assets = install / "mods" / "stock-engine" / "assets";
    create_stock_provider(provider_assets);
    touch(provider_assets / "songs" / "custom-song" / "Inst.ogg");
    touch(provider_assets / "songs" / "custom-song" / "Voices.ogg");

    const auto loaded = pulseforge::ChartLoader::load(chart);
    require(static_cast<bool>(loaded), "custom isolation chart loads");
    require(
        loaded.chart->audio.instrumental.empty()
            && loaded.chart->audio.vocals.empty(),
        "custom song audio never leaks across sibling mod packs"
    );
}


void test_stock_vocals_can_fill_thin_base_song_override(
    const std::filesystem::path& root
) {
    const auto install = root / "stock-vocal-fallback-install";
    std::filesystem::create_directories(install / "assets");
    const auto local_song = install / "mods" / "thin-mod" / "songs" / "bopeebo";
    const auto chart = install / "mods" / "thin-mod" / "data" / "bopeebo"
        / "bopeebo.json";
    write_text(
        chart,
        R"json({"song":{"song":"bopeebo","bpm":100,"needsVoices":true,"notes":[{"mustHitSection":true,"sectionNotes":[]}]}})json"
    );
    touch(local_song / "Inst.ogg");

    const auto provider_assets = install / "mods" / "stock-engine" / "assets";
    create_stock_provider(provider_assets);
    touch(provider_assets / "songs" / "bopeebo" / "Inst.ogg");
    touch(provider_assets / "songs" / "bopeebo" / "Voices.ogg");

    const auto loaded = pulseforge::ChartLoader::load(chart);
    require(static_cast<bool>(loaded), "thin base-song override loads");
    require(
        same_path(loaded.chart->audio.instrumental, local_song / "Inst.ogg"),
        "local mod Inst overrides stock provider"
    );
    require(
        loaded.chart->audio.vocals.size() == 1U
            && same_path(
                loaded.chart->audio.vocals.front(),
                provider_assets / "songs" / "bopeebo" / "Voices.ogg"
            ),
        "missing local base-song Voices falls back to stock provider"
    );
}

}  // namespace

int main() {
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    const auto root = std::filesystem::temp_directory_path()
        / ("pulseforge-chart-audio-" + unique);
    try {
        test_psych_mixed_stems(root);
        test_vslice_assets_layout(root);
        test_difficulty_mix_is_isolated(root);
        test_loose_named_audio_is_not_global_fallback(root);
        test_native_explicit_audio_wins(root);
        test_stock_song_audio_uses_complete_sibling_provider(root);
        test_custom_song_never_bleeds_from_sibling_provider(root);
        test_stock_vocals_can_fill_thin_base_song_override(root);
        std::filesystem::remove_all(root);
        std::cout << "chart audio resolution tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::cerr << "chart audio resolution test failed: "
                  << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
