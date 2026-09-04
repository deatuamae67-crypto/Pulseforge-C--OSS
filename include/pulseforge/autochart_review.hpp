#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulseforge {

enum class AutoChartReviewBand : std::uint8_t {
    low,
    medium,
    high,
};

struct AutoChartReviewDifficultyInfo final {
    std::string name;
    std::filesystem::path chart_path;
    std::filesystem::path review_file;
    std::uint64_t note_count{};
    std::uint64_t review_records{};
    std::uint64_t high_priority{};
    std::uint64_t medium_priority{};
    double mean_review_priority{};
    double maximum_review_priority{};
};

struct AutoChartReviewIndexInfo final {
    std::filesystem::path index_path;
    std::filesystem::path mod_root;
    std::filesystem::path queue_path;
    double high_priority_threshold{0.65};
    double medium_priority_threshold{0.40};
    std::vector<AutoChartReviewDifficultyInfo> difficulties;
};

struct AutoChartReviewQueueEntry final {
    std::string difficulty;
    std::uint64_t note_index{};
    double time_ms{};
    std::uint16_t lane{};
    double review_priority{};
    double confidence{};
    double ambiguity{};
    std::filesystem::path review_file;
    std::uint64_t byte_offset{};
    std::uint32_t byte_length{};
};

struct AutoChartNoteReview final {
    std::uint64_t note_index{};
    double time_ms{};
    double duration_ms{};
    std::uint16_t lane{};
    std::string role;
    std::string source;
    std::string section;
    double confidence{};
    double support_score{};
    double ambiguity{};
    double review_priority{};
    AutoChartReviewBand band{AutoChartReviewBand::low};
    std::uint32_t evidence_mask{};
    std::uint32_t evidence_count{};
    std::vector<std::string> evidence;
    std::vector<std::string> why;
    std::vector<std::string> review_concerns;
    double beat_alignment{};
    double downbeat_alignment{};
    double phrase_alignment{};
    double quantization_delta_ms{};
    double structural_priority{};
    double minimum_confidence{};
    double priority_threshold{};
    bool protected_accent{};
    bool polyphonic{};
    bool sustain{};
    bool chord_secondary{};
};

// Bounded reader for AutoChart's review/index.json, review/queue.json and the
// per-difficulty JSONL streams. Queue records carry byte offsets so detailed
// note provenance can be fetched without materializing a chart-sized review
// document in memory.
class AutoChartReviewReader final {
public:
    AutoChartReviewReader() = default;

    [[nodiscard]] static std::optional<AutoChartReviewReader> open(
        const std::filesystem::path& index_path,
        std::string* error = nullptr
    );

    [[nodiscard]] const AutoChartReviewIndexInfo& index() const noexcept;

    [[nodiscard]] std::vector<AutoChartReviewQueueEntry> load_queue(
        std::string_view difficulty = {},
        std::size_t maximum_entries = 5'000U,
        std::string* error = nullptr
    ) const;

    [[nodiscard]] std::optional<AutoChartNoteReview> read_record(
        const AutoChartReviewQueueEntry& entry,
        std::string* error = nullptr
    ) const;

private:
    AutoChartReviewIndexInfo index_;
};

[[nodiscard]] std::string_view to_string(AutoChartReviewBand band) noexcept;

}  // namespace pulseforge
