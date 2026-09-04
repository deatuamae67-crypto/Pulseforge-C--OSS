#include "pulseforge/autochart_review.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

namespace pulseforge {
namespace {

using Json = nlohmann::json;

constexpr std::uintmax_t maximum_review_index_bytes = 4ULL * 1024ULL * 1024ULL;
constexpr std::uintmax_t maximum_review_queue_bytes = 128ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t maximum_review_record_bytes = 64U * 1024U;
constexpr std::size_t maximum_review_difficulties = 64U;
constexpr std::size_t maximum_review_queue_entries = 100'000U;
constexpr std::size_t maximum_review_string_bytes = 4'096U;
constexpr std::size_t maximum_review_list_items = 64U;

void assign_error(std::string* const output, std::string value) {
    if (output != nullptr) {
        *output = std::move(value);
    }
}

[[nodiscard]] bool finite_unit(const double value) noexcept {
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

[[nodiscard]] bool safe_relative_path(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name()
        || path.has_root_directory()) {
        return false;
    }
    for (const auto& component : path) {
        if (component == std::filesystem::path{".."}) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<std::filesystem::path> resolve_under_root(
    const std::filesystem::path& root,
    const std::filesystem::path& relative,
    std::string& error
) {
    if (!safe_relative_path(relative)) {
        error = "AutoChart review metadata contains an unsafe relative path";
        return std::nullopt;
    }
    const auto lexical = (root / relative).lexically_normal();
    std::error_code root_error;
    const auto canonical_root = std::filesystem::weakly_canonical(root, root_error);
    if (root_error) {
        error = "AutoChart review root cannot be canonicalized";
        return std::nullopt;
    }
    std::error_code file_error;
    const auto canonical_file = std::filesystem::weakly_canonical(lexical, file_error);
    if (file_error) {
        // The target may be created later during generation. Lexical traversal
        // has already been rejected, so keep the bounded root-relative path.
        return lexical;
    }
    const auto relative_to_root = canonical_file.lexically_relative(canonical_root);
    if (relative_to_root.empty() && canonical_file != canonical_root) {
        error = "AutoChart review path escapes the mod root";
        return std::nullopt;
    }
    if (relative_to_root.is_absolute()) {
        error = "AutoChart review path escapes the mod root";
        return std::nullopt;
    }
    const auto first = relative_to_root.begin();
    if (first != relative_to_root.end()
        && *first == std::filesystem::path{".."}) {
        error = "AutoChart review path escapes the mod root";
        return std::nullopt;
    }
    return canonical_file;
}

[[nodiscard]] bool read_json_file_bounded(
    const std::filesystem::path& path,
    const std::uintmax_t maximum_bytes,
    Json& root,
    std::string& error
) {
    std::error_code size_error;
    const auto size = std::filesystem::file_size(path, size_error);
    if (size_error) {
        error = "cannot inspect AutoChart review JSON: " + size_error.message();
        return false;
    }
    if (size > maximum_bytes) {
        error = "AutoChart review JSON exceeds its bounded size limit";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "cannot open AutoChart review JSON";
        return false;
    }
    try {
        input >> root;
    } catch (const std::exception& exception) {
        error = std::string{"invalid AutoChart review JSON: "} + exception.what();
        return false;
    }
    if (!root.is_object()) {
        error = "AutoChart review JSON root must be an object";
        return false;
    }
    return true;
}

[[nodiscard]] std::string bounded_string(
    const Json& object,
    const char* const key,
    const std::string_view fallback = {}
) {
    const auto iterator = object.find(key);
    if (iterator == object.end() || !iterator->is_string()) {
        return std::string(fallback);
    }
    auto value = iterator->get<std::string>();
    if (value.size() > maximum_review_string_bytes) {
        value.resize(maximum_review_string_bytes);
    }
    return value;
}

template <typename T>
[[nodiscard]] T numeric_or(
    const Json& object,
    const char* const key,
    const T fallback
) {
    const auto iterator = object.find(key);
    if (iterator == object.end() || !iterator->is_number()) {
        return fallback;
    }
    return iterator->get<T>();
}

[[nodiscard]] bool boolean_or(
    const Json& object,
    const char* const key,
    const bool fallback
) {
    const auto iterator = object.find(key);
    if (iterator == object.end() || !iterator->is_boolean()) {
        return fallback;
    }
    return iterator->get<bool>();
}

[[nodiscard]] std::vector<std::string> string_list(
    const Json& object,
    const char* const key
) {
    std::vector<std::string> result;
    const auto iterator = object.find(key);
    if (iterator == object.end() || !iterator->is_array()) {
        return result;
    }
    result.reserve((std::min)(iterator->size(), maximum_review_list_items));
    for (const auto& item : *iterator) {
        if (result.size() >= maximum_review_list_items) {
            break;
        }
        if (!item.is_string()) {
            continue;
        }
        auto value = item.get<std::string>();
        if (value.size() > maximum_review_string_bytes) {
            value.resize(maximum_review_string_bytes);
        }
        result.push_back(std::move(value));
    }
    return result;
}

[[nodiscard]] AutoChartReviewBand review_band_from_string(
    const std::string_view value
) noexcept {
    if (value == "high") {
        return AutoChartReviewBand::high;
    }
    if (value == "medium") {
        return AutoChartReviewBand::medium;
    }
    return AutoChartReviewBand::low;
}

[[nodiscard]] bool approximately_equal(
    const double left,
    const double right,
    const double tolerance
) noexcept {
    return std::isfinite(left) && std::isfinite(right)
        && std::abs(left - right) <= tolerance;
}

}  // namespace

std::optional<AutoChartReviewReader> AutoChartReviewReader::open(
    const std::filesystem::path& index_path,
    std::string* const error_output
) {
    std::string error;
    Json root;
    if (!read_json_file_bounded(
            index_path,
            maximum_review_index_bytes,
            root,
            error
        )) {
        assign_error(error_output, std::move(error));
        return std::nullopt;
    }
    if (bounded_string(root, "schema")
        != "pulseforge-autochart-review-index-v1") {
        assign_error(error_output, "unsupported AutoChart review index schema");
        return std::nullopt;
    }
    if (bounded_string(root, "recordSchema")
        != "pulseforge-autochart-note-review-v1") {
        assign_error(error_output, "unsupported AutoChart note review schema");
        return std::nullopt;
    }
    if (bounded_string(root, "recordFormat") != "jsonl") {
        assign_error(error_output, "AutoChart review record format must be JSONL");
        return std::nullopt;
    }

    AutoChartReviewReader reader;
    reader.index_.index_path = index_path.lexically_normal();
    reader.index_.mod_root = index_path.parent_path().parent_path().lexically_normal();
    const double high = numeric_or(root, "highPriorityThreshold", 0.65);
    const double medium = numeric_or(root, "mediumPriorityThreshold", 0.40);
    if (!finite_unit(high) || !finite_unit(medium) || medium > high) {
        assign_error(error_output, "AutoChart review priority thresholds are invalid");
        return std::nullopt;
    }
    reader.index_.high_priority_threshold = high;
    reader.index_.medium_priority_threshold = medium;

    const auto queue_relative = std::filesystem::path(
        bounded_string(root, "queue", "review/queue.json")
    );
    const auto queue = resolve_under_root(
        reader.index_.mod_root,
        queue_relative,
        error
    );
    if (!queue.has_value()) {
        assign_error(error_output, std::move(error));
        return std::nullopt;
    }
    reader.index_.queue_path = *queue;

    const auto difficulties = root.find("difficulties");
    if (difficulties == root.end() || !difficulties->is_array()
        || difficulties->size() > maximum_review_difficulties) {
        assign_error(error_output, "AutoChart review index has an invalid difficulties array");
        return std::nullopt;
    }
    reader.index_.difficulties.reserve(difficulties->size());
    for (const auto& item : *difficulties) {
        if (!item.is_object()) {
            assign_error(error_output, "AutoChart review difficulty entry is not an object");
            return std::nullopt;
        }
        AutoChartReviewDifficultyInfo info;
        info.name = bounded_string(item, "name");
        if (info.name.empty()) {
            assign_error(error_output, "AutoChart review difficulty name is empty");
            return std::nullopt;
        }
        const auto chart = resolve_under_root(
            reader.index_.mod_root,
            std::filesystem::path(bounded_string(item, "chart")),
            error
        );
        const auto review = resolve_under_root(
            reader.index_.mod_root,
            std::filesystem::path(bounded_string(item, "reviewFile")),
            error
        );
        if (!chart.has_value() || !review.has_value()) {
            assign_error(error_output, std::move(error));
            return std::nullopt;
        }
        info.chart_path = *chart;
        info.review_file = *review;
        info.note_count = numeric_or<std::uint64_t>(item, "noteCount", 0U);
        info.review_records = numeric_or<std::uint64_t>(item, "reviewRecords", 0U);
        info.high_priority = numeric_or<std::uint64_t>(item, "highPriority", 0U);
        info.medium_priority = numeric_or<std::uint64_t>(item, "mediumPriority", 0U);
        info.mean_review_priority = numeric_or(item, "meanReviewPriority", 0.0);
        info.maximum_review_priority = numeric_or(item, "maximumReviewPriority", 0.0);
        if (std::any_of(
                reader.index_.difficulties.begin(),
                reader.index_.difficulties.end(),
                [&](const AutoChartReviewDifficultyInfo& existing) {
                    return existing.name == info.name;
                }
            )) {
            assign_error(error_output, "AutoChart review index contains duplicate difficulty names");
            return std::nullopt;
        }
        if (!finite_unit(info.mean_review_priority)
            || !finite_unit(info.maximum_review_priority)) {
            assign_error(error_output, "AutoChart review difficulty statistics are invalid");
            return std::nullopt;
        }
        reader.index_.difficulties.push_back(std::move(info));
    }
    assign_error(error_output, {});
    return reader;
}

const AutoChartReviewIndexInfo& AutoChartReviewReader::index() const noexcept {
    return index_;
}

std::vector<AutoChartReviewQueueEntry> AutoChartReviewReader::load_queue(
    const std::string_view difficulty,
    const std::size_t maximum_entries,
    std::string* const error_output
) const {
    std::vector<AutoChartReviewQueueEntry> result;
    if (maximum_entries == 0U) {
        assign_error(error_output, {});
        return result;
    }
    Json root;
    std::string error;
    if (!read_json_file_bounded(
            index_.queue_path,
            maximum_review_queue_bytes,
            root,
            error
        )) {
        assign_error(error_output, std::move(error));
        return result;
    }
    if (bounded_string(root, "schema")
        != "pulseforge-autochart-review-queue-v1") {
        assign_error(error_output, "unsupported AutoChart review queue schema");
        return result;
    }
    const auto entries = root.find("entries");
    if (entries == root.end() || !entries->is_array()
        || entries->size() > maximum_review_queue_entries) {
        assign_error(error_output, "AutoChart review queue has an invalid entries array");
        return result;
    }
    result.reserve((std::min)(maximum_entries, entries->size()));
    for (const auto& item : *entries) {
        if (result.size() >= maximum_entries) {
            break;
        }
        if (!item.is_object()) {
            continue;
        }
        AutoChartReviewQueueEntry entry;
        entry.difficulty = bounded_string(item, "difficulty");
        if (entry.difficulty.empty()) {
            continue;
        }
        if (!difficulty.empty() && entry.difficulty != difficulty) {
            continue;
        }
        const auto indexed_difficulty = std::find_if(
            index_.difficulties.begin(),
            index_.difficulties.end(),
            [&](const AutoChartReviewDifficultyInfo& value) {
                return value.name == entry.difficulty;
            }
        );
        if (indexed_difficulty == index_.difficulties.end()) {
            assign_error(error_output, "AutoChart review queue references an unknown difficulty");
            return {};
        }
        entry.note_index = numeric_or<std::uint64_t>(item, "noteIndex", 0U);
        entry.time_ms = numeric_or(item, "timeMs", 0.0);
        const auto raw_lane = numeric_or<std::uint64_t>(item, "lane", 0U);
        if (raw_lane > static_cast<std::uint64_t>(
                (std::numeric_limits<std::uint16_t>::max)())) {
            continue;
        }
        entry.lane = static_cast<std::uint16_t>(raw_lane);
        entry.review_priority = numeric_or(item, "reviewPriority", 0.0);
        entry.confidence = numeric_or(item, "confidence", 0.0);
        entry.ambiguity = numeric_or(item, "ambiguity", 0.0);
        entry.byte_offset = numeric_or<std::uint64_t>(item, "byteOffset", 0U);
        const auto raw_length = numeric_or<std::uint64_t>(item, "byteLength", 0U);
        if (!std::isfinite(entry.time_ms)
            || !finite_unit(entry.review_priority)
            || !finite_unit(entry.confidence)
            || !finite_unit(entry.ambiguity)
            || raw_length == 0U || raw_length > maximum_review_record_bytes) {
            continue;
        }
        entry.byte_length = static_cast<std::uint32_t>(raw_length);
        const auto review = resolve_under_root(
            index_.mod_root,
            std::filesystem::path(bounded_string(item, "reviewFile")),
            error
        );
        if (!review.has_value()) {
            assign_error(error_output, std::move(error));
            return {};
        }
        entry.review_file = *review;
        if (entry.review_file.lexically_normal()
            != indexed_difficulty->review_file.lexically_normal()) {
            assign_error(
                error_output,
                "AutoChart review queue references the wrong JSONL stream for its difficulty"
            );
            return {};
        }
        if (entry.byte_offset
            > (std::numeric_limits<std::uint64_t>::max)() - entry.byte_length) {
            continue;
        }
        result.push_back(std::move(entry));
    }
    assign_error(error_output, {});
    return result;
}

std::optional<AutoChartNoteReview> AutoChartReviewReader::read_record(
    const AutoChartReviewQueueEntry& entry,
    std::string* const error_output
) const {
    if (entry.byte_length == 0U || entry.byte_length > maximum_review_record_bytes) {
        assign_error(error_output, "AutoChart review record length is invalid");
        return std::nullopt;
    }
    const auto indexed_difficulty = std::find_if(
        index_.difficulties.begin(),
        index_.difficulties.end(),
        [&](const AutoChartReviewDifficultyInfo& value) {
            return value.name == entry.difficulty;
        }
    );
    if (indexed_difficulty == index_.difficulties.end()
        || entry.review_file.lexically_normal()
            != indexed_difficulty->review_file.lexically_normal()) {
        assign_error(error_output, "AutoChart review entry is not bound to the indexed JSONL stream");
        return std::nullopt;
    }
    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(entry.review_file, size_error);
    if (size_error) {
        assign_error(error_output, "cannot inspect AutoChart review stream");
        return std::nullopt;
    }
    const auto end = entry.byte_offset + static_cast<std::uint64_t>(entry.byte_length);
    if (end < entry.byte_offset || end > file_size) {
        assign_error(error_output, "AutoChart review queue points outside its JSONL stream");
        return std::nullopt;
    }
    std::ifstream input(entry.review_file, std::ios::binary);
    if (!input) {
        assign_error(error_output, "cannot open AutoChart review stream");
        return std::nullopt;
    }
    if (entry.byte_offset
        > static_cast<std::uint64_t>((std::numeric_limits<std::streamoff>::max)())) {
        assign_error(error_output, "AutoChart review stream offset is too large");
        return std::nullopt;
    }
    input.seekg(static_cast<std::streamoff>(entry.byte_offset), std::ios::beg);
    if (!input) {
        assign_error(error_output, "cannot seek AutoChart review stream");
        return std::nullopt;
    }
    std::string text(entry.byte_length, '\0');
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (input.gcount() != static_cast<std::streamsize>(text.size())) {
        assign_error(error_output, "AutoChart review stream ended before the queued record");
        return std::nullopt;
    }

    Json root;
    try {
        std::istringstream parser(text);
        parser >> root;
    } catch (const std::exception& exception) {
        assign_error(
            error_output,
            std::string{"invalid AutoChart JSONL review record: "} + exception.what()
        );
        return std::nullopt;
    }
    if (!root.is_object()
        || bounded_string(root, "schema")
            != "pulseforge-autochart-note-review-v1") {
        assign_error(error_output, "unsupported AutoChart JSONL review record schema");
        return std::nullopt;
    }

    AutoChartNoteReview review;
    review.note_index = numeric_or<std::uint64_t>(root, "noteIndex", 0U);
    review.time_ms = numeric_or(root, "timeMs", 0.0);
    review.duration_ms = numeric_or(root, "durationMs", 0.0);
    const auto raw_lane = numeric_or<std::uint64_t>(root, "lane", 0U);
    if (raw_lane > static_cast<std::uint64_t>(
            (std::numeric_limits<std::uint16_t>::max)())) {
        assign_error(error_output, "AutoChart review note lane is invalid");
        return std::nullopt;
    }
    review.lane = static_cast<std::uint16_t>(raw_lane);
    review.role = bounded_string(root, "role");
    review.source = bounded_string(root, "source");
    review.section = bounded_string(root, "section");
    review.confidence = numeric_or(root, "confidence", 0.0);
    review.support_score = numeric_or(root, "supportScore", 0.0);
    review.ambiguity = numeric_or(root, "ambiguity", 0.0);
    review.review_priority = numeric_or(root, "reviewPriority", 0.0);
    review.band = review_band_from_string(bounded_string(root, "reviewBand"));
    review.evidence_mask = numeric_or<std::uint32_t>(root, "evidenceMask", 0U);
    review.evidence_count = numeric_or<std::uint32_t>(root, "evidenceCount", 0U);
    review.evidence = string_list(root, "evidence");
    review.why = string_list(root, "why");
    review.review_concerns = string_list(root, "reviewConcerns");

    if (!std::isfinite(review.time_ms) || !std::isfinite(review.duration_ms)
        || review.duration_ms < 0.0 || !finite_unit(review.confidence)
        || !finite_unit(review.support_score) || !finite_unit(review.ambiguity)
        || !finite_unit(review.review_priority)) {
        assign_error(error_output, "AutoChart review record contains invalid numeric fields");
        return std::nullopt;
    }

    const auto timing = root.find("timing");
    if (timing != root.end() && timing->is_object()) {
        review.beat_alignment = numeric_or(*timing, "beatAlignment", 0.0);
        review.downbeat_alignment = numeric_or(*timing, "downbeatAlignment", 0.0);
        review.phrase_alignment = numeric_or(*timing, "phraseAlignment", 0.0);
        review.quantization_delta_ms = numeric_or(*timing, "quantizationDeltaMs", 0.0);
    }
    const auto selection = root.find("selection");
    if (selection != root.end() && selection->is_object()) {
        review.structural_priority = numeric_or(*selection, "structuralPriority", 0.0);
        review.minimum_confidence = numeric_or(*selection, "minimumConfidence", 0.0);
        review.priority_threshold = numeric_or(*selection, "priorityThreshold", 0.0);
        review.protected_accent = boolean_or(*selection, "protectedAccent", false);
    }
    const auto flags = root.find("flags");
    if (flags != root.end() && flags->is_object()) {
        review.polyphonic = boolean_or(*flags, "polyphonic", false);
        review.sustain = boolean_or(*flags, "sustain", false);
        review.chord_secondary = boolean_or(*flags, "chordSecondary", false);
    }

    // Queue/index corruption or a stale queue must not silently display a
    // different note's explanation after a direct seek.
    if (review.note_index != entry.note_index
        || review.lane != entry.lane
        || !approximately_equal(review.time_ms, entry.time_ms, 0.01)) {
        assign_error(error_output, "AutoChart review queue/record identity mismatch");
        return std::nullopt;
    }

    assign_error(error_output, {});
    return review;
}

std::string_view to_string(const AutoChartReviewBand band) noexcept {
    switch (band) {
    case AutoChartReviewBand::high: return "high";
    case AutoChartReviewBand::medium: return "medium";
    case AutoChartReviewBand::low:
    default: return "low";
    }
}

}  // namespace pulseforge
