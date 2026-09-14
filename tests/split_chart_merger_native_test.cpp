#include "pulseforge/split_chart_merger.hpp"

#include <nlohmann/json.hpp>

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
    if (!condition) throw std::runtime_error(std::string(message));
}

void write_chart(
    const std::filesystem::path& path,
    const std::vector<double>& first,
    const std::vector<double>& second = {}
) {
    nlohmann::json sections = nlohmann::json::array();
    for (const auto* times : {&first, &second}) {
        nlohmann::json notes = nlohmann::json::array();
        std::uint32_t lane = 0U;
        for (const auto time : *times) {
            notes.push_back(nlohmann::json::array({time, lane++ % 4U, 0.0}));
        }
        sections.push_back({
            {"lengthInSteps", 16},
            {"mustHitSection", sections.size() != 0U},
            {"sectionNotes", std::move(notes)},
        });
    }
    nlohmann::json root{
        {"song", {
            {"song", "Synthetic Split"},
            {"bpm", 120.0},
            {"notes", std::move(sections)},
        }},
    };
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "fixture opens");
    output << root.dump();
    require(static_cast<bool>(output), "fixture writes");
}

std::vector<std::vector<double>> read_times(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "merged chart opens");
    const auto root = nlohmann::json::parse(input);
    std::vector<std::vector<double>> result;
    for (const auto& section : root.at("song").at("notes")) {
        std::vector<double> values;
        for (const auto& note : section.at("sectionNotes")) {
            values.push_back(note.at(0).get<double>());
        }
        result.push_back(std::move(values));
    }
    return result;
}

}  // namespace

int main() {
    try {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto root = std::filesystem::temp_directory_path()
            / ("pulseforge-native-merge-test-" + std::to_string(stamp));
        std::filesystem::create_directories(root);
        try {
            const auto part1 = root / "FLP 6 part 1 chart.json";
            const auto part2 = root / "FLP 6 part 2 chart.json";
            const auto part10 = root / "FLP 6 part 10 chart.json";
            write_chart(part1, {100.0, 400.0}, {50.0});
            write_chart(part2, {200.0}, {150.0});
            write_chart(part10, {300.0}, {250.0});

            const auto discovered = pulseforge::discover_split_chart_parts(root);
            require(discovered.size() == 3U, "three JSON parts discovered");
            require(discovered[0].filename() == part1.filename(), "part 1 first");
            require(discovered[1].filename() == part2.filename(), "part 2 before part 10");
            require(discovered[2].filename() == part10.filename(), "part 10 last");

            const auto merged = root / "merged.json";
            pulseforge::SplitChartMergeOptions options;
            options.strategy = pulseforge::SplitChartMergeStrategy::k_way;
            const auto result = pulseforge::merge_split_charts(discovered, merged, options);
            require(result.success, result.error);
            require(result.note_count == 7U, "all split notes retained");
            require(result.section_count == 2U, "section structure retained");
            const auto merged_times = read_times(merged);
            require(
                merged_times == std::vector<std::vector<double>>{
                    {100.0, 200.0, 300.0, 400.0},
                    {50.0, 150.0, 250.0},
                },
                "k-way output is globally chronological per section"
            );

            const auto unsorted = root / "unsorted.json";
            const auto other = root / "other.json";
            write_chart(unsorted, {500.0, 25.0});
            write_chart(other, {75.0});
            const std::array unsorted_inputs{unsorted, other};
            const auto rejected = pulseforge::merge_split_charts(
                unsorted_inputs,
                root / "rejected.json",
                options
            );
            require(!rejected.success, "k-way rejects internally unsorted inputs");

            options.strategy = pulseforge::SplitChartMergeStrategy::external_sort;
            options.sort_memory_bytes = 1U * 1024U * 1024U;
            const auto external_path = root / "external.json";
            const auto external = pulseforge::merge_split_charts(
                unsorted_inputs,
                external_path,
                options
            );
            require(external.success, external.error);
            require(
                read_times(external_path).front()
                    == std::vector<double>{25.0, 75.0, 500.0},
                "external sort accepts arbitrary source order"
            );

            std::atomic<bool> cancel{true};
            options.strategy = pulseforge::SplitChartMergeStrategy::k_way;
            options.cancel = &cancel;
            const auto cancelled = pulseforge::merge_split_charts(
                discovered,
                root / "cancelled.json",
                options
            );
            require(cancelled.cancelled, "pre-requested cancellation is honored");
            require(!std::filesystem::exists(root / "cancelled.json"), "cancel never publishes partial output");
        } catch (...) {
            std::error_code cleanup_error;
            std::filesystem::remove_all(root, cleanup_error);
            throw;
        }
        std::error_code cleanup_error;
        std::filesystem::remove_all(root, cleanup_error);
        std::cout << "Native split chart merger tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "Native split chart merger tests failed: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
