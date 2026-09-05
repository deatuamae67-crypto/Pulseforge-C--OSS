#include "application_runner.hpp"
#include "discord_presence.hpp"
#include "offline_encoder.hpp"
#include "ps2_theme.hpp"
#include "psych_content_roots.hpp"
#include "psych_camera_target.hpp"
#include "psych_text_layout.hpp"
#include "runtime_post_effects.hpp"
#include "runtime_scene.hpp"
#include "mobile_touch_controls.hpp"
#include "sdl_input_actions.hpp"

#include "pulseforge/audio_controls.hpp"
#include "pulseforge/audio_transport.hpp"
#include "pulseforge/gameplay.hpp"
#include "pulseforge/musical_chart.hpp"
#include "pulseforge/note_render_lod.hpp"
#include "pulseforge/note_skin_catalog.hpp"
#include "pulseforge/note_types.hpp"
#include "pulseforge/replay.hpp"
#include "pulseforge/shader_catalog.hpp"
#include "pulseforge/streaming_chart_importer.hpp"
#include "pulseforge/streaming_gameplay.hpp"
#include "pulseforge/visual_density_index.hpp"

#ifndef PULSEFORGE_PATCH_BUILD
#define PULSEFORGE_PATCH_BUILD "unversioned-runtime"
#endif

#if defined(PULSEFORGE_HAS_LUA)
#include "pulseforge/script_manager.hpp"
#endif

#include <SDL3/SDL.h>
#include <stb_image.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulseforge::detail {

TransferredPlatform::TransferredPlatform(
    SDL_Window* const window_value,
    SDL_Renderer* const renderer_value,
    const bool owns_sdl_value
) noexcept
    : window(window_value),
      renderer(renderer_value),
      owns_sdl(owns_sdl_value) {}

TransferredPlatform::~TransferredPlatform() noexcept {
    reset();
}

TransferredPlatform::TransferredPlatform(TransferredPlatform&& other) noexcept
    : window(std::exchange(other.window, nullptr)),
      renderer(std::exchange(other.renderer, nullptr)),
      owns_sdl(std::exchange(other.owns_sdl, false)) {}

TransferredPlatform& TransferredPlatform::operator=(
    TransferredPlatform&& other
) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    window = std::exchange(other.window, nullptr);
    renderer = std::exchange(other.renderer, nullptr);
    owns_sdl = std::exchange(other.owns_sdl, false);
    return *this;
}

void TransferredPlatform::adopt(
    SDL_Window* const window_value,
    SDL_Renderer* const renderer_value,
    const bool owns_sdl_value
) noexcept {
    reset();
    window = window_value;
    renderer = renderer_value;
    owns_sdl = owns_sdl_value;
}

void TransferredPlatform::detach(
    SDL_Window*& window_output,
    SDL_Renderer*& renderer_output,
    bool& owns_sdl_output
) noexcept {
    window_output = std::exchange(window, nullptr);
    renderer_output = std::exchange(renderer, nullptr);
    owns_sdl_output = std::exchange(owns_sdl, false);
}

void TransferredPlatform::reset() noexcept {
    if (renderer != nullptr) {
        SDL_DestroyRenderer(renderer);
        renderer = nullptr;
    }
    if (window != nullptr) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }
    if (owns_sdl) {
        SDL_Quit();
        owns_sdl = false;
    }
}

bool TransferredPlatform::complete() const noexcept {
    return window != nullptr && renderer != nullptr;
}

}  // namespace pulseforge::detail

namespace pulseforge {
namespace {

template <typename Function>
class ScopeExit final {
public:
    explicit ScopeExit(Function function) noexcept
        : function_(std::move(function)) {}

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ScopeExit(ScopeExit&&) = delete;
    ScopeExit& operator=(ScopeExit&&) = delete;

    ~ScopeExit() noexcept { function_(); }

private:
    Function function_;
};

template <typename Function>
ScopeExit(Function) -> ScopeExit<Function>;

constexpr float logical_width = 1280.0F;
constexpr float logical_height = 720.0F;
constexpr std::size_t timing_history_size = 160;
constexpr std::size_t particle_capacity = 256;
constexpr std::array<float, 8> particle_direction_x{
    1.0F,
    0.70710678F,
    0.0F,
    -0.70710678F,
    -1.0F,
    -0.70710678F,
    0.0F,
    0.70710678F,
};
constexpr std::array<float, 8> particle_direction_y{
    0.0F,
    0.70710678F,
    1.0F,
    0.70710678F,
    0.0F,
    -0.70710678F,
    -1.0F,
    -0.70710678F,
};
constexpr std::uintmax_t maximum_script_file_bytes = 2U * 1024U * 1024U;

enum class ContentLoadPhase : std::uint8_t {
    inspecting,
    parsing_chart,
    preparing_stream_cache,
    indexing_notes,
    creating_session,
    loading_scripts,
    complete,
};

struct ContentLoadPhaseDisplay {
    std::string_view label;
};

[[nodiscard]] constexpr ContentLoadPhaseDisplay load_phase_display(
    const ContentLoadPhase phase
) noexcept {
    switch (phase) {
    case ContentLoadPhase::inspecting:
        return {"inspecting source"};
    case ContentLoadPhase::parsing_chart:
        return {"parsing and validating chart"};
    case ContentLoadPhase::preparing_stream_cache:
        return {"building or verifying PFC1 cache"};
    case ContentLoadPhase::indexing_notes:
        return {"indexing note spans"};
    case ContentLoadPhase::creating_session:
        return {"creating gameplay session"};
    case ContentLoadPhase::loading_scripts:
        return {"loading gameplay scripts"};
    case ContentLoadPhase::complete:
        return {"ready"};
    }
    return {"working"};
}

struct Particle {
    float x{};
    float y{};
    float velocity_x{};
    float velocity_y{};
    float life{};
    SDL_Color color{};
    bool active{};
};

// PULSEFORGE_P1_5_0C_NOTE_SPLASH_ANIMATION_POOL_V1
constexpr std::size_t note_splash_animation_capacity = 64U;
struct NoteSplashAnimation {
    detail::RuntimeNoteSplashProfile profile{
        detail::runtime_note_splash_invalid_profile
    };
    float center_x{};
    float center_y{};
    float elapsed{};
    float duration{0.45F};
    bool active{};
};

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {
        reinterpret_cast<const char*>(value.data()),
        value.size(),
    };
}

[[nodiscard]] std::string_view audio_visualizer_icon_filename(
    const AudioVisualizerImage image
) noexcept {
    switch (image) {
    case AudioVisualizerImage::star_of_david:
        return "audio_visualizer_star.png";
    case AudioVisualizerImage::circular_symbol:
        return "audio_visualizer_circular_symbol.png";
    case AudioVisualizerImage::hammer_sickle_star:
        return "audio_visualizer_hammer_sickle_star.png";
    case AudioVisualizerImage::custom:
        return "<custom>";
    }
    return "audio_visualizer_star.png";
}

[[nodiscard]] std::filesystem::path ps2_theme_movie_path(
    const AppLaunchOptions& options,
    const std::string_view filename
) {
    const auto relative = std::filesystem::path{"intro/ps2"} / std::string(filename);
    std::error_code error;
    for (const auto& root : options.content_roots) {
        const auto candidate = root / relative;
        if (std::filesystem::is_regular_file(candidate, error) && !error) return candidate;
        error.clear();
    }
    const auto assets_candidate = std::filesystem::path{"assets"} / relative;
    if (std::filesystem::is_regular_file(assets_candidate, error) && !error) return assets_candidate;
    return options.content_roots.empty() ? relative : options.content_roots.front() / relative;
}

[[nodiscard]] std::optional<std::filesystem::path>
audio_visualizer_icon_path(const AppLaunchOptions& options) {
    // PULSEFORGE_P1_5_0E_CUSTOM_VISUALIZER_IMAGE_V1
    if (options.settings.visual.audio_visualizer_image
            == AudioVisualizerImage::custom
        && !options.settings.visual.audio_visualizer_custom_image_path.empty()) {
        const std::filesystem::path custom(
            options.settings.visual.audio_visualizer_custom_image_path
        );
        auto extension = custom.extension().string();
        std::transform(
            extension.begin(), extension.end(), extension.begin(),
            [](const unsigned char value) {
                return static_cast<char>(std::tolower(value));
            }
        );
        const bool supported = extension == ".png" || extension == ".jpg"
            || extension == ".jpeg" || extension == ".bmp";
        std::error_code custom_error;
        if (supported && std::filesystem::is_regular_file(custom, custom_error)
            && !custom_error) {
            return custom;
        }
    }

    const std::filesystem::path relative =
        std::filesystem::path{"ui"}
        / audio_visualizer_icon_filename(
            options.settings.visual.audio_visualizer_image
                == AudioVisualizerImage::custom
                ? AudioVisualizerImage::star_of_david
                : options.settings.visual.audio_visualizer_image
        );
    std::error_code error;
    for (const auto& root : options.content_roots) {
        const auto candidate = root / relative;
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            return candidate;
        }
        error.clear();
    }
    const std::array fallbacks{
        std::filesystem::path{"assets"} / relative,
        std::filesystem::path{relative},
    };
    for (const auto& candidate : fallbacks) {
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            return candidate;
        }
        error.clear();
    }
    return std::nullopt;
}

[[nodiscard]] SDL_Texture* load_rgba_texture(
    SDL_Renderer* const renderer,
    const std::filesystem::path& path
) {
    if (renderer == nullptr) {
        return nullptr;
    }
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return nullptr;
    }
    const auto end = input.tellg();
    constexpr std::uintmax_t maximum_icon_bytes = 8U * 1024U * 1024U;
    if (end <= std::streampos{0}
        || static_cast<std::uintmax_t>(end) > maximum_icon_bytes) {
        return nullptr;
    }
    std::vector<stbi_uc> encoded(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    input.read(
        reinterpret_cast<char*>(encoded.data()),
        static_cast<std::streamsize>(encoded.size())
    );
    if (!input) {
        return nullptr;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    // PULSEFORGE_P1_5_0E_BOUNDED_CUSTOM_VISUALIZER_DECODE_V1
    constexpr int maximum_icon_dimension = 4'096;
    constexpr std::uint64_t maximum_icon_pixels = 16U * 1024U * 1024U;
    if (stbi_info_from_memory(
            encoded.data(),
            static_cast<int>(encoded.size()),
            &width,
            &height,
            &channels
        ) == 0
        || width <= 0 || height <= 0
        || width > maximum_icon_dimension || height > maximum_icon_dimension
        || static_cast<std::uint64_t>(width)
                * static_cast<std::uint64_t>(height) > maximum_icon_pixels) {
        return nullptr;
    }
    stbi_uc* const pixels = stbi_load_from_memory(
        encoded.data(),
        static_cast<int>(encoded.size()),
        &width,
        &height,
        &channels,
        STBI_rgb_alpha
    );
    if (pixels == nullptr || width <= 0 || height <= 0) {
        if (pixels != nullptr) {
            stbi_image_free(pixels);
        }
        return nullptr;
    }
    SDL_Surface* const surface = SDL_CreateSurfaceFrom(
        width,
        height,
        SDL_PIXELFORMAT_RGBA32,
        pixels,
        width * 4
    );
    if (surface == nullptr) {
        stbi_image_free(pixels);
        return nullptr;
    }
    SDL_Texture* const texture = SDL_CreateTextureFromSurface(
        renderer,
        surface
    );
    SDL_DestroySurface(surface);
    stbi_image_free(pixels);
    if (texture != nullptr) {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    }
    return texture;
}

void draw_star_of_david_fallback(
    SDL_Renderer* const renderer,
    const float center_x,
    const float center_y,
    const float radius,
    const float horizontal_scale,
    const SDL_Color color
) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    constexpr float root_three_over_two = 0.8660254F;
    const auto project_x = [center_x, horizontal_scale](const float x) {
        return center_x + (x - center_x) * horizontal_scale;
    };
    const std::array<SDL_FPoint, 3> up{{
        {center_x, center_y - radius},
        {project_x(center_x - radius * root_three_over_two),
         center_y + radius * 0.5F},
        {project_x(center_x + radius * root_three_over_two),
         center_y + radius * 0.5F},
    }};
    const std::array<SDL_FPoint, 3> down{{
        {center_x, center_y + radius},
        {project_x(center_x - radius * root_three_over_two),
         center_y - radius * 0.5F},
        {project_x(center_x + radius * root_three_over_two),
         center_y - radius * 0.5F},
    }};
    const auto draw_triangle = [renderer](const auto& points) {
        SDL_RenderLine(
            renderer,
            points[0].x,
            points[0].y,
            points[1].x,
            points[1].y
        );
        SDL_RenderLine(
            renderer,
            points[1].x,
            points[1].y,
            points[2].x,
            points[2].y
        );
        SDL_RenderLine(
            renderer,
            points[2].x,
            points[2].y,
            points[0].x,
            points[0].y
        );
    };
    draw_triangle(up);
    draw_triangle(down);
}

[[nodiscard]] std::vector<std::filesystem::path> pause_music_catalog(
    const AppLaunchOptions& options
) {
    std::vector<std::filesystem::path> candidates;
    std::error_code error;
    for (const auto& root : options.content_roots) {
        const auto directory = root / "menu";
        if (!std::filesystem::is_directory(directory, error) || error) {
            error.clear();
            continue;
        }
        for (std::filesystem::directory_iterator iterator(directory, error), end;
             iterator != end && !error;
            iterator.increment(error)) {
            if (iterator->is_regular_file(error) && !error) {
                auto extension = iterator->path().extension().string();
                std::transform(
                    extension.begin(),
                    extension.end(),
                    extension.begin(),
                    [](const unsigned char value) {
                        return value >= 'A' && value <= 'Z'
                            ? static_cast<char>(value - 'A' + 'a')
                            : static_cast<char>(value);
                    }
                );
                if (extension == ".mp3" || extension == ".ogg"
                    || extension == ".wav" || extension == ".flac") {
                    candidates.push_back(iterator->path());
                }
            }
            error.clear();
        }
        error.clear();
    }
    if (candidates.empty()) {
        constexpr std::string_view fallback =
            "menu/watch-dogs-fixer-ambush-jury-rigged.mp3";
        candidates.push_back(options.content_roots.empty()
            ? std::filesystem::path(fallback)
            : options.content_roots.front() / fallback);
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(
        std::unique(candidates.begin(), candidates.end()),
        candidates.end()
    );
    return candidates;
}

[[nodiscard]] std::optional<double> finite_number(
    const std::string_view text
) noexcept {
    if (text.empty() || text.size() > 128U) {
        return std::nullopt;
    }
    std::array<char, 129> buffer{};
    std::copy(text.begin(), text.end(), buffer.begin());
    char* end = nullptr;
    const double value = std::strtod(buffer.data(), &end);
    if (end == buffer.data() || *end != '\0' || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}


[[nodiscard]] bool parse_psych_color(
    std::string_view text,
    SDL_Color& color
) noexcept {
    if (!text.empty() && text.front() == '#') {
        text.remove_prefix(1U);
    } else if (text.size() > 2U && text[0] == '0'
        && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2U);
    }
    if (text.size() != 6U && text.size() != 8U) {
        return false;
    }

    std::uint32_t raw{};
    const auto parsed = std::from_chars(
        text.data(),
        text.data() + text.size(),
        raw,
        16
    );
    if (parsed.ec != std::errc{}
        || parsed.ptr != text.data() + text.size()) {
        return false;
    }

    if (text.size() == 8U) {
        color.a = static_cast<std::uint8_t>((raw >> 24U) & 0xFFU);
        color.r = static_cast<std::uint8_t>((raw >> 16U) & 0xFFU);
        color.g = static_cast<std::uint8_t>((raw >> 8U) & 0xFFU);
        color.b = static_cast<std::uint8_t>(raw & 0xFFU);
    } else {
        color.r = static_cast<std::uint8_t>((raw >> 16U) & 0xFFU);
        color.g = static_cast<std::uint8_t>((raw >> 8U) & 0xFFU);
        color.b = static_cast<std::uint8_t>(raw & 0xFFU);
        color.a = 255U;
    }
    return true;
}

[[nodiscard]] std::string read_text_file(
    const std::filesystem::path& path,
    std::string* error = nullptr
) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        if (error != nullptr) {
            *error = "cannot open file: " + path_utf8(path);
        }
        return {};
    }
    const auto end = input.tellg();
    if (end < std::streampos{0}
        || static_cast<std::uintmax_t>(end) > maximum_script_file_bytes) {
        if (error != nullptr) {
            *error = "script exceeds the 2 MiB safety limit: " + path_utf8(path);
        }
        return {};
    }
    std::string source(static_cast<std::size_t>(end), '\0');
    input.seekg(0, std::ios::beg);
    if (!source.empty()) {
        input.read(source.data(), static_cast<std::streamsize>(source.size()));
    }
    if (!input) {
        if (error != nullptr) {
            *error = "failed while reading file: " + path_utf8(path);
        }
        return {};
    }
    return source;
}

#if defined(PULSEFORGE_HAS_LUA)
struct DiscoveredScript {
    std::filesystem::path path;
    ScriptOrigin origin{ScriptOrigin::explicit_file};
    std::int32_t priority{};
};

[[nodiscard]] bool safe_script_id(const std::string_view value) noexcept {
    return !value.empty() && value != "." && value != ".."
        && value.find('/') == std::string_view::npos
        && value.find('\\') == std::string_view::npos
        && value.find(':') == std::string_view::npos
        && value.find('\0') == std::string_view::npos;
}

[[nodiscard]] bool path_is_within(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate
) {
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
    if (relative.empty() || relative.is_absolute()
        || relative.has_root_name() || relative.has_root_directory()) {
        return false;
    }
    return std::none_of(relative.begin(), relative.end(), [](const auto& part) {
        return part == "..";
    });
}

[[nodiscard]] std::optional<std::filesystem::path> contained_lua_file(
    const std::filesystem::path& root,
    const std::filesystem::path& relative
) {
    if (root.empty() || relative.empty() || relative.is_absolute()
        || relative.has_root_name() || relative.has_root_directory()) {
        return std::nullopt;
    }
    for (const auto& part : relative) {
        if (part == "..") {
            return std::nullopt;
        }
    }
    const auto candidate = root / relative;
    std::error_code error;
    if (!std::filesystem::is_regular_file(candidate, error) || error
        || !path_is_within(root, candidate)) {
        return std::nullopt;
    }
    return std::filesystem::weakly_canonical(candidate, error);
}

void append_lua_tree(
    std::vector<DiscoveredScript>& result,
    const std::filesystem::path& root,
    const std::filesystem::path& relative,
    const ScriptOrigin origin,
    const std::int32_t priority
) {
    const auto directory = root / relative;
    if (!path_is_within(root, directory)) {
        return;
    }
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error) {
        return;
    }
    std::filesystem::recursive_directory_iterator iterator(
        directory,
        std::filesystem::directory_options::skip_permission_denied,
        error
    );
    const std::filesystem::recursive_directory_iterator end;
    std::vector<std::filesystem::path> paths;
    paths.reserve(32);
    for (; !error && iterator != end && paths.size() < 1'024U;
         iterator.increment(error)) {
        if (iterator.depth() >= 8) {
            iterator.disable_recursion_pending();
        }
        std::error_code status_error;
        if (iterator->is_symlink(status_error)) {
            if (iterator->is_directory(status_error)) {
                iterator.disable_recursion_pending();
            }
            continue;
        }
        if (status_error || !iterator->is_regular_file(status_error)
            || status_error) {
            continue;
        }
        auto extension = path_utf8(iterator->path().extension());
        std::transform(
            extension.begin(),
            extension.end(),
            extension.begin(),
            [](const unsigned char value) {
                return value >= 'A' && value <= 'Z'
                    ? static_cast<char>(value - 'A' + 'a')
                    : static_cast<char>(value);
            }
        );
        if (extension == ".lua" && path_is_within(root, iterator->path())) {
            paths.push_back(iterator->path());
        }
    }
    std::sort(paths.begin(), paths.end(), [](const auto& left, const auto& right) {
        return path_utf8(left) < path_utf8(right);
    });
    for (auto& path : paths) {
        result.push_back({std::move(path), origin, priority});
    }
}

void append_named_lua(
    std::vector<DiscoveredScript>& result,
    const std::filesystem::path& root,
    const std::filesystem::path& directory,
    const std::string_view id,
    const ScriptOrigin origin,
    const std::int32_t priority
) {
    if (!safe_script_id(id)) {
        return;
    }
    if (const auto path = contained_lua_file(
            root,
            directory / (std::string(id) + ".lua")
        ); path.has_value()) {
        result.push_back({*path, origin, priority});
    }
}
#endif

[[nodiscard]] SDL_Color lane_color(const std::uint16_t lane) noexcept {
    constexpr std::array<SDL_Color, 9> colors{{
        {194, 91, 255, 255},
        {60, 220, 255, 255},
        {94, 255, 146, 255},
        {255, 90, 150, 255},
        {255, 205, 76, 255},
        {118, 134, 255, 255},
        {255, 137, 72, 255},
        {84, 255, 224, 255},
        {255, 255, 255, 255},
    }};
    return colors[lane % colors.size()];
}

[[nodiscard]] bool hurt_note_kind(const std::string_view kind) noexcept {
    return builtin_note_type_causes_miss(kind);
}

[[nodiscard]] bool note_is_visible(const NoteState state) noexcept {
    return state == NoteState::pending
        || state == NoteState::head_hit
        || state == NoteState::holding;
}

// PULSEFORGE_P1_4_0_THIRD_STRUM_RUNTIME_OWNER_V1
[[nodiscard]] bool third_strum_kind(const std::string_view kind) noexcept {
    return kind == "Third Strum";
}

[[nodiscard]] constexpr bool ai_note_owner(const NoteOwner owner) noexcept {
    return owner != NoteOwner::player;
}

[[nodiscard]] constexpr std::size_t note_owner_slot(
    const NoteOwner owner
) noexcept {
    switch (owner) {
    case NoteOwner::opponent: return 0U;
    case NoteOwner::player: return 1U;
    case NoteOwner::secondary_opponent: return 2U;
    }
    return 0U;
}

[[nodiscard]] NoteOwner runtime_owner(
    const PackedNoteOwner packed_owner,
    const std::string_view kind
) noexcept {
    if (third_strum_kind(kind)) {
        return NoteOwner::secondary_opponent;
    }
    return packed_owner == PackedNoteOwner::player
        ? NoteOwner::player
        : NoteOwner::opponent;
}

// PULSEFORGE_P1_5_0C2_MATERIALIZED_RUNTIME_OWNER_OVERLOAD_V1
// Materialized Chart::Note already stores the canonical NoteOwner enum. Keep
// that type intact instead of forcing it through PackedNoteOwner; only the
// Third Strum note-kind identity may upgrade a legacy opponent owner to the
// canonical secondary_opponent role.
[[nodiscard]] NoteOwner runtime_owner(
    const NoteOwner owner,
    const std::string_view kind
) noexcept {
    return third_strum_kind(kind)
        ? NoteOwner::secondary_opponent
        : owner;
}

// Dense/PVD construction cannot depend on the bounded gameplay window. Tap
// heads therefore keep raw screen coordinates inside the symmetric cache; the
// draw path clips translated cache rows to the live, unconsumed receptor side.
// Sustains are clipped here as well as at draw time. None of these visual LOD
// rules change judgment state or scoring.
[[nodiscard]] std::optional<NoteVisualSpan> dense_visual_note_span(
    const double note_time_ms,
    const double duration_ms,
    const double visual_time_ms,
    const double pixels_per_ms,
    const bool downscroll,
    const double receptor_y
) noexcept {
    if (duration_ms != 0.0) {
        return visual_note_span(
            note_time_ms,
            duration_ms,
            visual_time_ms,
            pixels_per_ms,
            downscroll,
            receptor_y
        );
    }
    auto span = note_screen_span(
        note_time_ms,
        0.0,
        visual_time_ms,
        pixels_per_ms,
        downscroll,
        receptor_y
    );
    if (!span.has_value()) {
        return std::nullopt;
    }
    return NoteVisualSpan{*span, true};
}

[[nodiscard]] double visible_time_window_ms(
    const double pixel_distance,
    const double pixels_per_ms,
    const double padding_ms = 0.0
) noexcept {
    if (!std::isfinite(pixel_distance) || pixel_distance < 0.0
        || !std::isfinite(pixels_per_ms) || pixels_per_ms <= 0.0
        || !std::isfinite(padding_ms) || padding_ms < 0.0) {
        return std::numeric_limits<double>::max();
    }
    const long double result = static_cast<long double>(pixel_distance)
            / static_cast<long double>(pixels_per_ms)
        + static_cast<long double>(padding_ms);
    return result >= static_cast<long double>(
               std::numeric_limits<double>::max())
        ? std::numeric_limits<double>::max()
        : static_cast<double>(result);
}

void set_draw_color(SDL_Renderer* renderer, const SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
}

void fill_rect(SDL_Renderer* renderer, const SDL_FRect& rectangle, const SDL_Color color) {
    set_draw_color(renderer, color);
    SDL_RenderFillRect(renderer, &rectangle);
}

class SolidQuadBatch final {
public:
    void begin(const std::size_t expected_quads) {
        vertices_.clear();
        indices_.clear();
        const auto safe_quads = std::min<std::size_t>(
            expected_quads,
            static_cast<std::size_t>(std::numeric_limits<int>::max() / 6)
        );
        vertices_.reserve(safe_quads * 4U);
        indices_.reserve(safe_quads * 6U);
    }

    void add(const SDL_FRect& rectangle, const SDL_Color color) {
        if (rectangle.w <= 0.0F || rectangle.h <= 0.0F
            || vertices_.size() > static_cast<std::size_t>(
                std::numeric_limits<int>::max() - 4)) {
            return;
        }
        const auto first = static_cast<int>(vertices_.size());
        const SDL_FColor vertex_color{
            static_cast<float>(color.r) / 255.0F,
            static_cast<float>(color.g) / 255.0F,
            static_cast<float>(color.b) / 255.0F,
            static_cast<float>(color.a) / 255.0F,
        };
        vertices_.insert(vertices_.end(), {
            {{rectangle.x, rectangle.y}, vertex_color, {0.0F, 0.0F}},
            {{rectangle.x + rectangle.w, rectangle.y}, vertex_color, {0.0F, 0.0F}},
            {{rectangle.x + rectangle.w, rectangle.y + rectangle.h}, vertex_color, {0.0F, 0.0F}},
            {{rectangle.x, rectangle.y + rectangle.h}, vertex_color, {0.0F, 0.0F}},
        });
        indices_.insert(indices_.end(), {
            first, first + 1, first + 2,
            first, first + 2, first + 3,
        });
    }

    [[nodiscard]] bool flush(SDL_Renderer* const renderer) const noexcept {
        if (vertices_.empty()) {
            return true;
        }
        return SDL_RenderGeometry(
            renderer,
            nullptr,
            vertices_.data(),
            static_cast<int>(vertices_.size()),
            indices_.data(),
            static_cast<int>(indices_.size())
        );
    }

    [[nodiscard]] std::size_t quad_count() const noexcept {
        return vertices_.size() / 4U;
    }

private:
    std::vector<SDL_Vertex> vertices_;
    std::vector<int> indices_;
};

void outline_rect(
    SDL_Renderer* renderer,
    const SDL_FRect& rectangle,
    const SDL_Color color
) {
    set_draw_color(renderer, color);
    SDL_RenderRect(renderer, &rectangle);
}

void debug_text(
    SDL_Renderer* renderer,
    const float x,
    const float y,
    const std::string_view text
) {
    const std::string terminated(text);
    SDL_RenderDebugText(renderer, x, y, terminated.c_str());
}

[[nodiscard]] double timestamp_to_song_time(
    const std::uint64_t timestamp_ns,
    const std::uint64_t now_ns,
    const double current_song_ms,
    const double playback_rate
) noexcept {
    if (timestamp_ns == 0 || timestamp_ns > now_ns + 1'000'000'000ULL) {
        return current_song_ms;
    }
    const auto delta_ns = static_cast<std::int64_t>(timestamp_ns)
        - static_cast<std::int64_t>(now_ns);
    const double safe_rate = std::isfinite(playback_rate)
        ? std::clamp(playback_rate, 0.25, 4.0)
        : 1.0;
    return current_song_ms
        + static_cast<double>(delta_ns) / 1'000'000.0 * safe_rate;
}

class DesktopApplication final : public detail::ApplicationRunner {
public:
    explicit DesktopApplication(AppLaunchOptions options)
        : options_(std::move(options)) {}

    DesktopApplication(
        AppLaunchOptions options,
        std::shared_ptr<detail::DiscordPresenceSession> discord_session
    ) : options_(std::move(options)),
        discord_presence_(std::move(discord_session)) {}

    DesktopApplication(
        AppLaunchOptions options,
        detail::TransferredPlatform platform,
        detail::TransferredPlatform* const return_platform,
        std::shared_ptr<detail::DiscordPresenceSession> discord_session = {}
    )
        : options_(std::move(options)),
          owned_platform_(std::move(platform)),
          return_platform_(return_platform),
          window_(owned_platform_.window),
          renderer_(owned_platform_.renderer),
          discord_presence_(std::move(discord_session)),
          sdl_initialized_(owned_platform_.owns_sdl),
          transferred_platform_(owned_platform_.complete()) {
        if (!transferred_platform_) {
            // A partial handoff cannot safely be mixed with a freshly-created
            // SDL stack. Dispose it now; initialize_platform() will build one
            // coherent replacement whose ownership is recorded below.
            owned_platform_.reset();
            window_ = nullptr;
            renderer_ = nullptr;
            sdl_initialized_ = false;
        }
    }

    ~DesktopApplication() {
        shutdown();
    }

    [[nodiscard]] int run() override {
        std::cerr
            << "[PulseForge] runtime patch: "
            << PULSEFORGE_PATCH_BUILD
            << '\n';
        {
            RuntimeTelemetrySnapshot loading_presence;
            loading_presence.activity = RuntimeActivityKind::loading;
            loading_presence.status = "Loading chart and runtime";
            discord_presence_.publish(
                loading_presence,
                options_.settings.discord
            );
        }
        // PULSEFORGE_P1_5_0E_LOW_LATENCY_RUNTIME_SCOPE_GUARD_V1
        // The process/thread priority and 1 ms Windows timer period must be
        // restored on every exit path, including chart-load/platform failures.
        // shutdown() retains its idempotent restore as a destructor fallback.
        configure_low_latency_runtime();
        [[maybe_unused]] const ScopeExit low_latency_runtime_scope{[this]() noexcept {
            restore_low_latency_runtime();
        }};

        const bool content_loaded = transferred_platform_
            ? load_content_with_progress()
            : load_content();
        if (!content_loaded) {
            if (loading_cancelled_ && options_.return_to_launcher) {
                return detail::runner_return_to_launcher;
            }
            if (!transferred_platform_
                && options_.settings.visual.theme == PresentationTheme::ps2
                && !options_.smoke_test) {
                ::pulseforge::detail::show_ps2_error_screen_standalone(
                    "CHART COULD NOT BE LOADED",
                    last_error_.empty()
                        ? std::string_view{"Chart validation failed; see diagnostics"}
                        : std::string_view{last_error_},
                    ps2_theme_movie_path(options_, "rsod_background.mp4"),
                    options_.settings.audio
                );
            }
            return transferred_platform_ && options_.return_to_launcher
                ? detail::runner_chart_load_failed
                : EXIT_FAILURE;
        }
        if (!initialize_platform()) {
            if (window_ != nullptr && renderer_ != nullptr
                && !last_error_.empty()
                && !options_.suppress_load_error_acknowledgement
                && (transferred_platform_
                    || options_.settings.visual.theme == PresentationTheme::ps2)) {
                wait_for_render_acknowledgement(
                    "CHART COULD NOT START",
                    last_error_
                );
            }
            return transferred_platform_ && options_.return_to_launcher
                ? detail::runner_chart_load_failed
                : EXIT_FAILURE;
        }

        if (!options_.offline_render.enabled
            && options_.return_to_launcher
            && !options_.smoke_test
            && !wait_for_ready_screen()) {
            return detail::runner_return_to_launcher;
        }

#if defined(PULSEFORGE_HAS_LUA)
        if (lua_create_pending_) {
            activate_lua_create_callbacks();
        }
        if (options_.offline_render.enabled && !script_song_started_) {
            // Offline rendering remains deterministic and does not expose an
            // interactive selector gate, but onSongStart must still fire.
            script_countdown_blocked_ = false;
            if (scripts_ != nullptr) {
                script_state_.clear_transient_output();
                static_cast<void>(scripts_->on_song_start());
                script_song_started_ = true;
                consume_script_output();
            } else {
                script_song_started_ = true;
            }
        }
#endif
        if (options_.offline_render.enabled) {
            const int render_result = run_offline_render();
            return options_.return_to_launcher
                ? detail::runner_return_to_launcher
                : render_result;
        }
#if defined(PULSEFORGE_HAS_LUA)
        start_script_song_if_ready();
        if (script_countdown_blocked_) {
            audio_.seek_ms(0.0);
            audio_.pause();
        }
#else
        audio_.play();
#endif
        running_ = true;
        last_frame_ns_ = SDL_GetTicksNS();
        while (running_) {
            discord_presence_.pump();
            const auto frame_start_ns = SDL_GetTicksNS();
            const double elapsed_seconds = std::clamp(
                static_cast<double>(frame_start_ns - last_frame_ns_) / 1'000'000'000.0,
                0.0,
                0.25
            );
            last_frame_ns_ = frame_start_ns;
            frame(elapsed_seconds, frame_start_ns);
            limit_frame_rate(frame_start_ns);
        }
        if (runtime_fatal_error_) {
            return EXIT_FAILURE;
        }
        save_replay_if_requested();
        if (!return_to_launcher_requested_) {
            return EXIT_SUCCESS;
        }
        if (options_.campaign_mode && result_shown_
            && campaign_result_acknowledged_
            && (session_ != nullptr || streaming_session_ != nullptr)) {
            return gameplay_failed()
                ? detail::runner_song_failed
                : detail::runner_song_completed;
        }
        return detail::runner_return_to_launcher;
    }

private:
    // PULSEFORGE_P1_5_0B_BOUNDED_NOTE_VISUAL_STATE_V1
    // Future/unspawn visual mutations collapse to (owner, noteType) state.
    // This keeps custom-note/event compatibility independent of physical chart
    // size and therefore safe for both materialized and PFC1 paths.
    struct ScriptNoteVisualState final {
        std::string texture;
        std::optional<detail::RuntimeNoteSkinProfile> texture_profile;
        std::string splash_texture;
        std::optional<detail::RuntimeNoteSplashProfile> splash_profile;
        std::optional<std::array<std::uint8_t, 3U>> rgb;
        std::optional<double> alpha;
        std::optional<double> scale;
        std::optional<bool> splash_disabled;
    };

    static constexpr std::size_t maximum_script_note_visual_overrides = 12'288U;

#if defined(PULSEFORGE_HAS_LUA)
    class ApplicationLuaHost final : public LuaHostInterface {
    public:
        explicit ApplicationLuaHost(DesktopApplication& application) noexcept
            : application_(application) {}

        [[nodiscard]] double song_position_ms() const noexcept override {
            return application_.gameplay_song_time_ms();
        }
        [[nodiscard]] std::int64_t current_beat() const noexcept override {
            return application_.script_floor_to_int64(
                application_.gameplay_timing().beat_at(
                    application_.gameplay_song_time_ms()
                )
            );
        }
        [[nodiscard]] std::int64_t current_step() const noexcept override {
            return application_.script_floor_to_int64(
                application_.gameplay_timing().step_at(
                    application_.gameplay_song_time_ms()
                )
            );
        }
        [[nodiscard]] std::int64_t current_section() const noexcept override {
            return application_.script_floor_to_int64(
                application_.gameplay_timing().step_at(
                    application_.gameplay_song_time_ms()
                ) / 16.0
            );
        }
        [[nodiscard]] double current_decimal_beat() const noexcept override {
            return application_.gameplay_timing().beat_at(
                application_.gameplay_song_time_ms()
            );
        }
        [[nodiscard]] double current_decimal_step() const noexcept override {
            return application_.gameplay_timing().step_at(
                application_.gameplay_song_time_ms()
            );
        }

        [[nodiscard]] bool get_property(
            std::string_view name,
            ScriptValue& value,
            std::string& error
        ) const override {
            return application_.script_host_get_property(name, value, error);
        }
        [[nodiscard]] bool set_property(
            std::string_view name,
            const ScriptValue& value,
            std::string& error
        ) override {
            return application_.script_host_set_property(name, value, error);
        }
        [[nodiscard]] bool add_score(
            std::int64_t amount,
            std::string& error
        ) override {
            return application_.script_host_add_score(amount, error);
        }
        [[nodiscard]] bool set_health(
            double health,
            std::string& error
        ) override {
            return application_.script_host_set_health(health, error);
        }
        [[nodiscard]] bool trigger_event(
            ScriptEventRequest event,
            std::string& error
        ) override {
            return application_.script_host_trigger_event(
                std::move(event), error
            );
        }
        void debug_print(std::string_view message) override {
            application_.script_host_debug_print(message);
        }
        [[nodiscard]] bool invoke_function(
            std::string_view name,
            std::span<const ScriptValue> arguments,
            ScriptValue& result,
            std::string& error
        ) override {
            return application_.script_invoke_compatibility(
                name, arguments, result, error
            );
        }

    private:
        DesktopApplication& application_;
    };

    struct ScriptStrumState final {
        std::optional<double> x;
        std::optional<double> y;
        // PULSEFORGE_P1_1_17_STRUM_SCALE_STATE_V1
        double scale_x{1.0};
        double scale_y{1.0};
        double angle{};
        double alpha{1.0};
        bool visible{true};
        // PULSEFORGE_P1_5_0B_STRUM_TEXTURE_PROFILE_STATE_V1
        std::string texture;
        std::optional<detail::RuntimeNoteSkinProfile> texture_profile;
    };

    struct ScriptUnspawnPrototype final {
        std::uint32_t kind_id{};
        NoteOwner owner{NoteOwner::opponent};
    };

    static constexpr std::size_t maximum_script_unspawn_prototypes = 8'192U;

    struct ScriptHudObjectState final {
        double x{};
        double y{};
        double scale_x{1.0};
        double scale_y{1.0};
        double angle{};
        double alpha{1.0};
        bool visible{true};

        // PULSEFORGE_P1_1_PSYCH_LUA_CORPUS_V1
        // Small, bounded Psych-style text objects. Existing HUD entries keep
        // their old aggregate initialization because all new fields default.
        bool text_object{};
        bool text_added{};
        bool camera_other{};
        double text_width{};
        double text_size{16.0};
        std::string text_alignment{"left"};
        // PULSEFORGE_P1_1_16_PSYCH_TEXT_STYLE_STATE_V1
        SDL_Color text_color{255U, 255U, 255U, 255U};
        SDL_Color border_color{0U, 0U, 0U, 255U};
        double border_size{};
        bool italic{};
        std::string font;
        std::string text;
    };

    enum class ScriptTweenTarget : std::uint8_t {
        object,
        strum,
        camera_game,
        camera_hud,
    };

    struct ScriptTween final {
        std::string tag;
        ScriptTweenTarget target{ScriptTweenTarget::object};
        std::string object;
        std::string property;
        std::size_t index{};
        double from{};
        double to{};
        double elapsed{};
        double duration{};
        std::string easing{"linear"};
    };

    struct ScriptTimer final {
        std::string tag;
        double interval_seconds{1.0};
        double remaining_seconds{1.0};
        // Psych/FlxTimer convention: loops == 0 means repeat forever.
        std::int64_t total_loops{1};
        std::int64_t loops_left{1};
    };

    // PULSEFORGE_P1_1_18_DYNAMIC_SCRIPT_REQUEST_TYPES_V1
    enum class DynamicScriptAction : std::uint8_t {
        add,
        remove,
    };

    struct DynamicScriptRequest final {
        DynamicScriptAction action{DynamicScriptAction::add};
        std::filesystem::path path;
        bool ignore_already_running{};
    };

    enum class ScriptAudioKind : std::uint8_t {
        sound,
        music,
    };
#endif

    struct ProfileMetric final {
        double last_us{};
        double average_us{};
        double peak_us{};
        std::uint64_t samples{};

        void sample(const std::uint64_t nanoseconds) noexcept {
            last_us = static_cast<double>(nanoseconds) / 1'000.0;
            average_us = samples == 0U
                ? last_us
                : average_us * 0.90 + last_us * 0.10;
            peak_us = std::max(peak_us, last_us);
            ++samples;
        }

        void reset_peak() noexcept {
            peak_us = 0.0;
        }
    };

    struct NoteProfileFrame final {
        std::uint64_t gameplay_update_ns{};
        std::uint64_t note_pipeline_ns{};
        std::uint64_t cache_rebuild_ns{};
        std::uint64_t pvd_visit_ns{};
        std::uint64_t pfc_visit_ns{};
        std::uint32_t cache_rebuilds{};
    };

    void reset_note_profile_peaks() noexcept {
        note_profile_note_.reset_peak();
        note_profile_cache_.reset_peak();
        note_profile_pvd_.reset_peak();
        note_profile_pfc_.reset_peak();
        note_profile_batch_build_.reset_peak();
        note_profile_batch_submit_.reset_peak();
        note_profile_fallback_.reset_peak();
        note_profile_present_.reset_peak();
        note_profile_gameplay_update_.reset_peak();
    }

    void sample_note_profile_frame() noexcept {
        if (!diagnostics_) {
            return;
        }
        const auto now = SDL_GetTicksNS();
        if (note_profile_peak_window_started_ns_ == 0U) {
            note_profile_peak_window_started_ns_ = now;
        } else if (now - note_profile_peak_window_started_ns_
                   >= 1'000'000'000ULL) {
            note_profile_rebuilds_last_second_ =
                note_profile_rebuilds_current_window_;
            note_profile_rebuilds_current_window_ = 0U;
            reset_note_profile_peaks();
            note_profile_peak_window_started_ns_ = now;
        }

        // Cache rebuild work is nested inside the streaming note pipeline.
        // Subtract it here so the displayed "note" metric isolates ordinary
        // traversal/culling/LOD/queue CPU instead of double-counting cache time.
        const auto note_cpu_ns =
            note_profile_frame_.note_pipeline_ns
                > note_profile_frame_.cache_rebuild_ns
            ? note_profile_frame_.note_pipeline_ns
                - note_profile_frame_.cache_rebuild_ns
            : std::uint64_t{0U};
        note_profile_gameplay_update_.sample(
            note_profile_frame_.gameplay_update_ns
        );
        note_profile_note_.sample(note_cpu_ns);
        note_profile_cache_.sample(note_profile_frame_.cache_rebuild_ns);
        note_profile_pvd_.sample(note_profile_frame_.pvd_visit_ns);
        note_profile_pfc_.sample(note_profile_frame_.pfc_visit_ns);
        note_profile_rebuilds_current_window_ +=
            note_profile_frame_.cache_rebuilds;

        detail::RuntimeNoteSkinProfileStats runtime_stats{};
        if (scene_ != nullptr) {
            runtime_stats = scene_->note_skin_profile_stats();
        }
        const auto accounted_ns = runtime_stats.geometry_submit_ns
            + runtime_stats.fallback_ns;
        const auto build_ns = runtime_stats.batch_total_ns > accounted_ns
            ? runtime_stats.batch_total_ns - accounted_ns
            : std::uint64_t{0U};
        note_profile_batch_build_.sample(build_ns);
        note_profile_batch_submit_.sample(runtime_stats.geometry_submit_ns);
        note_profile_fallback_.sample(runtime_stats.fallback_ns);
        note_profile_last_quads_ = runtime_stats.quads;
        note_profile_last_submissions_ = runtime_stats.submissions;
        note_profile_last_failed_submissions_ =
            runtime_stats.failed_submissions;
        note_profile_last_fallback_draws_ = runtime_stats.fallback_draws;
    }

    void set_loading_phase(
        const ContentLoadPhase phase,
        const std::uint64_t work_total = 0U
    ) noexcept {
        loading_work_complete_.store(0U, std::memory_order_relaxed);
        loading_work_total_.store(work_total, std::memory_order_relaxed);
        loading_phase_.store(phase, std::memory_order_release);
    }

    void set_loading_work_complete(const std::uint64_t value) noexcept {
        loading_work_complete_.store(value, std::memory_order_relaxed);
    }

    [[nodiscard]] bool load_content_with_progress() {
        set_loading_phase(ContentLoadPhase::inspecting);
        std::atomic<bool> complete{false};
        bool loaded = false;
        std::thread worker([&]() noexcept {
            try {
                loaded = load_content();
            } catch (const std::exception& exception) {
                last_error_ = std::string{"Unexpected chart load failure: "}
                    + exception.what();
                loaded = false;
            } catch (...) {
                last_error_ = "Unexpected chart load failure";
                loaded = false;
            }
            if (loaded) {
                set_loading_phase(ContentLoadPhase::complete);
            }
            complete.store(true, std::memory_order_release);
        });

        const auto began = SDL_GetTicksNS();
        std::uint64_t last_draw{};
        while (!complete.load(std::memory_order_acquire)) {
            discord_presence_.pump();
            SDL_Event event;
            while (detail::poll_mobile_event(&event)) {
                if (event.type == SDL_EVENT_QUIT
                    || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED
                    || (event.type == SDL_EVENT_KEY_DOWN
                        && !event.key.repeat
                        && event.key.scancode == SDL_SCANCODE_ESCAPE)) {
                    loading_cancelled_ = true;
                }
            }
            const auto now = SDL_GetTicksNS();
            if (last_draw == 0U || now - last_draw >= 100'000'000ULL) {
                const double elapsed = static_cast<double>(now - began)
                    / 1'000'000'000.0;
                const auto phase = loading_phase_.load(
                    std::memory_order_acquire
                );
                const auto phase_display = load_phase_display(phase);
                const auto work_total = loading_work_total_.load(
                    std::memory_order_relaxed
                );
                const auto work_complete = loading_work_complete_.load(
                    std::memory_order_relaxed
                );
                const std::optional<double> measured_progress =
                    phase == ContentLoadPhase::indexing_notes
                        && work_total != 0U
                    ? std::optional<double>{std::clamp(
                        static_cast<double>(work_complete)
                            / static_cast<double>(work_total),
                        0.0,
                        1.0
                    )}
                    : std::nullopt;
                char detail[320]{};
                std::snprintf(
                    detail,
                    sizeof(detail),
                    "%s  //  PHASE: %.*s  //  elapsed %.1f s",
                    path_utf8(options_.chart_path.filename()).c_str(),
                    static_cast<int>(phase_display.label.size()),
                    phase_display.label.data(),
                    elapsed
                );
                draw_runtime_message(
                    loading_cancelled_
                        ? "CANCEL REQUESTED - FINISHING SAFE VALIDATION"
                        : "LOADING CHART WITHOUT CLOSING THIS WINDOW",
                    detail,
                    measured_progress.has_value()
                        ? "Bar = notes indexed / total notes; no ETA is estimated"
                        : "Indeterminate phase; elapsed time is not an ETA",
                    measured_progress
                );
                last_draw = now;
            }
            SDL_Delay(8U);
        }
        worker.join();
        if (loaded && !loading_cancelled_) {
            draw_runtime_message(
                "CHART PROCESSING COMPLETE",
                path_utf8(options_.chart_path.filename()),
                "All measured loading work is complete",
                1.0
            );
        }
        if (!loaded) {
            if (!options_.suppress_load_error_acknowledgement) {
                wait_for_render_acknowledgement(
                    "CHART COULD NOT BE LOADED",
                    last_error_.empty()
                        ? std::string_view{
                            "Chart validation failed; see diagnostics"
                        }
                        : std::string_view{last_error_}
                );
            }
            return false;
        }
        return !loading_cancelled_;
    }

    void draw_runtime_message(
        const std::string_view title,
        const std::string_view detail,
        const std::string_view hint,
        const std::optional<double> progress = std::nullopt
    ) const {
        set_draw_color(renderer_, {5, 8, 18, 255});
        SDL_RenderClear(renderer_);
        fill_rect(
            renderer_,
            {135.0F, 165.0F, 1'010.0F, 390.0F},
            {12, 18, 38, 245}
        );
        outline_rect(
            renderer_,
            {135.0F, 165.0F, 1'010.0F, 390.0F},
            {64, 225, 235, 255}
        );
        float scale_x = 1.0F;
        float scale_y = 1.0F;
        SDL_GetRenderScale(renderer_, &scale_x, &scale_y);
        SDL_SetRenderScale(renderer_, 2.0F, 2.0F);
        debug_text(renderer_, 102.0F, 110.0F, title);
        SDL_SetRenderScale(renderer_, 1.35F, 1.35F);
        debug_text(renderer_, 151.0F, 205.0F, detail);
        debug_text(renderer_, 151.0F, 355.0F, hint);
        SDL_SetRenderScale(renderer_, scale_x, scale_y);
        if (progress.has_value()) {
            const float fraction = static_cast<float>(std::clamp(
                *progress,
                0.0,
                1.0
            ));
            fill_rect(
                renderer_,
                {205.0F, 445.0F, 870.0F, 18.0F},
                {20, 30, 52, 255}
            );
            fill_rect(
                renderer_,
                {205.0F, 445.0F, 870.0F * fraction, 18.0F},
                {64, 225, 235, 255}
            );
        }
        detail::present_with_mobile_touch(renderer_);
    }

    [[nodiscard]] std::size_t bound_lane_count() const {
        std::vector<bool> bound(chart_->key_count, false);
        constexpr auto invalid_lane = std::numeric_limits<std::uint16_t>::max();
        for (const auto lane : keyboard_lane_map_) {
            if (lane != invalid_lane && lane < bound.size()) {
                bound[lane] = true;
            }
        }
        for (const auto lane : gamepad_lane_map_) {
            if (lane != invalid_lane && lane < bound.size()) {
                bound[lane] = true;
            }
        }
        return static_cast<std::size_t>(std::count(bound.begin(), bound.end(), true));
    }

    void draw_chart_ready_screen() const {
        set_draw_color(renderer_, {5, 8, 18, 255});
        SDL_RenderClear(renderer_);
        fill_rect(
            renderer_,
            {115.0F, 62.0F, 1'050.0F, 596.0F},
            {12, 18, 38, 247}
        );
        outline_rect(
            renderer_,
            {115.0F, 62.0F, 1'050.0F, 596.0F},
            {64, 225, 235, 255}
        );

        float scale_x = 1.0F;
        float scale_y = 1.0F;
        SDL_GetRenderScale(renderer_, &scale_x, &scale_y);
        SDL_SetRenderScale(renderer_, 2.0F, 2.0F);
        debug_text(renderer_, 220.0F, 48.0F, "YOUR CHART IS READY");
        SDL_SetRenderScale(renderer_, 1.0F, 1.0F);

        const auto key_count = chart_->key_count;
        const auto bound_count = bound_lane_count();
        const std::string_view key_mode = key_count == 4U
            ? "STANDARD 4K"
            : (key_count < 4U ? "LOW-KEY MANIA" : "MULTIKEY MANIA");
        char summary[512]{};
        std::snprintf(
            summary,
            sizeof(summary),
            "%s  //  %llu NOTES  //  %u LANES (%.*s)  //  AUDIO %s",
            chart_->title.c_str(),
            static_cast<unsigned long long>(chart_source_note_count()),
            static_cast<unsigned int>(key_count),
            static_cast<int>(key_mode.size()),
            key_mode.data(),
            audio_.using_silent_audio() ? "missing" : "ready"
        );
        debug_text(renderer_, 150.0F, 142.0F, summary);

        char input_status[256]{};
        std::snprintf(
            input_status,
            sizeof(input_status),
            "INPUT: %zu/%u LANES BOUND%s",
            bound_count,
            static_cast<unsigned int>(key_count),
            bound_count < key_count
                ? "  -  CONFIGURE MISSING LANES IN CONTROLS"
                : ""
        );
        debug_text(
            renderer_,
            150.0F,
            171.0F,
            input_status
        );

        debug_text(renderer_, 150.0F, 215.0F, "USEFUL GAMEPLAY KEYS");
        constexpr std::array<std::string_view, 9> useful_keys{
            "- F1   OPEN PAUSE FOCUSED ON RETURN TO MENU",
            "- F2   ENABLE / DISABLE BOTPLAY",
            "- F3   SHOW / HIDE DEBUG AND PERFORMANCE DATA",
            "- F5   RELOAD LUA SCRIPTS (WHEN ENABLED)",
            "- F11  TOGGLE FULLSCREEN",
            "- ESC  PAUSE / RESUME",
            "- R    RESTART THE CHART",
            "- +/-  VOLUME DOWN / UP (REMAPPABLE)",
            "- M    MUTE / UNMUTE (REMAPPABLE)",
        };
        for (std::size_t index = 0U; index < useful_keys.size(); ++index) {
            debug_text(
                renderer_,
                170.0F,
                248.0F + static_cast<float>(index) * 32.0F,
                useful_keys[index]
            );
        }
        debug_text(
            renderer_,
            246.0F,
            590.0F,
            "ENTER / SPACE / GAMEPAD A STARTS    ESC RETURNS TO MENU"
        );
        SDL_SetRenderScale(renderer_, scale_x, scale_y);
        detail::present_with_mobile_touch(renderer_);
    }

    [[nodiscard]] bool wait_for_ready_screen() {
        draw_chart_ready_screen();
        while (true) {
            discord_presence_.pump();
            SDL_Event event;
            while (detail::poll_mobile_event(&event)) {
                if (event.type == SDL_EVENT_QUIT
                    || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                    return false;
                }
                if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                    if (handle_global_audio_action(event.key)) {
                        continue;
                    }
                    if (event.key.scancode == SDL_SCANCODE_RETURN
                        || event.key.scancode == SDL_SCANCODE_KP_ENTER
                        || event.key.scancode == SDL_SCANCODE_SPACE) {
                        return true;
                    }
                    if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                        return false;
                    }
                }
                if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN
                    && event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) {
                    return true;
                }
            }
            SDL_Delay(8U);
        }
    }

    void wait_for_render_acknowledgement(
        const std::string_view title,
        const std::string_view detail
    ) {
        if (options_.smoke_test
            || (!options_.return_to_launcher
                && options_.settings.visual.theme != PresentationTheme::ps2)) {
            return;
        }
        if (options_.settings.visual.theme == PresentationTheme::ps2
            && window_ != nullptr && renderer_ != nullptr) {
            ::pulseforge::detail::show_ps2_error_screen(
                window_,
                renderer_,
                title,
                detail,
                options_.return_to_launcher
                    ? "PRESS ENTER / SPACE / ESC TO RETURN TO PULSEFORGE"
                    : "PRESS ENTER / SPACE / ESC TO CLOSE THIS ERROR",
                ps2_theme_movie_path(options_, "rsod_background.mp4"),
                options_.settings.audio
            );
            return;
        }
        draw_runtime_message(
            title,
            detail,
            "ENTER / SPACE / gamepad A returns to PulseForge"
        );
        bool waiting = true;
        while (waiting) {
            discord_presence_.pump();
            SDL_Event event;
            while (detail::poll_mobile_event(&event)) {
                const bool acknowledged = event.type == SDL_EVENT_QUIT
                    || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED
                    || (event.type == SDL_EVENT_KEY_DOWN
                        && !event.key.repeat
                        && (event.key.scancode == SDL_SCANCODE_RETURN
                            || event.key.scancode == SDL_SCANCODE_KP_ENTER
                            || event.key.scancode == SDL_SCANCODE_SPACE
                            || event.key.scancode == SDL_SCANCODE_ESCAPE))
                    || (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN
                        && event.gbutton.button
                            == SDL_GAMEPAD_BUTTON_SOUTH);
                if (acknowledged) {
                    waiting = false;
                    break;
                }
            }
            if (waiting) {
                SDL_Delay(8U);
            }
        }
    }

    [[nodiscard]] bool offline_cancel_requested() {
        if (!options_.return_to_launcher || options_.smoke_test) {
            return false;
        }
        SDL_Event event;
        while (detail::poll_mobile_event(&event)) {
            if (event.type == SDL_EVENT_QUIT
                || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED
                || (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat
                    && event.key.scancode == SDL_SCANCODE_ESCAPE)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] int run_offline_render() {
        OfflineRenderPlanRequest request;
        request.config = options_.offline_render;
        request.chart_title = chart_->title;
        request.difficulty = chart_->difficulty;
        request.audio = chart_->audio;
        request.source_chart_path = options_.chart_path;
        request.duration_ms = audio_.using_silent_audio()
            ? std::max(1.0, gameplay_content_duration_ms())
            : audio_.duration_ms();
        request.forbidden_executable_roots = options_.content_roots;
        if (!options_.selected_content_root.empty()) {
            request.forbidden_executable_roots.push_back(
                options_.selected_content_root
            );
        }
        if (!options_.selected_mod_root.empty()) {
            request.forbidden_executable_roots.push_back(
                options_.selected_mod_root
            );
        }
        request.forbidden_executable_roots.push_back(
            std::filesystem::current_path() / "mods"
        );
        request.temporary_token = std::to_string(SDL_GetTicksNS());

        auto planned = build_offline_render_plan(request);
        if (!planned) {
            std::cerr << "Render setup failed: " << planned.error << '\n';
            wait_for_render_acknowledgement(
                "RENDER SETUP FAILED",
                planned.error
            );
            return EXIT_FAILURE;
        }
        offline_encoder_ = std::make_unique<detail::OfflineEncoder>();
        if (!offline_encoder_->start(std::move(*planned.plan), &last_error_)) {
            std::cerr << "Render setup failed: " << last_error_ << '\n';
            offline_encoder_.reset();
            wait_for_render_acknowledgement(
                "RENDER SETUP FAILED",
                last_error_
            );
            return EXIT_FAILURE;
        }
        const auto* plan = offline_encoder_->plan();
        if (plan == nullptr) {
            std::cerr << "Render setup failed: encoder lost its command plan\n";
            offline_encoder_.reset();
            wait_for_render_acknowledgement(
                "RENDER SETUP FAILED",
                "The encoder lost its validated command plan"
            );
            return EXIT_FAILURE;
        }
        const auto output_path = plan->final_output_path;
        const auto frame_count = plan->frame_count;
        const auto fps = plan->fps;
        std::cout << "Rendering " << frame_count << " deterministic frames at "
                  << fps << " FPS to " << path_utf8(output_path) << "\n";

        // With no replay, rendering uses the regular BOTPLAY path. This keeps
        // hits, animations, events and Lua callbacks identical to gameplay
        // while making input independent from wall-clock timing.
        if (!replay_.has_value()) {
            gameplay_settings().autoplay = true;
        }
        const double elapsed_seconds = 1.0 / static_cast<double>(fps);
        const auto progress_interval = std::max<std::uint64_t>(
            static_cast<std::uint64_t>(fps) * 5U,
            1U
        );
        const auto visual_progress_interval = std::max<std::uint64_t>(
            static_cast<std::uint64_t>(fps) / 2U,
            1U
        );
        if (options_.return_to_launcher && !options_.smoke_test) {
            draw_runtime_message(
                "OFFLINE RENDERING MODE",
                path_utf8(output_path.filename()),
                "Deterministic capture in progress   ESC cancels safely",
                0.0
            );
        }
        for (std::uint64_t frame_index = 0U;
             frame_index < frame_count;
             ++frame_index) {
            if (offline_cancel_requested()) {
                offline_encoder_->cancel();
                offline_encoder_.reset();
                if (release_offline_render_target()) {
                    wait_for_render_acknowledgement(
                        "RENDER CANCELLED",
                        "No partial MP4 or private log was kept"
                    );
                }
                return EXIT_SUCCESS;
            }
            const double song_time = static_cast<double>(frame_index)
                * 1'000.0 / static_cast<double>(fps);
            gameplay_begin_frame();
#if defined(PULSEFORGE_HAS_LUA)
            script_state_.clear_transient_output();
#endif
            if (!gameplay_failed()) {
                dispatch_replay_inputs(song_time);
                gameplay_update(song_time);
            }
            consume_gameplay_events();
#if defined(PULSEFORGE_HAS_LUA)
            if (scripts_ != nullptr) {
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
            }
#endif
            update_effects(static_cast<float>(elapsed_seconds));
            offline_frame_error_.clear();
            render(song_time);
            if (!offline_frame_error_.empty()) {
                std::cerr << "Render failed: " << offline_frame_error_ << '\n';
                offline_encoder_.reset();
                if (release_offline_render_target()) {
                    wait_for_render_acknowledgement(
                        "RENDER FAILED",
                        offline_frame_error_
                    );
                }
                return EXIT_FAILURE;
            }
            update_fps(elapsed_seconds);
            runtime_performance_.record_frame_ms(elapsed_seconds * 1'000.0);
            publish_render_presence(
                song_time,
                frame_index + 1U,
                frame_count,
                fps
            );
            if (options_.return_to_launcher && !options_.smoke_test
                && ((frame_index + 1U) % visual_progress_interval == 0U
                    || frame_index + 1U == frame_count)) {
                char progress[256]{};
                std::snprintf(
                    progress,
                    sizeof(progress),
                    "%llu / %llu frames  //  %.1f%%  //  %s",
                    static_cast<unsigned long long>(frame_index + 1U),
                    static_cast<unsigned long long>(frame_count),
                    offline_encoder_->progress_fraction() * 100.0,
                    path_utf8(output_path.filename()).c_str()
                );
                draw_runtime_message(
                    "OFFLINE RENDERING MODE",
                    progress,
                    "FFmpeg runs hidden   ESC cancels and removes partial files",
                    offline_encoder_->progress_fraction()
                );
            }
            if ((frame_index + 1U) % progress_interval == 0U
                || frame_index + 1U == frame_count) {
                std::cout << "Rendered " << frame_index + 1U << '/'
                          << frame_count << " frames\n";
            }
        }
        gameplay_begin_frame();
        gameplay_finish_song(request.duration_ms);
        update_nps(request.duration_ms);
        consume_gameplay_events();
        if (!offline_encoder_->finish(&last_error_)) {
            std::cerr << "Render failed: " << last_error_ << '\n';
            offline_encoder_.reset();
            if (release_offline_render_target()) {
                wait_for_render_acknowledgement("RENDER FAILED", last_error_);
            }
            return EXIT_FAILURE;
        }
        offline_encoder_.reset();
        publish_render_presence(
            request.duration_ms,
            frame_count,
            frame_count,
            fps
        );
        report_runtime_benchmark_once();
        std::cout << "Render complete: " << path_utf8(output_path) << '\n';
        if (release_offline_render_target()) {
            wait_for_render_acknowledgement(
                "YOUR RENDER IS READY",
                path_utf8(output_path)
            );
        }
        return EXIT_SUCCESS;
    }

    [[nodiscard]] bool streaming_mode() const noexcept {
        return streaming_session_ != nullptr;
    }

    // PULSEFORGE_P1_3_0_DYNAMIC_LANE_TOPOLOGY_APP_V1
    // The chart/reader key count remains the stable maximum source-lane
    // domain. P1/P2 mania events change only the active runtime topology.
    [[nodiscard]] std::uint16_t active_key_count(
        const NoteOwner owner
    ) const noexcept {
        // PULSEFORGE_P1_4_0_THIRD_STRUM_TOPOLOGY_V1
        // Denpa player4 shares the AI-side mania domain. P2/Change Mania
        // therefore remap both opponent strumlines without expanding the
        // stable source-lane domain.
        if (streaming_session_ != nullptr) {
            return owner == NoteOwner::player
                ? streaming_session_->player_key_count()
                : streaming_session_->opponent_key_count();
        }
        if (session_ != nullptr) {
            return owner == NoteOwner::player
                ? session_->player_key_count()
                : session_->opponent_key_count();
        }
        return chart_.has_value() ? chart_->key_count : std::uint16_t{4U};
    }

    [[nodiscard]] bool secondary_strum_enabled() const noexcept {
        return chart_.has_value() && chart_->secondary_opponent_enabled;
    }

    [[nodiscard]] double receptor_y_for_owner(
        const NoteOwner owner
    ) const noexcept {
        const double standard = gameplay_settings().downscroll ? 610.0 : 110.0;
        if (owner != NoteOwner::secondary_opponent) {
            return standard;
        }
        // Denpa's third strums are offset vertically from the normal HUD
        // strums. Keep the same 200 px separation while remaining inside the
        // 720p logical viewport in both scroll directions.
        return gameplay_settings().downscroll ? 410.0 : 310.0;
    }

    [[nodiscard]] const ScoreSummary& gameplay_summary() const noexcept {
        return streaming_mode()
            ? streaming_session_->summary()
            : session_->summary();
    }

    [[nodiscard]] GameplaySettings& gameplay_settings() noexcept {
        return streaming_mode()
            ? streaming_session_->settings()
            : session_->settings();
    }

    [[nodiscard]] const GameplaySettings& gameplay_settings() const noexcept {
        return streaming_mode()
            ? streaming_session_->settings()
            : session_->settings();
    }

    [[nodiscard]] const TimingMap& gameplay_timing() const noexcept {
        return streaming_mode()
            ? streaming_session_->timing_map()
            : session_->timing_map();
    }

    [[nodiscard]] double gameplay_song_time_ms() const noexcept {
        return streaming_mode()
            ? streaming_session_->song_time_ms()
            : session_->song_time_ms();
    }

    [[nodiscard]] bool gameplay_complete() const noexcept {
        return streaming_mode()
            ? streaming_session_->complete()
            : session_->complete();
    }

    [[nodiscard]] bool gameplay_failed() const noexcept {
        return gameplay_summary().failed
            || (streaming_mode() && !streaming_session_->healthy());
    }

    [[nodiscard]] bool gameplay_lane_held(
        const std::uint16_t lane
    ) const noexcept {
        return streaming_mode()
            ? streaming_session_->lane_held(lane)
            : session_->lane_held(lane);
    }

    void gameplay_begin_frame() noexcept {
        if (streaming_mode()) {
            streaming_session_->begin_frame();
        } else {
            session_->begin_frame();
        }
    }

    void report_streaming_runtime_error() {
        if (!streaming_mode() || streaming_session_->healthy()
            || streaming_error_reported_) {
            return;
        }
        streaming_error_reported_ = true;
        std::cerr << "Streaming gameplay error: "
                  << streaming_session_->error() << '\n';
    }

    void gameplay_update(const double song_time_ms) {
        if (streaming_mode()) {
            const auto gameplay_started_ns = diagnostics_
                ? SDL_GetTicksNS()
                : std::uint64_t{0U};
            static_cast<void>(streaming_session_->update(song_time_ms));
            if (diagnostics_) {
                note_profile_frame_.gameplay_update_ns =
                    SDL_GetTicksNS() - gameplay_started_ns;
            }
            report_streaming_runtime_error();
        } else {
            session_->update(song_time_ms);
        }
    }

    void gameplay_finish_song(const double media_end_time_ms) {
        if (streaming_mode()) {
            static_cast<void>(
                streaming_session_->finish_song(media_end_time_ms)
            );
            report_streaming_runtime_error();
        } else {
            session_->finish_song(media_end_time_ms);
        }
    }

    void gameplay_press(
        const std::uint16_t lane,
        const double song_time_ms
    ) {
        if (streaming_mode()) {
            if (!streaming_session_->catchup_pending()) {
                static_cast<void>(streaming_session_->press(lane, song_time_ms));
            }
            report_streaming_runtime_error();
        } else {
            session_->press(lane, song_time_ms);
        }
    }

    void gameplay_release(
        const std::uint16_t lane,
        const double song_time_ms
    ) {
        if (streaming_mode()) {
            static_cast<void>(streaming_session_->release(lane, song_time_ms));
            report_streaming_runtime_error();
        } else {
            session_->release(lane, song_time_ms);
        }
    }

    [[nodiscard]] bool gameplay_reset() {
        if (!streaming_mode()) {
            session_->reset();
            return true;
        }
        std::string error;
        if (!streaming_session_->reset(&error)) {
            std::cerr << "Streaming gameplay reset failed: " << error << '\n';
            return false;
        }
        streaming_error_reported_ = false;
        return true;
    }

    [[nodiscard]] std::uint64_t chart_source_note_count() const noexcept {
        return streaming_mode()
            ? streaming_reader_->logical_note_count()
            : static_cast<std::uint64_t>(chart_->notes.size());
    }

    void initialize_presence_note_total() {
        // PULSEFORGE_P1_5_0F_AUTHORITATIVE_PRESENCE_NOTE_TOTAL_V1
        // A raw source count is a valid denominator only if no chart event can
        // change logical occurrence multiplicity. We never guess a denominator
        // when Note Multiplier is present; Chart Total itself remains exact.
        presence_logical_note_total_ = chart_source_note_count();
        for (const auto& event : chart_->events) {
            std::string lowered = event.name;
            std::transform(
                lowered.begin(),
                lowered.end(),
                lowered.begin(),
                [](const unsigned char value) {
                    return static_cast<char>(std::tolower(value));
                }
            );
            if (lowered.find("note multiplier") != std::string::npos) {
                presence_logical_note_total_ = 0U;
                break;
            }
        }
    }

    [[nodiscard]] static std::uint64_t saturating_telemetry_add(
        const std::uint64_t left,
        const std::uint64_t right
    ) noexcept {
        return right > std::numeric_limits<std::uint64_t>::max() - left
            ? std::numeric_limits<std::uint64_t>::max()
            : left + right;
    }

    [[nodiscard]] RuntimeTelemetrySnapshot runtime_telemetry_snapshot(
        const double song_time
    ) const {
        RuntimeTelemetrySnapshot snapshot;
        snapshot.activity = result_shown_
            ? RuntimeActivityKind::results
            : paused_ ? RuntimeActivityKind::paused
                      : RuntimeActivityKind::gameplay;
        snapshot.chart_title = chart_->title;
        snapshot.difficulty = chart_->difficulty;
        if (!options_.selected_mod_root.empty()) {
            snapshot.mod_name = path_utf8(
                options_.selected_mod_root.filename()
            );
        }
        snapshot.key_count = active_key_count(NoteOwner::player);
        snapshot.song_position_ms = std::max(song_time, 0.0);
        snapshot.duration_ms = std::max(gameplay_content_duration_ms(), 0.0);
        snapshot.playback_rate = audio_.playback_rate();
        const auto& summary = gameplay_summary();
        snapshot.chart_total = summary.chart_total;
        snapshot.logical_note_total = presence_logical_note_total_;
        auto hits = saturating_telemetry_add(summary.marvelous, summary.sick);
        hits = saturating_telemetry_add(hits, summary.good);
        hits = saturating_telemetry_add(hits, summary.bad);
        snapshot.successful_hits = hits;
        snapshot.misses = summary.misses;
        snapshot.combo = summary.combo;
        snapshot.max_combo = summary.max_combo;
        snapshot.score = summary.score;
        snapshot.accuracy_percent = summary.accuracy_percent();
        snapshot.health = summary.health;
        snapshot.player_note_multiplier = streaming_mode()
            ? streaming_session_->player_note_multiplier()
            : session_->player_note_multiplier();
        snapshot.opponent_note_multiplier = streaming_mode()
            ? streaming_session_->opponent_note_multiplier()
            : session_->opponent_note_multiplier();
        snapshot.botplay = gameplay_settings().autoplay;
        snapshot.practice = gameplay_settings().practice;
        snapshot.paused = paused_;
        snapshot.failed = gameplay_failed();
        snapshot.completed = result_shown_ && !snapshot.failed;
        snapshot.streaming = streaming_mode();
        snapshot.third_strum = secondary_strum_enabled();
        snapshot.status = streaming_mode() ? "PFC1 streaming" : "Materialized";
        return snapshot;
    }

    void publish_runtime_presence(const double song_time) noexcept {
        try {
            discord_presence_.publish(
                runtime_telemetry_snapshot(song_time),
                options_.settings.discord
            );
        } catch (...) {
            // Presence is never allowed to escape into the gameplay loop.
        }
    }

    void publish_render_presence(
        const double song_time,
        const std::uint64_t frame,
        const std::uint64_t frame_count,
        const std::uint32_t fps
    ) noexcept {
        try {
            auto snapshot = runtime_telemetry_snapshot(song_time);
            snapshot.activity = RuntimeActivityKind::rendering;
            snapshot.render_frame = frame;
            snapshot.render_frame_count = frame_count;
            snapshot.render_width = options_.offline_render.width;
            snapshot.render_height = options_.offline_render.height;
            snapshot.render_fps = fps;
            snapshot.task_progress = frame_count == 0U
                ? 0.0
                : std::clamp(
                    static_cast<double>(frame)
                        / static_cast<double>(frame_count),
                    0.0,
                    1.0
                );
            snapshot.paused = false;
            snapshot.completed = frame_count != 0U && frame >= frame_count;
            discord_presence_.publish(snapshot, options_.settings.discord);
        } catch (...) {
        }
    }

    void report_runtime_benchmark_once() {
        if (runtime_benchmark_reported_) return;
        runtime_benchmark_reported_ = true;
#if defined(_MSC_VER)
        char* enabled = nullptr;
        std::size_t enabled_size = 0U;
        if (_dupenv_s(
                &enabled,
                &enabled_size,
                "PULSEFORGE_RUNTIME_TELEMETRY_LOG"
            ) != 0) {
            return;
        }
        const bool telemetry_logging_enabled = enabled != nullptr
            && enabled_size > 1U
            && enabled[0] != '0';
        std::free(enabled);
        if (!telemetry_logging_enabled) return;
#else
        const char* const enabled = std::getenv("PULSEFORGE_RUNTIME_TELEMETRY_LOG");
        if (enabled == nullptr || *enabled == '\0' || *enabled == '0') return;
#endif
        const auto report = runtime_performance_.report();
        std::cout
            << "Runtime benchmark: frame avg " << report.average_frame_ms
            << " ms | p50 " << report.frame_p50_ms
            << " | p95 " << report.frame_p95_ms
            << " | p99 " << report.frame_p99_ms
            << " | max " << report.max_frame_ms
            << " | input age avg " << report.average_input_age_ms
            << " ms | p95 " << report.input_age_p95_ms
            << " | samples " << report.frame_samples
            << '/' << report.input_samples << '\n';
    }


    [[nodiscard]] double gameplay_content_duration_ms() const noexcept {
        return streaming_mode()
            ? streaming_duration_ms_
            : chart_->duration_ms();
    }

    [[nodiscard]] bool load_streaming_content(
        const std::string_view materialized_error
    ) {
        if (options_.replay_path.has_value()) {
            std::cerr
                << "Replay error: large-chart streaming does not yet support "
                   "replay playback\n";
            return false;
        }
        StreamingChartCacheOptions cache_options;
        cache_options.cache_root = options_.large_chart_cache_root;
        cache_options.difficulty = options_.chart_options.difficulty;
        cache_options.difficulty_explicit =
            options_.chart_options.difficulty_explicit;
        set_loading_phase(ContentLoadPhase::preparing_stream_cache);
        std::cout << "Preparing bounded PFC1 runtime cache for "
                  << path_utf8(options_.chart_path) << "...\n";
        auto cached = prepare_streaming_chart_cache(
            options_.chart_path,
            cache_options
        );
        if (!cached) {
            last_error_ = "Chart error: " + std::string(materialized_error)
                + " | Streaming fallback error: " + cached.error;
            std::cerr << "Chart error: " << materialized_error << '\n'
                      << "Streaming fallback error: " << cached.error << '\n';
            return false;
        }
        chart_ = std::move(cached.chart_metadata);
        if (chart_->title.empty() || chart_->title == "Untitled") {
            chart_->title = path_utf8(options_.chart_path.stem());
        }
        if (options_.instrumental_override.has_value()) {
            chart_->audio.instrumental = *options_.instrumental_override;
        }
        if (!options_.vocal_overrides.empty()) {
            chart_->audio.vocals = options_.vocal_overrides;
        }
        streaming_reader_ = std::move(cached.reader);
        streaming_pattern_prefix_end_us_.clear();
        streaming_pattern_index_sorted_ = true;
        streaming_pattern_prefix_end_us_.reserve(
            streaming_reader_->patterns().size()
        );
        std::int64_t pattern_prefix_end =
            std::numeric_limits<std::int64_t>::min();
        std::optional<std::int64_t> previous_pattern_start;
        for (const auto& pattern : streaming_reader_->patterns()) {
            if (previous_pattern_start.has_value()
                && pattern.start_us < *previous_pattern_start) {
                streaming_pattern_index_sorted_ = false;
            }
            previous_pattern_start = pattern.start_us;
            auto end = pattern.start_us;
            if (pattern.count != 0U) {
                if (const auto last = pattern.note_at(pattern.count - 1U);
                    last.has_value()) {
                    const auto exact_end = static_cast<long double>(
                        last->time_us
                    ) + static_cast<long double>(last->duration_us);
                    end = static_cast<std::int64_t>(std::clamp(
                        exact_end,
                        static_cast<long double>(
                            std::numeric_limits<std::int64_t>::min()
                        ),
                        static_cast<long double>(
                            std::numeric_limits<std::int64_t>::max()
                        )
                    ));
                }
            }
            pattern_prefix_end = std::max(pattern_prefix_end, end);
            streaming_pattern_prefix_end_us_.push_back(pattern_prefix_end);
        }
        if (!cached.visual_density_path.empty()) {
            std::string visual_index_error;
            streaming_visual_density_reader_ = VisualDensityIndexReader::open(
                cached.visual_density_path,
                &visual_index_error
            );
        }
        if (!streaming_visual_density_reader_.has_value()
            && !cached.visual_density_path.empty()) {
            std::cerr << "Streaming visual index warning: "
                      << "PVD1 could not be opened; using exact PFC1 range visits\n";
        }
        streaming_duration_ms_ = std::min(
            static_cast<double>(cached.content_end_us) / 1'000.0 + 2'000.0,
            static_cast<double>(std::numeric_limits<std::int64_t>::max())
                / 1'000.0
        );
        StreamingGameplayOptions streaming_options;
        // Rendering already has its own PVD/PFC viewport cache. The gameplay
        // scheduler therefore needs only the temporal interval in which a note
        // can actually become an input candidate. Tying judgment look-ahead to
        // scroll speed previously materialized seconds of ultra-dense chart
        // data merely because it was visible.
        const auto finite_nonnegative = [](const double value) noexcept {
            return std::isfinite(value) ? std::max(0.0, value) : 0.0;
        };
        const double judgment_look_ahead_ms =
            finite_nonnegative(options_.settings.gameplay.windows.miss_ms)
            + std::abs(
                std::isfinite(options_.settings.gameplay.input_offset_ms)
                    ? options_.settings.gameplay.input_offset_ms
                    : 0.0
            )
            + finite_nonnegative(
                options_.settings.gameplay.stacked_note_tolerance_ms
            )
            + 50.0;
        streaming_options.look_ahead_us = static_cast<std::int64_t>(
            std::clamp(
                std::ceil(
                    std::max(100.0, judgment_look_ahead_ms)
                    * 1'000.0
                ),
                1.0,
                static_cast<double>(
                    std::numeric_limits<std::int64_t>::max()
                )
            )
        );
        // Judgment remains deliberately bounded. If this temporal window is
        // denser than 262144 notes, overdue taps are resolved by the streaming
        // scheduler while the independent visual range visitor below still
        // accounts for every on-screen head through fixed-memory LOD bins.
        streaming_options.max_window_notes = 262'144U;
        // Non-batchable explicit recovery is deliberately frame-budgeted.
        // Bulk tap paths above handle the dense common case; the remainder is
        // capped below one 16k block so pathological sustains/manual recovery
        // cannot monopolize a 60-160 ms frame.
        streaming_options.max_explicit_catchup_notes_per_update = 16'384U;
        set_loading_phase(ContentLoadPhase::creating_session);
        std::string session_error;
        auto created = StreamingGameplaySession::create(
            *streaming_reader_,
            options_.settings.gameplay,
            streaming_options,
            chart_->tempos,
            &session_error
        );
        if (!created.has_value()) {
            last_error_ = session_error;
            std::cerr << session_error << '\n';
            streaming_reader_.reset();
            streaming_pattern_prefix_end_us_.clear();
            chart_.reset();
            return false;
        }
        streaming_session_ = std::make_unique<StreamingGameplaySession>(
            std::move(*created)
        );
        lane_input_counts_.assign(chart_->key_count, 0U);
        pressed_scancodes_.fill(false);
        initialize_note_type_runtime();
        initialize_presence_note_total();
        std::cout << (cached.reused ? "Reused" : "Compiled")
                  << " verified PFC1 cache: "
                  << path_utf8(cached.cache_path) << " ("
                  << streaming_reader_->explicit_note_count()
                  << " explicit notes)\n";
#if defined(PULSEFORGE_HAS_LUA)
        const bool streaming_scripts_requested = options_.script_path.has_value()
            || !options_.script_paths.empty()
            || !options_.content_roots.empty()
            || !options_.selected_content_root.empty()
            || !options_.selected_mod_root.empty();
        if (options_.enable_lua && streaming_scripts_requested) {
            set_loading_phase(ContentLoadPhase::loading_scripts);
            script_state_.reset();
            lua_host_ = std::make_unique<ApplicationLuaHost>(*this);
            LuaRuntimeConfig config;
            const auto discovered_scripts = discover_script_files();
            const auto script_count = std::max<std::size_t>(
                discovered_scripts.size(),
                1U
            );
            const auto total_memory =
                static_cast<std::size_t>(options_.settings.performance.script_memory_mb)
                * 1024U * 1024U;
            config.memory_limit_bytes = std::max<std::size_t>(
                total_memory / script_count,
                256U * 1024U
            );
            config.deterministic_seed = streaming_session_->settings().random_seed;
            config.instruction_budget =
                options_.settings.performance.script_instruction_budget;
            scripts_ = std::make_unique<LuaScriptManager>(*lua_host_, config);
            if (!reload_script(&discovered_scripts)) {
                std::cerr << "No Lua scripts remained active after loading\n";
            }
        }
#else
        if (options_.enable_lua
            && (options_.script_path.has_value()
                || !options_.script_paths.empty())) {
            std::cerr << "Lua warning: this build was compiled without Lua support\n";
        }
#endif
        if (options_.save_replay_path.has_value()) {
            std::cerr
                << "Replay warning: recording is disabled for PFC1 v1 "
                   "streaming gameplay\n";
        }
        return true;
    }

    [[nodiscard]] bool load_content() {
        set_loading_phase(ContentLoadPhase::parsing_chart);

        // MIDI/PFM are source/interchange formats that compile straight to
        // the bounded PFC1 runtime. They deliberately bypass the materialized
        // JSON Chart path even when the source itself is tiny.
        if (!options_.chart_path.empty()
            && (is_midi_chart_path(options_.chart_path)
                || is_pfm_chart_path(options_.chart_path)
                || is_pfm_source_path(options_.chart_path))) {
            return load_streaming_content(
                "MIDI/PFM source selected; compiling directly to PFC1 streaming"
            );
        }

        // PULSEFORGE_P1_5_0E_SINGLE_PASS_LARGE_CHART_ROUTE_V1
        // The hard materialized limit remains unchanged, but large JSON sources
        // are much faster when we choose PFC1 before doing a complete first
        // parse that would only be thrown away. 32 MiB is a performance route,
        // not a compatibility cap; smaller sources still use the materialized
        // path and cached PFC1 sources remain bounded.
        constexpr std::uintmax_t preferred_streaming_json_bytes =
            32U * 1024U * 1024U;
        if (options_.enable_large_chart_streaming && !options_.chart_path.empty()) {
            std::error_code size_error;
            const auto source_bytes = std::filesystem::file_size(
                options_.chart_path,
                size_error
            );
            if (!size_error
                && source_bytes > preferred_streaming_json_bytes) {
                std::cerr
                    << "[PulseForge][streaming] source is " << source_bytes
                    << " bytes; selecting single-pass PFC1 runtime before "
                       "materialization (performance threshold "
                    << preferred_streaming_json_bytes << ")\n";
                return load_streaming_content(
                    "large JSON routed directly to bounded PFC1 before duplicate parsing"
                );
            }
        }

        auto loaded_chart = ChartLoader::load(options_.chart_path, options_.chart_options);
        if (!loaded_chart) {
            if (options_.enable_large_chart_streaming) {
                std::cerr
                    << "[PulseForge][streaming] materialized load returned: "
                    << loaded_chart.error << "\n"
                    << "[PulseForge][streaming] retrying transparently with "
                       "Phase A/B bounded PFC1 streaming\n";
                return load_streaming_content(loaded_chart.error);
            }
            last_error_ = loaded_chart.error;
            std::cerr << "Chart error: " << loaded_chart.error << '\n';
            return false;
        }
        chart_ = std::move(*loaded_chart.chart);
        if (options_.instrumental_override.has_value()) {
            chart_->audio.instrumental = *options_.instrumental_override;
        }
        if (!options_.vocal_overrides.empty()) {
            chart_->audio.vocals = options_.vocal_overrides;
        }
        set_loading_phase(
            ContentLoadPhase::indexing_notes,
            static_cast<std::uint64_t>(chart_->notes.size())
        );
        note_prefix_end_ms_.resize(chart_->notes.size());
        double furthest_note_end = -std::numeric_limits<double>::infinity();
        for (std::size_t index = 0; index < chart_->notes.size(); ++index) {
            furthest_note_end = std::max(
                furthest_note_end,
                chart_->notes[index].end_time_ms()
            );
            note_prefix_end_ms_[index] = furthest_note_end;
            if ((index & 0x0FFFU) == 0x0FFFU) {
                set_loading_work_complete(
                    static_cast<std::uint64_t>(index + 1U)
                );
            }
        }
        set_loading_work_complete(
            static_cast<std::uint64_t>(chart_->notes.size())
        );

        if (options_.replay_path.has_value()) {
            auto loaded_replay = load_replay(*options_.replay_path);
            if (!loaded_replay) {
                std::cerr << "Replay error: " << loaded_replay.error << '\n';
                return false;
            }
            if (loaded_replay.replay->chart_hash != chart_fingerprint(*chart_)) {
                std::cerr << "Replay error: chart fingerprint does not match\n";
                return false;
            }
            const double latest_replay_input = chart_->duration_ms() + 10'000.0;
            for (const auto& input : loaded_replay.replay->inputs) {
                if (input.lane >= chart_->key_count
                    || input.time_ms > latest_replay_input) {
                    std::cerr << "Replay error: input is outside this chart\n";
                    return false;
                }
            }
            replay_ = std::move(*loaded_replay.replay);
            options_.settings.gameplay = replay_->settings;
        }

        set_loading_phase(ContentLoadPhase::creating_session);
        session_ = std::make_unique<GameplaySession>(
            *chart_,
            options_.settings.gameplay
        );
        lane_input_counts_.assign(chart_->key_count, 0);
        pressed_scancodes_.fill(false);
        initialize_note_type_runtime();
        initialize_presence_note_total();

#if defined(PULSEFORGE_HAS_LUA)
        const bool verifiable_replay =
            options_.replay_path.has_value()
            || options_.save_replay_path.has_value();
        if (options_.enable_lua && verifiable_replay) {
            std::cout
                << "Lua disabled while recording/playing a verifiable replay\n";
        }
        const bool scripts_requested = options_.script_path.has_value()
            || !options_.script_paths.empty()
            || !options_.content_roots.empty()
            || !options_.selected_content_root.empty()
            || !options_.selected_mod_root.empty();
        if (options_.enable_lua
            && !verifiable_replay
            && scripts_requested) {
            set_loading_phase(ContentLoadPhase::loading_scripts);
            script_state_.reset();
            lua_host_ = std::make_unique<ApplicationLuaHost>(*this);
            LuaRuntimeConfig config;
            const auto discovered_scripts = discover_script_files();
            const auto script_count = std::max<std::size_t>(
                discovered_scripts.size(),
                1U
            );
            const auto total_memory =
                static_cast<std::size_t>(options_.settings.performance.script_memory_mb)
                * 1024U * 1024U;
            config.memory_limit_bytes = std::max<std::size_t>(
                total_memory / script_count,
                256U * 1024U
            );
            config.deterministic_seed = session_->settings().random_seed;
            config.instruction_budget =
                options_.settings.performance.script_instruction_budget;
            scripts_ = std::make_unique<LuaScriptManager>(*lua_host_, config);
            if (!reload_script(&discovered_scripts)) {
                std::cerr << "No Lua scripts remained active after loading\n";
            }
        }
#else
        if (options_.enable_lua
            && (options_.script_path.has_value()
                || !options_.script_paths.empty())) {
            std::cerr << "Lua warning: this build was compiled without Lua support\n";
        }
#endif
        return true;
    }

    [[nodiscard]] bool initialize_transferred_offline_target() {
        if (!transferred_platform_ || !options_.offline_render.enabled) {
            return true;
        }
        const auto width = options_.offline_render.width;
        const auto height = options_.offline_render.height;
        if (width == 0U || height == 0U
            || width > static_cast<std::uint32_t>(
                std::numeric_limits<int>::max()
            )
            || height > static_cast<std::uint32_t>(
                std::numeric_limits<int>::max()
            )) {
            last_error_ = "Offline render target dimensions are invalid";
            return false;
        }
        offline_render_target_ = SDL_CreateTexture(
            renderer_,
            SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_TARGET,
            static_cast<int>(width),
            static_cast<int>(height)
        );
        if (offline_render_target_ == nullptr) {
            last_error_ = std::string{
                "Unable to create the exact offline render target: "
            } + SDL_GetError();
            return false;
        }
        if (!SDL_SetRenderTarget(renderer_, offline_render_target_)) {
            last_error_ = std::string{
                "Renderer rejected the offline render target: "
            } + SDL_GetError();
            SDL_DestroyTexture(offline_render_target_);
            offline_render_target_ = nullptr;
            return false;
        }
        // Render-target state is independent from the window backbuffer in
        // SDL3. Configure the engine's 1280x720 coordinate space on this
        // texture too. STRETCH keeps the readback viewport equal to the exact
        // requested dimensions even for a custom non-16:9 render preset.
        if (!SDL_SetRenderLogicalPresentation(
                renderer_,
                static_cast<int>(logical_width),
                static_cast<int>(logical_height),
                SDL_LOGICAL_PRESENTATION_STRETCH
            )) {
            last_error_ = std::string{
                "Unable to configure the offline render coordinate space: "
            } + SDL_GetError();
            if (!SDL_SetRenderTarget(renderer_, nullptr)) {
                last_error_ += std::string{
                    " | backbuffer restore failed: "
                } + SDL_GetError();
                platform_return_safe_ = false;
            }
            SDL_DestroyTexture(offline_render_target_);
            offline_render_target_ = nullptr;
            return false;
        }
        if (!SDL_SetRenderTarget(renderer_, nullptr)) {
            last_error_ = std::string{
                "Renderer could not restore its window backbuffer: "
            } + SDL_GetError();
            platform_return_safe_ = false;
            SDL_DestroyTexture(offline_render_target_);
            offline_render_target_ = nullptr;
            return false;
        }
        return true;
    }

    [[nodiscard]] bool release_offline_render_target() noexcept {
        if (offline_render_target_ == nullptr) {
            return true;
        }
        bool restored = true;
        if (renderer_ != nullptr
            && SDL_GetRenderTarget(renderer_) == offline_render_target_
            && !SDL_SetRenderTarget(renderer_, nullptr)) {
            restored = false;
            platform_return_safe_ = false;
        }
        SDL_DestroyTexture(offline_render_target_);
        offline_render_target_ = nullptr;
        return restored;
    }

    [[nodiscard]] bool initialize_platform() {
        SDL_SetAppMetadata("PulseForge", PULSEFORGE_VERSION, "org.pulseforge.engine");
        const auto title = "PulseForge - " + chart_->title;
        if (!transferred_platform_) {
            if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
                last_error_ = std::string{"SDL initialization failed: "}
                    + SDL_GetError();
                std::cerr << last_error_ << '\n';
                return false;
            }
            sdl_initialized_ = true;
            owned_platform_.owns_sdl = true;
            const bool interactive_render = options_.offline_render.enabled
                && options_.return_to_launcher && !options_.smoke_test;
            SDL_WindowFlags flags = options_.offline_render.enabled
                ? (interactive_render
                    ? static_cast<SDL_WindowFlags>(0)
                    : SDL_WINDOW_HIDDEN)
                : SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
            if (options_.smoke_test) {
                flags |= SDL_WINDOW_HIDDEN;
            }
            if (options_.settings.visual.fullscreen
                && !options_.offline_render.enabled) {
                flags |= SDL_WINDOW_FULLSCREEN;
            }
            window_ = SDL_CreateWindow(
                title.c_str(),
                options_.offline_render.enabled
                    ? static_cast<int>(options_.offline_render.width)
                    : options_.settings.visual.width,
                options_.offline_render.enabled
                    ? static_cast<int>(options_.offline_render.height)
                    : options_.settings.visual.height,
                flags
            );
            owned_platform_.window = window_;
            if (window_ == nullptr) {
                last_error_ = std::string{"Window creation failed: "}
                    + SDL_GetError();
                std::cerr << last_error_ << '\n';
                return false;
            }
            renderer_ = SDL_CreateRenderer(
                window_,
                nullptr
            );
            if (renderer_ == nullptr && options_.offline_render.enabled) {
                // Headless/remote Windows sessions may expose no hardware
                // renderer. Preserve a functional fallback, but do not force
                // every normal render through the much slower software path.
                renderer_ = SDL_CreateRenderer(window_, "software");
            }
            owned_platform_.renderer = renderer_;
            if (renderer_ == nullptr) {
                last_error_ = std::string{"Renderer creation failed: "}
                    + SDL_GetError();
                std::cerr << last_error_ << '\n';
                return false;
            }
        } else {
            SDL_SetWindowTitle(window_, title.c_str());
            SDL_SetWindowFullscreen(
                window_,
                options_.settings.visual.fullscreen
            );
            SDL_ShowWindow(window_);
            SDL_RaiseWindow(window_);
        }
        compile_key_bindings();
        SDL_SetRenderLogicalPresentation(
            renderer_,
            static_cast<int>(logical_width),
            static_cast<int>(logical_height),
            SDL_LOGICAL_PRESENTATION_LETTERBOX
        );
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        detail::mobile_touch_controls().configure(
            window_,
            renderer_,
            options_.settings.touch,
            options_.settings.controls
        );
        // Ready/error acknowledgement screens are menu-like. The first live
        // gameplay frame switches this to lane controls using chart_->key_count.
        detail::mobile_touch_controls().set_context(
            options_.offline_render.enabled
                ? detail::MobileTouchContext::disabled
                : detail::MobileTouchContext::menu,
            chart_->key_count
        );
        const bool vsync_configured = SDL_SetRenderVSync(
            renderer_,
            options_.settings.visual.vsync
                    && !options_.offline_render.enabled
                ? 1
                : 0
        );
        vsync_active_ = options_.settings.visual.vsync
            && !options_.offline_render.enabled && vsync_configured;
        if (!vsync_configured) {
            std::cerr << "VSync warning: " << SDL_GetError() << '\n';
        }
        if (!initialize_transferred_offline_target()) {
            std::cerr << last_error_ << '\n';
            return false;
        }

        if (audio_visualizer_icon_ == nullptr) {
            if (const auto icon_path = audio_visualizer_icon_path(options_);
                icon_path.has_value()) {
                audio_visualizer_icon_ = load_rgba_texture(
                    renderer_,
                    *icon_path
                );
                if (audio_visualizer_icon_ == nullptr) {
                    std::cerr
                        << "Audio visualizer warning: could not decode "
                        << path_utf8(*icon_path)
                        << "; using vector fallback\n";
                }
            } else {
                std::cerr
                    << "Audio visualizer warning: selected image "
                    << audio_visualizer_icon_filename(
                        options_.settings.visual.audio_visualizer_image
                    )
                    << " was not found; using vector fallback\n";
            }
        }

        // Maximum-performance mode still honors the user's Note Skin choice.
        // It creates a bounded note-skin-only RuntimeScene: note textures and
        // atlas frames are available to render_lanes_and_notes(), while
        // stage/character descriptors, sprites, animation and scene drawing
        // remain bypassed.
        std::vector<std::filesystem::path> scene_roots;
        scene_roots.reserve(options_.content_roots.size() + 3U);
        const auto append_scene_root = [&](const std::filesystem::path& root) {
            if (root.empty()) {
                return;
            }
            const auto duplicate = std::find(
                scene_roots.begin(),
                scene_roots.end(),
                root
            );
            if (duplicate == scene_roots.end()) {
                scene_roots.push_back(root);
            }
        };
        for (const auto& root : options_.content_roots) {
            append_scene_root(root);
        }
        append_scene_root(options_.selected_content_root);
        append_scene_root(options_.selected_mod_root);

        // PULSEFORGE_P1_1_7_GENERIC_PSYCH_ROOT_RESOLVER_V1
        // One generic resolver now owns Psych/FNF distribution expansion.
        // RuntimeScene mounts LOW -> HIGH precedence, so discovered stock
        // layers are fallback-only and explicit mod/user roots remain highest.
        const auto psych_root_resolution =
            detail::resolve_psych_content_roots(scene_roots, 64U);
        scene_roots = psych_root_resolution.roots;

        for (const auto& fallback_root
             : psych_root_resolution.discovered_fallback_roots) {
            std::cout
                << "[Psych roots] fallback: "
                << path_utf8(fallback_root) << '\n';
        }
        // PULSEFORGE_P1_2_0_STOCK_PROVIDER_DIAGNOSTIC_V1
        if (psych_root_resolution.stock_provider_assets_root.has_value()) {
            std::cout
                << "[Psych roots] stock provider: "
                << path_utf8(*psych_root_resolution.stock_provider_assets_root)
                << " (score=" << psych_root_resolution.stock_provider_score
                << ", packs="
                << psych_root_resolution.stock_provider_packs_scanned
                << ", candidates="
                << psych_root_resolution.stock_provider_candidates_scanned
                << ")\n";
        }
        if (psych_root_resolution.stock_provider_scan_truncated) {
            std::cerr
                << "[Psych roots] warning: stock-provider discovery reached "
                   "its bounded scan limit\n";
        }
        if (psych_root_resolution.omitted_due_to_limit != 0U) {
            std::cerr
                << "[Psych roots] warning: omitted "
                << psych_root_resolution.omitted_due_to_limit
                << " low-priority root(s) because the bounded root budget "
                   "was exhausted\n";
        }

#if defined(PULSEFORGE_HAS_LUA)
        // PULSEFORGE_P1_1_12_SHADER_CATALOG_RUNTIME_V1
        // Reconnect the v8.0.9 Lua shader surface to the current scene roots.
        // Different imported engines mount at different depths, so the bounded
        // catalogue accepts the common Psych/FNF asset layouts without ever
        // escaping an approved root.
        {
            ShaderCatalogOptions shader_options;
            shader_options.search_directories = {
                "shaders",
                "data/shaders",
                "shared/shaders",
                "assets/shaders",
                "assets/data/shaders",
                "assets/shared/shaders",
            };
            shader_options.roots.reserve(scene_roots.size());
            for (std::size_t index = 0U; index < scene_roots.size(); ++index) {
                shader_options.roots.push_back(ShaderSearchRoot{
                    "scene-root-" + std::to_string(index),
                    scene_roots[index],
                    static_cast<std::int32_t>(index),
                });
            }
            shader_catalog_ = std::make_unique<ShaderCatalog>(
                ShaderCatalog::scan(shader_options)
            );
            initialized_shaders_.clear();
            if (shader_catalog_->truncated()) {
                std::cerr
                    << "[Shader catalog] warning: scan reached a configured "
                       "safety limit; gameplay will continue with the bounded "
                       "entries that were accepted\n";
            }
            for (const auto& diagnostic : shader_catalog_->diagnostics()) {
                if (diagnostic.severity == ShaderDiagnosticSeverity::error) {
                    std::cerr
                        << "[Shader catalog] "
                        << to_string(diagnostic.code) << ": "
                        << diagnostic.message;
                    if (!diagnostic.path.empty()) {
                        std::cerr << " (" << path_utf8(diagnostic.path) << ')';
                    }
                    std::cerr << '\n';
                }
            }
        }
#endif

        const auto parsed_note_skin = parse_note_skin_selection(
            options_.settings.visual.note_skin_selection
        );
        // An explicit user note-skin choice is authoritative. Quality and
        // performance profiles may simplify everything around the notes, while
        // dense LOD represents that same skin with bounded textured batches.
        if (!parsed_note_skin.chart_default) {
            // Notes and scene assets share the same VFS topology.
            std::vector<std::filesystem::path> discovery_roots = scene_roots;
if (const auto selected_skin = resolve_note_skin_selection(
                    discovery_roots,
                    options_.settings.visual.note_skin_selection
                );
                selected_skin.has_value()) {
                // User-selected skins keep highest VFS precedence in both
                // normal and maximum-performance modes.
                append_scene_root(selected_skin->source_root);
            }
        }

        detail::RuntimeSceneLimits scene_limits{};
        scene_limits.maximum_roots = std::min<std::size_t>(
            64U,
            std::max(scene_limits.maximum_roots, scene_roots.size())
        );
        scene_limits.note_skin_only =
            options_.settings.performance.maximum_performance_mode;

        scene_ = std::make_unique<detail::RuntimeScene>(
            renderer_,
            *chart_,
            scene_roots,
            scene_limits
        );
        if (!parsed_note_skin.chart_default) {
            scene_->override_note_skin(
                parsed_note_skin.style,
                parsed_note_skin.pixel
            );
        }
        resolve_script_note_visual_profiles();
        for (const auto& diagnostic : scene_->diagnostics()) {
            std::cerr << "Scene "
                      << (diagnostic.severity
                                  == detail::RuntimeSceneDiagnosticSeverity::error
                              ? "error: "
                              : "warning: ")
                      << diagnostic.message << '\n';
        }
#if defined(PULSEFORGE_HAS_LUA)
        activate_lua_create_callbacks();
#endif

        const bool audio_initialized = options_.smoke_test
                || options_.offline_render.enabled
            ? audio_.initialize(
                options_.settings.audio,
                AudioTransportBackend::null_device,
                &last_error_
            )
            : audio_.initialize(options_.settings.audio, &last_error_);
        if (!audio_initialized) {
            std::cerr << "Audio initialization failed: " << last_error_ << '\n';
            return false;
        }
        const bool audio_loaded = streaming_mode()
            ? audio_.load(
                chart_->audio,
                streaming_duration_ms_,
                120.0,
                &last_error_
            )
            : audio_.load(*chart_, &last_error_);
        if (!audio_loaded) {
            std::cerr << "Audio load failed: " << last_error_ << '\n';
            return false;
        }
        // PULSEFORGE_P1_5_0C_DECLARATIVE_HITSOUND_PRECACHE_V1
        precache_note_type_hitsounds();
        if (options_.settings.visual.theme == PresentationTheme::ps2 && !options_.smoke_test) {
            std::error_code instrumental_error;
            const bool missing_instrumental = chart_->audio.instrumental.empty()
                || !std::filesystem::is_regular_file(
                    chart_->audio.instrumental,
                    instrumental_error
                )
                || instrumental_error;
            if (missing_instrumental) {
                last_error_ = "Instrumental audio is missing or unreadable for chart '"
                    + chart_->title
                    + "'. PulseForge PS2 theme treats a missing Inst stem as a fatal disc/data read error.";
                std::cerr << "Audio load failed: " << last_error_ << '\n';
                return false;
            }
        }
        if (audio_.using_silent_audio()) {
            std::cerr
                << "Audio warning: no Inst/Voices stems were found for chart '"
                << chart_->title << "'; playback will be silent\n";
        }
        if (!options_.offline_render.enabled) {
            open_existing_gamepads();
        }
        return true;
    }

    void shutdown() noexcept {
        discord_presence_.clear();
#if defined(PULSEFORGE_HAS_LUA)
        if (scripts_ != nullptr) {
            try {
                script_state_.clear_transient_output();
                static_cast<void>(scripts_->on_destroy());
                consume_script_output();
            } catch (...) {
                std::cerr << "Lua shutdown warning: onDestroy could not finish\n";
            }
        }
        scripts_.reset();
        lua_host_.reset();
        initialized_shaders_.clear();
        shader_catalog_.reset();
#endif
        pause_music_.shutdown();
        audio_.shutdown();
        for (auto* gamepad : gamepads_) {
            SDL_CloseGamepad(gamepad);
        }
        gamepads_.clear();
        offline_encoder_.reset();
        scene_.reset();
        if (audio_visualizer_icon_ != nullptr) {
            SDL_DestroyTexture(audio_visualizer_icon_);
            audio_visualizer_icon_ = nullptr;
        }
        post_effects_.reset();
        static_cast<void>(release_offline_render_target());
        // RuntimeScene and every subsystem capable of referring to SDL
        // resources are gone before ownership crosses this boundary.
        owned_platform_.window = window_;
        owned_platform_.renderer = renderer_;
        owned_platform_.owns_sdl = sdl_initialized_;
        if (return_platform_ != nullptr && platform_return_safe_
            && owned_platform_.complete()) {
            // Same SDL renderer is about to return to MenuSession. Preserve the
            // global router/event watch but release all gameplay lanes and put
            // it in menu mode until the launcher refreshes its settings.
            detail::mobile_touch_controls().set_context(detail::MobileTouchContext::menu);
            *return_platform_ = std::move(owned_platform_);
        } else {
            // No owner will adopt this SDL stack; detach the event watch before
            // renderer/window destruction and SDL_Quit.
            detail::mobile_touch_controls().shutdown();
            owned_platform_.reset();
        }
        renderer_ = nullptr;
        window_ = nullptr;
        sdl_initialized_ = false;
        restore_low_latency_runtime();
    }

    void frame(const double elapsed_seconds, const std::uint64_t frame_start_ns) {
        sync_mobile_touch_context();
        gameplay_begin_frame();
#if defined(PULSEFORGE_HAS_LUA)
        script_state_.clear_transient_output();
        script_just_pressed_scancodes_.fill(false);
#endif
        double song_time = audio_.compensated_position_ms();
        process_events(frame_start_ns, song_time);
        // A pause/result transition can be caused by the events just consumed.
        // Switch overlays before this frame is rendered, not one frame later.
        sync_mobile_touch_context();
        advance_random_pause_music();
        if (physical_resync_pending_) {
            physical_resync_pending_ = false;
            resynchronize_physical_inputs(audio_.compensated_position_ms());
        }
        song_time = audio_.compensated_position_ms();
        // PULSEFORGE_P1_1_17_DEFERRED_SONG_CONTROL_V1
        const bool media_ended =
            audio_.state() == AudioTransportState::ended
#if defined(PULSEFORGE_HAS_LUA)
            || script_force_song_ended_
#endif
            ;
        if (media_ended) {
            // A malformed/short stem must not strand future chart objects
            // forever at a stopped audio clock.
            song_time = std::max(song_time, gameplay_content_duration_ms());
        }

        bool gameplay_advance_allowed = !paused_ && !gameplay_failed();
#if defined(PULSEFORGE_HAS_LUA)
        gameplay_advance_allowed =
            gameplay_advance_allowed && !script_countdown_blocked_;
#endif
        if (gameplay_advance_allowed) {
            dispatch_replay_inputs(song_time);
            if (media_ended) {
                gameplay_finish_song(song_time);
            } else {
                gameplay_update(song_time);
            }
        }
        update_nps(song_time);
        consume_gameplay_events();

#if defined(PULSEFORGE_HAS_LUA)
        if (scripts_ != nullptr && !paused_) {
            service_script_sound_completions();
            if (streaming_mode()) {
                static_cast<void>(scripts_->dispatch_frame(
                    *streaming_session_,
                    elapsed_seconds
                ));
            } else {
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

        update_effects(static_cast<float>(elapsed_seconds));
        render(song_time);
        update_fps(elapsed_seconds);
        runtime_performance_.record_frame_ms(elapsed_seconds * 1'000.0);

        if (((media_ended && gameplay_complete()) || gameplay_failed())
            && !result_shown_) {
            result_shown_ = true;
            audio_.pause();
            save_replay_if_requested();
        }
        publish_runtime_presence(song_time);
        if (result_shown_) {
            report_runtime_benchmark_once();
        }
        if (options_.smoke_test && ++smoke_test_frame_count_ >= 120) {
            running_ = false;
        }
    }

    void sync_mobile_touch_context() noexcept {
        if (!chart_.has_value() || options_.offline_render.enabled) {
            detail::mobile_touch_controls().set_context(detail::MobileTouchContext::disabled);
            return;
        }
        detail::mobile_touch_controls().set_context(
            (paused_ || result_shown_)
                ? detail::MobileTouchContext::menu
                : detail::MobileTouchContext::gameplay,
            chart_->key_count
        );
    }

    void process_events(
        const std::uint64_t frame_start_ns,
        const double current_song_ms
    ) {
        SDL_Event event;
        while (detail::poll_mobile_event(&event)) {
            switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                // X/Alt+F4 are deliberately non-destructive during gameplay:
                // they pause the chart. Exiting the engine remains a launcher
                // action, so the pause menu has no hidden confirmation path.
                close_request_notice_ = true;
                open_pause_menu();
                return;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                window_focused_ = false;
                audio_.set_focused(false);
                pause_music_.set_focused(false);
                if (options_.settings.performance.auto_pause_on_focus_loss
                    && can_auto_pause()) {
                    pause_playback();
                } else if (paused_) {
                    physical_resync_pending_ = false;
                    reset_physical_inputs();
                } else {
                    physical_resync_pending_ = false;
                    release_all_inputs(current_song_ms);
                }
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                window_focused_ = true;
                audio_.set_focused(true);
                pause_music_.set_focused(true);
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                open_gamepad(event.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED: {
                const bool removed = close_gamepad(event.gdevice.which);
                if (paused_) {
                    reset_physical_inputs();
                } else if (removed
                    && options_.settings.performance
                           .pause_on_controller_disconnect
                    && can_auto_pause()) {
                    pause_playback();
                } else {
                    physical_resync_pending_ = true;
                }
                break;
            }
            case SDL_EVENT_KEY_DOWN:
                handle_key(event.key, true, frame_start_ns, current_song_ms);
                break;
            case SDL_EVENT_KEY_UP:
                handle_key(event.key, false, frame_start_ns, current_song_ms);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                handle_gamepad_button(
                    event.gbutton,
                    true,
                    frame_start_ns,
                    current_song_ms
                );
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                handle_gamepad_button(
                    event.gbutton,
                    false,
                    frame_start_ns,
                    current_song_ms
                );
                break;
            default:
                break;
            }
        }
    }

    void apply_master_audio_settings() noexcept {
        audio_.set_master_volume(options_.settings.audio.master_volume);
        audio_.set_muted(options_.settings.audio.muted);
        if (pause_music_.initialized()) {
            pause_music_.set_master_volume(options_.settings.audio.master_volume);
            pause_music_.set_muted(options_.settings.audio.muted);
        }
    }

    void persist_audio_settings() {
        if (options_.settings_path.empty()) {
            return;
        }
        std::string error;
        if (!save_settings(
                options_.settings_path,
                options_.settings,
                &error
            )) {
            std::cerr << "Audio settings save warning: " << error << '\n';
        }
    }

    [[nodiscard]] bool handle_global_audio_action(
        const SDL_KeyboardEvent& event
    ) {
        if (detail::keyboard_action_matches(
                options_.settings.controls,
                "volume_up",
                event
            )) {
            static_cast<void>(adjust_master_volume(options_.settings.audio, 1));
        } else if (detail::keyboard_action_matches(
                       options_.settings.controls,
                       "volume_down",
                       event
                   )) {
            static_cast<void>(adjust_master_volume(options_.settings.audio, -1));
        } else if (detail::keyboard_action_matches(
                       options_.settings.controls,
                       "volume_mute",
                       event
                   )) {
            toggle_master_mute(options_.settings.audio);
        } else {
            return false;
        }
        apply_master_audio_settings();
        persist_audio_settings();
        return true;
    }

    [[nodiscard]] bool handle_global_audio_action(
        const SDL_GamepadButtonEvent& event
    ) {
        if (detail::gamepad_action_matches(
                options_.settings.controls,
                "volume_up",
                event
            )) {
            static_cast<void>(adjust_master_volume(options_.settings.audio, 1));
        } else if (detail::gamepad_action_matches(
                       options_.settings.controls,
                       "volume_down",
                       event
                   )) {
            static_cast<void>(adjust_master_volume(options_.settings.audio, -1));
        } else if (detail::gamepad_action_matches(
                       options_.settings.controls,
                       "volume_mute",
                       event
                   )) {
            toggle_master_mute(options_.settings.audio);
        } else {
            return false;
        }
        apply_master_audio_settings();
        persist_audio_settings();
        return true;
    }

    [[nodiscard]] std::optional<std::size_t> selected_pause_music_index() const {
        if (!options_.settings.audio.menu_music_loop_selected
            || options_.settings.audio.menu_music_selection == "randomized") {
            return std::nullopt;
        }
        const auto iterator = std::find_if(
            pause_music_catalog_.begin(),
            pause_music_catalog_.end(),
            [&](const auto& candidate) {
                return path_utf8(candidate.filename())
                    == options_.settings.audio.menu_music_selection;
            }
        );
        if (iterator == pause_music_catalog_.end()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(
            std::distance(pause_music_catalog_.begin(), iterator)
        );
    }

    [[nodiscard]] std::size_t next_random_pause_music_index() noexcept {
        const auto count = pause_music_catalog_.size();
        if (count <= 1U) {
            return 0U;
        }
        if (pause_music_random_state_ == 0U) {
            pause_music_random_state_ = SDL_GetTicksNS() | 1U;
        }
        pause_music_random_state_ ^= pause_music_random_state_ << 13U;
        pause_music_random_state_ ^= pause_music_random_state_ >> 7U;
        pause_music_random_state_ ^= pause_music_random_state_ << 17U;
        if (pause_music_index_ < count) {
            return (pause_music_index_ + 1U
                    + static_cast<std::size_t>(
                        pause_music_random_state_ % (count - 1U)
                    ))
                % count;
        }
        return static_cast<std::size_t>(pause_music_random_state_ % count);
    }

    [[nodiscard]] bool load_pause_music_track() {
        if (pause_music_catalog_.empty()) {
            pause_music_catalog_ = pause_music_catalog(options_);
        }
        if (pause_music_catalog_.empty()) {
            return false;
        }

        std::vector<std::size_t> candidates;
        candidates.reserve(pause_music_catalog_.size());
        const auto selected = selected_pause_music_index();
        if (selected.has_value()) {
            candidates.push_back(*selected);
        }
        const auto random_start = next_random_pause_music_index();
        for (std::size_t offset = 0U;
             offset < pause_music_catalog_.size();
             ++offset) {
            const auto index = (random_start + offset)
                % pause_music_catalog_.size();
            if (!selected.has_value() || index != *selected) {
                candidates.push_back(index);
            }
        }

        std::string last_error;
        for (const auto index : candidates) {
            AudioManifest manifest;
            manifest.instrumental = pause_music_catalog_[index];
            std::string error;
            if (!pause_music_.load(manifest, 60'000.0, 120.0, &error)) {
                last_error = std::move(error);
                continue;
            }
            pause_music_index_ = index;
            pause_music_selected_loop_active_ = selected.has_value()
                && index == *selected;
            pause_music_.set_looping(pause_music_selected_loop_active_);
            pause_music_.set_focused(window_focused_);
            return true;
        }
        std::cerr << "Pause music warning: "
                  << (last_error.empty()
                        ? "no usable menu music track"
                        : last_error)
                  << '\n';
        return false;
    }

    [[nodiscard]] bool ensure_pause_music() noexcept {
        if (pause_music_.loaded()) {
            return true;
        }
        if (pause_music_failed_ || options_.offline_render.enabled) {
            return false;
        }
        try {
            AudioSettings music_settings = options_.settings.audio;
            music_settings.playback_rate = 1.0;
            std::string error;
            const bool initialized = options_.smoke_test
                ? pause_music_.initialize(
                    music_settings,
                    AudioTransportBackend::null_device,
                    &error
                )
                : pause_music_.initialize(music_settings, &error);
            if (!initialized || !load_pause_music_track()) {
                pause_music_failed_ = true;
                pause_music_.shutdown();
                if (!initialized) {
                    std::cerr << "Pause music warning: " << error << '\n';
                }
                return false;
            }
            return true;
        } catch (const std::exception& exception) {
            pause_music_failed_ = true;
            pause_music_.shutdown();
            std::cerr << "Pause music warning: " << exception.what() << '\n';
            return false;
        } catch (...) {
            pause_music_failed_ = true;
            pause_music_.shutdown();
            std::cerr << "Pause music warning: unexpected initialization failure\n";
            return false;
        }
    }

    void advance_random_pause_music() noexcept {
        if (!paused_ || pause_music_selected_loop_active_
            || pause_music_.state() != AudioTransportState::ended) {
            return;
        }
        try {
            if (load_pause_music_track()) {
                apply_master_audio_settings();
                pause_music_.play();
            } else {
                pause_music_failed_ = true;
            }
        } catch (const std::exception& exception) {
            pause_music_failed_ = true;
            std::cerr << "Pause music warning: " << exception.what() << '\n';
        } catch (...) {
            pause_music_failed_ = true;
            std::cerr << "Pause music warning: playlist advance failed\n";
        }
    }

    void start_pause_music() noexcept {
        if (!ensure_pause_music()) {
            return;
        }
        apply_master_audio_settings();
        if (pause_music_.state() == AudioTransportState::paused) {
            pause_music_.resume();
        } else if (pause_music_.state() != AudioTransportState::playing) {
            pause_music_.play();
        }
    }

    void handle_key(
        const SDL_KeyboardEvent& key,
        const bool pressed,
        const std::uint64_t now_ns,
        const double current_song_ms
    ) {
        if (pressed && key.repeat) {
            return;
        }
        if (const auto touch_lane = detail::mobile_touch_lane_from_event(key);
            touch_lane.has_value()) {
#if defined(PULSEFORGE_HAS_LUA)
            if (script_countdown_blocked_) {
                return;
            }
#endif
            if (options_.replay_path.has_value() || paused_ || result_shown_
                || *touch_lane >= chart_->key_count) {
                return;
            }
            const double event_time = timestamp_to_song_time(
                key.timestamp,
                now_ns,
                current_song_ms,
                audio_.playback_rate()
            );
            last_input_age_ms_ = current_song_ms - event_time;
            runtime_performance_.record_input_age_ms(
                std::max(last_input_age_ms_, 0.0)
            );
            apply_lane_input(*touch_lane, pressed, event_time);
            return;
        }
#if defined(PULSEFORGE_HAS_LUA)
        if (pressed) {
            const auto script_index = static_cast<int>(key.scancode);
            if (script_index > static_cast<int>(SDL_SCANCODE_UNKNOWN)
                && script_index < SDL_SCANCODE_COUNT) {
                script_just_pressed_scancodes_[
                    static_cast<std::size_t>(script_index)
                ] = true;
            }
        }
#endif
        if (pressed) {
            if (handle_global_audio_action(key)) {
                return;
            }
            if (paused_ && handle_pause_menu_key(key.scancode)) {
                return;
            }
            if (result_shown_
                && (key.scancode == SDL_SCANCODE_RETURN
                    || key.scancode == SDL_SCANCODE_KP_ENTER
                    || key.scancode == SDL_SCANCODE_SPACE)) {
                campaign_result_acknowledged_ = true;
                request_return_to_menu();
                return;
            }
            switch (key.scancode) {
            case SDL_SCANCODE_F1:
                if (options_.return_to_launcher) {
                    open_pause_menu();
                    pause_menu_selection_ = 2U;
                }
                return;
            case SDL_SCANCODE_ESCAPE:
                toggle_pause();
                return;
            case SDL_SCANCODE_R:
                restart();
                return;
            case SDL_SCANCODE_F2:
                if (!replay_.has_value()
                    && !options_.save_replay_path.has_value()) {
                    gameplay_settings().autoplay =
                        !gameplay_settings().autoplay;
                }
                return;
            case SDL_SCANCODE_F3:
                diagnostics_ = !diagnostics_;
                return;
            case SDL_SCANCODE_F5:
#if defined(PULSEFORGE_HAS_LUA)
                if (options_.settings.performance.hot_reload_scripts) {
                    static_cast<void>(reload_script());
                }
#endif
                return;
            case SDL_SCANCODE_F11:
                toggle_fullscreen();
                return;
            default:
                break;
            }
        }

#if defined(PULSEFORGE_HAS_LUA)
        if (script_countdown_blocked_) {
            return;
        }
#endif
        if (options_.replay_path.has_value() || paused_ || result_shown_) {
            return;
        }
        const auto scancode_index = static_cast<std::size_t>(key.scancode);
        if (scancode_index >= pressed_scancodes_.size()) {
            return;
        }
        if (pressed_scancodes_[scancode_index] == pressed) {
            return;
        }
        pressed_scancodes_[scancode_index] = pressed;

        const auto lane = lane_for_scancode(key.scancode);
        if (!lane.has_value()) {
            return;
        }
        const double event_time = timestamp_to_song_time(
            key.timestamp,
            now_ns,
            current_song_ms,
            audio_.playback_rate()
        );
        last_input_age_ms_ = current_song_ms - event_time;
        runtime_performance_.record_input_age_ms(
            std::max(last_input_age_ms_, 0.0)
        );
        apply_lane_input(*lane, pressed, event_time);
    }

    void handle_gamepad_button(
        const SDL_GamepadButtonEvent& button,
        const bool pressed,
        const std::uint64_t now_ns,
        const double current_song_ms
    ) {
        if (pressed && handle_global_audio_action(button)) {
            return;
        }
        if (paused_ && pressed
            && handle_pause_menu_button(
                static_cast<SDL_GamepadButton>(button.button)
            )) {
            return;
        }
        if (button.button == SDL_GAMEPAD_BUTTON_START) {
            if (pressed) {
                toggle_pause();
            }
            return;
        }
        if (pressed && result_shown_
            && button.button == SDL_GAMEPAD_BUTTON_SOUTH) {
            campaign_result_acknowledged_ = true;
            request_return_to_menu();
            return;
        }
        if (options_.replay_path.has_value() || paused_ || result_shown_) {
            return;
        }
        const auto button_index = static_cast<int>(button.button);
        if (button_index < 0 || button_index >= SDL_GAMEPAD_BUTTON_COUNT) {
            return;
        }
        const auto lane = gamepad_lane_map_[
            static_cast<std::size_t>(button_index)
        ];
        if (lane == std::numeric_limits<std::uint16_t>::max()
            || lane >= chart_->key_count) {
            return;
        }
        const double event_time = timestamp_to_song_time(
            button.timestamp,
            now_ns,
            current_song_ms,
            audio_.playback_rate()
        );
        last_input_age_ms_ = current_song_ms - event_time;
        runtime_performance_.record_input_age_ms(
            std::max(last_input_age_ms_, 0.0)
        );
        apply_lane_input(lane, pressed, event_time);
    }

    [[nodiscard]] std::optional<std::uint16_t> lane_for_scancode(
        const SDL_Scancode scancode
    ) const {
        const auto index = static_cast<int>(scancode);
        if (index < 0 || index >= SDL_SCANCODE_COUNT) {
            return std::nullopt;
        }
        const auto lane = keyboard_lane_map_[static_cast<std::size_t>(index)];
        if (lane != std::numeric_limits<std::uint16_t>::max()) {
            return lane;
        }
        return std::nullopt;
    }

    void compile_key_bindings() {
        constexpr auto invalid_lane = std::numeric_limits<std::uint16_t>::max();
        keyboard_lane_map_.fill(invalid_lane);
        gamepad_lane_map_.fill(invalid_lane);
        for (const auto& binding : options_.settings.keyboard) {
            const auto scancode = SDL_GetScancodeFromName(binding.key.c_str());
            const auto index = static_cast<int>(scancode);
            if (index <= static_cast<int>(SDL_SCANCODE_UNKNOWN)
                || index >= SDL_SCANCODE_COUNT) {
                std::cerr << "Ignoring unknown SDL key binding: "
                          << binding.key << '\n';
                continue;
            }
            if (binding.lane >= chart_->key_count) {
                continue;
            }
            auto& slot = keyboard_lane_map_[static_cast<std::size_t>(index)];
            if (slot == invalid_lane) {
                slot = binding.lane;
            }
        }
        for (const auto& binding : options_.settings.gamepad) {
            const auto button = SDL_GetGamepadButtonFromString(
                binding.button.c_str()
            );
            const auto index = static_cast<int>(button);
            if (index < 0
                || index >= SDL_GAMEPAD_BUTTON_COUNT) {
                std::cerr << "Ignoring unknown SDL gamepad button binding: "
                          << binding.button << '\n';
                continue;
            }
            if (button == SDL_GAMEPAD_BUTTON_START) {
                std::cerr << "Ignoring gamepad START lane binding; START is "
                             "reserved for pause\n";
                continue;
            }
            if (binding.lane >= chart_->key_count) {
                continue;
            }
            auto& slot = gamepad_lane_map_[static_cast<std::size_t>(index)];
            if (slot == invalid_lane) {
                slot = binding.lane;
            }
        }
    }

    void apply_lane_input(
        const std::uint16_t lane,
        const bool pressed,
        const double event_time
    ) {
        if (lane >= lane_input_counts_.size()) {
            return;
        }
        auto& count = lane_input_counts_[lane];
        if (pressed) {
            if (count == 0) {
                gameplay_press(lane, event_time);
            }
            count = static_cast<std::uint16_t>(
                std::min<std::uint32_t>(count + 1U, 65'535U)
            );
        } else {
            if (count == 0) {
                return;
            }
            --count;
            if (count == 0) {
                gameplay_release(lane, event_time);
            }
        }
    }

    void release_all_inputs(const double song_time) {
        for (std::size_t lane = 0; lane < lane_input_counts_.size(); ++lane) {
            if (lane_input_counts_[lane] > 0) {
                gameplay_release(
                    static_cast<std::uint16_t>(lane),
                    song_time
                );
            }
        }
        reset_physical_inputs();
    }

    void reset_physical_inputs() {
        lane_input_counts_.assign(chart_->key_count, 0);
        pressed_scancodes_.fill(false);
    }

    void resynchronize_physical_inputs(const double song_time) {
        if (options_.replay_path.has_value()) {
            reset_physical_inputs();
            return;
        }
        std::vector<std::uint16_t> current_counts(chart_->key_count, 0);
        std::array<bool, SDL_SCANCODE_COUNT> current_scancodes{};
        const auto register_lane = [&current_counts](const std::uint16_t lane) {
            if (lane >= current_counts.size()) {
                return;
            }
            auto& count = current_counts[lane];
            count = static_cast<std::uint16_t>(
                std::min<std::uint32_t>(count + 1U, 65'535U)
            );
        };

        int keyboard_count = 0;
        const bool* keyboard_state = SDL_GetKeyboardState(&keyboard_count);
        const auto usable_keyboard_count = std::min<std::size_t>(
            keyboard_count > 0 ? static_cast<std::size_t>(keyboard_count) : 0U,
            current_scancodes.size()
        );
        if (keyboard_state != nullptr) {
            for (std::size_t index = 0; index < usable_keyboard_count; ++index) {
                if (!keyboard_state[index]) {
                    continue;
                }
                current_scancodes[index] = true;
                const auto lane = keyboard_lane_map_[index];
                if (lane != std::numeric_limits<std::uint16_t>::max()) {
                    register_lane(lane);
                }
            }
        }

        for (auto* gamepad : gamepads_) {
            for (std::size_t index = 0; index < gamepad_lane_map_.size(); ++index) {
                const auto lane = gamepad_lane_map_[index];
                if (lane == std::numeric_limits<std::uint16_t>::max()) {
                    continue;
                }
                if (SDL_GetGamepadButton(
                        gamepad,
                        static_cast<SDL_GamepadButton>(index)
                    )) {
                    register_lane(lane);
                }
            }
        }

        for (std::size_t lane = 0; lane < current_counts.size(); ++lane) {
            if (current_counts[lane] == 0
                && gameplay_lane_held(static_cast<std::uint16_t>(lane))) {
                gameplay_release(static_cast<std::uint16_t>(lane), song_time);
            }
        }
        lane_input_counts_ = std::move(current_counts);
        pressed_scancodes_ = current_scancodes;
    }

    enum class PauseMenuAction : std::uint8_t {
        resume,
        restart,
        return_to_menu,
    };

    static constexpr std::array<std::string_view, 3> pause_menu_labels{
        "RESUME",
        "RESTART",
        "RETURN TO MENU",
    };

    void toggle_pause() {
        if (paused_) {
            resume_from_pause();
        } else {
#if defined(PULSEFORGE_HAS_LUA)
            if (scripts_ != nullptr
                && !result_shown_
                && scripts_->on_pause().stop_requested) {
                return;
            }
#endif
            open_pause_menu();
        }
    }

    void open_pause_menu() noexcept {
        if (paused_) {
            return;
        }
        pause_playback();
    }

    void resume_from_pause() {
        if (!paused_) {
            return;
        }
        paused_ = false;
        close_request_notice_ = false;
        pause_music_.pause();
        if (result_shown_) {
            return;
        }
        physical_resync_pending_ = true;
        audio_.resume();
#if defined(PULSEFORGE_HAS_LUA)
        if (scripts_ != nullptr) {
            static_cast<void>(scripts_->on_resume());
        }
#endif
    }

    [[nodiscard]] bool handle_pause_menu_key(
        const SDL_Scancode scancode
    ) {
        switch (scancode) {
        case SDL_SCANCODE_UP:
        case SDL_SCANCODE_W:
            select_previous_pause_item();
            return true;
        case SDL_SCANCODE_DOWN:
        case SDL_SCANCODE_S:
            select_next_pause_item();
            return true;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
        case SDL_SCANCODE_SPACE:
            activate_pause_item();
            return true;
        case SDL_SCANCODE_ESCAPE:
            resume_from_pause();
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] bool handle_pause_menu_button(
        const SDL_GamepadButton button
    ) {
        switch (button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP:
            select_previous_pause_item();
            return true;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
            select_next_pause_item();
            return true;
        case SDL_GAMEPAD_BUTTON_SOUTH:
            activate_pause_item();
            return true;
        case SDL_GAMEPAD_BUTTON_EAST:
        case SDL_GAMEPAD_BUTTON_START:
            resume_from_pause();
            return true;
        default:
            return false;
        }
    }

    void select_previous_pause_item() noexcept {
        pause_menu_selection_ = (pause_menu_selection_
                + pause_menu_labels.size() - 1U)
            % pause_menu_labels.size();
    }

    void select_next_pause_item() noexcept {
        pause_menu_selection_ = (pause_menu_selection_ + 1U)
            % pause_menu_labels.size();
    }

    void activate_pause_item() {
        switch (static_cast<PauseMenuAction>(pause_menu_selection_)) {
        case PauseMenuAction::resume:
            resume_from_pause();
            break;
        case PauseMenuAction::restart:
            close_request_notice_ = false;
            restart();
            break;
        case PauseMenuAction::return_to_menu:
            request_return_to_menu();
            break;
        }
    }

    void request_return_to_menu() noexcept {
        if (options_.return_to_launcher) {
            return_to_launcher_requested_ = true;
        }
        running_ = false;
        audio_.pause();
        pause_music_.pause();
    }

    [[nodiscard]] bool can_auto_pause() const noexcept {
        return !paused_
            && !result_shown_
            && !options_.replay_path.has_value();
    }

    void pause_playback() noexcept {
        // Every newly opened pause starts on the non-destructive action. This
        // also covers focus-loss and controller-disconnect auto-pauses, which
        // enter through this lower-level helper.
        pause_menu_selection_ = 0U;
        paused_ = true;
        physical_resync_pending_ = false;
        audio_.pause();
        start_pause_music();
    }

    void restart() {
        if (!gameplay_reset()) {
            running_ = false;
            return;
        }
        gameplay_settings().scroll_speed =
            options_.settings.gameplay.scroll_speed;
        scroll_tween_duration_ = 0.0;
        scroll_tween_elapsed_ = 0.0;
        replay_input_index_ = 0;
        lane_input_counts_.assign(chart_->key_count, 0);
        pressed_scancodes_.fill(false);
        timing_history_.fill(0.0F);
        timing_history_count_ = 0;
        timing_history_cursor_ = 0;
        nps_samples_.clear();
        nps_last_successful_hits_ = 0U;
        current_nps_ = 0U;
        nps_last_song_time_ = 0.0;
        last_input_age_ms_ = 0.0;
        runtime_performance_.reset();
        runtime_benchmark_reported_ = false;
        result_shown_ = false;
        campaign_result_acknowledged_ = false;
        replay_saved_ = false;
        paused_ = false;
        pause_music_.pause();
        physical_resync_pending_ = false;
        audio_.seek_ms(0.0);
#if defined(PULSEFORGE_HAS_LUA)
        // A restart requested from Lua is serviced after callback dispatch,
        // so clear all one-shot control flags before the new song lifecycle.
        script_restart_requested_ = false;
        script_force_song_ended_ = false;
        script_state_.reset();
        if (scripts_ != nullptr) {
            static_cast<void>(scripts_->on_song_start());
            consume_script_output();
        }
#endif
        audio_.play();
    }

    void toggle_fullscreen() {
        const bool fullscreen =
            (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
        if (!SDL_SetWindowFullscreen(window_, !fullscreen)) {
            std::cerr << "Fullscreen warning: " << SDL_GetError() << '\n';
        }
    }

    void dispatch_replay_inputs(const double song_time) {
        if (!replay_.has_value()) {
            return;
        }
        while (replay_input_index_ < replay_->inputs.size()
               && replay_->inputs[replay_input_index_].time_ms <= song_time) {
            const auto& input = replay_->inputs[replay_input_index_++];
            if (input.pressed) {
                gameplay_press(input.lane, input.time_ms);
            } else {
                gameplay_release(input.lane, input.time_ms);
            }
        }
    }

    void update_nps(const double song_time) {
        if (!std::isfinite(song_time)) {
            return;
        }
        const auto& score = gameplay_summary();
        const std::uint64_t successful_hits = score.marvelous
            + score.sick + score.good + score.bad;
        if (song_time + 0.001 < nps_last_song_time_
            || successful_hits < nps_last_successful_hits_) {
            nps_samples_.clear();
            current_nps_ = 0U;
        }
        if (successful_hits > nps_last_successful_hits_) {
            const auto delta = successful_hits - nps_last_successful_hits_;
            nps_samples_.emplace_back(song_time, delta);
            current_nps_ = delta
                    > std::numeric_limits<std::uint64_t>::max() - current_nps_
                ? std::numeric_limits<std::uint64_t>::max()
                : current_nps_ + delta;
        }
        nps_last_successful_hits_ = successful_hits;
        nps_last_song_time_ = song_time;
        const double cutoff = song_time - 1'000.0;
        while (!nps_samples_.empty()
               && nps_samples_.front().first < cutoff) {
            const auto expired = nps_samples_.front().second;
            current_nps_ = expired > current_nps_
                ? 0U
                : current_nps_ - expired;
            nps_samples_.pop_front();
        }
    }

    [[nodiscard]] std::uint64_t dropped_gameplay_callbacks() const noexcept {
        return streaming_mode()
            ? streaming_session_->dropped_frame_events()
            : session_->dropped_frame_events();
    }

    void consume_gameplay_events() {
#if defined(PULSEFORGE_HAS_LUA)
        // PULSEFORGE_P1_1_19_AUTOMATIC_CAMERA_TURN_FOCUS_V1
        // Psych normally restores camFollow when mustHitSection changes. The
        // compact PulseForge chart model does not retain section objects, so
        // use the authoritative resolved singing owner as the generic turn
        // transition. Only owner changes refocus: scripts may still adjust or
        // override the camera while the same side remains active.
        const auto focus_camera_for_owner = [&](const NoteOwner owner) {
            if (scene_ == nullptr) {
                return;
            }
            const auto role = owner == NoteOwner::player
                ? detail::PsychCameraRole::player
                : detail::PsychCameraRole::opponent;
            // PULSEFORGE_P1_4_0_THREE_OWNER_CAMERA_TURN_V1
            // The legacy Psych turn tracker has only player/opponent roles.
            // Keep an owner-level identity as well so dad -> player4 and
            // player4 -> dad are real camera turns rather than being collapsed
            // into one opponent role.
            if (script_auto_camera_owner_.has_value()
                && *script_auto_camera_owner_ == owner) {
                return;
            }

            const std::string_view target = owner == NoteOwner::player
                ? std::string_view{"boyfriend"}
                : owner == NoteOwner::secondary_opponent
                    ? std::string_view{"player4"}
                    : std::string_view{"dad"};
            double target_x{};
            double target_y{};
            if (!scene_->script_get_camera_target(target, target_x, target_y)) {
                return;
            }

            script_cam_game_x_ = std::clamp(
                target_x - static_cast<double>(logical_width) * 0.5,
                -100'000.0,
                100'000.0
            );
            script_cam_game_y_ = std::clamp(
                target_y - static_cast<double>(logical_height) * 0.5,
                -100'000.0,
                100'000.0
            );
            scene_->script_set_game_camera(
                script_cam_game_x_,
                script_cam_game_y_,
                script_cam_game_zoom_,
                script_cam_game_angle_,
                script_cam_game_alpha_
            );
            script_auto_camera_turn_.commit(role);
            script_auto_camera_owner_ = owner;
        };
#else
        const auto focus_camera_for_owner = [](const NoteOwner) noexcept {};
#endif

        if (streaming_mode()) {
            std::uint32_t cosmetic_bursts = 0U;
            std::uint32_t character_animation_events = 0U;
            constexpr std::uint32_t maximum_character_animations_per_frame = 4U;
            const auto note_kinds = streaming_reader_->kinds();
            const auto note_kind = [&](const PackedNote& note) noexcept {
                return note.kind_id < note_kinds.size()
                    ? std::string_view(note_kinds[note.kind_id])
                    : std::string_view{};
            };
            const auto resolve_event_note = [&](const StreamingGameplayEvent& event)
                -> std::optional<std::pair<PackedNote, std::uint16_t>> {
                if (event.has_visual_note) {
                    return std::pair{
                        event.visual_note,
                        event.visual_display_lane,
                    };
                }
                const auto id = event.note_id;
                const auto notes = streaming_session_->window_notes();
                const auto found = std::find_if(
                    notes.begin(),
                    notes.end(),
                    [&](const StreamingWindowNote& candidate) {
                        return candidate.id == id;
                    }
                );
                if (found != notes.end()) {
                    return std::pair{found->note, found->display_lane};
                }
                if (id.origin == StreamingNoteOrigin::pattern_run
                    && streaming_reader_.has_value()
                    && id.pattern_index < streaming_reader_->patterns().size()) {
                    const auto note = streaming_reader_->patterns()[
                        static_cast<std::size_t>(id.pattern_index)
                    ].note_at(id.note_index);
                    if (note.has_value()) {
                        return std::pair{
                            *note,
                            streaming_session_->display_lane(
                                runtime_owner(note->owner, note_kind(*note)),
                                note->lane
                            ),
                        };
                    }
                }
                return std::nullopt;
            };
            for (const auto& event : streaming_session_->frame_events()) {
                switch (event.type) {
                case GameplayEventType::note_hit: {
                    focus_camera_for_owner(NoteOwner::player);
                    push_timing_offset(static_cast<float>(
                        static_cast<double>(event.offset_us) / 1'000.0
                    ));
                    const bool wants_animation = scene_ != nullptr
                        && character_animation_events
                            < maximum_character_animations_per_frame;
                    const bool wants_splash = event.rating != Rating::mine
                        && !options_.settings.visual.reduced_motion
                        && cosmetic_bursts
                            < options_.settings.performance
                                   .max_cosmetic_bursts_per_frame;
                    const bool wants_hitsound = !note_type_hitsounds_.empty();
                    const auto note = wants_animation || wants_splash || wants_hitsound
                        ? resolve_event_note(event)
                        : std::nullopt;
                    if (wants_animation && note.has_value()) {
                        const auto kind = note_kind(note->first);
                        if (!note_animation_suppressed(kind)) {
                            notify_runtime_note_animation(
                                runtime_owner(note->first.owner, kind),
                                note->second,
                                static_cast<double>(event.song_time_us) / 1'000.0,
                                kind,
                                event.rating == Rating::mine,
                                runtime_sustain_tail_ms(
                                    kind,
                                    static_cast<double>(note->first.time_us) / 1'000.0,
                                    static_cast<double>(note->first.duration_us) / 1'000.0
                                )
                            );
                            ++character_animation_events;
                        }
                    }
                    if (note.has_value() && event.rating != Rating::mine) {
                        play_note_type_hitsound(note_kind(note->first));
                    }
                    if (!wants_splash) {
                        break;
                    }
                    if (note.has_value()) {
                        const auto kind = note_kind(note->first);
                        const auto owner = runtime_owner(note->first.owner, kind);
                        if (script_note_splash_enabled(owner, kind)) {
                            spawn_note_splash_lane(
                                note->second,
                                script_note_splash_profile(owner, kind)
                            );
                            ++cosmetic_bursts;
                        }
                    }
                    break;
                }
                case GameplayEventType::note_miss:
                case GameplayEventType::hold_drop: {
                    focus_camera_for_owner(NoteOwner::player);
                    push_timing_offset(static_cast<float>(
                        options_.settings.gameplay.windows.miss_ms
                    ));
                    screen_flash_ = std::max(screen_flash_, 0.22F);
                    const auto note = scene_ != nullptr
                            && character_animation_events
                                < maximum_character_animations_per_frame
                        ? resolve_event_note(event)
                        : std::nullopt;
                    if (scene_ != nullptr && note.has_value()) {
                        const auto kind = note_kind(note->first);
                        const double event_time_ms =
                            static_cast<double>(event.song_time_us) / 1'000.0;
                        if (event.type == GameplayEventType::hold_drop) {
                            // PULSEFORGE_P1_4_0_CHARACTER_SUSTAIN_DROP_RELEASE_V1
                            release_runtime_sustain_animation(
                                NoteOwner::player,
                                event_time_ms,
                                kind,
                                runtime_sustain_tail_ms(
                                    kind,
                                    static_cast<double>(note->first.time_us) / 1'000.0,
                                    static_cast<double>(note->first.duration_us) / 1'000.0
                                )
                            );
                        }
                        if (!note_animation_suppressed(kind)) {
                            notify_runtime_note_animation(
                                NoteOwner::player,
                                note->second,
                                event_time_ms,
                                kind,
                                true,
                                -1.0
                            );
                            ++character_animation_events;
                        }
                    }
                    break;
                }
                case GameplayEventType::opponent_hit: {
                    const auto note = (scene_ != nullptr
                            && character_animation_events
                                < maximum_character_animations_per_frame)
                        ? resolve_event_note(event)
                        : std::nullopt;
                    const auto kind = note.has_value()
                        ? note_kind(note->first)
                        : std::string_view{};
                    const auto owner = note.has_value()
                        ? runtime_owner(note->first.owner, kind)
                        : NoteOwner::opponent;
                    focus_camera_for_owner(owner);
                    if (scene_ != nullptr && note.has_value()
                        && !note_animation_suppressed(kind)) {
                        notify_runtime_note_animation(
                            owner,
                            note->second,
                            static_cast<double>(event.song_time_us) / 1'000.0,
                            kind,
                            false,
                            runtime_sustain_tail_ms(
                                kind,
                                static_cast<double>(note->first.time_us) / 1'000.0,
                                static_cast<double>(note->first.duration_us) / 1'000.0
                            )
                        );
                        ++character_animation_events;
                    }
                    if (note.has_value()) {
                        play_note_type_hitsound(kind);
                    }
                    break;
                }
                // PULSEFORGE_P1_1_18_STREAMING_CHART_EVENT_VISUAL_PARITY_V1
                case GameplayEventType::chart_event:
                    if (event.chart_event_index
                        < streaming_session_->chart_events().size()) {
                        const auto& chart_event =
                            streaming_session_->chart_events()[
                                event.chart_event_index
                            ];
                        handle_visual_event(
                            chart_event.name,
                            chart_event.value1,
                            chart_event.value2
                        );
                    }
                    break;
                case GameplayEventType::beat:
                    if (!options_.settings.visual.reduced_motion) {
                        beat_pulse_ = 1.0F;
                    }
                    break;
                default:
                    break;
                }
            }
            return;
        }
        std::uint32_t cosmetic_bursts = 0;
        std::uint32_t character_animation_events = 0U;
        constexpr std::uint32_t maximum_character_animations_per_frame = 4U;
        for (const auto& event : session_->frame_events()) {
            switch (event.type) {
            case GameplayEventType::note_hit:
                focus_camera_for_owner(NoteOwner::player);
                push_timing_offset(static_cast<float>(event.offset_ms));
                if (scene_ != nullptr
                    && character_animation_events
                        < maximum_character_animations_per_frame
                    && event.note_index < chart_->notes.size()) {
                    const auto& note = chart_->notes[event.note_index];
                    if (!note_animation_suppressed(note.kind)) {
                        notify_runtime_note_animation(
                            NoteOwner::player,
                            session_->display_lane(event.note_index),
                            event.song_time_ms,
                            note.kind,
                            event.rating == Rating::mine,
                            runtime_sustain_tail_ms(
                                note.kind, note.time_ms, note.duration_ms
                            )
                        );
                        ++character_animation_events;
                    }
                }
                if (event.note_index < chart_->notes.size()
                    && event.rating != Rating::mine) {
                    play_note_type_hitsound(chart_->notes[event.note_index].kind);
                }
                if (event.note_index < chart_->notes.size()
                    && event.rating != Rating::mine
                    && !options_.settings.visual.reduced_motion
                    && cosmetic_bursts
                        < options_.settings.performance
                              .max_cosmetic_bursts_per_frame) {
                    spawn_note_splash(event.note_index);
                    ++cosmetic_bursts;
                }
                break;
            case GameplayEventType::note_miss:
            case GameplayEventType::hold_drop:
                focus_camera_for_owner(NoteOwner::player);
                push_timing_offset(static_cast<float>(options_.settings.gameplay.windows.miss_ms));
                screen_flash_ = std::max(screen_flash_, 0.22F);
                if (scene_ != nullptr
                    && character_animation_events
                        < maximum_character_animations_per_frame
                    && event.note_index < chart_->notes.size()) {
                    const auto& note = chart_->notes[event.note_index];
                    const double tail_ms = runtime_sustain_tail_ms(
                        note.kind, note.time_ms, note.duration_ms
                    );
                    if (event.type == GameplayEventType::hold_drop) {
                        // PULSEFORGE_P1_4_0_CHARACTER_SUSTAIN_DROP_RELEASE_V1
                        release_runtime_sustain_animation(
                            NoteOwner::player,
                            event.song_time_ms,
                            note.kind,
                            tail_ms
                        );
                    }
                    if (!note_animation_suppressed(note.kind)) {
                        notify_runtime_note_animation(
                            NoteOwner::player,
                            session_->display_lane(event.note_index),
                            event.song_time_ms,
                            note.kind,
                            true,
                            -1.0
                        );
                        ++character_animation_events;
                    }
                }
                break;
            case GameplayEventType::opponent_hit:
                if (event.note_index < chart_->notes.size()) {
                    const auto& note = chart_->notes[event.note_index];
                    focus_camera_for_owner(note.owner);
                    if (scene_ != nullptr
                        && character_animation_events
                            < maximum_character_animations_per_frame) {
                        if (!note_animation_suppressed(note.kind)) {
                            notify_runtime_note_animation(
                                note.owner,
                                session_->display_lane(event.note_index),
                                event.song_time_ms,
                                note.kind,
                                false,
                                runtime_sustain_tail_ms(
                                    note.kind, note.time_ms, note.duration_ms
                                )
                            );
                            ++character_animation_events;
                        }
                    }
                    play_note_type_hitsound(note.kind);
                }
                break;
            case GameplayEventType::chart_event:
                if (event.chart_event_index < chart_->events.size()) {
                    const auto& chart_event =
                        chart_->events[event.chart_event_index];
                    handle_visual_event(
                        chart_event.name,
                        chart_event.value1,
                        chart_event.value2
                    );
                }
                break;
            case GameplayEventType::beat:
                if (!options_.settings.visual.reduced_motion) {
                    beat_pulse_ = 1.0F;
                }
                break;
            default:
                break;
            }
        }
    }

    void handle_visual_event(
        const std::string_view name,
        const std::string_view value1 = {},
        const std::string_view value2 = {}
    ) {
        bool scene_handled = false;
        if (scene_ != nullptr) {
            scene_handled = scene_->handle_visual_event(
                name,
                value1,
                value2,
                audio_.compensated_position_ms()
            );
        }
        // PULSEFORGE_P1_1_5_CHANGE_CHARACTER_DIAGNOSTICS_V1
        if (name == "Change Character" || name == "ChangeCharacter") {
            std::cout
                << "[Lua character] "
                << (scene_handled ? "changed " : "FAILED ")
                << (value1.empty() ? "0" : std::string(value1))
                << " -> " << value2 << '\n';
        }
        if ((name == "Camera Flash" || name == "cameraFlash")
            && options_.settings.visual.flashing_lights) {
            screen_flash_ = 1.0F;
        } else if (!options_.settings.visual.reduced_motion
            && (name == "Pulse" || name == "ScriptPulse" || name == "Finale"
                || name == "Add Camera Zoom" || name == "Screen Shake"
                || name == "Hey!" || name == "Play Animation"
                || name == "FocusCamera" || name == "ZoomCamera")) {
            beat_pulse_ = 1.5F;
        }
#if defined(PULSEFORGE_HAS_LUA)
        // PULSEFORGE_P1_5_0_ADD_CAMERA_ZOOM_EVENT_V1
        // Psych's stock Add Camera Zoom mutates both cameras. Keep the
        // existing pulse as presentation feedback, but do not substitute it
        // for the actual camera state. Values are deltas and remain bounded.
        if (name == "Add Camera Zoom") {
            const double game_delta = std::clamp(
                finite_number(value1).value_or(0.015), -1.0, 1.0
            );
            const double hud_delta = std::clamp(
                finite_number(value2).value_or(0.03), -1.0, 1.0
            );
            script_cam_game_zoom_ = std::clamp(
                script_cam_game_zoom_ + game_delta, 0.05, 8.0
            );
            script_cam_hud_zoom_ = std::clamp(
                script_cam_hud_zoom_ + hud_delta, 0.05, 8.0
            );
            if (scene_ != nullptr) {
                scene_->script_set_game_camera(
                    script_cam_game_x_, script_cam_game_y_,
                    script_cam_game_zoom_, script_cam_game_angle_,
                    script_cam_game_alpha_
                );
                scene_->script_set_hud_camera(
                    script_cam_hud_x_, script_cam_hud_y_,
                    script_cam_hud_zoom_, script_cam_hud_angle_,
                    script_cam_hud_alpha_
                );
            }
            streaming_visual_cache_.reset();
        }
#endif
        if (name == "Change P1 Mania" || name == "Change Player Mania"
            || name == "Change P2 Mania" || name == "Change Opponent Mania"
            || name == "Change Mania") {
            // Core gameplay already applied the topology before visual dispatch.
            // Rebuild only bounded visual caches; never touch the source chart.
            streaming_visual_cache_.reset();
            frame_note_coverage_.reset();
        }
        if (name == "Change Scroll Speed") {
            const auto target = finite_number(value1);
            const auto duration = finite_number(value2).value_or(0.0);
            if (target.has_value() && *target > 0.0) {
                scroll_tween_from_ = gameplay_settings().scroll_speed;
                scroll_tween_to_ = std::clamp(*target, 0.1, 10.0);
                scroll_tween_duration_ = std::clamp(duration, 0.0, 3'600.0);
                scroll_tween_elapsed_ = 0.0;
                if (scroll_tween_duration_ <= 0.0) {
                    gameplay_settings().scroll_speed = scroll_tween_to_;
                }
            }
        }
    }

    void push_timing_offset(const float offset) noexcept {
        timing_history_[timing_history_cursor_] = std::clamp(offset, -180.0F, 180.0F);
        timing_history_cursor_ = (timing_history_cursor_ + 1) % timing_history_size;
        timing_history_count_ =
            std::min(timing_history_count_ + 1, timing_history_size);
    }

    void spawn_note_splash(const std::size_t note_index) {
        if (!options_.settings.visual.note_splashes
            || options_.settings.visual.low_quality
            || options_.settings.visual.reduced_motion
            || chart_ == std::nullopt
            || note_index >= chart_->notes.size()) {
            return;
        }
        const auto& note = chart_->notes[note_index];
        const auto owner = runtime_owner(note.owner, note.kind);
        if (!script_note_splash_enabled(owner, note.kind)) {
            return;
        }
        const auto lane = session_->display_lane(note_index);
        spawn_note_splash_lane(
            lane,
            script_note_splash_profile(owner, note.kind)
        );
    }

    void spawn_note_splash_lane(
        const std::uint16_t lane,
        const detail::RuntimeNoteSplashProfile splash_profile =
            detail::runtime_note_splash_invalid_profile
    ) {
        if (!options_.settings.visual.note_splashes
            || options_.settings.visual.low_quality
            || options_.settings.visual.reduced_motion) {
            return;
        }
        const auto layout = lane_layout(NoteOwner::player);
#if defined(PULSEFORGE_HAS_LUA)
        const float lane_x = script_lane_x(
            NoteOwner::player, lane, layout.first, layout.second
        );
        const float lane_width = script_lane_width(layout.second);
        const float center_x = lane_x + lane_width * 0.5F;
        const float receptor_y = static_cast<float>(
            script_lane_receptor_y(NoteOwner::player, lane)
        );
#else
        const float center_x = layout.first
            + (static_cast<float>(lane) + 0.5F) * layout.second;
        const float receptor_y = gameplay_settings().downscroll
            ? 610.0F
            : 110.0F;
#endif
        if (scene_ != nullptr
            && splash_profile != detail::runtime_note_splash_invalid_profile
            && scene_->note_splash_frame_count(splash_profile) != 0U) {
            auto& animation = note_splash_animations_[
                note_splash_animation_cursor_++ % note_splash_animations_.size()
            ];
            animation = NoteSplashAnimation{
                splash_profile,
                center_x,
                receptor_y,
                0.0F,
                0.45F,
                true,
            };
            return;
        }
        const auto color = lane_color(lane);
        for (std::size_t count = 0; count < 8; ++count) {
            auto& particle = particles_[particle_cursor_++ % particles_.size()];
            particle = {
                center_x,
                receptor_y,
                particle_direction_x[count]
                    * (55.0F + static_cast<float>(count % 3) * 8.0F),
                particle_direction_y[count]
                    * (55.0F + static_cast<float>(count % 2) * 12.0F),
                0.36F,
                color,
                true,
            };
        }
    }

    void update_effects(const float elapsed) {
#if defined(PULSEFORGE_HAS_LUA)
        update_script_tweens(static_cast<double>(elapsed));
        update_script_timers(static_cast<double>(elapsed));
        // Tween/timer completion callbacks may queue triggerEvent/debugPrint.
        // Consume those outputs in the same frame; the next frame clears the
        // transient buffer before onUpdate, so deferring would silently lose them.
        if (scripts_ != nullptr) {
            consume_script_output();
        }
#endif
        if (scroll_tween_duration_ > 0.0) {
            scroll_tween_elapsed_ = std::min(
                scroll_tween_elapsed_ + static_cast<double>(elapsed),
                scroll_tween_duration_
            );
            const double ratio = scroll_tween_elapsed_ / scroll_tween_duration_;
            gameplay_settings().scroll_speed = std::lerp(
                scroll_tween_from_,
                scroll_tween_to_,
                ratio
            );
            if (scroll_tween_elapsed_ >= scroll_tween_duration_) {
                scroll_tween_duration_ = 0.0;
            }
        }
        screen_flash_ = std::max(0.0F, screen_flash_ - elapsed * 2.8F);
        beat_pulse_ = options_.settings.visual.reduced_motion
            ? 0.0F
            : std::max(0.0F, beat_pulse_ - elapsed * 3.5F);
        for (auto& particle : particles_) {
            if (!particle.active) {
                continue;
            }
            particle.life -= elapsed;
            if (particle.life <= 0.0F) {
                particle.active = false;
                continue;
            }
            particle.x += particle.velocity_x * elapsed;
            particle.y += particle.velocity_y * elapsed;
            particle.velocity_y += 140.0F * elapsed;
        }
        for (auto& splash : note_splash_animations_) {
            if (!splash.active) continue;
            splash.elapsed += elapsed;
            if (!std::isfinite(splash.elapsed)
                || splash.elapsed >= splash.duration) {
                splash.active = false;
            }
        }
    }

    [[nodiscard]] std::optional<std::filesystem::path>
    resolve_note_type_hitsound_asset(const std::string_view id) const {
        if (id.empty() || id.size() > 512U
            || id.find('\0') != std::string_view::npos) {
            return std::nullopt;
        }
        std::filesystem::path requested{std::string(id)};
        if (requested.empty() || requested.is_absolute()
            || requested.has_root_name() || requested.has_root_directory()) {
            return std::nullopt;
        }
        for (const auto& part : requested) {
            if (part == "." || part == "..") return std::nullopt;
        }

        std::vector<std::filesystem::path> names;
        if (requested.has_extension()) {
            auto extension = path_utf8(requested.extension());
            std::transform(
                extension.begin(), extension.end(), extension.begin(),
                [](const unsigned char value) {
                    return value >= 'A' && value <= 'Z'
                        ? static_cast<char>(value - 'A' + 'a')
                        : static_cast<char>(value);
                }
            );
            if (extension != ".ogg" && extension != ".wav"
                && extension != ".mp3" && extension != ".flac") {
                return std::nullopt;
            }
            names.push_back(requested);
        } else {
            for (const auto extension : {
                     std::string_view{".ogg"},
                     std::string_view{".wav"},
                     std::string_view{".mp3"},
                     std::string_view{".flac"},
                 }) {
                auto candidate = requested;
                candidate += extension;
                names.push_back(std::move(candidate));
            }
        }

        std::vector<std::filesystem::path> explicit_roots;
        explicit_roots.reserve(options_.content_roots.size() + 2U);
        const auto append_root = [&](const std::filesystem::path& root) {
            if (root.empty()) return;
            const auto normalized = root.lexically_normal();
            if (std::find(explicit_roots.begin(), explicit_roots.end(), normalized)
                == explicit_roots.end()) {
                explicit_roots.push_back(normalized);
            }
        };
        for (const auto& root : options_.content_roots) append_root(root);
        append_root(options_.selected_content_root);
        append_root(options_.selected_mod_root);
        const auto expanded = detail::resolve_psych_content_roots(
            explicit_roots, 64U, true
        );
        // PULSEFORGE_P1_5_0C2_MSVC_FILESYSTEM_PATH_CONSTINIT_FIX_V1
        // std::filesystem::path construction is not constexpr on the supported
        // MSVC standard library, so this local lookup table must be const only.
        const std::array directories{
            std::filesystem::path{"sounds"},
            std::filesystem::path{"assets/sounds"},
            std::filesystem::path{"shared/sounds"},
            std::filesystem::path{"assets/shared/sounds"},
        };
        for (auto root_it = expanded.roots.rbegin();
             root_it != expanded.roots.rend(); ++root_it) {
            const auto& root = *root_it;
            for (const auto& directory : directories) {
                for (const auto& name : names) {
                    const auto candidate = root / directory / name;
                    std::error_code error;
                    if (!std::filesystem::is_regular_file(candidate, error)
                        || error || !path_is_within(root, candidate)) {
                        continue;
                    }
                    auto canonical = std::filesystem::weakly_canonical(
                        candidate, error
                    );
                    if (!error) return canonical;
                }
            }
        }
        return std::nullopt;
    }

    void precache_note_type_hitsounds() {
        for (const auto& [kind, path] : note_type_hitsounds_) {
            std::string error;
            if (!audio_.precache_sound(path, &error) && !error.empty()) {
                std::cerr << "Note type hitsound warning [" << kind << "]: "
                          << error << '\n';
            }
        }
    }

    void play_note_type_hitsound(const std::string_view kind) {
        const auto found = note_type_hitsounds_.find(kind);
        if (found == note_type_hitsounds_.end()) return;
        std::string error;
        if (!audio_.play_sound(found->second, 1.0F, {}, false, &error)
            && !error.empty()) {
            std::cerr << "Note type hitsound warning [" << kind << "]: "
                      << error << '\n';
        }
    }

    void initialize_note_type_runtime() {
        note_type_registry_ = NoteTypeRegistry{};
        materialized_note_speed_multipliers_.clear();
        streaming_kind_speed_multipliers_.clear();
        minimum_note_scroll_multiplier_ = 1.0;
        note_scroll_multiplier_active_ = false;
        note_sustain_disable_policy_active_ = false;
        for (auto& states : script_note_visual_overrides_) {
            states.clear();
        }
        script_note_visual_override_count_ = 0U;
        for (auto& splash : note_splash_animations_) {
            splash.active = false;
        }
        note_splash_animation_cursor_ = 0U;
        note_type_hitsounds_.clear();

        std::vector<std::string> kinds;
        if (streaming_mode() && streaming_reader_.has_value()) {
            const auto packed_kinds = streaming_reader_->kinds();
            kinds.assign(packed_kinds.begin(), packed_kinds.end());
        } else if (chart_.has_value()) {
            kinds.reserve(std::min<std::size_t>(chart_->notes.size(), 4'096U));
            for (const auto& note : chart_->notes) {
                kinds.push_back(note.kind);
            }
        }
        std::sort(kinds.begin(), kinds.end());
        kinds.erase(std::unique(kinds.begin(), kinds.end()), kinds.end());

        // PULSEFORGE_P1_5_0B_NOTE_DEFINITION_EXECUTABLE_ROOT_POLICY_V1
        // Declarative note definitions alter gameplay and therefore follow the
        // executable-script trust boundary: selected distribution layers only,
        // never the sibling stock provider. The resolver is LOW -> HIGH and
        // replace_custom lets the active/high-priority layer win deterministically.
        std::vector<std::filesystem::path> explicit_definition_roots;
        explicit_definition_roots.reserve(options_.content_roots.size() + 2U);
        const auto append_definition_root = [&](
            const std::filesystem::path& root
        ) {
            if (root.empty()) return;
            const auto normalized = root.lexically_normal();
            if (std::find(
                    explicit_definition_roots.begin(),
                    explicit_definition_roots.end(),
                    normalized
                ) == explicit_definition_roots.end()) {
                explicit_definition_roots.push_back(normalized);
            }
        };
        for (const auto& root : options_.content_roots) {
            append_definition_root(root);
        }
        append_definition_root(options_.selected_content_root);
        append_definition_root(options_.selected_mod_root);
        const auto expanded_definition_roots = detail::resolve_psych_content_roots(
            explicit_definition_roots, 64U, false
        );
        const auto& roots = expanded_definition_roots.roots;

        const auto safe_id = [](const std::string_view id) noexcept {
            return !id.empty() && id.size() <= 128U
                && id.find('/') == std::string_view::npos
                && id.find('\\') == std::string_view::npos
                && id.find(':') == std::string_view::npos
                && id != "." && id != "..";
        };
        for (const auto& kind : kinds) {
            if (!safe_id(kind)) continue;
            for (const auto& root : roots) {
                constexpr std::array<std::string_view, 3U> extensions{
                    ".txt", ".note", ".properties"
                };
                for (const auto extension : extensions) {
                    const auto path = root / "custom_notetypes"
                        / (kind + std::string(extension));
                    std::error_code error;
                    if (!std::filesystem::is_regular_file(path, error) || error) {
                        continue;
                    }
                    std::string read_error;
                    const auto source = read_text_file(path, &read_error);
                    if (!read_error.empty()) {
                        std::cerr << "Note type definition warning: "
                                  << read_error << '\n';
                        continue;
                    }
                    std::vector<NoteTypeParseDiagnostic> diagnostics;
                    std::string register_error;
                    if (!note_type_registry_.register_text(
                            kind,
                            source,
                            NoteTypeReplacePolicy::replace_custom,
                            &diagnostics,
                            &register_error
                        )) {
                        std::cerr << "Note type definition warning: "
                                  << path_utf8(path) << ": "
                                  << register_error << '\n';
                    }
                    for (const auto& diagnostic : diagnostics) {
                        std::cerr << "Note type definition "
                                  << path_utf8(path) << ':' << diagnostic.line
                                  << ": " << diagnostic.property << ": "
                                  << diagnostic.message << '\n';
                    }
                    break;
                }
            }
            const auto resolved = note_type_registry_.resolve(kind);
            const auto multiplier = static_cast<double>(
                resolved.behavior().scroll_multiplier
            );
            if (std::isfinite(multiplier) && multiplier > 0.0) {
                minimum_note_scroll_multiplier_ = std::min(
                    minimum_note_scroll_multiplier_, multiplier
                );
                if (std::abs(multiplier - 1.0) > 1.0e-6) {
                    note_scroll_multiplier_active_ = true;
                }
            }

            // PULSEFORGE_P1_5_0_NOTE_TYPE_DEFINITION_GAMEPLAY_BRIDGE_V1
            // Safe .txt/.note/.properties definitions are type-wide. Install
            // their gameplay fields once per kind so multi-million/PFC1 charts
            // never allocate one override per physical note.
            // PULSEFORGE_P1_5_0D_BUILTIN_NOTE_TYPE_RUNTIME_BRIDGE_V1
            // Built-in animation semantics are read directly from the registry.
            // Hurt Note additionally carries real typed gameplay + visual data,
            // so bridge that definition too instead of falling back to the old
            // generic red-mine constants. Normal/animation-only built-ins keep
            // the user's gameplay health settings untouched.
            if (!resolved.used_fallback) {
                const auto& definition = resolved.behavior();
                const bool custom_definition = !definition.builtin;
                const bool builtin_hurt = definition.builtin
                    && builtin_note_type_causes_miss(kind);
                if (custom_definition || builtin_hurt) {
                    NoteKindRuntimeBehavior runtime_behavior;
                    if (custom_definition) {
                        runtime_behavior.hit_health = definition.health.hit;
                    }
                    runtime_behavior.miss_health = definition.health.miss;
                    runtime_behavior.hit_causes_miss =
                        definition.health.hit_causes_miss;
                    runtime_behavior.sustain_miss_health =
                        definition.sustain.miss_health;
                    runtime_behavior.sustain_hit_causes_miss =
                        definition.sustain.hit_causes_miss;
                    runtime_behavior.sustain_enabled =
                        definition.sustain.enabled;
                    runtime_behavior.sustain_inherits_type =
                        definition.sustain.inherits_type;
                    note_sustain_disable_policy_active_ =
                        note_sustain_disable_policy_active_
                        || !definition.sustain.enabled;
                    const bool installed = streaming_mode()
                        ? streaming_session_->set_note_kind_behavior(
                            kind, runtime_behavior
                        )
                        : session_->set_note_kind_behavior(kind, runtime_behavior);
                    if (!installed) {
                        std::cerr << "Note type runtime warning: cannot install "
                                  << kind << " behavior\n";
                    }
                }

                // PULSEFORGE_P1_5_0B_NOTE_TYPE_VISUAL_BRIDGE_V1
                // This is intentionally safe for all built-ins: animation-only
                // definitions have no visual state and therefore allocate none.
                seed_note_type_visual_state(kind, definition);
                if (definition.feedback.hitsound_enabled
                    && !definition.feedback.hitsound_id.empty()) {
                    if (const auto path = resolve_note_type_hitsound_asset(
                            definition.feedback.hitsound_id
                        ); path.has_value()) {
                        note_type_hitsounds_[kind] = *path;
                    } else {
                        std::cerr << "Note type hitsound warning: asset '"
                                  << definition.feedback.hitsound_id
                                  << "' was not found for " << kind << '\n';
                    }
                }
            }
        }

#if defined(PULSEFORGE_HAS_LUA)
        if (!streaming_mode() && chart_.has_value()) {
            materialized_note_speed_multipliers_.assign(
                chart_->notes.size(),
                1.0F
            );
        }
        script_strums_.assign(
            static_cast<std::size_t>(chart_->key_count)
                * (secondary_strum_enabled() ? 3U : 2U),
            ScriptStrumState{}
        );
        // PULSEFORGE_P1_5_0B_STREAMING_UNSPAWN_OWNER_PROTOTYPES_V1
        // A streaming Lua view exposes at most two ordinary owner prototypes
        // per note type (opponent/player) and one secondary-opponent prototype
        // for Third Strum kinds. This fixes mustPress/strum semantics without
        // materializing the physical chart.
        script_streaming_unspawn_prototypes_.clear();
        if (streaming_mode() && streaming_reader_.has_value()) {
            const auto packed_kinds = streaming_reader_->kinds();
            const auto ordinary_prototype_capacity = std::min<std::size_t>(
                packed_kinds.size(), maximum_script_unspawn_prototypes / 2U
            ) * 2U;
            script_streaming_unspawn_prototypes_.reserve(
                ordinary_prototype_capacity
            );
            for (std::size_t kind_id = 0U;
                 kind_id < packed_kinds.size();
                 ++kind_id) {
                const auto append = [&](const NoteOwner owner) {
                    if (script_streaming_unspawn_prototypes_.size()
                        < maximum_script_unspawn_prototypes) {
                        script_streaming_unspawn_prototypes_.push_back({
                            static_cast<std::uint32_t>(kind_id), owner
                        });
                    }
                };
                if (third_strum_kind(packed_kinds[kind_id])) {
                    append(NoteOwner::secondary_opponent);
                } else {
                    append(NoteOwner::opponent);
                    append(NoteOwner::player);
                }
            }
            if (packed_kinds.size()
                > maximum_script_unspawn_prototypes / 2U) {
                std::cerr
                    << "Lua unspawnNotes warning: owner/type prototype budget "
                       "was truncated at "
                    << maximum_script_unspawn_prototypes << '\n';
            }
        }
        script_tweens_.clear();
        script_timers_.clear();
        script_property_store_.clear();
        script_note_no_animation_.clear();
        script_hud_objects_.clear();
        script_hud_objects_.emplace("healthBarBG", ScriptHudObjectState{250.0, 18.0});
        script_hud_objects_.emplace("healthBar", ScriptHudObjectState{252.0, 20.0});
        script_hud_objects_.emplace("scoreTxt", ScriptHudObjectState{372.0, 44.0});
        script_hud_objects_.emplace("timeBarBG", ScriptHudObjectState{310.0, 66.0});
        script_hud_objects_.emplace("timeBar", ScriptHudObjectState{310.0, 66.0});
        script_hud_objects_.emplace("timeTxt", ScriptHudObjectState{600.0, 78.0});
        script_hud_objects_.emplace("songName", ScriptHudObjectState{16.0, 16.0});
        // PULSEFORGE_P1_1_19_AUTOMATIC_CAMERA_TURN_RESET_V1
        script_auto_camera_turn_.reset();
        script_auto_camera_owner_.reset();
        script_cam_game_x_ = 0.0;
        script_cam_game_y_ = 0.0;
        script_cam_game_zoom_ = 1.0;
        script_cam_game_angle_ = 0.0;
        script_cam_game_alpha_ = 1.0;
        script_cam_hud_x_ = 0.0;
        script_cam_hud_y_ = 0.0;
        script_cam_hud_zoom_ = 1.0;
        script_cam_hud_angle_ = 0.0;
        script_cam_hud_alpha_ = 1.0;
        script_health_opponent_color_ = SDL_Color{10U, 12U, 24U, 255U};
        script_health_player_color_ = SDL_Color{70U, 240U, 156U, 255U};
        script_time_background_color_ = SDL_Color{20U, 24U, 42U, 255U};
        script_time_fill_color_ = SDL_Color{90U, 210U, 255U, 255U};
#endif
    }

    [[nodiscard]] const NoteKindRuntimeBehavior* runtime_note_behavior(
        const std::string_view kind
    ) const noexcept {
        if (streaming_mode()) {
            return streaming_session_ != nullptr
                ? streaming_session_->note_kind_behavior(kind)
                : nullptr;
        }
        return session_ != nullptr ? session_->note_kind_behavior(kind) : nullptr;
    }

    [[nodiscard]] bool set_runtime_note_behavior(
        const std::string_view kind,
        const NoteKindRuntimeBehavior& behavior
    ) {
        return streaming_mode()
            ? streaming_session_ != nullptr
                && streaming_session_->set_note_kind_behavior(kind, behavior)
            : session_ != nullptr
                && session_->set_note_kind_behavior(kind, behavior);
    }

    [[nodiscard]] bool note_animation_suppressed(
        const std::string_view kind
    ) const noexcept {
#if defined(PULSEFORGE_HAS_LUA)
        if (const auto found = script_note_no_animation_.find(kind);
            found != script_note_no_animation_.end() && found->second) {
            return true;
        }
#endif
        const auto* definition = note_type_registry_.find(kind);
        return definition != nullptr
            && definition->animation.cue == NoteAnimationCue::none;
    }


    // PULSEFORGE_P1_5_0C_DECLARATIVE_NOTE_RUNTIME_HELPERS_V1
    [[nodiscard]] bool runtime_sustain_enabled(
        const std::string_view kind,
        const double source_duration_ms
    ) const noexcept {
        if (!std::isfinite(source_duration_ms) || source_duration_ms <= 0.0) {
            return false;
        }
        const auto* behavior = runtime_note_behavior(kind);
        return behavior == nullptr || behavior->sustain_enabled.value_or(true);
    }

    [[nodiscard]] double runtime_visual_duration_ms(
        const std::string_view kind,
        const double source_duration_ms
    ) const noexcept {
        return runtime_sustain_enabled(kind, source_duration_ms)
            ? source_duration_ms
            : 0.0;
    }

    [[nodiscard]] bool runtime_sustain_inherits_type(
        const std::string_view kind
    ) const noexcept {
        const auto* behavior = runtime_note_behavior(kind);
        return behavior == nullptr
            || behavior->sustain_inherits_type.value_or(true);
    }

    [[nodiscard]] double runtime_sustain_tail_ms(
        const std::string_view kind,
        const double head_time_ms,
        const double source_duration_ms
    ) const noexcept {
        if (!runtime_sustain_enabled(kind, source_duration_ms)
            || !std::isfinite(head_time_ms)) {
            return -1.0;
        }
        const long double tail = static_cast<long double>(head_time_ms)
            + static_cast<long double>(source_duration_ms);
        return static_cast<double>(std::clamp(
            tail,
            -static_cast<long double>(std::numeric_limits<double>::max()),
            static_cast<long double>(std::numeric_limits<double>::max())
        ));
    }

    void notify_runtime_note_animation(
        const NoteOwner owner,
        const std::uint16_t lane,
        const double song_time_ms,
        const std::string_view kind,
        const bool missed,
        const double sustain_tail_ms
    ) noexcept {
        if (scene_ == nullptr) return;
        if (const auto* definition = note_type_registry_.find(kind);
            definition != nullptr) {
            scene_->notify_note_animation_configured(
                owner,
                lane,
                song_time_ms,
                definition->animation.target,
                definition->animation.cue,
                definition->animation.suffix,
                missed,
                definition->sustain.inherits_type ? sustain_tail_ms : -1.0
            );
            return;
        }
        scene_->notify_note_animation(
            owner, lane, song_time_ms, kind, missed, sustain_tail_ms
        );
    }

    void release_runtime_sustain_animation(
        const NoteOwner owner,
        const double song_time_ms,
        const std::string_view kind,
        const double sustain_tail_ms
    ) noexcept {
        if (scene_ == nullptr) return;
        if (const auto* definition = note_type_registry_.find(kind);
            definition != nullptr) {
            scene_->release_sustain_animation_configured(
                owner,
                song_time_ms,
                definition->animation.target,
                sustain_tail_ms
            );
            return;
        }
        scene_->release_sustain_animation(
            owner, song_time_ms, kind, sustain_tail_ms
        );
    }

    [[nodiscard]] static constexpr std::size_t script_note_visual_owner_slot(
        const NoteOwner owner
    ) noexcept {
        switch (owner) {
        case NoteOwner::opponent:
            return 0U;
        case NoteOwner::player:
            return 1U;
        case NoteOwner::secondary_opponent:
            return 2U;
        }
        return 0U;
    }

    [[nodiscard]] const ScriptNoteVisualState* script_note_visual_state(
        const NoteOwner owner,
        const std::string_view kind
    ) const noexcept {
        const auto& states = script_note_visual_overrides_[
            script_note_visual_owner_slot(owner)
        ];
        const auto found = states.find(kind);
        return found == states.end() ? nullptr : std::addressof(found->second);
    }

    [[nodiscard]] ScriptNoteVisualState* ensure_script_note_visual_state(
        const NoteOwner owner,
        const std::string_view kind
    ) {
        if (kind.empty() || kind.size() > 128U) {
            return nullptr;
        }
        auto& states = script_note_visual_overrides_[
            script_note_visual_owner_slot(owner)
        ];
        if (const auto found = states.find(kind); found != states.end()) {
            return std::addressof(found->second);
        }
        if (script_note_visual_override_count_
            >= maximum_script_note_visual_overrides) {
            return nullptr;
        }
        const auto [inserted, created] = states.emplace(
            std::string(kind), ScriptNoteVisualState{}
        );
        if (created) {
            ++script_note_visual_override_count_;
        }
        return std::addressof(inserted->second);
    }

    void seed_note_type_visual_state(
        const std::string_view kind,
        const NoteTypeDefinition& definition
    ) {
        // Custom declarative note definitions are type-wide. Mirror that
        // definition to each owner without allocating per physical note.
        const bool has_visual_state = !definition.visual.texture_id.empty()
            || definition.visual.rgb.has_value()
            || std::abs(static_cast<double>(definition.visual.alpha) - 1.0)
                > 1.0e-6
            || std::abs(static_cast<double>(definition.visual.scale) - 1.0)
                > 1.0e-6
            || !definition.feedback.splash_enabled
            || !definition.feedback.splash_id.empty();
        if (!has_visual_state) {
            return;
        }
        constexpr std::array owners{
            NoteOwner::opponent,
            NoteOwner::player,
            NoteOwner::secondary_opponent,
        };
        for (const auto owner : owners) {
            auto* state = ensure_script_note_visual_state(owner, kind);
            if (state == nullptr) {
                continue;
            }
            state->texture = definition.visual.texture_id;
            state->texture_profile.reset();
            state->rgb = definition.visual.rgb;
            state->alpha = std::clamp(
                static_cast<double>(definition.visual.alpha), 0.0, 1.0
            );
            state->scale = std::clamp(
                static_cast<double>(definition.visual.scale), 0.05, 8.0
            );
            state->splash_disabled = !definition.feedback.splash_enabled;
            state->splash_texture = definition.feedback.splash_id;
            state->splash_profile.reset();
        }
    }

    void resolve_script_note_visual_profiles() {
        if (scene_ == nullptr) {
            return;
        }
        for (auto& owner_states : script_note_visual_overrides_) {
            for (auto& [kind, state] : owner_states) {
                static_cast<void>(kind);
                if (!state.texture.empty() && !state.texture_profile.has_value()) {
                    state.texture_profile = scene_->resolve_note_skin_profile(
                        state.texture, false
                    );
                }
                if (!state.splash_texture.empty()
                    && !state.splash_profile.has_value()) {
                    state.splash_profile = scene_->resolve_note_splash_profile(
                        state.splash_texture
                    );
                }
            }
        }
    }

    [[nodiscard]] double script_note_visual_alpha(
        const NoteOwner owner,
        const std::string_view kind
    ) const noexcept {
        const auto* state = script_note_visual_state(owner, kind);
        return state != nullptr && state->alpha.has_value()
            ? std::clamp(*state->alpha, 0.0, 1.0)
            : 1.0;
    }

    [[nodiscard]] std::array<std::uint8_t, 3U> script_note_visual_rgb(
        const NoteOwner owner,
        const std::string_view kind
    ) const noexcept {
        const auto* state = script_note_visual_state(owner, kind);
        return state != nullptr && state->rgb.has_value()
            ? *state->rgb
            : std::array<std::uint8_t, 3U>{255U, 255U, 255U};
    }

    [[nodiscard]] double script_note_visual_scale(
        const NoteOwner owner,
        const std::string_view kind
    ) const noexcept {
        const auto* state = script_note_visual_state(owner, kind);
        return state != nullptr && state->scale.has_value()
            ? std::clamp(*state->scale, 0.05, 8.0)
            : 1.0;
    }

    [[nodiscard]] bool script_note_splash_enabled(
        const NoteOwner owner,
        const std::string_view kind
    ) const noexcept {
        const auto* state = script_note_visual_state(owner, kind);
        return state == nullptr
            || !state->splash_disabled.value_or(false);
    }

    [[nodiscard]] detail::RuntimeNoteSplashProfile script_note_splash_profile(
        const NoteOwner owner,
        const std::string_view kind
    ) const noexcept {
        const auto* state = script_note_visual_state(owner, kind);
        return state != nullptr && state->splash_profile.has_value()
            ? *state->splash_profile
            : detail::runtime_note_splash_invalid_profile;
    }

    [[nodiscard]] detail::RuntimeNoteSkinProfile script_note_skin_profile(
        const NoteOwner owner,
        const std::string_view kind,
        bool& custom_texture_missing
    ) const noexcept {
        custom_texture_missing = false;
        const auto* state = script_note_visual_state(owner, kind);
        if (state == nullptr || state->texture.empty()) {
            return detail::runtime_note_skin_default_profile;
        }
        if (!state->texture_profile.has_value()) {
            custom_texture_missing = true;
            return detail::runtime_note_skin_default_profile;
        }
        return *state->texture_profile;
    }

    [[nodiscard]] double note_scroll_multiplier(
        const std::string_view kind,
        const std::optional<std::size_t> materialized_index = std::nullopt
    ) const noexcept {
        double multiplier = 1.0;
        // NoteTypeRegistry::find() performs transparent std::map lookup and is
        // allocation-free. resolve() intentionally preserves the chart spelling
        // in an owning std::string, which is useful for serialization but was
        // far too expensive in the per-visible-note render hot path.
        if (const auto* definition = note_type_registry_.find(kind);
            definition != nullptr) {
            multiplier *= static_cast<double>(definition->scroll_multiplier);
        }
        if (materialized_index.has_value()
            && *materialized_index < materialized_note_speed_multipliers_.size()) {
            multiplier *= materialized_note_speed_multipliers_[*materialized_index];
        }
        if (const auto found = streaming_kind_speed_multipliers_.find(kind);
            found != streaming_kind_speed_multipliers_.end()) {
            multiplier *= found->second;
        }
        return std::isfinite(multiplier) && multiplier > 0.0
            ? std::clamp(multiplier, 0.01, 100.0)
            : 1.0;
    }

    [[nodiscard]] double minimum_visual_speed(
        const double base_speed
    ) const noexcept {
        const double factor = std::isfinite(minimum_note_scroll_multiplier_)
                && minimum_note_scroll_multiplier_ > 0.0
            ? std::clamp(minimum_note_scroll_multiplier_, 0.01, 1.0)
            : 1.0;
        return std::max(base_speed * factor, 0.000001);
    }

    [[nodiscard]] std::pair<float, float> lane_layout(const NoteOwner owner) const {
        const float keys = static_cast<float>(std::max<std::uint16_t>(
            active_key_count(owner), 1U
        ));
        if (gameplay_settings().middle_scroll && owner == NoteOwner::player) {
            const float total_width = std::min(520.0F, keys * 78.0F);
            return {(logical_width - total_width) * 0.5F, total_width / keys};
        }
        if (secondary_strum_enabled()
            && !gameplay_settings().middle_scroll) {
            // Three independent clusters: primary opponent, player and player4.
            // Shrink only the visual cluster width; logical lanes/judgment are
            // unchanged and still support the full 1K..18K domain.
            const float total_width = std::min(360.0F, keys * 68.0F);
            const float lane_width = total_width / keys;
            if (owner == NoteOwner::opponent) return {24.0F, lane_width};
            if (owner == NoteOwner::secondary_opponent) {
                return {(logical_width - total_width) * 0.5F, lane_width};
            }
            return {logical_width - total_width - 24.0F, lane_width};
        }
        const float total_width = std::min(520.0F, keys * 78.0F);
        const float lane_width = total_width / keys;
        if (owner == NoteOwner::player) {
            return {logical_width - total_width - 72.0F, lane_width};
        }
        return {72.0F, lane_width};
    }

    void render(const double song_time) {
        const bool rendering_to_texture = offline_render_target_ != nullptr;
        if (rendering_to_texture
            && !SDL_SetRenderTarget(renderer_, offline_render_target_)) {
            offline_frame_error_ = std::string{
                "Unable to select the exact offline frame target: "
            } + SDL_GetError();
            return;
        }
        post_effect_message_.clear();
        if (!post_effects_.begin_frame(
                renderer_,
                options_.settings.visual.post_effect,
                options_.settings.performance.maximum_performance_mode,
                logical_width,
                logical_height,
                &post_effect_message_
            )) {
            platform_return_safe_ = false;
            if (offline_encoder_ != nullptr) {
                offline_frame_error_ = post_effect_message_.empty()
                    ? "Post effect could not recover its frame destination"
                    : post_effect_message_;
            } else if (!post_effect_warning_reported_) {
                std::cerr << "Post effect error: " << post_effect_message_ << '\n';
                post_effect_warning_reported_ = true;
            }
            if (offline_encoder_ == nullptr) {
                runtime_fatal_error_ = true;
                running_ = false;
            }
            return;
        }
        if (!post_effect_message_.empty() && !post_effect_warning_reported_) {
            std::cerr << "Post effect warning: " << post_effect_message_ << '\n';
            post_effect_warning_reported_ = true;
        }
        render_background(song_time);
        if (scene_ != nullptr) {
            scene_->render(
                logical_width,
                logical_height,
                gameplay_timing().beat_at(song_time),
                song_time
            );
        }
        render_lanes_and_notes(song_time);
        render_particles();
        render_hud(song_time);
        render_audio_visualizer(song_time);
        if (paused_) {
            render_pause_menu();
        } else if (result_shown_) {
            const auto clear = streaming_mode()
                    && !streaming_session_->healthy()
                ? std::string{"STREAM ERROR"}
                : std::string(gameplay_summary().clear_type());
            render_center_panel(
                clear,
                options_.return_to_launcher
                    ? (options_.campaign_mode
                        ? "R retry - Enter continues Story - Esc opens pause"
                        : "R retry - Enter returns - Esc opens pause")
                    : "R to retry - Esc to exit"
            );
        }
        if (screen_flash_ > 0.0F && options_.settings.visual.flashing_lights) {
            const auto alpha = static_cast<std::uint8_t>(
                std::clamp(screen_flash_, 0.0F, 1.0F) * 180.0F
            );
            fill_rect(
                renderer_,
                {0.0F, 0.0F, logical_width, logical_height},
                {255, 255, 255, alpha}
            );
        }
        post_effect_message_.clear();
        if (!post_effects_.finish_frame(&post_effect_message_)) {
            // Restoration failure leaves the staging target selected. It is
            // not safe to destroy that texture or capture/present this frame.
            platform_return_safe_ = false;
            if (offline_encoder_ != nullptr && offline_frame_error_.empty()) {
                offline_frame_error_ = post_effect_message_.empty()
                    ? "Post effect could not restore the frame destination"
                    : post_effect_message_;
            }
            if (!post_effect_warning_reported_) {
                std::cerr << "Post effect fatal renderer error: "
                          << post_effect_message_ << '\n';
                post_effect_warning_reported_ = true;
            }
            if (offline_encoder_ == nullptr) {
                runtime_fatal_error_ = true;
                running_ = false;
            }
            return;
        }
        if (!post_effect_message_.empty() && !post_effect_warning_reported_) {
            std::cerr << "Post effect warning: " << post_effect_message_ << '\n';
            post_effect_warning_reported_ = true;
        }
        if (offline_encoder_ != nullptr
            && !offline_encoder_->write_frame(
                renderer_,
                &offline_frame_error_
            )) {
            // run_offline_render reports the detailed error and tears down the
            // child process immediately after this draw call.
        }
        if (rendering_to_texture) {
            if (!SDL_SetRenderTarget(renderer_, nullptr)) {
                const std::string restore_error = std::string{
                    "Renderer could not restore the progress backbuffer: "
                } + SDL_GetError();
                if (offline_frame_error_.empty()) {
                    offline_frame_error_ = restore_error;
                } else {
                    offline_frame_error_ += " | " + restore_error;
                }
                // Returning a renderer with an indeterminate active target to
                // MenuSession is unsafe. shutdown() will dispose the platform
                // and let the launcher construct a clean replacement.
                platform_return_safe_ = false;
            }
            return;
        }
        if (diagnostics_) {
            const auto present_started_ns = SDL_GetTicksNS();
            detail::present_with_mobile_touch(renderer_);
            note_profile_present_.sample(
                SDL_GetTicksNS() - present_started_ns
            );
        } else {
            detail::present_with_mobile_touch(renderer_);
        }
    }

    void render_background(const double song_time) {
        if (options_.settings.performance.maximum_performance_mode) {
            fill_rect(
                renderer_,
                {0.0F, 0.0F, logical_width, logical_height},
                {0, 0, 0, 255}
            );
            return;
        }
        const bool reduced_motion = options_.settings.visual.reduced_motion;
        const double beat = reduced_motion
            ? 0.0
            : gameplay_timing().beat_at(song_time);
        const float wave = reduced_motion
            ? 0.5F
            : static_cast<float>(
                (std::sin(beat * 0.785398163) + 1.0) * 0.5
            );
        const float pulse = reduced_motion
            ? 0.0F
            : std::clamp(beat_pulse_, 0.0F, 1.0F);
        for (int strip = 0; strip < 36; ++strip) {
            const float ratio = static_cast<float>(strip) / 35.0F;
            const auto red = static_cast<std::uint8_t>(
                12.0F + ratio * 18.0F + wave * 12.0F + pulse * 12.0F
            );
            const auto green = static_cast<std::uint8_t>(
                10.0F + ratio * 8.0F + pulse * 8.0F
            );
            const auto blue = static_cast<std::uint8_t>(
                26.0F + ratio * 36.0F + wave * 18.0F + pulse * 22.0F
            );
            fill_rect(
                renderer_,
                {
                    0.0F,
                    static_cast<float>(strip) * logical_height / 36.0F,
                    logical_width,
                    logical_height / 36.0F + 1.0F,
                },
                {red, green, blue, 255}
            );
        }

        if (!options_.settings.visual.low_quality) {
            set_draw_color(renderer_, {74, 50, 140, 55});
            const float offset = reduced_motion
                ? 0.0F
                : std::fmod(static_cast<float>(song_time * 0.025), 48.0F);
            for (float x = -logical_height; x < logical_width; x += 48.0F) {
                SDL_RenderLine(
                    renderer_,
                    x + offset,
                    logical_height,
                    x + logical_height * 0.55F + offset,
                    0.0F
                );
            }
        }
        const auto dim_alpha = static_cast<std::uint8_t>(
            std::clamp(options_.settings.visual.background_dim, 0.0F, 1.0F)
            * 180.0F
        );
        fill_rect(
            renderer_,
            {0.0F, 0.0F, logical_width, logical_height},
            {0, 0, 0, dim_alpha}
        );
    }

    [[nodiscard]] double dense_note_row_height(
        const std::size_t /*candidate_count*/
    ) const noexcept {
        // Keep the dense raster resolution stable for an entire quality mode.
        // The old density-tier switching (2/4/6/8 px) could cross a threshold
        // from one frame to the next and rebuild/re-bin the complete grid,
        // producing periodic visible blinking on textured custom note skins.
        // Dense mode is already entered only after the configured exact-note
        // threshold, so a fixed coarse grid is both cheaper and temporally
        // stable.
        if (options_.settings.performance.maximum_performance_mode) {
            return 8.0;
        }
        if (options_.settings.visual.low_quality) {
            return 6.0;
        }
        return 4.0;
    }

    [[nodiscard]] double streaming_visual_cache_margin_pixels() const noexcept {
        // Large rolling margins make expensive PVD/PFC visual-cache rebuilds
        // rare. The cache is screen-space bounded, so this does not scale with
        // total chart length or note count.
        if (options_.settings.performance.maximum_performance_mode) {
            return 4'096.0;
        }
        if (options_.settings.visual.low_quality) {
            return 3'072.0;
        }
        return 2'048.0;
    }

    [[nodiscard]] double exact_head_quantum_pixels() const noexcept {
        // Visual-only coincident-note suppression, analogous to the showcase's
        // one-draw-per-lane/per-screen-row fast path. Gameplay/judgment still
        // sees every logical note.
        if (options_.settings.performance.maximum_performance_mode) {
            return 4.0;
        }
        if (options_.settings.visual.low_quality) {
            return 2.0;
        }
        return 1.0;
    }

    [[nodiscard]] DenseNoteCoverage& frame_note_coverage(
        const std::size_t candidate_count
    ) {
        const double row_height = dense_note_row_height(candidate_count);
        if (frame_note_coverage_ == nullptr
            || frame_note_coverage_->lane_count() != chart_->key_count
            || std::abs(frame_note_coverage_->row_height() - row_height)
                > 0.000001) {
            frame_note_coverage_ = std::make_unique<DenseNoteCoverage>(
                chart_->key_count,
                logical_height,
                row_height
            );
        } else {
            frame_note_coverage_->reset();
        }
        return *frame_note_coverage_;
    }

    void render_lanes_and_notes(const double song_time) {
        const auto gameplay_update_ns =
            note_profile_frame_.gameplay_update_ns;
        note_profile_frame_ = {};
        note_profile_frame_.gameplay_update_ns =
            gameplay_update_ns;
        if (scene_ != nullptr) {
            scene_->begin_note_skin_profile_frame(diagnostics_);
        }
        visual_draw_units_ = 0U;
        visual_geometry_calls_ = 0U;
        streaming_density_buckets_ = 0U;
        streaming_explicit_visual_visits_ = 0U;
        if (note_skin_draw_batch_.capacity() < 16'384U) {
            note_skin_draw_batch_.reserve(16'384U);
        }
        note_skin_draw_batch_.clear();
        render_lane_set(NoteOwner::opponent);
        if (secondary_strum_enabled()) {
            render_lane_set(NoteOwner::secondary_opponent);
        }
        render_lane_set(NoteOwner::player);

        const auto note_pipeline_started_ns = diagnostics_
            ? SDL_GetTicksNS()
            : std::uint64_t{0U};
        const double visual_time = song_time
            + gameplay_settings().visual_offset_ms;
        const double chart_speed =
            options_.settings.visual.scroll_speed_mode
                    == ScrollSpeedMode::multiplicative
                ? chart_->chart_scroll_speed
                : 1.0;
        const double speed = 0.43
            * chart_speed
            * gameplay_settings().scroll_speed;
        if (streaming_mode()) {
            render_streaming_notes(visual_time, speed);
            if (diagnostics_) {
                note_profile_frame_.note_pipeline_ns =
                    SDL_GetTicksNS() - note_pipeline_started_ns;
            }
            flush_runtime_note_skin_batch();
            sample_note_profile_frame();
            return;
        }
        const double future_window = visible_time_window_ms(
            static_cast<double>(logical_height) + 38.0,
            minimum_visual_speed(speed),
            500.0
        );

        const auto prefix_begin = std::lower_bound(
            note_prefix_end_ms_.begin(),
            note_prefix_end_ms_.end(),
            visual_time
        );
        const auto begin = chart_->notes.begin()
            + std::distance(note_prefix_end_ms_.begin(), prefix_begin);
        const auto end = std::upper_bound(
            begin,
            chart_->notes.end(),
            visual_time + future_window,
            [](const double time, const Note& note) {
                return time < note.time_ms;
            }
        );

        const auto candidate_count = static_cast<std::size_t>(std::distance(begin, end));
        auto& coverage = frame_note_coverage(candidate_count);
        for (auto iterator = begin; iterator != end; ++iterator) {
            const auto index = static_cast<std::size_t>(
                std::distance(chart_->notes.begin(), iterator)
            );
            if (!note_is_visible(session_->note_state(index))) {
                continue;
            }
            if (ai_note_owner(iterator->owner)
                && (gameplay_settings().hide_opponent_notes
                    || gameplay_settings().middle_scroll)) {
                continue;
            }
            const auto state = session_->note_state(index);
            const double note_speed = speed * note_scroll_multiplier(
                iterator->kind,
                index
            );
            const auto visual = visual_note_span(
                iterator->time_ms,
                runtime_visual_duration_ms(iterator->kind, iterator->duration_ms),
                visual_time,
                note_speed,
                gameplay_settings().downscroll,
                receptor_y_for_owner(iterator->owner),
                state != NoteState::pending
            );
            if (!visual.has_value()) {
                continue;
            }
            const bool hurt = hurt_note_kind(iterator->kind);
            static_cast<void>(coverage.add(
                iterator->owner,
                session_->display_lane(index),
                visual->span,
                hurt,
                visual->include_head
            ));
        }
        rendered_notes_ = coverage.represented_note_count();
        const auto exact_threshold = static_cast<std::uint64_t>(
            options_.settings.performance.max_visible_notes
        );
        if (rendered_notes_ > exact_threshold) {
            coverage.finalize();
            draw_dense_note_coverage(coverage);
            if (diagnostics_) {
                note_profile_frame_.note_pipeline_ns =
                    SDL_GetTicksNS() - note_pipeline_started_ns;
            }
            flush_runtime_note_skin_batch();
            sample_note_profile_frame();
            return;
        }
        const double head_quantum = exact_head_quantum_pixels();
        std::vector<std::int32_t> last_head_bins(
            static_cast<std::size_t>(chart_->key_count) * 3U,
            std::numeric_limits<std::int32_t>::min()
        );
        for (auto iterator = begin; iterator != end; ++iterator) {
            const auto index = static_cast<std::size_t>(
                std::distance(chart_->notes.begin(), iterator)
            );
            if (!note_is_visible(session_->note_state(index))) {
                continue;
            }
            if (ai_note_owner(iterator->owner)
                && (gameplay_settings().hide_opponent_notes
                    || gameplay_settings().middle_scroll)) {
                continue;
            }
            const auto state = session_->note_state(index);
            const double note_speed = speed * note_scroll_multiplier(
                iterator->kind,
                index
            );
            const auto visual = visual_note_span(
                iterator->time_ms,
                runtime_visual_duration_ms(iterator->kind, iterator->duration_ms),
                visual_time,
                note_speed,
                gameplay_settings().downscroll,
                receptor_y_for_owner(iterator->owner),
                state != NoteState::pending
            );
            if (visual.has_value()
                && note_intersects_viewport(
                    visual->span,
                    logical_height
                )) {
                auto optimized_visual = *visual;
                if (optimized_visual.include_head) {
                    const auto lane = session_->display_lane(index);
                    const auto owner_index = note_owner_slot(iterator->owner);
                    const auto slot = owner_index
                        * static_cast<std::size_t>(chart_->key_count)
                        + lane;
                    const auto bin64 = static_cast<std::int64_t>(std::floor(
                        optimized_visual.span.head_y / head_quantum
                    ));
                    const auto bin = static_cast<std::int32_t>(std::clamp(
                        bin64,
                        static_cast<std::int64_t>(
                            std::numeric_limits<std::int32_t>::min()
                        ),
                        static_cast<std::int64_t>(
                            std::numeric_limits<std::int32_t>::max()
                        )
                    ));
                    if (slot < last_head_bins.size()
                        && last_head_bins[slot] == bin) {
                        optimized_visual.include_head = false;
                    } else if (slot < last_head_bins.size()) {
                        last_head_bins[slot] = bin;
                    }
                }
                draw_note(index, optimized_visual);
            }
        }
        if (diagnostics_) {
            note_profile_frame_.note_pipeline_ns =
                SDL_GetTicksNS() - note_pipeline_started_ns;
        }
        flush_runtime_note_skin_batch();
        sample_note_profile_frame();
    }

    struct PackedVisualCoverageContext {
        DenseNoteCoverage* coverage{};
        const StreamingGameplaySession* session{};
        const DesktopApplication* application{};
        std::span<const std::string> kinds;
        double visual_time_ms{};
        double speed{};
        double receptor_y{};
        bool downscroll{};
        bool hide_opponent{};
    };

    static void add_packed_visual_note(
        void* const raw_context,
        const PackedNote& note
    ) noexcept {
        auto& context = *static_cast<PackedVisualCoverageContext*>(raw_context);
        const std::string_view kind = note.kind_id < context.kinds.size()
            ? std::string_view(context.kinds[note.kind_id])
            : std::string_view{"normal"};
        const auto owner = runtime_owner(note.owner, kind);
        if (ai_note_owner(owner) && context.hide_opponent) {
            return;
        }
        const double note_speed = context.speed
            * (context.application != nullptr
                ? context.application->note_scroll_multiplier(kind)
                : 1.0);
        const auto visual = visual_note_span(
            static_cast<double>(note.time_us) / 1'000.0,
            context.application != nullptr
                ? context.application->runtime_visual_duration_ms(
                    kind, static_cast<double>(note.duration_us) / 1'000.0
                )
                : static_cast<double>(note.duration_us) / 1'000.0,
            context.visual_time_ms,
            note_speed,
            context.downscroll,
            context.application != nullptr
                ? context.application->receptor_y_for_owner(owner)
                    + (context.receptor_y
                        - (context.downscroll ? 610.0 : 110.0))
                : context.receptor_y
        );
        if (!visual.has_value()) {
            return;
        }
        if (context.application != nullptr
            && note.lane >= context.application->active_key_count(owner)) {
            return;
        }
        const bool hurt = hurt_note_kind(kind);
        static_cast<void>(context.coverage->add(
            owner,
            context.session->display_lane(owner, note.lane),
            visual->span,
            hurt,
            visual->include_head
        ));
    }

    struct VisualDensitySustainCell final {
        std::int64_t last_time_us{};
        std::uint64_t active{};
    };

    struct VisualDensityCoverageContext final {
        DenseNoteCoverage* coverage{};
        const StreamingGameplaySession* session{};
        std::span<VisualDensitySustainCell> sustain_cells;
        std::uint16_t key_count{};
        std::uint16_t player_key_count{};
        std::uint16_t opponent_key_count{};
        std::int64_t first_time_us{};
        std::int64_t last_time_us{};
        double visual_time_ms{};
        double speed{};
        double receptor_y{};
        bool downscroll{};
        bool hide_opponent{};
        std::uint64_t buckets_visited{};
    };

    [[nodiscard]] static std::int64_t density_bucket_time(
        const std::int64_t bucket,
        const std::uint64_t width,
        const bool end
    ) noexcept {
        long double value = static_cast<long double>(bucket)
            * static_cast<long double>(width);
        if (end) {
            value += static_cast<long double>(width);
        }
        return static_cast<std::int64_t>(std::clamp(
            value,
            static_cast<long double>(std::numeric_limits<std::int64_t>::min()),
            static_cast<long double>(std::numeric_limits<std::int64_t>::max())
        ));
    }

    static void add_density_sustain_segment(
        VisualDensityCoverageContext& context,
        const PackedNoteOwner packed_owner,
        const std::uint16_t source_lane,
        const std::int64_t begin_us,
        const std::int64_t end_us,
        const std::uint64_t count
    ) noexcept {
        if (count == 0U || end_us <= begin_us) {
            return;
        }
        const auto owner = packed_owner == PackedNoteOwner::player
            ? NoteOwner::player
            : NoteOwner::opponent;
        if (owner == NoteOwner::opponent && context.hide_opponent) {
            return;
        }
        const auto active_keys = owner == NoteOwner::player
            ? context.player_key_count
            : context.opponent_key_count;
        if (source_lane >= active_keys) {
            return;
        }
        const auto visual = visual_note_span(
            static_cast<double>(begin_us) / 1'000.0,
            static_cast<double>(end_us - begin_us) / 1'000.0,
            context.visual_time_ms,
            context.speed,
            context.downscroll,
            context.receptor_y
        );
        if (visual.has_value()) {
            static_cast<void>(context.coverage->add_coincident(
                owner,
                context.session->display_lane(owner, source_lane),
                visual->span,
                count,
                false,
                false
            ));
        }
    }

    static void add_visual_density_bucket(
        void* const raw_context,
        const VisualDensityBucket& bucket
    ) noexcept {
        auto& context = *static_cast<VisualDensityCoverageContext*>(raw_context);
        if (bucket.lane >= context.key_count) {
            return;
        }
        const auto owner_index = bucket.owner == PackedNoteOwner::player
            ? std::size_t{1U}
            : std::size_t{0U};
        const auto active_keys = bucket.owner == PackedNoteOwner::player
            ? context.player_key_count
            : context.opponent_key_count;
        if (bucket.lane >= active_keys) {
            return;
        }
        const auto cell_index = owner_index
                * static_cast<std::size_t>(context.key_count)
            + bucket.lane;
        if (cell_index >= context.sustain_cells.size()) {
            return;
        }
        auto& sustain = context.sustain_cells[cell_index];
        const auto bucket_begin = std::clamp(
            density_bucket_time(bucket.bucket_index, bucket.bucket_width_us, false),
            context.first_time_us,
            context.last_time_us
        );
        const auto bucket_end = std::clamp(
            density_bucket_time(bucket.bucket_index, bucket.bucket_width_us, true),
            context.first_time_us,
            context.last_time_us
        );
        add_density_sustain_segment(
            context,
            bucket.owner,
            bucket.lane,
            sustain.last_time_us,
            bucket_begin,
            sustain.active
        );
        sustain.last_time_us = bucket_begin;
        sustain.active = bucket.active_sustains_at_bucket_start;
        sustain.active = bucket.sustain_starts
                > std::numeric_limits<std::uint64_t>::max() - sustain.active
            ? std::numeric_limits<std::uint64_t>::max()
            : sustain.active + bucket.sustain_starts;
        sustain.active = bucket.sustain_ends > sustain.active
            ? 0U
            : sustain.active - bucket.sustain_ends;

        const auto owner = bucket.owner == PackedNoteOwner::player
            ? NoteOwner::player
            : NoteOwner::opponent;
        if (!(owner == NoteOwner::opponent && context.hide_opponent)) {
            const auto center = bucket_begin
                + (bucket_end - bucket_begin) / 2;
            const auto head = dense_visual_note_span(
                static_cast<double>(center) / 1'000.0,
                0.0,
                context.visual_time_ms,
                context.speed,
                context.downscroll,
                context.receptor_y
            );
            if (head.has_value()) {
                if (bucket.normal_heads != 0U) {
                    static_cast<void>(context.coverage->add_coincident(
                        owner,
                        context.session->display_lane(owner, bucket.lane),
                        head->span,
                        bucket.normal_heads,
                        false,
                        head->include_head
                    ));
                }
                if (bucket.hurt_heads != 0U) {
                    static_cast<void>(context.coverage->add_coincident(
                        owner,
                        context.session->display_lane(owner, bucket.lane),
                        head->span,
                        bucket.hurt_heads,
                        true,
                        head->include_head
                    ));
                }
            }
        }
        ++context.buckets_visited;
    }

    [[nodiscard]] static std::uint64_t pattern_lower_bound(
        const PatternRun& pattern,
        const std::int64_t target_us
    ) noexcept {
        std::uint64_t first = 0U;
        std::uint64_t count = pattern.count;
        while (count > 0U) {
            const auto step = count / 2U;
            const auto middle = first + step;
            const auto note = pattern.note_at(middle);
            if (!note.has_value() || note->time_us >= target_us) {
                count = step;
            } else {
                first = middle + 1U;
                count -= step + 1U;
            }
        }
        return first;
    }

    [[nodiscard]] static std::uint64_t pattern_upper_bound(
        const PatternRun& pattern,
        const std::int64_t target_us
    ) noexcept {
        std::uint64_t first = 0U;
        std::uint64_t count = pattern.count;
        while (count > 0U) {
            const auto step = count / 2U;
            const auto middle = first + step;
            const auto note = pattern.note_at(middle);
            if (!note.has_value() || note->time_us > target_us) {
                count = step;
            } else {
                first = middle + 1U;
                count -= step + 1U;
            }
        }
        return first;
    }

    [[nodiscard]] static std::int64_t bounded_microseconds(
        const double milliseconds,
        const bool round_up
    ) noexcept {
        if (!std::isfinite(milliseconds)) {
            return milliseconds < 0.0
                ? std::numeric_limits<std::int64_t>::min()
                : std::numeric_limits<std::int64_t>::max();
        }
        const long double value = static_cast<long double>(milliseconds)
            * 1'000.0L;
        if (value <= static_cast<long double>(
                std::numeric_limits<std::int64_t>::min())) {
            return std::numeric_limits<std::int64_t>::min();
        }
        if (value >= static_cast<long double>(
                std::numeric_limits<std::int64_t>::max())) {
            return std::numeric_limits<std::int64_t>::max();
        }
        return static_cast<std::int64_t>(
            round_up ? std::ceil(value) : std::floor(value)
        );
    }

    void add_pattern_visuals(
        DenseNoteCoverage& coverage,
        const double cache_visual_time,
        const double speed,
        const double receptor_y,
        const std::int64_t first_time_us,
        const std::int64_t last_time_us
    ) const {
        const auto kinds = streaming_reader_->kinds();
        const auto patterns = streaming_reader_->patterns();
        std::size_t pattern_begin{};
        std::size_t pattern_end = patterns.size();
        if (streaming_pattern_index_sorted_
            && streaming_pattern_prefix_end_us_.size() == patterns.size()) {
            pattern_begin = static_cast<std::size_t>(std::distance(
                streaming_pattern_prefix_end_us_.begin(),
                std::lower_bound(
                    streaming_pattern_prefix_end_us_.begin(),
                    streaming_pattern_prefix_end_us_.end(),
                    first_time_us
                )
            ));
            pattern_end = static_cast<std::size_t>(std::distance(
                patterns.begin(),
                std::upper_bound(
                    patterns.begin()
                        + static_cast<std::ptrdiff_t>(pattern_begin),
                    patterns.end(),
                    last_time_us,
                    [](const std::int64_t time, const PatternRun& pattern) {
                        return time < pattern.start_us;
                    }
                )
            ));
        }
        for (std::size_t pattern_index = pattern_begin;
             pattern_index < pattern_end;
             ++pattern_index) {
            const auto& pattern = patterns[pattern_index];
            if (pattern.count == 0U || pattern.lane_pattern.empty()) {
                continue;
            }

            const auto pattern_first_time_us = [&]() noexcept {
                if (pattern.duration_us
                    > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())) {
                    return std::numeric_limits<std::int64_t>::min();
                }
                const auto duration = static_cast<std::int64_t>(
                    pattern.duration_us
                );
                return first_time_us
                        < std::numeric_limits<std::int64_t>::min() + duration
                    ? std::numeric_limits<std::int64_t>::min()
                    : first_time_us - duration;
            }();
            const auto first = pattern_lower_bound(
                pattern,
                pattern_first_time_us
            );
            const auto end = pattern_upper_bound(pattern, last_time_us);
            if (first >= end || first >= pattern.count) {
                continue;
            }

            std::vector<std::vector<std::uint32_t>> lane_positions(
                chart_->key_count
            );
            for (std::size_t position = 0U;
                 position < pattern.lane_pattern.size();
                 ++position) {
                const auto lane = pattern.lane_pattern[position];
                if (lane < lane_positions.size()) {
                    lane_positions[lane].push_back(
                        static_cast<std::uint32_t>(position)
                    );
                }
            }
            const auto period = static_cast<std::uint64_t>(
                pattern.lane_pattern.size()
            );
            const auto count_before = [period](
                const std::vector<std::uint32_t>& positions,
                const std::uint64_t exclusive
            ) {
                const auto cycles = exclusive / period;
                const auto remainder = static_cast<std::uint32_t>(
                    exclusive % period
                );
                const auto partial = static_cast<std::uint64_t>(
                    std::distance(
                        positions.begin(),
                        std::lower_bound(
                            positions.begin(),
                            positions.end(),
                            remainder
                        )
                    )
                );
                return cycles * static_cast<std::uint64_t>(positions.size())
                    + partial;
            };
            const std::string_view kind = pattern.kind_id < kinds.size()
                ? std::string_view(kinds[pattern.kind_id])
                : std::string_view{"normal"};
            const auto owner = runtime_owner(pattern.owner, kind);
            if (ai_note_owner(owner)
                && (gameplay_settings().hide_opponent_notes
                    || gameplay_settings().middle_scroll)) {
                continue;
            }
            const bool hurt = hurt_note_kind(kind);
            const double pattern_speed = speed * note_scroll_multiplier(kind);

            auto group_first = first;
            while (group_first < end) {
                const auto first_note = pattern.note_at(group_first);
                if (!first_note.has_value()) {
                    break;
                }
                // PULSEFORGE_P1_4_0_THIRD_STRUM_PATTERN_RECEPTOR_V1
                // PatternRun grouping must use the same owner-specific receptor
                // for both the first sample and every binary-search probe.
                // Otherwise Third Strum runs can be grouped into the primary
                // opponent's rows even though their exact draw path is +200 px.
                const double pattern_receptor_y = receptor_y_for_owner(owner)
                    + (receptor_y - (gameplay_settings().downscroll
                        ? 610.0 : 110.0));
                const auto first_visual = dense_visual_note_span(
                    static_cast<double>(first_note->time_us) / 1'000.0,
                    runtime_visual_duration_ms(
                        kind, static_cast<double>(first_note->duration_us) / 1'000.0
                    ),
                    cache_visual_time,
                    pattern_speed,
                    gameplay_settings().downscroll,
                    pattern_receptor_y
                );
                if (!first_visual.has_value()) {
                    break;
                }
                const auto row_for = [&](const std::uint64_t index) {
                    const auto note = pattern.note_at(index);
                    if (!note.has_value()) {
                        return coverage.row_count() * 2U;
                    }
                    const auto visual = dense_visual_note_span(
                        static_cast<double>(note->time_us) / 1'000.0,
                        runtime_visual_duration_ms(
                            kind, static_cast<double>(note->duration_us) / 1'000.0
                        ),
                        cache_visual_time,
                        pattern_speed,
                        gameplay_settings().downscroll,
                        pattern_receptor_y
                    );
                    if (!visual.has_value()) {
                        return coverage.row_count() * 2U;
                    }
                    const auto y = std::clamp(
                        visual->include_head
                            ? visual->span.head_y
                            : visual->span.tail_y,
                        0.0,
                        std::nextafter(
                            static_cast<double>(coverage.row_count())
                                * coverage.row_height(),
                            0.0
                        )
                    );
                    const auto visual_row = std::min(
                        static_cast<std::size_t>(y / coverage.row_height()),
                        coverage.row_count() - 1U
                    );
                    return visual_row * 2U
                        + (visual->include_head ? 1U : 0U);
                };
                const auto row = row_for(group_first);
                std::uint64_t lower = group_first + 1U;
                std::uint64_t upper = end;
                while (lower < upper) {
                    const auto middle = lower + (upper - lower) / 2U;
                    if (row_for(middle) == row) {
                        lower = middle + 1U;
                    } else {
                        upper = middle;
                    }
                }
                const auto group_end = lower;
                for (std::uint16_t source_lane = 0U;
                     source_lane < lane_positions.size();
                     ++source_lane) {
                    if (source_lane >= active_key_count(owner)) {
                        continue;
                    }
                    const auto& positions = lane_positions[source_lane];
                    if (positions.empty()) {
                        continue;
                    }
                    const auto occurrences = count_before(
                        positions,
                        group_end
                    ) - count_before(positions, group_first);
                    static_cast<void>(coverage.add_coincident(
                        owner,
                        streaming_session_->display_lane(owner, source_lane),
                        first_visual->span,
                        occurrences,
                        hurt,
                        first_visual->include_head
                    ));
                }
                group_first = group_end;
            }
        }
    }

    [[nodiscard]] bool rebuild_streaming_visual_cache(
        const double visual_time,
        const double speed
    ) {
        const double margin_pixels = streaming_visual_cache_margin_pixels();
        const double coverage_height = static_cast<double>(logical_height)
            + margin_pixels * 2.0;
        const double receptor_y = (gameplay_settings().downscroll
            ? 610.0
            : 110.0) + margin_pixels;
        try {
            const auto density_hint = static_cast<std::size_t>(
                std::min<std::uint64_t>(
                    static_cast<std::uint64_t>(
                        options_.settings.performance.max_visible_notes
                    ) * 8U,
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::size_t>::max()
                    )
                )
            );
            const double cache_row_height = dense_note_row_height(density_hint);
            auto coverage = std::make_unique<DenseNoteCoverage>(
                chart_->key_count,
                coverage_height,
                cache_row_height
            );
            // Anchor every rebuild to the same screen-pixel phase. Without
            // this, each rebuild starts its density rows at an arbitrary song
            // time; textured heads then jump between rows and appear to blink.
            const double cache_visual_time = speed > 0.0
                ? std::floor(visual_time * speed / cache_row_height)
                    * cache_row_height / speed
                : visual_time;
            const double future_ms = visible_time_window_ms(
                static_cast<double>(logical_height) + margin_pixels + 38.0,
                minimum_visual_speed(speed),
                50.0
            );
            // Keep a symmetric offscreen trajectory plus the late-hit window.
            // Otherwise a cluster larger than max_window_notes vanished the
            // instant it crossed the receptor because only the bounded active
            // gameplay window could represent its past side.
            const double miss_ms = std::isfinite(gameplay_settings().windows.miss_ms)
                ? std::max(0.0, gameplay_settings().windows.miss_ms)
                : 180.0;
            const double past_ms = visible_time_window_ms(
                static_cast<double>(logical_height) + margin_pixels + 38.0,
                minimum_visual_speed(speed),
                miss_ms + 50.0
            );
            const auto first_us = bounded_microseconds(
                cache_visual_time - past_ms,
                false
            );
            const auto last_us = bounded_microseconds(
                cache_visual_time + future_ms,
                true
            );
            bool visual_index_used{};
            if (streaming_visual_density_reader_.has_value()
                && !note_scroll_multiplier_active_
                // PULSEFORGE_P1_5_0C_SUSTAIN_DISABLED_PVD_FALLBACK_V1
                // PVD1 aggregates sustain occupancy without retaining kind, so
                // it cannot erase tails for sustain.enabled=false. Fall back to
                // the exact bounded PFC visitor whenever such a policy exists.
                && !note_sustain_disable_policy_active_
                // PULSEFORGE_P1_4_0_THIRD_STRUM_PVD_FALLBACK_V1
                // PVD1 v1 aggregates only the packed two-owner field and does
                // not retain note kind, so it cannot distinguish Third Strum
                // from the primary opponent. Use exact bounded PFC1 visits for
                // charts that enable a secondary strumline.
                && !secondary_strum_enabled()) {
                std::vector<VisualDensitySustainCell> sustain_cells(
                    static_cast<std::size_t>(chart_->key_count) * 2U,
                    VisualDensitySustainCell{first_us, 0U}
                );
                VisualDensityCoverageContext density_context{
                    coverage.get(),
                    streaming_session_.get(),
                    sustain_cells,
                    chart_->key_count,
                    active_key_count(NoteOwner::player),
                    active_key_count(NoteOwner::opponent),
                    first_us,
                    last_us,
                    cache_visual_time,
                    speed,
                    receptor_y,
                    gameplay_settings().downscroll,
                    gameplay_settings().hide_opponent_notes
                        || gameplay_settings().middle_scroll,
                    0U,
                };
                // The saturated path already represents many logical notes
                // in one dense screen row. Asking PVD for twice-finer temporal
                // buckets only increases cache-rebuild CPU without producing
                // additional visible detail. Match the indexed resolution to
                // two dense rows per density sample to cut bucket visits.
                constexpr double pvd_dense_bucket_rows = 2.0;
                const auto target_bucket_us = static_cast<std::uint64_t>(
                    std::clamp(
                        std::ceil(
                            coverage->row_height() / speed * 1'000.0
                            * pvd_dense_bucket_rows
                        ),
                        1.0,
                        static_cast<double>(
                            std::numeric_limits<std::uint64_t>::max()
                        )
                    )
                );
                const auto pvd_started_ns = diagnostics_
                    ? SDL_GetTicksNS()
                    : std::uint64_t{0U};
                const auto visited = streaming_visual_density_reader_->visit(
                    first_us,
                    last_us,
                    target_bucket_us,
                    &density_context,
                    add_visual_density_bucket
                );
                if (diagnostics_) {
                    note_profile_frame_.pvd_visit_ns +=
                        SDL_GetTicksNS() - pvd_started_ns;
                }
                if (visited) {
                    for (std::uint16_t lane = 0U;
                         lane < chart_->key_count;
                         ++lane) {
                        for (const auto owner : {
                                PackedNoteOwner::opponent,
                                PackedNoteOwner::player,
                            }) {
                            const auto owner_index = owner
                                    == PackedNoteOwner::player
                                ? std::size_t{1U}
                                : std::size_t{0U};
                            const auto& sustain = sustain_cells[
                                owner_index
                                    * static_cast<std::size_t>(chart_->key_count)
                                + lane
                            ];
                            add_density_sustain_segment(
                                density_context,
                                owner,
                                lane,
                                sustain.last_time_us,
                                last_us,
                                sustain.active
                            );
                        }
                    }
                    streaming_density_buckets_ = visited.buckets_visited;
                    visual_index_used = true;
                } else if (!streaming_visual_error_reported_) {
                    std::cerr << "Streaming PVD1 warning: "
                              << visited.error
                              << "; using exact PFC1 visual visits\n";
                    streaming_visual_error_reported_ = true;
                    streaming_visual_density_reader_.reset();
                }
            }
            if (!visual_index_used) {
                PackedVisualCoverageContext context{
                    coverage.get(),
                    streaming_session_.get(),
                    this,
                    streaming_reader_->kinds(),
                    cache_visual_time,
                    speed,
                    receptor_y,
                    gameplay_settings().downscroll,
                    gameplay_settings().hide_opponent_notes
                        || gameplay_settings().middle_scroll,
                };
                const auto pfc_started_ns = diagnostics_
                    ? SDL_GetTicksNS()
                    : std::uint64_t{0U};
                const auto visited = streaming_reader_->visit_explicit_notes_in_range(
                    first_us,
                    last_us,
                    &context,
                    add_packed_visual_note
                );
                if (diagnostics_) {
                    note_profile_frame_.pfc_visit_ns +=
                        SDL_GetTicksNS() - pfc_started_ns;
                }
                if (!visited) {
                    if (!streaming_visual_error_reported_) {
                        std::cerr << "Streaming visual range warning: "
                                  << visited.error << '\n';
                        streaming_visual_error_reported_ = true;
                    }
                    return false;
                }
                streaming_explicit_visual_visits_ = visited.notes_visited;
            }
            add_pattern_visuals(
                *coverage,
                cache_visual_time,
                speed,
                receptor_y,
                first_us,
                last_us
            );
            coverage->finalize();
            streaming_visual_cache_ = std::move(coverage);
            streaming_visual_cache_time_ms_ = cache_visual_time;
            streaming_visual_cache_speed_ = speed;
            streaming_visual_cache_downscroll_ =
                gameplay_settings().downscroll;
            streaming_visual_cache_hide_opponent_ =
                gameplay_settings().hide_opponent_notes
                || gameplay_settings().middle_scroll;
            streaming_visual_error_reported_ = false;
            return true;
        } catch (const std::exception& exception) {
            if (!streaming_visual_error_reported_) {
                std::cerr << "Streaming visual cache warning: "
                          << exception.what() << '\n';
                streaming_visual_error_reported_ = true;
            }
            return false;
        }
    }

    [[nodiscard]] bool render_saturated_streaming_notes(
        const double visual_time,
        const double speed
    ) {
        const double margin_pixels = streaming_visual_cache_margin_pixels();
        const double travelled = std::abs(
            (visual_time - streaming_visual_cache_time_ms_) * speed
        );
        const bool cache_valid = streaming_visual_cache_ != nullptr
            && visual_time >= streaming_visual_cache_time_ms_
            && travelled <= margin_pixels * 0.75
            && std::abs(speed - streaming_visual_cache_speed_) <= 0.000001
            && gameplay_settings().downscroll
                == streaming_visual_cache_downscroll_
            && (gameplay_settings().hide_opponent_notes
                    || gameplay_settings().middle_scroll)
                == streaming_visual_cache_hide_opponent_;
        if (!cache_valid) {
            const auto cache_started_ns = diagnostics_
                ? SDL_GetTicksNS()
                : std::uint64_t{0U};
            const bool rebuilt = rebuild_streaming_visual_cache(
                visual_time,
                speed
            );
            if (diagnostics_) {
                note_profile_frame_.cache_rebuild_ns +=
                    SDL_GetTicksNS() - cache_started_ns;
                ++note_profile_frame_.cache_rebuilds;
            }
            if (!rebuilt) {
                return false;
            }
        }

        auto& active_coverage = frame_note_coverage(
            streaming_session_->window_notes().size()
        );
        const auto visual_us = bounded_microseconds(visual_time, false);
        const auto streaming_kinds = streaming_reader_->kinds();
        for (const auto& window : streaming_session_->window_notes()) {
            if (window.note.time_us >= visual_us) {
                break;
            }
            // Both explicit records and arithmetic PatternRun occurrences can
            // remain pending inside the hit/miss window after crossing the
            // receptor.  Re-add every bounded window entry here; otherwise the
            // receptor clip correctly removes the aggregate PatternRun cell
            // but also makes a still-judgeable procedural note disappear.
            if (!note_is_visible(window.state)) {
                continue;
            }
            const std::string_view kind =
                window.note.kind_id < streaming_kinds.size()
                ? std::string_view(
                    streaming_kinds[window.note.kind_id]
                )
                : std::string_view{"normal"};
            const auto owner = runtime_owner(window.note.owner, kind);
            if (ai_note_owner(owner)
                && (gameplay_settings().hide_opponent_notes
                    || gameplay_settings().middle_scroll)) {
                continue;
            }
            const double note_speed = speed * note_scroll_multiplier(kind);
            const auto visual = visual_note_span(
                static_cast<double>(window.note.time_us) / 1'000.0,
                runtime_visual_duration_ms(
                    kind, static_cast<double>(window.note.duration_us) / 1'000.0
                ),
                visual_time,
                note_speed,
                gameplay_settings().downscroll,
                receptor_y_for_owner(owner),
                window.state != NoteState::pending
            );
            if (visual.has_value()) {
                static_cast<void>(active_coverage.add(
                    owner,
                    window.display_lane,
                    visual->span,
                    hurt_note_kind(kind),
                    visual->include_head
                ));
            }
        }
        active_coverage.finalize();
        draw_dense_note_coverage(active_coverage);

        const double direction = gameplay_settings().downscroll ? -1.0 : 1.0;
        const double y_offset = -margin_pixels
            - direction
                * (visual_time - streaming_visual_cache_time_ms_) * speed;
        // The PVD cache now covers both sides of the receptor. Drawing only
        // its future half made any pending avalanche beyond max_window_notes
        // disappear as soon as it crossed. Active exact notes may overlap the
        // aggregate cell briefly, which only affects opacityÃ¢â‚¬â€not note count,
        // timing, judgment or per-note draw cost.
        // Re-clip after translating the reusable cache. Without this step,
        // rows from consumed chart time visibly cross the receptor until the
        // next 256 px cache rebuild even though gameplay has judged them.
        draw_dense_note_coverage(
            *streaming_visual_cache_,
            y_offset,
            DenseReceptorClip{
                gameplay_settings().downscroll ? 610.0 : 110.0,
                gameplay_settings().downscroll,
            }
        );
        const auto active_count = active_coverage.represented_note_count();
        const auto future_count = streaming_visual_cache_
            ->represented_note_count();
        rendered_notes_ = future_count
                > std::numeric_limits<std::uint64_t>::max() - active_count
            ? std::numeric_limits<std::uint64_t>::max()
            : future_count + active_count;
        return true;
    }

    void render_streaming_notes(
        const double visual_time,
        const double speed
    ) {
        // A saturated judgment window may contain hundreds of thousands of
        // entries. Decide on the indexed LOD path before walking it; the old
        // order performed a full dense scan and then immediately discarded it.
        if (streaming_session_->window_saturated()
            && render_saturated_streaming_notes(visual_time, speed)) {
            return;
        }
        auto& coverage = frame_note_coverage(
            streaming_session_->window_notes().size()
        );
        const auto streaming_kinds = streaming_reader_->kinds();
        for (const auto& window : streaming_session_->window_notes()) {
            if (!note_is_visible(window.state)) {
                continue;
            }
            const std::string_view kind =
                window.note.kind_id < streaming_kinds.size()
                ? std::string_view(
                    streaming_kinds[window.note.kind_id]
                )
                : std::string_view{"normal"};
            const auto owner = runtime_owner(window.note.owner, kind);
            if (ai_note_owner(owner)
                && (gameplay_settings().hide_opponent_notes
                    || gameplay_settings().middle_scroll)) {
                continue;
            }
            const double note_speed = speed * note_scroll_multiplier(kind);
            const auto visual = visual_note_span(
                static_cast<double>(window.note.time_us) / 1'000.0,
                runtime_visual_duration_ms(
                    kind, static_cast<double>(window.note.duration_us) / 1'000.0
                ),
                visual_time,
                note_speed,
                gameplay_settings().downscroll,
                receptor_y_for_owner(owner),
                window.state != NoteState::pending
            );
            if (!visual.has_value()) {
                continue;
            }
            const bool hurt = hurt_note_kind(kind);
            static_cast<void>(coverage.add(
                owner,
                window.display_lane,
                visual->span,
                hurt,
                visual->include_head
            ));
        }
        rendered_notes_ = coverage.represented_note_count();
        const auto exact_threshold = static_cast<std::uint64_t>(
            options_.settings.performance.max_visible_notes
        );
        if (rendered_notes_ > exact_threshold) {
            coverage.finalize();
            draw_dense_note_coverage(coverage);
            return;
        }
        const double head_quantum = exact_head_quantum_pixels();
        std::vector<std::int32_t> last_head_bins(
            static_cast<std::size_t>(chart_->key_count) * 3U,
            std::numeric_limits<std::int32_t>::min()
        );
        for (const auto& window : streaming_session_->window_notes()) {
            if (!note_is_visible(window.state)) {
                continue;
            }
            const std::string_view kind =
                window.note.kind_id < streaming_kinds.size()
                ? std::string_view(streaming_kinds[window.note.kind_id])
                : std::string_view{"normal"};
            const auto owner = runtime_owner(window.note.owner, kind);
            if (ai_note_owner(owner)
                && (gameplay_settings().hide_opponent_notes
                    || gameplay_settings().middle_scroll)) {
                continue;
            }
            const double note_speed = speed * note_scroll_multiplier(kind);
            const auto visual = visual_note_span(
                static_cast<double>(window.note.time_us) / 1'000.0,
                runtime_visual_duration_ms(
                    kind, static_cast<double>(window.note.duration_us) / 1'000.0
                ),
                visual_time,
                note_speed,
                gameplay_settings().downscroll,
                receptor_y_for_owner(owner),
                window.state != NoteState::pending
            );
            if (visual.has_value()
                && note_intersects_viewport(
                    visual->span,
                    logical_height
                )) {
                auto optimized_visual = *visual;
                if (optimized_visual.include_head) {
                    const auto owner_index = note_owner_slot(owner);
                    const auto slot = owner_index
                        * static_cast<std::size_t>(chart_->key_count)
                        + window.display_lane;
                    const auto bin64 = static_cast<std::int64_t>(std::floor(
                        optimized_visual.span.head_y / head_quantum
                    ));
                    const auto bin = static_cast<std::int32_t>(std::clamp(
                        bin64,
                        static_cast<std::int64_t>(
                            std::numeric_limits<std::int32_t>::min()
                        ),
                        static_cast<std::int64_t>(
                            std::numeric_limits<std::int32_t>::max()
                        )
                    ));
                    if (slot < last_head_bins.size()
                        && last_head_bins[slot] == bin) {
                        optimized_visual.include_head = false;
                    } else if (slot < last_head_bins.size()) {
                        last_head_bins[slot] = bin;
                    }
                }
                draw_streaming_note(
                    window,
                    owner,
                    optimized_visual,
                    kind
                );
            }
        }
    }

    struct DenseReceptorClip final {
        double receptor_y{};
        bool downscroll{};
    };

    void draw_dense_note_coverage(
        const DenseNoteCoverage& coverage,
        const double y_offset = 0.0,
        const std::optional<DenseReceptorClip> receptor_clip = std::nullopt
    ) {
        // PULSEFORGE_P1_4_0_DENSE_THREE_OWNER_RESERVE_V1
        // Two visual primitives (head+sustain) for each of three owners.
        const auto expected_quads = coverage.row_count()
            * static_cast<std::size_t>(coverage.lane_count()) * 6U;
        dense_quad_batch_.begin(expected_quads);

        const auto opacity = [](const std::uint64_t count) {
            if (count == 0U) {
                return std::uint8_t{0};
            }
            const double intensity = 70.0
                + 27.0 * std::log2(static_cast<double>(count) + 1.0);
            return static_cast<std::uint8_t>(
                std::clamp(intensity, 70.0, 255.0)
            );
        };

        for (const auto owner : {
                NoteOwner::opponent,
                NoteOwner::secondary_opponent,
                NoteOwner::player,
            }) {
            if (owner == NoteOwner::secondary_opponent
                && !secondary_strum_enabled()) {
                continue;
            }
            if (ai_note_owner(owner)
                && (gameplay_settings().hide_opponent_notes
                    || gameplay_settings().middle_scroll)) {
                continue;
            }

            const auto [base_x, logical_lane_width] = lane_layout(owner);
            const auto active_keys = active_key_count(owner);
            for (std::uint16_t lane = 0U;
                 lane < coverage.lane_count() && lane < active_keys;
                 ++lane) {
#if defined(PULSEFORGE_HAS_LUA)
                if (!script_lane_visible(owner, lane)) {
                    continue;
                }
                const float x = script_lane_x(
                    owner, lane, base_x, logical_lane_width
                );
                const float lane_width = script_lane_width(owner, logical_lane_width);
                const double script_alpha = script_lane_alpha(owner, lane);
                const double note_angle = script_lane_angle(owner, lane);
#else
                const float x = base_x
                    + static_cast<float>(lane) * logical_lane_width;
                const float lane_width = logical_lane_width;
                constexpr double script_alpha = 1.0;
                constexpr double note_angle = 0.0;
#endif
                auto base_color = lane_color(lane);
                if (ai_note_owner(owner)) {
                    base_color.a = 145U;
                }
                base_color.a = static_cast<std::uint8_t>(std::clamp(
                    static_cast<double>(base_color.a) * script_alpha,
                    0.0,
                    255.0
                ));

                const bool textured_head = scene_ != nullptr
                    && lane <= std::numeric_limits<std::uint8_t>::max()
                    && scene_->note_skin_available(
                        detail::RuntimeNoteSkinElement::note_head,
                        static_cast<std::uint8_t>(lane)
                    );
                const bool textured_sustain = scene_ != nullptr
                    && lane <= std::numeric_limits<std::uint8_t>::max()
                    && scene_->note_skin_available(
                        detail::RuntimeNoteSkinElement::sustain_body,
                        static_cast<std::uint8_t>(lane)
                    );

                bool sustain_run_active{};
                double sustain_run_y{};
                double sustain_run_end{};
                std::uint8_t sustain_run_alpha{};

                const auto flush_sustain_run = [&]() {
                    if (!sustain_run_active) {
                        return;
                    }
                    ++visual_draw_units_;
                    const SDL_FRect rectangle{
                        x + lane_width * 0.39F,
                        static_cast<float>(sustain_run_y),
                        lane_width * 0.22F,
                        std::max(
                            2.0F,
                            static_cast<float>(
                                sustain_run_end - sustain_run_y
                            )
                        ),
                    };
                    if (!textured_sustain
                        || !queue_runtime_note_skin(
                            detail::RuntimeNoteSkinElement::sustain_body,
                            lane,
                            rectangle,
                            sustain_run_alpha,
                            false,
                            note_angle
                        )) {
                        auto sustain = base_color;
                        sustain.a = sustain_run_alpha;
                        dense_quad_batch_.add(rectangle, sustain);
                    }
                    sustain_run_active = false;
                };

                for (std::size_t row = 0U;
                     row < coverage.row_count();
                     ++row) {
                    const auto& cell = coverage.cell(owner, lane, row);
                    if (cell.head_count == 0U
                        && cell.sustain_count == 0U) {
                        flush_sustain_run();
                        continue;
                    }

                    const double source_y = static_cast<double>(row)
                            * coverage.row_height()
                        + y_offset;
                    auto draw_span = DenseNoteRowSpan{
                        source_y,
                        coverage.row_height(),
                    };
                    if (receptor_clip.has_value()) {
                        const auto clipped = clip_dense_note_row_to_receptor(
                            source_y,
                            coverage.row_height(),
                            owner == NoteOwner::secondary_opponent
                                ? receptor_y_for_owner(owner)
                                : receptor_clip->receptor_y,
                            receptor_clip->downscroll
                        );
                        if (!clipped.has_value()) {
                            flush_sustain_run();
                            continue;
                        }
                        draw_span = *clipped;
                    }
#if defined(PULSEFORGE_HAS_LUA)
                    draw_span.y = script_note_y(owner, lane, draw_span.y);
                    draw_span.height = std::max(
                        0.5,
                        draw_span.height * script_owner_camera_zoom(owner)
                    );
#endif
                    if (draw_span.y + draw_span.height < 0.0
                        || draw_span.y > static_cast<double>(logical_height)) {
                        flush_sustain_run();
                        continue;
                    }

                    if (cell.sustain_count > 0U) {
                        const auto sustain_alpha =
                            static_cast<std::uint8_t>(
                                std::min<std::uint32_t>(
                                    std::min<std::uint32_t>(
                                        opacity(cell.sustain_count),
                                        210U
                                    ),
                                    base_color.a
                                )
                            );
                        const double row_end =
                            draw_span.y + draw_span.height;
                        const bool contiguous = sustain_run_active
                            && std::abs(
                                sustain_run_end - draw_span.y
                            ) <= 0.01;
                        if (!contiguous) {
                            flush_sustain_run();
                            sustain_run_active = true;
                            sustain_run_y = draw_span.y;
                            sustain_run_end = row_end;
                            sustain_run_alpha = sustain_alpha;
                        } else {
                            sustain_run_end = row_end;
                            sustain_run_alpha = std::max(
                                sustain_run_alpha,
                                sustain_alpha
                            );
                        }
                    } else {
                        flush_sustain_run();
                    }

                    if (cell.head_count == 0U) {
                        continue;
                    }

                    const auto hurt_count = std::min(
                        cell.hurt_head_count,
                        cell.head_count
                    );
                    const auto normal_count =
                        cell.head_count - hurt_count;

                    if (normal_count > 0U) {
                        ++visual_draw_units_;
                        const auto head_alpha =
                            static_cast<std::uint8_t>(
                                std::min<std::uint32_t>(
                                    opacity(normal_count),
                                    base_color.a
                                )
                            );
                        const float dense_head_height = receptor_clip.has_value()
                            ? std::max(
                                  2.0F,
                                  static_cast<float>(draw_span.height)
                              )
                            : std::clamp(
                                  static_cast<float>(
                                      coverage.row_height() * 2.5
                                  ),
                                  8.0F,
                                  22.0F
                              );
                        const SDL_FRect head_rectangle{
                            x + 8.0F,
                            static_cast<float>(
                                draw_span.y
                                + draw_span.height * 0.5
                            ) - dense_head_height * 0.5F,
                            lane_width - 16.0F,
                            dense_head_height,
                        };

                        if (!textured_head
                            || !queue_runtime_note_skin(
                                detail::RuntimeNoteSkinElement::note_head,
                                lane,
                                head_rectangle,
                                head_alpha,
                                false,
                                note_angle
                            )) {
                            auto head = base_color;
                            head.a = head_alpha;
                            dense_quad_batch_.add(
                                head_rectangle,
                                head
                            );
                        }
                    }

                    if (hurt_count > 0U) {
                        ++visual_draw_units_;
                        SDL_Color hurt{255, 56, 72, static_cast<std::uint8_t>(
                            std::clamp(
                                static_cast<double>(opacity(hurt_count))
                                    * script_alpha,
                                0.0,
                                255.0
                            )
                        )};
                        if (ai_note_owner(owner)) {
                            hurt.a = std::min<std::uint8_t>(
                                hurt.a,
                                static_cast<std::uint8_t>(std::clamp(
                                    145.0 * script_alpha, 0.0, 255.0
                                ))
                            );
                        }
                        dense_quad_batch_.add(
                            {
                                x + 8.0F,
                                static_cast<float>(draw_span.y),
                                lane_width - 16.0F,
                                receptor_clip.has_value()
                                    ? static_cast<float>(draw_span.height)
                                    : std::max(
                                          2.0F,
                                          static_cast<float>(
                                              draw_span.height
                                          )
                                      ),
                            },
                            hurt
                        );
                    }
                }
                flush_sustain_run();
            }
        }

        if (dense_quad_batch_.quad_count() != 0U) {
            ++visual_geometry_calls_;
            if (!dense_quad_batch_.flush(renderer_)
                && !dense_geometry_error_reported_) {
                std::cerr << "Dense note geometry warning: "
                          << SDL_GetError() << '\n';
                dense_geometry_error_reported_ = true;
            }
        }
    }

    [[nodiscard]] bool draw_runtime_note_skin(
        const detail::RuntimeNoteSkinElement element,
        const std::uint16_t lane,
        const SDL_FRect& rectangle,
        const std::uint8_t alpha,
        const bool flip_vertical = false,
        const double angle = 0.0,
        const detail::RuntimeNoteSkinProfile profile =
            detail::runtime_note_skin_default_profile,
        const std::array<std::uint8_t, 3U> rgb = {255U, 255U, 255U}
    ) noexcept {
        if (scene_ == nullptr
            || lane > std::numeric_limits<std::uint8_t>::max()) {
            return false;
        }
        return scene_->render_note_skin(detail::RuntimeNoteSkinDraw{
            element,
            static_cast<std::uint8_t>(lane),
            detail::RuntimeLogicalRect{
                rectangle.x,
                rectangle.y,
                rectangle.w,
                rectangle.h,
            },
            alpha,
            flip_vertical,
            angle,
            profile,
            rgb,
        });
    }

    [[nodiscard]] bool queue_runtime_note_skin(
        const detail::RuntimeNoteSkinElement element,
        const std::uint16_t lane,
        const SDL_FRect& rectangle,
        const std::uint8_t alpha,
        const bool flip_vertical = false,
        const double angle = 0.0,
        const detail::RuntimeNoteSkinProfile profile =
            detail::runtime_note_skin_default_profile,
        const std::array<std::uint8_t, 3U> rgb = {255U, 255U, 255U}
    ) {
        if (scene_ == nullptr
            || lane > std::numeric_limits<std::uint8_t>::max()
            || !scene_->note_skin_available(
                element,
                static_cast<std::uint8_t>(lane),
                profile
            )) {
            return false;
        }
        note_skin_draw_batch_.push_back(detail::RuntimeNoteSkinDraw{
            element,
            static_cast<std::uint8_t>(lane),
            detail::RuntimeLogicalRect{
                rectangle.x,
                rectangle.y,
                rectangle.w,
                rectangle.h,
            },
            alpha,
            flip_vertical,
            angle,
            profile,
            rgb,
        });
        return true;
    }

    void flush_runtime_note_skin_batch() noexcept {
        if (note_skin_draw_batch_.empty()) {
            return;
        }
        ++visual_geometry_calls_;
        if (scene_ == nullptr
            || !scene_->render_note_skin_batch(note_skin_draw_batch_)) {
            if (!note_skin_batch_error_reported_) {
                std::cerr << "Note-skin batch warning: "
                          << SDL_GetError() << '\n';
                note_skin_batch_error_reported_ = true;
            }
        } else {
            note_skin_batch_error_reported_ = false;
        }
        note_skin_draw_batch_.clear();
    }

    void render_lane_set(const NoteOwner owner) {
        if (owner == NoteOwner::secondary_opponent
            && !secondary_strum_enabled()) {
            return;
        }
        if (ai_note_owner(owner)
            && (gameplay_settings().hide_opponent_notes
                || gameplay_settings().middle_scroll)) {
            return;
        }
        const auto [base_x, logical_lane_width] = lane_layout(owner);
        const auto underlay_alpha = static_cast<std::uint8_t>(
            std::clamp(options_.settings.visual.lane_underlay_opacity, 0.0F, 1.0F)
            * (owner == NoteOwner::player ? 180.0F : 90.0F)
        );
        const auto active_keys = active_key_count(owner);
        for (std::uint16_t lane = 0; lane < active_keys; ++lane) {
#if defined(PULSEFORGE_HAS_LUA)
            if (!script_lane_visible(owner, lane)) {
                continue;
            }
            const float x = script_lane_x(
                owner,
                lane,
                base_x,
                logical_lane_width
            );
            const float lane_width = script_lane_width(owner, logical_lane_width);
            const float receptor_y = static_cast<float>(
                script_lane_receptor_y(owner, lane)
            );
            const double script_alpha = script_lane_alpha(owner, lane);
            const double note_angle = script_lane_angle(owner, lane);
#else
            const float x = base_x
                + static_cast<float>(lane) * logical_lane_width;
            const float lane_width = logical_lane_width;
            const float receptor_y = static_cast<float>(
                receptor_y_for_owner(owner)
            );
            constexpr double script_alpha = 1.0;
            constexpr double note_angle = 0.0;
#endif
            auto lane_underlay_alpha = static_cast<std::uint8_t>(std::clamp(
                static_cast<double>(underlay_alpha) * script_alpha,
                0.0,
                255.0
            ));
            fill_rect(
                renderer_,
                {x + 2.0F, 0.0F, std::max(1.0F, lane_width - 4.0F), logical_height},
                {4, 6, 16, lane_underlay_alpha}
            );
            auto color = lane_color(lane);
            color.a = static_cast<std::uint8_t>(std::clamp(
                static_cast<double>(owner == NoteOwner::player ? 215U : 105U)
                    * script_alpha,
                0.0,
                255.0
            ));
#if defined(PULSEFORGE_HAS_LUA)
            const auto global_strum = script_fixed_strum_index(owner, lane);
            const double receptor_scale_x =
                global_strum < script_strums_.size()
                ? script_strums_[global_strum].scale_x
                : 1.0;
            const double receptor_scale_y =
                global_strum < script_strums_.size()
                ? script_strums_[global_strum].scale_y
                : 1.0;
            detail::RuntimeNoteSkinProfile receptor_profile =
                detail::runtime_note_skin_default_profile;
            bool custom_receptor_texture_missing{};
            if (global_strum < script_strums_.size()
                && !script_strums_[global_strum].texture.empty()) {
                if (script_strums_[global_strum].texture_profile.has_value()) {
                    receptor_profile = *script_strums_[global_strum].texture_profile;
                } else {
                    custom_receptor_texture_missing = true;
                }
            }
#else
            constexpr double receptor_scale_x = 1.0;
            constexpr double receptor_scale_y = 1.0;
            constexpr detail::RuntimeNoteSkinProfile receptor_profile =
                detail::runtime_note_skin_default_profile;
            constexpr bool custom_receptor_texture_missing = false;
#endif
            const float receptor_width = std::max(
                1.0F,
                (lane_width - 14.0F)
                    * static_cast<float>(receptor_scale_x)
            );
            const float receptor_height = std::max(
                1.0F,
                50.0F * static_cast<float>(
#if defined(PULSEFORGE_HAS_LUA)
                    script_owner_camera_zoom(owner)
#else
                    1.0
#endif
                ) * static_cast<float>(receptor_scale_y)
            );
            const SDL_FRect receptor{
                x + lane_width * 0.5F - receptor_width * 0.5F,
                receptor_y - receptor_height * 0.5F,
                receptor_width,
                receptor_height,
            };
            if (custom_receptor_texture_missing
                || !draw_runtime_note_skin(
                    detail::RuntimeNoteSkinElement::receptor,
                    lane,
                    receptor,
                    color.a,
                    false,
                    note_angle,
                    receptor_profile
                )) {
                fill_rect(renderer_, receptor, {8, 10, 24, color.a});
                outline_rect(renderer_, receptor, color);
            }
            if (owner == NoteOwner::player && gameplay_lane_held(lane)) {
                auto held_color = color;
                held_color.a = static_cast<std::uint8_t>(
                    static_cast<unsigned>(held_color.a) * 115U / 215U
                );
                fill_rect(renderer_, receptor, held_color);
            }
        }
    }

    void draw_note(
        const std::size_t index,
        const NoteVisualSpan& visual
    ) {
        const auto& note = chart_->notes[index];
        if (note.lane >= active_key_count(note.owner)) {
            return;
        }
        const auto lane = session_->display_lane(index);
        const auto [base_x, logical_lane_width] = lane_layout(note.owner);
#if defined(PULSEFORGE_HAS_LUA)
        if (!script_lane_visible(note.owner, lane)) {
            return;
        }
        const float x = script_lane_x(
            note.owner, lane, base_x, logical_lane_width
        );
        const float lane_width = script_lane_width(note.owner, logical_lane_width);
        const float y = static_cast<float>(
            script_note_y(note.owner, lane, visual.span.head_y)
        );
        const double script_alpha = script_lane_alpha(note.owner, lane);
        const double note_angle = script_lane_angle(note.owner, lane);
#else
        const float x = base_x + static_cast<float>(lane) * logical_lane_width;
        const float lane_width = logical_lane_width;
        const float y = static_cast<float>(visual.span.head_y);
        constexpr double script_alpha = 1.0;
        constexpr double note_angle = 0.0;
#endif
        const auto visual_owner = runtime_owner(note.owner, note.kind);
        bool custom_note_texture_missing{};
        const auto note_skin_profile = script_note_skin_profile(
            visual_owner, note.kind, custom_note_texture_missing
        );
        const double type_alpha = script_note_visual_alpha(visual_owner, note.kind);
        const double effective_note_alpha = std::clamp(
            script_alpha * type_alpha, 0.0, 1.0
        );
        const auto note_rgb = script_note_visual_rgb(visual_owner, note.kind);
        const float note_scale = static_cast<float>(
            script_note_visual_scale(visual_owner, note.kind)
        );
        const bool sustain_inherits_type = runtime_sustain_inherits_type(note.kind);
        ++visual_draw_units_;
        auto base_color = lane_color(lane);
        base_color.a = static_cast<std::uint8_t>(std::clamp(
            static_cast<double>(ai_note_owner(note.owner) ? 145U : base_color.a)
                * std::clamp(script_alpha, 0.0, 1.0),
            0.0,
            255.0
        ));
        auto color = base_color;
        color.a = static_cast<std::uint8_t>(std::clamp(
            static_cast<double>(color.a) * type_alpha, 0.0, 255.0
        ));
        const bool hurt = hurt_note_kind(note.kind);
        // PULSEFORGE_P1_5_0D_HURT_NOTE_SKIN_RUNTIME_V1
        // A resolved HURTNOTE_assets profile is authoritative. The historical
        // red rectangle remains the deterministic fallback when that asset is
        // absent, malformed, or rejected by the bounded profile resolver.
        const bool skinnable_note = !custom_note_texture_missing;
        if (hurt && custom_note_texture_missing) {
            color = {255, 56, 72, static_cast<std::uint8_t>(std::clamp(
                230.0 * effective_note_alpha, 0.0, 255.0
            ))};
        }
        color.r = static_cast<std::uint8_t>(
            static_cast<unsigned>(color.r) * note_rgb[0] / 255U
        );
        color.g = static_cast<std::uint8_t>(
            static_cast<unsigned>(color.g) * note_rgb[1] / 255U
        );
        color.b = static_cast<std::uint8_t>(
            static_cast<unsigned>(color.b) * note_rgb[2] / 255U
        );

        if (runtime_sustain_enabled(note.kind, note.duration_ms)) {
#if defined(PULSEFORGE_HAS_LUA)
            const float tail_y = static_cast<float>(
                script_note_y(note.owner, lane, visual.span.tail_y)
            );
#else
            const float tail_y = static_cast<float>(visual.span.tail_y);
#endif
            const float top = std::min(y, tail_y);
            const float height = std::abs(tail_y - y);
            // PULSEFORGE_P1_5_0C_SUSTAIN_VISUAL_INHERITANCE_V1
            // `inheritsType=false` means the source head keeps its custom
            // definition while the visual tail returns to the normal note skin,
            // tint, alpha and scale. This remains a per-kind decision.
            auto sustain_color = sustain_inherits_type ? color : base_color;
            sustain_color.a = static_cast<std::uint8_t>(sustain_color.a * 0.55F);
            const auto sustain_rgb = sustain_inherits_type
                ? note_rgb
                : std::array<std::uint8_t, 3U>{255U, 255U, 255U};
            const float sustain_scale = sustain_inherits_type ? note_scale : 1.0F;
            const auto sustain_profile = sustain_inherits_type
                ? note_skin_profile
                : detail::runtime_note_skin_default_profile;
            const bool sustain_skinnable = sustain_inherits_type
                ? skinnable_note
                : true;
            const float sustain_width = std::max(1.0F,
                lane_width * 0.22F * sustain_scale
            );
            const SDL_FRect sustain_rectangle{
                x + lane_width * 0.5F - sustain_width * 0.5F,
                top,
                sustain_width,
                std::max(2.0F, height),
            };
            if (!sustain_skinnable
                || !queue_runtime_note_skin(
                    detail::RuntimeNoteSkinElement::sustain_body,
                    lane,
                    sustain_rectangle,
                    sustain_color.a,
                    false,
                    note_angle,
                    sustain_profile,
                    sustain_rgb
                )) {
                fill_rect(renderer_, sustain_rectangle, sustain_color);
            }
            const float end_height = std::clamp(
                lane_width * 0.18F * sustain_scale,
                4.0F,
                18.0F * sustain_scale * static_cast<float>(
#if defined(PULSEFORGE_HAS_LUA)
                    std::max(1.0, script_owner_camera_zoom(note.owner))
#else
                    1.0
#endif
                )
            );
            const SDL_FRect sustain_end{
                sustain_rectangle.x,
                tail_y - end_height * 0.5F,
                sustain_rectangle.w,
                end_height,
            };
            if (!sustain_skinnable
                || !queue_runtime_note_skin(
                    detail::RuntimeNoteSkinElement::sustain_end,
                    lane,
                    sustain_end,
                    sustain_color.a,
                    gameplay_settings().downscroll,
                    note_angle,
                    sustain_profile,
                    sustain_rgb
                )) {
                fill_rect(renderer_, sustain_end, sustain_color);
            }
        }

        if (visual.include_head) {
            const float head_height = 38.0F * note_scale * static_cast<float>(
#if defined(PULSEFORGE_HAS_LUA)
                script_owner_camera_zoom(note.owner)
#else
                1.0
#endif
            );
            const float head_width = std::max(
                1.0F, (lane_width - 16.0F) * note_scale
            );
            const SDL_FRect body{
                x + lane_width * 0.5F - head_width * 0.5F,
                y - head_height * 0.5F,
                head_width,
                std::max(1.0F, head_height),
            };
            if (!skinnable_note
                || !queue_runtime_note_skin(
                    detail::RuntimeNoteSkinElement::note_head,
                    lane,
                    body,
                    color.a,
                    false,
                    note_angle,
                    note_skin_profile,
                    note_rgb
                )) {
                fill_rect(renderer_, body, color);
                auto highlight = color;
                highlight.r = static_cast<std::uint8_t>(std::min(255, color.r + 40));
                highlight.g = static_cast<std::uint8_t>(std::min(255, color.g + 40));
                highlight.b = static_cast<std::uint8_t>(std::min(255, color.b + 40));
                highlight.a = color.a;
                if (body.w > 8.0F && body.h > 8.0F) {
                    fill_rect(
                        renderer_,
                        {body.x + 4.0F, body.y + 4.0F, body.w - 8.0F,
                         std::min(5.0F, body.h - 4.0F)},
                        highlight
                    );
                }
            }
        }
    }

    void draw_streaming_note(
        const StreamingWindowNote& window,
        const NoteOwner owner,
        const NoteVisualSpan& visual,
        const std::string_view kind
    ) {
        if (window.note.lane >= active_key_count(owner)) {
            return;
        }
        const auto lane = window.display_lane;
        const auto [base_x, logical_lane_width] = lane_layout(owner);
#if defined(PULSEFORGE_HAS_LUA)
        if (!script_lane_visible(owner, lane)) {
            return;
        }
        const float x = script_lane_x(owner, lane, base_x, logical_lane_width);
        const float lane_width = script_lane_width(owner, logical_lane_width);
        const float y = static_cast<float>(
            script_note_y(owner, lane, visual.span.head_y)
        );
        const double script_alpha = script_lane_alpha(owner, lane);
        const double note_angle = script_lane_angle(owner, lane);
#else
        const float x = base_x + static_cast<float>(lane) * logical_lane_width;
        const float lane_width = logical_lane_width;
        const float y = static_cast<float>(visual.span.head_y);
        constexpr double script_alpha = 1.0;
        constexpr double note_angle = 0.0;
#endif
        bool custom_note_texture_missing{};
        const auto note_skin_profile = script_note_skin_profile(
            owner, kind, custom_note_texture_missing
        );
        const double type_alpha = script_note_visual_alpha(owner, kind);
        const double effective_note_alpha = std::clamp(
            script_alpha * type_alpha, 0.0, 1.0
        );
        const auto note_rgb = script_note_visual_rgb(owner, kind);
        const float note_scale = static_cast<float>(
            script_note_visual_scale(owner, kind)
        );
        const double duration = static_cast<double>(window.note.duration_us)
            / 1'000.0;
        const bool has_sustain = runtime_sustain_enabled(kind, duration);
        const bool sustain_inherits_type = runtime_sustain_inherits_type(kind);
        ++visual_draw_units_;
        auto base_color = lane_color(lane);
        base_color.a = static_cast<std::uint8_t>(std::clamp(
            static_cast<double>(ai_note_owner(owner) ? 145U : base_color.a)
                * std::clamp(script_alpha, 0.0, 1.0),
            0.0,
            255.0
        ));
        auto color = base_color;
        color.a = static_cast<std::uint8_t>(std::clamp(
            static_cast<double>(color.a) * type_alpha, 0.0, 255.0
        ));
        const bool hurt = hurt_note_kind(kind);
        // PULSEFORGE_P1_5_0D_HURT_NOTE_SKIN_STREAMING_V1
        const bool skinnable_note = !custom_note_texture_missing;
        if (hurt && custom_note_texture_missing) {
            color = {255, 56, 72, static_cast<std::uint8_t>(std::clamp(
                230.0 * effective_note_alpha, 0.0, 255.0
            ))};
        }
        color.r = static_cast<std::uint8_t>(
            static_cast<unsigned>(color.r) * note_rgb[0] / 255U
        );
        color.g = static_cast<std::uint8_t>(
            static_cast<unsigned>(color.g) * note_rgb[1] / 255U
        );
        color.b = static_cast<std::uint8_t>(
            static_cast<unsigned>(color.b) * note_rgb[2] / 255U
        );
        if (has_sustain) {
#if defined(PULSEFORGE_HAS_LUA)
            const float tail_y = static_cast<float>(
                script_note_y(owner, lane, visual.span.tail_y)
            );
#else
            const float tail_y = static_cast<float>(visual.span.tail_y);
#endif
            const float top = std::min(y, tail_y);
            const float height = std::abs(tail_y - y);
            auto sustain_color = sustain_inherits_type ? color : base_color;
            sustain_color.a = static_cast<std::uint8_t>(sustain_color.a * 0.55F);
            const auto sustain_rgb = sustain_inherits_type
                ? note_rgb
                : std::array<std::uint8_t, 3U>{255U, 255U, 255U};
            const float sustain_scale = sustain_inherits_type ? note_scale : 1.0F;
            const auto sustain_profile = sustain_inherits_type
                ? note_skin_profile
                : detail::runtime_note_skin_default_profile;
            const bool sustain_skinnable = sustain_inherits_type
                ? skinnable_note
                : true;
            const float sustain_width = std::max(1.0F,
                lane_width * 0.22F * sustain_scale
            );
            const SDL_FRect sustain_rectangle{
                x + lane_width * 0.5F - sustain_width * 0.5F,
                top,
                sustain_width,
                std::max(2.0F, height),
            };
            if (!sustain_skinnable
                || !queue_runtime_note_skin(
                    detail::RuntimeNoteSkinElement::sustain_body,
                    lane,
                    sustain_rectangle,
                    sustain_color.a,
                    false,
                    note_angle,
                    sustain_profile,
                    sustain_rgb
                )) {
                fill_rect(renderer_, sustain_rectangle, sustain_color);
            }
            const float end_height = std::clamp(
                lane_width * 0.18F * sustain_scale,
                4.0F,
                18.0F * sustain_scale * static_cast<float>(
#if defined(PULSEFORGE_HAS_LUA)
                    std::max(1.0, script_owner_camera_zoom(owner))
#else
                    1.0
#endif
                )
            );
            const SDL_FRect sustain_end{
                sustain_rectangle.x,
                tail_y - end_height * 0.5F,
                sustain_rectangle.w,
                end_height,
            };
            if (!sustain_skinnable
                || !queue_runtime_note_skin(
                    detail::RuntimeNoteSkinElement::sustain_end,
                    lane,
                    sustain_end,
                    sustain_color.a,
                    gameplay_settings().downscroll,
                    note_angle,
                    sustain_profile,
                    sustain_rgb
                )) {
                fill_rect(renderer_, sustain_end, sustain_color);
            }
        }
        if (visual.include_head) {
            const float head_height = 38.0F * note_scale * static_cast<float>(
#if defined(PULSEFORGE_HAS_LUA)
                script_owner_camera_zoom(owner)
#else
                1.0
#endif
            );
            const float head_width = std::max(
                1.0F, (lane_width - 16.0F) * note_scale
            );
            const SDL_FRect body{
                x + lane_width * 0.5F - head_width * 0.5F,
                y - head_height * 0.5F,
                head_width,
                std::max(1.0F, head_height),
            };
            if (!skinnable_note
                || !queue_runtime_note_skin(
                    detail::RuntimeNoteSkinElement::note_head,
                    lane,
                    body,
                    color.a,
                    false,
                    note_angle,
                    note_skin_profile,
                    note_rgb
                )) {
                fill_rect(renderer_, body, color);
                auto highlight = color;
                highlight.r = static_cast<std::uint8_t>(std::min(255, color.r + 40));
                highlight.g = static_cast<std::uint8_t>(std::min(255, color.g + 40));
                highlight.b = static_cast<std::uint8_t>(std::min(255, color.b + 40));
                highlight.a = color.a;
                if (body.w > 8.0F && body.h > 8.0F) {
                    fill_rect(
                        renderer_,
                        {body.x + 4.0F, body.y + 4.0F, body.w - 8.0F,
                         std::min(5.0F, body.h - 4.0F)},
                        highlight
                    );
                }
            }
        }
    }

    void render_particles() {
        if (scene_ != nullptr) {
            for (const auto& splash : note_splash_animations_) {
                if (!splash.active || splash.duration <= 0.0F) continue;
                const auto frame_count = scene_->note_splash_frame_count(
                    splash.profile
                );
                if (frame_count == 0U) continue;
                const float progress = std::clamp(
                    splash.elapsed / splash.duration, 0.0F, 0.999999F
                );
                const auto frame = static_cast<std::uint32_t>(
                    progress * static_cast<float>(frame_count)
                );
                double x = splash.center_x;
                double y = splash.center_y;
                double scale = 1.0;
                double camera_alpha = 1.0;
#if defined(PULSEFORGE_HAS_LUA)
                x = script_camera_transform_x(x);
                y = script_camera_transform_y(y);
                scale = script_cam_hud_zoom_;
                camera_alpha = script_cam_hud_alpha_;
#endif
                const float size = static_cast<float>(
                    std::clamp(110.0 * scale, 16.0, 512.0)
                );
                const auto alpha = static_cast<std::uint8_t>(std::clamp(
                    (1.0 - static_cast<double>(progress))
                        * 255.0 * camera_alpha,
                    0.0, 255.0
                ));
                static_cast<void>(scene_->render_note_splash(
                    detail::RuntimeNoteSplashDraw{
                        splash.profile,
                        frame,
                        detail::RuntimeLogicalRect{
                            static_cast<float>(x) - size * 0.5F,
                            static_cast<float>(y) - size * 0.5F,
                            size,
                            size,
                        },
                        alpha,
                        0.0,
                    }
                ));
            }
        }
        for (const auto& particle : particles_) {
            if (!particle.active) {
                continue;
            }
            auto color = particle.color;
            double x = particle.x;
            double y = particle.y;
            double scale = 1.0;
            double camera_alpha = 1.0;
#if defined(PULSEFORGE_HAS_LUA)
            x = script_camera_transform_x(x);
            y = script_camera_transform_y(y);
            scale = script_cam_hud_zoom_;
            camera_alpha = script_cam_hud_alpha_;
#endif
            color.a = static_cast<std::uint8_t>(std::clamp(
                static_cast<double>(particle.life / 0.36F) * 255.0
                    * camera_alpha,
                0.0,
                255.0
            ));
            const float radius = static_cast<float>(std::max(1.0, 3.0 * scale));
            fill_rect(
                renderer_,
                {
                    static_cast<float>(x) - radius,
                    static_cast<float>(y) - radius,
                    radius * 2.0F,
                    radius * 2.0F,
                },
                color
            );
        }
    }

    void render_hud(const double song_time) {
        const auto& score = gameplay_summary();
        auto displayed_score = score.score;
        auto displayed_health = score.health;
#if defined(PULSEFORGE_HAS_LUA)
        if (!streaming_mode() && session_ != nullptr) {
            displayed_score = script_state_.effective_score(*session_);
            displayed_health = script_state_.effective_health(*session_);
        }
#endif
#if defined(PULSEFORGE_HAS_LUA)
        const auto hud_state = [&](const std::string_view tag)
            -> const ScriptHudObjectState* {
            const auto found = script_hud_objects_.find(tag);
            return found == script_hud_objects_.end()
                ? nullptr
                : std::addressof(found->second);
        };
        const auto hud_visible = [&](const std::string_view tag) noexcept {
            const auto* state = hud_state(tag);
            return (state == nullptr || state->visible)
                && script_cam_hud_alpha_ > 0.0001;
        };
        const auto hud_alpha = [&](
            const std::string_view tag,
            const std::uint8_t base
        ) noexcept {
            const auto* state = hud_state(tag);
            const double object_alpha = state == nullptr ? 1.0 : state->alpha;
            return static_cast<std::uint8_t>(std::clamp(
                static_cast<double>(base) * object_alpha * script_cam_hud_alpha_,
                0.0,
                255.0
            ));
        };
        const auto hud_rect = [&](
            const std::string_view tag,
            const SDL_FRect fallback
        ) noexcept {
            const auto* state = hud_state(tag);
            const double x = state == nullptr ? fallback.x : state->x;
            const double y = state == nullptr ? fallback.y : state->y;
            const double sx = state == nullptr ? 1.0 : state->scale_x;
            const double sy = state == nullptr ? 1.0 : state->scale_y;
            return SDL_FRect{
                static_cast<float>(script_camera_transform_x(x)),
                static_cast<float>(script_camera_transform_y(y)),
                static_cast<float>(std::max(
                    0.0,
                    static_cast<double>(fallback.w) * sx * script_cam_hud_zoom_
                )),
                static_cast<float>(std::max(
                    0.0,
                    static_cast<double>(fallback.h) * sy * script_cam_hud_zoom_
                )),
            };
        };
        const auto hud_point = [&](
            const std::string_view tag,
            const float fallback_x,
            const float fallback_y
        ) noexcept {
            const auto* state = hud_state(tag);
            const double x = state == nullptr ? fallback_x : state->x;
            const double y = state == nullptr ? fallback_y : state->y;
            return std::pair{
                static_cast<float>(script_camera_transform_x(x)),
                static_cast<float>(script_camera_transform_y(y)),
            };
        };
#else
        const auto hud_visible = [](const std::string_view) noexcept { return true; };
        const auto hud_alpha = [](const std::string_view, const std::uint8_t base) noexcept {
            return base;
        };
        const auto hud_rect = [](const std::string_view, const SDL_FRect fallback) noexcept {
            return fallback;
        };
        const auto hud_point = [](const std::string_view, const float x, const float y) noexcept {
            return std::pair{x, y};
        };
#endif
#if defined(PULSEFORGE_HAS_LUA)
        const auto health_opponent_color = script_health_opponent_color_;
        const auto health_player_color = script_health_player_color_;
        const auto time_background_color = script_time_background_color_;
        const auto time_fill_color = script_time_fill_color_;
#else
        const SDL_Color health_opponent_color{10U, 12U, 24U, 255U};
        const SDL_Color health_player_color{70U, 240U, 156U, 255U};
        const SDL_Color time_background_color{20U, 24U, 42U, 255U};
        const SDL_Color time_fill_color{90U, 210U, 255U, 255U};
#endif
        if (hud_visible("healthBarBG")) {
            fill_rect(
                renderer_,
                hud_rect("healthBarBG", {250.0F, 18.0F, 780.0F, 16.0F}),
                {health_opponent_color.r, health_opponent_color.g,
                 health_opponent_color.b, hud_alpha("healthBarBG", 210)}
            );
        }
        const float health_ratio = static_cast<float>(
            std::clamp(displayed_health / 2.0, 0.0, 1.0)
        );
        if (hud_visible("healthBar")) {
            fill_rect(
                renderer_,
                hud_rect(
                    "healthBar",
                    {252.0F, 20.0F, 776.0F * health_ratio, 12.0F}
                ),
                {health_player_color.r, health_player_color.g,
                 health_player_color.b, hud_alpha("healthBar", 235)}
            );
        }

        char line[256]{};
        std::snprintf(
            line,
            sizeof(line),
            "Score %lld  Combo %llu  Acc %.2f%%  Miss %llu  %s",
            static_cast<long long>(displayed_score),
            static_cast<unsigned long long>(score.combo),
            score.accuracy_percent(),
            static_cast<unsigned long long>(score.misses),
            score.clear_type().data()
        );
        if (hud_visible("scoreTxt")) {
            const auto [score_x, score_y] = hud_point("scoreTxt", 372.0F, 44.0F);
            debug_text(renderer_, score_x, score_y, line);
        }

        const double duration = std::max(1.0, audio_.duration_ms());
        const float progress = static_cast<float>(
            std::clamp(song_time / duration, 0.0, 1.0)
        );
        if (hud_visible("timeBarBG")) {
            fill_rect(
                renderer_,
                hud_rect("timeBarBG", {310.0F, 66.0F, 660.0F, 5.0F}),
                {time_background_color.r, time_background_color.g,
                 time_background_color.b, hud_alpha("timeBarBG", 220)}
            );
        }
        if (hud_visible("timeBar")) {
            fill_rect(
                renderer_,
                hud_rect("timeBar", {310.0F, 66.0F, 660.0F * progress, 5.0F}),
                {time_fill_color.r, time_fill_color.g,
                 time_fill_color.b, hud_alpha("timeBar", 240)}
            );
        }
        if (hud_visible("songName")) {
            const auto [title_x, title_y] = hud_point("songName", 16.0F, 16.0F);
            debug_text(renderer_, title_x, title_y, chart_->title);
        }

#if defined(PULSEFORGE_HAS_LUA)
        // PULSEFORGE_P1_1_8_PSYCH_LUA_TEXT_LAYOUT_V1
        // makeLuaText width is a FlxText field width, not just an alignment
        // hint. Keep requested vertical size, use a half-em horizontal fallback
        // for SDL's 8x8 debug font, and wrap inside the field.
        for (const auto& entry : script_hud_objects_) {
            const auto& state = entry.second;
            if (!state.text_object || !state.text_added || !state.visible
                || state.text.empty()) {
                continue;
            }

            const double camera_zoom = state.camera_other
                ? 1.0
                : script_cam_hud_zoom_;
            const auto metrics = detail::psych_debug_text_metrics(
                state.text_size,
                state.scale_x,
                state.scale_y,
                camera_zoom
            );

            const double base_x = state.camera_other
                ? state.x
                : script_camera_transform_x(state.x);
            const double base_y = state.camera_other
                ? state.y
                : script_camera_transform_y(state.y);
            const double field_width = state.text_width > 0.0
                ? state.text_width * camera_zoom
                : 0.0;

            const auto layout = detail::layout_psych_debug_text(
                state.text,
                field_width,
                metrics.glyph_advance_px,
                state.text_alignment
            );

            const double camera_alpha = state.camera_other
                ? 1.0
                : script_cam_hud_alpha_;
            const auto alpha = static_cast<std::uint8_t>(std::lround(
                std::clamp(state.alpha * camera_alpha, 0.0, 1.0) * 255.0
            ));

            float previous_scale_x{1.0F};
            float previous_scale_y{1.0F};
            static_cast<void>(SDL_GetRenderScale(
                renderer_, &previous_scale_x, &previous_scale_y
            ));

            const float scale_x = static_cast<float>(metrics.scale_x);
            const float scale_y = static_cast<float>(metrics.scale_y);
            static_cast<void>(SDL_SetRenderScale(renderer_, scale_x, scale_y));

            const auto draw_text_pass = [&](
                const SDL_Color color,
                const double offset_x,
                const double offset_y
            ) {
                set_draw_color(
                    renderer_,
                    {color.r, color.g, color.b, static_cast<std::uint8_t>(
                        (static_cast<std::uint16_t>(color.a)
                            * static_cast<std::uint16_t>(alpha)) / 255U
                    )}
                );
                for (std::size_t line_index = 0U;
                     line_index < layout.lines.size();
                     ++line_index) {
                    const auto& visual_line = layout.lines[line_index];
                    double x = base_x + visual_line.x_offset_px + offset_x;
                    const double y = base_y
                        + static_cast<double>(line_index) * metrics.line_height_px
                        + offset_y;
                    if (state.italic) {
                        // Bitmap fallback: a small line-dependent shear keeps
                        // setTextItalic visible without pretending to load TTF.
                        x += static_cast<double>(
                            layout.lines.size() - line_index
                        ) * 0.35;
                    }
                    debug_text(
                        renderer_,
                        static_cast<float>(x) / scale_x,
                        static_cast<float>(y) / scale_y,
                        visual_line.text
                    );
                }
            };

            const double border = std::clamp(state.border_size, 0.0, 8.0);
            if (border > 0.0 && state.border_color.a != 0U) {
                constexpr std::array<std::pair<double, double>, 8U> offsets{{
                    {-1.0,  0.0}, {1.0,  0.0}, {0.0, -1.0}, {0.0, 1.0},
                    {-1.0, -1.0}, {1.0, -1.0}, {-1.0, 1.0}, {1.0, 1.0},
                }};
                for (const auto [dx, dy] : offsets) {
                    draw_text_pass(
                        state.border_color,
                        dx * border,
                        dy * border
                    );
                }
            }
            draw_text_pass(state.text_color, 0.0, 0.0);

            static_cast<void>(SDL_SetRenderScale(
                renderer_, previous_scale_x, previous_scale_y
            ));
        }
#endif

        if (options_.settings.visual.show_timing_graph && timing_history_count_ > 1) {
            render_timing_graph();
        }
        // Permanent runtime telemetry. These three lines are intentionally
        // part of the normal HUD rather than F3 diagnostics: they describe the
        // currently running chart/renderer and are useful to players, chart
        // authors, benchmarkers and offline-render viewers alike.
        std::snprintf(
            line,
            sizeof(line),
            "%.1f FPS | %.2f ms | onscreen logical %llu | chart total %llu | audio %s %.2fx",
            smoothed_fps_,
            smoothed_frame_ms_,
            static_cast<unsigned long long>(rendered_notes_),
            static_cast<unsigned long long>(
                streaming_mode()
                    ? streaming_session_->summary().chart_total
                    : session_->summary().chart_total
            ),
            audio_.using_silent_audio() ? "silent" : "decoded",
            audio_.playback_rate()
        );
        debug_text(renderer_, 16.0F, logical_height - 24.0F, line);

        std::snprintf(
            line,
            sizeof(line),
            "NPS %llu | draw units %llu/%llu calls | PVD %llu | exact visits %llu | drops %llu",
            static_cast<unsigned long long>(current_nps_),
            static_cast<unsigned long long>(visual_draw_units_),
            static_cast<unsigned long long>(visual_geometry_calls_),
            static_cast<unsigned long long>(streaming_density_buckets_),
            static_cast<unsigned long long>(streaming_explicit_visual_visits_),
            static_cast<unsigned long long>(dropped_gameplay_callbacks())
        );
        debug_text(renderer_, 16.0F, logical_height - 40.0F, line);

        std::snprintf(
            line,
            sizeof(line),
            "song %.3f ms | beat %.3f | bpm %.3f | %s %s | Lua %s",
            song_time,
            gameplay_timing().beat_at(song_time),
            gameplay_timing().bpm_at(song_time),
            to_string(chart_->source_format).data(),
            streaming_mode() ? "PFC1-stream" : "materialized",
#if defined(PULSEFORGE_HAS_LUA)
            scripts_ != nullptr && scripts_->loaded_count() != 0
                ? "on"
                : "off"
#else
            "not built"
#endif
        );
        debug_text(renderer_, 16.0F, logical_height - 56.0F, line);

        // F3 remains reserved for the detailed profiling/diagnostic extension
        // below the permanent three-line telemetry block.
        if (diagnostics_) {
            std::snprintf(
                line,
                sizeof(line),
                "gameplay update %.0f/%.0f/%.0f us | saturated %s",
                note_profile_gameplay_update_.last_us,
                note_profile_gameplay_update_.average_us,
                note_profile_gameplay_update_.peak_us,
                streaming_mode() && streaming_session_->window_saturated()
                    ? "yes"
                    : "no"
            );
            debug_text(renderer_, 16.0F, logical_height - 88.0F, line);
            std::snprintf(
                line,
                sizeof(line),
                "PROF us last/avg/peak1s | note %.0f/%.0f/%.0f | cache %.0f/%.0f/%.0f",
                note_profile_note_.last_us,
                note_profile_note_.average_us,
                note_profile_note_.peak_us,
                note_profile_cache_.last_us,
                note_profile_cache_.average_us,
                note_profile_cache_.peak_us
            );
            debug_text(renderer_, 16.0F, logical_height - 104.0F, line);
            std::snprintf(
                line,
                sizeof(line),
                "PVD %.0f/%.0f/%.0f | PFC %.0f/%.0f/%.0f | geom build %.0f/%.0f/%.0f",
                note_profile_pvd_.last_us,
                note_profile_pvd_.average_us,
                note_profile_pvd_.peak_us,
                note_profile_pfc_.last_us,
                note_profile_pfc_.average_us,
                note_profile_pfc_.peak_us,
                note_profile_batch_build_.last_us,
                note_profile_batch_build_.average_us,
                note_profile_batch_build_.peak_us
            );
            debug_text(renderer_, 16.0F, logical_height - 120.0F, line);
            std::snprintf(
                line,
                sizeof(line),
                "geom submit %.0f/%.0f/%.0f | fallback %.0f us | present/wait %.0f/%.0f/%.0f",
                note_profile_batch_submit_.last_us,
                note_profile_batch_submit_.average_us,
                note_profile_batch_submit_.peak_us,
                note_profile_fallback_.last_us,
                note_profile_present_.last_us,
                note_profile_present_.average_us,
                note_profile_present_.peak_us
            );
            debug_text(renderer_, 16.0F, logical_height - 136.0F, line);
            std::snprintf(
                line,
                sizeof(line),
                "geometry %llu quads/%u submits | failed %u | fallback draws %llu | cache rebuilds/s %u",
                static_cast<unsigned long long>(note_profile_last_quads_),
                note_profile_last_submissions_,
                note_profile_last_failed_submissions_,
                static_cast<unsigned long long>(note_profile_last_fallback_draws_),
                note_profile_rebuilds_last_second_
            );
            debug_text(renderer_, 16.0F, logical_height - 152.0F, line);
            debug_text(
                renderer_,
                16.0F,
                logical_height - 168.0F,
                "F3 profiler | build " PULSEFORGE_PATCH_BUILD
                " | geom submit = CPU/driver submission, not hardware GPU timestamp"
            );
            if (streaming_mode()) {
                const auto memory = streaming_session_->memory_stats();
                std::snprintf(
                    line,
                    sizeof(line),
                    "window %zu | RAM~%llu KiB | catchup %s | saturated %s",
                    memory.window_notes,
                    static_cast<unsigned long long>(
                        memory.approximate_dynamic_bytes / 1'024U
                    ),
                    streaming_session_->catchup_pending() ? "yes" : "no",
                    streaming_session_->window_saturated() ? "yes" : "no"
                );
                debug_text(
                    renderer_,
                    16.0F,
                    logical_height - 184.0F,
                    line
                );
            } else {
                debug_text(
                    renderer_,
                    16.0F,
                    logical_height - 184.0F,
                    "F2 botplay | F3 profiler | +/- volume | F5 scripts | F11 fullscreen"
                );
            }
        }
        if (gameplay_settings().autoplay) {
            debug_text(renderer_, logical_width * 0.5F - 32.0F, 86.0F, "BOTPLAY");
        }
    }

    void render_audio_visualizer(const double song_time) {
        constexpr float panel_width = 410.0F;
        constexpr float panel_height = 88.0F;
        constexpr float right_margin = 24.0F;
        constexpr float top_margin = 92.0F;
        constexpr float bottom_margin = 24.0F;
        constexpr float icon_size = 58.0F;
        constexpr float icon_margin = 15.0F;
        constexpr float divider_x_offset = 88.0F;
        constexpr float bars_left_offset = 101.0F;
        constexpr float bars_right_padding = 16.0F;
        constexpr float bar_gap = 3.0F;

        const float x = logical_width - panel_width - right_margin;
        const float y = gameplay_settings().downscroll
            ? top_margin
            : logical_height - panel_height - bottom_margin;

        const auto background_alpha = static_cast<std::uint8_t>(
            std::clamp(
                options_.settings.visual
                    .audio_visualizer_background_opacity,
                0.0F,
                1.0F
            ) * 255.0F
        );
        fill_rect(
            renderer_,
            {x, y, panel_width, panel_height},
            {0, 0, 0, background_alpha}
        );
        outline_rect(
            renderer_,
            {x, y, panel_width, panel_height},
            {230, 235, 245, 150}
        );

        set_draw_color(renderer_, {230, 235, 245, 105});
        SDL_RenderLine(
            renderer_,
            x + divider_x_offset,
            y + 12.0F,
            x + divider_x_offset,
            y + panel_height - 12.0F
        );

        const float icon_center_x = x + icon_margin + icon_size * 0.5F;
        const float icon_center_y = y + panel_height * 0.5F;

        // Pseudo-3D yaw: the star behaves like a rigid object hanging from a
        // thread and rotating around its own vertical axis. The cycle is
        // exactly 5.0 seconds of chart time and loops until the chart ends.
        constexpr double star_rotation_period_ms = 5'000.0;
        constexpr double two_pi = 6.283185307179586476925286766559;
        const double wrapped_time_ms = std::fmod(
            std::max(song_time, 0.0),
            star_rotation_period_ms
        );
        const double phase = wrapped_time_ms / star_rotation_period_ms;
        const double yaw = phase * two_pi;
        const float cos_yaw = static_cast<float>(std::cos(yaw));
        const float sin_yaw = static_cast<float>(std::sin(yaw));
        const float facing = std::abs(cos_yaw);

        // A plain horizontal squash reads too flat, so add a perspective warp:
        // - foreshortening from cos(yaw)
        // - side-to-side shear from sin(yaw)
        // - a gentle vertical bow so the silhouette feels like a 3D card/object
        // - per-strip perspective variation, which makes the top/bottom recede
        //   differently instead of moving as one flat rectangle.
        constexpr float minimum_face_width = 2.0F;
        constexpr int warp_slices = 20;
        constexpr float perspective_strength = 0.22F;
        constexpr float shear_strength = 0.16F;
        constexpr float bow_strength = 0.075F;

        if (audio_visualizer_icon_ != nullptr) {
            SDL_SetTextureAlphaMod(audio_visualizer_icon_, 235);

            float texture_width = 0.0F;
            float texture_height = 0.0F;
            static_cast<void>(SDL_GetTextureSize(
                audio_visualizer_icon_,
                &texture_width,
                &texture_height
            ));

            if (texture_width > 0.0F && texture_height > 0.0F) {
                const SDL_FlipMode flip = cos_yaw < 0.0F
                    ? SDL_FLIP_HORIZONTAL
                    : SDL_FLIP_NONE;

                for (int slice = 0; slice < warp_slices; ++slice) {
                    const float v0 =
                        static_cast<float>(slice) / warp_slices;
                    const float v1 =
                        static_cast<float>(slice + 1) / warp_slices;
                    const float vm = (v0 + v1) * 0.5F;
                    const float centered_v = vm * 2.0F - 1.0F;

                    // Near one vertical edge, strips become slightly narrower
                    // and offset, mimicking perspective depth during yaw.
                    const float depth_bias =
                        1.0F - perspective_strength
                            * std::abs(sin_yaw)
                            * centered_v;
                    const float local_width = std::max(
                        minimum_face_width,
                        icon_size * facing * depth_bias
                    );

                    // Sideways shear is strongest edge-on and changes sign
                    // naturally as the object rotates through the back face.
                    const float shear =
                        sin_yaw
                        * shear_strength
                        * icon_size
                        * centered_v;

                    // Slight nonlinear vertical warp: middle stays close to
                    // center while top/bottom bend in opposite directions.
                    const float bow =
                        sin_yaw
                        * bow_strength
                        * icon_size
                        * centered_v
                        * std::abs(centered_v);

                    const float slice_height =
                        icon_size / static_cast<float>(warp_slices) + 0.75F;
                    const float destination_y =
                        icon_center_y - icon_size * 0.5F
                        + v0 * icon_size
                        + bow;

                    const SDL_FRect source{
                        0.0F,
                        v0 * texture_height,
                        texture_width,
                        std::max(
                            1.0F,
                            (v1 - v0) * texture_height
                        ),
                    };
                    const SDL_FRect destination{
                        icon_center_x - local_width * 0.5F + shear,
                        destination_y,
                        local_width,
                        slice_height,
                    };

                    static_cast<void>(SDL_RenderTextureRotated(
                        renderer_,
                        audio_visualizer_icon_,
                        &source,
                        &destination,
                        0.0,
                        nullptr,
                        flip
                    ));
                }
            }
        } else {
            // Vector fallback: approximate the same pseudo-3D feeling with a
            // yaw squash plus shear. It cannot match the sliced texture warp,
            // but it still reads as a turning 3D object rather than a flat spin.
            const float fallback_scale = std::copysign(
                std::max(minimum_face_width / icon_size, facing),
                cos_yaw
            );
            const float fallback_x =
                icon_center_x + sin_yaw * shear_strength * icon_size * 0.25F;
            draw_star_of_david_fallback(
                renderer_,
                fallback_x,
                icon_center_y,
                icon_size * 0.38F,
                fallback_scale,
                {245, 245, 248, 235}
            );
        }

        const auto levels = audio_.visualizer_levels();
        const float bars_width =
            panel_width - bars_left_offset - bars_right_padding;
        const float bar_width = std::max(
            2.0F,
            (bars_width - bar_gap
                * static_cast<float>(levels.size() - 1U))
                / static_cast<float>(levels.size())
        );
        const float center_y = y + panel_height * 0.5F;
        const float maximum_half_height = panel_height * 0.35F;

        for (std::size_t index = 0U; index < levels.size(); ++index) {
            float level = levels[index];
            if (index > 0U) {
                level += levels[index - 1U] * 0.22F;
            }
            if (index + 1U < levels.size()) {
                level += levels[index + 1U] * 0.22F;
            }
            level /= index > 0U && index + 1U < levels.size()
                ? 1.44F
                : 1.22F;

            // A gentle high-index taper matches the compact visual language
            // of the reference while the actual motion still comes from the
            // real mixed audio callback.
            const float normalized_index = levels.size() <= 1U
                ? 0.0F
                : static_cast<float>(index)
                    / static_cast<float>(levels.size() - 1U);
            const float taper = 1.0F - (normalized_index * 0.55F);
            const float half_height = std::max(
                1.2F,
                std::clamp(level * taper, 0.0F, 1.0F)
                    * maximum_half_height
            );
            const float bar_x = x + bars_left_offset
                + static_cast<float>(index) * (bar_width + bar_gap);
            fill_rect(
                renderer_,
                {
                    bar_x,
                    center_y - half_height,
                    bar_width,
                    half_height * 2.0F,
                },
                {245, 245, 248, 235}
            );
        }
    }

    void render_timing_graph() {
        constexpr float graph_x = 450.0F;
        constexpr float graph_y = 652.0F;
        constexpr float graph_width = 380.0F;
        constexpr float graph_height = 48.0F;
        fill_rect(
            renderer_,
            {graph_x, graph_y, graph_width, graph_height},
            {5, 7, 17, 170}
        );
        set_draw_color(renderer_, {120, 130, 160, 100});
        SDL_RenderLine(
            renderer_,
            graph_x,
            graph_y + graph_height * 0.5F,
            graph_x + graph_width,
            graph_y + graph_height * 0.5F
        );
        const std::size_t count = timing_history_count_;
        for (std::size_t index = 1; index < count; ++index) {
            const auto previous_slot =
                (timing_history_cursor_ + timing_history_size - count + index - 1)
                % timing_history_size;
            const auto current_slot =
                (timing_history_cursor_ + timing_history_size - count + index)
                % timing_history_size;
            const float previous_x = graph_x
                + static_cast<float>(index - 1) / static_cast<float>(count - 1)
                    * graph_width;
            const float current_x = graph_x
                + static_cast<float>(index) / static_cast<float>(count - 1)
                    * graph_width;
            const float previous_y = graph_y + graph_height * 0.5F
                + timing_history_[previous_slot] / 180.0F * graph_height * 0.5F;
            const float current_y = graph_y + graph_height * 0.5F
                + timing_history_[current_slot] / 180.0F * graph_height * 0.5F;
            set_draw_color(
                renderer_,
                timing_history_[current_slot] >= 0.0F
                    ? SDL_Color{255, 128, 116, 220}
                    : SDL_Color{80, 220, 255, 220}
            );
            SDL_RenderLine(renderer_, previous_x, previous_y, current_x, current_y);
        }
    }

    void render_center_panel(
        const std::string_view title,
        const std::string_view subtitle
    ) {
        fill_rect(renderer_, {350.0F, 270.0F, 580.0F, 180.0F}, {4, 6, 18, 235});
        outline_rect(renderer_, {350.0F, 270.0F, 580.0F, 180.0F}, {90, 220, 255, 230});
        debug_text(
            renderer_,
            logical_width * 0.5F - static_cast<float>(title.size()) * 4.0F,
            326.0F,
            title
        );
        debug_text(
            renderer_,
            logical_width * 0.5F - static_cast<float>(subtitle.size()) * 4.0F,
            366.0F,
            subtitle
        );
    }

    void render_pause_menu() {
        constexpr SDL_FRect panel{390.0F, 170.0F, 500.0F, 390.0F};
        fill_rect(renderer_, panel, {4, 6, 18, 242});
        outline_rect(renderer_, panel, {90, 220, 255, 235});
        debug_text(renderer_, 616.0F, 204.0F, "PAUSED");
        if (close_request_notice_) {
            debug_text(
                renderer_,
                442.0F,
                228.0F,
                "WINDOW CLOSE PAUSED THE CHART - RETURN TO MENU TO LEAVE"
            );
        } else {
            debug_text(
                renderer_,
                504.0F,
                228.0F,
                "UP/DOWN + ENTER  |  ESC/B TO RESUME"
            );
        }

        for (std::size_t index = 0; index < pause_menu_labels.size(); ++index) {
            const float y = 270.0F + static_cast<float>(index) * 58.0F;
            if (index == pause_menu_selection_) {
                fill_rect(
                    renderer_,
                    {430.0F, y - 14.0F, 420.0F, 38.0F},
                    {28, 120, 158, 230}
                );
                outline_rect(
                    renderer_,
                    {430.0F, y - 14.0F, 420.0F, 38.0F},
                    {118, 238, 255, 255}
                );
            }
            const auto label = pause_menu_labels[index];
            debug_text(
                renderer_,
                logical_width * 0.5F
                    - static_cast<float>(label.size()) * 4.0F,
                y,
                label
            );
        }
    }

    void update_fps(const double elapsed_seconds) noexcept {
        if (elapsed_seconds <= 0.0) {
            return;
        }
        const double frame_ms = elapsed_seconds * 1'000.0;
        const double fps = 1.0 / elapsed_seconds;
        constexpr double smoothing = 0.08;
        if (smoothed_fps_ <= 0.0) {
            smoothed_fps_ = fps;
            smoothed_frame_ms_ = frame_ms;
        } else {
            smoothed_fps_ += (fps - smoothed_fps_) * smoothing;
            smoothed_frame_ms_ += (frame_ms - smoothed_frame_ms_) * smoothing;
        }
    }

    void configure_low_latency_runtime() noexcept {
#if defined(_WIN32)
        if (low_latency_runtime_active_ || (!options_.settings.performance.ultra_low_latency
            && !options_.settings.performance.maximum_performance_mode)) {
            return;
        }
        // PULSEFORGE_P1_5_0E_WINDOWS_LOW_LATENCY_RUNTIME_V1
        // ABOVE_NORMAL + HIGHEST boosts the game without starving audio/driver
        // service threads the way REALTIME/TIME_CRITICAL priorities can.
        previous_process_priority_ = GetPriorityClass(GetCurrentProcess());
        previous_thread_priority_ = GetThreadPriority(GetCurrentThread());
        static_cast<void>(SetPriorityClass(
            GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS
        ));
        static_cast<void>(SetThreadPriority(
            GetCurrentThread(), THREAD_PRIORITY_HIGHEST
        ));

        winmm_module_ = LoadLibraryW(L"winmm.dll");
        if (winmm_module_ != nullptr) {
            using TimerPeriodFunction = unsigned int (WINAPI*)(unsigned int);
            const auto begin = reinterpret_cast<TimerPeriodFunction>(
                GetProcAddress(winmm_module_, "timeBeginPeriod")
            );
            if (begin != nullptr && begin(1U) == 0U) {
                one_ms_timer_period_active_ = true;
            }
        }
        low_latency_runtime_active_ = true;
#endif
    }

    void restore_low_latency_runtime() noexcept {
#if defined(_WIN32)
        if (!low_latency_runtime_active_) {
            return;
        }
        if (winmm_module_ != nullptr) {
            if (one_ms_timer_period_active_) {
                using TimerPeriodFunction = unsigned int (WINAPI*)(unsigned int);
                const auto end = reinterpret_cast<TimerPeriodFunction>(
                    GetProcAddress(winmm_module_, "timeEndPeriod")
                );
                if (end != nullptr) {
                    static_cast<void>(end(1U));
                }
            }
            FreeLibrary(winmm_module_);
            winmm_module_ = nullptr;
        }
        one_ms_timer_period_active_ = false;
        if (previous_thread_priority_ != THREAD_PRIORITY_ERROR_RETURN) {
            static_cast<void>(SetThreadPriority(
                GetCurrentThread(), previous_thread_priority_
            ));
        }
        if (previous_process_priority_ != 0U) {
            static_cast<void>(SetPriorityClass(
                GetCurrentProcess(), previous_process_priority_
            ));
        }
        low_latency_runtime_active_ = false;
#endif
    }

    void limit_frame_rate(const std::uint64_t frame_start_ns) const {
        if (vsync_active_ || options_.settings.visual.fps_cap <= 0) {
            return;
        }
        const auto target_ns = 1'000'000'000ULL
            / static_cast<std::uint64_t>(options_.settings.visual.fps_cap);
        const auto deadline = frame_start_ns + target_ns;
        const bool aggressive = options_.settings.performance.ultra_low_latency
            || options_.settings.performance.maximum_performance_mode;
        // PULSEFORGE_P1_5_0E_ADAPTIVE_FRAME_PACER_V1
        // Sleep for the bulk of the frame, then yield/spin only in the final
        // sub-millisecond window. Aggressive mode deliberately shortens that
        // window to reduce scheduler overshoot and input-to-present latency.
        const auto sleep_guard_ns = aggressive ? 350'000ULL : 750'000ULL;
        const auto yield_guard_ns = aggressive ? 90'000ULL : 250'000ULL;
        auto now = SDL_GetTicksNS();
        if (deadline > now + sleep_guard_ns + 1'000'000ULL) {
            const auto sleep_ms = static_cast<std::uint32_t>(
                (deadline - now - sleep_guard_ns) / 1'000'000ULL
            );
            if (sleep_ms > 0U) {
                SDL_Delay(sleep_ms);
            }
        }
        while ((now = SDL_GetTicksNS()) < deadline) {
            if (deadline - now > yield_guard_ns) {
                SDL_Delay(0);
            }
        }
    }

    void open_existing_gamepads() {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        if (ids == nullptr) {
            return;
        }
        for (int index = 0; index < count; ++index) {
            open_gamepad(ids[index]);
        }
        SDL_free(ids);
    }

    void open_gamepad(const SDL_JoystickID id) {
        if (SDL_Gamepad* gamepad = SDL_OpenGamepad(id); gamepad != nullptr) {
            gamepads_.push_back(gamepad);
        }
    }

    [[nodiscard]] bool close_gamepad(const SDL_JoystickID id) {
        const auto iterator = std::find_if(
            gamepads_.begin(),
            gamepads_.end(),
            [id](SDL_Gamepad* gamepad) {
                return SDL_GetGamepadID(gamepad) == id;
            }
        );
        if (iterator != gamepads_.end()) {
            SDL_CloseGamepad(*iterator);
            gamepads_.erase(iterator);
            return true;
        }
        return false;
    }

    void save_replay_if_requested() {
        if (replay_saved_
            || !options_.save_replay_path.has_value()
            || replay_.has_value()
            || session_ == nullptr) {
            return;
        }
        if (session_->input_recording_overflowed()) {
            std::cerr
                << "Replay save failed: session exceeded the 500000-input "
                   "safety limit\n";
            replay_saved_ = true;
            return;
        }
        const auto generated = make_replay(
            *chart_,
            *session_,
            PULSEFORGE_VERSION
        );
        if (std::string error;
            !save_replay(*options_.save_replay_path, generated, &error)) {
            std::cerr << "Replay save failed: " << error << '\n';
        } else {
            std::cout << "Replay saved: "
                      << options_.save_replay_path->string() << '\n';
            replay_saved_ = true;
        }
    }

#if defined(PULSEFORGE_HAS_LUA)
    [[nodiscard]] static bool script_number_value(
        const ScriptValue& value,
        double& result
    ) noexcept {
        if (const auto* number = std::get_if<double>(&value)) {
            if (!std::isfinite(*number)) return false;
            result = *number;
            return true;
        }
        if (const auto* integer = std::get_if<std::int64_t>(&value)) {
            result = static_cast<double>(*integer);
            return true;
        }
        return false;
    }

    [[nodiscard]] static bool script_integer_value(
        const ScriptValue& value,
        std::int64_t& result
    ) noexcept {
        if (const auto* integer = std::get_if<std::int64_t>(&value)) {
            result = *integer;
            return true;
        }
        if (const auto* number = std::get_if<double>(&value)) {
            if (!std::isfinite(*number)
                || *number < static_cast<double>(std::numeric_limits<std::int64_t>::min())
                || *number > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
                return false;
            }
            result = static_cast<std::int64_t>(*number);
            return true;
        }
        return false;
    }

    [[nodiscard]] static bool script_boolean_value(
        const ScriptValue& value,
        bool& result
    ) noexcept {
        if (const auto* boolean = std::get_if<bool>(&value)) {
            result = *boolean;
            return true;
        }
        if (const auto* integer = std::get_if<std::int64_t>(&value)) {
            result = *integer != 0;
            return true;
        }
        if (const auto* number = std::get_if<double>(&value)) {
            result = std::isfinite(*number) && *number != 0.0;
            return std::isfinite(*number);
        }
        return false;
    }

    [[nodiscard]] static bool script_string_value(
        const ScriptValue& value,
        std::string_view& result
    ) noexcept {
        if (const auto* string = std::get_if<std::string>(&value)) {
            result = *string;
            return true;
        }
        return false;
    }

    [[nodiscard]] static std::int64_t script_floor_to_int64(
        const double value
    ) noexcept {
        if (!std::isfinite(value)) return 0;
        const auto floored = std::floor(value);
        const auto minimum = static_cast<double>(
            std::numeric_limits<std::int64_t>::min()
        );
        const auto maximum = static_cast<double>(
            std::numeric_limits<std::int64_t>::max()
        );
        if (floored <= minimum) return std::numeric_limits<std::int64_t>::min();
        if (floored >= maximum) return std::numeric_limits<std::int64_t>::max();
        return static_cast<std::int64_t>(floored);
    }

    [[nodiscard]] static std::int64_t script_counter(
        const std::uint64_t value
    ) noexcept {
        constexpr auto maximum = static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()
        );
        return value > maximum
            ? std::numeric_limits<std::int64_t>::max()
            : static_cast<std::int64_t>(value);
    }

    void script_set_score(const std::int64_t target) noexcept {
        const auto current = gameplay_summary().score;
        const auto apply = [&](const std::int64_t amount) noexcept {
            if (streaming_mode()) streaming_session_->add_score(amount);
            else session_->add_score(amount);
        };
        // Avoid negating INT64_MIN. This mirrors the core Lua host and keeps
        // the operation defined even if a mod deliberately drives the score
        // to an extreme value.
        if (current == std::numeric_limits<std::int64_t>::min()) {
            apply(std::numeric_limits<std::int64_t>::max());
            apply(1);
        } else {
            apply(-current);
        }
        apply(target);
    }

    [[nodiscard]] bool script_host_get_property(
        const std::string_view name,
        ScriptValue& value,
        std::string& error
    ) const {
        const auto& summary = gameplay_summary();
        const auto& settings = gameplay_settings();

        const auto default_strum_property = [&](
            const std::string_view prefix,
            const bool player,
            const bool y_axis
        ) -> bool {
            if (!name.starts_with(prefix) || name.size() == prefix.size()
                || chart_ == std::nullopt || chart_->key_count == 0U) {
                return false;
            }
            std::size_t lane = 0U;
            for (const char character : name.substr(prefix.size())) {
                if (character < '0' || character > '9') {
                    return false;
                }
                const auto digit = static_cast<std::size_t>(character - '0');
                if (lane > (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
                    return false;
                }
                lane = lane * 10U + digit;
            }
            const auto keys = static_cast<std::size_t>(chart_->key_count);
            const auto active_keys = static_cast<std::size_t>(active_key_count(
                player ? NoteOwner::player : NoteOwner::opponent
            ));
            if (lane >= active_keys) {
                return false;
            }
            const auto global_index = player ? keys + lane : lane;
            value = y_axis
                ? script_base_strum_y()
                : script_base_strum_x(global_index);
            return true;
        };

        if (name == "health") value = summary.health;
        else if (name == "score" || name == "songScore") value = summary.score;
        else if (name == "combo") value = script_counter(summary.combo);
        else if (name == "maxCombo") value = script_counter(summary.max_combo);
        else if (name == "misses" || name == "songMisses") value = script_counter(summary.misses);
        // PULSEFORGE_P1_1_17_EXPANDED_PSYCH_GLOBALS_V1
        else if (name == "hits") {
            value = script_counter(
                summary.marvelous + summary.sick + summary.good + summary.bad
            );
        }
        else if (name == "ratingPercent" || name == "rating") {
            value = summary.accuracy_percent() / 100.0;
        }
        else if (name == "accuracy") value = summary.accuracy_percent();
        else if (name == "ratingFC") value = std::string(summary.clear_type());
        else if (name == "songPosition") value = gameplay_song_time_ms();
        else if (name == "songLength") {
            const double media_duration = audio_.duration_ms();
            value = std::isfinite(media_duration) && media_duration > 0.0
                ? media_duration
                : gameplay_content_duration_ms();
        }
        else if (name == "curBeat") value = script_floor_to_int64(
            gameplay_timing().beat_at(gameplay_song_time_ms())
        );
        else if (name == "curStep") value = script_floor_to_int64(
            gameplay_timing().step_at(gameplay_song_time_ms())
        );
        else if (name == "curSection") value = script_floor_to_int64(
            gameplay_timing().step_at(gameplay_song_time_ms()) / 16.0
        );
        else if (name == "bpm" || name == "curBpm") value =
            gameplay_timing().bpm_at(gameplay_song_time_ms());
        // PULSEFORGE_P1_1_3_UI_INPUT_V1
        else if (name == "screenWidth") value = static_cast<double>(logical_width);
        else if (name == "screenHeight") value = static_cast<double>(logical_height);
        else if (name == "bfHit" || name == "daHit") value = false;
        else if (name == "crochet" || name == "stepCrochet") {
            const double bpm = gameplay_timing().bpm_at(gameplay_song_time_ms());
            const double crochet = std::isfinite(bpm) && bpm > 0.0
                ? 60'000.0 / bpm
                : 500.0;
            value = name == "stepCrochet" ? crochet * 0.25 : crochet;
        }
        else if (default_strum_property("defaultPlayerStrumX", true, false)) {}
        else if (default_strum_property("defaultPlayerStrumY", true, true)) {}
        else if (default_strum_property("defaultOpponentStrumX", false, false)) {}
        else if (default_strum_property("defaultOpponentStrumY", false, true)) {}
        else if (name == "scrollSpeed" || name == "songSpeed") value = settings.scroll_speed;
        else if (name == "inputOffset" || name == "inputOffsetMs") value = settings.input_offset_ms;
        else if (name == "visualOffset" || name == "visualOffsetMs") value = settings.visual_offset_ms;
        else if (name == "botPlay" || name == "cpuControlled" || name == "autoplay") value = settings.autoplay;
        else if (name == "practice" || name == "practiceMode") value = settings.practice;
        else if (name == "ghostTapping") value = settings.ghost_tapping;
        else if (name == "downscroll") value = settings.downscroll;
        else if (name == "middlescroll" || name == "middleScroll") value = settings.middle_scroll;
        else if (name == "noFail") value = settings.no_fail;
        else if (name == "keyCount" || name == "playerKeyCount") {
            value = static_cast<std::int64_t>(active_key_count(NoteOwner::player));
        }
        else if (name == "opponentKeyCount") {
            value = static_cast<std::int64_t>(active_key_count(NoteOwner::opponent));
        }
        else if (name == "thirdKeyCount" || name == "secondaryOpponentKeyCount") {
            value = secondary_strum_enabled()
                ? static_cast<std::int64_t>(
                    active_key_count(NoteOwner::secondary_opponent)
                )
                : std::int64_t{0};
        }
        // PULSEFORGE_P1_1_18_NOTE_MULTIPLIER_PSYCH_GLOBALS_V1
        else if (name == "noteMultiplier"
                 || name == "noteMultiplierPlayer") {
            value = streaming_mode()
                ? streaming_session_->player_note_multiplier()
                : session_->player_note_multiplier();
        }
        else if (name == "noteMultiplierOpponent") {
            value = streaming_mode()
                ? streaming_session_->opponent_note_multiplier()
                : session_->opponent_note_multiplier();
        }
        else if (name == "songName") value = chart_->title;
        else if (name == "difficultyName") value = chart_->difficulty;
        else if (name == "playbackRate") value = audio_.playback_rate();
        else if (name == "startedCountdown") value = true;
        else if (name == "inCutscene") value = false;
        else if (name == "inGameOver") value = gameplay_failed();
        else if (name == "strumLineNotes.length") {
            value = script_counter(
                static_cast<std::uint64_t>(active_key_count(NoteOwner::opponent))
                + static_cast<std::uint64_t>(active_key_count(NoteOwner::player))
                + (secondary_strum_enabled()
                    ? static_cast<std::uint64_t>(active_key_count(
                        NoteOwner::secondary_opponent
                    ))
                    : 0U)
            );
        } else if (name == "playerStrums.length") {
            value = script_counter(active_key_count(NoteOwner::player));
        } else if (name == "opponentStrums.length") {
            value = script_counter(active_key_count(NoteOwner::opponent));
        } else if (name == "thirdStrums.length") {
            value = secondary_strum_enabled()
                ? script_counter(active_key_count(NoteOwner::secondary_opponent))
                : std::int64_t{0};
        } else if (name == "unspawnNotes.length") {
            // A materialized chart exposes each note, like Psych. A PFC1 chart
            // may contain billions/trillions of logical notes, so exposing that
            // literal count to a Lua `for` loop would destroy bounded streaming.
            // Streaming therefore exposes bounded owner+kind prototypes. The
            // group bridge maps writes back to that semantic owner/type pair,
            // preserving mustPress/Third Strum setup loops without expanding
            // the physical chart.
            value = streaming_mode()
                ? script_counter(script_streaming_unspawn_prototypes_.size())
                : script_counter(chart_->notes.size());
        } else if (name == "notes.length") {
            value = streaming_mode()
                ? script_counter(streaming_session_->window_notes().size())
                : script_counter(chart_->notes.size());
        } else if (script_get_visual_property(name, value, error)) return true;
        else return false;
        error.clear();
        return true;
    }

    [[nodiscard]] bool script_host_set_property(
        const std::string_view name,
        const ScriptValue& value,
        std::string& error
    ) {
        double number{};
        std::int64_t integer{};
        bool boolean{};
        if (name == "health") {
            if (!script_number_value(value, number)) {
                error = "health expects a finite number";
                return false;
            }
            return script_host_set_health(number, error);
        }
        if (name == "score" || name == "songScore") {
            if (!script_integer_value(value, integer)) {
                error = "score expects an integer";
                return false;
            }
            script_set_score(integer);
            error.clear();
            return true;
        }
        if (name == "scrollSpeed" || name == "songSpeed") {
            if (!script_number_value(value, number) || number <= 0.0) {
                error = "scrollSpeed expects a finite positive number";
                return false;
            }
            gameplay_settings().scroll_speed = std::clamp(number, 0.01, 100.0);
            streaming_visual_cache_.reset();
            error.clear();
            return true;
        }
        if (name == "inputOffset" || name == "inputOffsetMs") {
            if (!script_number_value(value, number)) {
                error = "input offset expects a finite number";
                return false;
            }
            gameplay_settings().input_offset_ms = std::clamp(number, -1'000.0, 1'000.0);
            error.clear();
            return true;
        }
        if (name == "visualOffset" || name == "visualOffsetMs") {
            if (!script_number_value(value, number)) {
                error = "visual offset expects a finite number";
                return false;
            }
            gameplay_settings().visual_offset_ms = std::clamp(number, -1'000.0, 1'000.0);
            streaming_visual_cache_.reset();
            error.clear();
            return true;
        }
        if (name == "botPlay" || name == "cpuControlled" || name == "autoplay"
            || name == "practice" || name == "practiceMode"
            || name == "ghostTapping" || name == "downscroll"
            || name == "middlescroll" || name == "middleScroll"
            || name == "noFail") {
            if (!script_boolean_value(value, boolean)) {
                error = "gameplay toggle expects a boolean";
                return false;
            }
            if (name == "botPlay" || name == "cpuControlled" || name == "autoplay") gameplay_settings().autoplay = boolean;
            else if (name == "practice" || name == "practiceMode") gameplay_settings().practice = boolean;
            else if (name == "ghostTapping") gameplay_settings().ghost_tapping = boolean;
            else if (name == "downscroll") gameplay_settings().downscroll = boolean;
            else if (name == "middlescroll" || name == "middleScroll") gameplay_settings().middle_scroll = boolean;
            else gameplay_settings().no_fail = boolean;
            streaming_visual_cache_.reset();
            error.clear();
            return true;
        }
        return script_set_visual_property(name, value, error);
    }

    [[nodiscard]] bool script_host_add_score(
        const std::int64_t amount,
        std::string& error
    ) noexcept {
        if (streaming_mode()) streaming_session_->add_score(amount);
        else session_->add_score(amount);
        error.clear();
        return true;
    }

    [[nodiscard]] bool script_host_set_health(
        const double health,
        std::string& error
    ) {
        if (!std::isfinite(health)) {
            error = "health must be finite";
            return false;
        }
        if (streaming_mode()) streaming_session_->set_health(health);
        else session_->set_health(health);
        error.clear();
        return true;
    }

    [[nodiscard]] bool script_host_trigger_event(
        ScriptEventRequest event,
        std::string& error
    ) {
        if (event.name.size() > 256U || event.value1.size() > 4'096U
            || event.value2.size() > 4'096U) {
            error = "triggerEvent payload exceeds the sandbox limit";
            return false;
        }
        if (script_state_.pending_events.size() >= script_state_.max_pending_events) {
            error = "too many Lua-triggered events in one frame";
            return false;
        }
        script_state_.pending_events.push_back(std::move(event));
        error.clear();
        return true;
    }

    void script_host_debug_print(const std::string_view message) {
        if (script_state_.debug_messages.size() >= script_state_.max_debug_messages) return;
        constexpr std::size_t maximum = 4U * 1'024U;
        script_state_.debug_messages.emplace_back(
            message.substr(0U, std::min(message.size(), maximum))
        );
    }

    [[nodiscard]] std::size_t script_fixed_strum_index(
        const NoteOwner owner,
        const std::uint16_t lane
    ) const noexcept {
        const auto keys = static_cast<std::size_t>(chart_->key_count);
        const auto base = owner == NoteOwner::player
            ? keys
            : owner == NoteOwner::secondary_opponent ? keys * 2U : 0U;
        return base + static_cast<std::size_t>(lane);
    }

    [[nodiscard]] std::optional<std::size_t> script_strum_global_index(
        const std::string_view group,
        const std::int64_t raw_index
    ) const noexcept {
        if (raw_index < 0 || chart_ == std::nullopt) return std::nullopt;
        const auto keys = static_cast<std::size_t>(chart_->key_count);
        const auto opponent_keys = static_cast<std::size_t>(
            active_key_count(NoteOwner::opponent)
        );
        const auto player_keys = static_cast<std::size_t>(
            active_key_count(NoteOwner::player)
        );
        const auto third_keys = secondary_strum_enabled()
            ? static_cast<std::size_t>(active_key_count(
                NoteOwner::secondary_opponent
            ))
            : 0U;
        const auto index = static_cast<std::size_t>(raw_index);
        if (group == "strumLineNotes") {
            if (index < opponent_keys) return index;
            auto relative = index - opponent_keys;
            if (relative < player_keys) return keys + relative;
            relative -= player_keys;
            return relative < third_keys
                ? std::optional<std::size_t>{keys * 2U + relative}
                : std::nullopt;
        }
        if (group == "opponentStrums") {
            return index < opponent_keys ? std::optional<std::size_t>{index}
                                         : std::nullopt;
        }
        if (group == "playerStrums") {
            return index < player_keys ? std::optional<std::size_t>{keys + index}
                                       : std::nullopt;
        }
        if (group == "thirdStrums") {
            return index < third_keys
                ? std::optional<std::size_t>{keys * 2U + index}
                : std::nullopt;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::pair<NoteOwner, std::uint16_t> script_strum_owner_lane(
        const std::size_t global_index
    ) const noexcept {
        const auto keys = static_cast<std::size_t>(chart_->key_count);
        const auto lane = static_cast<std::uint16_t>(
            keys == 0U ? 0U : global_index % keys
        );
        if (keys != 0U && global_index >= keys * 2U) {
            return {NoteOwner::secondary_opponent, lane};
        }
        if (keys != 0U && global_index >= keys) {
            return {NoteOwner::player, lane};
        }
        return {NoteOwner::opponent, lane};
    }

    [[nodiscard]] std::pair<float, float> raw_lane_layout(
        const NoteOwner owner
    ) const {
        return lane_layout(owner);
    }

    [[nodiscard]] double script_base_strum_x(
        const std::size_t global_index
    ) const noexcept {
        if (chart_ == std::nullopt || chart_->key_count == 0U) return 0.0;
        const auto [owner, lane] = script_strum_owner_lane(global_index);
        const auto [base_x, lane_width] = raw_lane_layout(owner);
        return static_cast<double>(base_x)
            + static_cast<double>(lane) * static_cast<double>(lane_width);
    }

    [[nodiscard]] double script_base_strum_y() const noexcept {
        return gameplay_settings().downscroll ? 610.0 : 110.0;
    }

    [[nodiscard]] double script_owner_camera_zoom(
        const NoteOwner owner
    ) const noexcept {
        if (owner != NoteOwner::secondary_opponent) {
            return script_cam_hud_zoom_;
        }
        const double stage_zoom = scene_ != nullptr
            ? scene_->game_camera_base_zoom()
            : 1.0;
        return std::clamp(stage_zoom * script_cam_game_zoom_, 0.0025, 64.0);
    }

    // PULSEFORGE_P1_4_0_THIRD_STRUM_CAMGAME_TRANSFORM_V1
    // DenpaEx puts thirdStrums on camGame with scrollFactor(1, 1).  Primary
    // player/opponent strums remain HUD-space.  Mirror RuntimeScene's world
    // camera convention here: game-camera x/y are camera scroll and therefore
    // subtract from world coordinates; camHUD x/y are visual translation and
    // therefore add.  This keeps player4 notes/receptors glued to the same
    // game-camera transform as the player4 character without changing judgment.
    [[nodiscard]] double script_camera_transform_x(
        const NoteOwner owner,
        const double x
    ) const noexcept {
        if (owner == NoteOwner::secondary_opponent) {
            return (x - script_cam_game_x_
                        - static_cast<double>(logical_width) * 0.5)
                    * script_owner_camera_zoom(owner)
                + static_cast<double>(logical_width) * 0.5;
        }
        return (x - static_cast<double>(logical_width) * 0.5)
                * script_cam_hud_zoom_
            + static_cast<double>(logical_width) * 0.5
            + script_cam_hud_x_;
    }

    [[nodiscard]] double script_camera_transform_y(
        const NoteOwner owner,
        const double y
    ) const noexcept {
        if (owner == NoteOwner::secondary_opponent) {
            return (y - script_cam_game_y_
                        - static_cast<double>(logical_height) * 0.5)
                    * script_owner_camera_zoom(owner)
                + static_cast<double>(logical_height) * 0.5;
        }
        return (y - static_cast<double>(logical_height) * 0.5)
                * script_cam_hud_zoom_
            + static_cast<double>(logical_height) * 0.5
            + script_cam_hud_y_;
    }

    // Existing HUD/script-object call sites intentionally remain HUD-space.
    [[nodiscard]] double script_camera_transform_x(
        const double x
    ) const noexcept {
        return script_camera_transform_x(NoteOwner::player, x);
    }

    [[nodiscard]] double script_camera_transform_y(
        const double y
    ) const noexcept {
        return script_camera_transform_y(NoteOwner::player, y);
    }

    [[nodiscard]] float script_lane_x(
        const NoteOwner owner,
        const std::uint16_t lane,
        const float base_x,
        const float lane_width
    ) const noexcept {
        const auto global = script_fixed_strum_index(owner, lane);
        double logical_x = static_cast<double>(base_x)
            + static_cast<double>(lane) * static_cast<double>(lane_width);
        if (global < script_strums_.size()
            && script_strums_[global].x.has_value()) {
            logical_x = *script_strums_[global].x;
        }
        return static_cast<float>(script_camera_transform_x(owner, logical_x));
    }

    [[nodiscard]] double script_lane_receptor_y(
        const NoteOwner owner,
        const std::uint16_t lane
    ) const noexcept {
        const auto global = script_fixed_strum_index(owner, lane);
        double logical_y = receptor_y_for_owner(owner);
        if (global < script_strums_.size()
            && script_strums_[global].y.has_value()) {
            logical_y = *script_strums_[global].y;
        }
        return script_camera_transform_y(owner, logical_y);
    }

    [[nodiscard]] double script_lane_y_offset(
        const NoteOwner owner,
        const std::uint16_t lane
    ) const noexcept {
        const auto default_y = script_camera_transform_y(
            owner, receptor_y_for_owner(owner)
        );
        return script_lane_receptor_y(owner, lane) - default_y;
    }

    [[nodiscard]] double script_lane_alpha(
        const NoteOwner owner,
        const std::uint16_t lane
    ) const noexcept {
        const auto global = script_fixed_strum_index(owner, lane);
        const double strum_alpha = global < script_strums_.size()
            ? script_strums_[global].alpha
            : 1.0;
        const double camera_alpha = owner == NoteOwner::secondary_opponent
            ? script_cam_game_alpha_
            : script_cam_hud_alpha_;
        return std::clamp(strum_alpha * camera_alpha, 0.0, 1.0);
    }

    [[nodiscard]] double script_lane_angle(
        const NoteOwner owner,
        const std::uint16_t lane
    ) const noexcept {
        const auto global = script_fixed_strum_index(owner, lane);
        const double strum_angle = global < script_strums_.size()
            ? script_strums_[global].angle
            : 0.0;
        return strum_angle + (owner == NoteOwner::secondary_opponent
            ? script_cam_game_angle_
            : script_cam_hud_angle_);
    }

    [[nodiscard]] bool script_lane_visible(
        const NoteOwner owner,
        const std::uint16_t lane
    ) const noexcept {
        const auto global = script_fixed_strum_index(owner, lane);
        return global >= script_strums_.size() || script_strums_[global].visible;
    }

    [[nodiscard]] float script_lane_width(
        const NoteOwner owner,
        const float lane_width
    ) const noexcept {
        const double camera_zoom = script_owner_camera_zoom(owner);
        return static_cast<float>(std::clamp(
            static_cast<double>(lane_width) * camera_zoom,
            1.0,
            100'000.0
        ));
    }

    [[nodiscard]] float script_lane_width(
        const float lane_width
    ) const noexcept {
        return script_lane_width(NoteOwner::player, lane_width);
    }

    [[nodiscard]] double script_note_y(
        const NoteOwner owner,
        const std::uint16_t lane,
        const double logical_y
    ) const noexcept {
        return script_camera_transform_y(owner, logical_y)
            + script_lane_y_offset(owner, lane);
    }

    [[nodiscard]] bool script_get_group_property(
        const std::string_view group,
        const std::int64_t raw_index,
        const std::string_view property,
        ScriptValue& value,
        std::string& error
    ) const {
        if (const auto strum = script_strum_global_index(group, raw_index);
            strum.has_value()) {
            const auto& state = script_strums_[*strum];
            if (property == "x") value = state.x.value_or(script_base_strum_x(*strum));
            else if (property == "y") {
                const auto owner = script_strum_owner_lane(*strum).first;
                value = state.y.value_or(receptor_y_for_owner(owner));
            }
            else if (property == "scale.x" || property == "scaleX") value = state.scale_x;
            else if (property == "scale.y" || property == "scaleY") value = state.scale_y;
            else if (property == "angle") value = state.angle;
            else if (property == "alpha") value = state.alpha;
            else if (property == "visible") value = state.visible;
            else if (property == "texture") value = state.texture;
            else {
                error = "unsupported strum property: ";
                error.append(property);
                return false;
            }
            error.clear();
            return true;
        }

        const bool note_group = group == "unspawnNotes" || group == "notes";
        if (!note_group || raw_index < 0) {
            error = "unsupported Lua group or index";
            return false;
        }
        const auto index = static_cast<std::size_t>(raw_index);
        if (streaming_mode()) {
            std::string_view kind{"normal"};
            std::int64_t note_data{};
            double strum_time{};
            bool sustain{};
            NoteOwner owner{NoteOwner::player};
            if (group == "unspawnNotes") {
                if (index >= script_streaming_unspawn_prototypes_.size()) {
                    error = "streaming owner/type prototype index is out of range";
                    return false;
                }
                const auto prototype = script_streaming_unspawn_prototypes_[index];
                const auto kinds = streaming_reader_->kinds();
                if (prototype.kind_id >= kinds.size()) {
                    error = "streaming owner/type prototype kind is invalid";
                    return false;
                }
                kind = kinds[prototype.kind_id];
                owner = prototype.owner;
            } else {
                const auto notes = streaming_session_->window_notes();
                if (index >= notes.size()) {
                    error = "streaming active-note index is out of range";
                    return false;
                }
                const auto& note = notes[index];
                const auto kinds = streaming_reader_->kinds();
                if (note.note.kind_id < kinds.size()) kind = kinds[note.note.kind_id];
                note_data = note.display_lane;
                strum_time = static_cast<double>(note.note.time_us) / 1'000.0;
                sustain = note.note.duration_us > 0;
                owner = runtime_owner(note.note.owner, kind);
            }
            const bool must_press = owner == NoteOwner::player;
            if (property == "noteType") value = std::string(kind);
            else if (property == "noteData") value = note_data;
            else if (property == "strumTime") value = strum_time;
            else if (property == "isSustainNote") value = sustain;
            else if (property == "mustPress") value = must_press;
            else if (property == "strum") {
                // PULSEFORGE_P1_4_0_THIRD_STRUM_NOTE_PROPERTY_V1
                value = static_cast<std::int64_t>(
                    owner == NoteOwner::secondary_opponent
                        ? 2
                        : must_press ? 1 : 0
                );
            }
            else if (property == "multSpeed" || property == "multspeed") {
                const auto found = streaming_kind_speed_multipliers_.find(kind);
                value = found == streaming_kind_speed_multipliers_.end()
                    ? 1.0
                    : static_cast<double>(found->second);
            } else if (property == "alpha") {
                const auto* visual = script_note_visual_state(owner, kind);
                value = visual != nullptr && visual->alpha.has_value()
                    ? *visual->alpha
                    : 1.0;
            } else if (property == "texture") {
                const auto* visual = script_note_visual_state(owner, kind);
                value = visual != nullptr ? visual->texture : std::string{};
            } else if (property == "noteSplashTexture") {
                const auto* visual = script_note_visual_state(owner, kind);
                value = visual != nullptr ? visual->splash_texture : std::string{};
            } else if (property == "noteSplashDisabled") {
                const auto* visual = script_note_visual_state(owner, kind);
                value = visual != nullptr && visual->splash_disabled.value_or(false);
            } else if (property == "missHealth") {
                const auto* behavior = runtime_note_behavior(kind);
                value = behavior != nullptr
                    ? behavior->miss_health.value_or(gameplay_settings().health_loss)
                    : gameplay_settings().health_loss;
            } else if (property == "hitHealth") {
                const auto* behavior = runtime_note_behavior(kind);
                value = behavior != nullptr
                    ? behavior->hit_health.value_or(gameplay_settings().health_gain)
                    : gameplay_settings().health_gain;
            } else if (property == "hitCausesMiss") {
                const auto* behavior = runtime_note_behavior(kind);
                value = behavior != nullptr
                    && behavior->hit_causes_miss.value_or(false);
            } else if (property == "ignoreNote") {
                const auto* behavior = runtime_note_behavior(kind);
                value = behavior != nullptr
                    && behavior->ignore_note.value_or(false);
            } else if (property == "noAnimation") {
                value = note_animation_suppressed(kind);
            } else {
                error = "unsupported streaming note property: ";
                error.append(property);
                return false;
            }
            error.clear();
            return true;
        }

        if (chart_ == std::nullopt || index >= chart_->notes.size()) {
            error = "note index is out of range";
            return false;
        }
        const auto& note = chart_->notes[index];
        const auto owner = runtime_owner(note.owner, note.kind);
        if (property == "noteType") value = note.kind;
        else if (property == "noteData") value = static_cast<std::int64_t>(
            session_->display_lane(index)
        );
        else if (property == "strumTime") value = note.time_ms;
        else if (property == "isSustainNote") value = note.duration_ms > 0.0;
        else if (property == "mustPress") value = owner == NoteOwner::player;
        else if (property == "strum") {
            value = static_cast<std::int64_t>(
                owner == NoteOwner::secondary_opponent
                    ? 2
                    : owner == NoteOwner::player ? 1 : 0
            );
        }
        else if (property == "multSpeed" || property == "multspeed") {
            value = index < materialized_note_speed_multipliers_.size()
                ? static_cast<double>(materialized_note_speed_multipliers_[index])
                : 1.0;
        } else if (property == "alpha") {
            const auto* visual = script_note_visual_state(owner, note.kind);
            value = visual != nullptr && visual->alpha.has_value()
                ? *visual->alpha
                : 1.0;
        } else if (property == "texture") {
            const auto* visual = script_note_visual_state(owner, note.kind);
            value = visual != nullptr ? visual->texture : std::string{};
        } else if (property == "noteSplashTexture") {
            const auto* visual = script_note_visual_state(owner, note.kind);
            value = visual != nullptr ? visual->splash_texture : std::string{};
        } else if (property == "noteSplashDisabled") {
            const auto* visual = script_note_visual_state(owner, note.kind);
            value = visual != nullptr && visual->splash_disabled.value_or(false);
        } else if (property == "missHealth") {
            const auto* behavior = runtime_note_behavior(note.kind);
            value = behavior != nullptr
                ? behavior->miss_health.value_or(gameplay_settings().health_loss)
                : gameplay_settings().health_loss;
        } else if (property == "hitHealth") {
            const auto* behavior = runtime_note_behavior(note.kind);
            value = behavior != nullptr
                ? behavior->hit_health.value_or(gameplay_settings().health_gain)
                : gameplay_settings().health_gain;
        } else if (property == "hitCausesMiss") {
            const auto* behavior = runtime_note_behavior(note.kind);
            value = behavior != nullptr
                && behavior->hit_causes_miss.value_or(false);
        } else if (property == "ignoreNote") {
            const auto* behavior = runtime_note_behavior(note.kind);
            value = behavior != nullptr
                && behavior->ignore_note.value_or(false);
        } else if (property == "noAnimation") {
            value = note_animation_suppressed(note.kind);
        } else {
            error = "unsupported note property: ";
            error.append(property);
            return false;
        }
        error.clear();
        return true;
    }

    [[nodiscard]] bool script_set_group_property(
        const std::string_view group,
        const std::int64_t raw_index,
        const std::string_view property,
        const ScriptValue& value,
        std::string& error
    ) {
        if (const auto strum = script_strum_global_index(group, raw_index);
            strum.has_value()) {
            auto& state = script_strums_[*strum];
            double number{};
            bool boolean{};
            if (property == "x" || property == "y" || property == "angle"
                || property == "alpha" || property == "scale.x"
                || property == "scale.y" || property == "scaleX"
                || property == "scaleY") {
                if (!script_number_value(value, number)) {
                    error = "strum property expects a finite number";
                    return false;
                }
                if (property == "x") state.x = std::clamp(number, -100'000.0, 100'000.0);
                else if (property == "y") state.y = std::clamp(number, -100'000.0, 100'000.0);
                else if (property == "scale.x" || property == "scaleX") {
                    state.scale_x = std::clamp(number, 0.01, 100.0);
                } else if (property == "scale.y" || property == "scaleY") {
                    state.scale_y = std::clamp(number, 0.01, 100.0);
                } else if (property == "angle") {
                    state.angle = std::clamp(number, -360'000.0, 360'000.0);
                } else {
                    state.alpha = std::clamp(number, 0.0, 1.0);
                }
            } else if (property == "visible") {
                if (!script_boolean_value(value, boolean)) {
                    error = "strum visible expects a boolean";
                    return false;
                }
                state.visible = boolean;
            } else if (property == "texture") {
                std::string_view texture;
                if (!script_string_value(value, texture) || texture.size() > 192U) {
                    error = "strum texture expects an asset id up to 192 bytes";
                    return false;
                }
                state.texture.assign(texture);
                state.texture_profile.reset();
                if (!texture.empty() && scene_ != nullptr) {
                    state.texture_profile = scene_->resolve_note_skin_profile(
                        texture, false
                    );
                }
            } else {
                error = "unsupported strum property: ";
                error.append(property);
                return false;
            }
            error.clear();
            return true;
        }

        if ((group != "unspawnNotes" && group != "notes") || raw_index < 0) {
            error = "unsupported Lua group or index";
            return false;
        }
        if (property == "multSpeed" || property == "multspeed") {
            double multiplier{};
            if (!script_number_value(value, multiplier) || multiplier <= 0.0) {
                error = "multSpeed must be a finite positive number";
                return false;
            }
            multiplier = std::clamp(multiplier, 0.01, 100.0);
            const auto index = static_cast<std::size_t>(raw_index);
            if (streaming_mode()) {
                std::string kind;
                if (group == "unspawnNotes") {
                    if (index >= script_streaming_unspawn_prototypes_.size()) {
                        error = "streaming owner/type prototype index is out of range";
                        return false;
                    }
                    const auto kinds = streaming_reader_->kinds();
                    const auto prototype = script_streaming_unspawn_prototypes_[index];
                    if (prototype.kind_id >= kinds.size()) {
                        error = "streaming owner/type prototype kind is invalid";
                        return false;
                    }
                    kind = kinds[prototype.kind_id];
                } else {
                    const auto notes = streaming_session_->window_notes();
                    if (index >= notes.size()) {
                        error = "streaming active-note index is out of range";
                        return false;
                    }
                    const auto kinds = streaming_reader_->kinds();
                    const auto kind_id = notes[index].note.kind_id;
                    kind = kind_id < kinds.size() ? kinds[kind_id] : "normal";
                }
                streaming_kind_speed_multipliers_[kind] = static_cast<float>(multiplier);
            } else {
                if (chart_ == std::nullopt
                    || index >= materialized_note_speed_multipliers_.size()) {
                    error = "note index is out of range";
                    return false;
                }
                materialized_note_speed_multipliers_[index] = static_cast<float>(multiplier);
            }
            minimum_note_scroll_multiplier_ = std::min(
                minimum_note_scroll_multiplier_,
                multiplier
            );
            if (std::abs(multiplier - 1.0) > 1.0e-6) {
                note_scroll_multiplier_active_ = true;
            }
            // Per-note speed changes invalidate any screen-space aggregate
            // built with the previous transform.
            streaming_visual_cache_.reset();
            error.clear();
            return true;
        }

        // PULSEFORGE_P1_5_0_LUA_NOTE_PROPERTY_GAMEPLAY_BRIDGE_V1
        // Psych custom note scripts commonly configure unspawnNotes before
        // gameplay. Resolve the referenced kind and mutate one bounded runtime
        // behavior entry rather than allocating state for every note.
        const auto index = static_cast<std::size_t>(raw_index);
        std::string kind;
        NoteOwner owner{NoteOwner::player};
        if (streaming_mode()) {
            const auto kinds = streaming_reader_->kinds();
            if (group == "unspawnNotes") {
                if (index >= script_streaming_unspawn_prototypes_.size()) {
                    error = "streaming owner/type prototype index is out of range";
                    return false;
                }
                const auto prototype = script_streaming_unspawn_prototypes_[index];
                if (prototype.kind_id >= kinds.size()) {
                    error = "streaming owner/type prototype kind is invalid";
                    return false;
                }
                kind = kinds[prototype.kind_id];
                owner = prototype.owner;
            } else {
                const auto notes = streaming_session_->window_notes();
                if (index >= notes.size() || notes[index].note.kind_id >= kinds.size()) {
                    error = "streaming active-note index is out of range";
                    return false;
                }
                kind = kinds[notes[index].note.kind_id];
                owner = runtime_owner(notes[index].note.owner, kind);
            }
        } else {
            if (chart_ == std::nullopt || index >= chart_->notes.size()) {
                error = "note index is out of range";
                return false;
            }
            kind = chart_->notes[index].kind;
            owner = runtime_owner(chart_->notes[index].owner, kind);
        }

        if (property == "missHealth" || property == "hitHealth"
            || property == "sustainMissHealth") {
            double number{};
            if (!script_number_value(value, number)
                || number < -2.0 || number > 2.0) {
                error = "note health property expects a finite value in [-2, 2]";
                return false;
            }
            NoteKindRuntimeBehavior behavior = runtime_note_behavior(kind) != nullptr
                ? *runtime_note_behavior(kind)
                : NoteKindRuntimeBehavior{};
            if (property == "missHealth") behavior.miss_health = number;
            else if (property == "hitHealth") behavior.hit_health = number;
            else behavior.sustain_miss_health = number;
            if (!set_runtime_note_behavior(kind, behavior)) {
                error = "runtime note-kind behavior limit or kind lookup failed";
                return false;
            }
            error.clear();
            return true;
        }
        if (property == "hitCausesMiss" || property == "ignoreNote"
            || property == "sustainHitCausesMiss") {
            bool boolean{};
            if (!script_boolean_value(value, boolean)) {
                error = "note boolean property expects a boolean";
                return false;
            }
            NoteKindRuntimeBehavior behavior = runtime_note_behavior(kind) != nullptr
                ? *runtime_note_behavior(kind)
                : NoteKindRuntimeBehavior{};
            if (property == "hitCausesMiss") behavior.hit_causes_miss = boolean;
            else if (property == "ignoreNote") behavior.ignore_note = boolean;
            else behavior.sustain_hit_causes_miss = boolean;
            if (!set_runtime_note_behavior(kind, behavior)) {
                error = "runtime note-kind behavior limit or kind lookup failed";
                return false;
            }
            error.clear();
            return true;
        }
        if (property == "noAnimation") {
            bool boolean{};
            if (!script_boolean_value(value, boolean)) {
                error = "noAnimation expects a boolean";
                return false;
            }
            script_note_no_animation_[kind] = boolean;
            error.clear();
            return true;
        }

        // PULSEFORGE_P1_5_0B_LUA_NOTE_VISUAL_PROPERTY_BRIDGE_V1
        // `unspawnNotes` mutations collapse to owner+kind state. This matches
        // the common Psych setup-loop semantics while remaining constant with
        // respect to the number of physical notes in a giant chart.
        if (property == "texture" || property == "noteSplashDisabled"
            || property == "noteSplashTexture" || property == "alpha") {
            auto* visual = ensure_script_note_visual_state(owner, kind);
            if (visual == nullptr) {
                error = "bounded note visual override limit or invalid kind";
                return false;
            }
            if (property == "texture") {
                std::string_view texture;
                if (!script_string_value(value, texture) || texture.size() > 192U) {
                    error = "note texture expects an asset id up to 192 bytes";
                    return false;
                }
                visual->texture.assign(texture);
                visual->texture_profile.reset();
                if (!texture.empty() && scene_ != nullptr) {
                    visual->texture_profile = scene_->resolve_note_skin_profile(
                        texture, false
                    );
                }
            } else if (property == "noteSplashTexture") {
                std::string_view texture;
                if (!script_string_value(value, texture) || texture.size() > 192U) {
                    error = "noteSplashTexture expects an asset id up to 192 bytes";
                    return false;
                }
                visual->splash_texture.assign(texture);
                visual->splash_profile.reset();
                if (!texture.empty() && scene_ != nullptr) {
                    visual->splash_profile = scene_->resolve_note_splash_profile(
                        texture
                    );
                }
            } else if (property == "noteSplashDisabled") {
                bool disabled{};
                if (!script_boolean_value(value, disabled)) {
                    error = "noteSplashDisabled expects a boolean";
                    return false;
                }
                visual->splash_disabled = disabled;
            } else {
                double alpha{};
                if (!script_number_value(value, alpha)) {
                    error = "note alpha expects a finite number";
                    return false;
                }
                visual->alpha = std::clamp(alpha, 0.0, 1.0);
            }
            streaming_visual_cache_.reset();
            error.clear();
            return true;
        }
        error = "unsupported note property: ";
        error.append(property);
        return false;
    }

    [[nodiscard]] bool script_get_visual_property(
        const std::string_view name,
        ScriptValue& value,
        std::string& error
    ) const {
        const auto length_property = [&](const std::string_view group) {
            return name == std::string(group) + ".length";
        };
        if (length_property("strumLineNotes")) {
            value = static_cast<std::int64_t>(
                active_key_count(NoteOwner::opponent)
            ) + static_cast<std::int64_t>(active_key_count(NoteOwner::player))
                + (secondary_strum_enabled()
                    ? static_cast<std::int64_t>(active_key_count(
                        NoteOwner::secondary_opponent
                    ))
                    : 0);
            error.clear();
            return true;
        }
        if (length_property("playerStrums")) {
            value = static_cast<std::int64_t>(active_key_count(NoteOwner::player));
            error.clear();
            return true;
        }
        if (length_property("opponentStrums")) {
            value = static_cast<std::int64_t>(active_key_count(NoteOwner::opponent));
            error.clear();
            return true;
        }
        if (length_property("thirdStrums")) {
            value = secondary_strum_enabled()
                ? static_cast<std::int64_t>(
                    active_key_count(NoteOwner::secondary_opponent)
                )
                : 0;
            error.clear();
            return true;
        }
        if (length_property("unspawnNotes")) {
            if (streaming_mode()) {
                // Streaming virtualizes unspawnNotes as bounded owner+type
                // prototypes. This preserves mustPress-aware custom-note setup
                // loops without expanding millions/billions of logical notes.
                value = static_cast<std::int64_t>(
                    script_streaming_unspawn_prototypes_.size()
                );
            } else {
                value = static_cast<std::int64_t>(std::min<std::uint64_t>(
                    chart_->notes.size(),
                    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                ));
            }
            error.clear();
            return true;
        }
        if (length_property("notes")) {
            value = streaming_mode()
                ? static_cast<std::int64_t>(streaming_session_->window_notes().size())
                : static_cast<std::int64_t>(std::min<std::uint64_t>(
                    chart_->notes.size(),
                    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                ));
            error.clear();
            return true;
        }

        if (name == "camGame.x") value = script_cam_game_x_;
        else if (name == "camGame.y") value = script_cam_game_y_;
        else if (name == "camFollow.x" || name == "camFollowPos.x") {
            value = script_cam_game_x_ + static_cast<double>(logical_width) * 0.5;
        }
        else if (name == "camFollow.y" || name == "camFollowPos.y") {
            value = script_cam_game_y_ + static_cast<double>(logical_height) * 0.5;
        }
        else if (name == "camGame.zoom" || name == "defaultCamZoom") value = script_cam_game_zoom_;
        else if (name == "camGame.angle") value = script_cam_game_angle_;
        else if (name == "camGame.alpha") value = script_cam_game_alpha_;
        else if (name == "camHUD.x") value = script_cam_hud_x_;
        else if (name == "camHUD.y") value = script_cam_hud_y_;
        else if (name == "camHUD.zoom") value = script_cam_hud_zoom_;
        else if (name == "camHUD.angle") value = script_cam_hud_angle_;
        else if (name == "camHUD.alpha") value = script_cam_hud_alpha_;
        else {
            const auto dot = name.find('.');
            if (dot != std::string_view::npos) {
                const auto object = name.substr(0U, dot);
                const auto property = name.substr(dot + 1U);
                if (const auto hud = script_hud_objects_.find(object);
                    hud != script_hud_objects_.end()) {
                    const auto& state = hud->second;
                    bool handled = true;
                    if (property == "x") value = state.x;
                    else if (property == "y") value = state.y;
                    else if (property == "scale.x") value = state.scale_x;
                    else if (property == "scale.y") value = state.scale_y;
                    else if (property == "zoom") value = (state.scale_x + state.scale_y) * 0.5;
                    else if (property == "angle") value = state.angle;
                    else if (property == "alpha") value = state.alpha;
                    else if (property == "visible") value = state.visible;
                    else handled = false;
                    if (handled) {
                        error.clear();
                        return true;
                    }
                }
                if (scene_ != nullptr) {
                    double number{};
                    if (scene_->script_get_number(object, property, number)) {
                        value = number;
                        error.clear();
                        return true;
                    }
                }
            }
            const auto stored = script_property_store_.find(name);
            if (stored != script_property_store_.end()) {
                value = stored->second;
                error.clear();
                return true;
            }
            // Psych scripts frequently probe optional object fields. Return a
            // neutral scalar for common transform/visibility fields rather
            // than aborting the whole callback on an absent decorative object.
            if (name.ends_with(".alpha") || name.ends_with(".zoom")
                || name.ends_with(".scale.x") || name.ends_with(".scale.y")) {
                value = 1.0;
                error.clear();
                return true;
            }
            if (name.ends_with(".x") || name.ends_with(".y")
                || name.ends_with(".angle")) {
                value = 0.0;
                error.clear();
                return true;
            }
            if (name.ends_with(".visible") || name.ends_with(".specialAnim")) {
                value = true;
                error.clear();
                return true;
            }
            error = "property is not exposed by the runtime visual bridge: ";
            error.append(name);
            return false;
        }
        error.clear();
        return true;
    }

    [[nodiscard]] bool script_set_visual_property(
        const std::string_view name,
        const ScriptValue& value,
        std::string& error
    ) {
        double number{};
        bool boolean{};
        const auto update_scene_camera = [&]() {
            if (scene_ != nullptr) {
                scene_->script_set_game_camera(
                    script_cam_game_x_,
                    script_cam_game_y_,
                    script_cam_game_zoom_,
                    script_cam_game_angle_,
                    script_cam_game_alpha_
                );
            }
        };
        const auto update_scene_hud_camera = [&]() {
            if (scene_ != nullptr) {
                scene_->script_set_hud_camera(
                    script_cam_hud_x_,
                    script_cam_hud_y_,
                    script_cam_hud_zoom_,
                    script_cam_hud_angle_,
                    script_cam_hud_alpha_
                );
            }
        };
        // PULSEFORGE_P1_1_16_CAMERA_FOLLOW_PROPERTY_V1
        if (name == "camFollow.x" || name == "camFollowPos.x"
            || name == "camFollow.y" || name == "camFollowPos.y") {
            if (!script_number_value(value, number)) {
                error = "camera-follow property expects a finite number";
                return false;
            }
            if (name.ends_with(".x")) {
                script_cam_game_x_ = std::clamp(
                    number - static_cast<double>(logical_width) * 0.5,
                    -100'000.0,
                    100'000.0
                );
            } else {
                script_cam_game_y_ = std::clamp(
                    number - static_cast<double>(logical_height) * 0.5,
                    -100'000.0,
                    100'000.0
                );
            }
            update_scene_camera();
            error.clear();
            return true;
        }

        if (name == "camGame.x" || name == "camGame.y"
            || name == "camGame.zoom" || name == "defaultCamZoom"
            || name == "camGame.angle" || name == "camGame.alpha") {
            if (!script_number_value(value, number)) {
                error = "camera property expects a finite number";
                return false;
            }
            if (name == "camGame.x") script_cam_game_x_ = std::clamp(number, -100'000.0, 100'000.0);
            else if (name == "camGame.y") script_cam_game_y_ = std::clamp(number, -100'000.0, 100'000.0);
            else if (name == "camGame.zoom" || name == "defaultCamZoom") script_cam_game_zoom_ = std::clamp(number, 0.05, 8.0);
            else if (name == "camGame.angle") script_cam_game_angle_ = std::clamp(number, -360'000.0, 360'000.0);
            else script_cam_game_alpha_ = std::clamp(number, 0.0, 1.0);
            update_scene_camera();
            error.clear();
            return true;
        }
        if (name == "camHUD.x" || name == "camHUD.y"
            || name == "camHUD.zoom" || name == "camHUD.angle"
            || name == "camHUD.alpha") {
            if (!script_number_value(value, number)) {
                error = "HUD camera property expects a finite number";
                return false;
            }
            if (name == "camHUD.x") script_cam_hud_x_ = std::clamp(number, -100'000.0, 100'000.0);
            else if (name == "camHUD.y") script_cam_hud_y_ = std::clamp(number, -100'000.0, 100'000.0);
            else if (name == "camHUD.zoom") script_cam_hud_zoom_ = std::clamp(number, 0.05, 8.0);
            else if (name == "camHUD.angle") script_cam_hud_angle_ = std::clamp(number, -360'000.0, 360'000.0);
            else script_cam_hud_alpha_ = std::clamp(number, 0.0, 1.0);
            update_scene_hud_camera();
            streaming_visual_cache_.reset();
            error.clear();
            return true;
        }

        const auto dot = name.find('.');
        if (dot != std::string_view::npos) {
            const auto object = name.substr(0U, dot);
            const auto property = name.substr(dot + 1U);
            if (auto hud = script_hud_objects_.find(object);
                hud != script_hud_objects_.end()) {
                auto& state = hud->second;
                if (property == "visible") {
                    if (!script_boolean_value(value, boolean)) {
                        error = "HUD visibility expects a boolean";
                        return false;
                    }
                    state.visible = boolean;
                    error.clear();
                    return true;
                }
                if (script_number_value(value, number)) {
                    bool handled = true;
                    if (property == "x") state.x = std::clamp(number, -100'000.0, 100'000.0);
                    else if (property == "y") state.y = std::clamp(number, -100'000.0, 100'000.0);
                    else if (property == "scale.x") state.scale_x = std::clamp(number, 0.01, 100.0);
                    else if (property == "scale.y") state.scale_y = std::clamp(number, 0.01, 100.0);
                    else if (property == "zoom") state.scale_x = state.scale_y = std::clamp(number, 0.01, 100.0);
                    else if (property == "angle") state.angle = std::clamp(number, -360'000.0, 360'000.0);
                    else if (property == "alpha") state.alpha = std::clamp(number, 0.0, 1.0);
                    else handled = false;
                    if (handled) {
                        error.clear();
                        return true;
                    }
                }
            }
            if (scene_ != nullptr) {
                if (script_number_value(value, number)
                    && scene_->script_set_number(object, property, number)) {
                    error.clear();
                    return true;
                }
                if (property == "visible" && script_boolean_value(value, boolean)
                    && scene_->script_set_visible(object, boolean)) {
                    error.clear();
                    return true;
                }
            }
            // specialAnim and similar state flags are consumed by Psych's
            // animation controller; PulseForge's explicit play-animation call
            // already owns the visible effect, so storing the scalar is enough.
        }
        if (name.size() <= 256U && script_property_store_.size() < 4'096U) {
            script_property_store_[std::string(name)] = value;
            error.clear();
            return true;
        }
        error = "runtime visual property store is full or property name is too long";
        return false;
    }

    [[nodiscard]] bool script_get_tween_value(
        const ScriptTween& tween,
        double& value
    ) const noexcept {
        if (tween.target == ScriptTweenTarget::strum) {
            if (tween.index >= script_strums_.size()) return false;
            const auto& state = script_strums_[tween.index];
            if (tween.property == "x") value = state.x.value_or(script_base_strum_x(tween.index));
            else if (tween.property == "y") {
                const auto owner = script_strum_owner_lane(tween.index).first;
                value = state.y.value_or(receptor_y_for_owner(owner));
            }
            else if (tween.property == "scale.x") value = state.scale_x;
            else if (tween.property == "scale.y") value = state.scale_y;
            else if (tween.property == "angle") value = state.angle;
            else if (tween.property == "alpha") value = state.alpha;
            else return false;
            return true;
        }
        if (tween.target == ScriptTweenTarget::camera_game) {
            if (tween.property == "x") value = script_cam_game_x_;
            else if (tween.property == "y") value = script_cam_game_y_;
            else if (tween.property == "zoom") value = script_cam_game_zoom_;
            else if (tween.property == "angle") value = script_cam_game_angle_;
            else if (tween.property == "alpha") value = script_cam_game_alpha_;
            else return false;
            return true;
        }
        if (tween.target == ScriptTweenTarget::camera_hud) {
            if (tween.property == "x") value = script_cam_hud_x_;
            else if (tween.property == "y") value = script_cam_hud_y_;
            else if (tween.property == "zoom") value = script_cam_hud_zoom_;
            else if (tween.property == "angle") value = script_cam_hud_angle_;
            else if (tween.property == "alpha") value = script_cam_hud_alpha_;
            else return false;
            return true;
        }
        if (const auto hud = script_hud_objects_.find(tween.object);
            hud != script_hud_objects_.end()) {
            const auto& state = hud->second;
            if (tween.property == "x") value = state.x;
            else if (tween.property == "y") value = state.y;
            else if (tween.property == "scale.x") value = state.scale_x;
            else if (tween.property == "scale.y") value = state.scale_y;
            else if (tween.property == "zoom") value = (state.scale_x + state.scale_y) * 0.5;
            else if (tween.property == "angle") value = state.angle;
            else if (tween.property == "alpha") value = state.alpha;
            else return false;
            return true;
        }
        return scene_ != nullptr
            && scene_->script_get_number(tween.object, tween.property, value);
    }

    [[nodiscard]] bool script_set_tween_value(
        const ScriptTween& tween,
        const double value
    ) noexcept {
        if (tween.target == ScriptTweenTarget::strum) {
            if (tween.index >= script_strums_.size()) return false;
            auto& state = script_strums_[tween.index];
            if (tween.property == "x") state.x = value;
            else if (tween.property == "y") state.y = value;
            else if (tween.property == "scale.x") {
                state.scale_x = std::clamp(value, 0.01, 100.0);
            } else if (tween.property == "scale.y") {
                state.scale_y = std::clamp(value, 0.01, 100.0);
            } else if (tween.property == "angle") state.angle = value;
            else if (tween.property == "alpha") state.alpha = std::clamp(value, 0.0, 1.0);
            else return false;
            streaming_visual_cache_.reset();
            return true;
        }
        if (tween.target == ScriptTweenTarget::camera_game) {
            if (tween.property == "x") script_cam_game_x_ = value;
            else if (tween.property == "y") script_cam_game_y_ = value;
            else if (tween.property == "zoom") script_cam_game_zoom_ = std::clamp(value, 0.05, 8.0);
            else if (tween.property == "angle") script_cam_game_angle_ = value;
            else if (tween.property == "alpha") script_cam_game_alpha_ = std::clamp(value, 0.0, 1.0);
            else return false;
            if (scene_ != nullptr) scene_->script_set_game_camera(
                script_cam_game_x_, script_cam_game_y_, script_cam_game_zoom_,
                script_cam_game_angle_, script_cam_game_alpha_
            );
            return true;
        }
        if (tween.target == ScriptTweenTarget::camera_hud) {
            if (tween.property == "x") script_cam_hud_x_ = value;
            else if (tween.property == "y") script_cam_hud_y_ = value;
            else if (tween.property == "zoom") script_cam_hud_zoom_ = std::clamp(value, 0.05, 8.0);
            else if (tween.property == "angle") script_cam_hud_angle_ = value;
            else if (tween.property == "alpha") script_cam_hud_alpha_ = std::clamp(value, 0.0, 1.0);
            else return false;
            if (scene_ != nullptr) scene_->script_set_hud_camera(
                script_cam_hud_x_, script_cam_hud_y_, script_cam_hud_zoom_,
                script_cam_hud_angle_, script_cam_hud_alpha_
            );
            streaming_visual_cache_.reset();
            return true;
        }
        if (auto hud = script_hud_objects_.find(tween.object);
            hud != script_hud_objects_.end()) {
            auto& state = hud->second;
            if (tween.property == "x") state.x = value;
            else if (tween.property == "y") state.y = value;
            else if (tween.property == "scale.x") state.scale_x = std::clamp(value, 0.01, 100.0);
            else if (tween.property == "scale.y") state.scale_y = std::clamp(value, 0.01, 100.0);
            else if (tween.property == "zoom") state.scale_x = state.scale_y = std::clamp(value, 0.01, 100.0);
            else if (tween.property == "angle") state.angle = value;
            else if (tween.property == "alpha") state.alpha = std::clamp(value, 0.0, 1.0);
            else return false;
            return true;
        }
        if (scene_ == nullptr) {
            return false;
        }
        if (tween.property == "color") {
            // Packed-RGB values are used only as compact endpoints. Interpolate
            // channels independently so a color tween never walks through
            // unrelated packed-integer colours.
            const double span = tween.to - tween.from;
            const double ratio = std::abs(span) > 0.5
                ? std::clamp((value - tween.from) / span, 0.0, 1.0)
                : 1.0;
            const auto from = static_cast<std::uint32_t>(std::clamp(
                std::llround(tween.from), 0LL, 0xFFFFFFFFLL
            ));
            const auto to = static_cast<std::uint32_t>(std::clamp(
                std::llround(tween.to), 0LL, 0xFFFFFFFFLL
            ));
            const auto channel = [ratio](
                const std::uint32_t left, const std::uint32_t right
            ) noexcept {
                return static_cast<std::uint32_t>(std::clamp(
                    std::llround(std::lerp(
                        static_cast<double>(left),
                        static_cast<double>(right), ratio
                    )),
                    0LL, 255LL
                ));
            };
            const auto red = channel((from >> 16U) & 0xFFU, (to >> 16U) & 0xFFU);
            const auto green = channel((from >> 8U) & 0xFFU, (to >> 8U) & 0xFFU);
            const auto blue = channel(from & 0xFFU, to & 0xFFU);
            const std::uint32_t packed = 0xFF000000U
                | (red << 16U) | (green << 8U) | blue;
            return scene_->script_set_number(
                tween.object, "color", static_cast<double>(packed)
            );
        }
        return scene_->script_set_number(tween.object, tween.property, value);
    }

    [[nodiscard]] bool script_start_tween(
        std::string tag,
        const ScriptTweenTarget target,
        std::string object,
        std::string property,
        const std::size_t index,
        const double destination,
        const double duration_seconds,
        std::string easing,
        std::string& error
    ) {
        if (!std::isfinite(destination) || !std::isfinite(duration_seconds)
            || duration_seconds < 0.0 || tag.size() > 128U
            || script_tweens_.size() >= 512U) {
            error = "invalid or excessive Lua tween";
            return false;
        }
        ScriptTween tween;
        tween.tag = std::move(tag);
        tween.target = target;
        tween.object = std::move(object);
        tween.property = std::move(property);
        tween.index = index;
        tween.to = destination;
        tween.duration = duration_seconds;
        tween.easing = std::move(easing);
        if (!script_get_tween_value(tween, tween.from)) {
            error = "Lua tween target/property is unavailable";
            return false;
        }
        script_tweens_.erase(
            std::remove_if(
                script_tweens_.begin(), script_tweens_.end(),
                [&](const ScriptTween& existing) { return existing.tag == tween.tag; }
            ),
            script_tweens_.end()
        );
        if (tween.duration <= 0.0) {
            static_cast<void>(script_set_tween_value(tween, tween.to));
            error.clear();
            return true;
        }
        script_tweens_.push_back(std::move(tween));
        error.clear();
        return true;
    }

    [[nodiscard]] static double script_ease_value(
        const double raw,
        const std::string_view easing
    ) noexcept {
        const double t = std::clamp(raw, 0.0, 1.0);
        if (easing == "linear") return t;
        if (easing == "quadIn") return t * t;
        if (easing == "quadOut") return 1.0 - (1.0 - t) * (1.0 - t);
        if (easing == "quadInOut") return t < 0.5
            ? 2.0 * t * t
            : 1.0 - std::pow(-2.0 * t + 2.0, 2.0) / 2.0;
        if (easing == "cubeIn") return t * t * t;
        if (easing == "cubeOut") return 1.0 - std::pow(1.0 - t, 3.0);
        if (easing == "sineInOut") return -(std::cos(3.14159265358979323846 * t) - 1.0) * 0.5;
        if (easing == "circIn" || easing == "CircIn") {
            return 1.0 - std::sqrt(std::max(0.0, 1.0 - t * t));
        }
        if (easing == "circOut" || easing == "CircOut") {
            const double shifted = t - 1.0;
            return std::sqrt(std::max(0.0, 1.0 - shifted * shifted));
        }
        if (easing == "circInOut" || easing == "CircInOut") {
            if (t < 0.5) {
                const double doubled = 2.0 * t;
                return (1.0 - std::sqrt(
                    std::max(0.0, 1.0 - doubled * doubled)
                )) * 0.5;
            }
            const double shifted = -2.0 * t + 2.0;
            return (std::sqrt(
                std::max(0.0, 1.0 - shifted * shifted)
            ) + 1.0) * 0.5;
        }
        return t;
    }

    void update_script_tweens(const double elapsed_seconds) {
        if (script_tweens_.empty() || !std::isfinite(elapsed_seconds)
            || elapsed_seconds <= 0.0) return;
        std::vector<std::pair<std::string, std::string>> completed;
        completed.reserve(std::min<std::size_t>(script_tweens_.size(), 64U));
        for (auto& tween : script_tweens_) {
            const bool was_complete = tween.elapsed >= tween.duration;
            tween.elapsed = std::min(tween.elapsed + elapsed_seconds, tween.duration);
            const double ratio = tween.duration > 0.0
                ? tween.elapsed / tween.duration
                : 1.0;
            const double eased = script_ease_value(ratio, tween.easing);
            static_cast<void>(script_set_tween_value(
                tween,
                std::lerp(tween.from, tween.to, eased)
            ));
            if (!was_complete && tween.elapsed >= tween.duration
                && completed.size() < 64U) {
                completed.emplace_back(tween.tag, tween.object);
            }
        }
        script_tweens_.erase(
            std::remove_if(
                script_tweens_.begin(), script_tweens_.end(),
                [](const ScriptTween& tween) {
                    return tween.elapsed >= tween.duration;
                }
            ),
            script_tweens_.end()
        );
        if (scripts_ != nullptr) {
            for (const auto& [tag, target] : completed) {
                static_cast<void>(scripts_->on_tween_completed(tag, target));
            }
        }
    }

    void update_script_timers(const double elapsed_seconds) {
        if (script_timers_.empty() || !std::isfinite(elapsed_seconds)
            || elapsed_seconds <= 0.0) {
            return;
        }

        struct Completion final {
            std::string tag;
            std::int64_t loops{};
            std::int64_t loops_left{};
        };
        std::vector<Completion> completed;
        constexpr std::size_t maximum_timer_callbacks_per_frame = 64U;
        completed.reserve(std::min(
            script_timers_.size(),
            maximum_timer_callbacks_per_frame
        ));

        for (auto& timer : script_timers_) {
            timer.remaining_seconds -= elapsed_seconds;
            while (timer.remaining_seconds <= 0.0
                   && completed.size() < maximum_timer_callbacks_per_frame) {
                const bool infinite = timer.total_loops == 0;
                if (!infinite && timer.loops_left <= 0) {
                    break;
                }
                if (!infinite) {
                    --timer.loops_left;
                }
                timer.remaining_seconds += timer.interval_seconds;
                completed.push_back({
                    timer.tag,
                    timer.total_loops,
                    infinite ? 0 : timer.loops_left,
                });
            }
            if (completed.size() >= maximum_timer_callbacks_per_frame) {
                // Leave any remaining overdue time intact. It will be drained
                // over following frames rather than fan out unbounded Lua work.
                break;
            }
        }

        script_timers_.erase(
            std::remove_if(
                script_timers_.begin(), script_timers_.end(),
                [](const ScriptTimer& timer) {
                    return timer.total_loops > 0 && timer.loops_left <= 0;
                }
            ),
            script_timers_.end()
        );

        if (scripts_ != nullptr) {
            for (const auto& item : completed) {
                static_cast<void>(scripts_->on_timer_completed(
                    item.tag, item.loops, item.loops_left
                ));
            }
        }
    }

    [[nodiscard]] bool script_invoke_compatibility(
        const std::string_view name,
        const std::span<const ScriptValue> arguments,
        ScriptValue& result,
        std::string& error
    ) {
        result = std::monostate{};
        const auto string_arg = [&](const std::size_t index, std::string_view& out) {
            return index < arguments.size()
                && script_string_value(arguments[index], out);
        };
        const auto number_arg = [&](const std::size_t index, double& out) {
            return index < arguments.size()
                && script_number_value(arguments[index], out);
        };
        const auto integer_arg = [&](const std::size_t index, std::int64_t& out) {
            return index < arguments.size()
                && script_integer_value(arguments[index], out);
        };
        const auto bool_arg = [&](const std::size_t index, bool& out) {
            return index < arguments.size()
                && script_boolean_value(arguments[index], out);
        };
        const auto compatibility_tag = [&](
            const std::size_t index,
            const std::string_view fallback,
            std::string& out
        ) {
            if (index >= arguments.size()
                || std::holds_alternative<std::monostate>(arguments[index])) {
                out.assign(fallback);
                return !out.empty() && out.size() <= 128U;
            }
            if (const auto* string = std::get_if<std::string>(&arguments[index])) {
                if (string->empty() || string->size() > 128U) {
                    return false;
                }
                out = *string;
                return true;
            }
            if (const auto* integer = std::get_if<std::int64_t>(&arguments[index])) {
                out = std::to_string(*integer);
                return out.size() <= 128U;
            }
            if (const auto* number = std::get_if<double>(&arguments[index])) {
                if (!std::isfinite(*number)) {
                    return false;
                }
                char buffer[64]{};
                const int written = std::snprintf(
                    buffer, sizeof(buffer), "%.17g", *number
                );
                if (written <= 0
                    || static_cast<std::size_t>(written) >= sizeof(buffer)) {
                    return false;
                }
                out.assign(buffer, static_cast<std::size_t>(written));
                return out.size() <= 128U;
            }
            if (const auto* boolean = std::get_if<bool>(&arguments[index])) {
                out = *boolean ? "true" : "false";
                return true;
            }
            return false;
        };


// PULSEFORGE_P1_1_17_SHARED_VAR_BRIDGE_V1
if (name == "setVar" || name == "getVar" || name == "removeVar") {
    std::string_view variable;
    if (!string_arg(0U, variable) || variable.empty()
        || variable.size() > 128U) {
        error = "Psych shared variable name must be 1..128 bytes";
        return false;
    }
    std::string key{"__psych_var."};
    key.append(variable);
    if (name == "setVar") {
        if (arguments.size() < 2U) {
            error = "setVar expects name and scalar value";
            return false;
        }
        if (script_property_store_.size() >= 4'096U
            && script_property_store_.find(key)
                == script_property_store_.end()) {
            error = "Psych shared variable store is full";
            return false;
        }
        script_property_store_[std::move(key)] = arguments[1U];
        result = arguments[1U];
        error.clear();
        return true;
    }
    const auto found = script_property_store_.find(key);
    if (name == "getVar") {
        result = found == script_property_store_.end()
            ? ScriptValue{std::monostate{}}
            : found->second;
        error.clear();
        return true;
    }
    if (found != script_property_store_.end()) {
        script_property_store_.erase(found);
    }
    result = true;
    error.clear();
    return true;
}

// PULSEFORGE_P1_1_16_SAFE_CLASS_PROPERTY_BRIDGE_V1
if (name == "getPropertyFromClass" || name == "setPropertyFromClass") {
    std::string_view class_name, property;
    if (!string_arg(0U, class_name) || !string_arg(1U, property)
        || class_name.size() > 256U || property.size() > 256U) {
        error = "class property access expects class and property strings";
        return false;
    }

    const bool client_prefs =
        class_name == "ClientPrefs"
        || class_name == "backend.ClientPrefs"
        || class_name.ends_with(".ClientPrefs");
    // Psych 0.7+ commonly addresses preferences through ClientPrefs.data.*,
    // while older mods pass the bare field. Normalize both onto the same
    // bounded whitelist instead of exposing arbitrary reflection.
    const std::string_view prefs_property =
        client_prefs && property.starts_with("data.")
        ? property.substr(std::string_view{"data."}.size())
        : property;
    const bool conductor =
        class_name == "Conductor"
        || class_name == "backend.Conductor"
        || class_name.ends_with(".Conductor");
    const bool flxg =
        class_name == "FlxG"
        || class_name == "flixel.FlxG"
        || class_name.ends_with(".FlxG");

    if (name == "getPropertyFromClass") {
        if (client_prefs) {
            const auto& settings = gameplay_settings();
            if (prefs_property == "downScroll") result = settings.downscroll;
            else if (prefs_property == "middleScroll") result = settings.middle_scroll;
            else if (prefs_property == "ghostTapping") result = settings.ghost_tapping;
            else if (prefs_property == "practice") result = settings.practice;
            else if (prefs_property == "noFail") result = settings.no_fail;
            else if (prefs_property == "flashing") {
                result = options_.settings.visual.flashing_lights;
            } else if (prefs_property == "lowQuality") {
                result = options_.settings.performance.maximum_performance_mode;
            } else {
                result = std::monostate{};
            }
            error.clear();
            return true;
        }
        if (conductor) {
            if (property == "bpm") {
                result = gameplay_timing().bpm_at(gameplay_song_time_ms());
            } else if (property == "crochet") {
                const double bpm = gameplay_timing().bpm_at(
                    gameplay_song_time_ms()
                );
                result = bpm > 0.0 ? 60'000.0 / bpm : 500.0;
            } else if (property == "stepCrochet") {
                const double bpm = gameplay_timing().bpm_at(
                    gameplay_song_time_ms()
                );
                result = bpm > 0.0 ? 15'000.0 / bpm : 125.0;
            } else if (property == "songPosition") {
                result = gameplay_song_time_ms();
            } else {
                result = std::monostate{};
            }
            error.clear();
            return true;
        }
        if (flxg) {
            if (property == "width") result = static_cast<double>(logical_width);
            else if (property == "height") result = static_cast<double>(logical_height);
            else result = std::monostate{};
            error.clear();
            return true;
        }

        // Reflection outside this explicit whitelist would expose native
        // engine state. Feature probes get nil instead of a fatal error.
        result = std::monostate{};
        error.clear();
        return true;
    }

    if (arguments.size() < 3U) {
        error = "setPropertyFromClass expects class, property, value";
        return false;
    }
    if (!client_prefs) {
        // Native reflection remains intentionally unavailable.
        error.clear();
        return true;
    }

    bool boolean{};
    if (!script_boolean_value(arguments[2U], boolean)) {
        error = "supported ClientPrefs writes expect a boolean";
        return false;
    }
    auto& settings = gameplay_settings();
    if (prefs_property == "downScroll") settings.downscroll = boolean;
    else if (prefs_property == "middleScroll") settings.middle_scroll = boolean;
    else if (prefs_property == "ghostTapping") settings.ghost_tapping = boolean;
    else if (prefs_property == "practice") settings.practice = boolean;
    else if (prefs_property == "noFail") settings.no_fail = boolean;
    else if (prefs_property == "flashing") {
        options_.settings.visual.flashing_lights = boolean;
    } else {
        // Known class but unsupported field: stay non-fatal for mods
        // that persist a preference PulseForge does not expose.
    }
    error.clear();
    return true;
}

        if (name == "getPropertyFromGroup") {
            std::string_view group, property;
            std::int64_t index{};
            if (!string_arg(0U, group) || !integer_arg(1U, index)
                || !string_arg(2U, property)) {
                error = "getPropertyFromGroup expects group, index, property";
                return false;
            }
            return script_get_group_property(group, index, property, result, error);
        }
        if (name == "setPropertyFromGroup") {
            std::string_view group, property;
            std::int64_t index{};
            if (arguments.size() < 4U || !string_arg(0U, group)
                || !integer_arg(1U, index) || !string_arg(2U, property)) {
                error = "setPropertyFromGroup expects group, index, property, value";
                return false;
            }
            return script_set_group_property(
                group, index, property, arguments[3U], error
            );
        }
        if (name == "luaSpriteExists") {
            std::string_view tag;
            if (!string_arg(0U, tag)) {
                error = "luaSpriteExists expects a tag";
                return false;
            }
            result = scene_ != nullptr && scene_->script_has_sprite(tag);
            error.clear();
            return true;
        }
        if (name == "getPropertyLuaSprite" || name == "setPropertyLuaSprite") {
            std::string_view tag, property;
            if (!string_arg(0U, tag) || !string_arg(1U, property)) {
                error = "Lua sprite property command expects tag and property";
                return false;
            }
            std::string path;
            path.reserve(tag.size() + property.size() + 1U);
            path.append(tag);
            path.push_back('.');
            path.append(property);
            if (name == "getPropertyLuaSprite") {
                return script_get_visual_property(path, result, error);
            }
            if (arguments.size() < 3U) {
                error = "setPropertyLuaSprite expects tag, property, value";
                return false;
            }
            return script_set_visual_property(path, arguments[2U], error);
        }
        if (name == "makeGraphic") {
            std::string_view tag, color{"FFFFFF"};
            double width{}, height{};
            if (!string_arg(0U, tag) || !number_arg(1U, width)
                || !number_arg(2U, height)) {
                error = "makeGraphic expects tag, width, height, color";
                return false;
            }
            if (arguments.size() > 3U) static_cast<void>(string_arg(3U, color));
            std::string color_text{color};
            if (!color_text.empty() && color_text.front() == '#') {
                color_text.erase(color_text.begin());
            } else if (color_text.size() > 2U && color_text[0] == '0'
                && (color_text[1] == 'x' || color_text[1] == 'X')) {
                color_text.erase(0U, 2U);
            }
            if (color_text.size() != 6U && color_text.size() != 8U) {
                error = "makeGraphic color must be RRGGBB or AARRGGBB";
                return false;
            }
            char* end{};
            const auto raw = std::strtoul(color_text.c_str(), &end, 16);
            if (end == nullptr || *end != '\0') {
                error = "makeGraphic color is not hexadecimal";
                return false;
            }
            std::uint8_t a = 255U;
            std::uint8_t r{}, g{}, b{};
            if (color_text.size() == 8U) {
                a = static_cast<std::uint8_t>((raw >> 24U) & 0xFFU);
                r = static_cast<std::uint8_t>((raw >> 16U) & 0xFFU);
                g = static_cast<std::uint8_t>((raw >> 8U) & 0xFFU);
                b = static_cast<std::uint8_t>(raw & 0xFFU);
            } else {
                r = static_cast<std::uint8_t>((raw >> 16U) & 0xFFU);
                g = static_cast<std::uint8_t>((raw >> 8U) & 0xFFU);
                b = static_cast<std::uint8_t>(raw & 0xFFU);
            }
            const bool ok = scene_ != nullptr
                && scene_->script_make_graphic(tag, width, height, r, g, b, a);
            if (!ok) error = "makeGraphic could not resolve its Lua sprite";
            else error.clear();
            return ok;
        }
        if (name == "makeLuaSprite" || name == "makeAnimatedLuaSprite") {
            std::string_view tag, image{};
            double x{}, y{};
            if (!string_arg(0U, tag)) {
                error = "makeLuaSprite expects at least a tag";
                return false;
            }
            if (arguments.size() > 1U
                && !std::holds_alternative<std::monostate>(arguments[1U])
                && !string_arg(1U, image)) {
                error = "makeLuaSprite image must be a string or nil";
                return false;
            }
            if (arguments.size() > 2U && !number_arg(2U, x)) {
                error = "makeLuaSprite x must be a finite number";
                return false;
            }
            if (arguments.size() > 3U && !number_arg(3U, y)) {
                error = "makeLuaSprite y must be a finite number";
                return false;
            }
            if (scene_ == nullptr) {
                error = "RuntimeScene is not ready";
                return false;
            }
            return scene_->script_create_sprite(
                tag, image, x, y, name == "makeAnimatedLuaSprite", &error
            );
        }

// PULSEFORGE_P1_1_16_PSYCH_SPRITE_UTILITY_BRIDGE_V1
if (name == "loadGraphic") {
    std::string_view tag, image;
    if (!string_arg(0U, tag) || !string_arg(1U, image)
        || scene_ == nullptr) {
        error = "loadGraphic expects an existing sprite tag and image";
        return false;
    }
    return scene_->script_load_graphic(tag, image, &error);
}
if (name == "precacheImage") {
    std::string_view image;
    if (!string_arg(0U, image) || scene_ == nullptr) {
        error = "precacheImage expects an image id";
        return false;
    }
    return scene_->script_precache_image(image, &error);
}
if (name == "setBlendMode") {
    std::string_view tag, mode;
    if (!string_arg(0U, tag) || !string_arg(1U, mode)
        || scene_ == nullptr
        || !scene_->script_set_blend_mode(tag, mode)) {
        error = "setBlendMode expects an existing sprite and supported blend mode";
        return false;
    }
    error.clear();
    return true;
}

        if (name == "addLuaSprite" || name == "addLuaSprire"
            || name == "removeLuaSprite") {
            std::string_view tag;
            bool front{};
            if (!string_arg(0U, tag)) {
                error = "Lua sprite command expects a tag";
                return false;
            }
            if (arguments.size() > 1U) static_cast<void>(bool_arg(1U, front));
            const bool adding = name == "addLuaSprite" || name == "addLuaSprire";
            const bool ok = scene_ != nullptr && (adding
                ? scene_->script_add_sprite(tag, front)
                : scene_->script_remove_sprite(tag));
            if (!ok) error = "Lua sprite tag was not found";
            else error.clear();
            return ok;
        }
        // PULSEFORGE_P1_5_0_LEGACY_SPRITE_SCROLL_FACTOR_ALIAS_V1
        if (name == "setScrollFactor" || name == "setLuaSpriteScrollFactor") {
            std::string_view tag;
            double x{1.0}, y{1.0};
            if (!string_arg(0U, tag)) {
                error = "setScrollFactor/setLuaSpriteScrollFactor expects a tag";
                return false;
            }
            if (arguments.size() > 1U && !number_arg(1U, x)) {
                error = "setScrollFactor/setLuaSpriteScrollFactor x must be a finite number";
                return false;
            }
            if (arguments.size() > 2U) {
                if (!number_arg(2U, y)) {
                    error = "setScrollFactor/setLuaSpriteScrollFactor y must be a finite number";
                    return false;
                }
            } else {
                y = x;
            }
            const bool ok = scene_ != nullptr
                && scene_->script_set_scroll_factor(tag, x, y);
            if (!ok) error = "setScrollFactor/setLuaSpriteScrollFactor could not resolve its sprite";
            else error.clear();
            return ok;
        }
        if (name == "scaleObject") {
            std::string_view tag;
            double x{}, y{};
            if (!string_arg(0U, tag) || !number_arg(1U, x) || !number_arg(2U, y)
                || scene_ == nullptr
                || !scene_->script_set_number(tag, "scale.x", x)
                || !scene_->script_set_number(tag, "scale.y", y)) {
                error = "scaleObject could not resolve its sprite/scale";
                return false;
            }
            error.clear();
            return true;
        }
        if (name == "setGraphicSize") {
            std::string_view tag;
            double width{}, height{};
            if (!string_arg(0U, tag) || !number_arg(1U, width)) {
                error = "setGraphicSize expects tag and width";
                return false;
            }
            if (arguments.size() > 2U) static_cast<void>(number_arg(2U, height));
            double current_width{}, current_height{};
            if (scene_ == nullptr
                || !scene_->script_get_number(tag, "width", current_width)
                || current_width <= 0.0) {
                error = "setGraphicSize sprite is unavailable";
                return false;
            }
            const double sx = width / current_width;
            double old_sx{1.0}, old_sy{1.0};
            static_cast<void>(scene_->script_get_number(tag, "scale.x", old_sx));
            static_cast<void>(scene_->script_get_number(tag, "scale.y", old_sy));
            static_cast<void>(scene_->script_set_number(tag, "scale.x", old_sx * sx));
            if (height > 0.0 && scene_->script_get_number(tag, "height", current_height)
                && current_height > 0.0) {
                static_cast<void>(scene_->script_set_number(
                    tag, "scale.y", old_sy * height / current_height
                ));
            } else {
                static_cast<void>(scene_->script_set_number(tag, "scale.y", old_sy * sx));
            }
            error.clear();
            return true;
        }
        if (name == "updateHitbox") {
            error.clear();
            return true;
        }
        if (name == "makeLuaText") {
            std::string_view tag, contents;
            double width{}, x{}, y{};
            if (!string_arg(0U, tag) || !string_arg(1U, contents)
                || !number_arg(2U, width) || !number_arg(3U, x)
                || !number_arg(4U, y) || tag.empty() || tag.size() > 128U
                || contents.size() > 4'096U || width < 0.0) {
                error = "makeLuaText expects tag, text, width, x, y";
                return false;
            }
            if (!script_hud_objects_.contains(tag)
                && script_hud_objects_.size() >= 256U) {
                error = "Lua HUD object count exceeded the 256-object limit";
                return false;
            }
            ScriptHudObjectState state;
            state.x = std::clamp(x, -100'000.0, 100'000.0);
            state.y = std::clamp(y, -100'000.0, 100'000.0);
            state.text_object = true;
            state.text_added = false;
            state.text_width = std::clamp(width, 0.0, 100'000.0);
            state.text.assign(contents);
            script_hud_objects_.insert_or_assign(std::string(tag), std::move(state));
            error.clear();
            return true;
        }

// PULSEFORGE_P1_1_16_PSYCH_TEXT_UTILITY_BRIDGE_V1
if (name == "luaTextExists") {
    std::string_view tag;
    if (!string_arg(0U, tag)) {
        error = "luaTextExists expects a tag";
        return false;
    }
    const auto found = script_hud_objects_.find(tag);
    result = found != script_hud_objects_.end()
        && found->second.text_object;
    error.clear();
    return true;
}
if (name == "removeLuaText") {
    std::string_view tag;
    bool destroy{true};
    if (!string_arg(0U, tag)) {
        error = "removeLuaText expects a tag";
        return false;
    }
    if (arguments.size() > 1U
        && !bool_arg(1U, destroy)) {
        error = "removeLuaText destroy flag must be boolean";
        return false;
    }
    auto found = script_hud_objects_.find(tag);
    if (found == script_hud_objects_.end()
        || !found->second.text_object) {
        error.clear();
        return true;
    }
    if (destroy) {
        script_hud_objects_.erase(found);
    } else {
        found->second.text_added = false;
        found->second.visible = false;
    }
    error.clear();
    return true;
}
if (name == "getTextString") {
    std::string_view tag;
    if (!string_arg(0U, tag)) {
        error = "getTextString expects a tag";
        return false;
    }
    const auto found = script_hud_objects_.find(tag);
    if (found == script_hud_objects_.end()
        || !found->second.text_object) {
        result = std::string{};
    } else {
        result = found->second.text;
    }
    error.clear();
    return true;
}
if (name == "setTextColor") {
    std::string_view tag, color_text;
    if (!string_arg(0U, tag) || !string_arg(1U, color_text)) {
        error = "setTextColor expects tag and hexadecimal color";
        return false;
    }
    auto found = script_hud_objects_.find(tag);
    SDL_Color color{};
    if (found == script_hud_objects_.end()
        || !found->second.text_object
        || !parse_psych_color(color_text, color)) {
        error = "setTextColor could not resolve text/color";
        return false;
    }
    found->second.text_color = color;
    error.clear();
    return true;
}
if (name == "setTextBorder") {
    std::string_view tag, color_text;
    double size{};
    if (!string_arg(0U, tag) || !number_arg(1U, size)
        || !string_arg(2U, color_text)) {
        error = "setTextBorder expects tag, size, color";
        return false;
    }
    auto found = script_hud_objects_.find(tag);
    SDL_Color color{};
    if (found == script_hud_objects_.end()
        || !found->second.text_object
        || !parse_psych_color(color_text, color)) {
        error = "setTextBorder could not resolve text/color";
        return false;
    }
    found->second.border_size = std::clamp(size, 0.0, 8.0);
    found->second.border_color = color;
    error.clear();
    return true;
}
if (name == "setTextWidth") {
    std::string_view tag;
    double width{};
    if (!string_arg(0U, tag) || !number_arg(1U, width)) {
        error = "setTextWidth expects tag and width";
        return false;
    }
    auto found = script_hud_objects_.find(tag);
    if (found == script_hud_objects_.end()
        || !found->second.text_object) {
        error = "setTextWidth could not resolve its Lua text object";
        return false;
    }
    found->second.text_width = std::clamp(width, 0.0, 100'000.0);
    error.clear();
    return true;
}
if (name == "setTextItalic") {
    std::string_view tag;
    bool italic{};
    if (!string_arg(0U, tag) || !bool_arg(1U, italic)) {
        error = "setTextItalic expects tag and boolean";
        return false;
    }
    auto found = script_hud_objects_.find(tag);
    if (found == script_hud_objects_.end()
        || !found->second.text_object) {
        error = "setTextItalic could not resolve its Lua text object";
        return false;
    }
    found->second.italic = italic;
    error.clear();
    return true;
}
if (name == "setTextFont") {
    std::string_view tag, font;
    if (!string_arg(0U, tag) || !string_arg(1U, font)
        || font.size() > 512U) {
        error = "setTextFont expects tag and bounded font id";
        return false;
    }
    auto found = script_hud_objects_.find(tag);
    if (found == script_hud_objects_.end()
        || !found->second.text_object) {
        error = "setTextFont could not resolve its Lua text object";
        return false;
    }
    // The current HUD fallback is the built-in debug bitmap font.
    // Preserve the requested identity so a future font backend can use
    // it, while keeping today's call non-fatal and deterministic.
    found->second.font.assign(font);
    error.clear();
    return true;
}

        if (name == "setTextAlignment") {
            std::string_view tag, alignment;
            if (!string_arg(0U, tag) || !string_arg(1U, alignment)) {
                error = "setTextAlignment expects tag and alignment";
                return false;
            }
            auto found = script_hud_objects_.find(tag);
            if (found == script_hud_objects_.end() || !found->second.text_object) {
                error = "setTextAlignment could not resolve its Lua text object";
                return false;
            }
            if (alignment == "left" || alignment == "Left") {
                found->second.text_alignment = "left";
            } else if (alignment == "center" || alignment == "Center") {
                found->second.text_alignment = "center";
            } else if (alignment == "right" || alignment == "Right") {
                found->second.text_alignment = "right";
            } else {
                error = "setTextAlignment expects left, center, or right";
                return false;
            }
            error.clear();
            return true;
        }
        if (name == "setTextSize") {
            std::string_view tag;
            double size{};
            if (!string_arg(0U, tag) || !number_arg(1U, size)) {
                error = "setTextSize expects tag and size";
                return false;
            }
            auto found = script_hud_objects_.find(tag);
            if (found == script_hud_objects_.end() || !found->second.text_object) {
                error = "setTextSize could not resolve its Lua text object";
                return false;
            }
            found->second.text_size = std::clamp(size, 1.0, 256.0);
            error.clear();
            return true;
        }
        if (name == "addLuaText") {
            std::string_view tag;
            if (!string_arg(0U, tag)) {
                error = "addLuaText expects a tag";
                return false;
            }
            auto found = script_hud_objects_.find(tag);
            if (found == script_hud_objects_.end() || !found->second.text_object) {
                error = "addLuaText could not resolve its Lua text object";
                return false;
            }
            found->second.text_added = true;
            found->second.visible = true;
            error.clear();
            return true;
        }
        if (name == "setTextString") {
            std::string_view tag, contents;
            if (!string_arg(0U, tag) || !string_arg(1U, contents)
                || contents.size() > 4'096U) {
                error = "setTextString expects tag and bounded text";
                return false;
            }
            auto found = script_hud_objects_.find(tag);
            if (found == script_hud_objects_.end() || !found->second.text_object) {
                error = "setTextString could not resolve its Lua text object";
                return false;
            }
            found->second.text.assign(contents);
            error.clear();
            return true;
        }

// PULSEFORGE_P1_1_16_PSYCH_HUD_COLOR_BRIDGE_V1
if (name == "setHealthBarColors" || name == "setTimeBarColors") {
    std::string_view first_text, second_text;
    if (!string_arg(0U, first_text) || !string_arg(1U, second_text)) {
        error = "bar color command expects two hexadecimal colors";
        return false;
    }
    SDL_Color first{}, second{};
    if (!parse_psych_color(first_text, first)
        || !parse_psych_color(second_text, second)) {
        error = "bar color command received an invalid color";
        return false;
    }
    if (name == "setHealthBarColors") {
        script_health_opponent_color_ = first;
        script_health_player_color_ = second;
    } else {
        script_time_background_color_ = first;
        script_time_fill_color_ = second;
    }
    error.clear();
    return true;
}

        if (name == "setObjectCamera") {
            std::string_view tag, camera;
            if (!string_arg(0U, tag) || !string_arg(1U, camera)) {
                error = "setObjectCamera expects tag and camera";
                return false;
            }
            if (auto hud = script_hud_objects_.find(tag);
                hud != script_hud_objects_.end()) {
                const bool other = camera == "other" || camera == "camOther"
                    || camera == "Other" || camera == "CamOther";
                const bool hud_camera = camera == "hud" || camera == "camHUD"
                    || camera == "HUD" || camera == "CamHUD";
                if (other || hud_camera) {
                    hud->second.camera_other = other;
                    error.clear();
                    return true;
                }
            }
            if (scene_ == nullptr || !scene_->script_set_camera(tag, camera)) {
                error = "setObjectCamera could not resolve its sprite/HUD object";
                return false;
            }
            error.clear();
            return true;
        }
        if (name == "setObjectOrder" || name == "getObjectOrder") {
            std::string_view tag;
            if (!string_arg(0U, tag) || scene_ == nullptr) {
                error = "object order command expects an existing tag";
                return false;
            }
            if (name == "getObjectOrder") {
                std::int64_t order{};
                if (!scene_->script_get_order(tag, order)) {
                    error = "object order tag was not found";
                    return false;
                }
                result = order;
                error.clear();
                return true;
            }
            std::int64_t order{};
            if (!integer_arg(1U, order) || !scene_->script_set_order(tag, order)) {
                error = "setObjectOrder expects tag and integer order";
                return false;
            }
            error.clear();
            return true;
        }
        if (name == "screenCenter") {
            std::string_view tag, axis{"xy"};
            if (!string_arg(0U, tag)) {
                error = "screenCenter expects a sprite tag";
                return false;
            }
            if (arguments.size() > 1U) static_cast<void>(string_arg(1U, axis));
            const bool horizontal = axis.find('x') != std::string_view::npos
                || axis.find('X') != std::string_view::npos;
            const bool vertical = axis.find('y') != std::string_view::npos
                || axis.find('Y') != std::string_view::npos;
            const bool ok = scene_ != nullptr
                && scene_->script_screen_center(tag, horizontal, vertical);
            if (!ok) error = "screenCenter sprite was not found";
            else error.clear();
            return ok;
        }
        if (name == "addAnimationByPrefix" || name == "addAnimationByIndices") {
            std::string_view tag, animation, prefix;
            double fps{24.0};
            bool loop{};
            if (!string_arg(0U, tag) || !string_arg(1U, animation)
                || !string_arg(2U, prefix)) {
                error = "addAnimationByPrefix expects tag, animation, prefix";
                return false;
            }
            if (arguments.size() > 3U) static_cast<void>(number_arg(3U, fps));
            if (arguments.size() > 4U) static_cast<void>(bool_arg(4U, loop));
            const bool ok = scene_ != nullptr
                && scene_->script_add_animation(tag, animation, prefix, fps, loop);
            if (!ok) error = "animation prefix did not resolve in the sprite atlas";
            else error.clear();
            return ok;
        }
        if (name == "cameraSetTarget") {
            // PULSEFORGE_P1_1_10_CAMERA_SET_TARGET_V1
            std::string_view target;
            if (!string_arg(0U, target)) {
                error = "cameraSetTarget expects a character target";
                return false;
            }

            double target_x{}, target_y{};
            if (scene_ == nullptr
                || !scene_->script_get_camera_target(
                    target, target_x, target_y
                )) {
                error = "cameraSetTarget could not resolve its character";
                return false;
            }

            script_cam_game_x_ = std::clamp(
                target_x - static_cast<double>(logical_width) * 0.5,
                -100'000.0,
                100'000.0
            );
            script_cam_game_y_ = std::clamp(
                target_y - static_cast<double>(logical_height) * 0.5,
                -100'000.0,
                100'000.0
            );
            scene_->script_set_game_camera(
                script_cam_game_x_,
                script_cam_game_y_,
                script_cam_game_zoom_,
                script_cam_game_angle_,
                script_cam_game_alpha_
            );
            error.clear();
            return true;
        }
        if (name == "characterPlayAnim" || name == "playAnim"
            || name == "objectPlayAnimation") {
            std::string_view tag, animation;
            bool force{};
            if (!string_arg(0U, tag) || !string_arg(1U, animation)) {
                error = "play animation expects tag and animation";
                return false;
            }
            if (arguments.size() > 2U) static_cast<void>(bool_arg(2U, force));
            const bool ok = scene_ != nullptr
                && scene_->script_play_animation(
                    tag, animation, force, gameplay_song_time_ms()
                );
            if (!ok) error = "requested sprite animation was not found";
            else error.clear();
            return ok;
        }

        // PULSEFORGE_P1_5_0_PSYCH_COLOR_TWEEN_V1
        if (name == "doTweenColor") {
            std::string_view object_view, color_view, easing_view{"linear"};
            double duration{};
            if (!string_arg(1U, object_view) || !string_arg(2U, color_view)
                || !number_arg(3U, duration)) {
                error = "doTweenColor expects tag, object, color, duration";
                return false;
            }
            SDL_Color color{};
            if (!parse_psych_color(color_view, color)) {
                error = "doTweenColor color must be RRGGBB or AARRGGBB hex";
                return false;
            }
            if (arguments.size() > 4U) {
                static_cast<void>(string_arg(4U, easing_view));
            }
            std::string fallback{"__pf_auto_doTweenColor_"};
            fallback.append(object_view);
            std::string tag;
            if (!compatibility_tag(0U, fallback, tag)) {
                error = "doTweenColor tag must be string/number/bool/nil";
                return false;
            }
            const std::uint32_t packed = 0xFF000000U
                | (static_cast<std::uint32_t>(color.r) << 16U)
                | (static_cast<std::uint32_t>(color.g) << 8U)
                | static_cast<std::uint32_t>(color.b);
            return script_start_tween(
                std::move(tag), ScriptTweenTarget::object,
                std::string(object_view), "color", 0U,
                static_cast<double>(packed), duration,
                std::string(easing_view), error
            );
        }

        const bool object_tween = name == "doTweenX" || name == "doTweenY"
            || name == "doTweenAngle" || name == "doTweenAlpha"
            || name == "doTweenZoom"
            || name == "doTweenScaleX" || name == "doTweenScaleY";
        if (object_tween) {
            std::string_view object_view, easing_view{"linear"};
            double destination{}, duration{};
            if (!string_arg(1U, object_view)
                || !number_arg(2U, destination) || !number_arg(3U, duration)) {
                error = "doTween* expects tag, object, value, duration";
                return false;
            }
            std::string fallback{"__pf_auto_"};
            fallback.append(name);
            fallback.push_back('_');
            fallback.append(object_view);
            std::string tag;
            if (!compatibility_tag(0U, fallback, tag)) {
                error = "doTween* tag must be string/number/bool/nil";
                return false;
            }
            if (arguments.size() > 4U) static_cast<void>(string_arg(4U, easing_view));
            std::string property = name == "doTweenX" ? "x"
                : name == "doTweenY" ? "y"
                : name == "doTweenAngle" ? "angle"
                : name == "doTweenAlpha" ? "alpha"
                : name == "doTweenScaleX" ? "scale.x"
                : name == "doTweenScaleY" ? "scale.y"
                : "zoom";
            ScriptTweenTarget target = ScriptTweenTarget::object;
            std::string object{object_view};
            if (object_view == "camGame") target = ScriptTweenTarget::camera_game;
            else if (object_view == "camHUD") target = ScriptTweenTarget::camera_hud;
            else if (object_view.ends_with(".scale")
                && (name == "doTweenX" || name == "doTweenY")) {
                object.resize(object.size() - std::string_view{".scale"}.size());
                property = name == "doTweenX" ? "scale.x" : "scale.y";
            }
            return script_start_tween(
                std::move(tag), target, std::move(object),
                std::move(property), 0U, destination, duration,
                std::string(easing_view), error
            );
        }
        const bool note_tween = name == "noteTweenX" || name == "noteTweenY"
            || name == "noteTweenAngle" || name == "noteTweenAlpha"
            || name == "noteTweenScaleX" || name == "noteTweenScaleY";
        if (note_tween) {
            std::string_view easing_view{"linear"};
            std::int64_t index{};
            double destination{}, duration{};
            if (!integer_arg(1U, index)
                || !number_arg(2U, destination) || !number_arg(3U, duration)
                || index < 0 || static_cast<std::size_t>(index) >= script_strums_.size()) {
                error = "noteTween* expects tag, strum index, value, duration";
                return false;
            }
            std::string fallback{"__pf_auto_"};
            fallback.append(name);
            fallback.push_back('_');
            fallback.append(std::to_string(index));
            std::string tag;
            if (!compatibility_tag(0U, fallback, tag)) {
                error = "noteTween* tag must be string/number/bool/nil";
                return false;
            }
            if (arguments.size() > 4U) static_cast<void>(string_arg(4U, easing_view));
            const std::string property = name == "noteTweenX" ? "x"
                : name == "noteTweenY" ? "y"
                : name == "noteTweenAngle" ? "angle"
                : name == "noteTweenScaleX" ? "scale.x"
                : name == "noteTweenScaleY" ? "scale.y"
                : "alpha";
            return script_start_tween(
                std::move(tag), ScriptTweenTarget::strum, {}, property,
                static_cast<std::size_t>(index), destination, duration,
                std::string(easing_view), error
            );
        }
        if (name == "cancelTween") {
            std::string_view tag;
            if (!string_arg(0U, tag)) {
                error = "cancelTween expects a tag";
                return false;
            }
            script_tweens_.erase(
                std::remove_if(
                    script_tweens_.begin(), script_tweens_.end(),
                    [&](const ScriptTween& tween) { return tween.tag == tag; }
                ),
                script_tweens_.end()
            );
            error.clear();
            return true;
        }
        if (name == "runTimer") {
            std::string_view tag;
            double interval{};
            std::int64_t loops{1};
            if (!string_arg(0U, tag) || !number_arg(1U, interval)
                || tag.empty() || tag.size() > 128U || interval < 0.0) {
                error = "runTimer expects tag and a finite non-negative time";
                return false;
            }
            if (arguments.size() > 2U && !integer_arg(2U, loops)) {
                error = "runTimer loops must be an integer";
                return false;
            }
            if (loops < 0 || loops > 1'000'000) {
                error = "runTimer loops is outside the bounded range [0, 1000000]";
                return false;
            }
            // Zero-duration FlxTimer calls are effectively deferred. Keep a 1ms
            // minimum here so a malicious/infinite timer cannot spin in-frame.
            interval = std::max(interval, 0.001);
            script_timers_.erase(
                std::remove_if(
                    script_timers_.begin(), script_timers_.end(),
                    [&](const ScriptTimer& timer) { return timer.tag == tag; }
                ),
                script_timers_.end()
            );
            if (script_timers_.size() >= 256U) {
                error = "Lua timer count exceeded the 256-timer runtime limit";
                return false;
            }
            script_timers_.push_back(ScriptTimer{
                std::string(tag), interval, interval, loops, loops
            });
            error.clear();
            return true;
        }
        if (name == "cancelTimer") {
            std::string_view tag;
            if (!string_arg(0U, tag)) {
                error = "cancelTimer expects a tag";
                return false;
            }
            script_timers_.erase(
                std::remove_if(
                    script_timers_.begin(), script_timers_.end(),
                    [&](const ScriptTimer& timer) { return timer.tag == tag; }
                ),
                script_timers_.end()
            );
            error.clear();
            return true;
        }
        if (name == "cameraFlash") {
            screen_flash_ = 1.0F;
            error.clear();
            return true;
        }
        if (name == "cameraShake") {
            beat_pulse_ = std::max(beat_pulse_, 0.65F);
            error.clear();
            return true;
        }
        if (name == "keyboardJustPressed") {
            std::string_view key;
            if (!string_arg(0U, key) || key.empty() || key.size() > 64U) {
                error = "keyboardJustPressed expects a bounded key name";
                return false;
            }
            // PULSEFORGE_P1_1_4_COUNTDOWN_INPUT_GATE_V1
            const auto psych_scancode = [](const std::string_view key_name) {
                if (key_name == "LEFT" || key_name == "left") return SDL_SCANCODE_LEFT;
                if (key_name == "RIGHT" || key_name == "right") return SDL_SCANCODE_RIGHT;
                if (key_name == "UP" || key_name == "up") return SDL_SCANCODE_UP;
                if (key_name == "DOWN" || key_name == "down") return SDL_SCANCODE_DOWN;
                if (key_name == "SPACE" || key_name == "space") return SDL_SCANCODE_SPACE;
                if (key_name == "X" || key_name == "x") return SDL_SCANCODE_X;
                if (key_name == "Z" || key_name == "z") return SDL_SCANCODE_Z;
                if (key_name == "ENTER" || key_name == "RETURN"
                    || key_name == "enter" || key_name == "return") {
                    return SDL_SCANCODE_RETURN;
                }
                if (key_name == "ESC" || key_name == "ESCAPE"
                    || key_name == "esc" || key_name == "escape") {
                    return SDL_SCANCODE_ESCAPE;
                }
                const std::string native_name{key_name};
                return SDL_GetScancodeFromName(native_name.c_str());
            };
            const SDL_Scancode scancode = psych_scancode(key);
            const auto index = static_cast<int>(scancode);
            result = index > static_cast<int>(SDL_SCANCODE_UNKNOWN)
                && index < SDL_SCANCODE_COUNT
                && script_just_pressed_scancodes_[static_cast<std::size_t>(index)];
            error.clear();
            return true;
        }
        // PULSEFORGE_P1_1_17_MODCHART_RUNTIME_BRIDGE_V1
        if (name == "getSongLength") {
            const double media_duration = audio_.duration_ms();
            result = std::isfinite(media_duration) && media_duration > 0.0
                ? media_duration
                : gameplay_content_duration_ms();
            error.clear();
            return true;
        }
        if (name == "restartSong") {
            script_restart_requested_ = true;
            result = true;
            error.clear();
            return true;
        }
        if (name == "endSong") {
            // Defer completion to the next frame's normal gameplay path so
            // onEndSong is produced by the authoritative session event flow.
            script_force_song_ended_ = true;
            result = true;
            error.clear();
            return true;
        }
        if (name == "getCharacterX" || name == "getCharacterY"
            || name == "setCharacterX" || name == "setCharacterY") {
            std::string_view requested;
            if (!string_arg(0U, requested) || requested.empty()
                || requested.size() > 128U || scene_ == nullptr) {
                error = "character position helper expects a character tag";
                return false;
            }
            const std::string_view tag =
                requested == "bf" || requested == "player"
                    ? std::string_view{"boyfriend"}
                : requested == "opponent"
                    ? std::string_view{"dad"}
                : requested;
            const bool horizontal =
                name == "getCharacterX" || name == "setCharacterX";
            const std::string_view property = horizontal ? "x" : "y";
            if (name.starts_with("get")) {
                double coordinate{};
                if (!scene_->script_get_number(tag, property, coordinate)) {
                    error = "character position is unavailable";
                    return false;
                }
                result = coordinate;
                error.clear();
                return true;
            }
            double coordinate{};
            if (!number_arg(1U, coordinate)) {
                error = "setCharacterX/Y expects a finite coordinate";
                return false;
            }
            if (!scene_->script_set_number(
                    tag,
                    property,
                    std::clamp(coordinate, -1.0e7, 1.0e7)
                )) {
                error = "character position is unavailable";
                return false;
            }
            result = true;
            error.clear();
            return true;
        }
        if (name == "getCameraScrollX" || name == "getCameraFollowX") {
            result = script_cam_game_x_
                + static_cast<double>(logical_width) * 0.5;
            error.clear();
            return true;
        }
        if (name == "getCameraScrollY" || name == "getCameraFollowY") {
            result = script_cam_game_y_
                + static_cast<double>(logical_height) * 0.5;
            error.clear();
            return true;
        }
        if (name == "addCameraScroll" || name == "addCameraFollowPoint") {
            double offset_x{}, offset_y{};
            if (!number_arg(0U, offset_x) || !number_arg(1U, offset_y)) {
                error = "camera offset helper expects finite x and y";
                return false;
            }
            script_cam_game_x_ = std::clamp(
                script_cam_game_x_ + offset_x, -1.0e7, 1.0e7
            );
            script_cam_game_y_ = std::clamp(
                script_cam_game_y_ + offset_y, -1.0e7, 1.0e7
            );
            if (scene_ != nullptr) {
                scene_->script_set_game_camera(
                    script_cam_game_x_,
                    script_cam_game_y_,
                    script_cam_game_zoom_,
                    script_cam_game_angle_,
                    script_cam_game_alpha_
                );
            }
            result = true;
            error.clear();
            return true;
        }

        // PULSEFORGE_P1_1_18_PSYCH_AUDIO_HOST_BRIDGE_V1
        constexpr std::string_view psych_music_tag{
            "__pulseforge_psych_music__"
        };
        if (name == "precacheSound") {
            std::string_view sound_id;
            if (!string_arg(0U, sound_id)) {
                error = "precacheSound expects a sound id";
                return false;
            }
            const auto path = resolve_script_audio_asset(
                sound_id,
                ScriptAudioKind::sound
            );
            if (!path.has_value()) {
                error = "Psych sound asset was not found: ";
                error.append(sound_id);
                return false;
            }
            std::string audio_error;
            const bool ok = audio_.precache_sound(*path, &audio_error);
            result = ok;
            error = std::move(audio_error);
            return ok;
        }
        if (name == "playSound") {
            std::string_view sound_id;
            double volume{1.0};
            std::string tag;
            if (!string_arg(0U, sound_id)) {
                error = "playSound expects a sound id";
                return false;
            }
            if (arguments.size() > 1U && !number_arg(1U, volume)) {
                error = "playSound volume must be finite";
                return false;
            }
            if (arguments.size() > 2U
                && !std::holds_alternative<std::monostate>(arguments[2U])
                && !compatibility_tag(2U, "sound", tag)) {
                error = "playSound tag must be empty or <= 128 bytes";
                return false;
            }
            const auto path = resolve_script_audio_asset(
                sound_id,
                ScriptAudioKind::sound
            );
            if (!path.has_value()) {
                error = "Psych sound asset was not found: ";
                error.append(sound_id);
                return false;
            }
            std::string audio_error;
            const bool ok = audio_.play_sound(
                *path,
                static_cast<float>(std::clamp(volume, 0.0, 2.0)),
                tag,
                false,
                &audio_error
            );
            result = ok;
            error = std::move(audio_error);
            return ok;
        }
        if (name == "stopSound" || name == "pauseSound"
            || name == "resumeSound" || name == "soundPlaying"
            || name == "soundFadeCancel") {
            std::string_view tag;
            if (!string_arg(0U, tag) || tag.empty() || tag.size() > 128U) {
                error = "Psych sound operation expects a non-empty tag";
                return false;
            }
            bool ok = false;
            if (name == "stopSound") {
                ok = audio_.stop_sound(tag);
            } else if (name == "pauseSound") {
                ok = audio_.pause_sound(tag);
            } else if (name == "resumeSound") {
                ok = audio_.resume_sound(tag);
            } else if (name == "soundPlaying") {
                result = audio_.sound_playing(tag);
                error.clear();
                return true;
            } else {
                ok = audio_.cancel_sound_fade(tag);
            }
            result = ok;
            error.clear();
            return true;
        }
        if (name == "getSoundVolume" || name == "getSoundTime") {
            std::string_view tag;
            if (!string_arg(0U, tag) || tag.empty() || tag.size() > 128U) {
                error = "Psych sound getter expects a non-empty tag";
                return false;
            }
            if (name == "getSoundVolume") {
                float volume{};
                if (!audio_.sound_volume(tag, volume)) {
                    result = 0.0;
                    error.clear();
                    return true;
                }
                result = static_cast<double>(volume);
            } else {
                double position_ms{};
                if (!audio_.sound_time_ms(tag, position_ms)) {
                    result = 0.0;
                    error.clear();
                    return true;
                }
                result = position_ms;
            }
            error.clear();
            return true;
        }
        if (name == "setSoundVolume" || name == "setSoundTime") {
            std::string_view tag;
            double value{};
            if (!string_arg(0U, tag) || tag.empty() || tag.size() > 128U
                || !number_arg(1U, value)) {
                error = "Psych sound setter expects tag and finite value";
                return false;
            }
            const bool ok = name == "setSoundVolume"
                ? audio_.set_sound_volume(
                      tag,
                      static_cast<float>(std::clamp(value, 0.0, 2.0))
                  )
                : audio_.set_sound_time_ms(tag, value);
            result = ok;
            error.clear();
            return true;
        }
        if (name == "soundFadeIn" || name == "soundFadeOut") {
            std::string_view tag;
            double duration{};
            if (!string_arg(0U, tag) || tag.empty() || tag.size() > 128U
                || !number_arg(1U, duration)) {
                error = "soundFadeIn/Out expects tag and duration";
                return false;
            }

            double from = 0.0;
            double to = name == "soundFadeIn" ? 1.0 : 0.0;
            if (name == "soundFadeIn") {
                if (arguments.size() > 2U && !number_arg(2U, from)) {
                    error = "soundFadeIn from-volume must be finite";
                    return false;
                }
                if (arguments.size() > 3U && !number_arg(3U, to)) {
                    error = "soundFadeIn to-volume must be finite";
                    return false;
                }
            } else {
                float current{1.0F};
                static_cast<void>(audio_.sound_volume(tag, current));
                from = current;
                if (arguments.size() > 2U && !number_arg(2U, to)) {
                    error = "soundFadeOut target volume must be finite";
                    return false;
                }
            }
            const bool ok = audio_.fade_sound(
                tag,
                duration,
                static_cast<float>(std::clamp(from, 0.0, 2.0)),
                static_cast<float>(std::clamp(to, 0.0, 2.0))
            );
            result = ok;
            error.clear();
            return true;
        }
        if (name == "playMusic") {
            std::string_view track;
            double volume{1.0};
            bool loop{false};
            if (!string_arg(0U, track) || track.empty() || track.size() > 512U) {
                error = "playMusic expects a bounded track id";
                return false;
            }
            if (arguments.size() > 1U && !number_arg(1U, volume)) {
                error = "playMusic volume must be finite";
                return false;
            }
            if (arguments.size() > 2U && !bool_arg(2U, loop)) {
                error = "playMusic loop flag must be boolean";
                return false;
            }
            const auto path = resolve_script_audio_asset(
                track,
                ScriptAudioKind::music
            );
            if (!path.has_value()) {
                error = "Psych music asset was not found: ";
                error.append(track);
                return false;
            }
            std::string audio_error;
            const bool ok = audio_.play_sound(
                *path,
                static_cast<float>(std::clamp(volume, 0.0, 2.0)),
                psych_music_tag,
                loop,
                &audio_error
            );
            result = ok;
            error = std::move(audio_error);
            return ok;
        }
        if (name == "stopMusic" || name == "pauseMusic"
            || name == "resumeMusic") {
            const bool ok = name == "stopMusic"
                ? audio_.stop_sound(psych_music_tag)
                : (name == "pauseMusic"
                    ? audio_.pause_sound(psych_music_tag)
                    : audio_.resume_sound(psych_music_tag));
            result = ok;
            error.clear();
            return true;
        }
        if (name == "startCountdown") {
            script_countdown_release_requested_ = true;
            result = true;
            error.clear();
            return true;
        }
        // PULSEFORGE_P1_1_18_DYNAMIC_SCRIPT_HOST_BRIDGE_V1
        if (name == "addLuaScript" || name == "removeLuaScript"
            || name == "luaScriptExists") {
            std::string_view requested;
            bool ignore_already_running{false};
            if (!string_arg(0U, requested)) {
                error = "dynamic Lua script operation expects a script id";
                return false;
            }
            if (arguments.size() > 1U
                && !bool_arg(1U, ignore_already_running)) {
                error = "dynamic Lua script ignore flag must be boolean";
                return false;
            }
            const auto path = resolve_dynamic_lua_script(requested);
            if (!path.has_value()) {
                if (name == "luaScriptExists") {
                    result = false;
                    error.clear();
                    return true;
                }
                error = "dynamic Lua script was not found: ";
                error.append(requested);
                return false;
            }
            const std::string source_name{"@" + path_utf8(*path)};
            const bool loaded = scripts_ != nullptr
                && scripts_->contains(source_name);
            const bool pending_add = std::any_of(
                script_dynamic_requests_.begin(),
                script_dynamic_requests_.end(),
                [&](const DynamicScriptRequest& request) {
                    return request.action == DynamicScriptAction::add
                        && request.path == *path;
                }
            );
            const bool pending_remove = std::any_of(
                script_dynamic_requests_.begin(),
                script_dynamic_requests_.end(),
                [&](const DynamicScriptRequest& request) {
                    return request.action == DynamicScriptAction::remove
                        && request.path == *path;
                }
            );

            if (name == "luaScriptExists") {
                result = (loaded || pending_add) && !pending_remove;
                error.clear();
                return true;
            }
            if (script_dynamic_requests_.size() >= 64U) {
                error = "dynamic Lua script request queue is full";
                return false;
            }

            if (name == "addLuaScript") {
                if (loaded || pending_add) {
                    result = false;
                    error.clear();
                    return true;
                }
                script_dynamic_requests_.push_back({
                    DynamicScriptAction::add,
                    *path,
                    ignore_already_running,
                });
            } else {
                if ((!loaded && !pending_add) || pending_remove) {
                    result = false;
                    error.clear();
                    return true;
                }
                script_dynamic_requests_.push_back({
                    DynamicScriptAction::remove,
                    *path,
                    ignore_already_running,
                });
            }
            result = true;
            error.clear();
            return true;
        }

        if (name == "getMidpointX" || name == "getMidpointY"
            || name == "getGraphicMidpointX"
            || name == "getGraphicMidpointY") {
            std::string_view tag;
            if (!string_arg(0U, tag) || scene_ == nullptr) {
                error = "getMidpoint* expects an existing sprite tag";
                return false;
            }
            double coordinate{}, extent{};
            const bool horizontal = name == "getMidpointX"
                || name == "getGraphicMidpointX";
            const auto property = horizontal ? "x" : "y";
            const auto extent_property = horizontal ? "width" : "height";
            if (!scene_->script_get_number(tag, property, coordinate)
                || !scene_->script_get_number(tag, extent_property, extent)) {
                error = "getMidpoint* sprite is unavailable";
                return false;
            }
            result = coordinate + extent * 0.5;
            error.clear();
            return true;
        }
        if (name == "getMouseX" || name == "getMouseY") {
            // PULSEFORGE_P1_1_17_REAL_MOUSE_COORDINATES_V1
            float window_x = 0.0F;
            float window_y = 0.0F;
            static_cast<void>(SDL_GetMouseState(&window_x, &window_y));
            float render_x = window_x;
            float render_y = window_y;
            if (renderer_ != nullptr) {
                static_cast<void>(SDL_RenderCoordinatesFromWindow(
                    renderer_,
                    window_x,
                    window_y,
                    &render_x,
                    &render_y
                ));
            }
            result = static_cast<double>(
                name == "getMouseX" ? render_x : render_y
            );
            error.clear();
            return true;
        }

        // PULSEFORGE_P1_1_12_SHADER_API_BRIDGE_V1
        if (name == "initLuaShader") {
            std::string_view shader_id;
            if (!string_arg(0U, shader_id) || shader_id.empty()
                || shader_id.size() > 192U) {
                error = "initLuaShader expects a bounded shader identifier";
                return false;
            }

            const auto* entry = shader_catalog_ != nullptr
                ? shader_catalog_->find(shader_id)
                : nullptr;
            if (entry == nullptr || !entry->valid) {
                // Psych's API is feature-probing friendly. A missing custom
                // shader must not abort a song on a non-programmable backend.
                result = false;
                error.clear();
                return true;
            }
            const auto loaded = shader_catalog_->load(entry->id);
            if (!loaded) {
                result = false;
                error.clear();
                return true;
            }

            const auto initialized = std::find(
                initialized_shaders_.begin(),
                initialized_shaders_.end(),
                entry->id
            );
            if (initialized == initialized_shaders_.end()) {
                if (initialized_shaders_.size() >= 256U) {
                    result = false;
                    error.clear();
                    return true;
                }
                initialized_shaders_.push_back(entry->id);
            }
            result = true;
            error.clear();
            return true;
        }

        if (name == "setSpriteShader") {
            std::string_view tag, shader_id;
            if (!string_arg(0U, tag) || !string_arg(1U, shader_id)) {
                error = "setSpriteShader expects sprite tag and shader identifier";
                return false;
            }
            const auto* entry = shader_catalog_ != nullptr
                ? shader_catalog_->find(shader_id)
                : nullptr;
            if (entry == nullptr || !entry->valid
                || scene_ == nullptr
                || !shader_catalog_->load(entry->id)
                || !scene_->script_set_shader(tag, entry->id)) {
                result = false;
                error.clear();
                return true;
            }

            if (std::find(
                    initialized_shaders_.begin(),
                    initialized_shaders_.end(),
                    entry->id
                ) == initialized_shaders_.end()
                && initialized_shaders_.size() < 256U) {
                initialized_shaders_.push_back(entry->id);
            }
            result = true;
            error.clear();
            return true;
        }

        if (name == "removeSpriteShader") {
            std::string_view tag;
            if (!string_arg(0U, tag)) {
                error = "removeSpriteShader expects a sprite tag";
                return false;
            }
            result = scene_ != nullptr && scene_->script_remove_shader(tag);
            error.clear();
            return true;
        }

        if (name == "setShaderFloat" || name == "setShaderInt"
            || name == "setShaderBool") {
            std::string_view tag, uniform;
            if (!string_arg(0U, tag) || !string_arg(1U, uniform)) {
                error = "setShader* expects sprite tag, uniform, value";
                return false;
            }

            double value{};
            if (name == "setShaderFloat") {
                if (!number_arg(2U, value)) {
                    error = "setShaderFloat value must be a finite number";
                    return false;
                }
            } else if (name == "setShaderInt") {
                std::int64_t integer{};
                if (!integer_arg(2U, integer)) {
                    error = "setShaderInt value must be an integer";
                    return false;
                }
                value = static_cast<double>(integer);
            } else {
                bool boolean{};
                if (!bool_arg(2U, boolean)) {
                    error = "setShaderBool value must be a boolean";
                    return false;
                }
                value = boolean ? 1.0 : 0.0;
            }

            result = scene_ != nullptr
                && scene_->script_set_shader_uniform(tag, uniform, value);
            error.clear();
            return true;
        }

        // PULSEFORGE_P1_1_15_SHADER_ARRAY_BRIDGE_V1
        // Lua tables are flattened by LuaRuntime before crossing the host
        // boundary. Store each vector component as a bounded scalar uniform;
        // renderer compatibility helpers consume uBlocksize[0]/[1], etc.
        if (name == "setShaderFloatArray" || name == "setShaderIntArray") {
            std::string_view tag, uniform;
            if (arguments.size() < 3U || arguments.size() > 10U
                || !string_arg(0U, tag) || !string_arg(1U, uniform)
                || uniform.empty() || uniform.size() > 160U) {
                error = "setShader*Array expects tag, uniform and 1..8 values";
                return false;
            }
            if (scene_ == nullptr) {
                result = false;
                error.clear();
                return true;
            }

            for (std::size_t index = 2U; index < arguments.size(); ++index) {
                double value{};
                if (name == "setShaderFloatArray") {
                    if (!number_arg(index, value)) {
                        error = "setShaderFloatArray values must be finite numbers";
                        return false;
                    }
                } else {
                    std::int64_t integer{};
                    if (!integer_arg(index, integer)) {
                        error = "setShaderIntArray values must be integers";
                        return false;
                    }
                    value = static_cast<double>(integer);
                }

                std::string component;
                component.reserve(uniform.size() + 8U);
                component.append(uniform);
                component.push_back('[');
                component.append(std::to_string(index - 2U));
                component.push_back(']');
                if (!scene_->script_set_shader_uniform(
                        tag,
                        component,
                        value
                    )) {
                    result = false;
                    error.clear();
                    return true;
                }
            }

            result = true;
            error.clear();
            return true;
        }

        if (name == "getShaderFloat") {
            std::string_view tag, uniform;
            if (!string_arg(0U, tag) || !string_arg(1U, uniform)) {
                error = "getShaderFloat expects sprite tag and uniform";
                return false;
            }
            double value{};
            if (scene_ != nullptr
                && scene_->script_get_shader_uniform(tag, uniform, value)) {
                result = value;
            } else {
                result = 0.0;
            }
            error.clear();
            return true;
        }

        // PULSEFORGE_P1_5_0_HORTAS_WIGGLE_EFFECT_BRIDGE_V1
        // Hortas-compatible addWiggleEffect/clearWiggleEffect are mapped to
        // PulseForge's existing renderer-native wiggle UV mesh. The preset
        // uses the same bounded scalar contract already covered by shader
        // compatibility tests (speed=2, frequency=8, amplitude=0.08).
        if (name == "addWiggleEffect") {
            std::string_view tag;
            if (!string_arg(0U, tag) || scene_ == nullptr
                || !scene_->script_has_sprite(tag)) {
                error = "addWiggleEffect expects an existing sprite tag";
                return false;
            }
            std::int64_t effect_type = 1; // Wavy default for one-arg corpus calls.
            if (arguments.size() > 1U) {
                std::string_view type_name;
                std::int64_t numeric{};
                if (string_arg(1U, type_name)) {
                    if (type_name == "Dreamy" || type_name == "dreamy") effect_type = 0;
                    else if (type_name == "Wavy" || type_name == "wavy") effect_type = 1;
                    else if (type_name == "Heat_wave_horizontal"
                        || type_name == "heat_wave_horizontal") effect_type = 2;
                    else if (type_name == "Heat_wave_vertical"
                        || type_name == "heat_wave_vertical") effect_type = 3;
                    else if (type_name == "Flag" || type_name == "flag") effect_type = 4;
                    else {
                        error = "addWiggleEffect effect type is unsupported";
                        return false;
                    }
                } else if (integer_arg(1U, numeric) && numeric >= 0 && numeric <= 4) {
                    effect_type = numeric;
                } else {
                    error = "addWiggleEffect effect type must be a known name or 0..4";
                    return false;
                }
            }
            if (!scene_->script_set_shader(tag, "wiggle")
                || !scene_->script_set_shader_uniform(tag, "uEnabled", 1.0)
                || !scene_->script_set_shader_uniform(tag, "uSpeed", 2.0)
                || !scene_->script_set_shader_uniform(tag, "uFrequency", 8.0)
                || !scene_->script_set_shader_uniform(tag, "uWaveAmplitude", 0.08)
                || !scene_->script_set_shader_uniform(
                    tag, "effectType", static_cast<double>(effect_type)
                )) {
                error = "addWiggleEffect could not bind its bounded wiggle effect";
                return false;
            }
            result = true;
            error.clear();
            return true;
        }
        if (name == "clearWiggleEffect") {
            std::string_view tag;
            if (!string_arg(0U, tag) || scene_ == nullptr
                || !scene_->script_has_sprite(tag)) {
                error = "clearWiggleEffect expects an existing sprite tag";
                return false;
            }
            // Idempotent feature-probe semantics: if no shader is active this
            // still succeeds for an existing sprite.
            static_cast<void>(
                scene_->script_set_shader_uniform(tag, "uEnabled", 0.0)
            );
            result = true;
            error.clear();
            return true;
        }
        if (name == "clearEffects") {
            std::string_view tag;
            if (!string_arg(0U, tag) || scene_ == nullptr
                || !scene_->script_has_sprite(tag)) {
                error = "clearEffects expects an existing sprite tag";
                return false;
            }
            static_cast<void>(scene_->script_set_wavy_effect(tag, 0.0, 0.0, 0.0));
            static_cast<void>(scene_->script_remove_shader(tag));
            result = true;
            error.clear();
            return true;
        }

        // PULSEFORGE_1_0_0_OVERKILL_GLITCH_COMPAT_V1
        if (name == "addGlitchEffect") {
            std::string_view tag;
            double intensity{}, frequency{}, speed{};
            if (!string_arg(0U, tag)) {
                error = "addGlitchEffect expects a sprite tag";
                return false;
            }
            if (arguments.size() > 1U && !number_arg(1U, intensity)) {
                error = "addGlitchEffect intensity must be a finite number";
                return false;
            }
            if (arguments.size() > 2U && !number_arg(2U, frequency)) {
                error = "addGlitchEffect frequency must be a finite number";
                return false;
            }
            if (arguments.size() > 3U && !number_arg(3U, speed)) {
                error = "addGlitchEffect speed must be a finite number";
                return false;
            }
            const double amplitude = std::clamp(intensity * 0.01, -0.12, 0.12);
            if (scene_ == nullptr || !scene_->script_set_wavy_effect(
                    tag, amplitude, frequency, speed
                )) {
                error = "addGlitchEffect sprite tag was not found";
                return false;
            }
            error.clear();
            return true;
        }

        // PULSEFORGE_P1_1_11_WAVY_EFFECT_BRIDGE_V1
        // Legacy Psych-compatible semantics used by the mod corpus:
        // wavyEffect(tag, amplitudeFraction, spatialCycles, cyclesPerSecond).
        if (name == "wavyEffect") {
            std::string_view tag;
            double amplitude{}, frequency{}, speed{};
            if (!string_arg(0U, tag)) {
                error = "wavyEffect expects a sprite tag";
                return false;
            }
            if (arguments.size() > 1U && !number_arg(1U, amplitude)) {
                error = "wavyEffect amplitude must be a finite number";
                return false;
            }
            if (arguments.size() > 2U && !number_arg(2U, frequency)) {
                error = "wavyEffect frequency must be a finite number";
                return false;
            }
            if (arguments.size() > 3U && !number_arg(3U, speed)) {
                error = "wavyEffect speed must be a finite number";
                return false;
            }
            if (scene_ == nullptr || !scene_->script_set_wavy_effect(
                    tag, amplitude, frequency, speed
                )) {
                error = "wavyEffect sprite tag was not found";
                return false;
            }
            error.clear();
            return true;
        }
        if (name == "close") {
            error.clear();
            return true;
        }

        error = "Psych compatibility function is not implemented: ";
        error.append(name);
        return false;
    }

    // PULSEFORGE_P1_1_18_SCRIPT_RESOURCE_RESOLUTION_V1
    // PULSEFORGE_P1_2_0_UNIFIED_RESOURCE_ROOTS_V1
    // Static resource lookups use the exact same Psych distribution expansion
    // as RuntimeScene. Code/script lookup can disable the sibling stock
    // provider so one mod can never inject executable Lua into another.
    [[nodiscard]] std::vector<std::filesystem::path>
    script_resource_roots(const bool include_stock_provider = true) const {
        std::vector<std::filesystem::path> explicit_roots;
        explicit_roots.reserve(options_.content_roots.size() + 2U);
        const auto append_explicit = [&explicit_roots](
            const std::filesystem::path& root
        ) {
            if (root.empty()) return;
            const auto key = path_utf8(root.lexically_normal());
            const bool duplicate = std::any_of(
                explicit_roots.begin(),
                explicit_roots.end(),
                [&](const auto& current) {
                    return path_utf8(current.lexically_normal()) == key;
                }
            );
            if (!duplicate) explicit_roots.push_back(root);
        };
        for (const auto& root : options_.content_roots) {
            append_explicit(root);
        }
        append_explicit(options_.selected_content_root);
        append_explicit(options_.selected_mod_root);

        const auto expanded = detail::resolve_psych_content_roots(
            explicit_roots,
            64U,
            include_stock_provider
        );

        // File lookup returns on the first match, so expose HIGH -> LOW here;
        // RuntimeScene itself consumes the resolver's native LOW -> HIGH order.
        std::vector<std::filesystem::path> roots;
        roots.reserve(expanded.roots.size());
        for (auto it = expanded.roots.rbegin(); it != expanded.roots.rend(); ++it) {
            roots.push_back(*it);
        }
        return roots;
    }

    [[nodiscard]] static bool safe_relative_resource_path(
        const std::filesystem::path& relative
    ) {
        if (relative.empty() || relative.is_absolute()
            || relative.has_root_name() || relative.has_root_directory()) {
            return false;
        }
        for (const auto& part : relative) {
            if (part == ".." || part == ".") {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::optional<std::filesystem::path>
    resolve_script_audio_asset(
        const std::string_view id,
        const ScriptAudioKind kind
    ) const {
        if (id.empty() || id.size() > 512U
            || id.find('\0') != std::string_view::npos) {
            return std::nullopt;
        }
        std::filesystem::path requested{std::string(id)};
        if (!safe_relative_resource_path(requested)) {
            return std::nullopt;
        }

        std::vector<std::filesystem::path> names;
        names.reserve(4U);
        if (requested.has_extension()) {
            names.push_back(requested);
        } else {
            for (const auto extension : {
                     std::string_view{".ogg"},
                     std::string_view{".wav"},
                     std::string_view{".mp3"},
                     std::string_view{".flac"},
                 }) {
                auto candidate = requested;
                candidate += extension;
                names.push_back(std::move(candidate));
            }
        }

        const std::array sound_directories{
            std::filesystem::path{"sounds"},
            std::filesystem::path{"assets/sounds"},
            std::filesystem::path{"shared/sounds"},
            std::filesystem::path{"assets/shared/sounds"},
        };
        const std::array music_directories{
            std::filesystem::path{"music"},
            std::filesystem::path{"assets/music"},
            std::filesystem::path{"shared/music"},
            std::filesystem::path{"assets/shared/music"},
        };

        const auto roots = script_resource_roots();
        for (const auto& root : roots) {
            const auto try_candidate = [&root](
                const std::filesystem::path& relative
            ) -> std::optional<std::filesystem::path> {
                if (!safe_relative_resource_path(relative)) {
                    return std::nullopt;
                }
                const auto candidate = root / relative;
                std::error_code error;
                if (!std::filesystem::is_regular_file(candidate, error)
                    || error || !path_is_within(root, candidate)) {
                    return std::nullopt;
                }
                auto canonical = std::filesystem::weakly_canonical(
                    candidate,
                    error
                );
                if (error) {
                    return std::nullopt;
                }
                return canonical;
            };

            if (kind == ScriptAudioKind::sound) {
                for (const auto& directory : sound_directories) {
                    for (const auto& name : names) {
                        if (const auto result = try_candidate(directory / name);
                            result.has_value()) {
                            return result;
                        }
                    }
                }
            } else {
                for (const auto& directory : music_directories) {
                    for (const auto& name : names) {
                        if (const auto result = try_candidate(directory / name);
                            result.has_value()) {
                            return result;
                        }
                    }
                }
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::filesystem::path>
    resolve_dynamic_lua_script(const std::string_view requested_id) const {
        if (requested_id.empty() || requested_id.size() > 512U
            || requested_id.find('\0') != std::string_view::npos) {
            return std::nullopt;
        }
        std::filesystem::path requested{std::string(requested_id)};
        if (!requested.has_extension()) {
            requested += ".lua";
        }
        auto extension = path_utf8(requested.extension());
        std::transform(
            extension.begin(),
            extension.end(),
            extension.begin(),
            [](const unsigned char value) {
                return value >= 'A' && value <= 'Z'
                    ? static_cast<char>(value - 'A' + 'a')
                    : static_cast<char>(value);
            }
        );
        if (!safe_relative_resource_path(requested) || extension != ".lua") {
            return std::nullopt;
        }

        const auto roots = script_resource_roots(false);
        for (const auto& root : roots) {
            for (const auto& relative : std::array{
                     requested,
                     std::filesystem::path{"scripts"} / requested,
                 }) {
                if (const auto result = contained_lua_file(root, relative);
                    result.has_value()) {
                    return result;
                }
            }

            if (!options_.chart_path.empty()
                && path_is_within(root, options_.chart_path.parent_path())) {
                const auto song_directory =
                    options_.chart_path.parent_path().lexically_relative(root);
                if (safe_relative_resource_path(song_directory)) {
                    if (const auto result = contained_lua_file(
                            root,
                            song_directory / requested
                        ); result.has_value()) {
                        return result;
                    }
                }
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::vector<DiscoveredScript> discover_script_files() const {
        std::vector<DiscoveredScript> result;
        if (chart_ == std::nullopt) {
            return result;
        }

        // PULSEFORGE_P1_2_0_PSYCH_SCRIPT_LAYER_EXPANSION_V1
        // Search the selected distribution's assets/data/preload/shared layers
        // for scripts, but never the sibling stock provider's executable code.
        std::vector<std::filesystem::path> explicit_script_roots;
        explicit_script_roots.reserve(options_.content_roots.size() + 2U);
        const auto append_explicit_script_root = [&explicit_script_roots](
            const std::filesystem::path& root
        ) {
            if (root.empty()) return;
            const auto key = path_utf8(root.lexically_normal());
            if (std::none_of(
                    explicit_script_roots.begin(),
                    explicit_script_roots.end(),
                    [&](const auto& current) {
                        return path_utf8(current.lexically_normal()) == key;
                    })) {
                explicit_script_roots.push_back(root);
            }
        };
        for (const auto& root : detail::select_psych_executable_roots(
                 options_.content_roots,
                 options_.selected_content_root,
                 options_.selected_mod_root
             )) {
            append_explicit_script_root(root);
        }
        const auto expanded_script_roots = detail::resolve_psych_content_roots(
            explicit_script_roots,
            64U,
            false
        );

        std::vector<std::pair<std::filesystem::path, std::int32_t>> roots;
        roots.reserve(expanded_script_roots.roots.size());
        std::int32_t base_priority = -2'000;
        for (const auto& root : expanded_script_roots.roots) {
            roots.emplace_back(root, base_priority++);
        }

        std::vector<std::string> note_types;
        if (streaming_mode() && streaming_reader_.has_value()) {
            const auto kinds = streaming_reader_->kinds();
            note_types.reserve(kinds.size());
            for (const auto& kind : kinds) {
                if (kind != "normal" && safe_script_id(kind)) {
                    note_types.push_back(kind);
                }
            }
        } else {
            note_types.reserve(chart_->notes.size());
            for (const auto& note : chart_->notes) {
                if (note.kind != "normal" && safe_script_id(note.kind)) {
                    note_types.push_back(note.kind);
                }
            }
        }
        std::sort(note_types.begin(), note_types.end());
        note_types.erase(
            std::unique(note_types.begin(), note_types.end()),
            note_types.end()
        );

        std::vector<std::string> event_names;
        event_names.reserve(chart_->events.size());
        for (const auto& event : chart_->events) {
            if (safe_script_id(event.name)) {
                event_names.push_back(event.name);
            }
        }
        std::sort(event_names.begin(), event_names.end());
        event_names.erase(
            std::unique(event_names.begin(), event_names.end()),
            event_names.end()
        );

        for (const auto& [root, priority] : roots) {
            append_lua_tree(
                result,
                root,
                "scripts",
                ScriptOrigin::global,
                priority
            );
            append_named_lua(
                result,
                root,
                "stages",
                chart_->stage_id,
                ScriptOrigin::stage,
                priority
            );
            append_named_lua(
                result,
                root,
                "characters",
                chart_->girlfriend_character,
                ScriptOrigin::girlfriend_character,
                priority
            );
            append_named_lua(
                result,
                root,
                "characters",
                chart_->opponent_character,
                ScriptOrigin::opponent_character,
                priority
            );
            append_named_lua(
                result,
                root,
                "characters",
                chart_->player_character,
                ScriptOrigin::player_character,
                priority
            );
            for (const auto& note_type : note_types) {
                append_named_lua(
                    result,
                    root,
                    "custom_notetypes",
                    note_type,
                    ScriptOrigin::note_type,
                    priority
                );
            }
            for (const auto& event_name : event_names) {
                append_named_lua(
                    result,
                    root,
                    "custom_events",
                    event_name,
                    ScriptOrigin::event,
                    priority
                );
            }
        }

        for (const auto& [root, priority] : roots) {
            if (path_is_within(root, options_.chart_path.parent_path())) {
                const auto relative = options_.chart_path.parent_path()
                    .lexically_relative(root);
                append_lua_tree(
                    result,
                    root,
                    relative,
                    ScriptOrigin::song,
                    priority
                );
            }
        }

        const auto append_explicit = [&result](
            const std::filesystem::path& path
        ) {
            const auto key = path_utf8(path.lexically_normal());
            const bool duplicate = std::any_of(
                result.begin(),
                result.end(),
                [&](const auto& script) {
                    return path_utf8(script.path.lexically_normal()) == key;
                }
            );
            if (!duplicate) {
                result.push_back({path, ScriptOrigin::explicit_file, 0});
            }
        };
        for (const auto& path : options_.script_paths) {
            append_explicit(path);
        }
        if (options_.script_path.has_value()) {
            const auto legacy_key = path_utf8(
                options_.script_path->lexically_normal()
            );
            const bool already_listed = std::any_of(
                options_.script_paths.begin(),
                options_.script_paths.end(),
                [&](const auto& path) {
                    return path_utf8(path.lexically_normal()) == legacy_key;
                }
            );
            if (!already_listed) {
                append_explicit(*options_.script_path);
            }
        }
        return result;
    }

    [[nodiscard]] bool reload_script(
        const std::vector<DiscoveredScript>* prefetched = nullptr
    ) {
        if (scripts_ == nullptr) {
            return false;
        }

        // PULSEFORGE_P1_5_0E_SINGLE_SCRIPT_DISCOVERY_PASS_V1
        // Initial loading already needs the discovery count for per-script Lua
        // budgets. Reuse that exact result instead of rescanning every content
        // root immediately afterwards. Hot reload still rediscovers normally.
        const auto discovered_storage = prefetched == nullptr
            ? discover_script_files()
            : std::vector<DiscoveredScript>{};
        const auto& discovered = prefetched != nullptr
            ? *prefetched
            : discovered_storage;
        if (discovered.empty()) {
            return false;
        }

        std::vector<LuaScriptDefinition> definitions;
        definitions.reserve(discovered.size());
        for (const auto& script : discovered) {
            std::string error;
            auto source = read_text_file(script.path, &error);
            if (!error.empty()) {
                std::cerr << "Lua load error: " << error << '\n';
                continue;
            }
            definitions.push_back({
                "@" + path_utf8(script.path),
                std::move(source),
                script.origin,
                script.priority,
            });
        }
        if (definitions.empty()) {
            return false;
        }

        script_state_.clear_transient_output();
        script_dynamic_requests_.clear();
        printed_lua_diagnostics_ = 0;
        const auto report = scripts_->load_scripts(definitions);
        if (!report.any_loaded()) {
            print_lua_diagnostics();
            return false;
        }
        if (report.failed != 0 || report.duplicates != 0) {
            std::cerr << "Lua scripts: " << report.loaded << " loaded, "
                      << report.failed << " failed, "
                      << report.duplicates << " duplicate(s) skipped\n";
        }
        lua_create_pending_ = true;
        if (scene_ != nullptr) {
            activate_lua_create_callbacks();
        }
        return true;
    }

    void activate_lua_create_callbacks() {
        if (!lua_create_pending_ || scripts_ == nullptr || scene_ == nullptr) {
            return;
        }
        lua_create_pending_ = false;
        script_initial_lifecycle_active_ = true;
        script_state_.clear_transient_output();
        static_cast<void>(scripts_->on_create());
        static_cast<void>(scripts_->on_create_post());
        if (!script_song_started_) {
            const auto countdown = scripts_->on_start_countdown();
            script_countdown_evaluated_ = true;
            script_countdown_blocked_ =
                countdown.stop_requested && !options_.offline_render.enabled;
            script_countdown_release_requested_ = false;
            if (script_countdown_blocked_) {
                std::cout
                    << "[Lua countdown] onStartCountdown requested stop; "
                       "gameplay clock held at zero\n";
            }
        }
        script_initial_lifecycle_active_ = false;
        consume_script_output();
    }

    void start_script_song_if_ready() {
        if (script_song_started_ || script_countdown_blocked_) {
            return;
        }
        if (scripts_ != nullptr) {
            script_state_.clear_transient_output();
            static_cast<void>(scripts_->on_song_start());
            // Dynamic scripts requested from onSongStart must receive the
            // same song-start lifecycle when they are initialized below.
            script_song_started_ = true;
            consume_script_output();
        } else {
            script_song_started_ = true;
        }
        audio_.play();
    }

    void service_script_countdown_release() {
        if (!script_countdown_blocked_
            || !script_countdown_release_requested_) {
            return;
        }

        script_countdown_release_requested_ = false;
        if (scripts_ != nullptr) {
            script_state_.clear_transient_output();
            const auto countdown = scripts_->on_start_countdown();
            consume_script_output();
            if (countdown.stop_requested) {
                return;
            }
        }

        script_countdown_blocked_ = false;
        std::cout
            << "[Lua countdown] startCountdown released gameplay clock\n";
        start_script_song_if_ready();
    }

    // PULSEFORGE_P1_1_18_SOUND_COMPLETION_DISPATCH_V1
    void service_script_sound_completions() {
        auto completions = audio_.consume_sound_completions();
        if (completions.empty() || scripts_ == nullptr) {
            return;
        }
        script_state_.clear_transient_output();
        for (const auto& tag : completions) {
            if (tag == "__pulseforge_psych_music__") {
                continue;
            }
            static_cast<void>(scripts_->on_sound_finished(tag));
        }
        consume_script_output();
    }

    // PULSEFORGE_P1_1_18_DYNAMIC_SCRIPT_REQUEST_SERVICE_V1
    void service_dynamic_script_requests() {
        if (servicing_dynamic_script_requests_
            || script_initial_lifecycle_active_
            || scripts_ == nullptr
            || script_dynamic_requests_.empty()) {
            return;
        }

        servicing_dynamic_script_requests_ = true;
        constexpr std::size_t maximum_operations_per_service = 64U;
        std::size_t processed = 0U;
        while (!script_dynamic_requests_.empty()
            && processed < maximum_operations_per_service) {
            DynamicScriptRequest request = std::move(
                script_dynamic_requests_.front()
            );
            script_dynamic_requests_.erase(script_dynamic_requests_.begin());
            ++processed;

            const std::string source_name{"@" + path_utf8(request.path)};
            if (request.action == DynamicScriptAction::add) {
                if (scripts_->contains(source_name)) {
                    if (!request.ignore_already_running) {
                        std::cerr
                            << "[Lua] addLuaScript ignored already-running "
                            << source_name << '\n';
                    }
                    continue;
                }

                std::string read_error;
                auto source = read_text_file(request.path, &read_error);
                if (!read_error.empty()) {
                    std::cerr << "[Lua] addLuaScript: " << read_error << '\n';
                    continue;
                }

                const auto report = scripts_->add_script({
                    source_name,
                    std::move(source),
                    ScriptOrigin::explicit_file,
                    0,
                });
                if (!report.any_loaded()) {
                    if (!request.ignore_already_running) {
                        print_lua_diagnostics();
                    }
                    continue;
                }
                script_state_.clear_transient_output();
                static_cast<void>(scripts_->initialize_script(
                    source_name,
                    script_song_started_
                ));
                consume_script_output();
            } else {
                if (!scripts_->remove_script(source_name)) {
                    if (!request.ignore_already_running) {
                        std::cerr
                            << "[Lua] removeLuaScript ignored missing "
                            << source_name << '\n';
                    }
                    continue;
                }
                consume_script_output();
            }
        }
        servicing_dynamic_script_requests_ = false;
    }

    [[nodiscard]] bool service_script_runtime_requests() {
        // PULSEFORGE_P1_1_17_DEFERRED_RUNTIME_REQUEST_SERVICE_V1
        if (!script_restart_requested_) {
            return false;
        }
        script_restart_requested_ = false;
        restart();
        return true;
    }

    // PULSEFORGE_P1_1_18_RECURSIVE_ENGINE_EVENT_BUS_V1
    void apply_script_builtin_event(const ScriptEventRequest& event) noexcept {
        if (streaming_mode()) {
            static_cast<void>(streaming_session_->apply_event(
                event.name,
                event.value1,
                event.value2
            ));
        } else {
            static_cast<void>(session_->apply_event(
                event.name,
                event.value1,
                event.value2
            ));
        }
    }

    void consume_script_output() {
        std::size_t processed_events = 0U;
        bool event_budget_exhausted = false;

        // triggerEvent() is a bounded breadth-first engine event bus:
        // built-in gameplay semantics -> visual handlers -> all Lua onEvent
        // callbacks. onEvent may queue another event, which is consumed by the
        // next loop iteration without recursive C++ calls.
        while (true) {
            for (const auto& message : script_state_.debug_messages) {
                std::cout << "[Lua] " << message << '\n';
            }
            script_state_.debug_messages.clear();

            if (script_state_.pending_events.empty()) {
                break;
            }

            auto pending = std::move(script_state_.pending_events);
            script_state_.pending_events.clear();

            for (const auto& event : pending) {
                if (processed_events >= script_state_.max_pending_events) {
                    event_budget_exhausted = true;
                    break;
                }
                ++processed_events;

                apply_script_builtin_event(event);
                handle_visual_event(event.name, event.value1, event.value2);
                if (scripts_ != nullptr) {
                    static_cast<void>(scripts_->on_event(
                        event.name,
                        event.value1,
                        event.value2
                    ));
                }
            }

            if (event_budget_exhausted) {
                script_state_.pending_events.clear();
                script_state_.debug_messages.emplace_back(
                    "triggerEvent recursion exceeded the bounded event budget"
                );
                continue;
            }
        }

        print_lua_diagnostics();
        if (!script_initial_lifecycle_active_
            && !servicing_dynamic_script_requests_) {
            service_dynamic_script_requests();
        }
    }

    void print_lua_diagnostics() {
        if (scripts_ == nullptr) {
            return;
        }
        const auto diagnostics = scripts_->diagnostics();
        for (std::size_t index = printed_lua_diagnostics_;
             index < diagnostics.size();
             ++index) {
            std::cerr << "[Lua:" << diagnostics[index].source_name << ':'
                      << diagnostics[index].diagnostic.callback << "] "
                      << diagnostics[index].diagnostic.message << '\n';
        }
        printed_lua_diagnostics_ = diagnostics.size();
    }
#endif

    AppLaunchOptions options_;
    detail::TransferredPlatform owned_platform_;
    detail::TransferredPlatform* return_platform_{};
    std::optional<Chart> chart_;
    std::unique_ptr<GameplaySession> session_;
    std::optional<PackedChartReader> streaming_reader_;
    std::optional<VisualDensityIndexReader> streaming_visual_density_reader_;
    std::unique_ptr<StreamingGameplaySession> streaming_session_;
    std::unique_ptr<DenseNoteCoverage> streaming_visual_cache_;
    std::unique_ptr<DenseNoteCoverage> frame_note_coverage_;
    std::vector<std::int64_t> streaming_pattern_prefix_end_us_;
    bool streaming_pattern_index_sorted_{true};
    std::optional<Replay> replay_;
    std::size_t replay_input_index_{};
    std::vector<double> note_prefix_end_ms_;
    AudioTransport audio_;
    AudioTransport pause_music_;
    std::vector<std::filesystem::path> pause_music_catalog_;
    std::size_t pause_music_index_{std::numeric_limits<std::size_t>::max()};
    std::uint64_t pause_music_random_state_{};
    SDL_Window* window_{};
    SDL_Renderer* renderer_{};
    SDL_Texture* offline_render_target_{};
    SDL_Texture* audio_visualizer_icon_{};
    detail::RuntimePostEffects post_effects_;
    std::unique_ptr<detail::RuntimeScene> scene_;
    std::unique_ptr<detail::OfflineEncoder> offline_encoder_;
    SolidQuadBatch dense_quad_batch_;
    std::vector<detail::RuntimeNoteSkinDraw> note_skin_draw_batch_;
    std::vector<SDL_Gamepad*> gamepads_;
    std::vector<std::uint16_t> lane_input_counts_;
    std::array<std::uint16_t, SDL_SCANCODE_COUNT> keyboard_lane_map_{};
    std::array<std::uint16_t, SDL_GAMEPAD_BUTTON_COUNT> gamepad_lane_map_{};
    std::array<bool, SDL_SCANCODE_COUNT> pressed_scancodes_{};
#if defined(PULSEFORGE_HAS_LUA)
    std::array<bool, SDL_SCANCODE_COUNT> script_just_pressed_scancodes_{};
#endif
    std::array<float, timing_history_size> timing_history_{};
    std::size_t timing_history_count_{};
    std::size_t timing_history_cursor_{};
    std::array<Particle, particle_capacity> particles_{};
    std::size_t particle_cursor_{};
    std::array<NoteSplashAnimation, note_splash_animation_capacity>
        note_splash_animations_{};
    std::size_t note_splash_animation_cursor_{};
    std::deque<std::pair<double, std::uint64_t>> nps_samples_;
    NoteTypeRegistry note_type_registry_;
    // PULSEFORGE_P1_5_0F_BOUNDED_RUNTIME_TELEMETRY_STATE_V1
    RuntimePerformanceAccumulator runtime_performance_;
    detail::DiscordPresencePublisher discord_presence_;
    std::vector<float> materialized_note_speed_multipliers_;
    std::map<std::string, float, std::less<>> streaming_kind_speed_multipliers_;
    double minimum_note_scroll_multiplier_{1.0};
    bool note_scroll_multiplier_active_{};
    bool note_sustain_disable_policy_active_{};
    std::array<
        std::map<std::string, ScriptNoteVisualState, std::less<>>,
        3U
    > script_note_visual_overrides_;
    std::size_t script_note_visual_override_count_{};
    // PULSEFORGE_P1_5_0C_DECLARATIVE_HITSOUND_STATE_V1
    // Paths are resolved once per note type; hit events only submit an already
    // validated file to AudioTransport's bounded sound bank.
    std::map<std::string, std::filesystem::path, std::less<>>
        note_type_hitsounds_;
#if defined(PULSEFORGE_HAS_LUA)
    // PULSEFORGE_P1_1_12_SHADER_RUNTIME_STATE_V1
    std::unique_ptr<ShaderCatalog> shader_catalog_;
    std::vector<std::string> initialized_shaders_;
    GameplayScriptState script_state_;
    std::unique_ptr<ApplicationLuaHost> lua_host_;
    std::unique_ptr<LuaScriptManager> scripts_;
    std::vector<ScriptStrumState> script_strums_;
    std::vector<ScriptUnspawnPrototype> script_streaming_unspawn_prototypes_;
    std::vector<ScriptTween> script_tweens_;
    std::vector<ScriptTimer> script_timers_;
    std::map<std::string, ScriptHudObjectState, std::less<>> script_hud_objects_;
    std::map<std::string, ScriptValue, std::less<>> script_property_store_;
    // PULSEFORGE_P1_5_0_LUA_NOTE_NO_ANIMATION_V1
    std::map<std::string, bool, std::less<>> script_note_no_animation_;
    // PULSEFORGE_P1_1_19_AUTOMATIC_CAMERA_TURN_STATE_V1
    detail::PsychCameraTurnTracker script_auto_camera_turn_;
    std::optional<NoteOwner> script_auto_camera_owner_;
    double script_cam_game_x_{};
    double script_cam_game_y_{};
    double script_cam_game_zoom_{1.0};
    double script_cam_game_angle_{};
    double script_cam_game_alpha_{1.0};
    double script_cam_hud_x_{};
    double script_cam_hud_y_{};
    double script_cam_hud_zoom_{1.0};
    double script_cam_hud_angle_{};
    double script_cam_hud_alpha_{1.0};
    // PULSEFORGE_P1_1_16_PSYCH_HUD_COLOR_STATE_V1
    SDL_Color script_health_opponent_color_{10U, 12U, 24U, 255U};
    SDL_Color script_health_player_color_{70U, 240U, 156U, 255U};
    SDL_Color script_time_background_color_{20U, 24U, 42U, 255U};
    SDL_Color script_time_fill_color_{90U, 210U, 255U, 255U};
    bool lua_create_pending_{};
    // PULSEFORGE_P1_1_4_COUNTDOWN_INPUT_GATE_V1
    bool script_countdown_evaluated_{};
    bool script_countdown_blocked_{};
    bool script_countdown_release_requested_{};
    bool script_song_started_{};
    // PULSEFORGE_P1_1_18_DYNAMIC_SCRIPT_REQUEST_STATE_V1
    std::vector<DynamicScriptRequest> script_dynamic_requests_;
    bool servicing_dynamic_script_requests_{};
    bool script_initial_lifecycle_active_{};
    // PULSEFORGE_P1_1_17_DEFERRED_RUNTIME_REQUEST_STATE_V1
    bool script_restart_requested_{};
    bool script_force_song_ended_{};
    std::size_t printed_lua_diagnostics_{};
#endif
    std::string last_error_;
    std::string offline_frame_error_;
    std::string post_effect_message_;
    std::uint64_t last_frame_ns_{};
    NoteProfileFrame note_profile_frame_{};
    ProfileMetric note_profile_note_{};
    ProfileMetric note_profile_cache_{};
    ProfileMetric note_profile_pvd_{};
    ProfileMetric note_profile_pfc_{};
    ProfileMetric note_profile_batch_build_{};
    ProfileMetric note_profile_batch_submit_{};
    ProfileMetric note_profile_fallback_{};
    ProfileMetric note_profile_present_{};
    ProfileMetric note_profile_gameplay_update_{};
    std::uint64_t note_profile_peak_window_started_ns_{};
    std::uint64_t note_profile_last_quads_{};
    std::uint64_t note_profile_last_fallback_draws_{};
    std::uint32_t note_profile_last_submissions_{};
    std::uint32_t note_profile_last_failed_submissions_{};
    std::uint32_t note_profile_rebuilds_current_window_{};
    std::uint32_t note_profile_rebuilds_last_second_{};
    double smoothed_fps_{};
    double smoothed_frame_ms_{};
    float screen_flash_{};
    float beat_pulse_{};
    double scroll_tween_from_{};
    double scroll_tween_to_{};
    double scroll_tween_duration_{};
    double scroll_tween_elapsed_{};
    double streaming_duration_ms_{2'000.0};
    double streaming_visual_cache_time_ms_{};
    double streaming_visual_cache_speed_{};
    double nps_last_song_time_{};
    double last_input_age_ms_{};
    std::uint64_t rendered_notes_{};
    std::uint64_t visual_draw_units_{};
    std::uint64_t streaming_density_buckets_{};
    std::uint64_t streaming_explicit_visual_visits_{};
    std::uint64_t visual_geometry_calls_{};
    std::uint64_t nps_last_successful_hits_{};
    std::uint64_t current_nps_{};
    std::uint64_t presence_logical_note_total_{};
    std::atomic<ContentLoadPhase> loading_phase_{
        ContentLoadPhase::inspecting
    };
    std::atomic<std::uint64_t> loading_work_complete_{};
    std::atomic<std::uint64_t> loading_work_total_{};
    bool running_{};
    bool paused_{};
    bool physical_resync_pending_{};
    bool result_shown_{};
    bool runtime_benchmark_reported_{};
    bool replay_saved_{};
    bool diagnostics_{};
    bool sdl_initialized_{};
    bool transferred_platform_{};
    bool platform_return_safe_{true};
    bool runtime_fatal_error_{};
    bool loading_cancelled_{};
#if defined(_WIN32)
    HMODULE winmm_module_{};
    DWORD previous_process_priority_{};
    int previous_thread_priority_{THREAD_PRIORITY_ERROR_RETURN};
    bool one_ms_timer_period_active_{};
    bool low_latency_runtime_active_{};
#endif
    bool vsync_active_{};
    bool window_focused_{true};
    bool pause_music_failed_{};
    bool pause_music_selected_loop_active_{};
    bool return_to_launcher_requested_{};
    bool campaign_result_acknowledged_{};
    bool close_request_notice_{};
    bool streaming_error_reported_{};
    bool streaming_visual_cache_downscroll_{};
    bool streaming_visual_cache_hide_opponent_{};
    bool streaming_visual_error_reported_{};
    bool dense_geometry_error_reported_{};
    bool note_skin_batch_error_reported_{};
    bool post_effect_warning_reported_{};
    std::size_t pause_menu_selection_{};
    std::uint32_t smoke_test_frame_count_{};
};

}  // namespace

namespace detail {

std::unique_ptr<ApplicationRunner> make_gameplay_application(
    AppLaunchOptions options
) {
    return std::make_unique<DesktopApplication>(std::move(options));
}

std::unique_ptr<ApplicationRunner> make_gameplay_application(
    AppLaunchOptions options,
    std::shared_ptr<DiscordPresenceSession> discord_session
) {
    return std::make_unique<DesktopApplication>(
        std::move(options),
        std::move(discord_session)
    );
}

std::unique_ptr<ApplicationRunner> make_gameplay_application(
    AppLaunchOptions options,
    TransferredPlatform platform
) {
    return make_gameplay_application(
        std::move(options),
        std::move(platform),
        nullptr
    );
}

std::unique_ptr<ApplicationRunner> make_gameplay_application(
    AppLaunchOptions options,
    TransferredPlatform platform,
    TransferredPlatform* const return_platform
) {
    return std::make_unique<DesktopApplication>(
        std::move(options),
        std::move(platform),
        return_platform
    );
}

std::unique_ptr<ApplicationRunner> make_gameplay_application(
    AppLaunchOptions options,
    TransferredPlatform platform,
    TransferredPlatform* const return_platform,
    std::shared_ptr<DiscordPresenceSession> discord_session
) {
    return std::make_unique<DesktopApplication>(
        std::move(options),
        std::move(platform),
        return_platform,
        std::move(discord_session)
    );
}

}  // namespace detail

}  // namespace pulseforge
