#include "pulseforge/offline_render.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string_view>

namespace pulseforge {
namespace {

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {
        reinterpret_cast<const char*>(value.data()),
        value.size(),
    };
}

[[nodiscard]] char ascii_lower(const char value) noexcept {
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value - 'A' + 'a')
        : value;
}

[[nodiscard]] std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), ascii_lower);
    return value;
}

[[nodiscard]] bool path_is_within(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate
) {
    if (root.empty() || candidate.empty()) {
        return false;
    }
    std::error_code error;
    const auto canonical_root = std::filesystem::weakly_canonical(root, error);
    if (error) {
        return false;
    }
    const auto canonical_candidate = std::filesystem::weakly_canonical(
        candidate,
        error
    );
    if (error) {
        return false;
    }
    const auto relative = canonical_candidate.lexically_relative(canonical_root);
    if (relative.empty()) {
        return canonical_candidate == canonical_root;
    }
    if (relative.is_absolute() || relative.has_root_name()
        || relative.has_root_directory()) {
        return false;
    }
    return std::none_of(relative.begin(), relative.end(), [](const auto& part) {
        return part == "..";
    });
}

[[nodiscard]] bool is_forbidden_executable(
    const std::filesystem::path& candidate,
    const std::vector<std::filesystem::path>& roots
) {
    return std::any_of(roots.begin(), roots.end(), [&](const auto& root) {
        return path_is_within(root, candidate);
    });
}

[[nodiscard]] bool ffmpeg_filename(const std::filesystem::path& path) {
    const auto filename = lower_ascii(path_utf8(path.filename()));
#if defined(_WIN32)
    return filename == "ffmpeg.exe";
#else
    return filename == "ffmpeg";
#endif
}

[[nodiscard]] std::optional<std::filesystem::path> checked_ffmpeg(
    const std::filesystem::path& candidate,
    const std::vector<std::filesystem::path>& forbidden_roots
) {
    if (candidate.empty() || !candidate.is_absolute()
        || !ffmpeg_filename(candidate)) {
        return std::nullopt;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(candidate, error) || error) {
        return std::nullopt;
    }
    const auto canonical = std::filesystem::canonical(candidate, error);
    if (error || is_forbidden_executable(canonical, forbidden_roots)) {
        return std::nullopt;
    }
    return canonical;
}

[[nodiscard]] std::vector<std::filesystem::path> path_directories() {
    std::vector<std::filesystem::path> result;
#if defined(_WIN32)
    char* environment_buffer = nullptr;
    std::size_t environment_size = 0U;
    if (_dupenv_s(&environment_buffer, &environment_size, "PATH") != 0
        || environment_buffer == nullptr) {
        return result;
    }
    const std::string environment_storage(environment_buffer);
    std::free(environment_buffer);
    const std::string_view text(environment_storage);
#else
    const char* environment = std::getenv("PATH");
    if (environment == nullptr) {
        return result;
    }
    const std::string_view text(environment);
#endif
#if defined(_WIN32)
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif
    std::size_t begin = 0U;
    while (begin <= text.size()) {
        const auto end = text.find(separator, begin);
        const auto item = text.substr(
            begin,
            end == std::string_view::npos ? text.size() - begin : end - begin
        );
        // Empty and relative PATH entries can resolve through the current
        // directory, which may be a mod/import root. They are never trusted.
        if (!item.empty()) {
            const std::filesystem::path directory(item);
            if (directory.is_absolute()) {
                result.push_back(directory);
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1U;
    }
    return result;
}

[[nodiscard]] std::optional<std::filesystem::path> discover_ffmpeg(
    const OfflineRenderConfig& config,
    const std::vector<std::filesystem::path>& forbidden_roots
) {
    if (!config.ffmpeg_executable.empty()) {
        // Never interpret an executable relative to the current working
        // directory. That directory may be controlled by a mod or imported
        // chart, and resolving it here would contradict the CLI contract.
        return checked_ffmpeg(config.ffmpeg_executable, forbidden_roots);
    }

    // A release may bundle the official FFmpeg package in a clearly owned
    // tools directory beside renders/. This location is checked before PATH,
    // but still goes through canonicalization and the forbidden-root guard.
    // In particular, tools copied into a mod/Drive content root remain barred.
    std::error_code output_error;
    auto output_directory = config.output_directory;
    if (!output_directory.is_absolute()) {
        output_directory = std::filesystem::absolute(
            output_directory,
            output_error
        );
    }
    if (!output_error) {
#if defined(_WIN32)
        const auto bundled = output_directory.parent_path()
            / "tools" / "ffmpeg" / "bin" / "ffmpeg.exe";
#else
        const auto bundled = output_directory.parent_path()
            / "tools" / "ffmpeg" / "bin" / "ffmpeg";
#endif
        if (auto candidate = checked_ffmpeg(bundled, forbidden_roots);
            candidate.has_value()) {
            return candidate;
        }
    }

#if defined(_WIN32)
    constexpr std::string_view executable_name = "ffmpeg.exe";
#else
    constexpr std::string_view executable_name = "ffmpeg";
#endif
    for (const auto& directory : path_directories()) {
        if (auto candidate = checked_ffmpeg(
                directory / executable_name,
                forbidden_roots
            ); candidate.has_value()) {
            return candidate;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::string safe_stem(std::string_view source) {
    std::string result;
    result.reserve(std::min<std::size_t>(source.size(), 80U));
    bool separator_pending = false;
    for (const unsigned char value : source) {
        const bool allowed = (value >= 'a' && value <= 'z')
            || (value >= 'A' && value <= 'Z')
            || (value >= '0' && value <= '9');
        if (allowed) {
            if (separator_pending && !result.empty()) {
                result.push_back('-');
            }
            result.push_back(ascii_lower(static_cast<char>(value)));
            separator_pending = false;
        } else {
            separator_pending = !result.empty();
        }
        if (result.size() >= 80U) {
            break;
        }
    }
    while (!result.empty() && result.back() == '-') {
        result.pop_back();
    }
    return result.empty() ? std::string{"chart"} : result;
}

[[nodiscard]] std::string decimal(const double value) {
    char buffer[64]{};
    const auto converted = std::to_chars(
        std::begin(buffer),
        std::end(buffer),
        value,
        std::chars_format::fixed,
        6
    );
    if (converted.ec != std::errc{}) {
        return "0.000000";
    }
    return {buffer, converted.ptr};
}

// PULSEFORGE_P1_5_0E_EXTENDED_FFMPEG_PROFILE_MATRIX_V1
[[nodiscard]] constexpr std::string_view software_preset(
    const OfflineRenderPreset preset
) noexcept {
    switch (preset) {
    case OfflineRenderPreset::realtime:
        return "ultrafast";
    case OfflineRenderPreset::fastest:
        return "superfast";
    case OfflineRenderPreset::quality:
        return "slow";
    case OfflineRenderPreset::compact:
        return "slower";
    case OfflineRenderPreset::balanced:
    default:
        return "veryfast";
    }
}

[[nodiscard]] constexpr std::string_view video_codec_name(
    const OfflineRenderVideoCodec codec
) noexcept {
    switch (codec) {
    case OfflineRenderVideoCodec::h265:
        return "libx265";
    case OfflineRenderVideoCodec::av1:
        return "libsvtav1";
    case OfflineRenderVideoCodec::h264:
    default:
        return "libx264";
    }
}

[[nodiscard]] constexpr std::string_view pixel_format_name(
    const OfflineRenderPixelFormat format
) noexcept {
    switch (format) {
    case OfflineRenderPixelFormat::yuv422p:
        return "yuv422p";
    case OfflineRenderPixelFormat::yuv444p:
        return "yuv444p";
    case OfflineRenderPixelFormat::yuv420p:
    default:
        return "yuv420p";
    }
}

[[nodiscard]] constexpr std::string_view svt_av1_preset(
    const OfflineRenderPreset preset
) noexcept {
    switch (preset) {
    case OfflineRenderPreset::realtime: return "12";
    case OfflineRenderPreset::fastest: return "10";
    case OfflineRenderPreset::quality: return "5";
    case OfflineRenderPreset::compact: return "4";
    case OfflineRenderPreset::balanced:
    default: return "8";
    }
}

[[nodiscard]] bool safe_output_name(const std::filesystem::path& name) {
    if (name.empty() || name.is_absolute() || name.has_root_name()
        || name.has_root_directory() || name.has_parent_path()
        || name.filename() == "." || name.filename() == "..") {
        return false;
    }
    return lower_ascii(path_utf8(name.extension())) == ".mp4";
}

void append_existing_audio(
    std::vector<std::filesystem::path>& result,
    const std::filesystem::path& path
) {
    if (path.empty()) {
        return;
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return;
    }
    const auto canonical = std::filesystem::canonical(path, error);
    if (error) {
        return;
    }
    if (std::find(result.begin(), result.end(), canonical) == result.end()) {
        result.push_back(canonical);
    }
}

}  // namespace

OfflineRenderPlanResult build_offline_render_plan(
    const OfflineRenderPlanRequest& request
) {
    if (!request.config.enabled) {
        return {{}, "offline rendering is not enabled"};
    }
    if (request.config.width < 320U || request.config.width > 7'680U
        || request.config.height < 180U || request.config.height > 4'320U
        || static_cast<std::uint64_t>(request.config.width)
                * request.config.height
            > 33'177'600ULL) {
        return {{}, "render resolution must be 320x180..7680x4320 (at most 33,177,600 pixels)"};
    }
    // PULSEFORGE_P1_5_0E_EFFECTIVE_PIXEL_FORMAT_VALIDATION_V1
    // Maximum-performance mode always emits yuv420p, so validate the effective
    // format rather than only the user-visible format selected before override.
    const auto validated_pixel_format = request.config.maximum_performance
        ? OfflineRenderPixelFormat::yuv420p
        : request.config.pixel_format;
    if (validated_pixel_format == OfflineRenderPixelFormat::yuv420p
        && (((request.config.width & 1U) != 0U)
            || ((request.config.height & 1U) != 0U))) {
        return {{}, "yuv420p render width and height must be even"};
    }
    if (validated_pixel_format == OfflineRenderPixelFormat::yuv422p
        && (request.config.width & 1U) != 0U) {
        return {{}, "yuv422p render width must be even"};
    }
    if (request.config.fps < 1U || request.config.fps > 1'000U) {
        return {{}, "render FPS must be in the range 1..1000"};
    }
    if (request.config.crf > 51U) {
        return {{}, "render CRF must be in the common cross-codec range 0..51"};
    }
    if (request.config.audio_bitrate_kbps < 64U
        || request.config.audio_bitrate_kbps > 512U) {
        return {{}, "AAC bitrate must be in the range 64..512 kbps"};
    }
    if (request.config.thread_count > 256U) {
        return {{}, "FFmpeg thread count must be 0 (auto) or 1..256"};
    }
    if (request.config.keyframe_interval_seconds > 30U) {
        return {{}, "keyframe interval must be in the range 0..30 seconds"};
    }
    if (!std::isfinite(request.duration_ms) || request.duration_ms <= 0.0) {
        return {{}, "render duration must be a finite positive value"};
    }
    if (request.config.output_directory.empty()) {
        return {{}, "render output directory is empty"};
    }

    std::error_code error;
    auto output_directory = request.config.output_directory;
    if (!output_directory.is_absolute()) {
        output_directory = std::filesystem::absolute(output_directory, error);
        if (error) {
            return {{}, "cannot resolve the renders directory"};
        }
    }
    output_directory = output_directory.lexically_normal();

    auto output_name = request.config.output_name;
    if (output_name.empty()) {
        auto base = safe_stem(request.chart_title);
        const auto difficulty = safe_stem(request.difficulty);
        if (!difficulty.empty() && difficulty != "normal") {
            base += '-' + difficulty;
        }
        output_name = base + ".mp4";
    }
    if (!safe_output_name(output_name)) {
        return {{}, "--render-output must be a single .mp4 filename inside renders/"};
    }

    if (!request.config.ffmpeg_executable.empty()
        && !request.config.ffmpeg_executable.is_absolute()) {
        return {{}, "--ffmpeg requires an absolute path to ffmpeg/ffmpeg.exe"};
    }

    auto forbidden_executable_roots = request.forbidden_executable_roots;
    if (!request.source_chart_path.empty()) {
        std::error_code source_error;
        auto source_chart = request.source_chart_path;
        if (!source_chart.is_absolute()) {
            source_chart = std::filesystem::absolute(source_chart, source_error);
        }
        if (!source_error && !source_chart.parent_path().empty()) {
            forbidden_executable_roots.push_back(source_chart.parent_path());
        }
    }

    const auto ffmpeg = discover_ffmpeg(
        request.config,
        forbidden_executable_roots
    );
    if (!ffmpeg.has_value()) {
        return {{}, request.config.ffmpeg_executable.empty()
            ? "FFmpeg was not found in an absolute trusted PATH directory; install FFmpeg or pass --ffmpeg C:\\path\\to\\ffmpeg.exe"
            : "the explicit FFmpeg path is missing, not named ffmpeg/ffmpeg.exe, or is inside a forbidden mod/content root"};
    }

    const long double exact_frames = std::ceil(
        static_cast<long double>(request.duration_ms)
        * static_cast<long double>(request.config.fps)
        / 1'000.0L
    );
    if (exact_frames < 1.0L
        || exact_frames
            > static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
        return {{}, "render frame count exceeds the supported 64-bit range"};
    }

    OfflineRenderPlan plan;
    plan.ffmpeg_executable = *ffmpeg;
    plan.final_output_path = output_directory / output_name;
    const auto token = safe_stem(
        request.temporary_token.empty()
            ? std::string_view{"session"}
            : std::string_view{request.temporary_token}
    );
    const auto output_stem = safe_stem(path_utf8(output_name.stem()));
    plan.temporary_output_path = output_directory
        / (".pulseforge-" + output_stem + '-' + token + ".partial.mp4");
    plan.diagnostic_log_path = output_directory
        / (".pulseforge-" + output_stem + '-' + token + ".ffmpeg.log");
    plan.width = request.config.width;
    plan.height = request.config.height;
    plan.fps = request.config.fps;
    plan.frame_count = static_cast<std::uint64_t>(exact_frames);
    plan.duration_ms = request.duration_ms;
    plan.maximum_performance = request.config.maximum_performance;
    plan.overwrite = request.config.overwrite;

    append_existing_audio(plan.audio_inputs, request.audio.instrumental);
    for (const auto& vocals : request.audio.vocals) {
        append_existing_audio(plan.audio_inputs, vocals);
    }

    auto& args = plan.arguments;
    args.reserve(40U + plan.audio_inputs.size() * 2U);
    args.push_back(path_utf8(plan.ffmpeg_executable));
    args.insert(args.end(), {
        "-hide_banner",
        "-loglevel", "warning",
        "-nostdin",
        "-y",
        "-f", "rawvideo",
        "-pixel_format", "rgba",
        "-video_size",
        std::to_string(plan.width) + 'x' + std::to_string(plan.height),
        "-framerate", std::to_string(plan.fps),
        "-i", "pipe:0",
    });
    for (const auto& audio : plan.audio_inputs) {
        args.push_back("-i");
        args.push_back(path_utf8(audio));
    }
    args.insert(args.end(), {"-map", "0:v:0"});

    // Maximum-performance mode is intentionally portable: it never assumes a
    // vendor-specific GPU encoder exists. Users can therefore rely on it on
    // every FFmpeg build that already satisfies PulseForge's libx264 baseline.
    const auto effective_codec = request.config.maximum_performance
        ? OfflineRenderVideoCodec::h264
        : request.config.video_codec;
    const auto effective_preset = request.config.maximum_performance
        ? OfflineRenderPreset::realtime
        : request.config.preset;
    const auto effective_pixel_format = request.config.maximum_performance
        ? OfflineRenderPixelFormat::yuv420p
        : request.config.pixel_format;

    args.insert(args.end(), {
        "-c:v", std::string(video_codec_name(effective_codec)),
        "-threads", std::to_string(request.config.thread_count),
    });
    if (effective_codec == OfflineRenderVideoCodec::av1) {
        args.insert(args.end(), {"-preset", std::string(svt_av1_preset(effective_preset))});
    } else {
        args.insert(args.end(), {"-preset", std::string(software_preset(effective_preset))});
    }
    args.insert(args.end(), {
        "-crf", std::to_string(request.config.crf),
        "-pix_fmt", std::string(pixel_format_name(effective_pixel_format)),
        // Raw input already has the authoritative fixed cadence. Passthrough
        // forbids FFmpeg's vsync layer from synthesizing duplicate frames.
        "-fps_mode", "passthrough",
    });
    if (request.config.keyframe_interval_seconds != 0U) {
        const auto gop = static_cast<std::uint64_t>(plan.fps)
            * request.config.keyframe_interval_seconds;
        args.insert(args.end(), {"-g", std::to_string(gop)});
    }
    if (request.config.maximum_performance) {
        // PULSEFORGE_P1_5_0E_FFMPEG_MAXIMUM_PERFORMANCE_ARGS_V1
        args.insert(args.end(), {
            "-tune", "zerolatency",
            "-bf", "0",
            "-refs", "1",
            "-flush_packets", "0",
        });
    }
    if (!plan.audio_inputs.empty()) {
        if (plan.audio_inputs.size() == 1U) {
            args.insert(args.end(), {"-map", "1:a:0"});
        } else {
            std::string filter;
            for (std::size_t index = 0; index < plan.audio_inputs.size(); ++index) {
                filter += '[' + std::to_string(index + 1U) + ":a:0]";
            }
            filter += "amix=inputs=" + std::to_string(plan.audio_inputs.size())
                + ":duration=longest:normalize=0,"
                  "alimiter=limit=0.95:latency=1[aout]";
            args.insert(args.end(), {
                "-filter_complex", std::move(filter),
                "-map", "[aout]",
            });
        }
        args.insert(args.end(), {
            "-c:a", "aac",
            "-b:a", std::to_string(request.config.audio_bitrate_kbps) + "k",
            "-shortest",
        });
    }
    args.insert(args.end(), {
        "-t", decimal(plan.duration_ms / 1'000.0),
        "-max_muxing_queue_size", "1024",
    });
    if (request.config.faststart && !request.config.maximum_performance) {
        // Relocating MP4 metadata adds a bounded final mux pass. Maximum
        // performance skips it deliberately to publish as soon as FFmpeg exits.
        args.insert(args.end(), {"-movflags", "+faststart"});
    }
    args.push_back(path_utf8(plan.temporary_output_path));
    return {std::move(plan), {}};
}

}  // namespace pulseforge
