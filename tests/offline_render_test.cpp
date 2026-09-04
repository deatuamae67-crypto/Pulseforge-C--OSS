#include "pulseforge/offline_render.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto token = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
            / ("pulseforge-offline-render-test-" + std::to_string(token));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void require(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void touch(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.put('\0');
    require(static_cast<bool>(output), "cannot create test fixture");
}

[[nodiscard]] std::filesystem::path ffmpeg_name(
    const std::filesystem::path& directory
) {
#if defined(_WIN32)
    return directory / "ffmpeg.exe";
#else
    return directory / "ffmpeg";
#endif
}

[[nodiscard]] bool has_argument(
    const std::vector<std::string>& arguments,
    const std::string& expected
) {
    return std::find(arguments.begin(), arguments.end(), expected)
        != arguments.end();
}

[[nodiscard]] bool has_argument_pair(
    const std::vector<std::string>& arguments,
    const std::string_view option,
    const std::string_view value
) {
    for (std::size_t index = 1U; index < arguments.size(); ++index) {
        if (arguments[index - 1U] == option && arguments[index] == value) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {
        reinterpret_cast<const char*>(value.data()),
        value.size(),
    };
}

void plan_is_deterministic_and_shell_free() {
    TemporaryDirectory temporary;
    const auto ffmpeg = ffmpeg_name(temporary.path() / "trusted tools");
    const auto instrumental = temporary.path() / "audio & stems" / "Inst.ogg";
    const auto vocals = temporary.path() / "audio & stems" / "Voices-bf.ogg";
    touch(ffmpeg);
    touch(instrumental);
    touch(vocals);

    pulseforge::OfflineRenderPlanRequest request;
    request.config.enabled = true;
    request.config.output_directory = temporary.path() / "renders";
    request.config.ffmpeg_executable = ffmpeg;
    request.config.width = 1280U;
    request.config.height = 720U;
    request.config.fps = 60U;
    request.chart_title = "Test / Song";
    request.difficulty = "hard";
    request.audio.instrumental = instrumental;
    request.audio.vocals.push_back(vocals);
    request.duration_ms = 1'000.1;
    request.temporary_token = "stable-token";

    const auto result = pulseforge::build_offline_render_plan(request);
    require(static_cast<bool>(result), result.error);
    const auto& plan = *result.plan;
    require(plan.frame_count == 61U, "frame count must use ceil(duration * fps)");
    require(
        plan.final_output_path.filename() == "test-song-hard.mp4",
        "default filename was not sanitized"
    );
    require(plan.audio_inputs.size() == 2U, "audio stems were not preserved");
    require(
        plan.arguments.front() == path_utf8(plan.ffmpeg_executable),
        "argv[0] is not the validated absolute executable"
    );
    require(
        has_argument(
            plan.arguments,
            path_utf8(std::filesystem::canonical(instrumental))
        ),
        "instrumental path must be one unsplit argv element"
    );
    require(
        has_argument(
            plan.arguments,
            path_utf8(std::filesystem::canonical(vocals))
        ),
        "vocal path must be one unsplit argv element"
    );
    require(
        has_argument(plan.arguments, "pipe:0")
            && has_argument(plan.arguments, "rawvideo")
            && has_argument(plan.arguments, "[aout]"),
        "FFmpeg plan is missing raw frames or mixed audio mapping"
    );
    require(
        has_argument(
            plan.arguments,
            "[1:a:0][2:a:0]amix=inputs=2:duration=longest:normalize=0,"
            "alimiter=limit=0.95:latency=1[aout]"
        ),
        "multi-stem limiter must compensate its lookahead latency"
    );
    require(
        has_argument_pair(plan.arguments, "-preset", "veryfast")
            && has_argument_pair(plan.arguments, "-threads", "0")
            && has_argument_pair(plan.arguments, "-fps_mode", "passthrough")
            && has_argument_pair(plan.arguments, "-movflags", "+faststart"),
        "balanced profile must use x264 auto threads and streamable MP4"
    );
}

void encoding_profiles_only_change_encoder_effort() {
    TemporaryDirectory temporary;
    const auto ffmpeg = ffmpeg_name(temporary.path() / "trusted");
    touch(ffmpeg);

    pulseforge::OfflineRenderPlanRequest request;
    request.config.enabled = true;
    request.config.output_directory = temporary.path() / "renders";
    request.config.output_name = "profile.mp4";
    request.config.ffmpeg_executable = ffmpeg;
    request.config.width = 640U;
    request.config.height = 360U;
    request.config.fps = 60U;
    request.config.crf = 21U;
    request.chart_title = "Profile";
    request.difficulty = "normal";
    request.duration_ms = 12'345.67;
    request.temporary_token = "test";

    request.config.preset = pulseforge::OfflineRenderPreset::fastest;
    const auto fastest = pulseforge::build_offline_render_plan(request);
    require(static_cast<bool>(fastest), fastest.error);
    request.config.preset = pulseforge::OfflineRenderPreset::compact;
    const auto compact = pulseforge::build_offline_render_plan(request);
    require(static_cast<bool>(compact), compact.error);

    require(
        has_argument_pair(fastest.plan->arguments, "-preset", "superfast")
            && has_argument_pair(compact.plan->arguments, "-preset", "slower"),
        "render profiles must map to the extended software presets"
    );
    require(
        fastest.plan->frame_count == compact.plan->frame_count
            && fastest.plan->duration_ms == compact.plan->duration_ms
            && fastest.plan->fps == compact.plan->fps,
        "encoding profile must not alter timing or frame count"
    );
    require(
        has_argument_pair(fastest.plan->arguments, "-crf", "21")
            && has_argument_pair(compact.plan->arguments, "-crf", "21"),
        "encoding profile must preserve the selected CRF"
    );
}

void extended_codec_and_maximum_performance_profiles_are_exact() {
    // PULSEFORGE_P1_5_0E_EXTENDED_FFMPEG_PROFILE_TEST_V1
    TemporaryDirectory temporary;
    const auto ffmpeg = ffmpeg_name(temporary.path() / "trusted");
    touch(ffmpeg);

    pulseforge::OfflineRenderPlanRequest request;
    request.config.enabled = true;
    request.config.output_directory = temporary.path() / "renders";
    request.config.output_name = "extended.mp4";
    request.config.ffmpeg_executable = ffmpeg;
    request.config.width = 1920U;
    request.config.height = 1080U;
    request.config.fps = 480U;
    request.config.crf = 16U;
    request.config.video_codec = pulseforge::OfflineRenderVideoCodec::av1;
    request.config.pixel_format = pulseforge::OfflineRenderPixelFormat::yuv444p;
    request.config.preset = pulseforge::OfflineRenderPreset::quality;
    request.config.audio_bitrate_kbps = 320U;
    request.config.thread_count = 8U;
    request.config.keyframe_interval_seconds = 5U;
    request.chart_title = "Extended";
    request.difficulty = "normal";
    request.duration_ms = 1'000.0;
    request.temporary_token = "test";

    const auto av1 = pulseforge::build_offline_render_plan(request);
    require(static_cast<bool>(av1), av1.error);
    require(
        has_argument_pair(av1.plan->arguments, "-c:v", "libsvtav1")
            && has_argument_pair(av1.plan->arguments, "-preset", "5")
            && has_argument_pair(av1.plan->arguments, "-pix_fmt", "yuv444p")
            && has_argument_pair(av1.plan->arguments, "-threads", "8")
            && has_argument_pair(av1.plan->arguments, "-g", "2400"),
        "AV1 profile must preserve codec, chroma, threads and GOP settings"
    );

    request.config.maximum_performance = true;
    request.config.faststart = true;
    const auto maximum = pulseforge::build_offline_render_plan(request);
    require(static_cast<bool>(maximum), maximum.error);
    require(maximum.plan->maximum_performance, "plan keeps maximum-performance intent");
    require(
        has_argument_pair(maximum.plan->arguments, "-c:v", "libx264")
            && has_argument_pair(maximum.plan->arguments, "-preset", "ultrafast")
            && has_argument_pair(maximum.plan->arguments, "-tune", "zerolatency")
            && has_argument_pair(maximum.plan->arguments, "-bf", "0")
            && has_argument_pair(maximum.plan->arguments, "-refs", "1")
            && !has_argument_pair(maximum.plan->arguments, "-movflags", "+faststart"),
        "maximum performance must use the portable low-overhead H.264 path"
    );
}

void unsafe_output_and_ffmpeg_are_rejected() {
    TemporaryDirectory temporary;
    const auto forbidden_root = temporary.path() / "mods";
    const auto untrusted = ffmpeg_name(forbidden_root / "downloaded-engine");
    touch(untrusted);

    pulseforge::OfflineRenderPlanRequest request;
    request.config.enabled = true;
    request.config.output_directory = temporary.path() / "renders";
    request.config.ffmpeg_executable = untrusted;
    request.config.output_name = "../escape.mp4";
    request.chart_title = "Unsafe";
    request.difficulty = "normal";
    request.duration_ms = 2'000.0;
    request.forbidden_executable_roots.push_back(forbidden_root);
    request.temporary_token = "test";

    auto result = pulseforge::build_offline_render_plan(request);
    require(!result, "parent traversal output must be rejected");

    request.config.output_name = "safe.mp4";
    result = pulseforge::build_offline_render_plan(request);
    require(!result, "FFmpeg from a mod/content root must be rejected");
    require(
        result.error.find("forbidden") != std::string::npos,
        "untrusted executable error should explain the forbidden root"
    );
}

void bounds_and_single_audio_mapping_are_validated() {
    TemporaryDirectory temporary;
    const auto ffmpeg = ffmpeg_name(temporary.path() / "trusted");
    const auto instrumental = temporary.path() / "Inst.mp3";
    touch(ffmpeg);
    touch(instrumental);

    pulseforge::OfflineRenderPlanRequest request;
    request.config.enabled = true;
    request.config.output_directory = temporary.path() / "renders";
    request.config.output_name = "single.mp4";
    request.config.ffmpeg_executable = ffmpeg;
    request.chart_title = "Single";
    request.difficulty = "normal";
    request.audio.instrumental = instrumental;
    request.duration_ms = 3'000.0;
    request.temporary_token = "test";

    auto result = pulseforge::build_offline_render_plan(request);
    require(static_cast<bool>(result), result.error);
    require(
        has_argument(result.plan->arguments, "1:a:0")
            && !has_argument(result.plan->arguments, "[aout]"),
        "one stem should use a direct audio map"
    );

    request.config.fps = 0U;
    result = pulseforge::build_offline_render_plan(request);
    require(!result, "zero FPS must be rejected");
    request.config.fps = 60U;
    request.config.width = 8'000U;
    result = pulseforge::build_offline_render_plan(request);
    require(!result, "oversized resolution must be rejected");

    request.config.width = 321U;
    request.config.height = 181U;
    result = pulseforge::build_offline_render_plan(request);
    require(!result, "odd yuv420p dimensions must be rejected before FFmpeg");

    request.config.pixel_format = pulseforge::OfflineRenderPixelFormat::yuv444p;
    request.config.maximum_performance = false;
    result = pulseforge::build_offline_render_plan(request);
    require(static_cast<bool>(result), "odd yuv444p dimensions should be legal");
    request.config.maximum_performance = true;
    result = pulseforge::build_offline_render_plan(request);
    require(
        !result,
        "maximum-performance yuv420p override must reject odd dimensions before FFmpeg"
    );
}

void bundled_ffmpeg_beside_renders_is_discovered() {
    TemporaryDirectory temporary;
    const auto ffmpeg = ffmpeg_name(
        temporary.path() / "tools" / "ffmpeg" / "bin"
    );
    touch(ffmpeg);

    pulseforge::OfflineRenderPlanRequest request;
    request.config.enabled = true;
    request.config.output_directory = temporary.path() / "renders";
    request.config.output_name = "bundled.mp4";
    request.chart_title = "Bundled";
    request.difficulty = "normal";
    request.duration_ms = 500.0;
    request.temporary_token = "test";

    const auto result = pulseforge::build_offline_render_plan(request);
    require(static_cast<bool>(result), result.error);
    require(
        result.plan->ffmpeg_executable == std::filesystem::canonical(ffmpeg),
        "project-owned FFmpeg beside renders was not discovered"
    );
}

void explicit_ffmpeg_must_be_absolute_and_outside_chart_root() {
    TemporaryDirectory temporary;
    const auto chart_root = temporary.path() / "downloaded chart";
    const auto adjacent_ffmpeg = ffmpeg_name(chart_root / "tools");
    const auto chart = chart_root / "song-hard.json";
    touch(adjacent_ffmpeg);
    touch(chart);

    pulseforge::OfflineRenderPlanRequest request;
    request.config.enabled = true;
    request.config.output_directory = temporary.path() / "renders";
    request.config.output_name = "security.mp4";
    request.config.ffmpeg_executable =
        std::filesystem::path{"tools"} / adjacent_ffmpeg.filename();
    request.source_chart_path = chart;
    request.chart_title = "Security";
    request.difficulty = "normal";
    request.duration_ms = 500.0;
    request.temporary_token = "test";

    auto result = pulseforge::build_offline_render_plan(request);
    require(!result, "relative explicit FFmpeg must be rejected");
    require(
        result.error.find("absolute") != std::string::npos,
        "relative executable error should state the absolute-path contract"
    );

    request.config.ffmpeg_executable = adjacent_ffmpeg;
    result = pulseforge::build_offline_render_plan(request);
    require(
        !result,
        "FFmpeg adjacent to the source chart must be rejected automatically"
    );
    require(
        result.error.find("forbidden") != std::string::npos,
        "chart-root executable error should explain the trust boundary"
    );
}

}  // namespace

int main() {
    try {
        plan_is_deterministic_and_shell_free();
        encoding_profiles_only_change_encoder_effort();
        extended_codec_and_maximum_performance_profiles_are_exact();
        unsafe_output_and_ffmpeg_are_rejected();
        bounds_and_single_audio_mapping_are_validated();
        bundled_ffmpeg_beside_renders_is_discovered();
        explicit_ffmpeg_must_be_absolute_and_outside_chart_root();
        std::cout << "offline render plan tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "offline render plan test failed: " << exception.what()
                  << '\n';
        return 1;
    }
}
