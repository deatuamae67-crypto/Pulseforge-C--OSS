#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace pulseforge {

enum class SplitChartMergeStrategy : std::uint8_t {
    k_way,
    external_sort,
    concatenate,
};

struct SplitChartMergeProgress final {
    std::atomic<std::uint64_t> source_bytes_total{0U};
    std::atomic<std::uint64_t> source_bytes_indexed{0U};
    std::atomic<std::uint64_t> notes_written{0U};
    std::atomic<std::uint64_t> sections_done{0U};
    std::atomic<std::uint64_t> sections_total{0U};
    std::atomic<bool> finished{false};
    std::atomic<bool> success{false};
    std::atomic<bool> cancelled{false};
    mutable std::mutex detail_mutex;
    std::string detail;
    std::string error;
};

struct SplitChartMergeOptions final {
    SplitChartMergeStrategy strategy{SplitChartMergeStrategy::k_way};
    std::size_t input_buffer_bytes{1U * 1024U * 1024U};
    std::size_t copy_buffer_bytes{8U * 1024U * 1024U};
    // External sorting retains raw note JSON. The byte budget is approximate
    // and counts note payloads plus a small per-record overhead.
    std::size_t sort_memory_bytes{128U * 1024U * 1024U};
    // Multi-pass run merging keeps the number of simultaneously open files
    // bounded even for charts large enough to create thousands of sort runs.
    std::size_t maximum_open_runs{32U};
    std::filesystem::path temporary_root;
    std::atomic<bool>* cancel{};
    SplitChartMergeProgress* progress{};
};

struct SplitChartMergeResult final {
    bool success{};
    bool cancelled{};
    bool used_external_sort{};
    std::filesystem::path output_path;
    std::filesystem::path template_path;
    std::uint64_t source_bytes{};
    std::uint64_t input_count{};
    std::uint64_t section_count{};
    std::uint64_t note_count{};
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return success; }
};

// Natural filename ordering intended for exports such as `part 1`, `part 2`,
// ..., `part 10`. Only regular `.json` files are returned.
[[nodiscard]] std::vector<std::filesystem::path> discover_split_chart_parts(
    const std::filesystem::path& directory
);

// Combines every explicit note from every input into a single Psych/FNF JSON
// document without materializing a complete source chart or complete note set.
// Metadata/section structure is copied from the first input; every input must
// contain the same number of sectionNotes arrays.
//
// k_way: O(input-count) note payload memory; each source section must already
// be sorted by strumTime.
// external_sort: accepts arbitrary order by creating bounded temporary runs.
// concatenate: preserves natural input order and does not sort notes.
[[nodiscard]] SplitChartMergeResult merge_split_charts(
    std::span<const std::filesystem::path> inputs,
    const std::filesystem::path& output,
    const SplitChartMergeOptions& options = {}
);

}  // namespace pulseforge
