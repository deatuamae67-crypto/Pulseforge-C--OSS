#include "media_routes.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <string_view>

namespace pulseforge::detail {
namespace {

[[nodiscard]] bool supported_movie_extension(
    const std::filesystem::path& extension
) {
    auto value = extension.string();
    std::ranges::transform(value, value.begin(), [](const unsigned char item) {
        return static_cast<char>(std::tolower(item));
    });
    return value == ".mp4" || value == ".m4v" || value == ".mov";
}

[[nodiscard]] std::filesystem::path normalized_file(
    const std::filesystem::path& candidate
) {
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(candidate, error);
    return error ? candidate.lexically_normal() : canonical;
}

[[nodiscard]] std::vector<std::filesystem::path> movies_in_directory(
    const std::span<const std::filesystem::path> content_roots,
    const std::filesystem::path& relative_directory
) {
    std::vector<std::filesystem::path> result;
    std::set<std::filesystem::path> unique;
    std::error_code error;
    for (const auto& root : content_roots) {
        const auto directory = root / relative_directory;
        if (!std::filesystem::is_directory(directory, error) || error) {
            error.clear();
            continue;
        }
        for (std::filesystem::directory_iterator iterator(directory, error), end;
             iterator != end && !error;
             iterator.increment(error)) {
            if (!iterator->is_regular_file(error) || error
                || !supported_movie_extension(iterator->path().extension())) {
                error.clear();
                continue;
            }
            const auto selected = normalized_file(iterator->path());
            if (unique.insert(selected).second) {
                result.push_back(selected);
            }
        }
        error.clear();
    }
    std::ranges::sort(result);
    return result;
}

}  // namespace

std::vector<std::filesystem::path> discover_startup_movies(
    const std::span<const std::filesystem::path> content_roots
) {
    auto result = movies_in_directory(content_roots, "intro/startup");
    std::set<std::filesystem::path> unique(result.begin(), result.end());
    constexpr std::string_view legacy_relative_name =
        "intro/watch-dogs-boot-FiL0S0V.mp4";
    std::error_code error;
    for (const auto& root : content_roots) {
        const auto candidate = root / legacy_relative_name;
        if (!std::filesystem::is_regular_file(candidate, error) || error) {
            error.clear();
            continue;
        }
        const auto selected = normalized_file(candidate);
        if (unique.insert(selected).second) {
            result.push_back(selected);
        }
    }
    std::ranges::sort(result);
    return result;
}

std::vector<std::filesystem::path> discover_return_movies(
    const std::span<const std::filesystem::path> content_roots
) {
    return movies_in_directory(content_roots, "intro/return-only");
}

std::filesystem::path discover_explicit_exit_movie(
    const std::span<const std::filesystem::path> content_roots
) {
    constexpr std::string_view relative_name =
        "intro/exit-only/dedsec-final-warning.mp4";
    std::error_code error;
    for (const auto& root : content_roots) {
        const auto candidate = root / relative_name;
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            return normalized_file(candidate);
        }
        error.clear();
    }
    return content_roots.empty()
        ? std::filesystem::path(relative_name)
        : content_roots.front() / relative_name;
}

}  // namespace pulseforge::detail
