#include "pulseforge/packed_chart.hpp"
#include "pulseforge/packed_chart_stream.hpp"
#include "pulseforge/streaming_chart_importer.hpp"
#include "pulseforge/visual_density_index.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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
        const auto root = std::filesystem::temp_directory_path();
        for (std::uint32_t attempt = 0U; attempt < 1'024U; ++attempt) {
            path_ = root / (
                "pulseforge-streaming-import-test-" + std::to_string(nonce)
                + "-" + std::to_string(attempt)
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

void write_text(
    const std::filesystem::path& path,
    const std::string_view text
) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    require(static_cast<bool>(output), "test JSON write succeeds");
}

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    require(static_cast<bool>(input), "test text read opens");
    const auto end = input.tellg();
    require(end >= std::streampos{0}, "test text size is valid");
    std::string text(static_cast<std::size_t>(end), '\0');
    input.seekg(0, std::ios::beg);
    if (!text.empty()) {
        input.read(text.data(), static_cast<std::streamsize>(text.size()));
    }
    require(static_cast<bool>(input), "test text read succeeds");
    return text;
}

void test_stream_writer(const std::filesystem::path& directory) {
    const auto path = directory / "writer.pfc";
    pulseforge::PackedChartStreamSpec spec;
    spec.key_count = 4U;
    spec.explicit_note_count = 5U;
    spec.kinds = {"normal", "mine"};
    pulseforge::PackedChartWriteOptions options;
    options.max_notes_per_chunk = 2U;
    std::string error;
    auto writer = pulseforge::PackedChartStreamWriter::create(
        path,
        spec,
        options,
        &error
    );
    require(writer.has_value(), error);
    const std::vector<pulseforge::PackedNote> notes{
        {-10, 0U, 0U, pulseforge::PackedNoteOwner::player, 0U, 0U},
        {0, 10U, 1U, pulseforge::PackedNoteOwner::opponent, 0U, 1U},
        {0, 0U, 2U, pulseforge::PackedNoteOwner::player, 0U, 0U},
        {10, 0U, 3U, pulseforge::PackedNoteOwner::player, 0U, 0U},
        {20, 5U, 0U, pulseforge::PackedNoteOwner::opponent, 0U, 1U},
    };
    require(writer->append(notes, &error), error);
    require(writer->finish(&error), error);
    require(writer->chunks_written() == 3U, "stream writer emits bounded chunks");

    auto reader = pulseforge::PackedChartReader::open(path, &error);
    require(reader.has_value(), error);
    const auto decoded = reader->read_explicit_notes_in_range(
        std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::int64_t>::max()
    );
    require(static_cast<bool>(decoded), decoded.error);
    require(decoded.notes == notes, "stream writer is PFC1-reader compatible");
}

void test_native_single_line_external_sort(
    const std::filesystem::path& directory
) {
    const auto source = directory / "native.json";
    const auto packed = directory / "native.pfc";
    std::string json = "{\"keyCount\":6,\"metadata\":{\"ignored\":true},\"notes\":[";
    constexpr std::size_t note_count = 5'000U;
    for (std::size_t index = 0U; index < note_count; ++index) {
        if (index != 0U) {
            json.push_back(',');
        }
        const auto reverse = note_count - index;
        json += "{\"owner\":\"";
        json += index % 2U == 0U ? "player" : "enemy";
        json += "\",\"kind\":\"";
        json += index % 3U == 0U ? "mine" : "normal";
        json += "\",\"durationMs\":1.25,\"lane\":"
            + std::to_string(index % 6U) + ",\"timeMs\":"
            + std::to_string(reverse) + "}";
    }
    json += "]}";
    require(json.find('\n') == std::string::npos, "fixture is one physical line");
    write_text(source, json);

    pulseforge::StreamingChartImportOptions options;
    options.input_buffer_bytes = 4'096U;
    options.max_sort_notes_in_memory = 37U;
    options.max_merge_fan_in = 3U;
    options.notes_per_pfc_chunk = 41U;
    const auto result = pulseforge::compile_streaming_json_chart_to_pfc(
        source,
        packed,
        options
    );
    require(static_cast<bool>(result), result.error);
    require(result.source_format == pulseforge::ChartFormat::native, "native detected");
    require(result.explicit_note_count == note_count, "all native notes compile");
    require(!result.input_was_time_sorted, "unsorted native input detected");
    require(result.used_external_sort, "bounded external sort is used");
    require(result.peak_buffered_notes == 37U, "sort RAM note bound is observed");
    require(result.key_count == 6U, "native key count is preserved");

    std::string error;
    auto reader = pulseforge::PackedChartReader::open(packed, &error);
    require(reader.has_value(), error);
    require(reader->explicit_note_count() == note_count, "native PFC count matches");
    require(reader->key_count() == 6U, "native PFC key count matches");
    std::int64_t previous = std::numeric_limits<std::int64_t>::min();
    for (std::uint64_t chunk = 0U; chunk < reader->chunk_count(); ++chunk) {
        const auto decoded = reader->read_chunk(chunk);
        require(static_cast<bool>(decoded), decoded.error);
        for (const auto& note : decoded.notes) {
            require(note.time_us >= previous, "external-sort result is chronological");
            previous = note.time_us;
        }
    }
}

void test_psych_owner_and_field_order(const std::filesystem::path& directory) {
    const auto source = directory / "psych.json";
    const auto packed = directory / "psych.pfc";
    // sectionNotes intentionally precedes mustHitSection. Pass one records the
    // ownership bit; pass two can still stream every tuple without buffering a
    // potentially gigantic section.
    const std::string json =
        "{\"song\":{\"keyCount\":4,\"notes\":["
        "{\"sectionNotes\":[[20,0,0,\"normal\"],[10,4,5,\"hurt\"],"
        "[15,-1,\"Camera Flash\",\"white\"]],\"mustHitSection\":true},"
        "{\"mustHitSection\":false,\"sectionNotes\":[[30,0,0],[40,4,0]]}"
        "]}}";
    write_text(source, json);

    pulseforge::StreamingChartImportOptions options;
    options.input_buffer_bytes = 4'096U;
    options.max_sort_notes_in_memory = 2U;
    options.max_merge_fan_in = 2U;
    options.notes_per_pfc_chunk = 2U;
    const auto result = pulseforge::compile_streaming_json_chart_to_pfc(
        source,
        packed,
        options
    );
    require(static_cast<bool>(result), result.error);
    require(result.source_format == pulseforge::ChartFormat::psych, "Psych detected");
    require(result.explicit_note_count == 4U, "legacy event tuple is not a note");
    require(result.section_count == 2U, "Psych sections counted in 64 bits");
    require(result.kind_count == 2U, "Psych kinds are interned");

    std::string error;
    auto reader = pulseforge::PackedChartReader::open(packed, &error);
    require(reader.has_value(), error);
    const auto notes = reader->read_explicit_notes_in_range(0, 100'000);
    require(static_cast<bool>(notes), notes.error);
    require(notes.notes.size() == 4U, "Psych notes decode");
    require(notes.notes[0U].time_us == 10'000, "Psych notes externally sort");
    require(
        notes.notes[0U].owner == pulseforge::PackedNoteOwner::opponent,
        "other strumline ownership follows mustHitSection"
    );
    require(
        notes.notes[1U].owner == pulseforge::PackedNoteOwner::player,
        "primary strumline ownership follows mustHitSection"
    );
    require(
        notes.notes[2U].owner == pulseforge::PackedNoteOwner::opponent,
        "opponent section primary strumline is opponent"
    );
    require(
        notes.notes[3U].owner == pulseforge::PackedNoteOwner::player,
        "opponent section alternate strumline is player"
    );
}


void test_psych_third_strum_pfc_identity(
    const std::filesystem::path& directory
) {
    // PULSEFORGE_P1_4_0D_STREAMING_THIRD_STRUM_IMPORT_PARITY_TEST_V1
    const auto source = directory / "psych-third-strum.json";
    const auto packed = directory / "psych-third-strum.pfc";
    write_text(
        source,
        R"json({"song":{"song":"Psych Third Stream","bpm":120,"keyCount":4,"notes":[{"mustHitSection":true,"sectionNotes":[[100,4,600,"Third Strum"]]}]}})json"
    );

    pulseforge::StreamingChartImportOptions options;
    options.input_buffer_bytes = 4'096U;
    const auto result = pulseforge::compile_streaming_json_chart_to_pfc(
        source,
        packed,
        options
    );
    require(static_cast<bool>(result), result.error);
    require(
        result.source_format == pulseforge::ChartFormat::psych,
        "generic Psych Third Strum remains a Psych source"
    );

    std::string error;
    const auto reader = pulseforge::PackedChartReader::open(packed, &error);
    require(reader.has_value(), error);
    const auto kinds = reader->kinds();
    const auto third_kind = std::find(kinds.begin(), kinds.end(), "Third Strum");
    require(third_kind != kinds.end(), "Psych PFC1 preserves Third Strum kind");
    const auto third_kind_id = static_cast<std::uint32_t>(
        std::distance(kinds.begin(), third_kind)
    );
    const auto notes = reader->read_explicit_notes_in_range(0, 1'000'000);
    require(static_cast<bool>(notes), notes.error);
    const auto third = std::find_if(
        notes.notes.begin(),
        notes.notes.end(),
        [third_kind_id](const auto& note) {
            return note.kind_id == third_kind_id;
        }
    );
    require(third != notes.notes.end(), "Psych Third Strum PFC note is present");
    require(
        third->owner == pulseforge::PackedNoteOwner::opponent
            && third->duration_us == 600'000U,
        "generic Psych Third Strum is physically AI-owned in PFC1 v1"
    );
}

void test_denpa_third_strum_pfc_identity(
    const std::filesystem::path& directory
) {
    // PULSEFORGE_P1_4_0_STREAMING_THIRD_STRUM_TEST_V1
    const auto source = directory / "denpa-third-strum.json";
    const auto packed = directory / "denpa-third-strum.pfc";
    write_text(
        source,
        R"json({"song":{"header":{"song":"Third Stream","bpm":120,"needsVoices":false},"assets":{"player1":"bf","player2":"dad","player4":"monster","enablePlayer4":true},"options":{"speed":1,"mania":3},"notes":[{"mustHitSection":false,"lengthInSteps":16,"sectionNotes":[[100,0,0,"normal"],[200,1,600,"Third Strum"]]}]}})json"
    );

    pulseforge::StreamingChartImportOptions options;
    options.input_buffer_bytes = 4'096U;
    const auto result = pulseforge::compile_streaming_json_chart_to_pfc(
        source,
        packed,
        options
    );
    require(static_cast<bool>(result), result.error);
    require(
        result.source_format == pulseforge::ChartFormat::denpa,
        "streaming Denpa schema is detected"
    );
    require(
        result.chart_metadata.secondary_opponent_enabled
            && result.chart_metadata.secondary_opponent_character == "monster",
        "streaming Denpa metadata preserves player4"
    );

    std::string error;
    const auto reader = pulseforge::PackedChartReader::open(packed, &error);
    require(reader.has_value(), error);
    const auto kinds = reader->kinds();
    const auto third_kind = std::find(kinds.begin(), kinds.end(), "Third Strum");
    require(third_kind != kinds.end(), "PFC1 kind dictionary preserves Third Strum");
    const auto third_kind_id = static_cast<std::uint32_t>(
        std::distance(kinds.begin(), third_kind)
    );
    const auto notes = reader->read_explicit_notes_in_range(0, 1'000'000);
    require(static_cast<bool>(notes), notes.error);
    const auto third = std::find_if(
        notes.notes.begin(),
        notes.notes.end(),
        [third_kind_id](const auto& note) {
            return note.kind_id == third_kind_id;
        }
    );
    require(third != notes.notes.end(), "Third Strum PFC note is present");
    require(
        third->owner == pulseforge::PackedNoteOwner::opponent
            && third->duration_us == 600'000U,
        "PFC1 v1 keeps Third Strum physically AI-owned while retaining its kind identity"
    );
}

void test_psych_negative_sustain_compatibility(
    const std::filesystem::path& directory
) {
    const auto source = directory / "psych-negative-sustain.json";
    const auto packed = directory / "psych-negative-sustain.pfc";
    write_text(
        source,
        "{\"song\":{\"notes\":[{\"mustHitSection\":true,"
        "\"sectionNotes\":[[1000,0,-1000],[1100,1,25],"
        "[1200,2,1e308]]}]}}"
    );

    pulseforge::StreamingChartImportOptions options;
    options.input_buffer_bytes = 4'096U;
    const auto result = pulseforge::compile_streaming_json_chart_to_pfc(
        source,
        packed,
        options
    );
    require(static_cast<bool>(result), result.error);
    require(
        result.source_format == pulseforge::ChartFormat::psych
            && result.explicit_note_count == 2U
            && result.skipped_entry_count == 1U,
        "streaming Psych keeps repaired taps but rejects positive overflow"
    );

    std::string error;
    const auto reader = pulseforge::PackedChartReader::open(packed, &error);
    require(reader.has_value(), error);
    const auto notes = reader->read_explicit_notes_in_range(
        0,
        std::numeric_limits<std::int64_t>::max()
    );
    require(static_cast<bool>(notes), notes.error);
    require(
        notes.notes.size() == 2U
            && notes.notes[0U].time_us == 1'000'000
            && notes.notes[0U].duration_us == 0U
            && notes.notes[1U].duration_us == 25'000U,
        "streaming Psych clamps only a negative sustain to a zero-length tap"
    );

    const auto native_source = directory / "native-negative-duration.json";
    const auto native_packed = directory / "native-negative-duration.pfc";
    write_text(
        native_source,
        "{\"notes\":[{\"time\":100,\"duration\":-100,\"lane\":0},"
        "{\"time\":200,\"duration\":0,\"lane\":1}]}"
    );
    const auto native = pulseforge::compile_streaming_json_chart_to_pfc(
        native_source,
        native_packed,
        options
    );
    require(static_cast<bool>(native), native.error);
    require(
        native.source_format == pulseforge::ChartFormat::native
            && native.explicit_note_count == 1U
            && native.skipped_entry_count == 1U,
        "native negative duration is not covered by Psych compatibility repair"
    );

    const auto non_finite_source = directory / "psych-non-finite.json";
    const auto non_finite_packed = directory / "psych-non-finite.pfc";
    write_text(
        non_finite_source,
        "{\"song\":{\"notes\":[{\"mustHitSection\":true,"
        "\"sectionNotes\":[[100,0,NaN]]}]}}"
    );
    const auto non_finite = pulseforge::compile_streaming_json_chart_to_pfc(
        non_finite_source,
        non_finite_packed,
        options
    );
    require(
        !static_cast<bool>(non_finite)
            && !std::filesystem::exists(non_finite_packed),
        "non-finite Psych sustain remains invalid JSON"
    );
}

void test_sorted_streaming_path(const std::filesystem::path& directory) {
    const auto source = directory / "sorted.json";
    const auto packed = directory / "sorted.pfc";
    std::string json = "{\"notes\":[";
    constexpr std::size_t note_count = 257U;
    for (std::size_t index = 0U; index < note_count; ++index) {
        if (index != 0U) {
            json.push_back(',');
        }
        // Deliberately non-arithmetic timestamps keep this fixture on the
        // explicit-note path; uniform runs are now represented by PatternRun.
        json += "{\"time\":" + std::to_string(index * index)
            + ",\"lane\":" + std::to_string(index % 4U) + "}";
    }
    json += "]}";
    write_text(source, json);

    pulseforge::StreamingChartImportOptions options;
    options.input_buffer_bytes = 4'096U;
    options.notes_per_pfc_chunk = 17U;
    const auto result = pulseforge::compile_streaming_json_chart_to_pfc(
        source,
        packed,
        options
    );
    require(static_cast<bool>(result), result.error);
    require(result.input_was_time_sorted, "chronological input is recognized");
    require(!result.used_external_sort, "sorted input writes directly to PFC1");
    require(result.explicit_note_count == note_count, "direct path preserves count");

    std::string error;
    const auto reader = pulseforge::PackedChartReader::open(packed, &error);
    require(reader.has_value(), error);
    require(reader->chunk_count() == 16U, "direct path honors PFC1 chunk size");
}

void test_psych_options_mania_index(const std::filesystem::path& directory) {
    const auto source = directory / "psych-9k.json";
    const auto packed = directory / "psych-9k.pfc";
    write_text(
        source,
        "{\"song\":{\"options\":{\"mania\":8},\"notes\":["
        "{\"mustHitSection\":true,\"sectionNotes\":[[0,17,0]]}"
        "]}}"
    );
    pulseforge::StreamingChartImportOptions options;
    options.input_buffer_bytes = 4'096U;
    const auto result = pulseforge::compile_streaming_json_chart_to_pfc(
        source,
        packed,
        options
    );
    require(static_cast<bool>(result), result.error);
    require(result.key_count == 9U, "options.mania=8 uses the 9K index convention");

    std::string error;
    const auto reader = pulseforge::PackedChartReader::open(packed, &error);
    require(reader.has_value(), error);
    const auto notes = reader->read_explicit_notes_in_range(0, 0);
    require(static_cast<bool>(notes), notes.error);
    require(notes.notes.size() == 1U, "9K note is retained");
    require(notes.notes.front().lane == 8U, "second strumline lane is normalized");
}

void test_lane_domain_key_mode_detection(
    const std::filesystem::path& directory
) {
    pulseforge::StreamingChartImportOptions options;
    options.input_buffer_bytes = 4'096U;

    const auto native_source = directory / "native-stale-keys.json";
    const auto native_packed = directory / "native-stale-keys.pfc";
    write_text(
        native_source,
        "{\"keyCount\":4,\"notes\":[{\"time\":0,\"lane\":5}]}"
    );
    const auto native = pulseforge::compile_streaming_json_chart_to_pfc(
        native_source,
        native_packed,
        options
    );
    require(static_cast<bool>(native), native.error);
    require(
        native.key_count == 6U,
        "native streaming import widens stale metadata from largest lane"
    );

    const auto psych_source = directory / "psych-stale-keys.json";
    const auto psych_packed = directory / "psych-stale-keys.pfc";
    write_text(
        psych_source,
        "{\"song\":{\"keyCount\":4,\"notes\":["
        "{\"mustHitSection\":true,\"sectionNotes\":[[0,17,0]]}]}}"
    );
    const auto psych = pulseforge::compile_streaming_json_chart_to_pfc(
        psych_source,
        psych_packed,
        options
    );
    require(static_cast<bool>(psych), psych.error);
    require(
        psych.key_count == 9U,
        "Psych streaming import widens stale metadata from combined lane domain"
    );

    const auto unsupported_source = directory / "psych-unsupported-keys.json";
    const auto unsupported_packed = directory / "psych-unsupported-keys.pfc";
    write_text(
        unsupported_source,
        "{\"song\":{\"notes\":[{\"mustHitSection\":true,"
        "\"sectionNotes\":[[0,36,0]]}]}}"
    );
    const auto unsupported = pulseforge::compile_streaming_json_chart_to_pfc(
        unsupported_source,
        unsupported_packed,
        options
    );
    require(
        !static_cast<bool>(unsupported)
            && !std::filesystem::exists(unsupported_packed),
        "streaming import rejects only lanes beyond the supported 18K domain"
    );

    const auto options_source = directory / "psych-options-sparse.json";
    const auto options_packed = directory / "psych-options-sparse.pfc";
    write_text(
        options_source,
        "{\"song\":{\"options\":{\"mania\":5,\"speed\":1.25},\"notes\":["
        "{\"mustHitSection\":true,\"sectionNotes\":[[0,0,0]]}]}}"
    );
    const auto options_mode = pulseforge::compile_streaming_json_chart_to_pfc(
        options_source,
        options_packed,
        options
    );
    require(static_cast<bool>(options_mode), options_mode.error);
    require(
        options_mode.key_count == 6U
            && options_mode.chart_metadata.chart_scroll_speed == 1.25,
        "streaming JS options.mania identifies a sparse 6K chart"
    );

    const auto stronger_source = directory / "psych-options-stronger.json";
    const auto stronger_packed = directory / "psych-options-stronger.pfc";
    write_text(
        stronger_source,
        "{\"song\":{\"keyCount\":9,\"options\":{\"mania\":5},"
        "\"notes\":[]}}"
    );
    const auto stronger = pulseforge::compile_streaming_json_chart_to_pfc(
        stronger_source,
        stronger_packed,
        options
    );
    require(static_cast<bool>(stronger), stronger.error);
    require(
        stronger.key_count == 9U,
        "streaming explicit keyCount remains stronger than options.mania"
    );
}

void test_streaming_metadata_projection(
    const std::filesystem::path& directory
) {
    const auto source = directory / "metadata.json";
    const auto packed = directory / "metadata.pfc";
    write_text(
        source,
        "{\"song\":{\"song\":\"Metadata Song\",\"artist\":\"Artist\"," 
        "\"charter\":\"Charter\",\"assets\":{\"stage\":\"stage-id\"," 
        "\"player1\":\"bf\",\"player2\":\"dad\",\"gfVersion\":\"gf\"," 
        "\"arrowSkin\":\"pixel\"},\"bpm\":1044,\"speed\":1.5," 
        "\"notes\":[{\"lengthInSteps\":16,\"mustHitSection\":true," 
        "\"sectionNotes\":[[100,0,25]]},{\"changeBPM\":true," 
        "\"bpm\":522,\"mustHitSection\":false," 
        "\"sectionNotes\":[[2200,4,50]]}]}}"
    );
    pulseforge::StreamingChartImportOptions options;
    options.input_buffer_bytes = 4'096U;
    const auto result = pulseforge::compile_streaming_json_chart_to_pfc(
        source,
        packed,
        options
    );
    require(static_cast<bool>(result), result.error);
    require(result.source_fingerprint != 0U, "source fingerprint is published");
    require(result.content_end_us == 2'250'000U, "furthest sustain tail is projected");
    require(result.chart_metadata.title == "Metadata Song", "title is projected");
    require(result.chart_metadata.artist == "Artist", "artist is projected");
    require(result.chart_metadata.stage_id == "stage-id", "stage is projected");
    require(result.chart_metadata.player_character == "bf", "player is projected");
    require(result.chart_metadata.opponent_character == "dad", "opponent is projected");
    require(result.chart_metadata.girlfriend_character == "gf", "gf is projected");
    require(result.chart_metadata.note_style == "pixel", "note style is projected");
    require(result.chart_metadata.chart_scroll_speed == 1.5, "speed is projected");
    require(result.chart_metadata.notes.empty(), "metadata never materializes notes");
    require(result.chart_metadata.events.empty(), "metadata never materializes events");
    require(
        result.chart_metadata.tempos.size() == 2U
            && result.chart_metadata.tempos[0U].bpm == 1'044.0
            && result.chart_metadata.tempos[1U].bpm == 522.0,
        "positive high BPM metadata is projected without a ceiling"
    );
    require(
        result.chart_metadata.tempos[1U].time_ms > 229.88
            && result.chart_metadata.tempos[1U].time_ms < 229.89,
        "section BPM timestamp is derived from lengthInSteps"
    );
}

void test_adjacent_event_sidecar_compatibility(
    const std::filesystem::path& directory
) {
    const auto fixture = directory / "adjacent-event-sidecars";
    const auto cache = fixture / "cache";
    std::filesystem::create_directories(fixture);
    const auto source = fixture / "chart.json";
    const auto plural = fixture / "events.json";
    const auto singular = fixture / "event.json";
    write_text(
        source,
        "{\"song\":{\"song\":\"Sidecar Test\",\"bpm\":120,\"notes\":["
        "{\"mustHitSection\":true,\"sectionNotes\":[[10,0,0]]}]}}"
    );
    write_text(
        plural,
        "{\"events\":["
        "{\"t\":100,\"e\":\"Plural Object\","
        "\"v\":[\"object-1\",\"object-2\"]},"
        "[200,[[\"Plural Group\",\"group-1\",\"group-2\"]]]]}"
    );
    write_text(
        singular,
        "{\"song\":{\"notes\":[{\"sectionNotes\":["
        "[50,-1,\"Singular Legacy\",\"legacy-1\",\"legacy-2\"]]}]}}"
    );

    pulseforge::StreamingChartCacheOptions options;
    options.cache_root = cache;
    options.import.input_buffer_bytes = 4'096U;
    options.strict_cache_validation = true;

    const auto preferred = pulseforge::prepare_streaming_chart_cache(
        source,
        options
    );
    require(static_cast<bool>(preferred), preferred.error);
    require(!preferred.reused, "first adjacent-event cache compiles");
    require(
        preferred.chart_metadata.events.size() == 2U
            && preferred.chart_metadata.events[0U].name == "Plural Object"
            && preferred.chart_metadata.events[0U].value1 == "object-1"
            && preferred.chart_metadata.events[0U].value2 == "object-2"
            && preferred.chart_metadata.events[1U].name == "Plural Group",
        "events.json wins and parses modern object/group timelines"
    );
    const auto manifest = preferred.cache_path.parent_path()
        / (preferred.cache_path.stem().string() + ".manifest.json");
    require(
        read_text(manifest).find("events.json") != std::string::npos,
        "cache signature records the preferred plural sidecar path"
    );

    {
        const auto bounded_fixture = directory / "adjacent-event-byte-budget";
        std::filesystem::create_directories(bounded_fixture);
        const auto bounded_source = bounded_fixture / "chart.json";
        const auto bounded_events = bounded_fixture / "events.json";
        write_text(
            bounded_source,
            "{\"song\":{\"song\":\"Bounded Events\",\"bpm\":120,"
            "\"notes\":[{\"mustHitSection\":true,"
            "\"sectionNotes\":[[10,0,0]]}]}}"
        );
        write_text(
            bounded_events,
            "{\"events\":[[100,[[\"Event\",\"value-1\",\"value-2\"]]]]}"
        );
        pulseforge::StreamingChartCacheOptions bounded_options;
        bounded_options.cache_root = bounded_fixture / "cache";
        bounded_options.import.input_buffer_bytes = 4'096U;
        bounded_options.import.max_adjacent_event_bytes = 16U;
        const auto bounded = pulseforge::prepare_streaming_chart_cache(
            bounded_source,
            bounded_options
        );
        require(
            !bounded
                && bounded.error.find("adjacent events JSON exceeds")
                    != std::string::npos,
            "adjacent event sidecars obey their explicit materialization budget"
        );
    }
    const auto pfc_write_time = std::filesystem::last_write_time(
        preferred.cache_path
    );

    // A lower-priority singular sidecar must not perturb the selected plural
    // signature or its already-built PFE derivative.
    write_text(
        singular,
        "{\"song\":{\"notes\":[{\"sectionNotes\":["
        "[75,-1,\"Fallback Selected\",\"fallback-1\",\"fallback-2\"]]}]}}"
    );
    const auto ignored_fallback = pulseforge::prepare_streaming_chart_cache(
        source,
        options
    );
    require(static_cast<bool>(ignored_fallback), ignored_fallback.error);
    require(
        ignored_fallback.reused
            && ignored_fallback.chart_metadata.events.size() == 2U
            && ignored_fallback.chart_metadata.events[0U].name
                == "Plural Object",
        "changing event.json is ignored while events.json is selected"
    );

    std::error_code remove_error;
    require(
        std::filesystem::remove(plural, remove_error) && !remove_error,
        "preferred events.json fixture can be removed"
    );
    const auto fallback = pulseforge::prepare_streaming_chart_cache(
        source,
        options
    );
    require(static_cast<bool>(fallback), fallback.error);
    require(
        fallback.reused
            && fallback.chart_metadata.events.size() == 1U
            && fallback.chart_metadata.events[0U].time_ms == 75.0
            && fallback.chart_metadata.events[0U].name == "Fallback Selected"
            && fallback.chart_metadata.events[0U].value1 == "fallback-1"
            && fallback.chart_metadata.events[0U].value2 == "fallback-2",
        "event.json fallback parses song-wrapped negative-lane section events"
    );
    require(
        fallback.cache_path == preferred.cache_path
            && std::filesystem::last_write_time(fallback.cache_path)
                == pfc_write_time,
        "sidecar selection changes repair PFE without recompiling PFC"
    );
    const auto fallback_manifest = read_text(manifest);
    require(
        fallback_manifest.find("/event.json") != std::string::npos
            && fallback_manifest.find("/events.json") == std::string::npos,
        "cache signature switches to the exact fallback sidecar path"
    );
}

void test_runtime_cache_fingerprint_invalidation(
    const std::filesystem::path& directory
) {
    const auto content_root = directory / "cache-content";
    const auto source_directory = content_root / "data" / "Cached";
    const auto audio_directory = content_root / "songs" / "Cached";
    std::filesystem::create_directories(source_directory);
    std::filesystem::create_directories(audio_directory);
    const auto source = source_directory / "cache-source.json";
    const auto cache = directory / "runtime-cache";
    const auto instrumental = audio_directory / "Inst.ogg";
    const auto voices = audio_directory / "Voices.ogg";
    write_text(instrumental, "fixture-inst");
    write_text(voices, "fixture-voices");
    const std::string first_json =
        "{\"song\":{\"song\":\"Cached\",\"bpm\":120,\"notes\":["
        "{\"mustHitSection\":true,\"sectionNotes\":[[10,0,0]]}]}}";
    const std::string changed_json =
        "{\"song\":{\"song\":\"Cached\",\"bpm\":120,\"notes\":["
        "{\"mustHitSection\":true,\"sectionNotes\":[[20,0,0]]}]}}";
    require(first_json.size() == changed_json.size(), "cache fixture sizes match");
    write_text(source, first_json);
    const auto original_time = std::filesystem::last_write_time(source);

    pulseforge::StreamingChartCacheOptions options;
    options.cache_root = cache;
    options.import.input_buffer_bytes = 4'096U;
    options.strict_cache_validation = true;
    const auto compiled = pulseforge::prepare_streaming_chart_cache(
        source,
        options
    );
    require(static_cast<bool>(compiled), compiled.error);
    require(!compiled.reused, "first cache request compiles");
    require(std::filesystem::exists(compiled.cache_path), "PFC cache is published");
    require(
        !compiled.visual_density_path.empty()
            && std::filesystem::exists(compiled.visual_density_path),
        "streaming compile publishes its optional visual-density sidecar"
    );
    std::string visual_error;
    const auto compiled_visual = pulseforge::VisualDensityIndexReader::open(
        compiled.visual_density_path,
        &visual_error
    );
    require(compiled_visual.has_value(), visual_error);
    require(
        std::filesystem::equivalent(
            compiled.chart_metadata.audio.instrumental,
            instrumental
        ),
        "streaming compile resolves the selected song Inst stem"
    );
    require(
        compiled.chart_metadata.audio.vocals.size() == 1U
            && std::filesystem::equivalent(
                compiled.chart_metadata.audio.vocals.front(),
                voices
            ),
        "streaming compile resolves Voices only from the selected song"
    );

    const auto reused = pulseforge::prepare_streaming_chart_cache(
        source,
        options
    );
    require(static_cast<bool>(reused), reused.error);
    require(reused.reused, "unchanged source reuses its verified cache");
    require(
        std::filesystem::equivalent(
            reused.chart_metadata.audio.instrumental,
            instrumental
        ) && reused.chart_metadata.audio.vocals.size() == 1U
            && std::filesystem::equivalent(
                reused.chart_metadata.audio.vocals.front(),
                voices
            ),
        "cache reuse re-resolves the authoritative selected-song stems"
    );
    require(
        reused.source_fingerprint == compiled.source_fingerprint,
        "reused cache retains source fingerprint"
    );

    // PVD1 is a disposable derivative. Interactive reuse must preserve PFC1
    // without blocking to rebuild it; an offline-render request then opts in
    // to deterministic reconstruction without recompiling the source chart.
    std::error_code remove_error;
    std::filesystem::remove(compiled.visual_density_path, remove_error);
    require(!remove_error, "visual sidecar fixture can be removed");
    const auto optional_visual = pulseforge::prepare_streaming_chart_cache(
        source,
        options
    );
    require(static_cast<bool>(optional_visual), optional_visual.error);
    require(
        optional_visual.reused
            && optional_visual.visual_density_path.empty(),
        "interactive reuse does not synchronously rebuild a missing optional PVD"
    );

    options.require_visual_density = true;
    const auto lazy_visual = pulseforge::prepare_streaming_chart_cache(
        source,
        options
    );
    require(static_cast<bool>(lazy_visual), lazy_visual.error);
    require(
        lazy_visual.reused,
        "required PVD reconstruction preserves the authoritative PFC cache"
    );
    require(
        !lazy_visual.visual_density_path.empty()
            && std::filesystem::exists(lazy_visual.visual_density_path),
        "missing PVD is reconstructed lazily on cache reuse"
    );
    visual_error.clear();
    require(
        pulseforge::VisualDensityIndexReader::open(
            lazy_visual.visual_density_path,
            &visual_error
        ).has_value(),
        visual_error
    );

    // A schema-4 PFC cache may still carry an older/incompatible PVD version.
    // Corrupt just the derivative header and verify warm reuse regenerates it
    // without touching/recompiling the authoritative PFC.
    {
        std::fstream incompatible(
            lazy_visual.visual_density_path,
            std::ios::binary | std::ios::in | std::ios::out
        );
        require(static_cast<bool>(incompatible), "PVD fixture opens for corruption");
        incompatible.seekp(4, std::ios::beg);
        const char incompatible_version[2]{0, 0};
        incompatible.write(incompatible_version, 2);
        require(static_cast<bool>(incompatible), "PVD fixture version is corrupted");
    }
    const auto upgraded_visual = pulseforge::prepare_streaming_chart_cache(
        source,
        options
    );
    require(static_cast<bool>(upgraded_visual), upgraded_visual.error);
    require(
        upgraded_visual.reused && !upgraded_visual.visual_density_path.empty(),
        "incompatible PVD is upgraded while schema-4 PFC remains reused"
    );
    visual_error.clear();
    require(
        pulseforge::VisualDensityIndexReader::open(
            upgraded_visual.visual_density_path,
            &visual_error
        ).has_value(),
        visual_error
    );
    const auto upgraded_pvd_bytes = std::filesystem::file_size(
        upgraded_visual.visual_density_path
    );
    require(upgraded_pvd_bytes > 1U, "upgraded PVD has a truncatable payload");
    std::filesystem::resize_file(
        upgraded_visual.visual_density_path,
        upgraded_pvd_bytes - 1U
    );
    const auto repaired_visual = pulseforge::prepare_streaming_chart_cache(
        source,
        options
    );
    require(static_cast<bool>(repaired_visual), repaired_visual.error);
    require(
        repaired_visual.reused
            && std::filesystem::file_size(repaired_visual.visual_density_path)
                == upgraded_pvd_bytes,
        "manifest-bound PVD size mismatch is rebuilt without recompiling PFC"
    );

    const auto manifest = compiled.cache_path.parent_path()
        / (compiled.cache_path.stem().string() + ".manifest.json");
    auto old_schema_manifest = read_text(manifest);
    constexpr std::string_view current_schema = "\"schema\": 7";
    const auto schema_position = old_schema_manifest.find(current_schema);
    require(
        schema_position != std::string::npos,
        "runtime cache manifest preserves schema 7"
    );
    old_schema_manifest.replace(
        schema_position,
        current_schema.size(),
        "\"schema\": 3"
    );
    write_text(manifest, old_schema_manifest);
    const auto schema_rebuilt = pulseforge::prepare_streaming_chart_cache(
        source,
        options
    );
    require(static_cast<bool>(schema_rebuilt), schema_rebuilt.error);
    require(
        !schema_rebuilt.reused
            && schema_rebuilt.source_fingerprint
                == compiled.source_fingerprint,
        "schema-3 manifests are invalidated despite an unchanged source"
    );

    write_text(source, changed_json);
    std::filesystem::last_write_time(source, original_time);
    const auto rebuilt = pulseforge::prepare_streaming_chart_cache(
        source,
        options
    );
    require(static_cast<bool>(rebuilt), rebuilt.error);
    require(
        !rebuilt.reused,
        "same-size/same-time source with different bytes is rebuilt"
    );
    require(
        rebuilt.source_fingerprint != compiled.source_fingerprint,
        "full-file fingerprint detects the source mutation"
    );
}

void test_limits_and_atomic_failure(const std::filesystem::path& directory) {
    const auto source = directory / "limited.json";
    write_text(
        source,
        "{\"notes\":[{\"time\":0,\"lane\":0},{\"time\":1,\"lane\":1}]}"
    );

    pulseforge::StreamingChartImportOptions options;
    require(
        options.max_source_bytes == std::numeric_limits<std::uint64_t>::max()
            && options.max_notes
                == std::numeric_limits<std::uint64_t>::max()
            && options.max_sections == 1'000'000'000ULL,
        "streaming importer leaves source/note totals open while bounding its materialized section map"
    );
    options.input_buffer_bytes = 4'096U;
    options.max_notes = 1U;
    const auto note_destination = directory / "too-many.pfc";
    const auto notes = pulseforge::compile_streaming_json_chart_to_pfc(
        source,
        note_destination,
        options
    );
    require(!static_cast<bool>(notes), "configured 64-bit note limit is enforced");
    require(!std::filesystem::exists(note_destination), "failed import commits no PFC");

    options.max_notes = 10U;
    options.max_source_bytes = 8U;
    const auto byte_destination = directory / "too-large.pfc";
    const auto bytes = pulseforge::compile_streaming_json_chart_to_pfc(
        source,
        byte_destination,
        options
    );
    require(!static_cast<bool>(bytes), "configured 64-bit byte limit is enforced");
    require(!std::filesystem::exists(byte_destination), "byte-limit failure is atomic");
}

void test_optional_visual_budget_preserves_pfc(
    const std::filesystem::path& directory
) {
    const auto source = directory / "hostile-visual-duration.json";
    const auto packed = directory / "hostile-visual-duration.pfc";
    write_text(
        source,
        "{\"notes\":[{\"time\":0,\"duration\":18446744073709550,"
        "\"lane\":0}]}"
    );
    pulseforge::StreamingChartImportOptions options;
    options.input_buffer_bytes = 4'096U;
    const auto begin = std::chrono::steady_clock::now();
    const auto result = pulseforge::compile_streaming_json_chart_to_pfc(
        source,
        packed,
        options
    );
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - begin
    ).count();
    require(static_cast<bool>(result), result.error);
    require(
        result.visual_density_path.empty()
            && result.visual_density_error.find("sidecar budget")
                != std::string::npos,
        "hostile duration disables only its optional PVD sidecar"
    );
    require(elapsed < 5.0, "hostile PVD rejection is bounded");
    std::string error;
    const auto reader = pulseforge::PackedChartReader::open(packed, &error);
    require(reader.has_value(), error);
    require(
        reader->logical_note_count() == 1U
            && reader->explicit_note_count() == 1U,
        "authoritative PFC survives optional PVD budget rejection"
    );
}

void test_many_unique_sustain_ends_preserve_pfc(
    const std::filesystem::path& directory
) {
    const auto source = directory / "many-unique-sustain-ends.json";
    const auto packed = directory / "many-unique-sustain-ends.pfc";
    std::ofstream output(source, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "unique sustain fixture opens");
    output << "{\"notes\":[";
    constexpr std::size_t note_count =
        pulseforge::visual_density_max_pending_end_buckets + 1U;
    for (std::size_t index = 0U; index < note_count; ++index) {
        if (index != 0U) {
            output.put(',');
        }
        output << "{\"time\":0,\"duration\":" << index + 1U
               << ",\"lane\":0}";
    }
    output << "]}";
    output.close();
    require(static_cast<bool>(output), "unique sustain fixture flushes");

    pulseforge::StreamingChartImportOptions options;
    options.input_buffer_bytes = 64U * 1024U;
    const auto result = pulseforge::compile_streaming_json_chart_to_pfc(
        source,
        packed,
        options
    );
    require(static_cast<bool>(result), result.error);
    require(
        result.logical_note_count == note_count
            && result.explicit_note_count == note_count,
        "all hostile sustain notes remain in authoritative PFC"
    );
    require(
        result.visual_density_path.empty()
            && result.visual_density_error.find("pending sustain ends")
                != std::string::npos,
        "unique sustain end pressure disables only optional PVD"
    );
    std::string error;
    const auto reader = pulseforge::PackedChartReader::open(packed, &error);
    require(reader.has_value(), error);
}

}  // namespace

int main() {
    try {
        const TemporaryDirectory directory;
        test_stream_writer(directory.path());
        test_native_single_line_external_sort(directory.path());
        test_psych_owner_and_field_order(directory.path());
        test_psych_third_strum_pfc_identity(directory.path());
        test_denpa_third_strum_pfc_identity(directory.path());
        test_psych_negative_sustain_compatibility(directory.path());
        test_sorted_streaming_path(directory.path());
        test_psych_options_mania_index(directory.path());
        test_lane_domain_key_mode_detection(directory.path());
        test_streaming_metadata_projection(directory.path());
        test_adjacent_event_sidecar_compatibility(directory.path());
        test_runtime_cache_fingerprint_invalidation(directory.path());
        test_limits_and_atomic_failure(directory.path());
        test_optional_visual_budget_preserves_pfc(directory.path());
        test_many_unique_sustain_ends_preserve_pfc(directory.path());
        std::cout << "streaming chart importer tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "streaming chart importer test failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
