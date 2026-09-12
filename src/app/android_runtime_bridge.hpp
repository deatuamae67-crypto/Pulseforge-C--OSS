#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulseforge::detail {

[[nodiscard]] std::string android_runtime_diagnostics();

[[nodiscard]] std::string publish_file_to_downloads(
    const std::filesystem::path& source,
    std::string_view display_name,
    std::string_view mime_type
);

struct AndroidFfmpegSession final {
    std::int64_t session_id{-1};
    std::string input_pipe;
};

[[nodiscard]] bool android_ffmpeg_available() noexcept;
[[nodiscard]] AndroidFfmpegSession start_android_ffmpeg(
    std::span<const std::string> arguments,
    std::string* error = nullptr
);
[[nodiscard]] int wait_android_ffmpeg(
    std::int64_t session_id,
    std::string* output = nullptr
);
void cancel_android_ffmpeg(std::int64_t session_id) noexcept;
void close_android_ffmpeg_pipe(std::string_view path) noexcept;

}  // namespace pulseforge::detail
