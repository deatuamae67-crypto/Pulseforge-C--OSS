from __future__ import annotations

from pathlib import Path
import re


def text(path: str) -> str:
    return Path(path).read_text(encoding="utf-8")


def write(path: str, value: str) -> None:
    Path(path).write_text(value, encoding="utf-8")


def replace_once(path: str, old: str, new: str) -> None:
    value = text(path)
    if new in value:
        print(f"{path}: already patched")
        return
    count = value.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, found {count}: {old[:160]!r}")
    write(path, value.replace(old, new, 1))
    print(f"{path}: patched")


def replace_all_required(path: str, old: str, new: str, minimum: int = 1) -> None:
    value = text(path)
    count = value.count(old)
    if count < minimum:
        raise SystemExit(f"{path}: expected at least {minimum} matches, found {count}: {old[:160]!r}")
    write(path, value.replace(old, new))
    print(f"{path}: replaced {count} occurrence(s)")


def insert_before_once(path: str, anchor: str, addition: str) -> None:
    value = text(path)
    if addition in value:
        print(f"{path}: insertion already present")
        return
    count = value.count(anchor)
    if count != 1:
        raise SystemExit(f"{path}: insertion anchor count {count}: {anchor[:160]!r}")
    write(path, value.replace(anchor, addition + anchor, 1))
    print(f"{path}: inserted block")


# ---------------------------------------------------------------------------
# Runtime bridge is compiled on every target and becomes a no-op off Android.
# ---------------------------------------------------------------------------
replace_once(
    "CMakeLists.txt",
    "        src/app/application.cpp\n        src/app/controls_ui.cpp\n",
    "        src/app/application.cpp\n        src/app/android_runtime_bridge.cpp\n        src/app/controls_ui.cpp\n",
)

# ---------------------------------------------------------------------------
# Persisted global Lua switch.
# ---------------------------------------------------------------------------
replace_once(
    "include/pulseforge/settings.hpp",
    "    std::uint32_t script_instruction_budget{1'000'000};\n    bool hot_reload_scripts{true};\n",
    "    std::uint32_t script_instruction_budget{1'000'000};\n    // Global user switch: when false, charts run without loading any Lua.\n    bool lua_enabled{true};\n    bool hot_reload_scripts{true};\n",
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

# ---------------------------------------------------------------------------
# Options menu + launch propagation.
# ---------------------------------------------------------------------------
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
for old, new in [
    ("        case 33: {\n            const std::vector<std::string> image_choices{", "        case 34: {\n            const std::vector<std::string> image_choices{"),
    ("        case 34: {\n            std::vector<std::filesystem::path> discovery_roots =", "        case 35: {\n            std::vector<std::filesystem::path> discovery_roots ="),
    ("        case 35:\n            show_discord_options(menu, options);", "        case 36:\n            show_discord_options(menu, options);"),
    ("        case 36:\n            show_touch_options(menu, options);", "        case 37:\n            show_touch_options(menu, options);"),
    ("        case 37:\n            show_controls_editor(", "        case 38:\n            show_controls_editor("),
]:
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
# Catalog-song direct launch bypasses options_for_entry's base result only after
# the entry is resolved, so it already inherits the switch through options_for_entry.

# ---------------------------------------------------------------------------
# Android gameplay touch controls: F3 toggles diagnostics, CAP/F4 records 10 s.
# ---------------------------------------------------------------------------
replace_once(
    "src/app/mobile_touch_controls.cpp",
    "    volume_down,\n    volume_mute,\n    volume_up,\n    editor_play,\n",
    "    volume_down,\n    volume_mute,\n    volume_up,\n    diagnostics_toggle,\n    performance_capture,\n    editor_play,\n",
)
replace_once(
    "src/app/mobile_touch_controls.cpp",
    "        key_map_[action_index(TouchAction::volume_up)] = action_key(\n            bindings_, \"volume_up\", SDL_SCANCODE_EQUALS, SDL_KMOD_SHIFT\n        );\n        key_map_[action_index(TouchAction::editor_play)] = action_key(\n",
    "        key_map_[action_index(TouchAction::volume_up)] = action_key(\n            bindings_, \"volume_up\", SDL_SCANCODE_EQUALS, SDL_KMOD_SHIFT\n        );\n        key_map_[action_index(TouchAction::diagnostics_toggle)] = {\n            SDL_SCANCODE_F3, SDL_KMOD_NONE\n        };\n        key_map_[action_index(TouchAction::performance_capture)] = {\n            SDL_SCANCODE_F4, SDL_KMOD_NONE\n        };\n        key_map_[action_index(TouchAction::editor_play)] = action_key(\n",
)
replace_once(
    "src/app/mobile_touch_controls.cpp",
    "                {238, 188, 61, 255},\n                \"PAUSE\",\n            });\n",
    "                {238, 188, 61, 255},\n                \"PAUSE\",\n            });\n            const float diag_width = std::clamp(64.0F * settings_.scale, 48.0F, 86.0F);\n            const float diag_gap = std::clamp(6.0F * settings_.scale, 4.0F, 10.0F);\n            result.push_back({\n                TouchAction::diagnostics_toggle,\n                {\n                    safe.x + safe.w - pause_width - margin\n                        - diag_gap - diag_width * 2.0F - diag_gap,\n                    safe.y + margin,\n                    diag_width,\n                    pause_height,\n                },\n                {92, 186, 238, 255},\n                \"F3\",\n            });\n            result.push_back({\n                TouchAction::performance_capture,\n                {\n                    safe.x + safe.w - pause_width - margin\n                        - diag_gap - diag_width,\n                    safe.y + margin,\n                    diag_width,\n                    pause_height,\n                },\n                {103, 225, 150, 255},\n                \"CAP\",\n            });\n",
)
replace_once(
    "src/app/mobile_touch_controls.cpp",
    "                    || button.action == TouchAction::volume_up\n                    || button.action == TouchAction::gameplay_left\n",
    "                    || button.action == TouchAction::volume_up\n                    || button.action == TouchAction::diagnostics_toggle\n                    || button.action == TouchAction::performance_capture\n                    || button.action == TouchAction::gameplay_left\n",
)

# ---------------------------------------------------------------------------
# Android Java bridge: public Downloads, ART/GC stats and FFmpegKit sessions.
# ---------------------------------------------------------------------------
java_path = Path("platform/android/app/src/main/java/org/pulseforge/engine/PulseForgeActivity.java")
java = java_path.read_text(encoding="utf-8")
if "publishPulseForgeDownload" not in java:
    java = java.replace(
        "import android.content.Intent;\n",
        "import android.content.ContentValues;\nimport android.content.Intent;\n",
        1,
    )
    java = java.replace("import android.os.Bundle;\n", "import android.os.Bundle;\nimport android.os.Debug;\nimport android.os.Environment;\n", 1)
    java = java.replace("import android.provider.OpenableColumns;\n", "import android.provider.MediaStore;\nimport android.provider.OpenableColumns;\n", 1)
    java = java.replace("import org.libsdl.app.SDLActivity;\n", "import org.libsdl.app.SDLActivity;\n\nimport com.arthenica.ffmpegkit.FFmpegKit;\nimport com.arthenica.ffmpegkit.FFmpegKitConfig;\nimport com.arthenica.ffmpegkit.FFmpegSession;\nimport com.arthenica.ffmpegkit.ReturnCode;\n", 1)
    java = java.replace("import java.io.File;\n", "import java.io.File;\nimport java.io.FileInputStream;\n", 1)
    java = java.replace("import java.io.InputStream;\n", "import java.io.InputStream;\nimport java.io.OutputStream;\nimport java.util.Map;\nimport java.util.TreeMap;\nimport java.util.concurrent.ConcurrentHashMap;\n", 1)
    java = java.replace(
        "    private static final long STALE_ARCHIVE_IMPORT_MS = 24L * 60L * 60L * 1000L;\n",
        "    private static final long STALE_ARCHIVE_IMPORT_MS = 24L * 60L * 60L * 1000L;\n    private final ConcurrentHashMap<Long, FFmpegSession> pulseForgeFfmpegSessions =\n        new ConcurrentHashMap<>();\n",
        1,
    )
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
                final Map<String, String> runtimeStats = new TreeMap<>(Debug.getRuntimeStats());
                for (final Map.Entry<String, String> entry : runtimeStats.entrySet()) {
                    if (entry.getKey().startsWith("art.gc.")
                            || entry.getKey().startsWith("art.gc-")) {
                        out.append(entry.getKey()).append('=').append(entry.getValue()).append('\n');
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
        if (!source.isFile()) {
            return null;
        }
        try {
            if (Build.VERSION.SDK_INT >= 29) {
                final ContentValues values = new ContentValues();
                values.put(MediaStore.Downloads.DISPLAY_NAME, displayName);
                values.put(MediaStore.Downloads.MIME_TYPE,
                    mimeType == null || mimeType.isEmpty() ? "application/octet-stream" : mimeType);
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
                        if (count != 0) output.write(buffer, 0, count);
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
                    if (count != 0) output.write(buffer, 0, count);
                }
                output.flush();
            }
            return target.getAbsolutePath();
        } catch (final Throwable throwable) {
            Log.e(TAG, "Downloads export failed", throwable);
            return null;
        }
    }

    public String createPulseForgeFfmpegPipe() {
        try {
            return FFmpegKitConfig.registerNewFFmpegPipe(this);
        } catch (final Throwable throwable) {
            Log.e(TAG, "FFmpeg pipe creation failed", throwable);
            return null;
        }
    }

    public long startPulseForgeFfmpeg(final String[] arguments) {
        try {
            final FFmpegSession session = FFmpegKit.executeWithArgumentsAsync(
                arguments,
                completed -> { }
            );
            final long id = session.getSessionId();
            pulseForgeFfmpegSessions.put(id, session);
            return id;
        } catch (final Throwable throwable) {
            Log.e(TAG, "FFmpeg session start failed", throwable);
            return -1L;
        }
    }

    public int waitPulseForgeFfmpeg(final long sessionId) {
        final FFmpegSession session = pulseForgeFfmpegSessions.get(sessionId);
        if (session == null) return -32000;
        try {
            while (session.getReturnCode() == null) {
                final String state = String.valueOf(session.getState());
                if ("FAILED".equals(state)) return -32001;
                Thread.sleep(10L);
            }
            final ReturnCode code = session.getReturnCode();
            return code == null ? -32002 : code.getValue();
        } catch (final InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            return -32003;
        } catch (final Throwable throwable) {
            Log.e(TAG, "FFmpeg wait failed", throwable);
            return -32004;
        }
    }

    public String getPulseForgeFfmpegOutput(final long sessionId) {
        final FFmpegSession session = pulseForgeFfmpegSessions.get(sessionId);
        if (session == null) return "";
        try {
            final String output = session.getOutput();
            return output == null ? "" : output;
        } catch (final Throwable throwable) {
            return "FFmpegKit output error: " + throwable;
        }
    }

    public void cancelPulseForgeFfmpeg(final long sessionId) {
        try {
            FFmpegKit.cancel(sessionId);
        } catch (final Throwable throwable) {
            Log.w(TAG, "FFmpeg cancel failed", throwable);
        }
    }

    public void closePulseForgeFfmpegPipe(final String path) {
        if (path == null || path.isEmpty()) return;
        try {
            FFmpegKitConfig.closeFFmpegPipe(path);
        } catch (final Throwable throwable) {
            Log.w(TAG, "FFmpeg pipe close failed", throwable);
        }
    }
'''
    end = java.rfind("\n}")
    if end < 0:
        raise SystemExit("PulseForgeActivity class terminator not found")
    java = java[:end] + methods + java[end:]
    java_path.write_text(java, encoding="utf-8")
    print("PulseForgeActivity.java: patched")
else:
    print("PulseForgeActivity.java: already patched")

replace_once(
    "platform/android/app/src/main/AndroidManifest.xml",
    "    <uses-permission android:name=\"android.permission.INTERNET\" />\n",
    "    <uses-permission android:name=\"android.permission.INTERNET\" />\n    <uses-permission android:name=\"android.permission.WRITE_EXTERNAL_STORAGE\" android:maxSdkVersion=\"28\" />\n",
)

# FFmpegKit is an Android library, not an ffmpeg.exe. It carries native arm64
# FFmpeg in the APK and exposes the stable com.arthenica.ffmpegkit API.
replace_once(
    "platform/android/app/build.gradle",
    "        minSdk = discordSdkEnabled ? 24 : 21\n",
    "        // FFmpegKit 8.1 is arm64/API-24+, matching the production Android runtime.\n        minSdk = 24\n",
)
replace_once(
    "platform/android/app/build.gradle",
    "dependencies {\n    implementation fileTree(include: ['*.jar'], dir: 'libs')\n",
    "dependencies {\n    implementation fileTree(include: ['*.jar'], dir: 'libs')\n    implementation 'dev.ffmpegkit-maintained:ffmpeg-kit-full:8.1.7'\n",
)

# ---------------------------------------------------------------------------
# C++ JNI bridge. Rewrite the two newly-added files deterministically.
# ---------------------------------------------------------------------------
bridge_h = r'''#pragma once

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
'''
Path("src/app/android_runtime_bridge.hpp").write_text(bridge_h, encoding="utf-8")

bridge_cpp = r'''#include "android_runtime_bridge.hpp"

#include <string>
#include <utility>

#if defined(__ANDROID__)
#include <jni.h>
#include <SDL3/SDL_system.h>
#endif

namespace pulseforge::detail {
namespace {

#if defined(__ANDROID__)
class AndroidLocalFrame final {
public:
    explicit AndroidLocalFrame(JNIEnv* const env) noexcept : env_(env) {
        valid_ = env_ != nullptr && env_->PushLocalFrame(96) == JNI_OK;
    }
    ~AndroidLocalFrame() {
        if (valid_) env_->PopLocalFrame(nullptr);
    }
    [[nodiscard]] bool valid() const noexcept { return valid_; }
private:
    JNIEnv* env_{};
    bool valid_{};
};

[[nodiscard]] bool clear_exception(JNIEnv* const env) noexcept {
    if (env == nullptr || !env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

[[nodiscard]] jobject activity(JNIEnv* const env) noexcept {
    return env == nullptr ? nullptr
                          : static_cast<jobject>(SDL_GetAndroidActivity());
}

[[nodiscard]] jmethodID method(
    JNIEnv* const env,
    jobject const object,
    const char* const name,
    const char* const signature
) noexcept {
    if (env == nullptr || object == nullptr) return nullptr;
    const auto cls = env->GetObjectClass(object);
    if (cls == nullptr || clear_exception(env)) return nullptr;
    const auto result = env->GetMethodID(cls, name, signature);
    return clear_exception(env) ? nullptr : result;
}

[[nodiscard]] std::string java_string(JNIEnv* const env, jstring const value) {
    if (env == nullptr || value == nullptr) return {};
    const char* const chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr || clear_exception(env)) return {};
    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    if (clear_exception(env)) return {};
    return result;
}
#endif

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

void assign_error(std::string* const error, std::string value) {
    if (error != nullptr) *error = std::move(value);
}

}  // namespace

std::string android_runtime_diagnostics() {
#if defined(__ANDROID__)
    auto* const env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    AndroidLocalFrame frame(env);
    if (!frame.valid()) return {};
    const auto owner = activity(env);
    if (owner == nullptr) return {};
    const auto call = method(env, owner, "getPulseForgeRuntimeStats", "()Ljava/lang/String;");
    if (call == nullptr) return {};
    const auto value = static_cast<jstring>(env->CallObjectMethod(owner, call));
    if (clear_exception(env)) return {};
    return java_string(env, value);
#else
    return {};
#endif
}

std::string publish_file_to_downloads(
    const std::filesystem::path& source,
    const std::string_view display_name,
    const std::string_view mime_type
) {
#if defined(__ANDROID__)
    auto* const env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    AndroidLocalFrame frame(env);
    if (!frame.valid()) return {};
    const auto owner = activity(env);
    if (owner == nullptr) return {};
    const auto call = method(
        env, owner, "publishPulseForgeDownload",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"
    );
    if (call == nullptr) return {};
    const auto source_text = path_utf8(source);
    const std::string name(display_name);
    const std::string mime(mime_type);
    const auto java_source = env->NewStringUTF(source_text.c_str());
    const auto java_name = env->NewStringUTF(name.c_str());
    const auto java_mime = env->NewStringUTF(mime.c_str());
    if (java_source == nullptr || java_name == nullptr || java_mime == nullptr
        || clear_exception(env)) return {};
    const auto value = static_cast<jstring>(env->CallObjectMethod(
        owner, call, java_source, java_name, java_mime
    ));
    if (clear_exception(env)) return {};
    return java_string(env, value);
#else
    static_cast<void>(display_name);
    static_cast<void>(mime_type);
    return path_utf8(source);
#endif
}

bool android_ffmpeg_available() noexcept {
#if defined(__ANDROID__)
    auto* const env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    AndroidLocalFrame frame(env);
    if (!frame.valid()) return false;
    const auto owner = activity(env);
    return owner != nullptr
        && method(env, owner, "createPulseForgeFfmpegPipe", "()Ljava/lang/String;") != nullptr
        && method(env, owner, "startPulseForgeFfmpeg", "([Ljava/lang/String;)J") != nullptr;
#else
    return false;
#endif
}

AndroidFfmpegSession start_android_ffmpeg(
    const std::span<const std::string> arguments,
    std::string* const error
) {
#if defined(__ANDROID__)
    AndroidFfmpegSession result;
    auto* const env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    AndroidLocalFrame frame(env);
    if (!frame.valid()) {
        assign_error(error, "Android JNI environment is unavailable");
        return result;
    }
    const auto owner = activity(env);
    if (owner == nullptr) {
        assign_error(error, "Android SDL activity is unavailable");
        return result;
    }
    const auto create_pipe = method(env, owner, "createPulseForgeFfmpegPipe", "()Ljava/lang/String;");
    const auto start = method(env, owner, "startPulseForgeFfmpeg", "([Ljava/lang/String;)J");
    if (create_pipe == nullptr || start == nullptr) {
        assign_error(error, "Android FFmpegKit bridge is unavailable");
        return result;
    }
    const auto pipe_value = static_cast<jstring>(env->CallObjectMethod(owner, create_pipe));
    if (clear_exception(env) || pipe_value == nullptr) {
        assign_error(error, "Android FFmpegKit could not create its raw-video pipe");
        return result;
    }
    result.input_pipe = java_string(env, pipe_value);
    if (result.input_pipe.empty()) {
        assign_error(error, "Android FFmpegKit returned an empty raw-video pipe");
        return result;
    }

    jclass string_class = env->FindClass("java/lang/String");
    if (string_class == nullptr || clear_exception(env)) {
        assign_error(error, "Android JNI could not resolve java.lang.String");
        close_android_ffmpeg_pipe(result.input_pipe);
        result.input_pipe.clear();
        return result;
    }
    const auto offset = !arguments.empty() ? std::size_t{1U} : std::size_t{0U};
    const auto count = arguments.size() - offset;
    auto array = env->NewObjectArray(static_cast<jsize>(count), string_class, nullptr);
    if (array == nullptr || clear_exception(env)) {
        assign_error(error, "Android JNI could not allocate FFmpeg arguments");
        close_android_ffmpeg_pipe(result.input_pipe);
        result.input_pipe.clear();
        return result;
    }
    for (std::size_t index = 0U; index < count; ++index) {
        std::string value = arguments[index + offset];
        if (value == "pipe:0") value = result.input_pipe;
        const auto java_value = env->NewStringUTF(value.c_str());
        if (java_value == nullptr || clear_exception(env)) {
            assign_error(error, "Android JNI could not encode an FFmpeg argument");
            close_android_ffmpeg_pipe(result.input_pipe);
            result.input_pipe.clear();
            return result;
        }
        env->SetObjectArrayElement(array, static_cast<jsize>(index), java_value);
        if (clear_exception(env)) {
            assign_error(error, "Android JNI could not populate FFmpeg arguments");
            close_android_ffmpeg_pipe(result.input_pipe);
            result.input_pipe.clear();
            return result;
        }
    }
    result.session_id = static_cast<std::int64_t>(env->CallLongMethod(owner, start, array));
    if (clear_exception(env) || result.session_id < 0) {
        assign_error(error, "Android FFmpegKit failed to start the encoding session");
        close_android_ffmpeg_pipe(result.input_pipe);
        result.input_pipe.clear();
        result.session_id = -1;
        return result;
    }
    if (error != nullptr) error->clear();
    return result;
#else
    static_cast<void>(arguments);
    assign_error(error, "Android FFmpegKit is unavailable on this platform");
    return {};
#endif
}

int wait_android_ffmpeg(const std::int64_t session_id, std::string* const output) {
#if defined(__ANDROID__)
    auto* const env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    AndroidLocalFrame frame(env);
    if (!frame.valid()) return -32010;
    const auto owner = activity(env);
    if (owner == nullptr) return -32011;
    const auto wait_call = method(env, owner, "waitPulseForgeFfmpeg", "(J)I");
    const auto output_call = method(env, owner, "getPulseForgeFfmpegOutput", "(J)Ljava/lang/String;");
    if (wait_call == nullptr) return -32012;
    const int code = static_cast<int>(env->CallIntMethod(owner, wait_call, static_cast<jlong>(session_id)));
    if (clear_exception(env)) return -32013;
    if (output != nullptr && output_call != nullptr) {
        const auto value = static_cast<jstring>(env->CallObjectMethod(
            owner, output_call, static_cast<jlong>(session_id)
        ));
        if (!clear_exception(env)) *output = java_string(env, value);
    }
    return code;
#else
    static_cast<void>(session_id);
    if (output != nullptr) output->clear();
    return -32014;
#endif
}

void cancel_android_ffmpeg(const std::int64_t session_id) noexcept {
#if defined(__ANDROID__)
    if (session_id < 0) return;
    auto* const env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    AndroidLocalFrame frame(env);
    if (!frame.valid()) return;
    const auto owner = activity(env);
    if (owner == nullptr) return;
    const auto call = method(env, owner, "cancelPulseForgeFfmpeg", "(J)V");
    if (call == nullptr) return;
    env->CallVoidMethod(owner, call, static_cast<jlong>(session_id));
    static_cast<void>(clear_exception(env));
#else
    static_cast<void>(session_id);
#endif
}

void close_android_ffmpeg_pipe(const std::string_view path) noexcept {
#if defined(__ANDROID__)
    if (path.empty()) return;
    auto* const env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    AndroidLocalFrame frame(env);
    if (!frame.valid()) return;
    const auto owner = activity(env);
    if (owner == nullptr) return;
    const auto call = method(env, owner, "closePulseForgeFfmpegPipe", "(Ljava/lang/String;)V");
    if (call == nullptr) return;
    const std::string copy(path);
    const auto java_path = env->NewStringUTF(copy.c_str());
    if (java_path == nullptr || clear_exception(env)) return;
    env->CallVoidMethod(owner, call, java_path);
    static_cast<void>(clear_exception(env));
#else
    static_cast<void>(path);
#endif
}

}  // namespace pulseforge::detail
'''
Path("src/app/android_runtime_bridge.cpp").write_text(bridge_cpp, encoding="utf-8")
print("android_runtime_bridge.*: rewritten")

# ---------------------------------------------------------------------------
# Android offline-render planning: use the in-APK FFmpegKit backend and
# MediaCodec H.264 rather than looking for ffmpeg.exe.
# ---------------------------------------------------------------------------
replace_once(
    "src/io/offline_render.cpp",
    "[[nodiscard]] std::optional<std::filesystem::path> discover_ffmpeg(\n    const OfflineRenderConfig& config,\n    const std::vector<std::filesystem::path>& forbidden_roots\n) {\n",
    "[[nodiscard]] std::optional<std::filesystem::path> discover_ffmpeg(\n    const OfflineRenderConfig& config,\n    const std::vector<std::filesystem::path>& forbidden_roots\n) {\n#if defined(__ANDROID__)\n    static_cast<void>(config);\n    static_cast<void>(forbidden_roots);\n    // Sentinel only: OfflineEncoder routes this plan to FFmpegKit through JNI.\n    return std::filesystem::path{\"/android/ffmpegkit/ffmpeg\"};\n#else\n",
)
replace_once(
    "src/io/offline_render.cpp",
    "    return std::nullopt;\n}\n\n[[nodiscard]] std::string safe_stem",
    "    return std::nullopt;\n#endif\n}\n\n[[nodiscard]] std::string safe_stem",
)
replace_once(
    "src/io/offline_render.cpp",
    "    args.insert(args.end(), {\n        \"-c:v\", std::string(video_codec_name(effective_codec)),\n        \"-threads\", std::to_string(request.config.thread_count),\n    });\n    if (effective_codec == OfflineRenderVideoCodec::av1) {\n        args.insert(args.end(), {\"-preset\", std::string(svt_av1_preset(effective_preset))});\n    } else {\n        args.insert(args.end(), {\"-preset\", std::string(software_preset(effective_preset))});\n    }\n    args.insert(args.end(), {\n        \"-crf\", std::to_string(request.config.crf),\n        \"-pix_fmt\", std::string(pixel_format_name(effective_pixel_format)),\n",
    "#if defined(__ANDROID__)\n    // Android encodes in-process through FFmpegKit. H.264 MediaCodec avoids the\n    // GPL-only libx264 dependency and uses the device encoder shipped by Android.\n    const auto android_bitrate = std::clamp<std::uint64_t>(\n        static_cast<std::uint64_t>(plan.width) * plan.height * plan.fps / 8U,\n        2'000'000ULL,\n        80'000'000ULL\n    );\n    args.insert(args.end(), {\n        \"-c:v\", \"h264_mediacodec\",\n        \"-b:v\", std::to_string(android_bitrate),\n        \"-pix_fmt\", \"yuv420p\",\n#else\n    args.insert(args.end(), {\n        \"-c:v\", std::string(video_codec_name(effective_codec)),\n        \"-threads\", std::to_string(request.config.thread_count),\n    });\n    if (effective_codec == OfflineRenderVideoCodec::av1) {\n        args.insert(args.end(), {\"-preset\", std::string(svt_av1_preset(effective_preset))});\n    } else {\n        args.insert(args.end(), {\"-preset\", std::string(software_preset(effective_preset))});\n    }\n    args.insert(args.end(), {\n        \"-crf\", std::to_string(request.config.crf),\n        \"-pix_fmt\", std::string(pixel_format_name(effective_pixel_format)),\n#endif\n",
)
replace_once(
    "src/io/offline_render.cpp",
    "    if (request.config.maximum_performance) {\n        // PULSEFORGE_P1_5_0E_FFMPEG_MAXIMUM_PERFORMANCE_ARGS_V1\n        args.insert(args.end(), {\n            \"-tune\", \"zerolatency\",\n            \"-bf\", \"0\",\n            \"-refs\", \"1\",\n            \"-flush_packets\", \"0\",\n        });\n    }\n",
    "#if !defined(__ANDROID__)\n    if (request.config.maximum_performance) {\n        // PULSEFORGE_P1_5_0E_FFMPEG_MAXIMUM_PERFORMANCE_ARGS_V1\n        args.insert(args.end(), {\n            \"-tune\", \"zerolatency\",\n            \"-bf\", \"0\",\n            \"-refs\", \"1\",\n            \"-flush_packets\", \"0\",\n        });\n    }\n#endif\n",
)

# ---------------------------------------------------------------------------
# OfflineEncoder Android backend: feed the FFmpegKit FIFO from the existing
# bounded writer queue, wait for the Java session, then export the MP4.
# ---------------------------------------------------------------------------
replace_once(
    "src/app/offline_encoder.cpp",
    "#include \"offline_encoder.hpp\"\n",
    "#include \"offline_encoder.hpp\"\n#include \"android_runtime_bridge.hpp\"\n",
)
replace_once(
    "src/app/offline_encoder.hpp",
    "#include <filesystem>\n",
    "#include <filesystem>\n#include <fstream>\n",
)
replace_once(
    "src/app/offline_encoder.hpp",
    "    OfflineRenderPlan plan_;\n#if defined(_WIN32)\n",
    "    OfflineRenderPlan plan_;\n#if defined(__ANDROID__)\n    std::int64_t android_session_id_{-1};\n    std::string android_pipe_path_;\n    std::ofstream android_input_;\n    std::string android_diagnostics_;\n#elif defined(_WIN32)\n",
)
# Convert all platform branches in OfflineEncoder from _WIN32/else to Android/_WIN32/else.
enc = text("src/app/offline_encoder.cpp")
if "PULSEFORGE_ANDROID_FFMPEGKIT_ENCODER_V1" not in enc:
    start_anchor = "    remove_if_present(plan_.temporary_output_path);\n    remove_if_present(plan_.diagnostic_log_path);\n\n#if defined(_WIN32)\n"
    android_start = r'''    remove_if_present(plan_.temporary_output_path);
    remove_if_present(plan_.diagnostic_log_path);

#if defined(__ANDROID__)
    // PULSEFORGE_ANDROID_FFMPEGKIT_ENCODER_V1
    if (!android_ffmpeg_available()) {
        assign_error(error, "Android FFmpegKit backend is unavailable in this APK");
        return false;
    }
    auto android_session = start_android_ffmpeg(plan_.arguments, error);
    if (android_session.session_id < 0 || android_session.input_pipe.empty()) {
        remove_private_files();
        return false;
    }
    android_session_id_ = android_session.session_id;
    android_pipe_path_ = std::move(android_session.input_pipe);
    android_input_.open(android_pipe_path_, std::ios::binary | std::ios::out);
    if (!android_input_) {
        cancel_android_ffmpeg(android_session_id_);
        close_android_ffmpeg_pipe(android_pipe_path_);
        android_session_id_ = -1;
        android_pipe_path_.clear();
        remove_private_files();
        assign_error(error, "cannot open Android FFmpegKit raw-video pipe");
        return false;
    }
#elif defined(_WIN32)
'''
    if start_anchor not in enc:
        raise SystemExit("offline_encoder.cpp: start platform anchor missing")
    enc = enc.replace(start_anchor, android_start, 1)

    enc = enc.replace(
        "#if defined(_WIN32)\n    const bool process_ready = process_handle_ != nullptr\n        && input_handle_ != nullptr;\n#else\n    const bool process_ready = process_ != nullptr && input_ != nullptr;\n#endif",
        "#if defined(__ANDROID__)\n    const bool process_ready = android_session_id_ >= 0 && android_input_.is_open();\n#elif defined(_WIN32)\n    const bool process_ready = process_handle_ != nullptr\n        && input_handle_ != nullptr;\n#else\n    const bool process_ready = process_ != nullptr && input_ != nullptr;\n#endif",
    )

    enc = enc.replace(
        "#if defined(_WIN32)\n    while (remaining != 0U) {",
        "#if defined(__ANDROID__)\n    android_input_.write(\n        reinterpret_cast<const char*>(bytes),\n        static_cast<std::streamsize>(byte_count)\n    );\n    if (!android_input_) {\n        error = \"Android FFmpegKit raw-video pipe closed\";\n        return false;\n    }\n    return true;\n#elif defined(_WIN32)\n    while (remaining != 0U) {",
        1,
    )

    enc = enc.replace(
        "void OfflineEncoder::interrupt_process_write() noexcept {\n#if defined(_WIN32)",
        "void OfflineEncoder::interrupt_process_write() noexcept {\n#if defined(__ANDROID__)\n    if (android_session_id_ >= 0) {\n        cancel_android_ffmpeg(android_session_id_);\n    }\n#elif defined(_WIN32)",
        1,
    )
    enc = enc.replace(
        "void OfflineEncoder::close_stdin() noexcept {\n#if defined(_WIN32)",
        "void OfflineEncoder::close_stdin() noexcept {\n#if defined(__ANDROID__)\n    if (android_input_.is_open()) {\n        android_input_.flush();\n        android_input_.close();\n    }\n#elif defined(_WIN32)",
        1,
    )
    enc = enc.replace(
        "void OfflineEncoder::stop_process() noexcept {\n    close_stdin();\n#if defined(_WIN32)",
        "void OfflineEncoder::stop_process() noexcept {\n    close_stdin();\n#if defined(__ANDROID__)\n    if (android_session_id_ >= 0) {\n        cancel_android_ffmpeg(android_session_id_);\n        static_cast<void>(wait_android_ffmpeg(android_session_id_, &android_diagnostics_));\n        android_session_id_ = -1;\n    }\n    if (!android_pipe_path_.empty()) {\n        close_android_ffmpeg_pipe(android_pipe_path_);\n        android_pipe_path_.clear();\n    }\n#elif defined(_WIN32)",
        1,
    )
    enc = enc.replace(
        "std::string OfflineEncoder::diagnostic_excerpt() const {\n    std::ifstream input(plan_.diagnostic_log_path, std::ios::binary | std::ios::ate);",
        "std::string OfflineEncoder::diagnostic_excerpt() const {\n#if defined(__ANDROID__)\n    if (!android_diagnostics_.empty()) {\n        if (android_diagnostics_.size() <= maximum_diagnostic_bytes) return android_diagnostics_;\n        return android_diagnostics_.substr(android_diagnostics_.size() - maximum_diagnostic_bytes);\n    }\n#endif\n    std::ifstream input(plan_.diagnostic_log_path, std::ios::binary | std::ios::ate);",
        1,
    )
    enc = enc.replace(
        "    close_stdin();\n    int exit_code = -1;\n#if defined(_WIN32)",
        "    close_stdin();\n    int exit_code = -1;\n#if defined(__ANDROID__)\n    const int android_exit_code = wait_android_ffmpeg(\n        android_session_id_,\n        &android_diagnostics_\n    );\n    const bool exited = android_exit_code != -32010\n        && android_exit_code != -32011\n        && android_exit_code != -32012\n        && android_exit_code != -32013\n        && android_exit_code != -32014;\n    exit_code = android_exit_code;\n    android_session_id_ = -1;\n    if (!android_pipe_path_.empty()) {\n        close_android_ffmpeg_pipe(android_pipe_path_);\n        android_pipe_path_.clear();\n    }\n#elif defined(_WIN32)",
        1,
    )
    enc = enc.replace(
        "#if defined(_WIN32)\n                    : wait_failure\n#else\n                    : std::string(SDL_GetError())\n#endif",
        "#if defined(__ANDROID__)\n                    : std::string{\"FFmpegKit session failed before returning a code\"}\n#elif defined(_WIN32)\n                    : wait_failure\n#else\n                    : std::string(SDL_GetError())\n#endif",
        1,
    )
    enc = enc.replace(
        "    if (!commit_output(error)) {\n        remove_private_files();\n        finished_ = true;\n        return false;\n    }\n    remove_if_present(plan_.diagnostic_log_path);",
        "    if (!commit_output(error)) {\n        remove_private_files();\n        finished_ = true;\n        return false;\n    }\n#if defined(__ANDROID__)\n    const auto published = publish_file_to_downloads(\n        plan_.final_output_path,\n        plan_.final_output_path.filename().string(),\n        \"video/mp4\"\n    );\n    if (published.empty()) {\n        finished_ = true;\n        assign_error(error, \"render completed but Android could not publish it to Downloads/PulseForge\");\n        return false;\n    }\n#endif\n    remove_if_present(plan_.diagnostic_log_path);",
        1,
    )
    enc = enc.replace(
        "bool OfflineEncoder::active() const noexcept {\n#if defined(_WIN32)\n    const bool process_ready = process_handle_ != nullptr;\n#else\n    const bool process_ready = process_ != nullptr;\n#endif",
        "bool OfflineEncoder::active() const noexcept {\n#if defined(__ANDROID__)\n    const bool process_ready = android_session_id_ >= 0;\n#elif defined(_WIN32)\n    const bool process_ready = process_handle_ != nullptr;\n#else\n    const bool process_ready = process_ != nullptr;\n#endif",
        1,
    )
    write("src/app/offline_encoder.cpp", enc)
    print("offline_encoder.cpp: Android FFmpegKit backend patched")
else:
    print("offline_encoder.cpp: Android backend already present")

# ---------------------------------------------------------------------------
# Gameplay profiler/report and Android render output root.
# ---------------------------------------------------------------------------
replace_once(
    "src/app/application.cpp",
    "#include \"application_runner.hpp\"\n",
    "#include \"application_runner.hpp\"\n#include \"android_runtime_bridge.hpp\"\n",
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
replace_once(
    "src/app/application.cpp",
    "        note_profile_last_fallback_draws_ = runtime_stats.fallback_draws;\n    }\n\n    void set_loading_phase(\n",
    "        note_profile_last_fallback_draws_ = runtime_stats.fallback_draws;\n        sample_performance_capture(now);\n    }\n\n    void set_loading_phase(\n",
)

capture_code = r'''
    struct PerformanceCaptureRow final {
        double elapsed_ms{};
        double fps{};
        double frame_ms{};
        double gameplay_us{};
        double lua_us{};
        double note_us{};
        double cache_us{};
        double pvd_us{};
        double pfc_us{};
        double batch_build_us{};
        double batch_submit_us{};
        double fallback_us{};
        double present_us{};
        double adaptive_scroll{};
        std::uint64_t onscreen_notes{};
        std::uint64_t chart_total{};
        std::uint64_t window_notes{};
        std::uint64_t window_bytes{};
        std::uint64_t draw_units{};
        std::uint64_t geometry_calls{};
        bool catchup{};
        bool saturated{};
    };

    struct PerformanceCaptureState final {
        bool active{};
        std::uint64_t started_ns{};
        std::vector<PerformanceCaptureRow> rows;
        std::string android_start_stats;
    };

    void start_performance_capture() {
        performance_capture_.active = true;
        performance_capture_.started_ns = SDL_GetTicksNS();
        performance_capture_.rows.clear();
        performance_capture_.rows.reserve(5'000U);
        performance_capture_.android_start_stats =
            detail::android_runtime_diagnostics();
        diagnostics_ = true;
        reset_note_profile_peaks();
        std::cerr << "[PulseForge] 10-second performance capture started\n";
    }

    void sample_performance_capture(const std::uint64_t now) {
        if (!performance_capture_.active) return;
        PerformanceCaptureRow row;
        row.elapsed_ms = static_cast<double>(
            now - performance_capture_.started_ns
        ) / 1'000'000.0;
        row.fps = smoothed_fps_;
        row.frame_ms = smoothed_frame_ms_;
        row.gameplay_us = note_profile_gameplay_update_.last_us;
        row.lua_us = note_profile_lua_.last_us;
        row.note_us = note_profile_note_.last_us;
        row.cache_us = note_profile_cache_.last_us;
        row.pvd_us = note_profile_pvd_.last_us;
        row.pfc_us = note_profile_pfc_.last_us;
        row.batch_build_us = note_profile_batch_build_.last_us;
        row.batch_submit_us = note_profile_batch_submit_.last_us;
        row.fallback_us = note_profile_fallback_.last_us;
        row.present_us = note_profile_present_.last_us;
        row.adaptive_scroll = adaptive_scroll_.multiplier();
        row.onscreen_notes = rendered_notes_;
        row.draw_units = visual_draw_units_;
        row.geometry_calls = visual_geometry_calls_;
        if (streaming_mode()) {
            const auto memory = streaming_session_->memory_stats();
            row.chart_total = streaming_session_->summary().chart_total;
            row.window_notes = memory.window_notes;
            row.window_bytes = memory.approximate_dynamic_bytes;
            row.catchup = streaming_session_->catchup_pending();
            row.saturated = streaming_session_->window_saturated();
        } else if (session_ != nullptr) {
            row.chart_total = session_->summary().chart_total;
        }
        if (performance_capture_.rows.size() < 5'000U) {
            performance_capture_.rows.push_back(row);
        }
        if (now - performance_capture_.started_ns >= 10'000'000'000ULL) {
            finish_performance_capture();
        }
    }

    void finish_performance_capture() {
        if (!performance_capture_.active) return;
        performance_capture_.active = false;
        std::filesystem::path directory;
        if (char* pref = SDL_GetPrefPath("PulseForge", "PulseForge"); pref != nullptr) {
            directory = std::filesystem::path(pref) / "diagnostics";
            SDL_free(pref);
        } else {
            directory = std::filesystem::temp_directory_path() / "pulseforge-diagnostics";
        }
        std::error_code filesystem_error;
        std::filesystem::create_directories(directory, filesystem_error);
        const auto filename = std::string{"pulseforge-performance-"}
            + std::to_string(SDL_GetTicksNS()) + ".txt";
        const auto report_path = directory / filename;
        std::ofstream output(report_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            std::cerr << "[PulseForge] performance report could not be created\n";
            return;
        }
        double fps_sum = 0.0;
        double frame_sum = 0.0;
        double min_fps = std::numeric_limits<double>::infinity();
        double max_frame = 0.0;
        for (const auto& row : performance_capture_.rows) {
            fps_sum += row.fps;
            frame_sum += row.frame_ms;
            min_fps = std::min(min_fps, row.fps);
            max_frame = std::max(max_frame, row.frame_ms);
        }
        const double count = static_cast<double>(performance_capture_.rows.size());
        output << "PulseForge on-device performance capture\n";
        output << "build=" << PULSEFORGE_PATCH_BUILD << '\n';
        output << "chart=" << path_utf8(options_.chart_path) << '\n';
        output << "duration_target_ms=10000\n";
        output << "samples=" << performance_capture_.rows.size() << '\n';
        output << "average_fps=" << (count > 0.0 ? fps_sum / count : 0.0) << '\n';
        output << "minimum_smoothed_fps=" << (std::isfinite(min_fps) ? min_fps : 0.0) << '\n';
        output << "average_frame_ms=" << (count > 0.0 ? frame_sum / count : 0.0) << '\n';
        output << "maximum_smoothed_frame_ms=" << max_frame << '\n';
        output << "lua_setting=" << (options_.settings.performance.lua_enabled ? "on" : "off") << '\n';
        output << "\n[android_runtime_start]\n" << performance_capture_.android_start_stats;
        output << "\n[android_runtime_end]\n" << detail::android_runtime_diagnostics();
        output << "\n[frames_csv]\n";
        output << "elapsed_ms,fps,frame_ms,gameplay_us,lua_us,note_us,cache_us,pvd_us,pfc_us,batch_build_us,batch_submit_us,fallback_us,present_us,adaptive_scroll,onscreen_notes,chart_total,window_notes,window_bytes,draw_units,geometry_calls,catchup,saturated\n";
        for (const auto& row : performance_capture_.rows) {
            output << row.elapsed_ms << ',' << row.fps << ',' << row.frame_ms << ','
                << row.gameplay_us << ',' << row.lua_us << ',' << row.note_us << ','
                << row.cache_us << ',' << row.pvd_us << ',' << row.pfc_us << ','
                << row.batch_build_us << ',' << row.batch_submit_us << ','
                << row.fallback_us << ',' << row.present_us << ','
                << row.adaptive_scroll << ',' << row.onscreen_notes << ','
                << row.chart_total << ',' << row.window_notes << ','
                << row.window_bytes << ',' << row.draw_units << ','
                << row.geometry_calls << ',' << (row.catchup ? 1 : 0) << ','
                << (row.saturated ? 1 : 0) << '\n';
        }
        output.close();
        const auto published = detail::publish_file_to_downloads(
            report_path,
            filename,
            "text/plain"
        );
        if (published.empty()) {
            std::cerr << "[PulseForge] capture complete, but Android export to Downloads/PulseForge failed: "
                      << path_utf8(report_path) << '\n';
        } else {
            std::cerr << "[PulseForge] capture exported to Downloads/PulseForge: "
                      << published << '\n';
        }
    }

'''
insert_before_once(
    "src/app/application.cpp",
    "    void set_loading_phase(\n",
    capture_code,
)

replace_once(
    "src/app/application.cpp",
    "            case SDL_SCANCODE_F3:\n                diagnostics_ = !diagnostics_;\n                return;\n            case SDL_SCANCODE_F5:\n",
    "            case SDL_SCANCODE_F3:\n                diagnostics_ = !diagnostics_;\n                return;\n            case SDL_SCANCODE_F4:\n                start_performance_capture();\n                return;\n            case SDL_SCANCODE_F5:\n",
)
replace_once(
    "src/app/application.cpp",
    "            const auto gameplay_started_ns = diagnostics_\n                ? SDL_GetTicksNS()\n                : std::uint64_t{0U};\n            static_cast<void>(streaming_session_->update(song_time_ms));\n            if (diagnostics_) {\n",
    "            const auto gameplay_started_ns = profiling_active()\n                ? SDL_GetTicksNS()\n                : std::uint64_t{0U};\n            static_cast<void>(streaming_session_->update(song_time_ms));\n            if (profiling_active()) {\n",
)
replace_once(
    "src/app/application.cpp",
    "        const auto gameplay_update_ns =\n            note_profile_frame_.gameplay_update_ns;\n        note_profile_frame_ = {};\n        note_profile_frame_.gameplay_update_ns =\n            gameplay_update_ns;\n        if (scene_ != nullptr) {\n            scene_->begin_note_skin_profile_frame(diagnostics_);\n        }\n",
    "        const auto gameplay_update_ns = note_profile_frame_.gameplay_update_ns;\n        const auto lua_ns = note_profile_frame_.lua_ns;\n        note_profile_frame_ = {};\n        note_profile_frame_.gameplay_update_ns = gameplay_update_ns;\n        note_profile_frame_.lua_ns = lua_ns;\n        if (scene_ != nullptr) {\n            scene_->begin_note_skin_profile_frame(profiling_active());\n        }\n",
)
replace_once(
    "src/app/application.cpp",
    "        const auto note_pipeline_started_ns = diagnostics_\n            ? SDL_GetTicksNS()\n            : std::uint64_t{0U};\n",
    "        const auto note_pipeline_started_ns = profiling_active()\n            ? SDL_GetTicksNS()\n            : std::uint64_t{0U};\n",
)
replace_all_required(
    "src/app/application.cpp",
    "            if (diagnostics_) {\n                note_profile_frame_.note_pipeline_ns =\n",
    "            if (profiling_active()) {\n                note_profile_frame_.note_pipeline_ns =\n",
    1,
)
replace_all_required(
    "src/app/application.cpp",
    "        if (diagnostics_) {\n            note_profile_frame_.note_pipeline_ns =\n",
    "        if (profiling_active()) {\n            note_profile_frame_.note_pipeline_ns =\n",
    1,
)
for metric in ("pvd", "pfc", "cache"):
    replace_all_required(
        "src/app/application.cpp",
        f"const auto {metric}_started_ns = diagnostics_",
        f"const auto {metric}_started_ns = profiling_active()",
        1,
    )
replace_all_required(
    "src/app/application.cpp",
    "                if (diagnostics_) {\n                    note_profile_frame_.pvd_visit_ns +=\n",
    "                if (profiling_active()) {\n                    note_profile_frame_.pvd_visit_ns +=\n",
    1,
)
replace_all_required(
    "src/app/application.cpp",
    "                if (diagnostics_) {\n                    note_profile_frame_.pfc_visit_ns +=\n",
    "                if (profiling_active()) {\n                    note_profile_frame_.pfc_visit_ns +=\n",
    1,
)
replace_all_required(
    "src/app/application.cpp",
    "            if (diagnostics_) {\n                note_profile_frame_.cache_rebuild_ns +=\n",
    "            if (profiling_active()) {\n                note_profile_frame_.cache_rebuild_ns +=\n",
    1,
)
replace_once(
    "src/app/application.cpp",
    "        if (diagnostics_) {\n            const auto present_started_ns = SDL_GetTicksNS();\n",
    "        if (profiling_active()) {\n            const auto present_started_ns = SDL_GetTicksNS();\n",
)
# Time the normal interactive Lua frame dispatch.
replace_once(
    "src/app/application.cpp",
    "#if defined(PULSEFORGE_HAS_LUA)\n        if (scripts_ != nullptr && !paused_) {\n            service_script_sound_completions();\n",
    "#if defined(PULSEFORGE_HAS_LUA)\n        if (scripts_ != nullptr && !paused_) {\n            const auto lua_started_ns = profiling_active()\n                ? SDL_GetTicksNS()\n                : std::uint64_t{0U};\n            service_script_sound_completions();\n",
)
replace_once(
    "src/app/application.cpp",
    "            consume_script_output();\n        }\n#endif\n\n        update_effects",
    "            consume_script_output();\n            if (profiling_active()) {\n                note_profile_frame_.lua_ns += SDL_GetTicksNS() - lua_started_ns;\n            }\n        }\n#endif\n\n        update_effects",
)
# Android render mode always uses an app-writable staging directory; completed
# MP4 is published to public Downloads/PulseForge by OfflineEncoder.
replace_once(
    "src/app/application.cpp",
    "        OfflineRenderPlanRequest request;\n        request.config = options_.offline_render;\n        request.chart_title = chart_->title;\n",
    "        OfflineRenderPlanRequest request;\n        request.config = options_.offline_render;\n#if defined(__ANDROID__)\n        if (!request.config.output_directory.is_absolute()) {\n            if (char* pref = SDL_GetPrefPath(\"PulseForge\", \"PulseForge\"); pref != nullptr) {\n                request.config.output_directory = std::filesystem::path(pref) / \"renders\";\n                SDL_free(pref);\n            }\n        }\n#endif\n        request.chart_title = chart_->title;\n",
)
# Metrics and capture members.
replace_once(
    "src/app/application.cpp",
    "    ProfileMetric note_profile_present_{};\n    ProfileMetric note_profile_gameplay_update_{};\n",
    "    ProfileMetric note_profile_present_{};\n    ProfileMetric note_profile_gameplay_update_{};\n    ProfileMetric note_profile_lua_{};\n",
)
replace_once(
    "src/app/application.cpp",
    "    double smoothed_fps_{};\n    double smoothed_frame_ms_{};\n    AdaptiveScrollController adaptive_scroll_;\n",
    "    double smoothed_fps_{};\n    double smoothed_frame_ms_{};\n    PerformanceCaptureState performance_capture_;\n    AdaptiveScrollController adaptive_scroll_;\n",
)

# ---------------------------------------------------------------------------
# Changelog: document all Android-visible behaviour.
# ---------------------------------------------------------------------------
changelog = Path("CHANGELOG.md")
ct = changelog.read_text(encoding="utf-8")
marker = "- Android gameplay now exposes touch `F3` and `CAP` controls"
if marker not in ct:
    insert = (
        "- Android gameplay now exposes touch `F3` and `CAP` controls; `CAP` records a 10-second on-device performance report (frame/gameplay/Lua/note/cache/PVD/PFC/geometry/present, streaming pressure, memory plus ART/GC snapshots) and publishes it to `Downloads/PulseForge` for direct sharing without ADB or a PC.\n"
        "- Adds a persisted `Lua scripts: On/Off` option that globally disables chart Lua loading while preserving explicit command-line `--no-lua` precedence.\n"
        "- Android Rendering Mode no longer searches for `ffmpeg.exe`: the APK links the maintained FFmpegKit Android AAR, feeds the existing bounded raw-frame queue through an app-private FFmpeg pipe, uses Android MediaCodec H.264, and publishes finished MP4 files to `Downloads/PulseForge`.\n"
    )
    first_newline = ct.find("\n") + 1
    ct = ct[:first_newline] + insert + ct[first_newline:]
    changelog.write_text(ct, encoding="utf-8")
    print("CHANGELOG.md: patched")

print("Android runtime patch complete")
