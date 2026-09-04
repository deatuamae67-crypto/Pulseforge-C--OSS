#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace pulseforge {

struct ModInstallLimits {
    std::uint64_t max_files{200'000ULL};
    std::uint64_t max_total_uncompressed_bytes{64ULL * 1024ULL * 1024ULL * 1024ULL};
    std::uint64_t max_single_file_bytes{8ULL * 1024ULL * 1024ULL * 1024ULL};
    std::uint64_t max_compression_ratio{5'000ULL};
    std::size_t max_relative_path_bytes{1'024U};
};

struct ModInstallOptions {
    ModInstallLimits limits;
    std::optional<std::string> destination_name;
    bool reject_executable_files{true};
};

struct ModInstallResult {
    std::filesystem::path installed_path;
    std::string mod_id;
    std::uint64_t installed_files{};
    std::uint64_t installed_bytes{};
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error.empty();
    }
};

// Installs an unpacked directory or a ZIP, 7z, RAR or TAR archive into a mods
// root. Installation is staged in a sibling directory and committed with a
// single rename. Existing mods are never overwritten. Paths, links, aggregate
// sizes, compression ratios and executable extensions are validated before an
// archive is committed.
[[nodiscard]] ModInstallResult install_mod(
    const std::filesystem::path& source,
    const std::filesystem::path& mods_root,
    const ModInstallOptions& options = {}
);

[[nodiscard]] bool is_supported_mod_archive(
    const std::filesystem::path& source
) noexcept;

}  // namespace pulseforge
