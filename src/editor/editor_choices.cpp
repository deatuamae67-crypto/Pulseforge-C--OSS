#include "pulseforge/editor_choices.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace pulseforge {
namespace {

[[nodiscard]] char lower_ascii_character(const unsigned char character) noexcept {
    return character >= static_cast<unsigned char>('A')
            && character <= static_cast<unsigned char>('Z')
        ? static_cast<char>(character - static_cast<unsigned char>('A')
            + static_cast<unsigned char>('a'))
        : static_cast<char>(character);
}

[[nodiscard]] std::string lower_ascii(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        result.push_back(lower_ascii_character(character));
    }
    return result;
}

[[nodiscard]] std::string compact_ascii(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        const char lowered = lower_ascii_character(character);
        if ((lowered >= 'a' && lowered <= 'z')
            || (lowered >= '0' && lowered <= '9')) {
            result.push_back(lowered);
        }
    }
    return result;
}

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return std::string(encoded.begin(), encoded.end());
}

void add_bounded(
    std::vector<std::string>& values,
    std::string value,
    const std::size_t maximum
) {
    if (!value.empty() && values.size() < maximum) {
        values.push_back(std::move(value));
    }
}

[[nodiscard]] bool has_component(
    const std::filesystem::path& path,
    const std::span<const std::string_view> names
) {
    for (const auto& component : path) {
        const auto compact = compact_ascii(path_utf8(component));
        if (std::ranges::any_of(names, [&](const std::string_view name) {
                return compact == name;
            })) {
            return true;
        }
    }
    return false;
}

void normalize_values(std::vector<std::string>& values) {
    values.erase(
        std::remove_if(values.begin(), values.end(), [](const std::string& value) {
            return value.empty();
        }),
        values.end()
    );
    std::stable_sort(values.begin(), values.end(), [](const auto& left, const auto& right) {
        const auto folded_left = lower_ascii(left);
        const auto folded_right = lower_ascii(right);
        if (folded_left != folded_right) {
            return folded_left < folded_right;
        }
        return left < right;
    });
    values.erase(
        std::unique(values.begin(), values.end(), [](const auto& left, const auto& right) {
            return lower_ascii(left) == lower_ascii(right);
        }),
        values.end()
    );
}

void merge_values(
    std::vector<std::string>& destination,
    const std::vector<std::string>& source
) {
    destination.insert(destination.end(), source.begin(), source.end());
}

struct MatchScore {
    std::size_t rank{};
    std::size_t distance{};
};

[[nodiscard]] std::optional<MatchScore> score_match(
    const std::string_view candidate,
    const std::string_view query
) noexcept {
    if (query.empty()) {
        return MatchScore{0U, 0U};
    }
    if (candidate.starts_with(query)) {
        return MatchScore{0U, candidate.size() - query.size()};
    }
    const auto substring = candidate.find(query);
    if (substring != std::string_view::npos) {
        return MatchScore{1U, substring};
    }

    std::size_t query_index = 0U;
    std::size_t first = 0U;
    std::size_t last = 0U;
    bool found_first = false;
    for (std::size_t index = 0U;
         index < candidate.size() && query_index < query.size();
         ++index) {
        if (candidate[index] != query[query_index]) {
            continue;
        }
        if (!found_first) {
            first = index;
            found_first = true;
        }
        last = index;
        ++query_index;
    }
    if (query_index != query.size()) {
        return std::nullopt;
    }
    const auto span = last >= first ? last - first + 1U : 0U;
    return MatchScore{2U, first + span - query.size()};
}

[[nodiscard]] std::size_t saturating_offset(
    const std::size_t value,
    const int delta,
    const std::size_t maximum
) noexcept {
    if (maximum == 0U) {
        return 0U;
    }
    if (delta < 0) {
        const auto magnitude = static_cast<std::uint64_t>(
            -static_cast<std::int64_t>(delta)
        );
        return magnitude >= value ? 0U : value - static_cast<std::size_t>(magnitude);
    }
    const auto amount = static_cast<std::uint64_t>(delta);
    const auto remaining = maximum - value;
    return amount >= remaining ? maximum : value + static_cast<std::size_t>(amount);
}

}  // namespace

ChartEditorChoiceCatalog discover_chart_editor_choices(
    const std::span<const std::filesystem::path> roots,
    const ChartEditorChoiceDiscoveryLimits& limits
) {
    ChartEditorChoiceCatalog choices;
    if (limits.maximum_entries == 0U || limits.maximum_files == 0U
        || limits.maximum_choices_per_category == 0U) {
        return choices;
    }

    constexpr std::array<std::string_view, 2U> character_directories{
        "characters", "character",
    };
    constexpr std::array<std::string_view, 2U> stage_directories{
        "stages", "stage",
    };
    constexpr std::array<std::string_view, 3U> note_type_directories{
        "customnotetypes", "notetypes", "notetype",
    };
    constexpr std::array<std::string_view, 2U> event_directories{
        "customevents", "events",
    };
    constexpr std::array<std::string_view, 4U> note_style_directories{
        "notestyles", "notestyle", "noteskins", "noteskin",
    };
    constexpr std::array<std::string_view, 2U> difficulty_directories{
        "difficulties", "difficulty",
    };

    std::size_t entries_seen = 0U;
    std::size_t files_seen = 0U;
    for (const auto& root : roots) {
        if (entries_seen >= limits.maximum_entries
            || files_seen >= limits.maximum_files) {
            break;
        }
        std::error_code error;
        if (!std::filesystem::is_directory(root, error) || error) {
            continue;
        }
        std::filesystem::recursive_directory_iterator iterator(
            root,
            std::filesystem::directory_options::skip_permission_denied,
            error
        );
        const std::filesystem::recursive_directory_iterator end;
        while (!error && iterator != end
            && entries_seen < limits.maximum_entries
            && files_seen < limits.maximum_files) {
            const auto entry = *iterator;
            ++entries_seen;
            const auto depth = static_cast<std::size_t>(iterator.depth());
            std::error_code entry_error;
            if (entry.is_symlink(entry_error)) {
                if (!entry_error && entry.is_directory(entry_error)) {
                    iterator.disable_recursion_pending();
                }
                iterator.increment(error);
                continue;
            }
            if (depth >= limits.maximum_depth && entry.is_directory(entry_error)) {
                iterator.disable_recursion_pending();
            }
            if (entry_error || !entry.is_regular_file(entry_error) || entry_error) {
                iterator.increment(error);
                continue;
            }
            ++files_seen;

            const auto relative = entry.path().lexically_relative(root);
            const auto extension = lower_ascii(path_utf8(entry.path().extension()));
            const auto stem = path_utf8(entry.path().stem());
            const bool is_json = extension == ".json";
            const bool is_script = extension == ".lua" || extension == ".hx"
                || extension == ".hscript" || extension == ".js";
            const bool is_note_type_definition = is_script || is_json
                || extension == ".txt";
            const auto compact_stem = compact_ascii(stem);
            const bool conventional_note_atlas =
                (extension == ".png" || extension == ".xml")
                && (compact_stem.starts_with("noteassets")
                    || compact_stem.starts_with("noteskin"));

            if (is_json && has_component(relative.parent_path(), character_directories)) {
                add_bounded(
                    choices.characters,
                    stem,
                    limits.maximum_choices_per_category
                );
            }
            if ((is_json || extension == ".lua")
                && has_component(relative.parent_path(), stage_directories)) {
                add_bounded(
                    choices.stages,
                    stem,
                    limits.maximum_choices_per_category
                );
            }
            if (is_note_type_definition
                && has_component(relative.parent_path(), note_type_directories)) {
                add_bounded(
                    choices.note_types,
                    stem,
                    limits.maximum_choices_per_category
                );
            }
            if ((is_script || is_json)
                && has_component(relative.parent_path(), event_directories)) {
                add_bounded(
                    choices.event_names,
                    stem,
                    limits.maximum_choices_per_category
                );
            }
            if (((is_json || extension == ".png" || extension == ".xml")
                    && has_component(
                        relative.parent_path(),
                        note_style_directories
                    ))
                || conventional_note_atlas) {
                add_bounded(
                    choices.note_styles,
                    stem,
                    limits.maximum_choices_per_category
                );
            }
            if (is_json
                && has_component(relative.parent_path(), difficulty_directories)) {
                add_bounded(
                    choices.difficulties,
                    stem,
                    limits.maximum_choices_per_category
                );
            }
            if (is_script) {
                add_bounded(
                    choices.scripts,
                    path_utf8(relative),
                    limits.maximum_choices_per_category
                );
            }
            iterator.increment(error);
        }
    }
    normalize_chart_editor_choices(choices);
    return choices;
}

void merge_chart_editor_choices(
    ChartEditorChoiceCatalog& destination,
    const ChartEditorChoiceCatalog& source
) {
    merge_values(destination.characters, source.characters);
    merge_values(destination.stages, source.stages);
    merge_values(destination.note_types, source.note_types);
    merge_values(destination.note_styles, source.note_styles);
    merge_values(destination.event_names, source.event_names);
    merge_values(destination.scripts, source.scripts);
    merge_values(destination.difficulties, source.difficulties);
    normalize_chart_editor_choices(destination);
}

void normalize_chart_editor_choices(ChartEditorChoiceCatalog& choices) {
    normalize_values(choices.characters);
    normalize_values(choices.stages);
    normalize_values(choices.note_types);
    normalize_values(choices.note_styles);
    normalize_values(choices.event_names);
    normalize_values(choices.scripts);
    normalize_values(choices.difficulties);
}

SearchableOptionModel::SearchableOptionModel(const std::size_t visible_rows)
    : visible_rows_(std::max<std::size_t>(visible_rows, 1U)) {}

void SearchableOptionModel::set_options(std::vector<std::string> options) {
    entries_.clear();
    entries_.reserve(options.size());
    std::unordered_set<std::string> seen;
    seen.reserve(options.size());
    std::size_t insertion_order = 0U;
    for (auto& option : options) {
        if (option.empty()) {
            continue;
        }
        auto folded = lower_ascii(option);
        if (!seen.emplace(folded).second) {
            continue;
        }
        entries_.push_back({
            std::move(option),
            std::move(folded),
            insertion_order,
        });
        ++insertion_order;
    }
    rebuild_results();
}

void SearchableOptionModel::set_query(std::string query) {
    query_ = std::move(query);
    folded_query_ = lower_ascii(query_);
    rebuild_results();
}

void SearchableOptionModel::set_visible_rows(const std::size_t visible_rows) noexcept {
    visible_rows_ = std::max<std::size_t>(visible_rows, 1U);
    keep_selection_visible();
}

std::string_view SearchableOptionModel::query() const noexcept {
    return query_;
}

std::size_t SearchableOptionModel::result_count() const noexcept {
    return matches_.size();
}

std::size_t SearchableOptionModel::visible_rows() const noexcept {
    return visible_rows_;
}

std::size_t SearchableOptionModel::first_visible_row() const noexcept {
    return first_visible_row_;
}

std::optional<std::size_t> SearchableOptionModel::selected_row() const noexcept {
    return selected_row_;
}

std::optional<std::string_view> SearchableOptionModel::selected_value() const noexcept {
    if (!selected_row_.has_value() || *selected_row_ >= matches_.size()) {
        return std::nullopt;
    }
    return entries_[matches_[*selected_row_].entry_index].value;
}

std::string_view SearchableOptionModel::result_at(const std::size_t row) const {
    return entries_.at(matches_.at(row).entry_index).value;
}

void SearchableOptionModel::move_selection(const int rows) noexcept {
    if (matches_.empty()) {
        selected_row_.reset();
        first_visible_row_ = 0U;
        return;
    }
    const auto current = selected_row_.value_or(0U);
    selected_row_ = saturating_offset(current, rows, matches_.size() - 1U);
    keep_selection_visible();
}

void SearchableOptionModel::page_selection(const int pages) noexcept {
    if (pages == 0) {
        return;
    }
    const auto page_count = static_cast<std::uint64_t>(
        pages < 0 ? -static_cast<std::int64_t>(pages)
                  : static_cast<std::int64_t>(pages)
    );
    constexpr auto maximum_step = static_cast<std::uint64_t>(
        std::numeric_limits<int>::max()
    );
    std::uint64_t step = maximum_step;
    if (visible_rows_ <= maximum_step
        && page_count <= maximum_step
            / static_cast<std::uint64_t>(visible_rows_)) {
        step = page_count * static_cast<std::uint64_t>(visible_rows_);
    }
    move_selection(pages < 0 ? -static_cast<int>(step) : static_cast<int>(step));
}

void SearchableOptionModel::select_home() noexcept {
    if (!matches_.empty()) {
        selected_row_ = 0U;
        first_visible_row_ = 0U;
    }
}

void SearchableOptionModel::select_end() noexcept {
    if (!matches_.empty()) {
        selected_row_ = matches_.size() - 1U;
        keep_selection_visible();
    }
}

void SearchableOptionModel::scroll_view(const int rows) noexcept {
    if (matches_.empty()) {
        selected_row_.reset();
        first_visible_row_ = 0U;
        return;
    }
    const auto maximum_first = matches_.size() > visible_rows_
        ? matches_.size() - visible_rows_
        : 0U;
    first_visible_row_ = saturating_offset(first_visible_row_, rows, maximum_first);
    const auto remaining = matches_.size() - 1U - first_visible_row_;
    const auto last_visible = first_visible_row_
        + std::min(remaining, visible_rows_ - 1U);
    if (!selected_row_.has_value() || *selected_row_ < first_visible_row_) {
        selected_row_ = first_visible_row_;
    } else if (*selected_row_ > last_visible) {
        selected_row_ = last_visible;
    }
}

bool SearchableOptionModel::select_row(const std::size_t row) noexcept {
    if (row >= matches_.size()) {
        return false;
    }
    selected_row_ = row;
    keep_selection_visible();
    return true;
}

bool SearchableOptionModel::select_visible_row(
    const std::size_t visible_row
) noexcept {
    if (visible_row >= visible_rows_
        || visible_row > std::numeric_limits<std::size_t>::max()
            - first_visible_row_) {
        return false;
    }
    return select_row(first_visible_row_ + visible_row);
}

bool SearchableOptionModel::select_value(const std::string_view value) {
    const auto folded = lower_ascii(value);
    for (std::size_t row = 0U; row < matches_.size(); ++row) {
        if (entries_[matches_[row].entry_index].folded == folded) {
            selected_row_ = row;
            keep_selection_visible();
            return true;
        }
    }
    return false;
}

void SearchableOptionModel::rebuild_results() {
    matches_.clear();
    matches_.reserve(entries_.size());
    for (std::size_t index = 0U; index < entries_.size(); ++index) {
        const auto score = score_match(entries_[index].folded, folded_query_);
        if (!score.has_value()) {
            continue;
        }
        matches_.push_back({index, score->rank, score->distance});
    }
    std::stable_sort(matches_.begin(), matches_.end(), [&](const Match& left, const Match& right) {
        if (left.rank != right.rank) {
            return left.rank < right.rank;
        }
        if (left.distance != right.distance) {
            return left.distance < right.distance;
        }
        const auto& left_entry = entries_[left.entry_index];
        const auto& right_entry = entries_[right.entry_index];
        if (left_entry.folded != right_entry.folded) {
            return left_entry.folded < right_entry.folded;
        }
        return left_entry.insertion_order < right_entry.insertion_order;
    });
    first_visible_row_ = 0U;
    selected_row_ = matches_.empty()
        ? std::nullopt
        : std::optional<std::size_t>{0U};
}

void SearchableOptionModel::keep_selection_visible() noexcept {
    if (matches_.empty() || !selected_row_.has_value()) {
        selected_row_.reset();
        first_visible_row_ = 0U;
        return;
    }
    *selected_row_ = std::min(*selected_row_, matches_.size() - 1U);
    if (*selected_row_ < first_visible_row_) {
        first_visible_row_ = *selected_row_;
    } else if (*selected_row_ - first_visible_row_ >= visible_rows_) {
        first_visible_row_ = *selected_row_ - visible_rows_ + 1U;
    }
    const auto maximum_first = matches_.size() > visible_rows_
        ? matches_.size() - visible_rows_
        : 0U;
    first_visible_row_ = std::min(first_visible_row_, maximum_first);
}

}  // namespace pulseforge
