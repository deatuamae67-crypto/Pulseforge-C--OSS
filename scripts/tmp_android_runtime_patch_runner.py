from __future__ import annotations

from pathlib import Path
import runpy


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    value = p.read_text(encoding="utf-8")
    if new in value:
        print(f"{path}: continuation already present")
        return
    count = value.count(old)
    if count != 1:
        raise SystemExit(
            f"{path}: continuation expected one match, found {count}: {old[:140]!r}"
        )
    p.write_text(value.replace(old, new, 1), encoding="utf-8")
    print(f"{path}: continuation patched")


try:
    runpy.run_path("scripts/tmp_android_runtime_patch.py", run_name="__main__")
except SystemExit as failure:
    message = str(failure)
    expected = "consume_script_output();\\n        }\\n#endif\\n\\n        update_effects"
    if expected not in message:
        raise
    print("Known Lua timing anchor drift encountered; continuing with robust patch.")

# The first half of the original patch already introduced lua_started_ns inside
# the Lua block before it hit the known anchor mismatch. Hoist it one scope so
# the metric can be finalized immediately before #endif without depending on
# the exact callback/service statements inside that block.
replace_once(
    "src/app/application.cpp",
    "#if defined(PULSEFORGE_HAS_LUA)\n        if (scripts_ != nullptr && !paused_) {\n            const auto lua_started_ns = profiling_active()\n                ? SDL_GetTicksNS()\n                : std::uint64_t{0U};\n            service_script_sound_completions();\n",
    "#if defined(PULSEFORGE_HAS_LUA)\n        const auto lua_started_ns = profiling_active()\n            ? SDL_GetTicksNS()\n            : std::uint64_t{0U};\n        if (scripts_ != nullptr && !paused_) {\n            service_script_sound_completions();\n",
)
replace_once(
    "src/app/application.cpp",
    "#endif\n\n        update_effects(static_cast<float>(elapsed_seconds));\n        render(song_time);\n",
    "        if (profiling_active() && lua_started_ns != 0U) {\n            note_profile_frame_.lua_ns += SDL_GetTicksNS() - lua_started_ns;\n        }\n#endif\n\n        update_effects(static_cast<float>(elapsed_seconds));\n        render(song_time);\n",
)

# Android renders stage into an app-writable directory. OfflineEncoder publishes
# the completed MP4 into public Downloads/PulseForge after FFmpegKit succeeds.
replace_once(
    "src/app/application.cpp",
    "        OfflineRenderPlanRequest request;\n        request.config = options_.offline_render;\n        request.chart_title = chart_->title;\n",
    "        OfflineRenderPlanRequest request;\n        request.config = options_.offline_render;\n#if defined(__ANDROID__)\n        if (!request.config.output_directory.is_absolute()) {\n            if (char* pref = SDL_GetPrefPath(\"PulseForge\", \"PulseForge\"); pref != nullptr) {\n                request.config.output_directory = std::filesystem::path(pref) / \"renders\";\n                SDL_free(pref);\n            }\n        }\n#endif\n        request.chart_title = chart_->title;\n",
)
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

# Document the three user-visible Android features.
changelog = Path("CHANGELOG.md")
value = changelog.read_text(encoding="utf-8")
marker = "- Android gameplay now exposes touch `F3` and `CAP` controls"
if marker not in value:
    additions = (
        "- Android gameplay now exposes touch `F3` and `CAP` controls; `CAP` records a 10-second on-device performance report (frame/gameplay/Lua/note/cache/PVD/PFC/geometry/present, streaming pressure, memory plus ART/GC snapshots) and publishes it to `Downloads/PulseForge` for direct sharing without ADB or a PC.\n"
        "- Adds a persisted `Lua scripts: On/Off` option that globally disables chart Lua loading while preserving explicit command-line `--no-lua` precedence.\n"
        "- Android Rendering Mode no longer searches for `ffmpeg.exe`: the APK links the maintained FFmpegKit Android AAR, feeds the existing bounded raw-frame queue through an app-private FFmpeg pipe, uses Android MediaCodec H.264, and publishes finished MP4 files to `Downloads/PulseForge`.\n"
    )
    position = value.find("\n") + 1
    changelog.write_text(value[:position] + additions + value[position:], encoding="utf-8")
    print("CHANGELOG.md: continuation patched")

print("Android runtime continuation patch complete")
