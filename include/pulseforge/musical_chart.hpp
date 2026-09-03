#pragma once

#include "pulseforge/chart.hpp"
#include "pulseforge/packed_chart.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>

namespace pulseforge {

struct MusicalChartLimits {
    // Total counts are representation-limited by default. Per-buffer and
    // metadata fields below remain bounded so MIDI/PFM conversion stays
    // streaming where the format permits it. Applications may lower any
    // total for an untrusted-input policy.
    std::uint64_t max_source_bytes{
        std::numeric_limits<std::uint64_t>::max()
    };
    std::uint64_t max_midi_notes{
        std::numeric_limits<std::uint64_t>::max()
    };
    std::uint64_t max_pfm_explicit_notes{
        std::numeric_limits<std::uint64_t>::max()
    };
    // PFM patterns and their lane cycles are materialized as vectors. These
    // are independent allocation budgets, not chart-wide note ceilings: each
    // PatternRun may still represent up to UINT64_MAX logical occurrences.
    std::uint64_t max_pfm_patterns{1'000'000ULL};
    std::uint64_t max_pattern_lanes{4'000'000ULL};
    std::uint64_t max_logical_notes{
        std::numeric_limits<std::uint64_t>::max()
    };
    std::size_t max_metadata_bytes{4U * 1024U * 1024U};
    std::size_t max_kind_bytes{4'096U};
    std::size_t max_sort_notes_in_memory{262'144U};
    std::size_t max_midi_tracks{65'535U};
};

struct MidiChartOptions {
    std::uint32_t ppqn{960U};
    std::uint8_t lane_base_note{60U};
    std::uint8_t opponent_channel{0U};
    std::uint8_t player_channel{1U};
    std::uint16_t fallback_key_count{4U};
    bool embed_metadata{true};
    bool write_sidecar_metadata{true};
    MusicalChartLimits limits;
    PackedChartWriteOptions packed;
};

struct PfmChartOptions {
    std::uint32_t ppqn{960U};
    // Sidecar metadata is the storage-light/mod-friendly default. Set true for
    // a self-contained single-file PFM; the note-kind dictionary always stays
    // in the binary chart regardless.
    bool embed_metadata{false};
    bool write_sidecar_metadata{true};
    MusicalChartLimits limits;
    PackedChartWriteOptions packed;
};

struct MusicalChartCompileResult {
    bool success{};
    ChartFormat source_format{ChartFormat::native};
    std::uint64_t source_bytes{};
    std::uint64_t source_fingerprint{};
    std::uint64_t explicit_note_count{};
    std::uint64_t logical_note_count{};
    std::uint64_t content_end_us{};
    std::uint16_t key_count{};
    Chart chart_metadata;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return success;
    }
};

// Standard MIDI File (SMF) importer. Format 0 and 1 are accepted. PPQN timing
// is supported; SMPTE division is rejected because rhythm-chart timing needs a
// musical tick domain. PulseForge exports channel 0=opponent, channel 1=player,
// note 60+lane, and optional sequencer-specific metadata for exact note kinds.
[[nodiscard]] MusicalChartCompileResult compile_midi_chart_to_pfc(
    const std::filesystem::path& source_midi,
    const std::filesystem::path& destination_pfc,
    const MidiChartOptions& options = {}
);

// Binary PulseForge Musical Chart. PFM stores PPQN, tempo map, metadata, kind
// dictionary, explicit notes and arithmetic PatternRuns. PatternRuns remain
// constant-storage even for trillion-note sequences.
[[nodiscard]] MusicalChartCompileResult compile_pfm_chart_to_pfc(
    const std::filesystem::path& source_pfm,
    const std::filesystem::path& destination_pfc,
    const PfmChartOptions& options = {}
);

// Human-editable compact PFM source. A single RUN object can describe trillions
// of notes without expanding them. The source is compiled directly to PFC1.
[[nodiscard]] MusicalChartCompileResult compile_pfm_source_to_pfc(
    const std::filesystem::path& source_json,
    const std::filesystem::path& destination_pfc,
    const PfmChartOptions& options = {}
);

[[nodiscard]] bool export_chart_to_midi(
    const Chart& chart,
    const std::filesystem::path& destination_midi,
    const MidiChartOptions& options = {},
    std::string* error = nullptr
);

// Converts an already compiled/streamable chart to compact PFM without
// materializing PatternRuns. Explicit PFC1 chunks are processed one at a time.
[[nodiscard]] bool export_packed_chart_to_pfm(
    const PackedChartReader& reader,
    const Chart& metadata,
    const std::filesystem::path& destination_pfm,
    const PfmChartOptions& options = {},
    std::string* error = nullptr
);

// Writes a tiny declarative .pfm.json template. The default RUN demonstrates a
// one-trillion-note 4-lane cycle while occupying only a few hundred bytes.
[[nodiscard]] bool write_pfm_source_template(
    const std::filesystem::path& destination,
    std::uint32_t ppqn = 960U,
    std::string* error = nullptr
);

[[nodiscard]] bool is_midi_chart_path(
    const std::filesystem::path& path
) noexcept;

[[nodiscard]] bool is_pfm_chart_path(
    const std::filesystem::path& path
) noexcept;

[[nodiscard]] bool is_pfm_source_path(
    const std::filesystem::path& path
) noexcept;

}  // namespace pulseforge
