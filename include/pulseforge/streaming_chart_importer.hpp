#pragma once

#include "pulseforge/chart.hpp"
#include "pulseforge/packed_chart.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>

namespace pulseforge {

// Limits for the JSON-to-PFC1 compilation path. All counters and file offsets
// are 64-bit. Total source/count defaults have no product-policy ceiling;
// checked arithmetic, the platform file API and the PFC1 geometry are the
// authoritative limits. Buffer/token sizes still bound working memory.
struct StreamingChartImportOptions {
    std::uint64_t max_source_bytes{
        std::numeric_limits<std::uint64_t>::max()
    };
    std::uint64_t max_notes{std::numeric_limits<std::uint64_t>::max()};
    // Psych sections are tracked in a materialized vector<bool>. Keep that
    // allocation finite without limiting the 64-bit note/source totals.
    std::uint64_t max_sections{1'000'000'000ULL};
    std::size_t input_buffer_bytes{256U * 1024U};
    std::size_t max_json_depth{128U};
    std::size_t max_string_source_bytes{4U * 1024U * 1024U};
    std::size_t max_decoded_string_bytes{1U * 1024U * 1024U};
    // Adjacent Psych events.json/event.json is parsed with nlohmann::json and
    // therefore materialized in memory. Keep a separate, configurable input
    // budget instead of tying it to the effectively-unbounded note JSON size.
    std::uint64_t max_adjacent_event_bytes{64U * 1024U * 1024U};
    std::size_t max_sort_notes_in_memory{262'144U};
    std::size_t max_merge_fan_in{64U};
    std::uint32_t notes_per_pfc_chunk{65'536U};
    PackedChartLimits packed_limits;
};

struct StreamingChartImportResult {
    bool success{};
    ChartFormat source_format{ChartFormat::native};
    std::uint64_t source_bytes{};
    std::uint64_t explicit_note_count{};
    std::uint64_t logical_note_count{};
    std::uint64_t skipped_entry_count{};
    std::uint64_t section_count{};
    std::uint64_t pfc_chunk_count{};
    std::uint64_t peak_buffered_notes{};
    std::uint16_t key_count{};
    std::uint64_t kind_count{};
    // FNV-1a over every source byte. The compiler compares this value across
    // both parsing passes. The runtime cache persists it; strict cache validation
    // can reread the source to verify it on a later cache hit.
    std::uint64_t source_fingerprint{};
    // Furthest note/event time in the authoritative source. This is also the
    // fallback clock boundary when the selected song has no decodable audio stem.
    std::uint64_t content_end_us{};
    std::uint64_t event_count{};
    // Bounded metadata projection used by the visual streaming runtime. Notes
    // remain empty. Chart events are bounded by maximum_chart_events and are
    // persisted into the private PFE1 event sidecar by the runtime cache.
    Chart chart_metadata;
    std::string requested_song_id;
    bool discover_vocals{true};
    bool input_was_time_sorted{};
    bool used_external_sort{};
    // Optional multi-resolution, fixed-memory visual sidecar generated beside
    // PFC1. A failure to create it never invalidates the authoritative PFC1;
    // the runtime falls back to exact chunk visits.
    std::filesystem::path visual_density_path;
    std::filesystem::path event_sidecar_path;
    std::string visual_density_error;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return success; }
};

// Compiles the explicit note stream from native PulseForge or Psych-family
// JSON into an indexed PFC1 note cache. The lexer never loads the complete
// document and is independent of line breaks, so a multi-gigabyte single-line
// JSON file follows the same bounded path as a pretty-printed file.
//
// PFC1 currently stores notes/kinds/key count, not chart metadata, tempos or
// events. Source JSON therefore remains authoritative. This function does not
// imply that the interactive gameplay runtime consumes PFC1 lazily.
[[nodiscard]] StreamingChartImportResult
compile_streaming_json_chart_to_pfc(
    const std::filesystem::path& source_json,
    const std::filesystem::path& destination_pfc,
    const StreamingChartImportOptions& options = {}
);

struct StreamingChartCacheOptions {
    // Empty selects <current working directory>/out/cache/large-charts.
    std::filesystem::path cache_root;
    StreamingChartImportOptions import;
    std::string difficulty{"normal"};
    bool difficulty_explicit{false};

    // false: path + size + modification time + a valid PFC1 are enough for a
    // normal cache hit. true: additionally reread/hash the complete source JSON.
    bool strict_cache_validation{false};

    // Offline rendering benefits from the multi-resolution visual index on
    // every frame. When true, a missing/corrupt PVD1 is rebuilt during render
    // preparation and its size is committed to the manifest. Interactive play
    // leaves this false and falls back immediately to exact PFC1 visits.
    bool require_visual_density{false};
};

struct StreamingChartCacheResult {
    bool success{};
    bool reused{};
    std::filesystem::path cache_path;
    std::filesystem::path visual_density_path;
    std::filesystem::path event_sidecar_path;
    std::optional<PackedChartReader> reader;
    Chart chart_metadata;
    std::uint64_t source_bytes{};
    std::uint64_t source_fingerprint{};
    std::uint64_t content_end_us{};
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return success && reader.has_value();
    }
};

// Produces or reuses the private PFC1 cache consumed by the SDL runtime. The
// JSON source is never modified. A small manifest is committed only after the
// PFC1 writer has atomically published and the reader has verified its header.
// Reuse normally requires matching canonical source path, size and write time,
// plus a valid PFC1 reader/count/key-count check. strict_cache_validation also
// requires a matching full-file FNV-1a fingerprint.
[[nodiscard]] StreamingChartCacheResult prepare_streaming_chart_cache(
    const std::filesystem::path& source_json,
    const StreamingChartCacheOptions& options = {}
);

}  // namespace pulseforge
