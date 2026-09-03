#include "pulseforge/chart_loader.hpp"
#include "pulseforge/streaming_chart_editor.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto nonce = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
        const auto base = std::filesystem::temp_directory_path();
        for (std::uint32_t attempt = 0U; attempt < 1'024U; ++attempt) {
            path_ = base / (
                "pulseforge-streaming-editor-test-"
                + std::to_string(nonce) + '-' + std::to_string(attempt)
            );
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                owned_ = true;
                return;
            }
            if (error) {
                throw std::runtime_error("cannot create test directory");
            }
        }
        throw std::runtime_error("cannot reserve test directory");
    }

    ~TemporaryDirectory() {
        if (owned_) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
    bool owned_{};
};

void run_test() {
    TemporaryDirectory temporary;
    pulseforge::PackedChartData packed;
    packed.key_count = 4U;
    packed.kinds = {"normal", "mine"};
    for (std::uint16_t lane = 0U; lane < 4U; ++lane) {
        packed.notes.push_back({
            static_cast<std::int64_t>(lane) * 1'000,
            0U,
            lane,
            pulseforge::PackedNoteOwner::opponent,
            0U,
            0U,
        });
    }
    packed.notes.push_back({
        4'000,
        500U,
        0U,
        pulseforge::PackedNoteOwner::player,
        0U,
        1U,
    });
    packed.notes.push_back({
        5'000,
        0U,
        1U,
        pulseforge::PackedNoteOwner::player,
        0U,
        0U,
    });

    const auto pfc = temporary.path() / "source.pfc";
    pulseforge::PackedChartWriteOptions write_options;
    write_options.max_notes_per_chunk = 2U;
    std::string error;
    require(
        pulseforge::write_packed_chart(pfc, packed, write_options, &error),
        error
    );

    pulseforge::PackedChartLimits read_limits;
    read_limits.max_query_notes = 2U;
    auto reader = pulseforge::PackedChartReader::open(pfc, &error, read_limits);
    require(reader.has_value(), error);

    const auto indexed = reader->read_indexed_explicit_notes_in_range(
        0,
        5'000,
        2U
    );
    require(static_cast<bool>(indexed), indexed.error);
    require(indexed.truncated, "indexed query reports dense truncation");
    require(indexed.notes.size() == 2U, "indexed query preserves bounded prefix");
    require(
        indexed.notes[0U].index == 0U && indexed.notes[1U].index == 1U,
        "indexed query exposes stable PFC indices"
    );

    pulseforge::Chart metadata;
    metadata.title = "Huge Editable";
    metadata.artist = "Tests";
    metadata.charter = "PulseForge";
    metadata.player_character = "bf";
    metadata.opponent_character = "dad";
    metadata.girlfriend_character = "gf";
    metadata.stage_id = "stage";
    metadata.note_style = "NOTE_assets";
    metadata.chart_scroll_speed = 2.0;
    metadata.tempos.push_back({0.0, 120.0, 4U, 4U});

    pulseforge::StreamingChartEditor editor(
        std::move(*reader),
        std::move(metadata),
        temporary.path() / "source.json",
        6'000U
    );
    const auto dense = editor.query(0, 5'000, 2U);
    require(static_cast<bool>(dense), dense.error);
    require(dense.dense_lod, "dense viewport switches to explicit LOD");
    require(!dense.density_spans.empty(), "dense viewport represents all chunks");

    require(editor.remove_note(1U, &error), error);
    auto changed = packed.notes[1U];
    changed.time_us = 1'250;
    changed.lane = 3U;
    require(editor.update_note(2U, changed, &error), error);
    const auto added = editor.add_note({
        2'500,
        250U,
        2U,
        pulseforge::PackedNoteOwner::player,
        0U,
        1U,
    }, &error);
    require(added.has_value(), error);
    require(editor.note_count() == packed.notes.size(), "overlay count is exact");

    pulseforge::EditorStorage storage(temporary.path());
    require(storage.ready(), storage.initialization_error());
    const auto patch = editor.save_patch(storage, "edits/huge.pfpatch.json");
    require(static_cast<bool>(patch), patch.message);
    const auto psych = editor.export_psych_json(
        storage,
        "charts/huge/huge.json"
    );
    require(static_cast<bool>(psych), psych.message);

    const auto loaded = pulseforge::ChartLoader::load(psych.path);
    require(static_cast<bool>(loaded), loaded.error);
    require(
        loaded.chart->notes.size() == packed.notes.size(),
        "streaming Psych export preserves overlay note count"
    );
    require(
        loaded.chart->notes.front().time_ms == 1.25,
        "deleted source note is absent and updated timestamp survives export"
    );
}

void run_selection_survives_viewport_test() {
    TemporaryDirectory temporary;
    pulseforge::PackedChartData packed;
    packed.key_count = 4U;
    packed.kinds = {"normal", "Hurt Note"};
    packed.notes = {
        {
            1'000,
            0U,
            1U,
            pulseforge::PackedNoteOwner::player,
            0U,
            0U,
        },
        {
            50'000'000,
            0U,
            2U,
            pulseforge::PackedNoteOwner::player,
            0U,
            0U,
        },
    };
    const auto pfc = temporary.path() / "stable-selection.pfc";
    std::string error;
    require(pulseforge::write_packed_chart(pfc, packed, {}, &error), error);
    auto reader = pulseforge::PackedChartReader::open(pfc, &error);
    require(reader.has_value(), error);
    pulseforge::Chart metadata;
    metadata.tempos.push_back({0.0, 120.0, 4U, 4U});
    pulseforge::StreamingChartEditor editor(
        std::move(*reader),
        std::move(metadata),
        pfc,
        50'000'000U
    );

    const auto selected_view = editor.query(0, 2'000, 16U);
    require(static_cast<bool>(selected_view), selected_view.error);
    require(selected_view.notes.size() == 1U, "initial viewport selects one note");
    const auto selected_id = selected_view.notes.front().id;

    // Model a modal picker remaining open while audio advances far enough that
    // the current viewport no longer contains the originally selected note.
    const auto advanced_view = editor.query(49'000'000, 51'000'000, 16U);
    require(static_cast<bool>(advanced_view), advanced_view.error);
    require(
        std::none_of(
            advanced_view.notes.begin(),
            advanced_view.notes.end(),
            [selected_id](const pulseforge::StreamingEditorViewportNote& note) {
                return note.id == selected_id;
            }
        ),
        "advanced viewport no longer contains the selected note"
    );

    auto stable_note = editor.note_by_id(selected_id, &error);
    require(stable_note.has_value(), error);
    stable_note->kind_id = 1U;
    require(editor.update_note(selected_id, *stable_note, &error), error);
    const auto changed = editor.note_by_id(selected_id, &error);
    require(
        changed.has_value() && changed->kind_id == 1U,
        "note type applies by stable id after the viewport advances"
    );

    require(editor.remove_note(selected_id, &error), error);
    error.clear();
    require(
        !editor.note_by_id(selected_id, &error).has_value()
            && error.find("removed") != std::string::npos,
        "a genuinely removed selected note returns explicit feedback"
    );
}

void run_pattern_and_patch_test() {
    TemporaryDirectory temporary;
    pulseforge::PackedChartData packed;
    packed.key_count = 4U;
    packed.kinds = {"normal", "mine"};
    packed.notes.push_back({
        0,
        0U,
        0U,
        pulseforge::PackedNoteOwner::opponent,
        0U,
        0U,
    });
    pulseforge::PatternRun pattern;
    pattern.start_us = 1'000'000;
    pattern.interval_us = 1'000'000U;
    pattern.count = 5U;
    pattern.duration_us = 250'000U;
    pattern.lane_pattern = {0U, 1U};
    pattern.owner = pulseforge::PackedNoteOwner::player;
    pattern.kind_id = 0U;
    packed.patterns.push_back(pattern);

    const auto pfc = temporary.path() / "pattern-source.pfc";
    std::string error;
    require(pulseforge::write_packed_chart(pfc, packed, {}, &error), error);
    auto reader = pulseforge::PackedChartReader::open(pfc, &error);
    require(reader.has_value(), error);

    pulseforge::Chart metadata;
    metadata.title = "Pattern Source";
    metadata.artist = "Pattern Artist";
    metadata.charter = "Pattern Charter";
    metadata.difficulty = "insane";
    metadata.player_character = "bf-test";
    metadata.opponent_character = "dad-test";
    metadata.girlfriend_character = "gf-test";
    metadata.stage_id = "stage-test";
    metadata.note_style = "pixel-test";
    metadata.chart_scroll_speed = 1.75;
    metadata.tempos = {
        {0.0, 120.0, 4U, 4U},
        {1'500.0, 180.0, 3U, 4U},
    };
    metadata.events.push_back({
        2'000.0,
        "Change Character",
        "dad",
        "spooky",
        "{\"extra\":true}",
    });
    metadata.note_payloads = {"", "{\"skin\":\"custom\"}"};
    metadata.audio.instrumental = "songs/pattern/Inst.ogg";
    metadata.audio.vocals = {"songs/pattern/Voices.ogg"};
    const std::vector<std::string> scripts{
        "scripts/song.lua",
        "scripts/events.lua",
    };
    constexpr std::uint64_t source_fingerprint = 0x1234'5678'90AB'CDEFULL;

    pulseforge::StreamingChartEditor editor(
        std::move(*reader),
        metadata,
        temporary.path() / "pattern-source.json",
        6'250'000U,
        scripts,
        source_fingerprint
    );
    require(editor.source_note_count() == 6U, "PatternRun counts logically");
    require(
        editor.explicit_source_note_count() == 1U,
        "explicit and logical counts remain distinguishable"
    );
    const auto bounded = editor.query(0, 10'000'000, 2U);
    require(static_cast<bool>(bounded), bounded.error);
    require(bounded.notes.size() == 2U, "pattern viewport respects its bound");
    require(bounded.dense_lod, "unexpanded PatternRun activates density LOD");
    std::uint64_t represented = 0U;
    for (const auto& span : bounded.density_spans) {
        represented += span.note_count;
    }
    require(represented == 4U, "PatternRun density represents every omitted note");

    require(editor.remove_note(2U, &error), error);
    require(editor.set_note_style("NOTE_assets-classic", &error), error);
    require(
        editor.metadata().note_style == "NOTE_assets-classic",
        "streaming editor changes the chart-wide note skin without materializing"
    );
    require(
        !editor.set_note_style(
            std::string{"bad-"} + static_cast<char>(0xC3U),
            &error
        ),
        "streaming editor rejects an incomplete UTF-8 note skin ID"
    );
    error.clear();
    require(
        !editor.ensure_note_kind(
            std::string{"broken-"} + static_cast<char>(0xC3U),
            &error
        ).has_value(),
        "streaming editor rejects an incomplete UTF-8 note type"
    );
    error.clear();
    const auto custom_kind = editor.ensure_note_kind("the note", &error);
    require(custom_kind.has_value(), error);
    auto selected_pattern = editor.note_by_id(3U, &error);
    require(
        selected_pattern.has_value()
            && selected_pattern->time_us == 2'000'000,
        error.empty() ? "PatternRun stable-id lookup failed" : error
    );
    auto changed = *selected_pattern;
    changed.time_us = 2'250'000;
    changed.lane = 3U;
    require(editor.update_note(3U, changed, &error), error);
    const auto added = editor.add_note({
        7'000'000,
        0U,
        2U,
        pulseforge::PackedNoteOwner::player,
        0U,
        *custom_kind,
    }, &error);
    require(added.has_value(), error);
    require(editor.note_count() == 6U, "pattern overlay count remains exact");
    require(
        editor.content_end_us() == 7'000'000U,
        "notes added beyond the source extend the editable timeline"
    );

    pulseforge::EditorStorage storage(temporary.path());
    require(storage.ready(), storage.initialization_error());
    const auto patch_path = std::filesystem::path{"edits/pattern.pfpatch.json"};
    const auto saved = editor.save_patch(storage, patch_path);
    require(static_cast<bool>(saved), saved.message);
    require(!editor.dirty(), "saving a patch advances the saved revision");

    auto fresh_reader = pulseforge::PackedChartReader::open(pfc, &error);
    require(fresh_reader.has_value(), error);
    pulseforge::Chart fallback;
    fallback.title = "Wrong fallback";
    pulseforge::StreamingChartEditor reopened(
        std::move(*fresh_reader),
        std::move(fallback),
        temporary.path() / "pattern-source.json",
        6'250'000U,
        {},
        source_fingerprint
    );
    const auto loaded_patch = reopened.load_patch(storage, patch_path);
    require(static_cast<bool>(loaded_patch), loaded_patch.message);
    require(!reopened.dirty(), "a reopened patch is a clean saved baseline");
    require(reopened.note_count() == 6U, "reopened overlay count is exact");
    require(
        reopened.metadata().title == metadata.title,
        "patch restores chart metadata instead of losing it"
    );
    require(
        reopened.metadata().note_style == "NOTE_assets-classic",
        "patch restores a freely selected note skin ID"
    );
    require(
        reopened.metadata().tempos.size() == metadata.tempos.size()
            && reopened.metadata().tempos[1U].time_ms
                == metadata.tempos[1U].time_ms
            && reopened.metadata().tempos[1U].bpm
                == metadata.tempos[1U].bpm
            && reopened.metadata().tempos[1U].numerator
                == metadata.tempos[1U].numerator
            && reopened.metadata().tempos[1U].denominator
                == metadata.tempos[1U].denominator,
        "patch restores every BPM/signature change"
    );
    require(
        reopened.metadata().events.size() == 1U
            && reopened.metadata().events.front().payload_json
                == "{\"extra\":true}",
        "patch restores events and extended payloads"
    );
    require(reopened.scripts() == scripts, "patch restores script paths");
    require(
        reopened.note_kinds().size() == 3U
            && reopened.note_kinds()[2U] == "the note",
        "patch v3 restores custom note types without rewriting PFC1"
    );
    require(
        reopened.metadata().audio.instrumental
            == metadata.audio.instrumental
            && reopened.metadata().audio.vocals == metadata.audio.vocals,
        "patch restores the audio manifest"
    );

    auto wrong_reader = pulseforge::PackedChartReader::open(pfc, &error);
    require(wrong_reader.has_value(), error);
    pulseforge::StreamingChartEditor wrong_source(
        std::move(*wrong_reader),
        metadata,
        temporary.path() / "same-shape-different-source.json",
        6'250'000U,
        scripts,
        source_fingerprint + 1U
    );
    const auto wrong_source_patch = wrong_source.load_patch(storage, patch_path);
    require(
        !wrong_source_patch,
        "a same-shape patch with another source fingerprint is rejected"
    );

    const auto reopened_view = reopened.query(0, 10'000'000, 100U);
    require(static_cast<bool>(reopened_view), reopened_view.error);
    require(reopened_view.notes.size() == 6U, "reopened pattern edits are visible");
    require(
        std::none_of(
            reopened_view.notes.begin(),
            reopened_view.notes.end(),
            [](const pulseforge::StreamingEditorViewportNote& note) {
                return note.id == 2U;
            }
        ),
        "deleted PatternRun occurrence remains deleted"
    );
    const auto updated = std::find_if(
        reopened_view.notes.begin(),
        reopened_view.notes.end(),
        [](const pulseforge::StreamingEditorViewportNote& note) {
            return note.id == 3U;
        }
    );
    require(
        updated != reopened_view.notes.end()
            && updated->note.time_us == 2'250'000
            && updated->note.lane == 3U,
        "updated PatternRun occurrence keeps its stable id"
    );

    const auto psych = reopened.export_psych_json(
        storage,
        "charts/pattern/pattern.json"
    );
    require(static_cast<bool>(psych), psych.message);
    const auto loaded_chart = pulseforge::ChartLoader::load(psych.path);
    require(static_cast<bool>(loaded_chart), loaded_chart.error);
    require(
        loaded_chart.chart->notes.size() == 6U,
        "PatternRun export streams every logical note"
    );
    require(
        std::any_of(
            loaded_chart.chart->notes.begin(),
            loaded_chart.chart->notes.end(),
            [](const pulseforge::Note& note) { return note.kind == "the note"; }
        ),
        "custom streaming note type survives compatible Psych export"
    );
    require(
        loaded_chart.chart->events.size() == 1U,
        "streaming Psych export preserves standard events"
    );
    require(
        loaded_chart.chart->note_style == "NOTE_assets-classic",
        "custom note skin survives compatible Psych export"
    );
    require(
        std::any_of(
            loaded_chart.chart->tempos.begin(),
            loaded_chart.chart->tempos.end(),
            [](const pulseforge::TempoChange& tempo) {
                return std::abs(tempo.bpm - 180.0) < 1.0e-9;
            }
        ),
        "streaming Psych export preserves BPM changes"
    );

    std::ifstream psych_stream(psych.path, std::ios::binary);
    require(static_cast<bool>(psych_stream), "cannot reopen Psych export");
    const auto psych_json = nlohmann::json::parse(psych_stream);
    require(
        psych_json["song"]["pulseforgeScripts"].size() == scripts.size(),
        "PulseForge extension preserves scripts in compatible JSON"
    );
    require(
        psych_json["song"]["pulseforgeTempos"].size()
            == metadata.tempos.size(),
        "PulseForge extension preserves non-4/4 tempo metadata losslessly"
    );
    require(
        psych_json["song"]["pulseforgeEvents"][0U]["payloadJson"]
            == "{\"extra\":true}",
        "PulseForge extension preserves rich event payloads losslessly"
    );

    const auto invalid_patch = storage.write_atomic(
        "edits/invalid.pfpatch.json",
        "{\"format\":\"pulseforge-streaming-chart-patch\","
        "\"version\":2,\"sourceNoteCount\":999,\"deleted\":[],"
        "\"updated\":[],\"added\":[]}"
    );
    require(static_cast<bool>(invalid_patch), invalid_patch.message);
    const auto before_invalid_count = reopened.note_count();
    const auto before_invalid_title = reopened.metadata().title;
    const auto rejected = reopened.load_patch(
        storage,
        "edits/invalid.pfpatch.json"
    );
    require(!rejected, "a patch for another source must be rejected");
    require(
        reopened.note_count() == before_invalid_count
            && reopened.metadata().title == before_invalid_title,
        "failed patch load is transactional"
    );
}

void run_trillion_pattern_test() {
    TemporaryDirectory temporary;
    constexpr std::uint64_t trillion = 1'000'000'000'000ULL;
    pulseforge::PackedChartData packed;
    packed.key_count = 4U;
    packed.kinds = {"normal"};
    pulseforge::PatternRun pattern;
    pattern.start_us = 0;
    pattern.interval_us = 100U;
    pattern.count = trillion;
    pattern.lane_pattern = {0U, 1U, 2U, 3U};
    packed.patterns.push_back(pattern);

    const auto pfc = temporary.path() / "trillion.pfc";
    std::string error;
    require(pulseforge::write_packed_chart(pfc, packed, {}, &error), error);
    auto reader = pulseforge::PackedChartReader::open(pfc, &error);
    require(reader.has_value(), error);
    pulseforge::Chart metadata;
    metadata.tempos.push_back({0.0, 120.0, 4U, 4U});
    pulseforge::StreamingChartEditor editor(
        std::move(*reader),
        std::move(metadata),
        pfc,
        100'000'000'000'000ULL
    );
    require(editor.note_count() == trillion, "trillion-note count is arithmetic");
    const auto viewport = editor.query(
        std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::int64_t>::max(),
        8U
    );
    require(static_cast<bool>(viewport), viewport.error);
    require(viewport.notes.size() == 8U, "trillion query remains memory bounded");
    require(viewport.density_spans.size() == 1U, "trillion run has compact LOD");
    require(
        viewport.density_spans.front().note_count == trillion - 8U,
        "trillion LOD accounts for every non-materialized occurrence"
    );
    require(editor.remove_note(trillion, &error), error);
    require(editor.note_count() == trillion - 1U, "far PatternRun ids are editable");

    pulseforge::EditorStorage storage(temporary.path());
    require(storage.ready(), storage.initialization_error());
    const auto patch = editor.save_patch(storage, "trillion.pfpatch.json");
    require(static_cast<bool>(patch), patch.message);
    require(
        std::filesystem::file_size(patch.path) < 8'192U,
        "a trillion-note edit stays a compact patch"
    );
}

void run_real_cache_probe(const std::filesystem::path& path) {
    std::string error;
    auto reader = pulseforge::PackedChartReader::open(path, &error);
    require(reader.has_value(), error);
    const auto expected = reader->explicit_note_count();
    pulseforge::Chart metadata;
    metadata.title = "Real PFC probe";
    metadata.tempos.push_back({0.0, 120.0, 4U, 4U});
    pulseforge::StreamingChartEditor editor(
        std::move(*reader),
        std::move(metadata),
        path,
        1U
    );
    const auto viewport = editor.query(
        std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::int64_t>::max(),
        250'000U
    );
    require(static_cast<bool>(viewport), viewport.error);
    require(editor.note_count() == expected, "real PFC count stays indexed");
    require(!viewport.notes.empty(), "real PFC viewport returns notes");
    std::cout << "Real PFC streaming editor probe: " << expected
              << " notes, returned " << viewport.notes.size()
              << ", dense LOD=" << (viewport.dense_lod ? "yes" : "no")
              << '\n';
}

}  // namespace

int main(const int argc, char** const argv) {
    try {
        run_test();
        run_selection_survives_viewport_test();
        run_pattern_and_patch_test();
        run_trillion_pattern_test();
        if (argc > 1 && argv[1] != nullptr) {
            run_real_cache_probe(argv[1]);
        }
        std::cout << "PulseForge streaming chart editor tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "PulseForge streaming chart editor tests failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
