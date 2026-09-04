#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace pulseforge::detail {

inline constexpr char audited_sniff_sha256[] =
    "182387A573EC9E71BC3E3341632831A63655808153A0DAAB6A228383E26550B8";
inline constexpr char audited_sniff_wrapper_sha256[] =
    "7108D2663CDED2575DD1D571296EA241B55A6D9A9D2C6BA8EA1C6C75C9C34EC2";

struct SniffBridgeRequest {
    std::filesystem::path wrapper;
    std::filesystem::path executable;
    std::filesystem::path input_flp;
    std::filesystem::path output_json;
    std::string song_name;
};

struct SniffBridgeResult {
    bool success{};
    bool cancelled{};
    unsigned long exit_code{};
    std::string error;
};

// Verifies both the packaged file shape and the exact project-owned wrapper
// bytes before Windows PowerShell is allowed to interpret them.
[[nodiscard]] bool audited_sniff_wrapper_integrity(
    const std::filesystem::path& wrapper
) noexcept;

// Runs the project-owned guard script without a shell or visible console. The
// guard verifies the exact audited converter hash before it creates the SNIFF
// process and commits output transactionally. `pump_events` is called while
// waiting; returning false cancels the child process.
[[nodiscard]] SniffBridgeResult run_sniff_bridge(
    const SniffBridgeRequest& request,
    const std::function<bool()>& pump_events
);

}  // namespace pulseforge::detail
