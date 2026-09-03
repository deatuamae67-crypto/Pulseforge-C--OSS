#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include "pulseforge/CoreEngine.h"
#include "pulseforge/chart_loader.hpp"
#include "pulseforge/content_catalog.hpp"
#include "pulseforge/gameplay.hpp"
#include "pulseforge/mod_installer.hpp"
#include "pulseforge/packed_chart.hpp"
#include "pulseforge/packed_chart_bridge.hpp"
#include "pulseforge/replay.hpp"
#include "pulseforge/settings.hpp"
#include "pulseforge/streaming_chart_importer.hpp"
#include "pulseforge/streaming_gameplay.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <limits>
#include <memory>
#include <optional>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <typeinfo>
#include <utility>
#include <vector>

#if defined(__ANDROID__)
#include <SDL3/SDL_main.h>
#endif

#if defined(__APPLE__)
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>
#endif

#if defined(_WIN32) && defined(PULSEFORGE_CLI_FRONTEND)
#include <windows.h>
#include <dbghelp.h>
#include <io.h>
#endif

namespace {

std::mutex cli_trace_mutex;
const auto cli_trace_start = std::chrono::steady_clock::now();
bool cli_pause_on_crash = true;
thread_local std::string cli_current_stage = "startup";

[[nodiscard]] std::string cli_trace_timestamp() {
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - cli_trace_start
    ).count();
    std::ostringstream output;
    output << std::fixed << std::setprecision(3) << elapsed << 's';
    return output.str();
}

void cli_trace(
    const std::string_view category,
    const std::string_view message,
    const std::source_location location = std::source_location::current()
) noexcept {
    try {
        std::scoped_lock lock(cli_trace_mutex);
        std::ostringstream line;
        line << "[PulseForge CLI][" << cli_trace_timestamp() << "]["
             << category << "][tid " << std::this_thread::get_id() << "] "
             << message << "  @ " << location.file_name() << ':'
             << location.line() << " (" << location.function_name() << ")\n";
        const auto rendered = line.str();
        std::cerr << rendered;
        std::cerr.flush();
        std::ofstream log("pulseforge-cli.log", std::ios::binary | std::ios::app);
        if (log) {
            log << rendered;
            log.flush();
        }
    } catch (...) {
        // Diagnostics must never be able to crash the crash reporter.
    }
}

void cli_set_stage(
    const std::string_view stage,
    const std::source_location location = std::source_location::current()
) noexcept {
    try {
        cli_current_stage.assign(stage);
    } catch (...) {
        cli_current_stage = "stage-allocation-failed";
    }
    cli_trace("STAGE", stage, location);
}

void cli_pause_for_crash() noexcept {
#if defined(_WIN32) && defined(PULSEFORGE_CLI_FRONTEND)
    if (!cli_pause_on_crash || _isatty(_fileno(stdin)) == 0) {
        return;
    }
    std::cerr << "\nPulseForge crashed. The CLI will stay open so the diagnostics "
                 "above can be read.\nPress ENTER to close...";
    std::cerr.flush();
    std::cin.clear();
    std::string ignored;
    std::getline(std::cin, ignored);
#endif
}

#if defined(_WIN32) && defined(PULSEFORGE_CLI_FRONTEND)
[[nodiscard]] std::string symbol_for_address(const void* const address) noexcept {
    HANDLE process = GetCurrentProcess();
    if (!SymInitialize(process, nullptr, TRUE)) {
        return {};
    }
    std::array<std::byte, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> storage{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage.data());
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement{};
    std::string result;
    if (SymFromAddr(
            process,
            reinterpret_cast<DWORD64>(address),
            &displacement,
            symbol
        )) {
        std::ostringstream output;
        output << symbol->Name << "+0x" << std::hex << displacement;
        result = output.str();
    }
    SymCleanup(process);
    return result;
}

LONG WINAPI pulseforge_cli_exception_filter(EXCEPTION_POINTERS* const info) noexcept {
    const DWORD code = info != nullptr && info->ExceptionRecord != nullptr
        ? info->ExceptionRecord->ExceptionCode
        : 0U;
    const void* const address = info != nullptr && info->ExceptionRecord != nullptr
        ? info->ExceptionRecord->ExceptionAddress
        : nullptr;
    std::ostringstream detail;
    detail << "UNHANDLED WINDOWS EXCEPTION 0x" << std::hex << code
           << " at " << address << " | stage=" << cli_current_stage;
    const auto symbol = symbol_for_address(address);
    if (!symbol.empty()) {
        detail << " | symbol=" << symbol;
    }
    cli_trace("CRASH", detail.str());

    void* frames[48]{};
    const auto count = CaptureStackBackTrace(0U, 48U, frames, nullptr);
    for (USHORT index = 0U; index < count; ++index) {
        std::ostringstream frame;
        frame << "frame " << index << " = " << frames[index];
        const auto frame_symbol = symbol_for_address(frames[index]);
        if (!frame_symbol.empty()) {
            frame << " | " << frame_symbol;
        }
        cli_trace("STACK", frame.str());
    }
    cli_pause_for_crash();
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void install_cli_crash_diagnostics() noexcept {
#if defined(PULSEFORGE_CLI_FRONTEND)
    cli_trace("BOOT", "installing fatal-error diagnostics");
    std::set_terminate([]() noexcept {
        std::string detail = "std::terminate invoked | stage=" + cli_current_stage;
        if (const auto exception = std::current_exception(); exception != nullptr) {
            try {
                std::rethrow_exception(exception);
            } catch (const std::exception& value) {
                detail += " | exception=";
                detail += typeid(value).name();
                detail += " | what=";
                detail += value.what();
            } catch (...) {
                detail += " | non-std exception";
            }
        }
        cli_trace("CRASH", detail);
        cli_pause_for_crash();
#if defined(_WIN32)
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(EXIT_FAILURE));
#else
        std::abort();
#endif
    });
#if defined(_WIN32)
    SetUnhandledExceptionFilter(pulseforge_cli_exception_filter);
#endif
#endif
}

struct CommandLine {
    pulseforge::AppLaunchOptions launch;
    std::filesystem::path settings_path;
    bool validate{};
    bool benchmark{};
    bool list_songs{};
    bool list_mods{};
    std::optional<std::filesystem::path> doctor_mod_root;
    std::optional<std::filesystem::path> doctor_json_output;
    bool doctor_deep{};
    std::optional<std::filesystem::path> install_mod_source;
    std::optional<std::filesystem::path> mods_root;
    std::optional<std::filesystem::path> compile_pfc_output;
    bool streaming_pfc_compile{};
    std::optional<std::filesystem::path> inspect_pfc_path;
    std::optional<std::filesystem::path> simulate_pfc_path;
    std::optional<std::filesystem::path> generate_pattern_pfc_output;
    bool inspect_pfc_notes{};
    bool discard_pfc_payloads{};
    std::uint32_t pfc_chunk_notes{65'536U};
    std::uint64_t pattern_note_count{};
    std::uint64_t pattern_interval_us{125'000U};
    std::vector<std::uint16_t> pattern_lanes{0U, 1U, 2U, 3U};
    bool show_help{};
    bool no_crash_pause{};
    std::uint32_t benchmark_iterations{5};
};

[[nodiscard]] std::optional<std::string> environment_value(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t size = 0U;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string result(value, size > 0U ? size - 1U : 0U);
    std::free(value);
    if (result.empty()) {
        return std::nullopt;
    }
    return result;
#else
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

[[nodiscard]] std::filesystem::path locate_assets(const char* executable) {
    if (const auto configured = environment_value("PULSEFORGE_ASSET_ROOT")) {
        const std::filesystem::path candidate(*configured);
        if (std::filesystem::exists(candidate / "demo" / "chart.json")) {
            return candidate;
        }
    }
    const auto executable_path = std::filesystem::absolute(executable);
    const std::vector<std::filesystem::path> candidates{
        executable_path.parent_path() / "assets",
#if defined(__APPLE__)
        // A native macOS application stores non-code resources below
        // PulseForge.app/Contents/Resources, while the executable lives in
        // PulseForge.app/Contents/MacOS.
        executable_path.parent_path().parent_path() / "Resources" / "assets",
#endif
        std::filesystem::current_path() / "assets",
        executable_path.parent_path().parent_path() / "assets",
        executable_path.parent_path().parent_path().parent_path() / "assets",
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate / "demo" / "chart.json")) {
            return candidate;
        }
    }
    return std::filesystem::current_path() / "assets";
}

struct RuntimePaths {
    std::filesystem::path settings;
    std::filesystem::path mods;
    std::filesystem::path renders;
};

[[nodiscard]] RuntimePaths default_runtime_paths(
    const std::filesystem::path& assets
) {
    RuntimePaths paths{
        .settings = assets / "settings.json",
        .mods = std::filesystem::current_path() / "mods",
        .renders = assets.parent_path() / "renders",
    };
#if defined(__APPLE__)
    struct SdlAllocationDeleter {
        void operator()(void* value) const noexcept {
            SDL_free(value);
        }
    };
    std::unique_ptr<char, SdlAllocationDeleter> preference_path(
        SDL_GetPrefPath("PulseForge", "PulseForge")
    );
    if (!preference_path) {
        throw std::runtime_error(
            "SDL_GetPrefPath could not create the macOS application data directory"
        );
    }

    const std::filesystem::path root(preference_path.get());
    paths.settings = root / "settings.json";
    paths.mods = root / "mods";
    paths.renders = root / "renders";

    std::error_code filesystem_error;
    std::filesystem::create_directories(paths.mods, filesystem_error);
    if (filesystem_error) {
        throw std::runtime_error(
            "could not create the macOS mods directory: "
            + filesystem_error.message()
        );
    }
    std::filesystem::create_directories(paths.renders, filesystem_error);
    if (filesystem_error) {
        throw std::runtime_error(
            "could not create the macOS renders directory: "
            + filesystem_error.message()
        );
    }

    const auto bundled_settings = assets / "settings.json";
    if (!std::filesystem::exists(paths.settings)
        && std::filesystem::is_regular_file(bundled_settings)) {
        std::filesystem::copy_file(
            bundled_settings,
            paths.settings,
            std::filesystem::copy_options::skip_existing,
            filesystem_error
        );
        if (filesystem_error && !std::filesystem::exists(paths.settings)) {
            throw std::runtime_error(
                "could not initialize macOS settings: "
                + filesystem_error.message()
            );
        }
    }
#endif
    return paths;
}

[[nodiscard]] std::string require_value(
    int& index,
    const int count,
    char** values,
    const std::string_view option
) {
    if (index + 1 >= count) {
        throw std::runtime_error("missing value after " + std::string(option));
    }
    ++index;
    return values[index];
}

[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> parse_render_size(
    const std::string_view text
) {
    const auto separator = text.find_first_of("xX");
    if (separator == std::string_view::npos || separator == 0U
        || separator + 1U >= text.size()
        || text.find_first_of("xX", separator + 1U) != std::string_view::npos) {
        throw std::runtime_error(
            "--render-size expects WIDTHxHEIGHT, for example 1920x1080"
        );
    }
    const auto parse_component = [](const std::string_view value) {
        std::size_t consumed = 0U;
        const auto parsed = std::stoull(std::string(value), &consumed);
        if (consumed != value.size()
            || parsed > (std::numeric_limits<std::uint32_t>::max)()) {
            throw std::runtime_error("render dimension is outside uint32 range");
        }
        return static_cast<std::uint32_t>(parsed);
    };
    return {
        parse_component(text.substr(0U, separator)),
        parse_component(text.substr(separator + 1U)),
    };
}

[[nodiscard]] std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const char ch) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    });
    return value;
}

[[nodiscard]] pulseforge::OfflineRenderPreset parse_render_preset(
    std::string value
) {
    value = lower_ascii(std::move(value));
    if (value == "realtime" || value == "ultrafast") {
        return pulseforge::OfflineRenderPreset::realtime;
    }
    if (value == "fastest" || value == "superfast") {
        return pulseforge::OfflineRenderPreset::fastest;
    }
    if (value == "balanced" || value == "veryfast") {
        return pulseforge::OfflineRenderPreset::balanced;
    }
    if (value == "quality" || value == "slow") {
        return pulseforge::OfflineRenderPreset::quality;
    }
    if (value == "compact" || value == "slower") {
        return pulseforge::OfflineRenderPreset::compact;
    }
    throw std::runtime_error(
        "--render-preset expects realtime, fastest, balanced, quality, or compact"
    );
}

// PULSEFORGE_P1_5_0E_EXTENDED_RENDER_CLI_V1
[[nodiscard]] pulseforge::OfflineRenderVideoCodec parse_render_codec(
    std::string value
) {
    value = lower_ascii(std::move(value));
    if (value == "h264" || value == "avc") {
        return pulseforge::OfflineRenderVideoCodec::h264;
    }
    if (value == "h265" || value == "hevc") {
        return pulseforge::OfflineRenderVideoCodec::h265;
    }
    if (value == "av1") {
        return pulseforge::OfflineRenderVideoCodec::av1;
    }
    throw std::runtime_error("--render-codec expects h264, h265, or av1");
}

[[nodiscard]] pulseforge::OfflineRenderPixelFormat parse_render_pixel_format(
    std::string value
) {
    value = lower_ascii(std::move(value));
    if (value == "yuv420p" || value == "420") {
        return pulseforge::OfflineRenderPixelFormat::yuv420p;
    }
    if (value == "yuv422p" || value == "422") {
        return pulseforge::OfflineRenderPixelFormat::yuv422p;
    }
    if (value == "yuv444p" || value == "444") {
        return pulseforge::OfflineRenderPixelFormat::yuv444p;
    }
    throw std::runtime_error(
        "--render-pixel-format expects yuv420p, yuv422p, or yuv444p"
    );
}

[[nodiscard]] std::vector<std::uint16_t> parse_pattern_lanes(
    const std::string_view text
) {
    if (text.empty() || text.size() > 4'096U) {
        throw std::runtime_error("--pattern-lanes requires a bounded CSV list");
    }
    std::vector<std::uint16_t> lanes;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto separator = text.find(',', start);
        const auto end = separator == std::string_view::npos
            ? text.size()
            : separator;
        const auto item = text.substr(start, end - start);
        if (item.empty()) {
            throw std::runtime_error("--pattern-lanes contains an empty item");
        }
        const auto value = std::stoul(std::string(item));
        if (value > 1'023U) {
            throw std::runtime_error("--pattern-lanes supports lane ids 0..1023");
        }
        lanes.push_back(static_cast<std::uint16_t>(value));
        if (lanes.size() > 1'024U) {
            throw std::runtime_error("--pattern-lanes has too many items");
        }
        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1U;
    }
    return lanes;
}

[[nodiscard]] CommandLine parse_command_line(
    const int count,
    char** values,
    const std::filesystem::path& assets,
    const RuntimePaths& runtime_paths
) {
    CommandLine command;
    bool explicit_content_selection = false;
    const auto demo_chart = assets / "demo" / "chart.json";
    command.launch.content_roots.push_back(assets);
    command.launch.show_launcher = true;
    command.settings_path = runtime_paths.settings;
    command.launch.offline_render.output_directory =
        runtime_paths.renders;

    for (int index = 1; index < count; ++index) {
        const std::string_view option(values[index]);
        if (option == "--help" || option == "-h") {
            command.show_help = true;
        } else if (option == "--no-crash-pause") {
            command.no_crash_pause = true;
        } else if (option == "--play" || option == "--chart") {
            explicit_content_selection = true;
            command.launch.chart_path =
                require_value(index, count, values, option);
            command.launch.show_launcher = false;
        } else if (option == "--menu" || option == "--freeplay") {
            command.launch.chart_path.clear();
            command.launch.catalog_song.reset();
            command.launch.show_launcher = true;
        } else if (option == "--content-root" || option == "--mods") {
            command.launch.content_roots.push_back(
                require_value(index, count, values, option)
            );
        } else if (option == "--song") {
            explicit_content_selection = true;
            command.launch.catalog_song = require_value(index, count, values, option);
            command.launch.chart_path.clear();
            command.launch.show_launcher = false;
        } else if (option == "--list-songs") {
            command.list_songs = true;
        } else if (option == "--list-mods") {
            command.list_mods = true;
        } else if (option == "--doctor-mod") {
            command.doctor_mod_root = require_value(
                index,
                count,
                values,
                option
            );
            command.launch.show_launcher = false;
        } else if (option == "--doctor-json") {
            command.doctor_json_output = require_value(
                index,
                count,
                values,
                option
            );
        }
        else if (option == "--doctor-deep") {
            command.doctor_deep = true;
            command.launch.show_launcher = false;
        } else if (option == "--install-mod") {
            command.install_mod_source = require_value(
                index,
                count,
                values,
                option
            );
        } else if (option == "--mods-root") {
            command.mods_root = require_value(index, count, values, option);
        } else if (option == "--compile-pfc") {
            command.compile_pfc_output = require_value(
                index,
                count,
                values,
                option
            );
            command.streaming_pfc_compile = false;
        } else if (option == "--stream-compile-pfc") {
            command.compile_pfc_output = require_value(
                index,
                count,
                values,
                option
            );
            command.streaming_pfc_compile = true;
        } else if (option == "--inspect-pfc") {
            command.inspect_pfc_path = require_value(
                index,
                count,
                values,
                option
            );
        } else if (option == "--simulate-pfc") {
            command.simulate_pfc_path = require_value(
                index,
                count,
                values,
                option
            );
        } else if (option == "--generate-pattern-pfc") {
            command.generate_pattern_pfc_output = require_value(
                index,
                count,
                values,
                option
            );
        } else if (option == "--pattern-notes") {
            command.pattern_note_count = std::stoull(
                require_value(index, count, values, option)
            );
        } else if (option == "--pattern-interval-us") {
            command.pattern_interval_us = std::stoull(
                require_value(index, count, values, option)
            );
        } else if (option == "--pattern-lanes") {
            command.pattern_lanes = parse_pattern_lanes(
                require_value(index, count, values, option)
            );
        } else if (option == "--scan-pfc-notes") {
            command.inspect_pfc_notes = true;
        } else if (option == "--discard-pfc-payloads") {
            command.discard_pfc_payloads = true;
        } else if (option == "--pfc-chunk-notes") {
            command.pfc_chunk_notes = static_cast<std::uint32_t>(std::stoul(
                require_value(index, count, values, option)
            ));
        } else if (option == "--settings") {
            command.settings_path = require_value(index, count, values, option);
        } else if (option == "--metadata") {
            command.launch.chart_options.metadata_path =
                require_value(index, count, values, option);
        } else if (option == "--difficulty") {
            command.launch.chart_options.difficulty =
                require_value(index, count, values, option);
            command.launch.chart_options.difficulty_explicit = true;
        } else if (option == "--inst") {
            command.launch.instrumental_override =
                require_value(index, count, values, option);
        } else if (option == "--voices") {
            command.launch.vocal_overrides.push_back(
                require_value(index, count, values, option)
            );
        } else if (option == "--script") {
            auto script = std::filesystem::path(
                require_value(index, count, values, option)
            );
            command.launch.script_paths.push_back(script);
            if (!command.launch.script_path.has_value()) {
                command.launch.script_path = std::move(script);
            }
        } else if (option == "--no-lua") {
            command.launch.enable_lua = false;
        } else if (option == "--safe-mode") {
            command.launch.safe_mode = true;
            command.launch.enable_lua = false;
        } else if (option == "--smoke-test") {
            command.launch.smoke_test = true;
            command.launch.smoke_test_chart_path = demo_chart;
            command.launch.settings.gameplay.autoplay = true;
            command.launch.chart_path = demo_chart;
            command.launch.show_launcher = false;
        } else if (option == "--replay") {
            command.launch.replay_path =
                require_value(index, count, values, option);
        } else if (option == "--save-replay") {
            command.launch.save_replay_path =
                require_value(index, count, values, option);
        } else if (option == "--validate") {
            command.validate = true;
            if (index + 1 < count && values[index + 1][0] != '-') {
                explicit_content_selection = true;
                command.launch.chart_path = values[++index];
                command.launch.show_launcher = false;
            }
        } else if (option == "--strict") {
            command.launch.chart_options.strict = true;
        } else if (option == "--benchmark") {
            command.benchmark = true;
            if (index + 1 < count && values[index + 1][0] != '-') {
                explicit_content_selection = true;
                command.launch.chart_path = values[++index];
                command.launch.show_launcher = false;
            }
        } else if (option == "--render") {
            command.launch.offline_render.enabled = true;
            command.launch.show_launcher = false;
            if (index + 1 < count && values[index + 1][0] != '-') {
                explicit_content_selection = true;
                command.launch.chart_path = values[++index];
            }
        } else if (option == "--render-size") {
            const auto [width, height] = parse_render_size(
                require_value(index, count, values, option)
            );
            command.launch.offline_render.width = width;
            command.launch.offline_render.height = height;
        } else if (option == "--render-fps") {
            command.launch.offline_render.fps = static_cast<std::uint32_t>(
                std::stoul(require_value(index, count, values, option))
            );
        } else if (option == "--render-output") {
            command.launch.offline_render.output_name =
                require_value(index, count, values, option);
        } else if (option == "--render-crf") {
            command.launch.offline_render.crf = static_cast<std::uint32_t>(
                std::stoul(require_value(index, count, values, option))
            );
        } else if (option == "--render-preset") {
            command.launch.offline_render.preset = parse_render_preset(
                require_value(index, count, values, option)
            );
        } else if (option == "--render-codec") {
            command.launch.offline_render.video_codec = parse_render_codec(
                require_value(index, count, values, option)
            );
        } else if (option == "--render-pixel-format") {
            command.launch.offline_render.pixel_format = parse_render_pixel_format(
                require_value(index, count, values, option)
            );
        } else if (option == "--render-audio-bitrate") {
            command.launch.offline_render.audio_bitrate_kbps =
                static_cast<std::uint32_t>(std::stoul(
                    require_value(index, count, values, option)
                ));
        } else if (option == "--render-threads") {
            command.launch.offline_render.thread_count =
                static_cast<std::uint32_t>(std::stoul(
                    require_value(index, count, values, option)
                ));
        } else if (option == "--render-keyframe-seconds") {
            command.launch.offline_render.keyframe_interval_seconds =
                static_cast<std::uint32_t>(std::stoul(
                    require_value(index, count, values, option)
                ));
        } else if (option == "--render-faststart") {
            command.launch.offline_render.faststart = true;
        } else if (option == "--no-render-faststart") {
            command.launch.offline_render.faststart = false;
        } else if (option == "--render-maximum-performance") {
            command.launch.offline_render.maximum_performance = true;
        } else if (option == "--render-overwrite") {
            command.launch.offline_render.overwrite = true;
        } else if (option == "--ffmpeg") {
            command.launch.offline_render.ffmpeg_executable =
                require_value(index, count, values, option);
        } else if (option == "--iterations") {
            command.benchmark_iterations = static_cast<std::uint32_t>(
                std::stoul(require_value(index, count, values, option))
            );
        } else if (option == "--botplay") {
            command.launch.settings.gameplay.autoplay = true;
        } else if (option == "--practice") {
            command.launch.settings.gameplay.practice = true;
        } else if (option == "--no-fail") {
            command.launch.settings.gameplay.no_fail = true;
        } else if (option == "--ghost-tapping") {
            command.launch.settings.gameplay.ghost_tapping = true;
        } else if (option == "--no-ghost-tapping") {
            command.launch.settings.gameplay.ghost_tapping = false;
        } else if (option == "--downscroll") {
            command.launch.settings.gameplay.downscroll = true;
        } else if (option == "--middlescroll") {
            command.launch.settings.gameplay.middle_scroll = true;
        } else if (option == "--mirror") {
            command.launch.settings.gameplay.mirror = true;
        } else if (option == "--random") {
            command.launch.settings.gameplay.randomize_lanes = true;
        } else if (option == "--hide-opponent") {
            command.launch.settings.gameplay.hide_opponent_notes = true;
        } else if (option == "--scroll-speed") {
            command.launch.settings.gameplay.scroll_speed = std::stod(
                require_value(index, count, values, option)
            );
        } else if (option == "--input-offset") {
            command.launch.settings.gameplay.input_offset_ms = std::stod(
                require_value(index, count, values, option)
            );
        } else if (option == "--visual-offset") {
            command.launch.settings.gameplay.visual_offset_ms = std::stod(
                require_value(index, count, values, option)
            );
        } else if (option == "--audio-offset") {
            command.launch.settings.audio.audio_offset_ms = std::stod(
                require_value(index, count, values, option)
            );
        } else if (option == "--latency-compensation") {
            command.launch.settings.audio.output_latency_compensation_ms = std::stod(
                require_value(index, count, values, option)
            );
        } else if (option == "--playback-rate") {
            command.launch.settings.audio.playback_rate = std::stod(
                require_value(index, count, values, option)
            );
        } else if (option == "--audio-buffer") {
            command.launch.settings.audio.buffer_frames = static_cast<std::uint32_t>(
                std::stoul(require_value(index, count, values, option))
            );
        } else if (option == "--low-quality") {
            command.launch.settings.visual.low_quality = true;
        } else if (option == "--reduced-motion") {
            command.launch.settings.visual.reduced_motion = true;
        } else if (option == "--no-splashes") {
            command.launch.settings.visual.note_splashes = false;
        } else if (option == "--no-flashing") {
            command.launch.settings.visual.flashing_lights = false;
        } else if (option == "--fullscreen") {
            command.launch.settings.visual.fullscreen = true;
        } else if (option == "--max-visible-notes") {
            command.launch.settings.performance.max_visible_notes =
                static_cast<std::uint32_t>(std::stoul(
                    require_value(index, count, values, option)
                ));
        } else if (option == "--script-memory") {
            command.launch.settings.performance.script_memory_mb =
                static_cast<std::uint32_t>(std::stoul(
                    require_value(index, count, values, option)
                ));
        } else if (option == "--script-budget") {
            command.launch.settings.performance.script_instruction_budget =
                static_cast<std::uint32_t>(std::stoul(
                    require_value(index, count, values, option)
                ));
        } else if (option == "--fps") {
            command.launch.settings.visual.fps_cap =
                std::stoi(require_value(index, count, values, option));
        } else if (option == "--vsync") {
            command.launch.settings.visual.vsync = true;
        } else if (option == "--no-vsync") {
            command.launch.settings.visual.vsync = false;
        } else if (!option.empty() && option.front() != '-') {
            explicit_content_selection = true;
            command.launch.chart_path = values[index];
            command.launch.show_launcher = false;
        } else {
            throw std::runtime_error("unknown option: " + std::string(option));
        }
    }
    command.benchmark_iterations =
        std::clamp<std::uint32_t>(command.benchmark_iterations, 1, 1'000);
    command.pfc_chunk_notes = std::clamp<std::uint32_t>(
        command.pfc_chunk_notes,
        1'024U,
        1'048'576U
    );
    const auto pfc_actions = static_cast<unsigned>(
        command.compile_pfc_output.has_value()
    ) + static_cast<unsigned>(command.inspect_pfc_path.has_value())
        + static_cast<unsigned>(command.simulate_pfc_path.has_value())
        + static_cast<unsigned>(command.generate_pattern_pfc_output.has_value());
    if (pfc_actions > 1U) {
        throw std::runtime_error(
            "PFC compile, inspect and pattern generation actions are mutually exclusive"
        );
    }
    const auto non_game_actions = static_cast<unsigned>(command.validate)
        + static_cast<unsigned>(command.benchmark)
        + static_cast<unsigned>(command.list_songs)
        + static_cast<unsigned>(command.list_mods)
        + static_cast<unsigned>(command.doctor_mod_root.has_value())
        + static_cast<unsigned>(command.install_mod_source.has_value())
        + pfc_actions;
    if (command.doctor_json_output.has_value()
        && !command.doctor_mod_root.has_value()) {
        throw std::runtime_error("--doctor-json requires --doctor-mod");
    }
    if (command.doctor_deep && !command.doctor_mod_root.has_value()) {
        throw std::runtime_error("--doctor-deep requires --doctor-mod");
    }
    if (command.doctor_mod_root.has_value()
        && (command.validate || command.benchmark
            || command.list_songs || command.list_mods
            || command.install_mod_source.has_value()
            || pfc_actions != 0U)) {
        throw std::runtime_error(
            "--doctor-mod cannot be combined with another CLI action"
        );
    }
    if (command.launch.offline_render.enabled && non_game_actions != 0U) {
        throw std::runtime_error(
            "--render cannot be combined with validation, benchmarks, lists, installation or PFC tools"
        );
    }
    if (command.launch.offline_render.enabled
        && command.launch.chart_path.empty()) {
        throw std::runtime_error("--render requires a chart JSON path");
    }
    if ((command.validate || command.benchmark)
        && command.launch.chart_path.empty()) {
        command.launch.chart_path = demo_chart;
        command.launch.show_launcher = false;
    }
    const auto bundled_demo = demo_chart.lexically_normal();
    if (command.launch.smoke_test
        && (explicit_content_selection
            || command.launch.catalog_song.has_value()
            || command.launch.chart_path.lexically_normal() != bundled_demo)) {
        throw std::runtime_error(
            "--smoke-test is an internal diagnostic and cannot be combined "
            "with a chart or catalog-song selection"
        );
    }
    if (!command.launch.script_path.has_value()
        && command.launch.chart_path.lexically_normal() == bundled_demo) {
        command.launch.script_path = assets / "demo" / "script.lua";
        command.launch.script_paths.push_back(*command.launch.script_path);
    }
    return command;
}

void print_help() {
    std::cout
        << "PulseForge " << PULSEFORGE_VERSION << " - low-latency C++ rhythm engine\n\n"
        << "Windows frontends:\n"
        << "  PulseForge.exe       Graphical game/launcher; no background console\n"
        << "  pulseforge-cli.exe   Console companion for automation, validation,\n"
        << "                       benchmarks, PFC1 and headless rendering\n"
        << "Both frontends use the same engine core and file formats. Use the CLI\n"
        << "when stdout/stderr and a reliable process exit code are required.\n"
        << "On a fatal crash the CLI stays open and writes pulseforge-cli.log.\n"
        << "Use --no-crash-pause for CI/automation.\n\n"
        << "Usage:\n"
        << "  pulseforge-cli [chart.json] [options]\n"
        << "  pulseforge-cli --validate [chart.json] [--strict]\n"
        << "  pulseforge-cli --benchmark [chart.json] [--iterations N]\n\n"
        << "  pulseforge-cli --render chart.json [render options]\n\n"
        << "  pulseforge-cli --chart chart.json --compile-pfc notes.pfc\n"
        << "  pulseforge-cli --chart huge.json --stream-compile-pfc notes.pfc\n"
        << "  pulseforge-cli --inspect-pfc notes.pfc [--scan-pfc-notes]\n\n"
        << "  pulseforge-cli --simulate-pfc notes.pfc\n\n"
        << "  pulseforge-cli --generate-pattern-pfc pattern.pfc --pattern-notes N\n\n"
        << "  pulseforge-cli --menu [--content-root PATH]\n"
        << "  pulseforge-cli --list-songs [--content-root PATH]\n\n"
        << "Content:\n"
        << "  --play, --chart PATH   Native, Psych/P-Slice/H-Slice/JS, DenpaEx, "
           "or V-Slice JSON\n"
        << "  --metadata PATH        V-Slice metadata JSON\n"
        << "  --difficulty NAME      Difficulty inside a V-Slice chart\n"
        << "  --inst PATH            Override instrumental (OGG/WAV/MP3/FLAC)\n"
        << "  --voices PATH          Add/override a vocal stem; repeatable\n"
        << "  --script PATH          Lua song script; repeatable\n"
        << "  --menu, --freeplay     Open the graphical multi-song browser\n"
        << "  --content-root PATH    Scan an assets/mods root; repeatable\n"
        << "  --song ID              Play a catalog id/song/title directly\n"
        << "  --list-songs           Print the discovered catalog and exit\n"
        << "  --list-mods            Print detected mods, order and state\n"
        << "  --doctor-mod PATH      Audit one mod with the real catalog/chart loaders\n"
        << "  --doctor-json PATH     Override Mod Doctor JSON report path\n"
        << "  --doctor-deep          Compile/simulate deferred charts through streaming\n"
        << "  --install-mod PATH     Safely install a ZIP or unpacked mod\n"
        << "  --mods-root PATH       Destination used by --install-mod\n"
        << "  --compile-pfc PATH     Build an atomic indexed PFC1 note cache\n"
        << "  --stream-compile-pfc PATH  Bounded-memory native/Psych JSON compiler\n"
        << "  --inspect-pfc PATH     Inspect a PFC1 cache without expanding it\n"
        << "  --simulate-pfc PATH    Run bounded autoplay through a PFC1 cache\n"
        << "  --generate-pattern-pfc PATH  Create one procedural PatternRun\n"
        << "  --pattern-notes N      Logical note count for generated PatternRun\n"
        << "  --pattern-interval-us N  Microseconds between procedural notes\n"
        << "  --pattern-lanes CSV    Repeating lane pattern (default 0,1,2,3)\n"
        << "  --scan-pfc-notes       Verify all chunk CRCs while inspecting\n"
        << "  --pfc-chunk-notes N    Explicit notes per cache chunk\n"
        << "  --discard-pfc-payloads Explicitly allow lossy V-Slice payload discard\n"
        << "  --settings PATH        Engine settings JSON\n\n"
        << "Offline render (deterministic, written below renders/):\n"
        << "  --render CHART         Render the real gameplay path frame by frame\n"
        << "  --render-size WxH      Output resolution (default 1920x1080)\n"
        << "  --render-fps N         Output FPS 1..1000 (default 60)\n"
        << "  --render-output NAME.mp4  Filename inside renders/\n"
        << "  --render-crf N         Video CRF 0..51 (default 18)\n"
        << "  --render-preset NAME   realtime, fastest, balanced, quality, compact\n"
        << "  --render-codec NAME    h264 (default), h265, or av1\n"
        << "  --render-pixel-format NAME  yuv420p, yuv422p, or yuv444p\n"
        << "  --render-audio-bitrate N  AAC bitrate 64..512 kbps\n"
        << "  --render-threads N     Encoder threads 0..256; 0 = automatic\n"
        << "  --render-keyframe-seconds N  GOP interval 0..30; 0 = encoder default\n"
        << "  --render-faststart / --no-render-faststart  MP4 metadata relocation\n"
        << "  --render-maximum-performance  Portable x264 ultrafast/zerolatency path\n"
        << "  --render-overwrite     Atomically replace an existing render\n"
        << "  --ffmpeg PATH          Trusted absolute ffmpeg/ffmpeg.exe path\n\n"
        << "Gameplay:\n"
        << "  --botplay --practice --no-fail --ghost-tapping --no-ghost-tapping\n"
        << "  --downscroll --middlescroll --mirror --random --hide-opponent\n"
        << "  --scroll-speed X --input-offset MS --visual-offset MS\n"
        << "  --audio-offset MS --latency-compensation MS --playback-rate X\n"
        << "  --audio-buffer FRAMES --fps N --vsync --no-vsync --fullscreen\n"
        << "  --low-quality --reduced-motion --no-splashes --no-flashing\n"
        << "  --max-visible-notes N --script-memory MiB --script-budget N\n"
        << "  --no-lua --safe-mode\n"
        << "  --replay PATH --save-replay PATH\n\n"
        << "Runtime keys: D/F/J/K or arrows, Esc pause, R restart, F2 botplay,\n"
        << "F1 pause/menu shortcut, F3 diagnostics, F5 Lua reload, F11 fullscreen,\n"
        << "+/- master volume and M mute. Bindings are configurable in Options.\n";
}

[[nodiscard]] int list_songs(const CommandLine& command) {
    pulseforge::ContentCatalogOptions options;
    options.roots = command.launch.content_roots;
    const auto catalog = pulseforge::ContentCatalog::scan(options);
    for (const auto& diagnostic : catalog.diagnostics()) {
        std::cerr << "WARNING: " << diagnostic.message << '\n';
    }
    for (const auto& entry : catalog.entries()) {
        const auto raw_path = entry.chart_path.generic_u8string();
        const std::string path(
            reinterpret_cast<const char*>(raw_path.data()),
            raw_path.size()
        );
        std::cout << entry.id << '\t' << entry.title << '\t'
                  << entry.difficulty << '\t'
                  << pulseforge::to_string(entry.layout) << '\t'
                  << path << '\n';
    }
    if (catalog.truncated()) {
        std::cerr << "WARNING: content catalog was truncated by safety limits\n";
    }
    return catalog.entries().empty() ? EXIT_FAILURE : EXIT_SUCCESS;
}

[[nodiscard]] int list_mods(const CommandLine& command) {
    pulseforge::ContentCatalogOptions options;
    options.roots = command.launch.content_roots;
    const auto catalog = pulseforge::ContentCatalog::scan(options);
    for (const auto& mod : catalog.mods()) {
        const auto raw_path = mod.root.generic_u8string();
        const std::string path(
            reinterpret_cast<const char*>(raw_path.data()),
            raw_path.size()
        );
        std::cout << mod.order << '\t'
                  << (mod.enabled ? "enabled" : "disabled") << '\t'
                  << mod.id << '\t' << mod.name << '\t'
                  << pulseforge::to_string(mod.profile) << '\t'
                  << path << '\n';
    }
    for (const auto& diagnostic : catalog.diagnostics()) {
        std::cerr << "Content warning: " << diagnostic.message << '\n';
    }
    return catalog.truncated() ? EXIT_FAILURE : EXIT_SUCCESS;
}


struct DoctorIssue final {
    std::string path;
    std::string message;
};

struct DoctorCount final {
    std::string name;
    std::uint64_t count{};
};

[[nodiscard]] std::string cli_path_utf8(const std::filesystem::path& path) {
    const auto raw = path.generic_u8string();
    return {
        reinterpret_cast<const char*>(raw.data()),
        raw.size(),
    };
}

[[nodiscard]] std::string cli_json_escape(const std::string_view value) {
    std::ostringstream output;
    for (const unsigned char c : value) {
        switch (c) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (c < 0x20U) {
                output << "\\u00"
                       << std::hex << std::setw(2) << std::setfill('0')
                       << static_cast<unsigned>(c)
                       << std::dec << std::setfill(' ');
            } else {
                output << static_cast<char>(c);
            }
            break;
        }
    }
    return output.str();
}

[[nodiscard]] std::string ascii_lower(std::string value) {
    for (auto& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return value;
}

void increment_doctor_count(
    std::vector<DoctorCount>& values,
    const std::string_view name
) {
    const auto found = std::find_if(
        values.begin(),
        values.end(),
        [&](const DoctorCount& value) {
            return value.name == name;
        }
    );
    if (found != values.end()) {
        ++found->count;
        return;
    }
    values.push_back({std::string{name}, 1U});
}

void append_unique_bounded(
    std::vector<std::string>& values,
    const std::string_view value,
    const std::size_t maximum
) {
    if (value.empty() || values.size() >= maximum) {
        return;
    }
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.emplace_back(value);
    }
}

[[nodiscard]] int doctor_mod(const CommandLine& command) {
    constexpr std::uintmax_t maximum_materialized_chart_bytes =
        128ULL * 1024ULL * 1024ULL;
    constexpr std::uint64_t maximum_inventory_files = 250'000U;
    constexpr std::size_t maximum_report_issues = 100U;
    constexpr std::size_t maximum_report_note_types = 128U;
    constexpr std::uint64_t doctor_stream_catchup_budget = 5'000'000U;

    if (!command.doctor_mod_root.has_value()) {
        std::cerr << "--doctor-mod requires a mod directory\n";
        return EXIT_FAILURE;
    }

    std::error_code error;
    auto root = std::filesystem::absolute(*command.doctor_mod_root, error);
    if (error) {
        error.clear();
        root = *command.doctor_mod_root;
    }
    root = root.lexically_normal();

    if (!std::filesystem::is_directory(root, error) || error) {
        std::cerr << "Mod doctor: directory not found: "
                  << cli_path_utf8(root) << '\n';
        return EXIT_FAILURE;
    }

    auto default_report_name = root.filename();
    default_report_name += "-doctor.json";
    auto json_path = command.doctor_json_output.value_or(
        std::filesystem::current_path()
            / "out" / "diagnostics" / "mod-doctor"
            / default_report_name
    );
    error.clear();
    std::filesystem::create_directories(json_path.parent_path(), error);
    if (error) {
        std::cerr << "Mod doctor: cannot create report directory: "
                  << error.message() << '\n';
        return EXIT_FAILURE;
    }

    auto probe_directory = json_path.parent_path() / ".streaming-probes";
    if (command.doctor_deep) {
        error.clear();
        std::filesystem::create_directories(probe_directory, error);
        if (error) {
            std::cerr << "Mod doctor: cannot create streaming probe directory: "
                      << error.message() << '\n';
            return EXIT_FAILURE;
        }
    }

    pulseforge::ContentCatalogOptions catalog_options;
    catalog_options.roots = {root};
    const auto catalog = pulseforge::ContentCatalog::scan(catalog_options);

    std::string profile{"unknown"};
    for (const auto& mod : catalog.mods()) {
        std::error_code mod_error;
        auto mod_root = std::filesystem::absolute(mod.root, mod_error);
        if (mod_error) {
            mod_root = mod.root;
        }
        if (mod_root.lexically_normal() == root) {
            profile = std::string{pulseforge::to_string(mod.profile)};
            break;
        }
    }

    std::vector<DoctorCount> layouts;
    for (const auto& entry : catalog.entries()) {
        increment_doctor_count(layouts, pulseforge::to_string(entry.layout));
    }
    if (profile == "unknown" && layouts.size() == 1U) {
        profile = layouts.front().name;
    } else if (profile == "unknown" && layouts.size() > 1U) {
        profile = "mixed";
    }

    std::uint64_t inventory_files = 0U;
    std::uint64_t audio_files = 0U;
    std::uint64_t video_files = 0U;
    std::uint64_t lua_files = 0U;
    std::uint64_t shader_files = 0U;
    bool inventory_truncated = false;

    std::filesystem::recursive_directory_iterator iterator(
        root,
        std::filesystem::directory_options::skip_permission_denied,
        error
    );
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        std::error_code type_error;
        if (iterator->is_regular_file(type_error) && !type_error) {
            ++inventory_files;
            auto extension = ascii_lower(
                cli_path_utf8(iterator->path().extension())
            );
            if (extension == ".ogg" || extension == ".wav"
                || extension == ".mp3" || extension == ".flac"
                || extension == ".opus") {
                ++audio_files;
            } else if (extension == ".mp4" || extension == ".webm"
                || extension == ".avi" || extension == ".mov"
                || extension == ".mkv") {
                ++video_files;
            } else if (extension == ".lua") {
                ++lua_files;
            } else if (extension == ".frag" || extension == ".vert"
                || extension == ".glsl" || extension == ".fs"
                || extension == ".vs") {
                ++shader_files;
            }
            if (inventory_files >= maximum_inventory_files) {
                inventory_truncated = true;
                break;
            }
        }
        iterator.increment(error);
    }
    if (error) {
        inventory_truncated = true;
    }

    std::uint64_t materialized_clean = 0U;
    std::uint64_t materialized_warnings = 0U;
    std::uint64_t streaming_ok = 0U;
    std::uint64_t failed_charts = 0U;
    std::uint64_t deferred_large_charts = 0U;
    std::uint64_t deferred_streaming_charts = 0U;
    std::uint64_t validation_warnings = 0U;
    std::uint64_t validation_errors = 0U;
    std::uint64_t materialized_notes = 0U;
    std::uint64_t materialized_events = 0U;
    std::uint64_t streaming_logical_notes = 0U;
    std::uint64_t streaming_explicit_notes = 0U;
    std::uint64_t streaming_updates = 0U;
    std::uint64_t peak_streaming_dynamic_bytes = 0U;
    std::vector<std::string> note_types;
    std::vector<DoctorIssue> issues;

    const auto probe_nonce = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    std::uint64_t probe_index = 0U;

    auto run_streaming_probe = [&](
        const std::filesystem::path& chart_path,
        std::string& probe_error,
        std::uint64_t& logical_notes,
        std::uint64_t& explicit_notes,
        std::uint64_t& updates,
        std::uint64_t& dynamic_bytes
    ) -> bool {
        ++probe_index;
        auto pfc_path = probe_directory
            / ("doctor-probe-" + std::to_string(probe_nonce)
               + "-" + std::to_string(probe_index) + ".pfc");
        struct ProbeCleanup final {
            std::filesystem::path path;
            ~ProbeCleanup() {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        } cleanup{pfc_path};

        const auto imported = pulseforge::compile_streaming_json_chart_to_pfc(
            chart_path,
            pfc_path
        );
        if (!imported) {
            probe_error = "streaming compile failed: " + imported.error;
            return false;
        }

        logical_notes = imported.logical_note_count;
        explicit_notes = imported.explicit_note_count;

        std::string open_error;
        const auto reader = pulseforge::PackedChartReader::open(
            pfc_path,
            &open_error
        );
        if (!reader.has_value()) {
            probe_error = "PFC1 open failed after streaming compile: " + open_error;
            return false;
        }

        auto gameplay = command.launch.settings.gameplay;
        gameplay.autoplay = true;
        gameplay.practice = true;
        gameplay.no_fail = true;

        pulseforge::StreamingGameplayOptions gameplay_options;
        gameplay_options.max_explicit_catchup_notes_per_update =
            doctor_stream_catchup_budget;

        auto tempos = imported.chart_metadata.tempos;
        if (tempos.empty()) {
            tempos.push_back({0.0, 120.0, 4U, 4U});
        }

        std::string session_error;
        auto session = pulseforge::StreamingGameplaySession::create(
            *reader,
            gameplay,
            gameplay_options,
            std::move(tempos),
            &session_error
        );
        if (!session.has_value()) {
            probe_error = "streaming gameplay session failed: " + session_error;
            return false;
        }

        const auto final_time_ms =
            static_cast<double>(imported.content_end_us) / 1'000.0 + 3'000.0;
        if (!std::isfinite(final_time_ms)) {
            probe_error = "streaming timeline cannot be represented";
            return false;
        }

        const auto maximum_updates = reader->explicit_note_count()
            / gameplay_options.max_explicit_catchup_notes_per_update + 4U;
        updates = 0U;
        do {
            session->begin_frame();
            if (!session->finish_song(final_time_ms)) {
                probe_error = "streaming gameplay failed: "
                    + std::string{session->error()};
                return false;
            }
            ++updates;
        } while (session->catchup_pending() && updates < maximum_updates);

        dynamic_bytes = static_cast<std::uint64_t>(
            session->memory_stats().approximate_dynamic_bytes
        );
        if (!session->healthy() || session->catchup_pending()
            || !session->complete()
            || session->total_resolved_notes() != reader->logical_note_count()) {
            probe_error = "streaming simulation did not reach a healthy complete state";
            if (!session->error().empty()) {
                probe_error += ": " + std::string{session->error()};
            }
            return false;
        }
        return true;
    };

    for (const auto& entry : catalog.entries()) {
        std::error_code size_error;
        const auto source_bytes = std::filesystem::file_size(
            entry.chart_path,
            size_error
        );
        const bool obviously_large =
            !size_error && source_bytes > maximum_materialized_chart_bytes;

        if (obviously_large) {
            if (!command.doctor_deep) {
                ++deferred_large_charts;
                continue;
            }

            std::string probe_error;
            std::uint64_t logical{};
            std::uint64_t explicit_count{};
            std::uint64_t updates{};
            std::uint64_t dynamic_bytes{};
            std::cout << "[Mod Doctor][streaming] "
                      << cli_path_utf8(entry.chart_path) << '\n';
            if (run_streaming_probe(
                    entry.chart_path,
                    probe_error,
                    logical,
                    explicit_count,
                    updates,
                    dynamic_bytes
                )) {
                ++streaming_ok;
                streaming_logical_notes += logical;
                streaming_explicit_notes += explicit_count;
                streaming_updates += updates;
                peak_streaming_dynamic_bytes = std::max(
                    peak_streaming_dynamic_bytes,
                    dynamic_bytes
                );
            } else {
                ++failed_charts;
                if (issues.size() < maximum_report_issues) {
                    issues.push_back({
                        cli_path_utf8(entry.chart_path),
                        "STREAMING FAILURE: " + probe_error,
                    });
                }
            }
            continue;
        }

        auto load_options = command.launch.chart_options;
        if (!entry.difficulty.empty()) {
            load_options.difficulty = entry.difficulty;
            load_options.difficulty_explicit = true;
        }
        if (entry.metadata_path.has_value()) {
            load_options.metadata_path = entry.metadata_path;
        }

        const auto loaded = pulseforge::ChartLoader::load(
            entry.chart_path,
            load_options
        );
        if (!loaded) {
            if (command.doctor_deep) {
                std::string probe_error;
                std::uint64_t logical{};
                std::uint64_t explicit_count{};
                std::uint64_t updates{};
                std::uint64_t dynamic_bytes{};
                std::cout << "[Mod Doctor][fallback streaming] "
                          << cli_path_utf8(entry.chart_path) << '\n';
                if (run_streaming_probe(
                        entry.chart_path,
                        probe_error,
                        logical,
                        explicit_count,
                        updates,
                        dynamic_bytes
                    )) {
                    ++streaming_ok;
                    streaming_logical_notes += logical;
                    streaming_explicit_notes += explicit_count;
                    streaming_updates += updates;
                    peak_streaming_dynamic_bytes = std::max(
                        peak_streaming_dynamic_bytes,
                        dynamic_bytes
                    );
                    if (issues.size() < maximum_report_issues) {
                        issues.push_back({
                            cli_path_utf8(entry.chart_path),
                            "STREAMING_OK after materialized rejection: "
                                + loaded.error,
                        });
                    }
                } else {
                    ++failed_charts;
                    if (issues.size() < maximum_report_issues) {
                        issues.push_back({
                            cli_path_utf8(entry.chart_path),
                            "MATERIALIZED: " + loaded.error
                                + " | STREAMING: " + probe_error,
                        });
                    }
                }
                continue;
            }

            const bool requires_streaming =
                loaded.error.find("5000000") != std::string::npos
                || loaded.error.find("too many notes") != std::string::npos
                || loaded.error.find("note limit") != std::string::npos
                || loaded.error.find("configured safety limits")
                    != std::string::npos
                || loaded.error.find("too many gameplay events")
                    != std::string::npos;
            if (requires_streaming) {
                ++deferred_streaming_charts;
                if (issues.size() < maximum_report_issues) {
                    issues.push_back({
                        cli_path_utf8(entry.chart_path),
                        "DEFERRED TO STREAMING: " + loaded.error,
                    });
                }
                continue;
            }
            ++failed_charts;
            if (issues.size() < maximum_report_issues) {
                issues.push_back({
                    cli_path_utf8(entry.chart_path),
                    loaded.error,
                });
            }
            continue;
        }

        materialized_notes += static_cast<std::uint64_t>(
            loaded.chart->notes.size()
        );
        materialized_events += static_cast<std::uint64_t>(
            loaded.chart->events.size()
        );
        for (const auto& note : loaded.chart->notes) {
            if (!note.kind.empty()) {
                append_unique_bounded(
                    note_types,
                    note.kind,
                    maximum_report_note_types
                );
            }
        }

        const auto chart_issues = pulseforge::validate_chart(*loaded.chart);
        std::uint64_t chart_errors = 0U;
        std::uint64_t chart_warnings = 0U;
        for (const auto& issue : chart_issues) {
            if (issue.severity == pulseforge::ValidationSeverity::error) {
                ++chart_errors;
                ++validation_errors;
            } else {
                ++chart_warnings;
                ++validation_warnings;
            }
            if (issues.size() < maximum_report_issues) {
                issues.push_back({
                    cli_path_utf8(entry.chart_path),
                    std::string{
                        issue.severity == pulseforge::ValidationSeverity::error
                            ? "ERROR: "
                            : "WARNING: "
                    } + issue.message,
                });
            }
        }

        if (chart_errors != 0U) {
            ++failed_charts;
        } else if (chart_warnings != 0U) {
            ++materialized_warnings;
        } else {
            ++materialized_clean;
        }
    }

    if (command.doctor_deep) {
        std::error_code cleanup_error;
        std::filesystem::remove(probe_directory, cleanup_error);
    }

    const auto catalog_diagnostic_count =
        static_cast<std::uint64_t>(catalog.diagnostics().size());
    for (const auto& diagnostic : catalog.diagnostics()) {
        if (issues.size() >= maximum_report_issues) {
            break;
        }
        issues.push_back({"<catalog>", diagnostic.message});
    }

    const auto total_charts =
        static_cast<std::uint64_t>(catalog.entries().size());
    const auto verified_supported =
        materialized_clean + materialized_warnings + streaming_ok;
    const auto verified_checked = verified_supported + failed_charts;
    const auto deferred_total =
        deferred_large_charts + deferred_streaming_charts;

    std::string verdict;
    if (total_charts == 0U) {
        verdict = "NO CHARTS DETECTED";
    } else if (failed_charts != 0U) {
        verdict = "INCOMPATIBILITIES DETECTED";
    } else if (deferred_total != 0U || catalog.truncated()
        || inventory_truncated || materialized_warnings != 0U
        || catalog_diagnostic_count != 0U) {
        verdict = command.doctor_deep
            ? "PLAYABLE / REVIEW WARNINGS"
            : "DEFERRED CHECKS REMAIN - RUN --doctor-deep";
    } else {
        verdict = command.doctor_deep ? "DEEP CLEAN" : "CLEAN";
    }

    const double verified_compatibility = verified_checked == 0U
        ? 0.0
        : 100.0 * static_cast<double>(verified_supported)
            / static_cast<double>(verified_checked);
    const double corpus_coverage = total_charts == 0U
        ? 0.0
        : 100.0 * static_cast<double>(verified_checked)
            / static_cast<double>(total_charts);

    auto temporary_json = json_path;
    temporary_json += ".tmp";
    std::ofstream json(temporary_json, std::ios::binary | std::ios::trunc);
    if (!json) {
        std::cerr << "Mod doctor: cannot create JSON report: "
                  << cli_path_utf8(temporary_json) << '\n';
        return EXIT_FAILURE;
    }

    json << "{\n"
         << "  \"schema\": \"pulseforge-mod-doctor-v2\",\n"
         << "  \"deep\": " << (command.doctor_deep ? "true" : "false") << ",\n"
         << "  \"root\": \"" << cli_json_escape(cli_path_utf8(root)) << "\",\n"
         << "  \"profile\": \"" << cli_json_escape(profile) << "\",\n"
         << "  \"verdict\": \"" << cli_json_escape(verdict) << "\",\n"
         << "  \"catalogTruncated\": "
         << (catalog.truncated() ? "true" : "false") << ",\n"
         << "  \"inventoryTruncated\": "
         << (inventory_truncated ? "true" : "false") << ",\n"
         << "  \"counts\": {\n"
         << "    \"charts\": " << total_charts << ",\n"
         << "    \"verifiedCheckedCharts\": " << verified_checked << ",\n"
         << "    \"materializedCleanCharts\": " << materialized_clean << ",\n"
         << "    \"materializedWarningCharts\": " << materialized_warnings << ",\n"
         << "    \"streamingOkCharts\": " << streaming_ok << ",\n"
         << "    \"failedCharts\": " << failed_charts << ",\n"
         << "    \"deferredLargeCharts\": " << deferred_large_charts << ",\n"
         << "    \"deferredStreamingCharts\": " << deferred_streaming_charts << ",\n"
         << "    \"validationWarnings\": " << validation_warnings << ",\n"
         << "    \"validationErrors\": " << validation_errors << ",\n"
         << "    \"materializedNotes\": " << materialized_notes << ",\n"
         << "    \"materializedEvents\": " << materialized_events << ",\n"
         << "    \"streamingLogicalNotes\": " << streaming_logical_notes << ",\n"
         << "    \"streamingExplicitNotes\": " << streaming_explicit_notes << ",\n"
         << "    \"streamingUpdates\": " << streaming_updates << ",\n"
         << "    \"peakStreamingDynamicBytes\": " << peak_streaming_dynamic_bytes << ",\n"
         << "    \"files\": " << inventory_files << ",\n"
         << "    \"audioFiles\": " << audio_files << ",\n"
         << "    \"videoFiles\": " << video_files << ",\n"
         << "    \"luaFiles\": " << lua_files << ",\n"
         << "    \"shaderFiles\": " << shader_files << ",\n"
         << "    \"catalogDiagnostics\": " << catalog_diagnostic_count << "\n"
         << "  },\n"
         << "  \"verifiedCompatibilityPercent\": "
         << std::fixed << std::setprecision(2) << verified_compatibility << ",\n"
         << "  \"corpusCoveragePercent\": "
         << std::fixed << std::setprecision(2) << corpus_coverage << ",\n"
         << "  \"layouts\": [";
    for (std::size_t index = 0U; index < layouts.size(); ++index) {
        if (index != 0U) {
            json << ',';
        }
        json << "\n    {\"name\": \""
             << cli_json_escape(layouts[index].name)
             << "\", \"count\": " << layouts[index].count << "}";
    }
    if (!layouts.empty()) {
        json << '\n';
    }
    json << "  ],\n"
         << "  \"noteTypes\": [";
    for (std::size_t index = 0U; index < note_types.size(); ++index) {
        if (index != 0U) {
            json << ", ";
        }
        json << '"' << cli_json_escape(note_types[index]) << '"';
    }
    json << "],\n"
         << "  \"issues\": [";
    for (std::size_t index = 0U; index < issues.size(); ++index) {
        if (index != 0U) {
            json << ',';
        }
        json << "\n    {\"path\": \""
             << cli_json_escape(issues[index].path)
             << "\", \"message\": \""
             << cli_json_escape(issues[index].message)
             << "\"}";
    }
    if (!issues.empty()) {
        json << '\n';
    }
    json << "  ]\n"
         << "}\n";
    json.close();
    if (!json) {
        std::cerr << "Mod doctor: failed while writing JSON report\n";
        return EXIT_FAILURE;
    }

    error.clear();
    std::filesystem::remove(json_path, error);
    error.clear();
    std::filesystem::rename(temporary_json, json_path, error);
    if (error) {
        std::cerr << "Mod doctor: cannot promote JSON report: "
                  << error.message() << '\n';
        return EXIT_FAILURE;
    }

    std::cout
        << "PULSEFORGE MOD COMPATIBILITY REPORT v2\n"
        << "======================================\n"
        << "Mode: " << (command.doctor_deep ? "DEEP STREAMING" : "QUICK") << '\n'
        << "Root: " << cli_path_utf8(root) << '\n'
        << "Detected profile/layout: " << profile << '\n'
        << "Charts detected: " << total_charts << '\n'
        << "  materialized clean: " << materialized_clean << '\n'
        << "  materialized warnings: " << materialized_warnings << '\n'
        << "  streaming verified: " << streaming_ok << '\n'
        << "  genuine failures: " << failed_charts << '\n'
        << "  deferred large: " << deferred_large_charts << '\n'
        << "  deferred streaming: " << deferred_streaming_charts << '\n'
        << "Verified compatibility: " << std::fixed << std::setprecision(2)
        << verified_compatibility << "%\n"
        << "Corpus coverage: " << std::fixed << std::setprecision(2)
        << corpus_coverage << "%\n"
        << "Materialized notes/events: "
        << materialized_notes << " / " << materialized_events << '\n'
        << "Streaming logical/explicit notes: "
        << streaming_logical_notes << " / " << streaming_explicit_notes << '\n'
        << "Streaming updates: " << streaming_updates << '\n'
        << "Peak streaming dynamic bytes: "
        << peak_streaming_dynamic_bytes << '\n'
        << "Files: " << inventory_files
        << "  audio=" << audio_files
        << "  video=" << video_files
        << "  lua=" << lua_files
        << "  shaders=" << shader_files << '\n'
        << "Catalog diagnostics: " << catalog_diagnostic_count << '\n'
        << "Verdict: " << verdict << '\n'
        << "JSON: " << cli_path_utf8(json_path) << '\n';

    if (!issues.empty()) {
        std::cout << "\nCompatibility notes/issues (max "
                  << maximum_report_issues << "):\n";
        for (const auto& issue : issues) {
            std::cout << "  [" << issue.path << "] " << issue.message << '\n';
        }
    }

    if (total_charts == 0U || failed_charts != 0U || catalog.truncated()) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

void print_pfc_statistics(const pulseforge::PackedChartStatistics& statistics) {
    std::cout << "Keys: " << statistics.key_count << '\n'
              << "Kinds: " << statistics.kind_count << '\n'
              << "Explicit notes: " << statistics.explicit_note_count << '\n'
              << "Logical notes: " << statistics.logical_note_count << '\n'
              << "Pattern runs: " << statistics.pattern_run_count << '\n'
              << "Pattern notes: " << statistics.pattern_note_count << '\n'
              << "Chunks: " << statistics.chunk_count << '\n';
    if (statistics.first_explicit_time_us.has_value()) {
        std::cout << "Explicit time range: "
                  << *statistics.first_explicit_time_us << ".."
                  << *statistics.last_explicit_time_us << " us\n";
    }
    if (statistics.explicit_notes_scanned) {
        std::cout << "Player/opponent: " << statistics.player_note_count
                  << '/' << statistics.opponent_note_count << '\n'
                  << "Holds: " << statistics.hold_note_count << '\n'
                  << "Non-zero flags: "
                  << statistics.nonzero_flag_note_count << '\n';
    }
}

[[nodiscard]] int compile_pfc(const CommandLine& command) {
    if (!command.compile_pfc_output.has_value()) {
        return EXIT_FAILURE;
    }
    if (command.launch.chart_path.empty()) {
        std::cerr << "--compile-pfc requires --chart INPUT (or a positional chart)\n";
        return EXIT_FAILURE;
    }
    if (command.streaming_pfc_compile) {
        pulseforge::StreamingChartImportOptions options;
        options.notes_per_pfc_chunk = command.pfc_chunk_notes;
        const auto result = pulseforge::compile_streaming_json_chart_to_pfc(
            command.launch.chart_path,
            *command.compile_pfc_output,
            options
        );
        if (!result) {
            std::cerr << "Streaming PFC1 compilation failed: "
                      << result.error << '\n';
            return EXIT_FAILURE;
        }
        std::string verification_error;
        const auto verified = pulseforge::PackedChartReader::open(
            *command.compile_pfc_output,
            &verification_error
        );
        if (!verified.has_value()) {
            std::cerr << "PFC1 post-write verification failed: "
                      << verification_error << '\n';
            return EXIT_FAILURE;
        }
        std::cout << "Streamed PFC1 note cache created: "
                  << command.compile_pfc_output->string() << '\n'
                  << "Source bytes: " << result.source_bytes << '\n'
                  << "Format: " << pulseforge::to_string(result.source_format) << '\n'
                  << "Keys: " << result.key_count << '\n'
                  << "Explicit notes: " << result.explicit_note_count << '\n'
                  << "Logical notes: " << result.logical_note_count << '\n'
                  << "Pattern runs: " << verified->patterns().size() << '\n'
                  << "Skipped entries: " << result.skipped_entry_count << '\n'
                  << "Psych sections: " << result.section_count << '\n'
                  << "Chunks: " << result.pfc_chunk_count << '\n'
                  << "Peak buffered notes: " << result.peak_buffered_notes << '\n'
                  << "External sort: "
                  << (result.used_external_sort ? "yes" : "no") << '\n'
                  << "The source JSON remains authoritative for tempo, events, audio and metadata.\n";
        return EXIT_SUCCESS;
    }
    const auto loaded = pulseforge::ChartLoader::load(
        command.launch.chart_path,
        command.launch.chart_options
    );
    if (!loaded) {
        std::cerr << loaded.error << '\n';
        return EXIT_FAILURE;
    }
    pulseforge::ChartToPackedOptions bridge_options;
    if (command.discard_pfc_payloads) {
        bridge_options.payload_policy = pulseforge::ChartPayloadPolicy::discard;
    }
    const auto converted = pulseforge::convert_chart_to_packed(
        *loaded.chart,
        bridge_options
    );
    if (!converted) {
        std::cerr << "PFC1 conversion failed: " << converted.error << '\n';
        return EXIT_FAILURE;
    }
    pulseforge::PackedChartWriteOptions write_options;
    write_options.max_notes_per_chunk = command.pfc_chunk_notes;
    std::string error;
    if (!pulseforge::write_packed_chart(
            *command.compile_pfc_output,
            converted.chart,
            write_options,
            &error
        )) {
        std::cerr << "PFC1 write failed: " << error << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "PFC1 note cache created: "
              << command.compile_pfc_output->string() << '\n';
    const auto verified_reader = pulseforge::PackedChartReader::open(
        *command.compile_pfc_output,
        &error
    );
    if (!verified_reader.has_value()) {
        std::cerr << "PFC1 post-write verification failed: " << error << '\n';
        return EXIT_FAILURE;
    }
    pulseforge::PackedChartInspectionOptions inspection_options;
    inspection_options.scan_explicit_notes = true;
    inspection_options.max_explicit_notes_to_scan =
        converted.statistics.explicit_note_count;
    const auto verified = pulseforge::inspect_packed_chart(
        *verified_reader,
        inspection_options
    );
    if (!verified) {
        std::cerr << "PFC1 post-write verification failed: "
                  << verified.error << '\n';
        return EXIT_FAILURE;
    }
    print_pfc_statistics(verified.statistics);
    std::cout << "The source JSON remains authoritative for tempo, events, audio and metadata.\n";
    return EXIT_SUCCESS;
}

[[nodiscard]] int install_mod_package(const CommandLine& command) {
    if (!command.install_mod_source.has_value()) {
        return EXIT_FAILURE;
    }
    const auto destination = command.mods_root.value_or(
        std::filesystem::current_path() / "mods"
    );
    const auto result = pulseforge::install_mod(
        *command.install_mod_source,
        destination
    );
    if (!result) {
        std::cerr << "Mod installation rejected: " << result.error << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "Installed mod: " << result.mod_id << '\n'
              << "Path: " << result.installed_path.string() << '\n'
              << "Files: " << result.installed_files << '\n'
              << "Bytes: " << result.installed_bytes << '\n';
    return EXIT_SUCCESS;
}

[[nodiscard]] int inspect_pfc(const CommandLine& command) {
    std::string error;
    const auto reader = pulseforge::PackedChartReader::open(
        *command.inspect_pfc_path,
        &error
    );
    if (!reader.has_value()) {
        std::cerr << "PFC1 open failed: " << error << '\n';
        return EXIT_FAILURE;
    }
    pulseforge::PackedChartInspectionOptions options;
    options.scan_explicit_notes = command.inspect_pfc_notes;
    if (command.inspect_pfc_notes) {
        options.max_explicit_notes_to_scan = 100'000'000ULL;
    }
    const auto inspection = pulseforge::inspect_packed_chart(*reader, options);
    if (!inspection) {
        std::cerr << "PFC1 inspection failed: " << inspection.error << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "Valid PFC1 cache: " << command.inspect_pfc_path->string() << '\n';
    print_pfc_statistics(inspection.statistics);
    return EXIT_SUCCESS;
}

[[nodiscard]] int simulate_pfc(const CommandLine& command) {
    if (!command.simulate_pfc_path.has_value()) {
        return EXIT_FAILURE;
    }
    std::string error;
    const auto reader = pulseforge::PackedChartReader::open(
        *command.simulate_pfc_path,
        &error
    );
    if (!reader.has_value()) {
        std::cerr << "PFC1 open failed: " << error << '\n';
        return EXIT_FAILURE;
    }

    long double final_time_us = 0.0L;
    if (reader->chunk_count() > 0U) {
        const auto final_chunk = reader->read_chunk(reader->chunk_count() - 1U);
        if (!final_chunk) {
            std::cerr << "PFC1 final chunk failed: " << final_chunk.error << '\n';
            return EXIT_FAILURE;
        }
        for (const auto& note : final_chunk.notes) {
            final_time_us = (std::max)(
                final_time_us,
                static_cast<long double>(note.time_us)
                    + static_cast<long double>(note.duration_us)
            );
        }
    }
    for (const auto& pattern : reader->patterns()) {
        if (pattern.count == 0U) {
            continue;
        }
        const auto last = static_cast<long double>(pattern.start_us)
            + static_cast<long double>(pattern.count - 1U)
                * static_cast<long double>(pattern.interval_us)
            + static_cast<long double>(pattern.duration_us);
        final_time_us = (std::max)(final_time_us, last);
    }
    const auto final_time_ms = static_cast<double>(final_time_us / 1'000.0L)
        + 3'000.0;
    if (!std::isfinite(final_time_ms)) {
        std::cerr << "PFC1 timeline cannot be represented by this simulator\n";
        return EXIT_FAILURE;
    }

    auto gameplay = command.launch.settings.gameplay;
    gameplay.autoplay = true;
    gameplay.practice = true;
    gameplay.no_fail = true;
    pulseforge::StreamingGameplayOptions options;
    options.max_explicit_catchup_notes_per_update = 5'000'000U;
    auto session = pulseforge::StreamingGameplaySession::create(
        *reader,
        gameplay,
        options,
        {{0.0, 120.0, 4U, 4U}},
        &error
    );
    if (!session.has_value()) {
        std::cerr << "PFC1 gameplay session failed: " << error << '\n';
        return EXIT_FAILURE;
    }

    const auto maximum_updates = reader->explicit_note_count()
        / options.max_explicit_catchup_notes_per_update + 4U;
    std::uint64_t updates = 0U;
    do {
        session->begin_frame();
        if (!session->finish_song(final_time_ms)) {
            std::cerr << "PFC1 gameplay failed: " << session->error() << '\n';
            return EXIT_FAILURE;
        }
        ++updates;
    } while (session->catchup_pending() && updates < maximum_updates);

    const auto& summary = session->summary();
    const auto memory = session->memory_stats();
    std::cout << "PFC1 bounded gameplay simulation: "
              << command.simulate_pfc_path->string() << '\n'
              << "Logical notes: " << reader->logical_note_count() << '\n'
              << "Resolved notes: " << session->total_resolved_notes() << '\n'
              << "Updates: " << updates << '\n'
              << "Score: " << summary.score << '\n'
              << "Misses: " << summary.misses << '\n'
              << "Accuracy: " << std::fixed << std::setprecision(2)
              << summary.accuracy_percent() << "%\n"
              << "Approximate dynamic bytes: "
              << memory.approximate_dynamic_bytes << '\n'
              << "Complete: " << (session->complete() ? "yes" : "no") << '\n';
    if (!session->healthy() || session->catchup_pending()
        || !session->complete()) {
        std::cerr << "PFC1 simulation did not reach a healthy complete state: "
                  << session->error() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

[[nodiscard]] int generate_pattern_pfc(const CommandLine& command) {
    if (!command.generate_pattern_pfc_output.has_value()
        || command.pattern_note_count == 0U
        || command.pattern_interval_us == 0U
        || command.pattern_lanes.empty()) {
        std::cerr << "Pattern generation requires an output, --pattern-notes > 0, "
                     "a positive interval and at least one lane\n";
        return EXIT_FAILURE;
    }
    const auto highest_lane = *std::max_element(
        command.pattern_lanes.begin(),
        command.pattern_lanes.end()
    );
    pulseforge::PackedChartData chart;
    chart.key_count = static_cast<std::uint16_t>(highest_lane + 1U);
    chart.kinds = {"normal"};
    pulseforge::PatternRun run;
    run.interval_us = command.pattern_interval_us;
    run.count = command.pattern_note_count;
    run.lane_pattern = command.pattern_lanes;
    run.owner = pulseforge::PackedNoteOwner::player;
    chart.patterns.push_back(std::move(run));

    std::string error;
    if (!pulseforge::write_packed_chart(
            *command.generate_pattern_pfc_output,
            chart,
            {},
            &error
        )) {
        std::cerr << "PFC1 pattern write failed: " << error << '\n';
        return EXIT_FAILURE;
    }
    const auto reader = pulseforge::PackedChartReader::open(
        *command.generate_pattern_pfc_output,
        &error
    );
    if (!reader.has_value()) {
        std::cerr << "PFC1 pattern verification failed: " << error << '\n';
        return EXIT_FAILURE;
    }
    const auto inspection = pulseforge::inspect_packed_chart(*reader);
    if (!inspection) {
        std::cerr << "PFC1 pattern verification failed: "
                  << inspection.error << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "PFC1 PatternRun created without expansion: "
              << command.generate_pattern_pfc_output->string() << '\n';
    print_pfc_statistics(inspection.statistics);
    return EXIT_SUCCESS;
}

[[nodiscard]] int validate_chart(const CommandLine& command) {
    const auto result = pulseforge::ChartLoader::load(
        command.launch.chart_path,
        command.launch.chart_options
    );
    if (!result) {
        std::cerr << result.error << '\n';
        return EXIT_FAILURE;
    }
    const auto issues = pulseforge::validate_chart(*result.chart);
    std::cout << "Valid chart: " << result.chart->title << '\n'
              << "Format: " << pulseforge::to_string(result.chart->source_format) << '\n'
              << "Difficulty: " << result.chart->difficulty << '\n'
              << "Keys: " << result.chart->key_count << '\n'
              << "Notes: " << result.chart->notes.size() << '\n'
              << "Events: " << result.chart->events.size() << '\n'
              << "Tempo segments: " << result.chart->tempos.size() << '\n'
              << "Duration: " << std::fixed << std::setprecision(2)
              << result.chart->duration_ms() / 1'000.0 << " s\n"
              << "Fingerprint: " << pulseforge::chart_fingerprint(*result.chart) << '\n';
    for (const auto& issue : issues) {
        std::cout << (issue.severity == pulseforge::ValidationSeverity::error
                          ? "ERROR"
                          : "WARNING")
                  << ": " << issue.message << " (item " << issue.item_index << ")\n";
    }
    return EXIT_SUCCESS;
}

[[nodiscard]] int benchmark_chart(const CommandLine& command) {
    using Clock = std::chrono::steady_clock;
    std::vector<double> load_times;
    std::vector<double> scheduler_times;
    load_times.reserve(command.benchmark_iterations);
    scheduler_times.reserve(command.benchmark_iterations);
    std::size_t note_count = 0;

    for (std::uint32_t iteration = 0;
         iteration < command.benchmark_iterations;
         ++iteration) {
        const auto load_start = Clock::now();
        auto result = pulseforge::ChartLoader::load(
            command.launch.chart_path,
            command.launch.chart_options
        );
        const auto load_end = Clock::now();
        if (!result) {
            std::cerr << result.error << '\n';
            return EXIT_FAILURE;
        }
        load_times.push_back(
            std::chrono::duration<double, std::milli>(load_end - load_start).count()
        );
        note_count = result.chart->notes.size();

        auto gameplay = command.launch.settings.gameplay;
        gameplay.autoplay = true;
        gameplay.practice = true;
        pulseforge::GameplaySession session(*result.chart, gameplay);
        const auto scheduler_start = Clock::now();
        session.finish_song(result.chart->duration_ms());
        const auto scheduler_end = Clock::now();
        scheduler_times.push_back(
            std::chrono::duration<double, std::milli>(
                scheduler_end - scheduler_start
            ).count()
        );
        if (!session.complete()) {
            std::cerr << "benchmark invariant failed: scheduler skipped notes\n";
            return EXIT_FAILURE;
        }
    }

    auto report = [](std::string_view name, std::vector<double> samples) {
        std::sort(samples.begin(), samples.end());
        const auto percentile = [&](const double position) {
            const auto index = static_cast<std::size_t>(
                position * static_cast<double>(samples.size() - 1)
            );
            return samples[index];
        };
        std::cout << name << ": p50=" << percentile(0.50)
                  << " ms, p95=" << percentile(0.95)
                  << " ms, max=" << samples.back() << " ms\n";
    };

    std::cout << "Benchmark: " << command.launch.chart_path.string() << '\n'
              << "Notes: " << note_count
              << ", iterations: " << command.benchmark_iterations << '\n';
    report("Parse+validate", std::move(load_times));
    report("Judge all (BOTPLAY)", std::move(scheduler_times));
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
    install_cli_crash_diagnostics();
    cli_set_stage("locating assets");
    try {
        const auto assets = locate_assets(argv[0]);
        cli_trace("BOOT", std::string("assets=") + assets.string());
        cli_set_stage("parsing command line");
        const auto runtime_paths = default_runtime_paths(assets);
        auto command = parse_command_line(argc, argv, assets, runtime_paths);
        cli_pause_on_crash = !command.no_crash_pause;
        const auto executable_directory =
            std::filesystem::absolute(argv[0]).parent_path();
        const std::array extra_content_roots{
            std::filesystem::current_path() / "mods",
            executable_directory / "mods",
            executable_directory.parent_path() / "mods",
        };
        for (const auto& root : extra_content_roots) {
            if (std::filesystem::is_directory(root)
                && std::find(
                    command.launch.content_roots.begin(),
                    command.launch.content_roots.end(),
                    root
                ) == command.launch.content_roots.end()) {
                command.launch.content_roots.push_back(root);
            }
        }
        if (std::filesystem::is_directory(runtime_paths.mods)
            && std::find(
                command.launch.content_roots.begin(),
                command.launch.content_roots.end(),
                runtime_paths.mods
            ) == command.launch.content_roots.end()) {
            command.launch.content_roots.push_back(runtime_paths.mods);
        }
        if (const auto configured = environment_value("PULSEFORGE_MOD_ROOT")) {
            const std::filesystem::path root(*configured);
            if (std::filesystem::is_directory(root)
                && std::find(
                    command.launch.content_roots.begin(),
                    command.launch.content_roots.end(),
                    root
                ) == command.launch.content_roots.end()) {
                command.launch.content_roots.push_back(root);
            }
        }
        if (command.show_help) {
            print_help();
            return EXIT_SUCCESS;
        }

        command.launch.settings_path = command.settings_path;
        cli_set_stage("loading settings");
        if (const auto loaded = pulseforge::load_settings(command.settings_path); loaded) {
            command.launch.settings = *loaded.settings;
            // Explicit command-line switches are parsed again below because a file
            // should not silently override the user's launch request.
            for (int index = 1; index < argc; ++index) {
                const std::string_view option(argv[index]);
                if (option == "--botplay") {
                    command.launch.settings.gameplay.autoplay = true;
                } else if (option == "--practice") {
                    command.launch.settings.gameplay.practice = true;
                } else if (option == "--no-fail") {
                    command.launch.settings.gameplay.no_fail = true;
                } else if (option == "--ghost-tapping") {
                    command.launch.settings.gameplay.ghost_tapping = true;
                } else if (option == "--no-ghost-tapping") {
                    command.launch.settings.gameplay.ghost_tapping = false;
                } else if (option == "--downscroll") {
                    command.launch.settings.gameplay.downscroll = true;
                } else if (option == "--middlescroll") {
                    command.launch.settings.gameplay.middle_scroll = true;
                } else if (option == "--mirror") {
                    command.launch.settings.gameplay.mirror = true;
                } else if (option == "--random") {
                    command.launch.settings.gameplay.randomize_lanes = true;
                } else if (option == "--hide-opponent") {
                    command.launch.settings.gameplay.hide_opponent_notes = true;
                } else if (option == "--scroll-speed" && index + 1 < argc) {
                    command.launch.settings.gameplay.scroll_speed =
                        std::stod(argv[++index]);
                } else if (option == "--input-offset" && index + 1 < argc) {
                    command.launch.settings.gameplay.input_offset_ms =
                        std::stod(argv[++index]);
                } else if (option == "--visual-offset" && index + 1 < argc) {
                    command.launch.settings.gameplay.visual_offset_ms =
                        std::stod(argv[++index]);
                } else if (option == "--audio-offset" && index + 1 < argc) {
                    command.launch.settings.audio.audio_offset_ms =
                        std::stod(argv[++index]);
                } else if (option == "--latency-compensation"
                    && index + 1 < argc) {
                    command.launch.settings.audio.output_latency_compensation_ms =
                        std::stod(argv[++index]);
                } else if (option == "--playback-rate" && index + 1 < argc) {
                    command.launch.settings.audio.playback_rate =
                        std::stod(argv[++index]);
                } else if (option == "--audio-buffer" && index + 1 < argc) {
                    command.launch.settings.audio.buffer_frames =
                        static_cast<std::uint32_t>(std::stoul(argv[++index]));
                } else if (option == "--low-quality") {
                    command.launch.settings.visual.low_quality = true;
                } else if (option == "--reduced-motion") {
                    command.launch.settings.visual.reduced_motion = true;
                } else if (option == "--no-splashes") {
                    command.launch.settings.visual.note_splashes = false;
                } else if (option == "--no-flashing") {
                    command.launch.settings.visual.flashing_lights = false;
                } else if (option == "--fullscreen") {
                    command.launch.settings.visual.fullscreen = true;
                } else if (option == "--max-visible-notes" && index + 1 < argc) {
                    command.launch.settings.performance.max_visible_notes =
                        static_cast<std::uint32_t>(std::stoul(argv[++index]));
                } else if (option == "--script-memory" && index + 1 < argc) {
                    command.launch.settings.performance.script_memory_mb =
                        static_cast<std::uint32_t>(std::stoul(argv[++index]));
                } else if (option == "--script-budget" && index + 1 < argc) {
                    command.launch.settings.performance.script_instruction_budget =
                        static_cast<std::uint32_t>(std::stoul(argv[++index]));
                } else if (option == "--vsync") {
                    command.launch.settings.visual.vsync = true;
                } else if (option == "--no-vsync") {
                    command.launch.settings.visual.vsync = false;
                } else if (option == "--fps" && index + 1 < argc) {
                    command.launch.settings.visual.fps_cap = std::stoi(argv[++index]);
                } else if (option == "--smoke-test") {
                    command.launch.settings.gameplay.autoplay = true;
                }
            }
        } else {
            std::cerr << "Settings warning: " << loaded.error
                      << " (using safe defaults)\n";
        }

        if (command.launch.safe_mode) {
            command.launch.settings.visual.low_quality = true;
            command.launch.settings.visual.reduced_motion = true;
            command.launch.settings.visual.note_splashes = false;
            command.launch.settings.visual.flashing_lights = false;
            command.launch.settings.performance.hot_reload_scripts = false;
        }
        command.launch.settings.visual.fps_cap = std::clamp(
            command.launch.settings.visual.fps_cap,
            0,
            1'000
        );
        command.launch.settings.audio.buffer_frames = std::clamp<std::uint32_t>(
            command.launch.settings.audio.buffer_frames,
            64U,
            8'192U
        );
        command.launch.settings.performance.max_visible_notes =
            std::clamp<std::uint32_t>(
                command.launch.settings.performance.max_visible_notes,
                128U,
                1'000'000U
            );
        command.launch.settings.performance.script_memory_mb =
            std::clamp<std::uint32_t>(
                command.launch.settings.performance.script_memory_mb,
                1U,
                256U
            );
        command.launch.settings.performance.script_instruction_budget =
            std::clamp<std::uint32_t>(
                command.launch.settings.performance.script_instruction_budget,
                10'000U,
                100'000'000U
            );
        if (command.validate) {
            cli_set_stage("validating chart");
            return validate_chart(command);
        }
        if (command.benchmark) {
            cli_set_stage("benchmarking chart");
            return benchmark_chart(command);
        }
        if (command.list_songs) {
            return list_songs(command);
        }
        if (command.list_mods) {
            return list_mods(command);
        }
        if (command.doctor_mod_root.has_value()) {
            return doctor_mod(command);
        }
        if (command.install_mod_source.has_value()) {
            return install_mod_package(command);
        }
        if (command.compile_pfc_output.has_value()) {
            cli_set_stage("compiling PFC1");
            return compile_pfc(command);
        }
        if (command.inspect_pfc_path.has_value()) {
            return inspect_pfc(command);
        }
        if (command.simulate_pfc_path.has_value()) {
            return simulate_pfc(command);
        }
        if (command.generate_pattern_pfc_output.has_value()) {
            return generate_pattern_pfc(command);
        }
        cli_set_stage("constructing CoreEngine");
        pulseforge::CoreEngine engine(std::move(command.launch));
        cli_set_stage("running CoreEngine");
        const int result = engine.run();
        cli_trace("EXIT", std::string("CoreEngine returned ") + std::to_string(result));
        return result;
    } catch (const std::exception& exception) {
        std::ostringstream detail;
        detail << "caught std::exception | stage=" << cli_current_stage
               << " | type=" << typeid(exception).name()
               << " | what=" << exception.what();
        cli_trace("CRASH", detail.str());
        cli_pause_for_crash();
        return EXIT_FAILURE;
    } catch (...) {
        cli_trace(
            "CRASH",
            std::string("caught unknown exception | stage=") + cli_current_stage
        );
        cli_pause_for_crash();
        return EXIT_FAILURE;
    }
}
