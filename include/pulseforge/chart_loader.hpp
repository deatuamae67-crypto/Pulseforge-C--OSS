#pragma once

#include "pulseforge/chart.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace pulseforge {

struct ChartLoadOptions {
    std::string difficulty{"normal"};
    std::optional<std::filesystem::path> metadata_path;
    bool difficulty_explicit{false};
    bool strict{false};
};

struct ChartLoadResult {
    std::optional<Chart> chart;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return chart.has_value();
    }
};

class ChartLoader {
public:
    [[nodiscard]] static ChartLoadResult load(
        const std::filesystem::path& path,
        const ChartLoadOptions& options = {}
    );

    [[nodiscard]] static ChartLoadResult parse(
        std::string_view json_text,
        const std::filesystem::path& source_path = {},
        const ChartLoadOptions& options = {}
    );

    [[nodiscard]] static ChartFormat detect_format(std::string_view json_text);

    // Resolves the conventional Inst/Voices layouts shared by Psych, Slice,
    // JS Engine and DenpaEx. Exposed for the bounded streaming metadata path,
    // which intentionally does not materialize a complete Chart.
    static void resolve_conventional_audio(
        Chart& chart,
        const std::filesystem::path& source_path,
        std::string_view requested_song_id = {},
        bool discover_vocals = true
    );
};

}  // namespace pulseforge
