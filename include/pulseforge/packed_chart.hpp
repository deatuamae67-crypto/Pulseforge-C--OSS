#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pulseforge {

enum class PackedNoteOwner : std::uint8_t {
    opponent = 0,
    player = 1,
};

struct PackedNote {
    std::int64_t time_us{};
    std::uint64_t duration_us{};
    std::uint16_t lane{};
    PackedNoteOwner owner{PackedNoteOwner::player};
    std::uint16_t flags{};
    std::uint32_t kind_id{};

    [[nodiscard]] bool operator==(const PackedNote&) const noexcept = default;
};

// A compact arithmetic note sequence. Even a run with trillions of notes has
// constant storage and note_at() computes any element without expanding it.
struct PatternRun {
    std::int64_t start_us{};
    std::uint64_t interval_us{};
    std::uint64_t count{};
    std::uint64_t duration_us{};
    std::vector<std::uint16_t> lane_pattern;
    PackedNoteOwner owner{PackedNoteOwner::player};
    std::uint16_t flags{};
    std::uint32_t kind_id{};
    // Legacy PFC1 patterns use denominator=1. A denominator >1 turns
    // interval_us into a rational microsecond numerator, allowing PPQN-derived
    // runs to remain exact/bounded instead of accumulating rounded interval
    // drift across billions or trillions of notes.
    std::uint32_t interval_denominator{1U};

    [[nodiscard]] std::optional<PackedNote> note_at(
        std::uint64_t index
    ) const noexcept;
    [[nodiscard]] bool operator==(const PatternRun&) const noexcept = default;
};

// Limits are part of the reader contract: counts are checked before any
// allocation and chunk payloads remain independently bounded. Total-file and
// total-count defaults deliberately follow the 64-bit format/platform
// representation instead of imposing a product policy ceiling. Callers can
// still lower any field for an untrusted-input policy.
struct PackedChartLimits {
    std::uint64_t max_file_bytes{static_cast<std::uint64_t>(
        std::numeric_limits<std::streamoff>::max()
    )};
    std::uint64_t max_explicit_notes{
        std::numeric_limits<std::uint64_t>::max()
    };
    std::uint64_t max_logical_notes{
        std::numeric_limits<std::uint64_t>::max()
    };
    std::uint64_t max_chunks{std::numeric_limits<std::uint64_t>::max()};
    // The chart-wide 64-bit totals above intentionally have no product
    // ceiling. Sections that the reader materializes into vectors keep
    // independent finite allocation budgets so a corrupt file cannot turn a
    // valid 64-bit count into an unbounded host allocation.
    std::uint64_t max_kinds{1'000'000ULL};
    std::uint64_t max_patterns{1'000'000ULL};
    std::uint64_t max_pattern_lanes{4'000'000ULL};
    std::uint64_t max_dictionary_bytes{64ULL * 1024ULL * 1024ULL};
    std::uint64_t max_pattern_bytes{128ULL * 1024ULL * 1024ULL};
    // Per-allocation budgets remain bounded. They do not cap total chart
    // size: a PFC1 file is processed one chunk/query window at a time.
    std::uint64_t max_chunk_bytes{64ULL * 1024ULL * 1024ULL};
    std::uint32_t max_kind_bytes{4'096U};
    std::size_t max_query_notes{1'000'000U};
};

struct PackedChartData {
    std::uint16_t key_count{4U};
    std::vector<std::string> kinds;
    std::vector<PackedNote> notes;
    std::vector<PatternRun> patterns;
};

struct PackedChartWriteOptions {
    std::uint32_t max_notes_per_chunk{65'536U};
    PackedChartLimits limits;
};

struct PackedChartChunkInfo {
    std::int64_t first_time_us{};
    std::int64_t last_time_us{};
    std::uint64_t first_note_index{};
    std::uint32_t note_count{};
    std::uint64_t file_offset{};
    std::uint64_t byte_size{};
    std::uint32_t crc32{};

    [[nodiscard]] bool operator==(
        const PackedChartChunkInfo&
    ) const noexcept = default;
};

struct PackedNoteReadResult {
    std::vector<PackedNote> notes;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error.empty();
    }
};

using PackedNoteVisitor = void (*)(void* context, const PackedNote& note) noexcept;

struct PackedNoteVisitResult {
    std::uint64_t notes_visited{};
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error.empty();
    }
};

// Stable global indices are required by bounded editors and patch journals.
// The index refers to the explicit-note order stored in PFC1 and therefore
// remains stable for the lifetime of a verified cache.
struct IndexedPackedNote {
    std::uint64_t index{};
    PackedNote note;
};

struct IndexedPackedNoteReadResult {
    std::vector<IndexedPackedNote> notes;
    // A viewport query may deliberately return a bounded prefix. This is not
    // data loss: callers can zoom in or page through narrower time ranges.
    bool truncated{};
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error.empty();
    }
};

// The destination must not already exist. The writer commits a fully flushed
// sibling temporary file with rename, so a failed write never damages another
// chart or silently overwrites user data.
[[nodiscard]] bool write_packed_chart(
    const std::filesystem::path& destination,
    const PackedChartData& chart,
    const PackedChartWriteOptions& options = {},
    std::string* error = nullptr
);

class PackedChartReader final {
public:
    [[nodiscard]] static std::optional<PackedChartReader> open(
        const std::filesystem::path& path,
        std::string* error = nullptr,
        const PackedChartLimits& limits = {}
    );

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] std::uint16_t key_count() const noexcept;
    [[nodiscard]] std::uint64_t explicit_note_count() const noexcept;
    [[nodiscard]] std::uint64_t logical_note_count() const noexcept;
    [[nodiscard]] std::uint64_t chunk_count() const noexcept;
    [[nodiscard]] std::span<const std::string> kinds() const noexcept;
    [[nodiscard]] std::span<const PatternRun> patterns() const noexcept;

    [[nodiscard]] std::optional<PackedChartChunkInfo> chunk_info(
        std::uint64_t chunk_index,
        std::string* error = nullptr
    ) const;
    [[nodiscard]] PackedNoteReadResult read_chunk(
        std::uint64_t chunk_index
    ) const;

    // Both endpoints are inclusive. Only intersecting chunks are decoded; the
    // directory itself is searched on disk and is never materialized wholesale.
    [[nodiscard]] PackedNoteReadResult read_explicit_notes_in_range(
        std::int64_t first_time_us,
        std::int64_t last_time_us,
        std::size_t max_results = 0U
    ) const;

    // Streams every explicit note in the inclusive interval through one
    // decoded chunk at a time. Unlike the vector-returning query, this has no
    // result-count ceiling and therefore supports lossless fixed-memory visual
    // aggregation for arbitrarily dense viewport ranges.
    [[nodiscard]] PackedNoteVisitResult visit_explicit_notes_in_range(
        std::int64_t first_time_us,
        std::int64_t last_time_us,
        void* context,
        PackedNoteVisitor visitor
    ) const;

    // Equivalent bounded range query with each note's stable PFC1 index.
    // Reaching max_results sets truncated and preserves the returned prefix
    // instead of converting a dense-but-valid viewport into an error.
    [[nodiscard]] IndexedPackedNoteReadResult
    read_indexed_explicit_notes_in_range(
        std::int64_t first_time_us,
        std::int64_t last_time_us,
        std::size_t max_results = 0U
    ) const;

private:
    std::filesystem::path path_;
    PackedChartLimits limits_;
    std::uint16_t key_count_{};
    std::uint64_t explicit_note_count_{};
    std::uint64_t logical_note_count_{};
    std::uint64_t chunk_count_{};
    std::uint64_t directory_offset_{};
    std::uint64_t directory_size_{};
    std::uint64_t note_data_offset_{};
    std::uint64_t note_data_size_{};
    std::uint64_t file_size_{};
    std::uint32_t max_notes_per_chunk_{};
    std::vector<std::string> kinds_;
    std::vector<PatternRun> patterns_;
};

}  // namespace pulseforge
