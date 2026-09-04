#pragma once

#include "pulseforge/chart.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace pulseforge::detail {

// Result of the bounded simdjson path used for large Psych-family charts.
// `recognized == false` asks ChartLoader to use its general-purpose parser.
struct FastPsychLoadResult {
    bool recognized{};
    std::optional<Chart> chart;
    std::string requested_song_id;
    bool denpa_schema{};
    bool discover_vocals{true};
    std::string error;
};

[[nodiscard]] FastPsychLoadResult load_fast_psych_chart(
    const std::filesystem::path& path,
    std::string_view difficulty
);

}  // namespace pulseforge::detail
