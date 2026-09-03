#include "sniff_bridge.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const char* const message) {
    if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main(const int argc, char** const argv) {
    try {
        require(argc == 2, "expected the project wrapper path");
        const std::filesystem::path wrapper = argv[1];
        require(
            pulseforge::detail::audited_sniff_wrapper_integrity(wrapper),
            "the exact project wrapper must pass its native SHA-256 guard"
        );

        const auto nonce = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        const auto test_root = std::filesystem::temp_directory_path()
            / ("pulseforge-sniff-integrity-" + std::to_string(nonce));
        std::filesystem::create_directories(test_root);
        const auto altered = test_root / "import-sniff.ps1";
        std::filesystem::copy_file(wrapper, altered);
        {
            std::ofstream output(altered, std::ios::binary | std::ios::app);
            require(static_cast<bool>(output), "cannot create altered wrapper fixture");
            output.put('\n');
        }
        require(
            !pulseforge::detail::audited_sniff_wrapper_integrity(altered),
            "one modified byte must reject the wrapper before PowerShell"
        );
        std::error_code ignored;
        std::filesystem::remove_all(test_root, ignored);
        std::cout << "SNIFF bridge integrity tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "SNIFF bridge integrity test failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
