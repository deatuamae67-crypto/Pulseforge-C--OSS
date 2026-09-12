from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    file = Path(path)
    text = file.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, got {count}: {old[:120]!r}")
    file.write_text(text.replace(old, new, 1))


# Build JNI bridge.
replace_once(
    "CMakeLists.txt",
    "        src/app/application.cpp\n        src/app/controls_ui.cpp\n",
    "        src/app/application.cpp\n        src/app/android_runtime_bridge.cpp\n        src/app/controls_ui.cpp\n",
)

# Persistent global Lua switch.
replace_once(
    "include/pulseforge/settings.hpp",
    "    std::uint32_t script_instruction_budget{1'000'000};\n    bool hot_reload_scripts{true};\n",
    "    std::uint32_t script_instruction_budget{1'000'000};\n    bool lua_enabled{true};\n    bool hot_reload_scripts{true};\n",
)
replace_once(
    "src/io/settings_loader.cpp",
    "        settings.performance.hot_reload_scripts = value_or(\n            performance,\n            \"hotReloadScripts\",\n            settings.performance.hot_reload_scripts\n        );\n",
    "        settings.performance.lua_enabled = value_or(\n            performance,\n            \"luaEnabled\",\n            settings.performance.lua_enabled\n        );\n        settings.performance.hot_reload_scripts = value_or(\n            performance,\n            \"hotReloadScripts\",\n            settings.performance.hot_reload_scripts\n        );\n",
)
replace_once(
    "src/io/settings_loader.cpp",
    "                {\"scriptInstructionBudget\",\n                 settings.performance.script_instruction_budget},\n                {\"hotReloadScripts\", settings.performance.hot_reload_scripts},\n",
    "                {\"scriptInstructionBudget\",\n                 settings.performance.script_instruction_budget},\n                {\"luaEnabled\", settings.performance.lua_enabled},\n                {\"hotReloadScripts\", settings.performance.hot_reload_scripts},\n",
)

# Options row + shifted cases.
replace_once(
    "src/app/launcher.cpp",
    "            \"Ultra-low latency mode: \" + on_off(\n                performance.ultra_low_latency\n            ),\n            \"Audio visualizer opacity: \"\n",
    "            \"Ultra-low latency mode: \" + on_off(\n                performance.ultra_low_latency\n            ),\n            \"Lua scripts: \" + on_off(performance.lua_enabled),\n            \"Audio visualizer opacity: \"\n",
)
replace_once(
    "src/app/launcher.cpp",
    "        case 32: {\n            std::vector<std::string> opacity_choices;\n",
    "        case 32:\n            performance.lua_enabled = !performance.lua_enabled;\n            break;\n        case 33: {\n            std::vector<std::string> opacity_choices;\n",
)
for old, new in (
    (
        "        case 33: {\n            const std::vector<std::string> image_choices{",
        "        case 34: {\n            const std::vector<std::string> image_choices{",
    ),
    (
        "        case 34: {\n            std::vector<std::filesystem::path> discovery_roots =",
        "        case 35: {\n            std::vector<std::filesystem::path> discovery_roots =",
    ),
    (
        "        case 35:\n            show_discord_options(menu, options);",
        "        case 36:\n            show_discord_options(menu, options);",
    ),
    (
        "        case 36:\n            show_touch_options(menu, options);",
        "        case 37:\n            show_touch_options(menu, options);",
    ),
    (
        "        case 37:\n            show_controls_editor(",
        "        case 38:\n            show_controls_editor(",
    ),
):
    replace_once("src/app/launcher.cpp", old, new)

replace_once(
    "src/app/launcher.cpp",
    "    AppLaunchOptions result = base;\n    result.chart_path = entry.chart_path;\n",
    "    AppLaunchOptions result = base;\n    result.enable_lua = base.enable_lua\n        && base.settings.performance.lua_enabled;\n    result.chart_path = entry.chart_path;\n",
)
replace_once(
    "src/app/launcher.cpp",
    "            auto direct = options_;\n            const int result = make_gameplay_application(\n",
    "            auto direct = options_;\n            direct.enable_lua = direct.enable_lua\n                && direct.settings.performance.lua_enabled;\n            const int result = make_gameplay_application(\n",
)

# Touch controls: F3 + capture.
replace_once(
    "src/app/mobile_touch_controls.hpp",
    "    volume_up,\n    editor_play,\n",
    "    volume_up,\n    diagnostics_toggle,\n    performance_capture,\n    editor_play,\n",
)
replace_once(
    "src/app/mobile_touch_controls.cpp",
    "        {TouchAction::volume_up, SDL_SCANCODE_UNKNOWN, \"volume_up\"},\n        {TouchAction::editor_play, SDL_SCANCODE_UNKNOWN, \"editor_play_pause\"},\n",
    "        {TouchAction::volume_up, SDL_SCANCODE_UNKNOWN, \"volume_up\"},\n        {TouchAction::diagnostics_toggle, SDL_SCANCODE_F3, {}},\n        {TouchAction::performance_capture, SDL_SCANCODE_F4, {}},\n        {TouchAction::editor_play, SDL_SCANCODE_UNKNOWN, \"editor_play_pause\"},\n",
)
replace_once(
    "src/app/mobile_touch_controls.cpp",
    "        result.push_back({\n            TouchAction::pause,\n            {\n                safe.x + safe.w - pause_width - margin,\n                safe.y + margin,\n                pause_width,\n                pause_height,\n            },\n            {238, 188, 61, 255},\n            \"PAUSE\",\n        });\n\n        // PULSEFORGE_P1_1_4_ANDROID_PSYCH_RAW_KEY_CLUSTER_V1\n",
    "        result.push_back({\n            TouchAction::pause,\n            {\n                safe.x + safe.w - pause_width - margin,\n                safe.y + margin,\n                pause_width,\n                pause_height,\n            },\n            {238, 188, 61, 255},\n            \"PAUSE\",\n        });\n        const float diagnostics_width = std::clamp(64.0F * scale, 48.0F, 84.0F);\n        const float diagnostics_gap = std::clamp(6.0F * scale, 4.0F, 10.0F);\n        result.push_back({\n            TouchAction::diagnostics_toggle,\n            {\n                safe.x + safe.w - pause_width - margin\n                    - diagnostics_gap * 2.0F - diagnostics_width * 2.0F,\n                safe.y + margin, diagnostics_width, pause_height,\n            },\n            {92, 186, 238, 255},\n            \"F3\",\n        });\n        result.push_back({\n            TouchAction::performance_capture,\n            {\n                safe.x + safe.w - pause_width - margin\n                    - diagnostics_gap - diagnostics_width,\n                safe.y + margin, diagnostics_width, pause_height,\n            },\n            {103, 225, 150, 255},\n            \"CAP\",\n        });\n\n        // PULSEFORGE_P1_1_4_ANDROID_PSYCH_RAW_KEY_CLUSTER_V1\n",
)

# Android Java imports + runtime/Downloads bridge.
java_path = Path("platform/android/app/src/main/java/org/pulseforge/engine/PulseForgeActivity.java")
java = java_path.read_text()
anchor = "import android.content.Intent;\n"
imports = """import android.content.ContentValues;\nimport android.content.Intent;\nimport android.net.Uri;\nimport android.os.Build;\nimport android.os.Debug;\nimport android.os.Environment;\nimport android.provider.MediaStore;\n\nimport java.io.File;\nimport java.io.FileInputStream;\nimport java.io.FileOutputStream;\nimport java.io.InputStream;\nimport java.io.OutputStream;\nimport java.util.Map;\nimport java.util.TreeMap;\n"""
if java.count(anchor) != 1:
    raise SystemExit("PulseForgeActivity import anchor mismatch")
java = java.replace(anchor, imports, 1)
class_end = java.rfind("\n}")
if class_end < 0:
    raise SystemExit("PulseForgeActivity class end missing")
methods = r'''

    public String getPulseForgeRuntimeStats() {
        try {
            final Runtime runtime = Runtime.getRuntime();
            final StringBuilder out = new StringBuilder();
            out.append("android.manufacturer=").append(Build.MANUFACTURER).append('\n');
            out.append("android.model=").append(Build.MODEL).append('\n');
            out.append("android.device=").append(Build.DEVICE).append('\n');
            out.append("android.sdk=").append(Build.VERSION.SDK_INT).append('\n');
            out.append("java.heap.total_bytes=").append(runtime.totalMemory()).append('\n');
            out.append("java.heap.free_bytes=").append(runtime.freeMemory()).append('\n');
            out.append("java.heap.max_bytes=").append(runtime.maxMemory()).append('\n');
            out.append("native.heap.allocated_bytes=").append(Debug.getNativeHeapAllocatedSize()).append('\n');
            out.append("native.heap.free_bytes=").append(Debug.getNativeHeapFreeSize()).append('\n');
            out.append("native.heap.size_bytes=").append(Debug.getNativeHeapSize()).append('\n');
            out.append("process.pss_kib=").append(Debug.getPss()).append('\n');
            if (Build.VERSION.SDK_INT >= 23) {
                final Map<String, String> stats = new TreeMap<>(Debug.getRuntimeStats());
                for (final Map.Entry<String, String> entry : stats.entrySet()) {
                    final String key = entry.getKey();
                    if (key.startsWith("art.gc.") || key.startsWith("art.gc-")) {
                        out.append(key).append('=').append(entry.getValue()).append('\n');
                    }
                }
            }
            return out.toString();
        } catch (final Throwable throwable) {
            return "android.runtime_stats_error=" + throwable + "\n";
        }
    }

    public String publishPulseForgeDownload(
            final String sourcePath,
            final String displayName,
            final String mimeType) {
        if (sourcePath == null || displayName == null || displayName.isEmpty()) {
            return null;
        }
        final File source = new File(sourcePath);
        if (!source.isFile()) return null;
        try {
            if (Build.VERSION.SDK_INT >= 29) {
                final ContentValues values = new ContentValues();
                values.put(MediaStore.Downloads.DISPLAY_NAME, displayName);
                values.put(MediaStore.Downloads.MIME_TYPE,
                    mimeType == null || mimeType.isEmpty()
                        ? "application/octet-stream" : mimeType);
                values.put(MediaStore.Downloads.RELATIVE_PATH,
                    Environment.DIRECTORY_DOWNLOADS + "/PulseForge");
                values.put(MediaStore.Downloads.IS_PENDING, 1);
                final Uri uri = getContentResolver().insert(
                    MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);
                if (uri == null) return null;
                boolean complete = false;
                try (InputStream input = new FileInputStream(source);
                     OutputStream output = getContentResolver().openOutputStream(uri, "w")) {
                    if (output == null) return null;
                    final byte[] buffer = new byte[64 * 1024];
                    int count;
                    while ((count = input.read(buffer)) >= 0) {
                        if (count > 0) output.write(buffer, 0, count);
                    }
                    output.flush();
                    complete = true;
                } finally {
                    if (!complete) getContentResolver().delete(uri, null, null);
                }
                values.clear();
                values.put(MediaStore.Downloads.IS_PENDING, 0);
                getContentResolver().update(uri, values, null, null);
                return uri.toString();
            }
            final File downloads = new File(
                Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS),
                "PulseForge");
            if (!downloads.exists() && !downloads.mkdirs()) return null;
            final File target = new File(downloads, displayName);
            try (InputStream input = new FileInputStream(source);
                 OutputStream output = new FileOutputStream(target, false)) {
                final byte[] buffer = new byte[64 * 1024];
                int count;
                while ((count = input.read(buffer)) >= 0) {
                    if (count > 0) output.write(buffer, 0, count);
                }
                output.flush();
            }
            return target.getAbsolutePath();
        } catch (final Throwable throwable) {
            android.util.Log.e("PulseForge", "Downloads export failed", throwable);
            return null;
        }
    }
'''
java_path.write_text(java[:class_end] + methods + java[class_end:])

replace_once(
    "platform/android/app/src/main/AndroidManifest.xml",
    '    <uses-permission android:name="android.permission.INTERNET" />\n',
    '    <uses-permission android:name="android.permission.INTERNET" />\n    <uses-permission android:name="android.permission.WRITE_EXTERNAL_STORAGE" android:maxSdkVersion="28" />\n',
)

# Runtime profiler and capture.
replace_once(
    "src/app/application.cpp",
    '#include "application_runner.hpp"\n',
    '#include "application_runner.hpp"\n#include "android_runtime_bridge.hpp"\n',
)
replace_once(
    "src/app/application.cpp",
    "    struct NoteProfileFrame final {\n        std::uint64_t gameplay_update_ns{};\n        std::uint64_t note_pipeline_ns{};\n",
    "    struct NoteProfileFrame final {\n        std::uint64_t gameplay_update_ns{};\n        std::uint64_t lua_ns{};\n        std::uint64_t note_pipeline_ns{};\n",
)
replace_once(
    "src/app/application.cpp",
    "        note_profile_present_.reset_peak();\n        note_profile_gameplay_update_.reset_peak();\n",
    "        note_profile_present_.reset_peak();\n        note_profile_gameplay_update_.reset_peak();\n        note_profile_lua_.reset_peak();\n",
)
replace_once(
    "src/app/application.cpp",
    "    void sample_note_profile_frame() noexcept {\n        if (!diagnostics_) {\n            return;\n        }\n",
    "    [[nodiscard]] bool profiling_active() const noexcept {\n        return diagnostics_ || performance_capture_.active;\n    }\n\n    void sample_note_profile_frame() noexcept {\n        if (!profiling_active()) {\n            return;\n        }\n",
)
replace_once(
    "src/app/application.cpp",
    "        note_profile_gameplay_update_.sample(\n            note_profile_frame_.gameplay_update_ns\n        );\n        note_profile_note_.sample(note_cpu_ns);\n",
    "        note_profile_gameplay_update_.sample(\n            note_profile_frame_.gameplay_update_ns\n        );\n        note_profile_lua_.sample(note_profile_frame_.lua_ns);\n        note_profile_note_.sample(note_cpu_ns);\n",
)

capture_code = r'''

    struct PerformanceCaptureRow final {
        double elapsed_ms{};
        double wall_frame_ms{};
        double cpu_frame_us{};
        double gameplay_us{};
        double lua_us{};
        double note_us{};
        double cache_us{};
        double pvd_us{};
        double pfc_us{};
        double geometry_build_us{};
        double geometry_submit_us{};
        double fallback_us{};
        double present_us{};
        std::uint64_t rendered_notes{};
        std::uint64_t nps{};
        std::uint64_t draw_units{};
        std::uint64_t draw_calls{};
        std::uint64_t pvd_buckets{};
        std::uint64_t exact_visits{};
        std::uint64_t dropped_callbacks{};
        std::size_t window_notes{};
        std::uint64_t streaming_ram_bytes{};
        bool catchup{};
        bool saturated{};
    };

    struct PerformanceCaptureState final {
        bool active{};
        std::uint64_t started_ns{};
        std::uint64_t deadline_ns{};
        std::vector<PerformanceCaptureRow> rows;
        std::string runtime_start;
    };

    [[nodiscard]] std::filesystem::path performance_capture_directory() const {
        if (char* const pref = SDL_GetPrefPath("PulseForge", "PulseForge"); pref != nullptr) {
            const std::filesystem::path result =
                std::filesystem::path(pref) / "performance-captures";
            SDL_free(pref);
            return result;
        }
        std::error_code error;
        const auto temp = std::filesystem::temp_directory_path(error);
        return (error ? std::filesystem::current_path() : temp)
            / "PulseForge-performance-captures";
    }

    [[nodiscard]] static std::string capture_file_stem(std::string_view source) {
        std::string result;
        result.reserve(std::min<std::size_t>(source.size(), 48U));
        for (const unsigned char value : source) {
            if (result.size() >= 48U) break;
            const bool safe = (value >= 'a' && value <= 'z')
                || (value >= 'A' && value <= 'Z')
                || (value >= '0' && value <= '9')
                || value == '-' || value == '_';
            result.push_back(safe ? static_cast<char>(value) : '_');
        }
        return result.empty() ? std::string{"chart"} : result;
    }

    void start_performance_capture() {
        performance_capture_.active = true;
        performance_capture_.started_ns = SDL_GetTicksNS();
        performance_capture_.deadline_ns = performance_capture_.started_ns
            + 10'000'000'000ULL;
        performance_capture_.rows.clear();
        performance_capture_.rows.reserve(2'000U);
        performance_capture_.runtime_start = detail::android_runtime_diagnostics();
        performance_capture_message_ = "PERFORMANCE CAPTURE: recording 10 seconds";
        performance_capture_message_until_ns_ = performance_capture_.deadline_ns;
        reset_note_profile_peaks();
    }

    void sample_performance_capture(
        const double wall_frame_seconds,
        const std::uint64_t frame_start_ns
    ) {
        if (!performance_capture_.active) return;
        const auto now = SDL_GetTicksNS();
        if (performance_capture_.rows.size() < 20'000U) {
            PerformanceCaptureRow row;
            row.elapsed_ms = static_cast<double>(now - performance_capture_.started_ns)
                / 1'000'000.0;
            row.wall_frame_ms = std::max(0.0, wall_frame_seconds * 1'000.0);
            row.cpu_frame_us = static_cast<double>(now - frame_start_ns) / 1'000.0;
            row.gameplay_us = note_profile_gameplay_update_.last_us;
            row.lua_us = note_profile_lua_.last_us;
            row.note_us = note_profile_note_.last_us;
            row.cache_us = note_profile_cache_.last_us;
            row.pvd_us = note_profile_pvd_.last_us;
            row.pfc_us = note_profile_pfc_.last_us;
            row.geometry_build_us = note_profile_batch_build_.last_us;
            row.geometry_submit_us = note_profile_batch_submit_.last_us;
            row.fallback_us = note_profile_fallback_.last_us;
            row.present_us = note_profile_present_.last_us;
            row.rendered_notes = rendered_notes_;
            row.nps = current_nps_;
            row.draw_units = visual_draw_units_;
            row.draw_calls = visual_geometry_calls_;
            row.pvd_buckets = streaming_density_buckets_;
            row.exact_visits = streaming_explicit_visual_visits_;
            row.dropped_callbacks = dropped_gameplay_callbacks();
            if (streaming_mode()) {
                const auto memory = streaming_session_->memory_stats();
                row.window_notes = memory.window_notes;
                row.streaming_ram_bytes = memory.approximate_dynamic_bytes;
                row.catchup = streaming_session_->catchup_pending();
                row.saturated = streaming_session_->window_saturated();
            }
            performance_capture_.rows.push_back(row);
        }
        if (now >= performance_capture_.deadline_ns) {
            finish_performance_capture(false);
        }
    }

    void finish_performance_capture(const bool early) {
        if (!performance_capture_.active) return;
        performance_capture_.active = false;
        const auto finished_ns = SDL_GetTicksNS();
        const auto runtime_end = detail::android_runtime_diagnostics();
        const auto duration_ms = static_cast<double>(
            finished_ns - performance_capture_.started_ns
        ) / 1'000'000.0;
        double frame_ms_sum = 0.0;
        double frame_ms_max = 0.0;
        for (const auto& row : performance_capture_.rows) {
            frame_ms_sum += row.wall_frame_ms;
            frame_ms_max = std::max(frame_ms_max, row.wall_frame_ms);
        }
        const double average_frame_ms = performance_capture_.rows.empty()
            ? 0.0
            : frame_ms_sum / static_cast<double>(performance_capture_.rows.size());
        const double measured_fps = duration_ms > 0.0
            ? static_cast<double>(performance_capture_.rows.size()) * 1'000.0 / duration_ms
            : 0.0;

        std::error_code error;
        const auto directory = performance_capture_directory();
        std::filesystem::create_directories(directory, error);
        const auto display_name = std::string{"PulseForge-performance-"}
            + capture_file_stem(chart_.has_value() ? chart_->title : "chart")
            + '-' + std::to_string(finished_ns) + ".txt";
        const auto path = directory / display_name;
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            performance_capture_message_ = "CAPTURE FAILED: cannot create report";
            performance_capture_message_until_ns_ = finished_ns + 8'000'000'000ULL;
            return;
        }
        output
            << "PulseForge Android performance capture v1\n"
            << "build=" << PULSEFORGE_PATCH_BUILD << "\n"
            << "chart=" << (chart_.has_value() ? chart_->title : std::string{"unknown"}) << "\n"
            << "source=" << (streaming_mode() ? "PFC1-stream" : "materialized") << "\n"
#if defined(PULSEFORGE_HAS_LUA)
            << "lua=" << (scripts_ != nullptr ? "on" : "off") << "\n"
#else
            << "lua=not-built\n"
#endif
            << "early_stop=" << (early ? "yes" : "no") << "\n"
            << "duration_ms=" << duration_ms << "\n"
            << "frames=" << performance_capture_.rows.size() << "\n"
            << "measured_fps=" << measured_fps << "\n"
            << "average_frame_ms=" << average_frame_ms << "\n"
            << "maximum_frame_ms=" << frame_ms_max << "\n"
            << "\n[RUNTIME_START]\n" << performance_capture_.runtime_start
            << "[RUNTIME_END]\n" << runtime_end
            << "\n[FRAMES_CSV]\n"
            << "elapsed_ms,wall_frame_ms,cpu_frame_us,gameplay_us,lua_us,note_us,cache_us,pvd_us,pfc_us,geometry_build_us,geometry_submit_us,fallback_us,present_us,rendered_notes,nps,draw_units,draw_calls,pvd_buckets,exact_visits,dropped_callbacks,window_notes,streaming_ram_bytes,catchup,saturated\n";
        for (const auto& row : performance_capture_.rows) {
            output
                << row.elapsed_ms << ',' << row.wall_frame_ms << ',' << row.cpu_frame_us << ','
                << row.gameplay_us << ',' << row.lua_us << ',' << row.note_us << ','
                << row.cache_us << ',' << row.pvd_us << ',' << row.pfc_us << ','
                << row.geometry_build_us << ',' << row.geometry_submit_us << ','
                << row.fallback_us << ',' << row.present_us << ','
                << row.rendered_notes << ',' << row.nps << ',' << row.draw_units << ','
                << row.draw_calls << ',' << row.pvd_buckets << ',' << row.exact_visits << ','
                << row.dropped_callbacks << ',' << row.window_notes << ','
                << row.streaming_ram_bytes << ',' << (row.catchup ? 1 : 0) << ','
                << (row.saturated ? 1 : 0) << '\n';
        }
        output.flush();
        if (!output) {
            performance_capture_message_ = "CAPTURE FAILED: report write error";
            performance_capture_message_until_ns_ = finished_ns + 8'000'000'000ULL;
            return;
        }
        output.close();
        const auto published = detail::publish_file_to_downloads(
            path, display_name, "text/plain"
        );
        performance_capture_message_ = published.empty()
            ? "CAPTURE SAVED IN APP STORAGE (Downloads export failed): " + display_name
            : "CAPTURE SAVED: Downloads/PulseForge/" + display_name;
        performance_capture_message_until_ns_ = finished_ns + 12'000'000'000ULL;
    }

    void toggle_performance_capture() {
        if (performance_capture_.active) finish_performance_capture(true);
        else start_performance_capture();
    }
'''
replace_once(
    "src/app/application.cpp",
    "    void set_loading_phase(\n",
    capture_code + "\n    void set_loading_phase(\n",
)

app_path = Path("src/app/application.cpp")
app = app_path.read_text()

# Existing timing sites collect whenever F3 or CAP is active.
app = app.replace(
    "const auto gameplay_started_ns = diagnostics_\n                ? SDL_GetTicksNS()",
    "const auto gameplay_started_ns = profiling_active()\n                ? SDL_GetTicksNS()",
    1,
)
app = app.replace(
    "            if (diagnostics_) {\n                note_profile_frame_.gameplay_update_ns =",
    "            if (profiling_active()) {\n                note_profile_frame_.gameplay_update_ns =",
    1,
)
app = app.replace(
    "            scene_->begin_note_skin_profile_frame(diagnostics_);",
    "            scene_->begin_note_skin_profile_frame(profiling_active());",
    1,
)
replace_reset_old = """        const auto gameplay_update_ns =
            note_profile_frame_.gameplay_update_ns;
        note_profile_frame_ = {};
        note_profile_frame_.gameplay_update_ns =
            gameplay_update_ns;
"""
replace_reset_new = """        const auto gameplay_update_ns =
            note_profile_frame_.gameplay_update_ns;
        const auto lua_ns = note_profile_frame_.lua_ns;
        note_profile_frame_ = {};
        note_profile_frame_.gameplay_update_ns = gameplay_update_ns;
        note_profile_frame_.lua_ns = lua_ns;
"""
if app.count(replace_reset_old) != 1:
    raise SystemExit("application.cpp profile reset anchor mismatch")
app = app.replace(replace_reset_old, replace_reset_new, 1)
app = app.replace(
    "const auto note_pipeline_started_ns = diagnostics_\n            ? SDL_GetTicksNS()",
    "const auto note_pipeline_started_ns = profiling_active()\n            ? SDL_GetTicksNS()",
    1,
)
app = app.replace(
    "            if (diagnostics_) {\n                note_profile_frame_.note_pipeline_ns =",
    "            if (profiling_active()) {\n                note_profile_frame_.note_pipeline_ns =",
    2,
)
app = app.replace(
    "        if (diagnostics_) {\n            note_profile_frame_.note_pipeline_ns =",
    "        if (profiling_active()) {\n            note_profile_frame_.note_pipeline_ns =",
    1,
)
for old, new in (
    (
        "const auto pvd_started_ns = diagnostics_\n                    ? SDL_GetTicksNS()",
        "const auto pvd_started_ns = profiling_active()\n                    ? SDL_GetTicksNS()",
    ),
    (
        "                if (diagnostics_) {\n                    note_profile_frame_.pvd_visit_ns +=",
        "                if (profiling_active()) {\n                    note_profile_frame_.pvd_visit_ns +=",
    ),
    (
        "const auto pfc_started_ns = diagnostics_\n                    ? SDL_GetTicksNS()",
        "const auto pfc_started_ns = profiling_active()\n                    ? SDL_GetTicksNS()",
    ),
    (
        "                if (diagnostics_) {\n                    note_profile_frame_.pfc_visit_ns +=",
        "                if (profiling_active()) {\n                    note_profile_frame_.pfc_visit_ns +=",
    ),
    (
        "const auto cache_started_ns = diagnostics_\n                ? SDL_GetTicksNS()",
        "const auto cache_started_ns = profiling_active()\n                ? SDL_GetTicksNS()",
    ),
    (
        "            if (diagnostics_) {\n                note_profile_frame_.cache_rebuild_ns +=",
        "            if (profiling_active()) {\n                note_profile_frame_.cache_rebuild_ns +=",
    ),
):
    if app.count(old) != 1:
        raise SystemExit(f"application.cpp timing anchor mismatch: {old[:60]!r} count={app.count(old)}")
    app = app.replace(old, new, 1)

present_old = """        if (diagnostics_) {
            const auto present_started_ns = SDL_GetTicksNS();
            detail::present_with_mobile_touch(renderer_);
            note_profile_present_.sample(
                SDL_GetTicksNS() - present_started_ns
            );
        } else {
            detail::present_with_mobile_touch(renderer_);
        }
"""
if app.count(present_old) != 1:
    raise SystemExit("application.cpp present profiler anchor mismatch")
app = app.replace(present_old, present_old.replace("if (diagnostics_)", "if (profiling_active())", 1), 1)

lua_old = """#if defined(PULSEFORGE_HAS_LUA)
        if (scripts_ != nullptr && !paused_) {
            service_script_sound_completions();
            if (streaming_mode()) {
                static_cast<void>(scripts_->dispatch_frame(
                    *streaming_session_,
                    elapsed_seconds
                ));
            } else if (session_ != nullptr) {
                static_cast<void>(scripts_->dispatch_frame(
                    *session_,
                    elapsed_seconds
                ));
            }
            consume_script_output();
            service_script_countdown_release();
            if (service_script_runtime_requests()) {
                song_time = audio_.compensated_position_ms();
            }
        }
#endif
"""
lua_new = """#if defined(PULSEFORGE_HAS_LUA)
        if (scripts_ != nullptr && !paused_) {
            const auto lua_started_ns = profiling_active()
                ? SDL_GetTicksNS()
                : std::uint64_t{0U};
            service_script_sound_completions();
            if (streaming_mode()) {
                static_cast<void>(scripts_->dispatch_frame(
                    *streaming_session_,
                    elapsed_seconds
                ));
            } else if (session_ != nullptr) {
                static_cast<void>(scripts_->dispatch_frame(
                    *session_,
                    elapsed_seconds
                ));
            }
            consume_script_output();
            service_script_countdown_release();
            if (service_script_runtime_requests()) {
                song_time = audio_.compensated_position_ms();
            }
            if (profiling_active()) {
                note_profile_frame_.lua_ns += SDL_GetTicksNS() - lua_started_ns;
            }
        }
#endif
"""
if app.count(lua_old) != 1:
    raise SystemExit(f"application.cpp interactive Lua block mismatch count={app.count(lua_old)}")
app = app.replace(lua_old, lua_new, 1)

frame_anchor = """        update_fps(elapsed_seconds);
        runtime_performance_.record_frame_ms(elapsed_seconds * 1'000.0);

        if (((media_ended && gameplay_complete()) || gameplay_failed())
"""
frame_new = """        update_fps(elapsed_seconds);
        runtime_performance_.record_frame_ms(elapsed_seconds * 1'000.0);
        sample_performance_capture(elapsed_seconds, frame_start_ns);

        if (((media_ended && gameplay_complete()) || gameplay_failed())
"""
if app.count(frame_anchor) != 1:
    raise SystemExit("application.cpp frame sample anchor mismatch")
app = app.replace(frame_anchor, frame_new, 1)

f3_old = """            case SDL_SCANCODE_F3:
                diagnostics_ = !diagnostics_;
                return;
            case SDL_SCANCODE_F5:
"""
f3_new = """            case SDL_SCANCODE_F3:
                diagnostics_ = !diagnostics_;
                return;
            case SDL_SCANCODE_F4:
                toggle_performance_capture();
                return;
            case SDL_SCANCODE_F5:
"""
if app.count(f3_old) != 1:
    raise SystemExit("application.cpp F3 anchor mismatch")
app = app.replace(f3_old, f3_new, 1)

if app.count("        constexpr std::array<std::string_view, 9> useful_keys{") != 1:
    raise SystemExit("application.cpp ready key array mismatch")
app = app.replace(
    "        constexpr std::array<std::string_view, 9> useful_keys{",
    "        constexpr std::array<std::string_view, 10> useful_keys{",
    1,
)
app = app.replace(
    '            "- F3   SHOW / HIDE DEBUG AND PERFORMANCE DATA",\n            "- F5   RELOAD LUA SCRIPTS (WHEN ENABLED)",',
    '            "- F3   SHOW / HIDE DEBUG AND PERFORMANCE DATA",\n            "- F4   RECORD 10 S PERFORMANCE CAPTURE TO DOWNLOADS",\n            "- F5   RELOAD LUA SCRIPTS (WHEN ENABLED)",',
    1,
)

hud_anchor = """        // F3 remains reserved for the detailed profiling/diagnostic extension
        // below the permanent three-line telemetry block.
        if (diagnostics_) {
"""
hud_new = """        const auto capture_now_ns = SDL_GetTicksNS();
        if (performance_capture_.active) {
            char capture_line[192]{};
            const double remaining = performance_capture_.deadline_ns > capture_now_ns
                ? static_cast<double>(performance_capture_.deadline_ns - capture_now_ns)
                    / 1'000'000'000.0
                : 0.0;
            std::snprintf(
                capture_line,
                sizeof(capture_line),
                "CAPTURE recording %.1f s | touch CAP/F4 stops early",
                remaining
            );
            debug_text(renderer_, 16.0F, logical_height - 200.0F, capture_line);
        } else if (!performance_capture_message_.empty()
                   && capture_now_ns < performance_capture_message_until_ns_) {
            debug_text(
                renderer_, 16.0F, logical_height - 200.0F,
                performance_capture_message_
            );
        }

        // F3 remains reserved for the detailed profiling/diagnostic extension
        // below the permanent three-line telemetry block.
        if (diagnostics_) {
"""
if app.count(hud_anchor) != 1:
    raise SystemExit("application.cpp diagnostics HUD anchor mismatch")
app = app.replace(hud_anchor, hud_new, 1)

profiler_line_old = """                "gameplay update %.0f/%.0f/%.0f us | saturated %s",
                note_profile_gameplay_update_.last_us,
                note_profile_gameplay_update_.average_us,
                note_profile_gameplay_update_.peak_us,
"""
profiler_line_new = """                "gameplay %.0f/%.0f/%.0f us | Lua %.0f/%.0f/%.0f us | saturated %s",
                note_profile_gameplay_update_.last_us,
                note_profile_gameplay_update_.average_us,
                note_profile_gameplay_update_.peak_us,
                note_profile_lua_.last_us,
                note_profile_lua_.average_us,
                note_profile_lua_.peak_us,
"""
if app.count(profiler_line_old) != 1:
    raise SystemExit("application.cpp profiler line anchor mismatch")
app = app.replace(profiler_line_old, profiler_line_new, 1)

shutdown_old = """    void shutdown() noexcept {
        discord_presence_.clear();
"""
shutdown_new = """    void shutdown() noexcept {
        if (performance_capture_.active) {
            try {
                finish_performance_capture(true);
            } catch (...) {
            }
        }
        discord_presence_.clear();
"""
if app.count(shutdown_old) != 1:
    raise SystemExit("application.cpp shutdown anchor mismatch")
app = app.replace(shutdown_old, shutdown_new, 1)

if app.count("    ProfileMetric note_profile_gameplay_update_{};\n") != 1:
    raise SystemExit("application.cpp profile state anchor mismatch")
app = app.replace(
    "    ProfileMetric note_profile_gameplay_update_{};\n",
    "    ProfileMetric note_profile_gameplay_update_{};\n    ProfileMetric note_profile_lua_{};\n",
    1,
)
if app.count("    bool diagnostics_{};\n") != 1:
    raise SystemExit("application.cpp diagnostics member anchor mismatch")
app = app.replace(
    "    bool diagnostics_{};\n",
    "    PerformanceCaptureState performance_capture_{};\n    std::string performance_capture_message_;\n    std::uint64_t performance_capture_message_until_ns_{};\n    bool diagnostics_{};\n",
    1,
)
app_path.write_text(app)

# Changelog.
changelog = Path("CHANGELOG.md")
text = changelog.read_text()
marker = "# Changelog\n"
if text.count(marker) != 1:
    raise SystemExit("CHANGELOG heading mismatch")
entry = """# Changelog

## Android diagnostics / render follow-up (2026-09-12)

- Added in-chart touch **F3** and **CAP** controls. CAP records a bounded 10-second frame-by-frame performance trace without requiring ADB or a PC.
- Android performance traces include gameplay, Lua, note/PVD/PFC/cache/geometry/present timings, streaming catch-up/window state and ART GC plus Java/native heap snapshots, then publish to `Downloads/PulseForge`.
- Added a persistent global **Lua scripts: On/Off** option; disabling it prevents Lua from loading for subsequently launched charts while preserving explicit `--no-lua`.
"""
changelog.write_text(text.replace(marker, entry, 1))
