#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulseforge::detail {

// Returns a compact platform/runtime snapshot suitable for performance reports.
// Android includes ART/GC counters and Java heap figures; other platforms return
// an empty string so the gameplay capture remains completely portable.
[[nodiscard]] std::string android_runtime_diagnostics();

// Publishes an app-private file into the user-visible Downloads/PulseForge
// collection on Android. Returns the published content URI/path on success.
// Other platforms simply return the original filesystem path.
[[nodiscard]] std::string publish_file_to_downloads(
    const std::filesystem::path& source,
    std::string_view display_name,
    std::string_view mime_type
);

struct AndroidFfmpegSession final {
    std::int64_t id{-1};
    std::filesystem::path pipe_path;
};

// Android uses the in-APK FFmpegKit runtime instead of trying to execute a
// desktop ffmpeg/ffmpeg.exe. The first raw-video pipe argument is replaced by
// an app-private FIFO owned by FFmpegKit.
[[nodiscard]] std::optional<AndroidFfmpegSession> start_android_ffmpeg(
    std::span<const std::string> arguments,
    std::string* error = nullptr
);

[[nodiscard]] bool finish_android_ffmpeg(
    std::int64_t session_id,
    int& exit_code,
    std::string& diagnostic_output,
    std::string* error = nullptr
);

void cancel_android_ffmpeg(std::int64_t session_id) noexcept;

}  // namespace pulseforge::detail
