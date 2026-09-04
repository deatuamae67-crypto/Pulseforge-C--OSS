#include "pulseforge/editor_choices.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void write_file(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    require(!error, "fixture directory is created");
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(stream), "fixture file opens");
    stream << "fixture";
    require(static_cast<bool>(stream), "fixture file is written");
}

void test_filter_and_ranking() {
    pulseforge::SearchableOptionModel model(3U);
    model.set_options({
        "Monster",
        "bf",
        "The Monster",
        "monster",
        "Pico Speaker",
        "Dad",
    });
    require(model.result_count() == 5U, "case-insensitive duplicates are removed");

    model.set_query("mo");
    require(model.result_count() == 2U, "case-insensitive query filters choices");
    require(model.result_at(0U) == "Monster", "prefix match ranks first");
    require(model.result_at(1U) == "The Monster", "substring match follows prefix");

    const auto broad_count = model.result_count();
    model.set_query("mon");
    require(
        model.result_count() <= broad_count,
        "extending a query cannot increase the match count"
    );
    require(
        model.selected_value().has_value()
            && *model.selected_value() == "Monster",
        "first filtered row has an explicit selection"
    );

    model.set_query("PS");
    require(model.result_count() == 1U, "simple fuzzy match is ASCII case-insensitive");
    require(model.result_at(0U) == "Pico Speaker", "fuzzy subsequence is deterministic");

    model.set_query("not-present");
    require(model.result_count() == 0U, "missing query has no rows");
    require(!model.selected_row().has_value(), "empty results clear selection");
}

void test_navigation_and_viewport() {
    pulseforge::SearchableOptionModel model(3U);
    model.set_options({"0", "1", "2", "3", "4", "5", "6", "7"});
    model.select_end();
    require(model.selected_row() == 7U, "End selects the final result");
    require(model.first_visible_row() == 5U, "End scrolls the viewport");

    model.select_home();
    model.page_selection(1);
    require(model.selected_row() == 3U, "PageDown advances one visible page");
    require(model.first_visible_row() == 1U, "PageDown keeps selection visible");
    model.page_selection(-1);
    require(model.selected_row() == 0U, "PageUp clamps at the first row");

    model.scroll_view(4);
    require(model.first_visible_row() == 4U, "mouse-wheel style scrolling moves viewport");
    require(model.selected_row() == 4U, "wheel scrolling keeps highlight visible");
    require(model.select_visible_row(2U), "a visible mouse row can be selected");
    require(model.selected_row() == 6U, "visible row maps through scroll offset");
    require(!model.select_visible_row(3U), "row below clipped viewport is rejected");

    model.move_selection(100);
    require(model.selected_row() == 7U, "down navigation saturates at end");
    model.move_selection(-100);
    require(model.selected_row() == 0U, "up navigation saturates at home");
    require(model.select_value("5"), "current value can seed selection");
    require(model.selected_row() == 5U, "value seeding is deterministic");

    model.set_options({"Custom ♥ Skin", "normal"});
    model.set_query({});
    require(
        model.select_value("Custom ♥ Skin")
            && model.selected_value() == std::optional<std::string_view>{
                "Custom ♥ Skin"
            },
        "unknown UTF-8 IDs remain byte-exact instead of being canonicalized"
    );
    model.set_query("custom");
    require(
        model.result_count() == 1U
            && model.selected_row() == 0U
            && model.first_visible_row() == 0U,
        "the first filtered row remains visible and selected"
    );
}

void test_choice_discovery() {
    const auto stamp = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("pulseforge-choice-test-" + std::to_string(stamp));
    try {
        write_file(root / "characters" / "BF.json");
        write_file(root / "characters" / "bf.JSON");
        write_file(root / "stages" / "philly.json");
        write_file(root / "stages" / "lua-stage.lua");
        write_file(root / "custom_notetypes" / "Hurt Note.lua");
        write_file(root / "custom_notetypes" / "Txt Note.txt");
        write_file(root / "custom_events" / "Camera Flash.lua");
        write_file(root / "notestyles" / "pixel.json");
        write_file(root / "notestyles" / "classic.png");
        write_file(root / "images" / "noteskins" / "NOTE_assets-future.xml");
        write_file(root / "images" / "NOTE_assets-custom.png");
        write_file(root / "difficulties" / "erect.json");
        write_file(root / "data" / "song" / "script.lua");
        write_file(root / "unrelated" / "not-a-character.json");

        const std::vector roots{root};
        const auto choices = pulseforge::discover_chart_editor_choices(roots);
        require(choices.characters.size() == 1U, "character choices de-duplicate");
        require(choices.characters.front() == "BF", "discovery ordering is stable");
        require(
            choices.stages == std::vector<std::string>{"lua-stage", "philly"},
            "JSON and static Lua stages are discovered"
        );
        require(
            choices.note_types
                == std::vector<std::string>{"Hurt Note", "Txt Note"},
            "Lua and H-Slice text note types are discovered"
        );
        require(
            choices.event_names == std::vector<std::string>{"Camera Flash"},
            "custom event discovered"
        );
        require(
            choices.note_styles == std::vector<std::string>{
                "classic", "NOTE_assets-custom", "NOTE_assets-future", "pixel",
            },
            "descriptor and Psych/JS image note styles are discovered"
        );
        require(choices.difficulties == std::vector<std::string>{"erect"}, "difficulty discovered");
        require(
            choices.scripts.size() == 4U,
            "all supported Lua scripts are listed without opening them"
        );

        pulseforge::ChartEditorChoiceDiscoveryLimits shallow;
        shallow.maximum_depth = 0U;
        const auto bounded = pulseforge::discover_chart_editor_choices(roots, shallow);
        require(bounded.scripts.empty(), "depth limit bounds recursive discovery");

        const auto directory_only_root = root / "directory-budget";
        write_file(
            directory_only_root / "empty-0" / "empty-1" / "characters" / "Late.json"
        );
        pulseforge::ChartEditorChoiceDiscoveryLimits entry_bounded;
        entry_bounded.maximum_entries = 2U;
        const std::vector directory_roots{directory_only_root};
        const auto entry_limited = pulseforge::discover_chart_editor_choices(
            directory_roots,
            entry_bounded
        );
        require(
            entry_limited.characters.empty(),
            "directory entries consume the global synchronous scan budget"
        );
    } catch (...) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(root, cleanup_error);
        throw;
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

}  // namespace

int main() {
    try {
        test_filter_and_ranking();
        test_navigation_and_viewport();
        test_choice_discovery();
        std::cout << "Editor choice tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "Editor choice tests failed: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
