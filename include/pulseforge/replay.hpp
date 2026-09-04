#pragma once

#include "pulseforge/gameplay.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pulseforge {

struct Replay {
    std::uint32_t format_version{1};
    std::string engine_version;
    std::string chart_hash;
    std::string difficulty;
    std::uint64_t random_seed{};
    GameplaySettings settings;
    std::vector<InputRecord> inputs;
};

struct ReplayLoadResult {
    std::optional<Replay> replay;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return replay.has_value();
    }
};

[[nodiscard]] std::string chart_fingerprint(const Chart& chart);
[[nodiscard]] Replay make_replay(
    const Chart& chart,
    const GameplaySession& session,
    std::string engine_version
);
[[nodiscard]] bool save_replay(
    const std::filesystem::path& path,
    const Replay& replay,
    std::string* error = nullptr
);
[[nodiscard]] ReplayLoadResult load_replay(const std::filesystem::path& path);
[[nodiscard]] ScoreSummary simulate_replay(
    const Chart& chart,
    const Replay& replay,
    double simulation_step_ms = 1.0
);

}  // namespace pulseforge

