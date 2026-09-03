#pragma once

#include "pulseforge/chart.hpp"
#include "pulseforge/packed_chart.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pulseforge {

enum class ChartPayloadPolicy : std::uint8_t {
    reject_non_empty,
    discard,
};

enum class PackedPatternPolicy : std::uint8_t {
    reject,
    ignore,
};

struct ChartToPackedOptions {
    std::size_t max_notes{maximum_chart_notes};
    std::size_t max_kinds{maximum_chart_notes};
    std::size_t max_kind_bytes{maximum_chart_note_kind_bytes};
    std::size_t max_total_kind_bytes{16U * 1024U * 1024U};
    // Chart::Note has no flags member. A non-empty sidecar must contain exactly
    // one entry per note; an empty sidecar means all flags are zero.
    std::span<const std::uint16_t> note_flags;
    ChartPayloadPolicy payload_policy{ChartPayloadPolicy::reject_non_empty};
};

struct PackedToChartOptions {
    std::size_t max_notes{maximum_chart_notes};
    std::size_t max_kinds{maximum_chart_notes};
    std::size_t max_kind_bytes{maximum_chart_note_kind_bytes};
    std::size_t max_total_kind_bytes{16U * 1024U * 1024U};
    // Pattern runs are never expanded. Ignoring them requires an explicit opt-in.
    PackedPatternPolicy pattern_policy{PackedPatternPolicy::reject};
};

struct PackedChartInspectionOptions {
    bool scan_explicit_notes{};
    std::uint64_t max_explicit_notes_to_scan{maximum_chart_notes};
};

struct PackedChartStatistics {
    std::uint16_t key_count{};
    std::uint64_t kind_count{};
    std::uint64_t explicit_note_count{};
    std::uint64_t logical_note_count{};
    std::uint64_t pattern_run_count{};
    std::uint64_t pattern_note_count{};
    std::uint64_t chunk_count{};
    std::optional<std::int64_t> first_explicit_time_us;
    std::optional<std::int64_t> last_explicit_time_us;

    // These counters are authoritative only when explicit_notes_scanned is true.
    bool explicit_notes_scanned{};
    std::uint64_t player_note_count{};
    std::uint64_t opponent_note_count{};
    std::uint64_t hold_note_count{};
    std::uint64_t nonzero_flag_note_count{};
    std::uint64_t discarded_payload_note_count{};
};

struct ChartToPackedResult {
    PackedChartData chart;
    PackedChartStatistics statistics;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error.empty();
    }
};

struct PackedToChartResult {
    Chart chart;
    // Aligned with chart.notes. This preserves PFC1 flags without abusing
    // Chart::Note::payload_id, whose semantics are unrelated.
    std::vector<std::uint16_t> note_flags;
    PackedChartStatistics statistics;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error.empty();
    }
};

struct PackedChartInspectionResult {
    PackedChartStatistics statistics;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error.empty();
    }
};

// Conversion is exact: a timestamp or duration that cannot make a lossless
// milliseconds <-> microseconds round-trip is rejected instead of rounded.
[[nodiscard]] ChartToPackedResult convert_chart_to_packed(
    const Chart& chart,
    const ChartToPackedOptions& options = {}
);

// Converts explicit notes only. Pattern runs follow pattern_policy and are
// never implicitly materialized into Chart::notes.
[[nodiscard]] PackedToChartResult convert_packed_to_chart(
    const PackedChartReader& reader,
    const PackedToChartOptions& options = {}
);

// Metadata inspection does not decode note payloads. Set scan_explicit_notes
// to verify every chunk CRC and calculate owner/hold/flag counters under budget.
[[nodiscard]] PackedChartInspectionResult inspect_packed_chart(
    const PackedChartReader& reader,
    const PackedChartInspectionOptions& options = {}
);

}  // namespace pulseforge
