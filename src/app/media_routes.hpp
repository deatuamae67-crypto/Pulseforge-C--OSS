#pragma once

#include <filesystem>
#include <span>
#include <vector>

namespace pulseforge::detail {

// Purpose-specific media discovery. Startup discovery never descends into the
// exit-only or return-only directories; the DedSec clip is resolved only by
// the explicit-quit route.
[[nodiscard]] std::vector<std::filesystem::path> discover_startup_movies(
    std::span<const std::filesystem::path> content_roots
);

[[nodiscard]] std::vector<std::filesystem::path> discover_return_movies(
    std::span<const std::filesystem::path> content_roots
);

[[nodiscard]] std::filesystem::path discover_explicit_exit_movie(
    std::span<const std::filesystem::path> content_roots
);

}  // namespace pulseforge::detail
