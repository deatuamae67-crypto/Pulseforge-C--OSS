cmake_minimum_required(VERSION 3.24)

project(
    PulseForge
    VERSION 0.9.7
    DESCRIPTION "A deterministic, low-latency C++ rhythm-game engine"
    LANGUAGES C CXX
)

include(FetchContent)
include(CheckIPOSupported)

option(PULSEFORGE_BUILD_APP "Build the SDL3 desktop application" ON)
option(PULSEFORGE_BUILD_TESTS "Build the deterministic core tests" ON)
option(PULSEFORGE_ENABLE_LUA "Enable the sandboxed Lua 5.4 runtime" ON)
option(PULSEFORGE_ENABLE_IPO "Enable interprocedural optimization in Release builds" ON)
option(PULSEFORGE_NATIVE_ARCH "Optimize for this machine instead of portable CPUs" OFF)
option(PULSEFORGE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(
    PULSEFORGE_STATIC_MSVC_RUNTIME
    "Link the MSVC runtime statically so release packages are self-contained"
    ON
)
option(
    PULSEFORGE_WINDOWS_GUI
    "Hide the console window for the Windows Release application"
    ON
)
option(
    PULSEFORGE_BUNDLE_FFMPEG
    "Copy/install the audited GPLv3 FFmpeg executable on Windows"
    ON
)
option(
    PULSEFORGE_BUILD_AUTOCHART
    "Build the audio/video-to-chart analysis tool"
    ON
)
option(
    PULSEFORGE_ENABLE_DISCORD_SOCIAL_SDK
    "Enable Discord Social SDK Rich Presence when a local SDK is supplied"
    ON
)
option(
    PULSEFORGE_REQUIRE_DISCORD_SOCIAL_SDK
    "Fail configure instead of silently using the no-op backend when Discord SDK was requested"
    OFF
)
set(
    PULSEFORGE_DISCORD_SDK_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/discord_social_sdk"
    CACHE PATH
    "Optional unpacked Discord Social SDK root; explicit include/library paths override it"
)
set(
    PULSEFORGE_DISCORD_SDK_INCLUDE_DIR
    ""
    CACHE PATH
    "Directory containing Discord Social SDK discordpp.h and cdiscord.h"
)
set(
    PULSEFORGE_DISCORD_SDK_LIBRARY
    ""
    CACHE FILEPATH
    "Discord Social SDK shared/import library; empty keeps the no-op backend"
)
set(
    PULSEFORGE_DISCORD_SDK_RUNTIME_LIBRARY
    ""
    CACHE FILEPATH
    "Discord Social SDK runtime shared library (for Windows: discord_partner_sdk.dll)"
)
set(
    PULSEFORGE_DISCORD_APPLICATION_ID
    "1544144634832355429"
    CACHE STRING
    "Discord Application ID compiled into PulseForge; settings.json overrides nonzero values"
)
if(NOT PULSEFORGE_DISCORD_APPLICATION_ID MATCHES "^(0|[1-9][0-9]*)$")
    message(FATAL_ERROR
        "PULSEFORGE_DISCORD_APPLICATION_ID must be 0 or an unsigned decimal Discord snowflake"
    )
endif()

# The optional Windows encoder is a separately distributed executable, but it
# is still pinned like the source dependencies below. This prevents a stale or
# locally replaced binary from being silently copied into a PulseForge build.
set(
    PULSEFORGE_FFMPEG_BUNDLE
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/ffmpeg/bin/ffmpeg.exe"
)
set(
    PULSEFORGE_FFMPEG_BUNDLE_SHA256
    "1326dde4c84ff1f96fe6b8916c5bed29e163e9b5dccf995f6f3db069d143ec5e"
)
if(PULSEFORGE_BUNDLE_FFMPEG AND WIN32
    AND EXISTS "${PULSEFORGE_FFMPEG_BUNDLE}")
    file(SHA256 "${PULSEFORGE_FFMPEG_BUNDLE}" pulseforge_ffmpeg_actual_sha256)
    string(TOLOWER
        "${pulseforge_ffmpeg_actual_sha256}"
        pulseforge_ffmpeg_actual_sha256
    )
    if(NOT "${pulseforge_ffmpeg_actual_sha256}" STREQUAL
        "${PULSEFORGE_FFMPEG_BUNDLE_SHA256}")
        message(FATAL_ERROR
            "tools/ffmpeg/bin/ffmpeg.exe does not match the audited FFmpeg "
            "8.1.2 bundle; expected ${PULSEFORGE_FFMPEG_BUNDLE_SHA256}, got "
            "${pulseforge_ffmpeg_actual_sha256}"
        )
    endif()
endif()

if(MSVC AND PULSEFORGE_STATIC_MSVC_RUNTIME)
    # Apply the same CRT choice to PulseForge and every FetchContent target.
    # This avoids requiring a separately installed VC++ Redistributable on a
    # clean Windows machine while retaining the debug CRT for Debug builds.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(FETCHCONTENT_QUIET OFF)
set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_d")

FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/archive/55f93686c01528224f448c19128836e7df245f72.tar.gz
    URL_HASH SHA256=67f4cdd9ca930c9c1e130af4a437c7fc98fab77a2846fc2d2a14b4943831f8ef
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# Large community charts can contain hundreds of thousands of notes.  The
# on-demand front-end validates and consumes those files without first
# materializing a memory-heavy JSON DOM.
set(SIMDJSON_DEVELOPER_MODE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    simdjson
    URL https://github.com/simdjson/simdjson/archive/1bcf71bd85059ab6574ea1159de9298dcc1212c5.tar.gz
    URL_HASH SHA256=e75d695a3d8caf7df3ad35c8949ae75e8134cfa6e193c068e96f0f7943da59e2
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(simdjson)

# miniz is used only by the bounded, ZIP-slip-safe mod installer. Build the
# pinned sources directly so the upstream project cannot alter global compiler
# flags or install rules.
FetchContent_Declare(
    miniz
    URL https://github.com/richgel999/miniz/archive/77d0dce8627735138c51770d1799a1ef48f2117d.tar.gz
    URL_HASH SHA256=baa14349e7c5efb4a493a03af7d2f29d0ef45d87d488567fe5360edb33f8b7ba
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR cmake/pulseforge-populate-only
)
FetchContent_MakeAvailable(miniz)
set(PULSEFORGE_MINIZ_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated/miniz")
file(MAKE_DIRECTORY "${PULSEFORGE_MINIZ_GENERATED_DIR}")
file(WRITE "${PULSEFORGE_MINIZ_GENERATED_DIR}/miniz_export.h"
    "#pragma once\n#define MINIZ_EXPORT\n"
)
add_library(pulseforge_miniz STATIC
    ${miniz_SOURCE_DIR}/miniz.c
    ${miniz_SOURCE_DIR}/miniz_tdef.c
    ${miniz_SOURCE_DIR}/miniz_tinfl.c
    ${miniz_SOURCE_DIR}/miniz_zip.c
)
target_include_directories(pulseforge_miniz PUBLIC
    ${miniz_SOURCE_DIR}
    ${PULSEFORGE_MINIZ_GENERATED_DIR}
)
target_compile_definitions(pulseforge_miniz PRIVATE
    $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
)

# 7-Zip and RAR mod packages are decoded in-process with libarchive. Both
# dependencies are pinned by upstream release/commit plus SHA-256. Only the static
# libraries and the LZMA/LZMA2 decoder needed by ordinary 7z files are built;
# no archive command-line tool or external decompressor is spawned.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(XZ_NLS OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_XZ OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_XZDEC OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_LZMADEC OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_LZMAINFO OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_SCRIPTS OFF CACHE BOOL "" FORCE)
set(XZ_DOC OFF CACHE BOOL "" FORCE)
# XZ includes CTest internally and otherwise adds its complete upstream test
# suite to PulseForge's ALL_BUILD. PulseForge tests remain enabled below via
# its own PULSEFORGE_BUILD_TESTS gate.
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    xz
    URL https://github.com/tukaani-project/xz/releases/download/v5.8.3/xz-5.8.3.tar.gz
    URL_HASH SHA256=3d3a1b973af218114f4f889bbaa2f4c037deaae0c8e815eec381c3d546b974a0
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(xz)
# If this build directory was configured previously with BUILD_TESTING=ON,
# XZ leaves a generated CTestTestfile.cmake behind when testing is disabled.
# Remove only that generated registration file so PulseForge's CTest suite
# never advertises dependency tests whose executables are intentionally absent.
file(REMOVE "${xz_BINARY_DIR}/CTestTestfile.cmake")

set(ENABLE_TAR OFF CACHE BOOL "" FORCE)
set(ENABLE_CPIO OFF CACHE BOOL "" FORCE)
set(ENABLE_CAT OFF CACHE BOOL "" FORCE)
set(ENABLE_UNZIP OFF CACHE BOOL "" FORCE)
set(ENABLE_TEST OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_ACL OFF CACHE BOOL "" FORCE)
set(ENABLE_XATTR OFF CACHE BOOL "" FORCE)
set(ENABLE_ICONV OFF CACHE BOOL "" FORCE)
set(ENABLE_ZLIB OFF CACHE BOOL "" FORCE)
set(ENABLE_BZip2 OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBXML2 OFF CACHE BOOL "" FORCE)
set(ENABLE_EXPAT OFF CACHE BOOL "" FORCE)
set(ENABLE_LZ4 OFF CACHE BOOL "" FORCE)
set(ENABLE_LZO OFF CACHE BOOL "" FORCE)
set(ENABLE_ZSTD OFF CACHE BOOL "" FORCE)
set(ENABLE_LZMA OFF CACHE BOOL "" FORCE)
set(ENABLE_CNG OFF CACHE BOOL "" FORCE)
set(MSVC_USE_STATIC_CRT ${PULSEFORGE_STATIC_MSVC_RUNTIME} CACHE BOOL "" FORCE)
FetchContent_Declare(
    libarchive
    URL https://github.com/libarchive/libarchive/archive/27cbc7827172698143e440801fc0ba39ccb4f1f5.tar.gz
    URL_HASH SHA256=81e981eff4746da4c80e6622dc827b232e6333ff2e27320a6ee8fde86c891b92
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(libarchive)
# libarchive's standalone configure searches for an already-installed
# liblzma. In this superbuild the pinned target exists in the same graph, so
# enable the reader explicitly and link it without relying on host packages.
target_include_directories(
    archive_static PRIVATE "${xz_SOURCE_DIR}/src/liblzma/api"
)
target_compile_definitions(
    archive_static PRIVATE HAVE_LZMA_H=1 HAVE_LIBLZMA=1
)
target_compile_definitions(archive_static INTERFACE LIBARCHIVE_STATIC)
target_link_libraries(archive_static liblzma)
if(WIN32)
    target_link_libraries(archive_static bcrypt)
endif()

add_library(pulseforge_core STATIC
    src/core/audio_controls.cpp
    src/autochart/autochart.cpp
    src/editor/chart_editor.cpp
    src/editor/descriptor_editors.cpp
    src/editor/editor_choices.cpp
    src/editor/editor_storage.cpp
    src/editor/streaming_chart_editor.cpp
    src/core/chart.cpp
    src/core/gameplay.cpp
    src/core/input_bindings.cpp
    src/core/media_hub.cpp
    src/core/note_render_lod.cpp
    src/core/note_types.cpp
    src/core/replay.cpp
    src/core/runtime_telemetry.cpp
    src/core/streaming_gameplay.cpp
    src/core/timing_map.cpp
    src/io/chart_loader.cpp
    src/io/content_catalog.cpp
    src/io/content_descriptors.cpp
    src/io/fast_psych_loader.cpp
    src/io/mod_installer.cpp
    src/io/musical_chart.cpp
    src/io/offline_render.cpp
    src/io/packed_chart.cpp
    src/io/packed_chart_bridge.cpp
    src/io/packed_chart_stream.cpp
    src/io/shader_catalog.cpp
    src/io/settings_loader.cpp
    src/io/stage_lua.cpp
    src/io/streaming_chart_importer.cpp
    src/io/visual_density_index.cpp
    src/io/virtual_file_system.cpp
    src/autochart/autochart_review.cpp
)
add_library(PulseForge::Core ALIAS pulseforge_core)

target_include_directories(pulseforge_core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_compile_definitions(pulseforge_core PRIVATE
    PULSEFORGE_CORE_PATCH_BUILD="v8.0.6-ps2-rsod-scope-fix"
)
target_link_libraries(pulseforge_core
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE simdjson::simdjson pulseforge_miniz archive_static
)
target_compile_features(pulseforge_core PUBLIC cxx_std_20)

function(pulseforge_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus
            /wd4324
        )
        if(PULSEFORGE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        )
        if(PULSEFORGE_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

pulseforge_set_warnings(pulseforge_core)

if(PULSEFORGE_NATIVE_ARCH)
    if(MSVC)
        target_compile_options(pulseforge_core PRIVATE /arch:AVX2)
    else()
        target_compile_options(pulseforge_core PRIVATE -march=native)
    endif()
endif()

if(PULSEFORGE_ENABLE_IPO)
    check_ipo_supported(RESULT pulseforge_ipo_supported OUTPUT pulseforge_ipo_error)
    if(pulseforge_ipo_supported)
        set_property(TARGET pulseforge_core PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set_property(TARGET pulseforge_core PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
    else()
        message(STATUS "IPO unavailable: ${pulseforge_ipo_error}")
    endif()
endif()

if(PULSEFORGE_BUILD_AUTOCHART)
    add_executable(
        pulseforge_autochart
        src/tools/autochart_main.cpp
    )
    set_property(
        TARGET pulseforge_autochart
        PROPERTY OUTPUT_NAME "pulseforge-autochart"
    )
    target_link_libraries(pulseforge_autochart PRIVATE pulseforge_core)
    target_compile_definitions(
        pulseforge_autochart
        PRIVATE PULSEFORGE_VERSION="${PROJECT_VERSION}"
    )
    pulseforge_set_warnings(pulseforge_autochart)
    if(WIN32)
        add_executable(
            pulseforge_autochart_gui WIN32
            src/tools/autochart_gui_win.cpp
        )
        set_property(
            TARGET pulseforge_autochart_gui
            PROPERTY OUTPUT_NAME "pulseforge-autochart-studio"
        )
        target_compile_definitions(
            pulseforge_autochart_gui PRIVATE UNICODE _UNICODE
        )
        target_link_libraries(
            pulseforge_autochart_gui PRIVATE
            comctl32 comdlg32 shell32 ole32
        )
        add_dependencies(pulseforge_autochart_gui pulseforge_autochart)
        pulseforge_set_warnings(pulseforge_autochart_gui)
    endif()
    add_custom_command(TARGET pulseforge_autochart POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:pulseforge_autochart>/tools/autochart"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/autochart/ml_backend.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/autochart/requirements-ml.txt"
            "$<TARGET_FILE_DIR:pulseforge_autochart>/tools/autochart"
        COMMENT "Copying optional AutoChart neural backend"
    )

    if(PULSEFORGE_ENABLE_IPO AND pulseforge_ipo_supported)
        set_property(
            TARGET pulseforge_autochart
            PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
        )
        set_property(
            TARGET pulseforge_autochart
            PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE
        )
    endif()
    if(PULSEFORGE_BUNDLE_FFMPEG AND WIN32
        AND EXISTS "${PULSEFORGE_FFMPEG_BUNDLE}")
        add_custom_command(TARGET pulseforge_autochart POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:pulseforge_autochart>/tools/ffmpeg/bin"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PULSEFORGE_FFMPEG_BUNDLE}"
                "$<TARGET_FILE_DIR:pulseforge_autochart>/tools/ffmpeg/bin/ffmpeg.exe"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/ffmpeg/LICENSE"
                "$<TARGET_FILE_DIR:pulseforge_autochart>/tools/ffmpeg/LICENSE"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/ffmpeg/README.txt"
                "$<TARGET_FILE_DIR:pulseforge_autochart>/tools/ffmpeg/README.txt"
            COMMENT "Copying trusted FFmpeg runtime for AutoChart"
        )
    endif()
endif()

if(PULSEFORGE_ENABLE_LUA)
    FetchContent_Declare(
        lua
        URL https://github.com/lua/lua/archive/6e22fedb74cf0c9b6656e9fce8b7331db847c605.tar.gz
        URL_HASH SHA256=3d7c84b8d255a831dab310c7fae723cf778fcac6ca0c6cfe46822b18f7db0af8
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(lua)

    set(PULSEFORGE_LUA_SOURCES
        ${lua_SOURCE_DIR}/lapi.c
        ${lua_SOURCE_DIR}/lauxlib.c
        ${lua_SOURCE_DIR}/lbaselib.c
        ${lua_SOURCE_DIR}/lcode.c
        ${lua_SOURCE_DIR}/lctype.c
        ${lua_SOURCE_DIR}/ldebug.c
        ${lua_SOURCE_DIR}/ldo.c
        ${lua_SOURCE_DIR}/ldump.c
        ${lua_SOURCE_DIR}/lfunc.c
        ${lua_SOURCE_DIR}/lgc.c
        ${lua_SOURCE_DIR}/llex.c
        ${lua_SOURCE_DIR}/lmathlib.c
        ${lua_SOURCE_DIR}/lmem.c
        ${lua_SOURCE_DIR}/lobject.c
        ${lua_SOURCE_DIR}/lopcodes.c
        ${lua_SOURCE_DIR}/lparser.c
        ${lua_SOURCE_DIR}/lstate.c
        ${lua_SOURCE_DIR}/lstring.c
        ${lua_SOURCE_DIR}/lstrlib.c
        ${lua_SOURCE_DIR}/ltable.c
        ${lua_SOURCE_DIR}/ltablib.c
        ${lua_SOURCE_DIR}/ltm.c
        ${lua_SOURCE_DIR}/lundump.c
        ${lua_SOURCE_DIR}/lutf8lib.c
        ${lua_SOURCE_DIR}/lvm.c
        ${lua_SOURCE_DIR}/lzio.c
    )
    add_library(pulseforge_lua STATIC ${PULSEFORGE_LUA_SOURCES})
    target_include_directories(pulseforge_lua PUBLIC ${lua_SOURCE_DIR})
    target_compile_definitions(pulseforge_lua PRIVATE
        $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
    )

    add_library(pulseforge_script STATIC
        src/script/lua_runtime.cpp
        src/script/script_manager.cpp
    )
    add_library(PulseForge::Script ALIAS pulseforge_script)
    target_include_directories(pulseforge_script PUBLIC include)
    target_link_libraries(pulseforge_script PUBLIC pulseforge_core pulseforge_lua)
    target_compile_definitions(pulseforge_script PUBLIC PULSEFORGE_HAS_LUA=1)
    pulseforge_set_warnings(pulseforge_script)
endif()

if(PULSEFORGE_BUILD_APP)
    # SDLActivity loads libSDL3.so followed by libmain.so. Desktop builds keep
    # the self-contained static SDL configuration used by existing releases.
    if(ANDROID)
        set(SDL_SHARED ON CACHE BOOL "" FORCE)
        set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    else()
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
    endif()
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        SDL3
        URL https://github.com/libsdl-org/SDL/archive/f87239e71e42da91ca317a12eefb82cfbf3393eb.tar.gz
        URL_HASH SHA256=363ab3eb2225d700bf0b67bc16349f9949125c747ac50da3ba805c2d3a61b2a0
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(SDL3)

    FetchContent_Declare(
        miniaudio
        URL https://github.com/mackron/miniaudio/archive/9634bedb5b5a2ca38c1ee7108a9358a4e233f14d.tar.gz
        URL_HASH SHA256=1a3a79b80fc6f0b0cc155e28b954a598e0ddfa2db64e2afa8466be88c476fa55
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        # PulseForge embeds the single-header implementation in audio_transport.cpp.
        # Populating a deliberately empty subdirectory prevents miniaudio's own
        # optional libraries/tools from entering our default build graph.
        SOURCE_SUBDIR cmake/pulseforge-header-only
    )
    FetchContent_MakeAvailable(miniaudio)

    FetchContent_Declare(
        stb
        URL https://github.com/nothings/stb/archive/31c1ad37456438565541f4919958214b6e762fb4.tar.gz
        URL_HASH SHA256=e4e3bba9c572a4a4148373a914d88ea0f0d11de8cc2c66739926e7eca0223319
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(stb)

    add_library(pulseforge_runtime STATIC
        src/app/application.cpp
        src/app/controls_ui.cpp
        src/app/CoreEngine.cpp
        src/app/discord_presence.cpp
        src/app/discord_token_store.cpp
        src/app/editor_ui.cpp
        src/app/intro_player.cpp
        src/app/media_routes.cpp
        src/app/sniff_bridge.cpp
        src/app/launcher.cpp
        src/app/offline_encoder.cpp
        src/app/ps2_theme.cpp
        src/app/runtime_post_effects.cpp
        src/app/runtime_scene.cpp
        src/app/streaming_editor_ui.cpp
        src/platform/audio_transport.cpp
    src/app/mobile_touch_controls.cpp
    )
    add_library(PulseForge::Runtime ALIAS pulseforge_runtime)
    target_include_directories(pulseforge_runtime
        PUBLIC include
        PRIVATE
        ${miniaudio_SOURCE_DIR}
        ${stb_SOURCE_DIR}
    )
    target_link_libraries(pulseforge_runtime PRIVATE
        pulseforge_core
        SDL3::SDL3
        $<$<BOOL:${PULSEFORGE_ENABLE_LUA}>:pulseforge_script>
    )
    # PULSEFORGE_DISCORD_SOCIAL_SDK_CROSS_PLATFORM_V2
    # Desktop can consume an unpacked SDK through explicit include/library
    # paths. Android consumes discord_partner_sdk.aar through Gradle Prefab and
    # exposes the imported discord_partner_sdk::discord_partner_sdk target.
    set(PULSEFORGE_DISCORD_SOCIAL_SDK_ACTIVE OFF)
    set(_pulseforge_discord_include "${PULSEFORGE_DISCORD_SDK_INCLUDE_DIR}")
    set(_pulseforge_discord_library "${PULSEFORGE_DISCORD_SDK_LIBRARY}")
    set(_pulseforge_discord_runtime "${PULSEFORGE_DISCORD_SDK_RUNTIME_LIBRARY}")
    if(NOT ANDROID AND EXISTS "${PULSEFORGE_DISCORD_SDK_ROOT}/include/discordpp.h")
        if(NOT _pulseforge_discord_include)
            set(_pulseforge_discord_include "${PULSEFORGE_DISCORD_SDK_ROOT}/include")
        endif()
        if(WIN32)
            if(NOT _pulseforge_discord_library)
                set(_pulseforge_discord_library "${PULSEFORGE_DISCORD_SDK_ROOT}/lib/release/discord_partner_sdk.lib")
            endif()
            if(NOT _pulseforge_discord_runtime)
                set(_pulseforge_discord_runtime "${PULSEFORGE_DISCORD_SDK_ROOT}/bin/release/discord_partner_sdk.dll")
            endif()
        elseif(APPLE)
            if(NOT _pulseforge_discord_library)
                set(_pulseforge_discord_library "${PULSEFORGE_DISCORD_SDK_ROOT}/lib/release/libdiscord_partner_sdk.dylib")
            endif()
            if(NOT _pulseforge_discord_runtime)
                set(_pulseforge_discord_runtime "${PULSEFORGE_DISCORD_SDK_ROOT}/lib/release/libdiscord_partner_sdk.dylib")
            endif()
        else()
            if(NOT _pulseforge_discord_library)
                set(_pulseforge_discord_library "${PULSEFORGE_DISCORD_SDK_ROOT}/lib/release/libdiscord_partner_sdk.so")
            endif()
            if(NOT _pulseforge_discord_runtime)
                set(_pulseforge_discord_runtime "${PULSEFORGE_DISCORD_SDK_ROOT}/lib/release/libdiscord_partner_sdk.so")
            endif()
        endif()
    endif()
    if(PULSEFORGE_ENABLE_DISCORD_SOCIAL_SDK)
        if(ANDROID)
            find_package(discord_partner_sdk CONFIG QUIET)
            if(TARGET discord_partner_sdk::discord_partner_sdk)
                target_link_libraries(
                    pulseforge_runtime PRIVATE discord_partner_sdk::discord_partner_sdk
                )
                target_compile_definitions(
                    pulseforge_runtime PRIVATE PULSEFORGE_HAS_DISCORD_SOCIAL_SDK=1
                )
                set(PULSEFORGE_DISCORD_SOCIAL_SDK_ACTIVE ON)
                message(STATUS "PulseForge Discord Social SDK: Android Prefab enabled")
            else()
                message(STATUS
                    "PulseForge Discord Social SDK: Android no-op backend "
                    "(place discord_partner_sdk.aar in app/libs or set "
                    "PULSEFORGE_DISCORD_SDK_AAR in Gradle)"
                )
            endif()
        elseif(_pulseforge_discord_include
            AND _pulseforge_discord_library
            AND EXISTS "${_pulseforge_discord_include}/discordpp.h"
            AND EXISTS "${_pulseforge_discord_include}/cdiscord.h"
            AND EXISTS "${_pulseforge_discord_library}")
            target_include_directories(
                pulseforge_runtime PRIVATE "${_pulseforge_discord_include}"
            )
            target_link_libraries(
                pulseforge_runtime PRIVATE "${_pulseforge_discord_library}"
            )
            target_compile_definitions(
                pulseforge_runtime PRIVATE PULSEFORGE_HAS_DISCORD_SOCIAL_SDK=1
            )
            set(PULSEFORGE_DISCORD_SOCIAL_SDK_ACTIVE ON)
            message(STATUS "PulseForge Discord Social SDK: desktop enabled")
        else()
            message(STATUS
                "PulseForge Discord Social SDK: no-op backend "
                "(install under PULSEFORGE_DISCORD_SDK_ROOT or set explicit "
                "include/library paths to enable on desktop)"
            )
        endif()
    else()
        message(STATUS "PulseForge Discord Social SDK: disabled by option")
    endif()

    if(PULSEFORGE_REQUIRE_DISCORD_SOCIAL_SDK
        AND NOT PULSEFORGE_DISCORD_SOCIAL_SDK_ACTIVE)
        message(FATAL_ERROR
            "Discord Social SDK was required but could not be activated. "
            "Run scripts/setup-discord-social-sdk.* or provide the explicit SDK paths."
        )
    endif()

    if(WIN32)
        # MFPlay decodes the user-provided MP4 in the existing native game
        # window; it adds no helper executable or redistributable codec bundle.
        target_link_libraries(pulseforge_runtime PRIVATE
            bcrypt
            mfplay
            mfplat
            mfuuid
            ole32
            crypt32
        )
    endif()
    if(APPLE)
        find_library(PULSEFORGE_SECURITY_FRAMEWORK Security REQUIRED)
        target_link_libraries(pulseforge_runtime PRIVATE "${PULSEFORGE_SECURITY_FRAMEWORK}")
    endif()
    if(UNIX AND NOT APPLE)
        find_package(Threads REQUIRED)
        target_link_libraries(pulseforge_runtime PRIVATE
            Threads::Threads
            ${CMAKE_DL_LIBS}
            m
        )
    endif()
    target_compile_definitions(pulseforge_runtime PRIVATE
        SDL_MAIN_HANDLED
        PULSEFORGE_VERSION="${PROJECT_VERSION}"
        PULSEFORGE_PATCH_BUILD="v0.9.7-discord-detailed-rich-presence"
        PULSEFORGE_DISCORD_APPLICATION_ID=${PULSEFORGE_DISCORD_APPLICATION_ID}ULL
        $<$<BOOL:${PULSEFORGE_ENABLE_LUA}>:PULSEFORGE_HAS_LUA=1>
    )
    pulseforge_set_warnings(pulseforge_runtime)

    if(PULSEFORGE_NATIVE_ARCH)
        if(MSVC)
            target_compile_options(pulseforge_runtime PRIVATE /arch:AVX2)
        else()
            target_compile_options(pulseforge_runtime PRIVATE -march=native)
        endif()
    endif()

    if(ANDROID)
        # SDLActivity resolves SDL_main from libmain.so. Keep the logical
        # target name stable for Gradle while emitting the required soname.
        add_library(pulseforge SHARED src/app/main.cpp)
        set_property(TARGET pulseforge PROPERTY OUTPUT_NAME "main")
        add_library(PulseForge::App ALIAS pulseforge)
    else()
        add_executable(pulseforge src/app/main.cpp)
        add_executable(PulseForge::App ALIAS pulseforge)
    endif()
    # main.cpp uses SDL's platform entry-point/filesystem headers directly on
    # Android and macOS. pulseforge_runtime deliberately keeps SDL private, so
    # the application target must link SDL as a direct usage requirement too.
    target_link_libraries(pulseforge PRIVATE pulseforge_runtime SDL3::SDL3)
    target_compile_definitions(pulseforge PRIVATE
        PULSEFORGE_VERSION="${PROJECT_VERSION}"
    )
    pulseforge_set_warnings(pulseforge)

    if(WIN32)
        set(PULSEFORGE_RC_FILE_DESCRIPTION "PulseForge rhythm-game engine")
        set(PULSEFORGE_RC_INTERNAL_NAME "pulseforge")
        set(PULSEFORGE_RC_ORIGINAL_FILENAME "pulseforge.exe")
        configure_file(
            src/app/pulseforge_version.rc.in
            "${CMAKE_CURRENT_BINARY_DIR}/generated/pulseforge.rc"
            @ONLY
        )
        target_sources(
            pulseforge
            PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/pulseforge.rc"
        )
    endif()

    if(WIN32 AND PULSEFORGE_WINDOWS_GUI)
        # A GUI-subsystem process is not synchronously awaited by PowerShell.
        # Keep an explicit console frontend for validation, CI and benchmarks;
        # both executables call the exact same main/core code.
        add_executable(pulseforge_cli src/app/main.cpp)
        set_property(TARGET pulseforge_cli PROPERTY OUTPUT_NAME "pulseforge-cli")
        target_link_libraries(
            pulseforge_cli PRIVATE pulseforge_runtime SDL3::SDL3
        )
        target_compile_definitions(pulseforge_cli PRIVATE
            PULSEFORGE_VERSION="${PROJECT_VERSION}"
            PULSEFORGE_CLI_FRONTEND=1
        )
        target_link_libraries(pulseforge_cli PRIVATE dbghelp)
        # Keep symbols for the CLI even in Release so the fatal-error handler can
        # resolve crash addresses to function names when the PDB is beside it.
        target_compile_options(pulseforge_cli PRIVATE
            "$<$<CONFIG:Release>:/Zi>"
        )
        target_link_options(pulseforge_cli PRIVATE
            "$<$<CONFIG:Release>:/DEBUG:FULL>"
            "$<$<CONFIG:Release>:/OPT:REF>"
            "$<$<CONFIG:Release>:/OPT:ICF>"
        )
        pulseforge_set_warnings(pulseforge_cli)
        set(PULSEFORGE_RC_FILE_DESCRIPTION "PulseForge command-line tools")
        set(PULSEFORGE_RC_INTERNAL_NAME "pulseforge-cli")
        set(PULSEFORGE_RC_ORIGINAL_FILENAME "pulseforge-cli.exe")
        configure_file(
            src/app/pulseforge_version.rc.in
            "${CMAKE_CURRENT_BINARY_DIR}/generated/pulseforge-cli.rc"
            @ONLY
        )
        target_sources(
            pulseforge_cli
            PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/pulseforge-cli.rc"
        )
    endif()

    # Keep Debug/RelWithDebInfo console-friendly for diagnostics and CLI tools,
    # while the distributable Release build behaves like a native game.
    if(WIN32 AND PULSEFORGE_WINDOWS_GUI)
        set_property(
            TARGET pulseforge
            PROPERTY WIN32_EXECUTABLE "$<CONFIG:Release>"
        )
        target_link_options(pulseforge PRIVATE
            "$<$<CONFIG:Release>:/ENTRY:mainCRTStartup>"
        )
    endif()

    # Package the Discord Social SDK runtime beside desktop executables. Android
    # uses the Prefab/AAR flow and therefore does not use this path.
    if(PULSEFORGE_DISCORD_SOCIAL_SDK_ACTIVE
        AND NOT ANDROID
        AND _pulseforge_discord_runtime
        AND EXISTS "${_pulseforge_discord_runtime}")
        add_custom_command(
            TARGET pulseforge POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_pulseforge_discord_runtime}"
                "$<TARGET_FILE_DIR:pulseforge>"
            COMMENT "Copying Discord Social SDK runtime beside PulseForge"
            VERBATIM
        )
        if(TARGET pulseforge_cli)
            add_custom_command(
                TARGET pulseforge_cli POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_pulseforge_discord_runtime}"
                    "$<TARGET_FILE_DIR:pulseforge_cli>"
                VERBATIM
            )
        endif()
        if(UNIX AND NOT APPLE)
            set_property(TARGET pulseforge APPEND PROPERTY BUILD_RPATH "$ORIGIN")
            set_property(TARGET pulseforge APPEND PROPERTY INSTALL_RPATH "$ORIGIN")
            if(TARGET pulseforge_cli)
                set_property(TARGET pulseforge_cli APPEND PROPERTY BUILD_RPATH "$ORIGIN")
                set_property(TARGET pulseforge_cli APPEND PROPERTY INSTALL_RPATH "$ORIGIN")
            endif()
        elseif(APPLE)
            set_property(TARGET pulseforge APPEND PROPERTY BUILD_RPATH "@loader_path")
            set_property(TARGET pulseforge APPEND PROPERTY INSTALL_RPATH "@loader_path")
            if(TARGET pulseforge_cli)
                set_property(TARGET pulseforge_cli APPEND PROPERTY BUILD_RPATH "@loader_path")
                set_property(TARGET pulseforge_cli APPEND PROPERTY INSTALL_RPATH "@loader_path")
            endif()
        endif()
    elseif(PULSEFORGE_DISCORD_SOCIAL_SDK_ACTIVE AND NOT ANDROID)
        message(FATAL_ERROR
            "Discord Social SDK is linked, but its runtime shared library is missing. "
            "A linked PulseForge build would fail in the operating-system loader before "
            "the fail-open Discord backend can run. Supply "
            "PULSEFORGE_DISCORD_SDK_RUNTIME_LIBRARY or install the complete SDK archive."
        )
    endif()

    if(PULSEFORGE_ENABLE_IPO AND pulseforge_ipo_supported)
        set_property(TARGET pulseforge_runtime PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set_property(TARGET pulseforge_runtime PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
        set_property(TARGET pulseforge PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set_property(TARGET pulseforge PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
        if(TARGET pulseforge_cli)
            set_property(TARGET pulseforge_cli PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
            set_property(TARGET pulseforge_cli PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
        endif()
    endif()

    # Gradle packages Android assets through preparePulseForgeAssets; copying
    # them into the native-library directory would produce a malformed APK.
    if(NOT ANDROID)
        add_custom_command(TARGET pulseforge POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/assets"
                "$<TARGET_FILE_DIR:pulseforge>/assets"
            COMMENT "Copying PulseForge demo assets"
        )
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/sniff")
            add_custom_command(TARGET pulseforge POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_FILE_DIR:pulseforge>/tools/sniff"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/sniff/import-sniff.ps1"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/sniff/audited-builds.json"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/sniff/README.md"
                    "$<TARGET_FILE_DIR:pulseforge>/tools/sniff"
                COMMENT "Copying the audited SNIFF conversion wrapper"
            )
        endif()
    endif()
    # Rendering is an in-engine feature, so a packaged build must be able to
    # find the trusted project-owned FFmpeg binary beside its renders folder.
    # Only ffmpeg itself is required at runtime; ffplay/ffprobe stay optional
    # developer tools in the source tree.
    if(PULSEFORGE_BUNDLE_FFMPEG AND WIN32
        AND EXISTS "${PULSEFORGE_FFMPEG_BUNDLE}")
        add_custom_command(TARGET pulseforge POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:pulseforge>/tools/ffmpeg/bin"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PULSEFORGE_FFMPEG_BUNDLE}"
                "$<TARGET_FILE_DIR:pulseforge>/tools/ffmpeg/bin/ffmpeg.exe"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/ffmpeg/LICENSE"
                "$<TARGET_FILE_DIR:pulseforge>/tools/ffmpeg/LICENSE"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/ffmpeg/README.txt"
                "$<TARGET_FILE_DIR:pulseforge>/tools/ffmpeg/README.txt"
            COMMENT "Copying trusted FFmpeg runtime for rendering mode"
        )
    endif()
endif()

if(PULSEFORGE_BUILD_TESTS)
    enable_testing()
    add_executable(pulseforge_tests tests/test_main.cpp)
    target_link_libraries(pulseforge_tests PRIVATE pulseforge_core)
    pulseforge_set_warnings(pulseforge_tests)
    if(PULSEFORGE_ENABLE_IPO AND pulseforge_ipo_supported)
        set_property(TARGET pulseforge_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set_property(TARGET pulseforge_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
    endif()
    add_test(NAME pulseforge.core COMMAND pulseforge_tests)

    add_executable(
        pulseforge_runtime_telemetry_tests
        tests/runtime_telemetry_test.cpp
    )
    target_link_libraries(
        pulseforge_runtime_telemetry_tests PRIVATE pulseforge_core
    )
    pulseforge_set_warnings(pulseforge_runtime_telemetry_tests)
    add_test(
        NAME pulseforge.runtime_telemetry
        COMMAND pulseforge_runtime_telemetry_tests
    )

    if(TARGET pulseforge_runtime)
        add_executable(
            pulseforge_discord_presence_tests
            tests/discord_presence_test.cpp
        )
        target_include_directories(
            pulseforge_discord_presence_tests
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/app"
        )
        target_compile_definitions(
            pulseforge_discord_presence_tests
            PRIVATE
                PULSEFORGE_EXPECTED_COMPILED_DISCORD_APP_ID=${PULSEFORGE_DISCORD_APPLICATION_ID}ULL
        )
        target_link_libraries(
            pulseforge_discord_presence_tests PRIVATE pulseforge_runtime SDL3::SDL3
        )
        pulseforge_set_warnings(pulseforge_discord_presence_tests)
        add_test(
            NAME pulseforge.discord_presence
            COMMAND pulseforge_discord_presence_tests
        )
    endif()

    add_executable(
        pulseforge_media_hub_tests
        tests/media_hub_test.cpp
    )
    target_link_libraries(pulseforge_media_hub_tests PRIVATE pulseforge_core)
    pulseforge_set_warnings(pulseforge_media_hub_tests)
    add_test(NAME pulseforge.media_hub COMMAND pulseforge_media_hub_tests)

    add_executable(
        pulseforge_input_binding_tests
        tests/input_bindings_test.cpp
    )
    target_link_libraries(pulseforge_input_binding_tests PRIVATE pulseforge_core)
    pulseforge_set_warnings(pulseforge_input_binding_tests)
    add_test(
        NAME pulseforge.input_bindings
        COMMAND pulseforge_input_binding_tests
    )

    add_executable(pulseforge_catalog_tests tests/content_catalog_test.cpp)
    target_link_libraries(pulseforge_catalog_tests PRIVATE pulseforge_core)
    pulseforge_set_warnings(pulseforge_catalog_tests)
    if(PULSEFORGE_ENABLE_IPO AND pulseforge_ipo_supported)
        set_property(TARGET pulseforge_catalog_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set_property(TARGET pulseforge_catalog_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
    endif()
    add_test(NAME pulseforge.catalog COMMAND pulseforge_catalog_tests)

    add_executable(
        pulseforge_chart_audio_resolution_tests
        tests/chart_audio_resolution_test.cpp
    )
    target_link_libraries(
        pulseforge_chart_audio_resolution_tests
        PRIVATE pulseforge_core
    )
    pulseforge_set_warnings(pulseforge_chart_audio_resolution_tests)
    add_test(
        NAME pulseforge.chart_audio_resolution
        COMMAND pulseforge_chart_audio_resolution_tests
    )

    add_executable(
        pulseforge_vfs_tests
        tests/virtual_file_system_test.cpp
    )
    target_link_libraries(pulseforge_vfs_tests PRIVATE pulseforge_core)
    pulseforge_set_warnings(pulseforge_vfs_tests)
    add_test(NAME pulseforge.vfs COMMAND pulseforge_vfs_tests)

    add_executable(
        pulseforge_content_descriptor_tests
        tests/content_descriptors_test.cpp
    )
    target_link_libraries(pulseforge_content_descriptor_tests PRIVATE pulseforge_core)
    pulseforge_set_warnings(pulseforge_content_descriptor_tests)
    add_test(
        NAME pulseforge.content_descriptors
        COMMAND pulseforge_content_descriptor_tests
    )

    add_executable(
        pulseforge_packed_chart_tests
        tests/packed_chart_test.cpp
    )
    target_link_libraries(pulseforge_packed_chart_tests PRIVATE pulseforge_core)
    pulseforge_set_warnings(pulseforge_packed_chart_tests)
    add_test(NAME pulseforge.packed_chart COMMAND pulseforge_packed_chart_tests)

    add_executable(
        pulseforge_musical_chart_tests
        tests/musical_chart_test.cpp
    )
    target_link_libraries(pulseforge_musical_chart_tests PRIVATE pulseforge_core)
    pulseforge_set_warnings(pulseforge_musical_chart_tests)
    add_test(
        NAME pulseforge.musical_chart
        COMMAND pulseforge_musical_chart_tests
    )

    add_executable(
        pulseforge_packed_chart_bridge_tests
        tests/packed_chart_bridge_test.cpp
    )
    target_link_libraries(pulseforge_packed_chart_bridge_tests PRIVATE pulseforge_core)
    pulseforge_set_warnings(pulseforge_packed_chart_bridge_tests)
    add_test(
        NAME pulseforge.packed_chart_bridge
        COMMAND pulseforge_packed_chart_bridge_tests
    )

    add_executable(
        pulseforge_streaming_chart_importer_tests
        tests/streaming_chart_importer_test.cpp
    )
    target_link_libraries(
        pulseforge_streaming_chart_importer_tests
        PRIVATE pulseforge_core
    )
    pulseforge_set_warnings(pulseforge_streaming_chart_importer_tests)
    add_test(
        NAME pulseforge.streaming_chart_importer
        COMMAND pulseforge_streaming_chart_importer_tests
    )

    add_executable(
        pulseforge_streaming_gameplay_tests
        tests/streaming_gameplay_test.cpp
    )
    target_link_libraries(
        pulseforge_streaming_gameplay_tests
        PRIVATE pulseforge_core
    )
    pulseforge_set_warnings(pulseforge_streaming_gameplay_tests)
    add_test(
        NAME pulseforge.streaming_gameplay
        COMMAND pulseforge_streaming_gameplay_tests
    )

    add_executable(
        pulseforge_streaming_scale_benchmark EXCLUDE_FROM_ALL
        tests/streaming_scale_benchmark.cpp
    )
    target_link_libraries(
        pulseforge_streaming_scale_benchmark
        PRIVATE pulseforge_core
    )
    pulseforge_set_warnings(pulseforge_streaming_scale_benchmark)

    add_executable(
        pulseforge_visual_density_index_tests
        tests/visual_density_index_test.cpp
    )
    target_link_libraries(
        pulseforge_visual_density_index_tests
        PRIVATE pulseforge_core
    )
    pulseforge_set_warnings(pulseforge_visual_density_index_tests)
    add_test(
        NAME pulseforge.visual_density_index
        COMMAND pulseforge_visual_density_index_tests
    )

    add_executable(
        pulseforge_shader_catalog_tests
        tests/shader_catalog_test.cpp
    )
    target_link_libraries(
        pulseforge_shader_catalog_tests
        PRIVATE pulseforge_core
    )
    pulseforge_set_warnings(pulseforge_shader_catalog_tests)
    add_test(
        NAME pulseforge.shader_catalog
        COMMAND pulseforge_shader_catalog_tests
    )

    add_executable(pulseforge_stage_lua_tests tests/stage_lua_test.cpp)
    target_link_libraries(pulseforge_stage_lua_tests PRIVATE pulseforge_core)
    pulseforge_set_warnings(pulseforge_stage_lua_tests)
    add_test(NAME pulseforge.stage_lua COMMAND pulseforge_stage_lua_tests)

    add_executable(
        pulseforge_streaming_chart_editor_tests
        tests/streaming_chart_editor_test.cpp
    )
    target_link_libraries(
        pulseforge_streaming_chart_editor_tests
        PRIVATE pulseforge_core
    )
    pulseforge_set_warnings(pulseforge_streaming_chart_editor_tests)
    add_test(
        NAME pulseforge.streaming_chart_editor
        COMMAND pulseforge_streaming_chart_editor_tests
    )

    add_executable(
        pulseforge_mod_installer_tests
        tests/mod_installer_test.cpp
    )
    target_link_libraries(
        pulseforge_mod_installer_tests
        PRIVATE pulseforge_core pulseforge_miniz archive_static
    )
    pulseforge_set_warnings(pulseforge_mod_installer_tests)
    add_test(
        NAME pulseforge.mod_installer
        COMMAND pulseforge_mod_installer_tests
    )

    add_executable(
        pulseforge_offline_render_tests
        tests/offline_render_test.cpp
    )
    target_link_libraries(pulseforge_offline_render_tests PRIVATE pulseforge_core)
    pulseforge_set_warnings(pulseforge_offline_render_tests)
    add_test(
        NAME pulseforge.offline_render
        COMMAND pulseforge_offline_render_tests
    )

    add_executable(
        pulseforge_editor_tests
        tests/editor_models_test.cpp
    )
    target_link_libraries(pulseforge_editor_tests PRIVATE pulseforge_core)
    pulseforge_set_warnings(pulseforge_editor_tests)
    add_test(NAME pulseforge.editors COMMAND pulseforge_editor_tests)

    add_executable(
        pulseforge_editor_choice_tests
        tests/editor_choices_test.cpp
    )
    target_link_libraries(
        pulseforge_editor_choice_tests
        PRIVATE pulseforge_core
    )
    pulseforge_set_warnings(pulseforge_editor_choice_tests)
    add_test(
        NAME pulseforge.editor_choices
        COMMAND pulseforge_editor_choice_tests
    )

    add_executable(
        pulseforge_note_type_tests
        tests/note_types_test.cpp
    )
    target_link_libraries(pulseforge_note_type_tests PRIVATE pulseforge_core)
    pulseforge_set_warnings(pulseforge_note_type_tests)
    add_test(NAME pulseforge.note_types COMMAND pulseforge_note_type_tests)

    if(PULSEFORGE_ENABLE_LUA)
        add_executable(pulseforge_lua_tests tests/lua_test.cpp)
        target_link_libraries(pulseforge_lua_tests PRIVATE pulseforge_script)
        pulseforge_set_warnings(pulseforge_lua_tests)
        if(PULSEFORGE_ENABLE_IPO AND pulseforge_ipo_supported)
            set_property(TARGET pulseforge_lua_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
            set_property(TARGET pulseforge_lua_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
        endif()
        add_test(NAME pulseforge.lua COMMAND pulseforge_lua_tests)
    endif()

    if(PULSEFORGE_BUILD_APP)
        add_executable(
            pulseforge_audio_transport_tests
            tests/audio_transport_test.cpp
        )
        target_link_libraries(
            pulseforge_audio_transport_tests
            PRIVATE pulseforge_runtime
        )
        pulseforge_set_warnings(pulseforge_audio_transport_tests)
        add_test(
            NAME pulseforge.audio_transport
            COMMAND pulseforge_audio_transport_tests
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/menu/watch-dogs-fixer-ambush-jury-rigged.mp3"
        )

        add_executable(pulseforge_fake_ffmpeg tests/fake_ffmpeg.cpp)
        set_property(TARGET pulseforge_fake_ffmpeg PROPERTY OUTPUT_NAME ffmpeg)
        pulseforge_set_warnings(pulseforge_fake_ffmpeg)

        add_executable(
            pulseforge_offline_encoder_tests
            tests/offline_encoder_test.cpp
            src/app/offline_encoder.cpp
        )
        target_include_directories(
            pulseforge_offline_encoder_tests
            PRIVATE src/app include
        )
        target_link_libraries(
            pulseforge_offline_encoder_tests
            PRIVATE SDL3::SDL3
        )
        target_compile_definitions(
            pulseforge_offline_encoder_tests
            PRIVATE
            PULSEFORGE_FAKE_FFMPEG="$<TARGET_FILE:pulseforge_fake_ffmpeg>"
        )
        add_dependencies(
            pulseforge_offline_encoder_tests
            pulseforge_fake_ffmpeg
        )
        pulseforge_set_warnings(pulseforge_offline_encoder_tests)
        add_test(
            NAME pulseforge.offline_encoder
            COMMAND pulseforge_offline_encoder_tests
        )

        add_executable(
            pulseforge_editor_ui_tests
            tests/editor_ui_test.cpp
        )
        target_include_directories(
            pulseforge_editor_ui_tests
            PRIVATE src/app
        )
        target_link_libraries(
            pulseforge_editor_ui_tests
            PRIVATE pulseforge_runtime SDL3::SDL3
        )
        pulseforge_set_warnings(pulseforge_editor_ui_tests)
        add_test(
            NAME pulseforge.editor_ui
            COMMAND pulseforge_editor_ui_tests
        )

        add_executable(
            pulseforge_intro_tests
            tests/intro_player_test.cpp
        )
        target_include_directories(
            pulseforge_intro_tests
            PRIVATE src/app
        )
        target_link_libraries(
            pulseforge_intro_tests
            PRIVATE pulseforge_runtime SDL3::SDL3
        )
        pulseforge_set_warnings(pulseforge_intro_tests)
        add_test(
            NAME pulseforge.intro
            COMMAND pulseforge_intro_tests
        )
        add_test(
            NAME pulseforge.intro_decoded
            COMMAND pulseforge_intro_tests
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/intro/watch-dogs-boot-FiL0S0V.mp4"
        )

        add_executable(
            pulseforge_media_routes_tests
            tests/media_routes_test.cpp
            src/app/media_routes.cpp
        )
        target_include_directories(
            pulseforge_media_routes_tests
            PRIVATE src/app
        )
        pulseforge_set_warnings(pulseforge_media_routes_tests)
        add_test(
            NAME pulseforge.media_routes
            COMMAND pulseforge_media_routes_tests
        )

        if(WIN32)
            add_executable(pulseforge_fake_sniff tests/fake_sniff.cpp)
            set_property(
                TARGET pulseforge_fake_sniff
                PROPERTY OUTPUT_NAME "sniff-rusted-fake"
            )
            pulseforge_set_warnings(pulseforge_fake_sniff)
            add_test(
                NAME pulseforge.sniff_wrapper
                COMMAND powershell.exe
                    -NoLogo
                    -NoProfile
                    -NonInteractive
                    -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/sniff_wrapper_test.ps1"
                    -Wrapper "${CMAKE_CURRENT_SOURCE_DIR}/tools/sniff/import-sniff.ps1"
                    -FakeSniff "$<TARGET_FILE:pulseforge_fake_sniff>"
                    -WorkRoot "${CMAKE_CURRENT_BINARY_DIR}/sniff-wrapper-tests"
            )

            add_executable(
                pulseforge_sniff_bridge_integrity_tests
                tests/sniff_bridge_integrity_test.cpp
                src/app/sniff_bridge.cpp
            )
            target_include_directories(
                pulseforge_sniff_bridge_integrity_tests
                PRIVATE src/app
            )
            target_link_libraries(
                pulseforge_sniff_bridge_integrity_tests
                PRIVATE bcrypt
            )
            pulseforge_set_warnings(pulseforge_sniff_bridge_integrity_tests)
            add_test(
                NAME pulseforge.sniff_bridge_integrity
                COMMAND pulseforge_sniff_bridge_integrity_tests
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/sniff/import-sniff.ps1"
            )
        endif()

        add_executable(
            pulseforge_psych_content_root_tests
            tests/psych_content_roots_test.cpp
        )
        target_include_directories(
            pulseforge_psych_content_root_tests
            PRIVATE
                src/app
                include
        )
        # PULSEFORGE_P1_2_0A_PSYCH_CONTENT_ROOT_TEST_INCLUDE_V1
        # psych_content_roots.hpp now includes public PulseForge headers; keep
        # this standalone regression target on the same public include surface.
        pulseforge_set_warnings(pulseforge_psych_content_root_tests)
        add_test(
            NAME pulseforge.psych_content_roots
            COMMAND pulseforge_psych_content_root_tests
        )

        add_executable(
            pulseforge_psych_character_semantics_tests
            tests/psych_character_semantics_test.cpp
        )
        target_include_directories(
            pulseforge_psych_character_semantics_tests
            PRIVATE src/app
        )
        pulseforge_set_warnings(pulseforge_psych_character_semantics_tests)
        add_test(
            NAME pulseforge.psych_character_semantics
            COMMAND pulseforge_psych_character_semantics_tests
        )

        add_executable(
            pulseforge_psych_text_layout_tests
            tests/psych_text_layout_test.cpp
        )
        target_include_directories(
            pulseforge_psych_text_layout_tests
            PRIVATE src/app
        )
        pulseforge_set_warnings(pulseforge_psych_text_layout_tests)
        add_test(
            NAME pulseforge.psych_text_layout
            COMMAND pulseforge_psych_text_layout_tests
        )

        add_executable(
            pulseforge_runtime_texture_policy_tests
            tests/runtime_texture_policy_test.cpp
        )
        target_include_directories(
            pulseforge_runtime_texture_policy_tests
            PRIVATE src/app
        )
        pulseforge_set_warnings(pulseforge_runtime_texture_policy_tests)
        add_test(
            NAME pulseforge.runtime_texture_policy
            COMMAND pulseforge_runtime_texture_policy_tests
        )

        add_executable(
            pulseforge_psych_camera_target_tests
            tests/psych_camera_target_test.cpp
        )
        target_include_directories(
            pulseforge_psych_camera_target_tests
            PRIVATE src/app
        )
        pulseforge_set_warnings(pulseforge_psych_camera_target_tests)
        add_test(
            NAME pulseforge.psych_camera_target
            COMMAND pulseforge_psych_camera_target_tests
        )

        add_executable(
            pulseforge_psych_wavy_effect_tests
            tests/psych_wavy_effect_test.cpp
        )
        target_include_directories(
            pulseforge_psych_wavy_effect_tests
            PRIVATE src/app
        )
        pulseforge_set_warnings(pulseforge_psych_wavy_effect_tests)
        add_test(
            NAME pulseforge.psych_wavy_effect
            COMMAND pulseforge_psych_wavy_effect_tests
        )

        add_executable(
            pulseforge_psych_shader_compat_tests
            tests/psych_shader_compat_test.cpp
        )
        target_include_directories(
            pulseforge_psych_shader_compat_tests
            PRIVATE src/app
        )
        pulseforge_set_warnings(pulseforge_psych_shader_compat_tests)
        add_test(
            NAME pulseforge.psych_shader_compat
            COMMAND pulseforge_psych_shader_compat_tests
        )

        add_executable(
            pulseforge_runtime_note_skin_tests
            tests/runtime_note_skin_test.cpp
        )
        target_include_directories(
            pulseforge_runtime_note_skin_tests
            PRIVATE src/app
        )
        target_link_libraries(
            pulseforge_runtime_note_skin_tests
            PRIVATE pulseforge_runtime SDL3::SDL3
        )
        pulseforge_set_warnings(pulseforge_runtime_note_skin_tests)
        add_test(
            NAME pulseforge.runtime_note_skins
            COMMAND pulseforge_runtime_note_skin_tests
        )

        add_executable(pulseforge_runtime_tests tests/core_engine_test.cpp)
        target_include_directories(
            pulseforge_runtime_tests
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/app"
        )
        target_compile_definitions(
            pulseforge_runtime_tests
            PRIVATE
                PULSEFORGE_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
                PULSEFORGE_TEST_FFMPEG_PATH="$<TARGET_FILE:pulseforge_fake_ffmpeg>"
        )
        add_dependencies(pulseforge_runtime_tests pulseforge_fake_ffmpeg)
        target_link_libraries(
            pulseforge_runtime_tests
            PRIVATE pulseforge_runtime SDL3::SDL3
        )
        pulseforge_set_warnings(pulseforge_runtime_tests)
        if(PULSEFORGE_ENABLE_IPO AND pulseforge_ipo_supported)
            set_property(TARGET pulseforge_runtime_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
            set_property(TARGET pulseforge_runtime_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
        endif()
        add_test(NAME pulseforge.runtime COMMAND pulseforge_runtime_tests)
    endif()
endif()

if(PULSEFORGE_BUILD_APP)
    if(ANDROID)
        install(TARGETS pulseforge LIBRARY DESTINATION lib)
    else()
        install(TARGETS pulseforge RUNTIME DESTINATION bin)
    endif()
    if(TARGET pulseforge_cli)
        install(TARGETS pulseforge_cli RUNTIME DESTINATION bin)
    endif()
    if(PULSEFORGE_DISCORD_SOCIAL_SDK_ACTIVE
        AND NOT ANDROID
        AND _pulseforge_discord_runtime
        AND EXISTS "${_pulseforge_discord_runtime}")
        install(
            FILES "${_pulseforge_discord_runtime}"
            DESTINATION bin
        )
    endif()
    install(DIRECTORY assets/ DESTINATION bin/assets)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/mods")
        install(DIRECTORY mods/ DESTINATION bin/mods)
    endif()
    if(PULSEFORGE_BUNDLE_FFMPEG AND WIN32
        AND EXISTS "${PULSEFORGE_FFMPEG_BUNDLE}")
        install(
            PROGRAMS "${PULSEFORGE_FFMPEG_BUNDLE}"
            DESTINATION bin/tools/ffmpeg/bin
        )
        install(
            FILES tools/ffmpeg/LICENSE tools/ffmpeg/README.txt
            DESTINATION bin/tools/ffmpeg
        )
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/sniff")
        install(
            FILES
                tools/sniff/import-sniff.ps1
                tools/sniff/audited-builds.json
                tools/sniff/README.md
            DESTINATION bin/tools/sniff
        )
    endif()
endif()
if(TARGET pulseforge_autochart)
    install(TARGETS pulseforge_autochart RUNTIME DESTINATION bin)
    if(TARGET pulseforge_autochart_gui)
        install(TARGETS pulseforge_autochart_gui RUNTIME DESTINATION bin)
    endif()
    install(
        FILES
            tools/autochart/ml_backend.py
            tools/autochart/requirements-ml.txt
        DESTINATION bin/tools/autochart
    )
    install(
        FILES
            scripts/setup-autochart-ml.ps1
            scripts/test-autochart-ml.ps1
            scripts/test-autochart-final.ps1
            scripts/autochart-studio.ps1
        DESTINATION bin/scripts
    )
endif()
install(FILES README.md LICENSE NOTICE THIRD_PARTY.md DESTINATION .)
install(DIRECTORY docs/ DESTINATION docs)
install(DIRECTORY THIRD_PARTY_LICENSES/ DESTINATION THIRD_PARTY_LICENSES)
