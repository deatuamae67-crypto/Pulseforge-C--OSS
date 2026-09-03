#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulseforge {

struct ChartEditorChoiceCatalog {
    std::vector<std::string> characters;
    std::vector<std::string> stages;
    std::vector<std::string> note_types;
    std::vector<std::string> note_styles;
    std::vector<std::string> event_names;
    std::vector<std::string> scripts;
    std::vector<std::string> difficulties;
};

struct ChartEditorChoiceDiscoveryLimits {
    // Counts every visited directory entry, including directories and entries
    // that cannot be classified. This is the primary wall-clock bound for the
    // synchronous editor catalogue scan.
    std::size_t maximum_entries{200'000U};
    std::size_t maximum_files{100'000U};
    std::size_t maximum_depth{16U};
    std::size_t maximum_choices_per_category{16'384U};
};

// Scans content roots by the folder conventions shared by Psych Engine and
// related forks. The scan is bounded, does not follow directory symlinks and
// never opens scripts or assets. Results are sorted and de-duplicated using
// ASCII case-insensitive comparisons so the UI remains deterministic.
[[nodiscard]] ChartEditorChoiceCatalog discover_chart_editor_choices(
    std::span<const std::filesystem::path> roots,
    const ChartEditorChoiceDiscoveryLimits& limits = {}
);

void merge_chart_editor_choices(
    ChartEditorChoiceCatalog& destination,
    const ChartEditorChoiceCatalog& source
);

void normalize_chart_editor_choices(ChartEditorChoiceCatalog& choices);

// Renderer-independent state for the Chart Editor's searchable option list.
// Matching is ASCII case-insensitive and ranks prefix, substring and fuzzy
// subsequence matches in that order. Extending a query can only remove
// matches, which avoids a visually unstable result count while typing.
class SearchableOptionModel final {
public:
    explicit SearchableOptionModel(std::size_t visible_rows = 10U);

    void set_options(std::vector<std::string> options);
    void set_query(std::string query);
    void set_visible_rows(std::size_t visible_rows) noexcept;

    [[nodiscard]] std::string_view query() const noexcept;
    [[nodiscard]] std::size_t result_count() const noexcept;
    [[nodiscard]] std::size_t visible_rows() const noexcept;
    [[nodiscard]] std::size_t first_visible_row() const noexcept;
    [[nodiscard]] std::optional<std::size_t> selected_row() const noexcept;
    [[nodiscard]] std::optional<std::string_view> selected_value() const noexcept;
    [[nodiscard]] std::string_view result_at(std::size_t row) const;

    void move_selection(int rows) noexcept;
    void page_selection(int pages) noexcept;
    void select_home() noexcept;
    void select_end() noexcept;
    void scroll_view(int rows) noexcept;
    [[nodiscard]] bool select_row(std::size_t row) noexcept;
    [[nodiscard]] bool select_visible_row(std::size_t visible_row) noexcept;
    [[nodiscard]] bool select_value(std::string_view value);

private:
    struct Entry {
        std::string value;
        std::string folded;
        std::size_t insertion_order{};
    };

    struct Match {
        std::size_t entry_index{};
        std::size_t rank{};
        std::size_t distance{};
    };

    void rebuild_results();
    void keep_selection_visible() noexcept;

    std::vector<Entry> entries_;
    std::vector<Match> matches_;
    std::string query_;
    std::string folded_query_;
    std::size_t visible_rows_{10U};
    std::size_t first_visible_row_{};
    std::optional<std::size_t> selected_row_;
};

}  // namespace pulseforge
