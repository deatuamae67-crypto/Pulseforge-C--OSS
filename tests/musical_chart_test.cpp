#include "pulseforge/musical_chart.hpp"
#include "pulseforge/packed_chart_stream.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void write_be16(std::ostream& output, const std::uint16_t value) {
    output.put(static_cast<char>((value >> 8U) & 0xFFU));
    output.put(static_cast<char>(value & 0xFFU));
}

void write_be32(std::ostream& output, const std::uint32_t value) {
    output.put(static_cast<char>((value >> 24U) & 0xFFU));
    output.put(static_cast<char>((value >> 16U) & 0xFFU));
    output.put(static_cast<char>((value >> 8U) & 0xFFU));
    output.put(static_cast<char>(value & 0xFFU));
}

std::filesystem::path unique_test_root() {
    const auto stamp = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    auto root = std::filesystem::temp_directory_path()
        / ("pulseforge-musical-chart-test-" + std::to_string(stamp));
    std::filesystem::create_directories(root);
    return root;
}

void test_minimal_midi_import(const std::filesystem::path& root) {
    using namespace pulseforge;
    const auto midi = root / "minimal.mid";
    const auto pfc = root / "minimal.pfc";
    {
        std::ofstream output(midi, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output), "cannot create minimal MIDI fixture");
        output.write("MThd", 4);
        write_be32(output, 6U);
        write_be16(output, 0U);
        write_be16(output, 1U);
        write_be16(output, 480U);
        const std::vector<std::uint8_t> track{
            0x00U, 0xFFU, 0x51U, 0x03U, 0x07U, 0xA1U, 0x20U,
            0x00U, 0x91U, 60U, 100U,
            0x83U, 0x60U, 0x81U, 60U, 0U,
            0x00U, 0xFFU, 0x2FU, 0x00U,
        };
        output.write("MTrk", 4);
        write_be32(output, static_cast<std::uint32_t>(track.size()));
        output.write(
            reinterpret_cast<const char*>(track.data()),
            static_cast<std::streamsize>(track.size())
        );
    }
    const auto compiled = compile_midi_chart_to_pfc(midi, pfc);
    require(static_cast<bool>(compiled), compiled.error);
    require(
        compiled.source_format == ChartFormat::midi,
        "MIDI import reports the wrong source format"
    );
    std::string error;
    const auto reader = PackedChartReader::open(pfc, &error);
    require(reader.has_value(), error);
    require(
        reader->explicit_note_count() == 1U,
        "minimal MIDI did not produce exactly one explicit note"
    );
    const auto notes = reader->read_chunk(0U);
    require(static_cast<bool>(notes), notes.error);
    require(notes.notes.size() == 1U, "minimal MIDI chunk size is incorrect");
    require(notes.notes.front().lane == 0U, "minimal MIDI lane is incorrect");
    require(
        notes.notes.front().owner == PackedNoteOwner::player,
        "minimal MIDI owner is incorrect"
    );
    require(
        notes.notes.front().duration_us == 500'000U,
        "minimal MIDI duration is incorrect"
    );
}


void test_secondary_opponent_midi_export(const std::filesystem::path& root) {
    using namespace pulseforge;
    // PULSEFORGE_P1_4_0_MUSICAL_SECONDARY_OPPONENT_TEST_V1
    Chart chart;
    chart.title = "Third Strum MIDI";
    chart.key_count = 4U;
    chart.tempos = {{0.0, 120.0, 4U, 4U}};
    chart.notes = {{
        500.0,
        125.0,
        2U,
        NoteOwner::secondary_opponent,
        "Third Strum",
        0U,
    }};
    chart.secondary_opponent_enabled = true;
    chart.normalize();

    const auto midi = root / "third-strum.mid";
    const auto pfc = root / "third-strum.pfc";
    std::string error;
    require(export_chart_to_midi(chart, midi, {}, &error), error);
    const auto compiled = compile_midi_chart_to_pfc(midi, pfc);
    require(static_cast<bool>(compiled), compiled.error);
    const auto reader = PackedChartReader::open(pfc, &error);
    require(reader.has_value(), error);
    const auto notes = reader->read_chunk(0U);
    require(static_cast<bool>(notes), notes.error);
    require(notes.notes.size() == 1U, "Third Strum MIDI note count changed");
    require(
        notes.notes.front().owner == PackedNoteOwner::opponent,
        "secondary opponent must stay on the physical AI MIDI side"
    );
    require(
        notes.notes.front().kind_id < reader->kinds().size()
            && reader->kinds()[notes.notes.front().kind_id] == "Third Strum",
        "Third Strum MIDI export must preserve the canonical kind identity"
    );
}

void test_trillion_pattern_roundtrip(const std::filesystem::path& root) {
    using namespace pulseforge;
    const auto pfc = root / "trillion.pfc";
    const auto pfm = root / "trillion.pfm";
    const auto roundtrip = root / "trillion-roundtrip.pfc";

    PatternRun pattern;
    pattern.start_us = 0;
    pattern.interval_us = 500'000U;
    pattern.interval_denominator = 3U;
    pattern.count = 1'000'000'000'000ULL;
    pattern.lane_pattern = {0U, 1U, 2U, 3U};

    PackedChartStreamSpec spec;
    spec.key_count = 4U;
    spec.kinds = {"normal"};
    spec.patterns = {pattern};
    std::string error;
    auto writer = PackedChartStreamWriter::create(pfc, spec, {}, &error);
    require(writer.has_value(), error);
    require(writer->finish(&error), error);

    auto reader = PackedChartReader::open(pfc, &error);
    require(reader.has_value(), error);
    Chart metadata;
    metadata.key_count = 4U;
    metadata.tempos = {{0.0, 120.0, 4U, 4U}};
    PfmChartOptions options;
    options.ppqn = 960U;
    options.embed_metadata = false;
    options.write_sidecar_metadata = false;
    require(
        export_packed_chart_to_pfm(
            *reader,
            metadata,
            pfm,
            options,
            &error
        ),
        error
    );
    const auto compiled = compile_pfm_chart_to_pfc(pfm, roundtrip, options);
    require(static_cast<bool>(compiled), compiled.error);
    require(
        compiled.logical_note_count == 1'000'000'000'000ULL,
        "trillion-note PFM logical count changed during round trip"
    );

    const auto reopened = PackedChartReader::open(roundtrip, &error);
    require(reopened.has_value(), error);
    require(
        reopened->patterns().size() == 1U,
        "trillion-note PFM pattern count changed during round trip"
    );
    require(
        reopened->patterns().front().interval_denominator == 3U,
        "fractional pattern interval changed during round trip"
    );
    const auto fourth = reopened->patterns().front().note_at(3U);
    require(fourth.has_value(), "cannot address the fourth pattern note");
    require(
        fourth->time_us == 500'000,
        "fractional pattern timing changed during round trip"
    );
    // The entire trillion-note source remains tiny because no notes were
    // physically expanded.
    require(
        std::filesystem::file_size(pfm) < 1'024U,
        "trillion-note PFM unexpectedly expanded its pattern"
    );
}

void test_explicit_run_detection(const std::filesystem::path& root) {
    using namespace pulseforge;
    const auto source = root / "arithmetic.pfc";
    const auto pfm = root / "arithmetic.pfm";
    const auto roundtrip = root / "arithmetic-roundtrip.pfc";

    PackedChartStreamSpec spec;
    spec.key_count = 4U;
    spec.explicit_note_count = 100U;
    spec.kinds = {"normal"};
    std::string error;
    auto writer = PackedChartStreamWriter::create(source, spec, {}, &error);
    require(writer.has_value(), error);
    for (std::uint16_t index = 0U; index < 100U; ++index) {
        PackedNote note;
        note.time_us = static_cast<std::int64_t>(index) * 500;
        note.lane = static_cast<std::uint16_t>(index % 4U);
        note.owner = PackedNoteOwner::player;
        require(writer->append(note, &error), error);
    }
    require(writer->finish(&error), error);
    auto reader = PackedChartReader::open(source, &error);
    require(reader.has_value(), error);

    Chart metadata;
    metadata.key_count = 4U;
    metadata.tempos = {{0.0, 120.0, 4U, 4U}};
    PfmChartOptions options;
    options.ppqn = 1'000U;  // exactly 500 us/tick at 120 BPM
    options.embed_metadata = false;
    options.write_sidecar_metadata = false;
    require(
        export_packed_chart_to_pfm(
            *reader,
            metadata,
            pfm,
            options,
            &error
        ),
        error
    );
    const auto compiled = compile_pfm_chart_to_pfc(pfm, roundtrip, options);
    require(static_cast<bool>(compiled), compiled.error);
    const auto reopened = PackedChartReader::open(roundtrip, &error);
    require(reopened.has_value(), error);
    require(
        reopened->explicit_note_count() == 0U,
        "arithmetic notes were not collapsed into a pattern"
    );
    require(
        reopened->logical_note_count() == 100U,
        "arithmetic pattern logical note count is incorrect"
    );
    require(
        reopened->patterns().size() == 1U,
        "arithmetic notes produced the wrong number of patterns"
    );
    require(
        reopened->patterns().front().lane_pattern.size() == 4U,
        "arithmetic pattern lane cycle is incorrect"
    );
}

void test_pfm_source_beyond_legacy_logical_cap(
    const std::filesystem::path& root
) {
    using namespace pulseforge;
    constexpr std::uint64_t note_count = 10'000'000'000'000'001ULL;
    const auto source = root / "beyond-legacy-logical-limit.pfm.json";
    const auto packed = root / "beyond-legacy-logical-limit.pfc";
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output), "cannot create large logical PFM source");
        output
            << "{\"format\":\"pulseforge-pfm-source-v1\","
            << "\"ppqn\":960,\"keyCount\":4,\"kinds\":[\"normal\"],"
            << "\"runs\":[{\"startTick\":0,\"intervalTicks\":0,"
            << "\"durationTicks\":0,\"count\":" << note_count
            << ",\"lanes\":[0,1,2,3],\"owner\":\"player\","
            << "\"kindId\":0}]}";
        output.flush();
        require(static_cast<bool>(output), "cannot flush large logical PFM source");
    }

    const auto compiled = compile_pfm_source_to_pfc(source, packed);
    require(static_cast<bool>(compiled), compiled.error);
    require(
        compiled.logical_note_count == note_count,
        "PFM source import retains a count above the former 10^16 ceiling"
    );
    std::string error;
    const auto reader = PackedChartReader::open(packed, &error);
    require(reader.has_value(), error);
    require(
        reader->logical_note_count() == note_count
            && reader->patterns().size() == 1U
            && reader->explicit_note_count() == 0U,
        "large logical PFM source remains one constant-space PFC1 PatternRun"
    );
}

void test_materialized_pfm_defaults_are_bounded() {
    const pulseforge::MusicalChartLimits limits;
    require(
        limits.max_pfm_patterns == 1'000'000ULL
            && limits.max_pattern_lanes == 4'000'000ULL,
        "materialized PFM pattern vectors keep finite allocation budgets"
    );
    require(
        limits.max_pfm_explicit_notes
                == std::numeric_limits<std::uint64_t>::max()
            && limits.max_logical_notes
                == std::numeric_limits<std::uint64_t>::max(),
        "PFM allocation budgets do not restore chart-wide note ceilings"
    );
}

}  // namespace

int main() {
    const auto root = unique_test_root();
    try {
        test_minimal_midi_import(root);
        test_secondary_opponent_midi_export(root);
        test_trillion_pattern_roundtrip(root);
        test_explicit_run_detection(root);
        test_pfm_source_beyond_legacy_logical_cap(root);
        test_materialized_pfm_defaults_are_bounded();
        std::filesystem::remove_all(root);
        std::cout << "PulseForge MIDI/PFM tests passed\n";
        return 0;
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
}
