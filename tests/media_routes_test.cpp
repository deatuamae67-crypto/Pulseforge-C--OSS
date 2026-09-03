#include "media_routes.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class TempTree final {
public:
    TempTree() {
        static std::atomic<unsigned long long> sequence{};
        const auto stamp = static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
        root = std::filesystem::current_path()
            / ("pulseforge-media-routes-test-" + std::to_string(stamp)
                + "-" + std::to_string(sequence.fetch_add(1U)));
        std::filesystem::create_directories(root);
    }

    ~TempTree() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    std::filesystem::path root;
};

void touch(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << "test";
    require(static_cast<bool>(output), "media fixture write failed");
}

}  // namespace

int main() {
    try {
        TempTree tree;
        const auto first = tree.root / "assets-a";
        const auto second = tree.root / "assets-b";
        const auto legacy = first / "intro/watch-dogs-boot-FiL0S0V.mp4";
        const auto ctos = first / "intro/startup/ctos.MP4";
        const auto alternative = second / "intro/startup/alternative.mov";
        const auto dedsec = first
            / "intro/exit-only/dedsec-final-warning.mp4";
        const auto transition = first / "intro/return-only/return.m4v";
        touch(legacy);
        touch(ctos);
        touch(alternative);
        touch(dedsec);
        touch(transition);
        touch(first / "intro/startup/not-a-movie.txt");

        const std::vector roots{first, second};
        const auto startup = pulseforge::detail::discover_startup_movies(roots);
        require(startup.size() == 3U, "startup pool did not contain every intro");
        require(
            std::ranges::find(startup, std::filesystem::weakly_canonical(dedsec))
                == startup.end(),
            "DedSec leaked into the startup pool"
        );
        require(
            std::ranges::find(
                startup,
                std::filesystem::weakly_canonical(transition)
            ) == startup.end(),
            "return-only media leaked into the startup pool"
        );

        const auto returns = pulseforge::detail::discover_return_movies(roots);
        require(returns.size() == 1U, "return-only route discovery mismatch");
        require(
            returns.front() == std::filesystem::weakly_canonical(transition),
            "wrong return-only media selected"
        );
        require(
            pulseforge::detail::discover_explicit_exit_movie(roots)
                == std::filesystem::weakly_canonical(dedsec),
            "explicit Quit did not resolve the DedSec movie"
        );

        std::cout << "PulseForge media routing tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "PulseForge media routing test failed: "
                  << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
