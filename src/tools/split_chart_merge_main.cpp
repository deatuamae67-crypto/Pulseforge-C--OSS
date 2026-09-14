#include "pulseforge/split_chart_merger.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] std::string safe_name(std::string value) {
    for (char& c : value) {
        const auto byte = static_cast<unsigned char>(c);
        const bool allowed = std::isalnum(byte) != 0 || c == ' ' || c == '-'
            || c == '_' || c == '.';
        if (!allowed) c = '-';
    }
    while (!value.empty() && (value.front() == ' ' || value.front() == '.'
                              || value.front() == '-')) {
        value.erase(value.begin());
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '.'
                              || value.back() == '-')) {
        value.pop_back();
    }
    return value.empty() ? "merged-chart" : value;
}

void usage() {
    std::cout
        << "PulseForge Split Chart Merger\n\n"
        << "Usage:\n"
        << "  pulseforge-chart-merge <part.json|directory> [...] [options]\n\n"
        << "Options:\n"
        << "  -o, --output <file>       Explicit merged JSON path\n"
        << "  --name <name>             Name under mods/pulseforge-created/charts\n"
        << "  --mod-root <directory>    Created-content mod root\n"
        << "  --strategy <kway|external-sort|concat>\n"
        << "  --sort-memory-mib <MiB>   External-sort RAM budget (default 128)\n"
        << "  --temp-dir <directory>    Temporary run directory\n"
        << "  -h, --help                Show this help\n";
}

[[nodiscard]] bool parse_size(const std::string_view value, std::size_t& result) {
    if (value.empty()) return false;
    std::size_t parsed = 0U;
    for (const char c : value) {
        if (c < '0' || c > '9') return false;
        const auto digit = static_cast<std::size_t>(c - '0');
        if (parsed > (static_cast<std::size_t>(-1) - digit) / 10U) return false;
        parsed = parsed * 10U + digit;
    }
    result = parsed;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc <= 1) {
        usage();
        return EXIT_FAILURE;
    }

    std::vector<std::filesystem::path> raw_inputs;
    std::filesystem::path output;
    std::filesystem::path mod_root{"mods/pulseforge-created"};
    std::filesystem::path temp_root;
    std::string name{"merged-chart"};
    pulseforge::SplitChartMergeStrategy strategy{
        pulseforge::SplitChartMergeStrategy::k_way
    };
    std::size_t sort_memory_mib = 128U;

    const auto require_value = [&](int& index, const char* option) -> std::string {
        if (index + 1 >= argc) {
            std::cerr << option << " requires a value\n";
            std::exit(EXIT_FAILURE);
        }
        return argv[++index];
    };

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "-h" || argument == "--help") {
            usage();
            return EXIT_SUCCESS;
        }
        if (argument == "-o" || argument == "--output") {
            output = require_value(index, argv[index]);
        } else if (argument == "--name") {
            name = require_value(index, argv[index]);
        } else if (argument == "--mod-root") {
            mod_root = require_value(index, argv[index]);
        } else if (argument == "--temp-dir") {
            temp_root = require_value(index, argv[index]);
        } else if (argument == "--sort-memory-mib") {
            const auto value = require_value(index, argv[index]);
            if (!parse_size(value, sort_memory_mib) || sort_memory_mib == 0U) {
                std::cerr << "invalid --sort-memory-mib value\n";
                return EXIT_FAILURE;
            }
        } else if (argument == "--strategy") {
            const auto value = require_value(index, argv[index]);
            if (value == "kway") {
                strategy = pulseforge::SplitChartMergeStrategy::k_way;
            } else if (value == "external-sort") {
                strategy = pulseforge::SplitChartMergeStrategy::external_sort;
            } else if (value == "concat") {
                strategy = pulseforge::SplitChartMergeStrategy::concatenate;
            } else {
                std::cerr << "unknown merge strategy: " << value << '\n';
                return EXIT_FAILURE;
            }
        } else if (!argument.empty() && argument.front() == '-') {
            std::cerr << "unknown option: " << argument << '\n';
            return EXIT_FAILURE;
        } else {
            raw_inputs.emplace_back(argument);
        }
    }

    std::vector<std::filesystem::path> inputs;
    for (const auto& raw : raw_inputs) {
        std::error_code ec;
        if (std::filesystem::is_directory(raw, ec) && !ec) {
            auto discovered = pulseforge::discover_split_chart_parts(raw);
            inputs.insert(inputs.end(), discovered.begin(), discovered.end());
        } else {
            inputs.push_back(raw);
        }
    }
    std::sort(inputs.begin(), inputs.end());
    inputs.erase(std::unique(inputs.begin(), inputs.end()), inputs.end());
    if (inputs.size() < 2U) {
        std::cerr << "at least two JSON chart parts are required\n";
        return EXIT_FAILURE;
    }

    if (output.empty()) {
        output = mod_root / "charts" / (safe_name(name) + ".json");
    }

    pulseforge::SplitChartMergeProgress progress;
    pulseforge::SplitChartMergeOptions options;
    options.strategy = strategy;
    options.sort_memory_bytes = sort_memory_mib * 1024U * 1024U;
    options.temporary_root = temp_root;
    options.progress = &progress;

    std::cout << "Merging " << inputs.size() << " chart parts -> "
              << output.string() << '\n';
    const auto result = pulseforge::merge_split_charts(inputs, output, options);
    if (!result) {
        std::cerr << "PulseForge chart merge failed: " << result.error << '\n';
        return result.cancelled ? 130 : EXIT_FAILURE;
    }

    std::cout << "Merged notes: " << result.note_count << '\n'
              << "Sections: " << result.section_count << '\n'
              << "Source bytes: " << result.source_bytes << '\n'
              << "Output: " << result.output_path.string() << '\n';
    return EXIT_SUCCESS;
}
