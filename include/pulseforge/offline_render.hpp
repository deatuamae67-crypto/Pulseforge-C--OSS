#pragma once

#include "pulseforge/chart.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pulseforge {

// x264 speed/compression profiles exposed by both the launcher and CLI. These
// only change encoder effort: frame timestamps, CRF, audio and duration remain
// governed by the same deterministic render plan.
enum class OfflineRenderPreset : std::uint8_t {
    realtime,
    fastest,
    balanced,
    quality,
    compact,
};

enum class OfflineRenderVideoCodec : std::uint8_t {
    h264,
    h265,
    av1,
};

enum class OfflineRenderPixelFormat : std::uint8_t {
    yuv420p,
    yuv422p,
    yuv444p,
};

// Settings for deterministic, non-realtime chart capture. The output name is
// deliberately a filename (not an arbitrary path): every finished render is
// committed below output_directory and FFmpeg writes to an atomic temporary
// file first.
struct OfflineRenderConfig {
    bool enabled{false};
    std::filesystem::path output_directory{"renders"};
    std::filesystem::path output_name;
    std::filesystem::path ffmpeg_executable;
    std::uint32_t width{1920U};
    std::uint32_t height{1080U};
    std::uint32_t fps{60U};
    std::uint32_t crf{18U};
    OfflineRenderPreset preset{OfflineRenderPreset::balanced};
    OfflineRenderVideoCodec video_codec{OfflineRenderVideoCodec::h264};
    OfflineRenderPixelFormat pixel_format{OfflineRenderPixelFormat::yuv420p};
    std::uint32_t audio_bitrate_kbps{256U};
    // Zero delegates thread selection to FFmpeg/the encoder.
    std::uint32_t thread_count{0U};
    // Zero keeps the encoder default GOP policy.
    std::uint32_t keyframe_interval_seconds{2U};
    bool faststart{true};
    // PULSEFORGE_P1_5_0E_FFMPEG_MAXIMUM_PERFORMANCE_CONFIG_V1
    // Forces the universally available software H.264 ultrafast/zero-latency
    // path and a deeper but still bounded producer/encoder pipeline.
    bool maximum_performance{false};
    bool overwrite{false};
};

struct OfflineRenderPlanRequest {
    OfflineRenderConfig config;
    std::string chart_title;
    std::string difficulty;
    AudioManifest audio;
    double duration_ms{};
    // When supplied, the chart's containing directory is automatically
    // treated as untrusted for executable discovery. This is a defence in
    // depth guard for direct CLI renders that are not associated with a
    // catalogued mod/content root.
    std::filesystem::path source_chart_path;
    // FFmpeg candidates below one of these roots are untrusted. Callers put
    // every mod/content/Drive import root here; no executable found there is
    // ever launched.
    std::vector<std::filesystem::path> forbidden_executable_roots;
    // Stable in tests, unique per runtime invocation. It is used only for
    // private temporary filenames, never as a process argument supplied by a
    // mod.
    std::string temporary_token;
};

struct OfflineRenderPlan {
    std::filesystem::path ffmpeg_executable;
    std::filesystem::path final_output_path;
    std::filesystem::path temporary_output_path;
    std::filesystem::path diagnostic_log_path;
    std::vector<std::filesystem::path> audio_inputs;
    // argv[0] is always the validated absolute FFmpeg path. Arguments are
    // passed as an array directly to the OS; a command shell is never used.
    std::vector<std::string> arguments;
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t fps{};
    std::uint64_t frame_count{};
    double duration_ms{};
    bool maximum_performance{};
    bool overwrite{};
};

struct OfflineRenderPlanResult {
    std::optional<OfflineRenderPlan> plan;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return plan.has_value();
    }
};

// Pure apart from filesystem inspection. It does not execute FFmpeg and is
// therefore suitable for unit tests and UI previews of the exact command.
[[nodiscard]] OfflineRenderPlanResult build_offline_render_plan(
    const OfflineRenderPlanRequest& request
);

}  // namespace pulseforge
