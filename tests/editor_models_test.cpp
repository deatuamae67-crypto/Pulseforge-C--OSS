#if !defined(PULSEFORGE_EDITOR_TEST_NO_CHART_LOADER)
#include "pulseforge/chart_loader.hpp"
#endif
#include "pulseforge/editor_models.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
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

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto nonce = static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
        path_ = std::filesystem::temp_directory_path()
            / ("pulseforge-editor-test-" + std::to_string(nonce));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void test_storage(const std::filesystem::path& root) {
    pulseforge::EditorStorage storage(root / "storage");
    require(storage.ready(), "editor storage initializes");

    auto result = storage.write_atomic("nested/value.json", "old");
    require(static_cast<bool>(result), "initial atomic write succeeds");
    result = storage.write_atomic("nested/value.json", "new");
    require(static_cast<bool>(result), "atomic replacement succeeds");
    std::string text;
    result = storage.read_text("nested/value.json", text);
    require(static_cast<bool>(result) && text == "new", "replacement is readable");

    result = storage.write_atomic("../escape.json", "bad");
    require(
        result.status == pulseforge::EditorIoStatus::unsafe_path,
        "parent traversal is rejected"
    );

    pulseforge::EditorStorageLimits tiny_limits;
    tiny_limits.maximum_write_bytes = 8U;
    pulseforge::EditorStorage tiny(root / "tiny", tiny_limits);
    result = tiny.write_atomic("oversized.json", "123456789");
    require(
        result.status == pulseforge::EditorIoStatus::too_large,
        "streaming write budget is enforced"
    );
    require(
        !std::filesystem::exists(root / "tiny" / "oversized.json"),
        "failed atomic write does not publish a partial target"
    );
}

void test_chart_editor(const std::filesystem::path& root) {
    pulseforge::Chart chart;
    chart.title = "Editor Test";
    chart.artist = "Artist";
    chart.charter = "Charter";
    chart.difficulty = "hard";
    chart.stage_id = "stage";
    chart.player_character = "bf";
    chart.opponent_character = "dad";
    chart.girlfriend_character = "gf";
    chart.chart_scroll_speed = 1.5;
    chart.tempos = {{0.0, 120.0, 4U, 4U}};
    chart.normalize();

    pulseforge::ChartEditor editor(chart, {"data/editor-test/script.lua"});
    require(!editor.dirty(), "new chart editor begins saved");

    std::string error;
    auto edited_metadata = editor.metadata();
    edited_metadata.note_style = "NOTE_assets-future";
    require(
        editor.set_metadata(edited_metadata, &error),
        "materialized editor accepts an unknown note-skin ID for mod round-trip"
    );
    edited_metadata.note_style = std::string{"bad-"} + static_cast<char>(0xC3U);
    require(
        !editor.set_metadata(edited_metadata, &error),
        "materialized editor rejects an incomplete UTF-8 note-skin ID"
    );
    error.clear();
    require(
        !editor.add_note(
            pulseforge::Note{
                250.0,
                0.0,
                0U,
                pulseforge::NoteOwner::player,
                std::string{"broken-"} + static_cast<char>(0xC3U),
                0U,
            },
            &error
        ).has_value(),
        "materialized editor rejects an incomplete UTF-8 note type"
    );
    error.clear();
    const auto payload = editor.intern_note_payload(
        R"json({"accent":true})json",
        &error
    );
    require(payload.has_value(), "custom note payload is interned");
    const auto note_id = editor.add_note(
        pulseforge::Note{
            500.0,
            125.0,
            2U,
            pulseforge::NoteOwner::player,
            "Hurt Note",
            *payload,
        },
        &error
    );
    require(note_id.has_value(), "note command succeeds");
    const auto opponent_note = editor.add_note(
        pulseforge::Note{
            1'500.0,
            0.0,
            1U,
            pulseforge::NoteOwner::opponent,
            "normal",
            0U,
        },
        &error
    );
    require(opponent_note.has_value(), "opponent note command succeeds");
    // PULSEFORGE_P1_4_0_EDITOR_SECONDARY_OPPONENT_TEST_V1
    const auto secondary_note = editor.add_note(
        pulseforge::Note{
            1'750.0,
            125.0,
            0U,
            pulseforge::NoteOwner::secondary_opponent,
            "Third Strum",
            0U,
        },
        &error
    );
    require(secondary_note.has_value(), "secondary opponent note command succeeds");
    const auto event_id = editor.add_event(
        pulseforge::ChartEvent{1'000.0, "Camera Flash", "FFFFFF", "0.5", {}},
        &error
    );
    require(event_id.has_value(), "event command succeeds");
    const auto tempo_id = editor.add_tempo(
        pulseforge::TempoChange{2'000.0, 180.0, 4U, 4U},
        &error
    );
    require(tempo_id.has_value(), "BPM command succeeds");
    require(editor.set_scroll_speed(2.25, &error), "scroll command succeeds");
    require(editor.dirty(), "commands mark chart dirty");

    require(editor.undo(), "undo succeeds");
    require(editor.scroll_speed() == 1.5, "undo restores scroll speed");
    require(editor.redo(), "redo succeeds");
    require(editor.scroll_speed() == 2.25, "redo restores scroll speed");
    require(
        !editor.set_key_count(2U, &error),
        "key-count change cannot orphan a lane"
    );
    require(editor.update_note(
        *note_id,
        pulseforge::Note{
            500.0,
            125.0,
            1U,
            pulseforge::NoteOwner::player,
            "Hurt Note",
            *payload,
        },
        &error
    ), "note update succeeds");
    require(editor.set_key_count(2U, &error), "safe key-count change succeeds");

    pulseforge::EditorStorage storage(root / "chart");
    auto io = editor.save_project(storage, "projects/editor.pfchart.json");
    require(static_cast<bool>(io), "editor project saves atomically");
    require(!editor.dirty(), "explicit project save marks state saved");

    require(editor.add_event(
        pulseforge::ChartEvent{2'500.0, "Add Camera Zoom", "0.1", "0.2", {}},
        &error
    ).has_value(), "post-save event succeeds");
    const auto autosave = editor.force_autosave(storage, &error);
    require(
        autosave == pulseforge::EditorAutosaveStatus::saved,
        "forced autosave writes a recovery project"
    );
    require(editor.dirty(), "autosave does not pretend the user saved");

    pulseforge::ChartEditor reopened;
    io = reopened.load_project(storage, "projects/editor.pfchart.json");
    require(static_cast<bool>(io), "editor project reopens");
    require(reopened.notes().size() == 3U, "project retains notes");
    require(reopened.events().size() == 1U, "explicit save retains its event set");
    require(reopened.tempos().size() == 2U, "project retains BPM changes");
    require(reopened.scripts().size() == 1U, "project retains scripts");
    require(reopened.note_payloads().size() == 2U, "project retains payloads");

    io = reopened.save_psych_json(storage, "exports/editor-hard.json");
    require(static_cast<bool>(io), "Psych chart export succeeds");
    std::string psych_text;
    io = storage.read_text("exports/editor-hard.json", psych_text);
    require(static_cast<bool>(io), "Psych export is readable");
    require(
        psych_text.find('\n') == std::string::npos,
        "Psych export is compact single-line JSON"
    );
    const auto psych_dom = nlohmann::json::parse(psych_text);
    require(psych_dom["song"]["notes"].is_array(), "Psych sections are emitted");
    require(psych_dom["song"]["events"].is_array(), "Psych events are emitted");

#if !defined(PULSEFORGE_EDITOR_TEST_NO_CHART_LOADER)
    pulseforge::ChartLoadOptions options;
    options.difficulty = "hard";
    const auto loaded = pulseforge::ChartLoader::parse(
        psych_text,
        root / "chart" / "exports" / "editor-hard.json",
        options
    );
    require(static_cast<bool>(loaded), "engine loader accepts editor Psych export");
    require(loaded.chart->notes.size() == 3U, "Psych round-trip retains notes");
    require(
        std::any_of(
            loaded.chart->notes.begin(),
            loaded.chart->notes.end(),
            [](const pulseforge::Note& note) {
                return note.owner == pulseforge::NoteOwner::secondary_opponent
                    && note.kind == "Third Strum";
            }
        ),
        "Psych round-trip reconstructs Third Strum as secondary opponent"
    );
    require(loaded.chart->events.size() == 1U, "Psych round-trip retains events");
    require(loaded.chart->tempos.size() == 2U, "Psych round-trip retains BPM");
#endif
}

void test_high_bpm_chart_editor(const std::filesystem::path& root) {
    pulseforge::Chart chart;
    chart.title = "High BPM Editor Test";
    chart.tempos = {{0.0, 120.0, 4U, 4U}};
    chart.normalize();

    pulseforge::ChartEditor editor(chart);
    require(editor.tempos().size() == 1U, "high-BPM fixture has one initial tempo");

    std::string error;
    const auto initial_tempo_id = editor.tempos().begin()->first;
    require(
        editor.update_tempo(
            initial_tempo_id,
            pulseforge::TempoChange{0.0, 1'044.0, 4U, 4U},
            &error
        ),
        "editor command accepts an intentional BPM above 1000"
    );
    const auto half_time_id = editor.add_tempo(
        pulseforge::TempoChange{2'000.0, 522.0, 4U, 4U},
        &error
    );
    require(half_time_id.has_value(), "editor command accepts the 522 BPM change");

    error.clear();
    require(
        !editor.add_tempo(
            pulseforge::TempoChange{1'000.0, 0.0, 4U, 4U},
            &error
        ).has_value(),
        "editor rejects a zero BPM command"
    );
    error.clear();
    require(
        !editor.add_tempo(
            pulseforge::TempoChange{1'000.0, -1.0, 4U, 4U},
            &error
        ).has_value(),
        "editor rejects a negative BPM command"
    );
    require(
        editor.tempos().size() == 2U,
        "rejected BPM commands do not mutate the editor"
    );

    pulseforge::EditorStorage storage(root / "high-bpm-chart");
    auto io = editor.save_project(storage, "projects/high-bpm.pfchart.json");
    require(static_cast<bool>(io), "high-BPM editor project saves");

    pulseforge::ChartEditor reopened;
    io = reopened.load_project(storage, "projects/high-bpm.pfchart.json");
    require(static_cast<bool>(io), "high-BPM editor project reopens");
    require(reopened.tempos().size() == 2U, "high-BPM project retains both tempos");

    bool retained_initial_bpm = false;
    bool retained_half_time_bpm = false;
    for (const auto& [id, tempo] : reopened.tempos()) {
        (void)id;
        retained_initial_bpm = retained_initial_bpm
            || (tempo.time_ms == 0.0 && tempo.bpm == 1'044.0);
        retained_half_time_bpm = retained_half_time_bpm
            || (tempo.time_ms == 2'000.0 && tempo.bpm == 522.0);
    }
    require(retained_initial_bpm, "1044 BPM survives editor project round-trip");
    require(retained_half_time_bpm, "522 BPM survives editor project round-trip");

    io = reopened.save_psych_json(storage, "exports/high-bpm.json");
    require(static_cast<bool>(io), "high-BPM Psych export succeeds");
    std::string psych_text;
    io = storage.read_text("exports/high-bpm.json", psych_text);
    require(static_cast<bool>(io), "high-BPM Psych export is readable");
    const auto psych_dom = nlohmann::json::parse(psych_text);
    require(
        psych_dom["song"]["bpm"] == 1'044.0,
        "Psych export preserves the initial 1044 BPM"
    );
    bool exported_half_time_bpm = false;
    for (const auto& section : psych_dom["song"]["notes"]) {
        exported_half_time_bpm = exported_half_time_bpm
            || (section.value("changeBPM", false)
                && section.value("bpm", 0.0) == 522.0);
    }
    require(exported_half_time_bpm, "Psych export preserves the 522 BPM change");
}

void test_character_editor(const std::filesystem::path& root) {
    pulseforge::CharacterDescriptor character;
    character.id = "bf-editor";
    character.image = "characters/BOYFRIEND";
    character.scale = 1.0;
    character.health_icon = "bf";
    character.healthbar_color = {49, 176, 209};
    character.extensions_json = R"json({"futureRenderer":"atlas"})json";
    pulseforge::AnimationDescriptor animation;
    animation.id = "idle";
    animation.name = "BF idle dance";
    animation.fps = 24;
    animation.extensions_json = R"json({"atlasLayer":"body"})json";
    character.animations.push_back(animation);

    pulseforge::CharacterEditor editor(character);
    auto changed = character;
    changed.scale = 1.25;
    std::string error;
    require(editor.replace(changed, "Scale character", &error), "character edit applies");
    require(editor.undo(), "character undo succeeds");
    require(editor.document().scale == 1.0, "character undo restores snapshot");
    require(editor.redo(), "character redo succeeds");

    pulseforge::EditorStorage storage(root / "descriptors");
    auto io = editor.save_psych_json(storage, "characters/bf-editor.json");
    require(static_cast<bool>(io), "character saves in Psych format");

    pulseforge::CharacterEditor reopened;
    io = reopened.load_psych_json(
        storage,
        "characters/bf-editor.json",
        "bf-editor"
    );
    require(static_cast<bool>(io), "character reopens");
    require(reopened.document().scale == 1.25, "character scale round-trips");
    require(
        reopened.document().extensions_json.find("futureRenderer")
            != std::string::npos,
        "character unknown fields survive"
    );
    require(
        reopened.document().animations.front().extensions_json.find("atlasLayer")
            != std::string::npos,
        "animation unknown fields survive"
    );
}

void test_week_editor(const std::filesystem::path& root) {
    pulseforge::WeekDescriptor week;
    week.id = "week-editor";
    week.display_name = "Editor Week";
    week.story_name = "EDITOR WEEK";
    week.characters = {"dad", "bf", "gf"};
    week.songs.push_back({"Editor Test", "dad", {10, 20, 30}, {}});
    week.difficulties = {"Easy", "Normal", "Hard"};
    week.extensions_json = R"json({"ranked":true})json";

    pulseforge::WeekEditor editor(week);
    auto changed = week;
    changed.hide_freeplay = true;
    std::string error;
    require(editor.replace(changed, "Hide from Freeplay", &error), "week edit applies");
    require(editor.undo(), "week undo succeeds");
    require(!editor.document().hide_freeplay, "week undo restores snapshot");
    require(editor.redo(), "week redo succeeds");

    pulseforge::EditorStorage storage(root / "descriptors");
    auto io = editor.save_psych_json(storage, "weeks/week-editor.json");
    require(static_cast<bool>(io), "week saves in Psych format");

    pulseforge::WeekEditor reopened;
    io = reopened.load_psych_json(
        storage,
        "weeks/week-editor.json",
        "week-editor"
    );
    require(static_cast<bool>(io), "week reopens");
    require(reopened.document().hide_freeplay, "week flags round-trip");
    require(reopened.document().songs.size() == 1U, "week songs round-trip");
    require(reopened.document().difficulties.size() == 3U, "difficulties round-trip");
    require(
        reopened.document().extensions_json.find("ranked") != std::string::npos,
        "week unknown fields survive"
    );
}

}  // namespace

int main() {
    try {
        const TemporaryDirectory temporary;
        test_storage(temporary.path());
        test_chart_editor(temporary.path());
        test_high_bpm_chart_editor(temporary.path());
        test_character_editor(temporary.path());
        test_week_editor(temporary.path());
        std::cout << "Editor model tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "Editor model tests failed: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
