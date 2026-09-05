#include "runtime_scene.hpp"
#include "runtime_texture_policy.hpp"

#include "pulseforge/ascii_number.hpp"

#include "pulseforge/content_descriptors.hpp"
#include "psych_camera_target.hpp"
#include "psych_character_semantics.hpp"
#include "psych_shader_compat.hpp"
#include "psych_wavy_effect.hpp"
#include "pulseforge/stage_lua.hpp"
#include "pulseforge/timing_map.hpp"
#include "pulseforge/virtual_file_system.hpp"

#include <SDL3/SDL.h>

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_FAILURE_USERMSG
#define STB_IMAGE_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4505)
#endif
#include <stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulseforge::detail {
namespace {

constexpr float logical_width = 1'280.0F;
constexpr float logical_height = 720.0F;
constexpr std::size_t no_texture = std::numeric_limits<std::size_t>::max();

struct TextureDeleter {
    void operator()(SDL_Texture* texture) const noexcept {
        SDL_DestroyTexture(texture);
    }
};

using TexturePointer = std::unique_ptr<SDL_Texture, TextureDeleter>;

struct AtlasFrame {
    std::string name;
    SDL_FRect source{};
    float frame_width{};
    float frame_height{};
    float content_offset_x{};
    float content_offset_y{};
};

struct TextureResource {
    TexturePointer texture;
    float width{};
    float height{};
    // PULSEFORGE_P1_1_14_CPU_COLOR_SOURCE_V1
    // Original PNG identity is retained, but decoded RGBA is loaded lazily only
    // if a renderer-native CPU colour shader actually needs it.
    std::string source_virtual_path;
    std::string source_mount_id;
    std::vector<std::uint8_t> cpu_color_source_rgba;
    bool cpu_color_source_failed{};
    std::vector<AtlasFrame> atlas;
    // Built once when the texture is loaded. Animation prefix queries then use
    // two binary searches instead of rescanning and resorting the atlas for
    // every descriptor animation.
    std::vector<std::uint32_t> atlas_name_order;
};

constexpr std::size_t note_skin_lane_count = 4U;
constexpr std::size_t note_skin_element_count = 4U;
// PULSEFORGE_P1_5_0B_BOUNDED_NOTE_SKIN_PROFILE_LIMIT_V1
// Custom-event/note-type texture writes resolve into this fixed scene-local
// profile budget. The texture/atlas budgets remain the stronger global caps.
constexpr std::size_t maximum_runtime_note_skin_profiles = 32U;
constexpr std::size_t maximum_runtime_note_skin_batch_keys = 96U;
// PULSEFORGE_P1_5_0C_BOUNDED_NOTE_SPLASH_PROFILE_LIMIT_V1
constexpr std::size_t maximum_runtime_note_splash_profiles = 32U;
constexpr std::size_t maximum_runtime_note_splash_frames_per_profile = 256U;

struct NoteSkinFrame {
    std::size_t texture_index{no_texture};
    SDL_FRect source{};
    float frame_width{};
    float frame_height{};
    float content_offset_x{};
    float content_offset_y{};
    SDL_ScaleMode scale_mode{SDL_SCALEMODE_LINEAR};
    bool valid{};
};

using NoteSkinFrames = std::array<
    std::array<NoteSkinFrame, note_skin_lane_count>,
    note_skin_element_count
>;

struct NoteSkinProfileEntry final {
    std::string style;
    bool force_pixel{};
    NoteSkinFrames frames{};
};

struct NoteSplashProfileEntry final {
    std::string style;
    std::size_t texture_index{no_texture};
    std::vector<std::uint32_t> frame_indices;
};

enum class SpriteRole : std::uint8_t {
    stage,
    girlfriend,
    opponent,
    // PULSEFORGE_P1_4_0_SECONDARY_OPPONENT_SPRITE_ROLE_V1
    secondary_opponent,
    player,
};

constexpr std::size_t psych_shader_max_uniforms_per_sprite = 32U;
constexpr std::size_t psych_shader_max_identifier_bytes = 192U;

// PULSEFORGE_P1_1_14_CPU_COLOR_BUDGETS_V1
// PULSEFORGE_P1_1_15_SHADER_EFFECTS_BATCH_BUDGETS_V1
// CPU-backed colour/mosaic/blur fallbacks are deliberately bounded. These
// caps prevent a Lua tween on a giant atlas from turning into an unbounded
// CPU/GPU-memory or per-frame workload.
constexpr std::uint64_t psych_cpu_color_max_pixels_per_texture =
    8ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t psych_cpu_color_max_source_bytes =
    128ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t psych_cpu_color_max_variant_bytes =
    128ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t psych_cpu_color_max_pixels_per_frame =
    8ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t psych_cpu_blur_max_pixels_per_texture =
    1ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t psych_cpu_blur_max_pixels_per_frame =
    1ULL * 1024ULL * 1024ULL;
constexpr std::size_t psych_cpu_color_max_variants = 16U;

struct SceneShaderScalarUniform {
    std::string name;
    double value{};
};

struct CpuShaderVariant {
    std::uint64_t sprite_insertion_order{};
    std::size_t texture_index{no_texture};
    PsychCpuShaderKey key{};
    TexturePointer texture;
    std::uint64_t decoded_bytes{};
};

struct SceneSprite {
    std::size_t texture_index{no_texture};
    SpriteRole role{SpriteRole::stage};
    std::string tag;
    std::int64_t order{};
    std::uint64_t insertion_order{};
    float x{};
    float y{};
    // PULSEFORGE_P1_1_10_PSYCH_CAMERA_TARGET_V1
    float camera_position_x{};
    float camera_position_y{};
    float stage_camera_offset_x{};
    float stage_camera_offset_y{};
    float scale_x{1.0F};
    float scale_y{1.0F};
    float scroll_factor_x{1.0F};
    float scroll_factor_y{1.0F};
    float fallback_width{256.0F};
    float fallback_height{160.0F};
    float alpha{1.0F};
    double angle{};
    SDL_Color color{255, 255, 255, 255};
    SDL_FlipMode flip{SDL_FLIP_NONE};
    SDL_ScaleMode scale_mode{SDL_SCALEMODE_LINEAR};
    SDL_BlendMode blend_mode{SDL_BLENDMODE_BLEND};
    bool screen_space{};
    bool visible{true};
    bool script_created{};
    // PULSEFORGE_P1_1_6_TEXTURELESS_SCRIPT_SPRITES_V1
    bool allow_textureless_draw{true};
    std::optional<AtlasFrame> frame;
    struct AnimationClip {
        std::string id;
        std::vector<std::uint32_t> frames;
        double fps{24.0};
        bool loop{};
        float offset_x{};
        float offset_y{};
    };
    std::vector<AnimationClip> animations;
    std::size_t default_animation{no_texture};
    std::size_t transient_animation{no_texture};
    double transient_started_ms{};
    double transient_until_ms{};
    // PULSEFORGE_P1_4_0_CHARACTER_SUSTAIN_ANIMATION_HOLD_V2
    // Separate from the ordinary Psych singDuration timeout. Keep a bounded
    // set of active tails so chords/overlapping sustains can release
    // independently on hold_drop without letting idle/dance interrupt another
    // sustain that is still active. The array is fixed-size: no gameplay-frame
    // allocation is introduced.
    static constexpr std::size_t maximum_sustain_animation_holds =
        static_cast<std::size_t>(maximum_supported_key_count) * 4U;
    std::array<double, maximum_sustain_animation_holds> sustain_animation_tails{};
    double sustain_animation_until_ms{};
    double sing_duration_steps{4.0};
    // PULSEFORGE_P1_1_11_WAVY_EFFECT_STATE_V1
    PsychWavyEffect wavy_effect{};

    // PULSEFORGE_P1_1_12_PSYCH_SHADER_STATE_V1
    // Shader source execution remains catalog-validated and backend-specific.
    // The SDL_Renderer compatibility path stores only a bounded scalar surface.
    std::string shader_id;
    std::vector<SceneShaderScalarUniform> shader_uniforms;
};

[[nodiscard]] bool ascii_equal_insensitive(
    const char left,
    const char right
) noexcept {
    const auto fold = [](const unsigned char value) noexcept {
        return value >= static_cast<unsigned char>('A')
                && value <= static_cast<unsigned char>('Z')
            ? static_cast<unsigned char>(
                value - static_cast<unsigned char>('A')
                + static_cast<unsigned char>('a')
            )
            : value;
    };
    return fold(static_cast<unsigned char>(left))
        == fold(static_cast<unsigned char>(right));
}

[[nodiscard]] bool starts_with_ascii_insensitive(
    const std::string_view text,
    const std::string_view prefix
) noexcept {
    return text.size() >= prefix.size()
        && std::equal(
            prefix.begin(),
            prefix.end(),
            text.begin(),
            ascii_equal_insensitive
        );
}

[[nodiscard]] bool ascii_less_insensitive(
    const std::string_view left,
    const std::string_view right
) noexcept {
    const auto fold = [](const unsigned char value) noexcept {
        return value >= static_cast<unsigned char>('A')
                && value <= static_cast<unsigned char>('Z')
            ? static_cast<unsigned char>(
                value - static_cast<unsigned char>('A')
                    + static_cast<unsigned char>('a')
            )
            : value;
    };
    const auto count = std::min(left.size(), right.size());
    for (std::size_t index = 0U; index < count; ++index) {
        const auto left_value = fold(static_cast<unsigned char>(left[index]));
        const auto right_value = fold(static_cast<unsigned char>(right[index]));
        if (left_value != right_value) return left_value < right_value;
    }
    return left.size() < right.size();
}

[[nodiscard]] bool animation_id_equal(
    const std::string_view left,
    const std::string_view right
) noexcept {
    std::size_t left_index{};
    std::size_t right_index{};
    const auto separator = [](const char value) noexcept {
        return value == ' ' || value == '-' || value == '_';
    };
    while (left_index < left.size() || right_index < right.size()) {
        while (left_index < left.size() && separator(left[left_index])) {
            ++left_index;
        }
        while (right_index < right.size() && separator(right[right_index])) {
            ++right_index;
        }
        if (left_index == left.size() || right_index == right.size()) {
            return left_index == left.size() && right_index == right.size();
        }
        if (!ascii_equal_insensitive(left[left_index], right[right_index])) {
            return false;
        }
        ++left_index;
        ++right_index;
    }
    return true;
}

[[nodiscard]] bool equals_ascii_insensitive(
    const std::string_view left,
    const std::string_view right
) noexcept {
    return left.size() == right.size()
        && std::equal(
            left.begin(),
            left.end(),
            right.begin(),
            ascii_equal_insensitive
        );
}

[[nodiscard]] bool contains_ascii_insensitive(
    const std::string_view text,
    const std::string_view needle
) noexcept {
    if (needle.empty()) return true;
    if (needle.size() > text.size()) return false;
    return std::search(
        text.begin(), text.end(), needle.begin(), needle.end(),
        ascii_equal_insensitive
    ) != text.end();
}

[[nodiscard]] bool ends_with_ascii_insensitive(
    const std::string_view text,
    const std::string_view suffix
) noexcept {
    return text.size() >= suffix.size()
        && std::equal(
            suffix.begin(),
            suffix.end(),
            text.end() - static_cast<std::ptrdiff_t>(suffix.size()),
            ascii_equal_insensitive
        );
}

[[nodiscard]] bool safe_asset_id(const std::string_view id) noexcept {
    if (id.empty() || id.size() > 512U || id.front() == '/'
        || id.front() == '\\') {
        return false;
    }
    std::size_t segment_start = 0U;
    for (std::size_t index = 0U; index <= id.size(); ++index) {
        if (index == id.size() || id[index] == '/') {
            const auto segment = id.substr(segment_start, index - segment_start);
            if (segment.empty() || segment == "." || segment == "..") {
                return false;
            }
            segment_start = index + 1U;
            continue;
        }
        const unsigned char value = static_cast<unsigned char>(id[index]);
        if (id[index] == '\\' || id[index] == ':' || id[index] == '*'
            || id[index] == '?' || id[index] == '"' || id[index] == '<'
            || id[index] == '>' || id[index] == '|' || value < 0x20U) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<std::string> descriptor_path(
    const std::string_view directory,
    const std::string_view id
) {
    if (!safe_asset_id(id)) {
        return std::nullopt;
    }
    std::string result;
    result.reserve(directory.size() + id.size() + 6U);
    result.append(directory);
    result.push_back('/');
    result.append(id);
    if (!ends_with_ascii_insensitive(result, ".json")) {
        result.append(".json");
    }
    return result;
}

[[nodiscard]] std::optional<std::string> image_path(
    std::string_view id
) {
    // PULSEFORGE_P1_2_0_PRELOAD_IMAGE_ALIASES_V1
    // The supplied source corpus contains hundreds of Psych assets below
    // assets/preload/images. Treat both the distribution-relative and
    // assets-relative spellings as already rooted instead of accidentally
    // producing images/preload/images/... or images/assets/preload/images/....
    const bool already_rooted = starts_with_ascii_insensitive(id, "images/")
        || starts_with_ascii_insensitive(id, "shared/images/")
        || starts_with_ascii_insensitive(id, "preload/images/")
        || starts_with_ascii_insensitive(id, "assets/images/")
        || starts_with_ascii_insensitive(id, "assets/shared/images/")
        || starts_with_ascii_insensitive(id, "assets/preload/images/");
    if (ends_with_ascii_insensitive(id, ".png")) {
        id.remove_suffix(4U);
    }
    if (!safe_asset_id(id)) {
        return std::nullopt;
    }
    std::string result;
    if (!already_rooted) {
        result = "images/";
    }
    result.append(id);
    result.append(".png");
    return result;
}

[[nodiscard]] std::string atlas_path_from_image(
    const std::string_view png_path
) {
    std::string result(png_path);
    if (ends_with_ascii_insensitive(result, ".png")) {
        result.resize(result.size() - 4U);
    }
    result.append(".xml");
    return result;
}

[[nodiscard]] std::optional<std::string_view> xml_attribute(
    const std::string_view tag,
    const std::string_view name
) noexcept {
    std::size_t cursor = 0U;
    while (cursor < tag.size()) {
        const auto found = tag.find(name, cursor);
        if (found == std::string_view::npos) {
            return std::nullopt;
        }
        const bool valid_left = found == 0U
            || tag[found - 1U] == ' ' || tag[found - 1U] == '\t'
            || tag[found - 1U] == '\r' || tag[found - 1U] == '\n'
            || tag[found - 1U] == '<';
        std::size_t equals = found + name.size();
        while (equals < tag.size()
               && (tag[equals] == ' ' || tag[equals] == '\t')) {
            ++equals;
        }
        if (!valid_left || equals >= tag.size() || tag[equals] != '=') {
            cursor = found + name.size();
            continue;
        }
        ++equals;
        while (equals < tag.size()
               && (tag[equals] == ' ' || tag[equals] == '\t')) {
            ++equals;
        }
        if (equals >= tag.size()
            || (tag[equals] != '"' && tag[equals] != '\'')) {
            return std::nullopt;
        }
        const char quote = tag[equals];
        const auto end = tag.find(quote, equals + 1U);
        if (end == std::string_view::npos) {
            return std::nullopt;
        }
        return tag.substr(equals + 1U, end - equals - 1U);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::int32_t> parse_xml_integer(
    const std::optional<std::string_view> value
) noexcept {
    if (!value.has_value() || value->empty() || value->size() > 24U) {
        return std::nullopt;
    }
    std::int32_t parsed{};
    const auto result = std::from_chars(
        value->data(),
        value->data() + value->size(),
        parsed
    );
    if (result.ec != std::errc{} || result.ptr != value->data() + value->size()) {
        return std::nullopt;
    }
    return parsed;
}

[[nodiscard]] std::vector<AtlasFrame> parse_sparrow_atlas(
    const std::string_view xml,
    const float texture_width,
    const float texture_height,
    const std::size_t maximum_frames
) {
    std::vector<AtlasFrame> frames;
    frames.reserve(std::min<std::size_t>(maximum_frames, 256U));
    std::size_t cursor = 0U;
    while (frames.size() < maximum_frames) {
        const auto start = xml.find("<SubTexture", cursor);
        if (start == std::string_view::npos) {
            break;
        }
        const auto end = xml.find('>', start + 11U);
        if (end == std::string_view::npos) {
            break;
        }
        cursor = end + 1U;
        if (end - start > 4'096U) {
            continue;
        }
        const auto tag = xml.substr(start, end - start + 1U);
        const auto name = xml_attribute(tag, "name");
        const auto x = parse_xml_integer(xml_attribute(tag, "x"));
        const auto y = parse_xml_integer(xml_attribute(tag, "y"));
        const auto width = parse_xml_integer(xml_attribute(tag, "width"));
        const auto height = parse_xml_integer(xml_attribute(tag, "height"));
        if (!name.has_value() || name->empty() || name->size() > 256U
            || !x.has_value() || !y.has_value() || !width.has_value()
            || !height.has_value() || *x < 0 || *y < 0 || *width <= 0
            || *height <= 0) {
            continue;
        }
        const std::int64_t right = static_cast<std::int64_t>(*x) + *width;
        const std::int64_t bottom = static_cast<std::int64_t>(*y) + *height;
        if (right > static_cast<std::int64_t>(texture_width)
            || bottom > static_cast<std::int64_t>(texture_height)) {
            continue;
        }

        const auto frame_x = parse_xml_integer(xml_attribute(tag, "frameX"));
        const auto frame_y = parse_xml_integer(xml_attribute(tag, "frameY"));
        const auto frame_width = parse_xml_integer(
            xml_attribute(tag, "frameWidth")
        );
        const auto frame_height = parse_xml_integer(
            xml_attribute(tag, "frameHeight")
        );
        const auto logical_frame_width = frame_width.value_or(*width);
        const auto logical_frame_height = frame_height.value_or(*height);
        if (logical_frame_width <= 0 || logical_frame_height <= 0
            || logical_frame_width > 32'768 || logical_frame_height > 32'768) {
            continue;
        }
        frames.push_back(AtlasFrame{
            std::string(*name),
            SDL_FRect{
                static_cast<float>(*x),
                static_cast<float>(*y),
                static_cast<float>(*width),
                static_cast<float>(*height),
            },
            static_cast<float>(logical_frame_width),
            static_cast<float>(logical_frame_height),
            static_cast<float>(-frame_x.value_or(0)),
            static_cast<float>(-frame_y.value_or(0)),
        });
    }
    return frames;
}

[[nodiscard]] std::optional<AtlasFrame> select_atlas_frame(
    const TextureResource& resource,
    const std::string_view preferred_prefix
) {
    if (resource.atlas.empty()) {
        return std::nullopt;
    }
    if (!preferred_prefix.empty()) {
        const auto found = std::find_if(
            resource.atlas.begin(),
            resource.atlas.end(),
            [&](const AtlasFrame& frame) {
                return frame.name.starts_with(preferred_prefix);
            }
        );
        if (found != resource.atlas.end()) {
            return *found;
        }
    }
    return resource.atlas.front();
}

[[nodiscard]] std::optional<std::size_t> note_skin_element_index(
    const RuntimeNoteSkinElement element
) noexcept {
    switch (element) {
        case RuntimeNoteSkinElement::receptor:
            return 0U;
        case RuntimeNoteSkinElement::note_head:
            return 1U;
        case RuntimeNoteSkinElement::sustain_body:
            return 2U;
        case RuntimeNoteSkinElement::sustain_end:
            return 3U;
    }
    return std::nullopt;
}

[[nodiscard]] std::array<std::string_view, 4U> note_frame_prefixes(
    const RuntimeNoteSkinElement element,
    const std::size_t lane
) noexcept {
    constexpr std::array receptor{
        std::array{
            std::string_view{"arrowLEFT"}, std::string_view{"left static"},
            std::string_view{}, std::string_view{},
        },
        std::array{
            std::string_view{"arrowDOWN"}, std::string_view{"down static"},
            std::string_view{}, std::string_view{},
        },
        std::array{
            std::string_view{"arrowUP"}, std::string_view{"up static"},
            std::string_view{}, std::string_view{},
        },
        std::array{
            std::string_view{"arrowRIGHT"}, std::string_view{"right static"},
            std::string_view{}, std::string_view{},
        },
    };
    constexpr std::array heads{
        std::array{
            std::string_view{"purple0"}, std::string_view{"purple scroll"},
            std::string_view{"A0"}, std::string_view{},
        },
        std::array{
            std::string_view{"blue0"}, std::string_view{"blue scroll"},
            std::string_view{"B0"}, std::string_view{},
        },
        std::array{
            std::string_view{"green0"}, std::string_view{"green scroll"},
            std::string_view{"C0"}, std::string_view{},
        },
        std::array{
            std::string_view{"red0"}, std::string_view{"red scroll"},
            std::string_view{"D0"}, std::string_view{},
        },
    };
    constexpr std::array sustain_bodies{
        std::array{
            std::string_view{"purple hold piece"}, std::string_view{},
            std::string_view{"A hold"}, std::string_view{},
        },
        std::array{
            std::string_view{"blue hold piece"}, std::string_view{},
            std::string_view{"B hold"}, std::string_view{},
        },
        std::array{
            std::string_view{"green hold piece"}, std::string_view{},
            std::string_view{"C hold"}, std::string_view{},
        },
        std::array{
            std::string_view{"red hold piece"}, std::string_view{},
            std::string_view{"D hold"}, std::string_view{},
        },
    };
    constexpr std::array sustain_ends{
        std::array{
            std::string_view{"pruple end hold"}, std::string_view{"purple hold end"},
            std::string_view{"purple end hold"}, std::string_view{"A tail"},
        },
        std::array{
            std::string_view{"blue hold end"}, std::string_view{"blue end hold"},
            std::string_view{"B tail"}, std::string_view{},
        },
        std::array{
            std::string_view{"green hold end"}, std::string_view{"green end hold"},
            std::string_view{"C tail"}, std::string_view{},
        },
        std::array{
            std::string_view{"red hold end"}, std::string_view{"red end hold"},
            std::string_view{"D tail"}, std::string_view{},
        },
    };
    if (lane >= note_skin_lane_count) {
        return {};
    }
    switch (element) {
        case RuntimeNoteSkinElement::receptor:
            return receptor[lane];
        case RuntimeNoteSkinElement::note_head:
            return heads[lane];
        case RuntimeNoteSkinElement::sustain_body:
            return sustain_bodies[lane];
        case RuntimeNoteSkinElement::sustain_end:
            return sustain_ends[lane];
    }
    return {};
}

[[nodiscard]] const AtlasFrame* select_note_atlas_frame(
    const TextureResource& resource,
    const RuntimeNoteSkinElement element,
    const std::size_t lane
) noexcept {
    const auto prefixes = note_frame_prefixes(element, lane);
    for (const auto prefix : prefixes) {
        if (prefix.empty()) {
            continue;
        }
        const auto found = std::find_if(
            resource.atlas.begin(),
            resource.atlas.end(),
            [prefix](const AtlasFrame& frame) {
                return starts_with_ascii_insensitive(frame.name, prefix);
            }
        );
        if (found != resource.atlas.end()) {
            return std::addressof(*found);
        }
    }
    return nullptr;
}

[[nodiscard]] std::string_view preferred_animation(
    const std::vector<AnimationDescriptor>& animations,
    const std::string_view requested
) noexcept {
    if (!requested.empty()) {
        const auto found = std::find_if(
            animations.begin(),
            animations.end(),
            [&](const AnimationDescriptor& animation) {
                return animation.id == requested;
            }
        );
        if (found != animations.end()) {
            return found->name;
        }
    }
    constexpr std::array preferred_ids{
        std::string_view{"idle"},
        std::string_view{"danceLeft"},
        std::string_view{"danceRight"},
    };
    for (const auto id : preferred_ids) {
        const auto found = std::find_if(
            animations.begin(),
            animations.end(),
            [&](const AnimationDescriptor& animation) {
                return animation.id == id;
            }
        );
        if (found != animations.end()) {
            return found->name;
        }
    }
    return animations.empty() ? std::string_view{} : animations.front().name;
}

[[nodiscard]] const AnimationDescriptor* preferred_animation_descriptor(
    const std::vector<AnimationDescriptor>& animations,
    const std::string_view requested
) noexcept {
    const auto prefix = preferred_animation(animations, requested);
    if (prefix.empty()) return nullptr;
    const auto found = std::find_if(
        animations.begin(), animations.end(),
        [&](const AnimationDescriptor& animation) {
            return animation.name == prefix;
        }
    );
    return found == animations.end() ? nullptr : std::addressof(*found);
}

[[nodiscard]] std::optional<SDL_Color> parse_color(
    std::string_view text
) noexcept {
    if (!text.empty() && text.front() == '#') {
        text.remove_prefix(1U);
    }
    if (text.size() != 6U && text.size() != 8U) {
        return std::nullopt;
    }
    std::uint32_t value{};
    const auto parsed = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value,
        16
    );
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return SDL_Color{
        static_cast<std::uint8_t>((value >> (text.size() == 8U ? 24U : 16U)) & 0xFFU),
        static_cast<std::uint8_t>((value >> (text.size() == 8U ? 16U : 8U)) & 0xFFU),
        static_cast<std::uint8_t>((value >> (text.size() == 8U ? 8U : 0U)) & 0xFFU),
        static_cast<std::uint8_t>(text.size() == 8U ? value & 0xFFU : 0xFFU),
    };
}

[[nodiscard]] SDL_FlipMode make_flip(
    const bool horizontal,
    const bool vertical
) noexcept {
    const int value = (horizontal ? static_cast<int>(SDL_FLIP_HORIZONTAL) : 0)
        | (vertical ? static_cast<int>(SDL_FLIP_VERTICAL) : 0);
    return static_cast<SDL_FlipMode>(value);
}

[[nodiscard]] SDL_BlendMode blend_mode(
    const std::string_view value
) noexcept {
    if (equals_ascii_insensitive(value, "add")
        || equals_ascii_insensitive(value, "screen")) {
        return SDL_BLENDMODE_ADD;
    }
    if (equals_ascii_insensitive(value, "multiply")
        || equals_ascii_insensitive(value, "darken")) {
        return SDL_BLENDMODE_MUL;
    }
    if (equals_ascii_insensitive(value, "mod")) return SDL_BLENDMODE_MOD;
    return SDL_BLENDMODE_BLEND;
}

[[nodiscard]] float finite_float(
    const double value,
    const float fallback,
    const float minimum,
    const float maximum
) noexcept {
    return std::isfinite(value)
        ? std::clamp(static_cast<float>(value), minimum, maximum)
        : fallback;
}

[[nodiscard]] double character_pulse(const double beat_position) noexcept {
    if (!std::isfinite(beat_position)) {
        return 1.0;
    }
    const double phase = beat_position - std::floor(beat_position);
    return 1.0 + 0.018 * std::exp(-phase * 9.0);
}

}  // namespace

struct RuntimeScene::Implementation {
    SDL_Renderer* renderer{};
    RuntimeSceneLimits limits;
    TimingMap timing;
    std::unique_ptr<VirtualFileSystem> files;
    std::vector<TextureResource> textures;
    std::unordered_map<std::string, std::size_t> texture_cache;
    std::vector<SceneSprite> sprites;
    // PULSEFORGE_P1_1_14_CPU_COLOR_CACHE_V1
    std::vector<CpuShaderVariant> cpu_color_variants;
    std::vector<std::uint8_t> cpu_color_scratch;
    std::uint64_t cpu_color_source_bytes{};
    std::uint64_t cpu_color_variant_bytes{};
    std::uint64_t cpu_color_pixels_this_frame{};
    std::vector<RuntimeSceneDiagnostic> diagnostics;
    NoteSkinFrames note_skin_frames{};
    // PULSEFORGE_P1_5_0B_BOUNDED_NOTE_SKIN_PROFILE_CACHE_V1
    std::vector<NoteSkinProfileEntry> note_skin_profiles;
    // PULSEFORGE_P1_5_0C_BOUNDED_NOTE_SPLASH_PROFILE_CACHE_V1
    std::vector<NoteSplashProfileEntry> note_splash_profiles;
    // Reused batch buffers: no per-frame heap churn after the high-water mark.
    std::vector<SDL_Vertex> note_skin_batch_vertices;
    std::vector<int> note_skin_batch_indices;
    std::vector<RuntimeNoteSkinDraw> note_skin_batch_fallback_draws;
    RuntimeNoteSkinProfileStats note_skin_profile{};
    bool note_skin_profiling{};
    std::uint64_t decoded_texture_bytes{};
    std::size_t atlas_frames{};
    std::size_t animation_clips{};
    std::size_t animation_frame_references{};
    bool animation_budget_diagnosed{};
    float stage_zoom{0.9F};
    // PULSEFORGE_P1_1_5_DYNAMIC_CHANGE_CHARACTER_V1
    DescriptorVec2 girlfriend_anchor{};
    DescriptorVec2 opponent_anchor{};
    DescriptorVec2 secondary_opponent_anchor{};
    DescriptorVec2 player_anchor{};
    DescriptorVec2 stage_camera_boyfriend{};
    DescriptorVec2 stage_camera_girlfriend{};
    DescriptorVec2 stage_camera_opponent{};
    DescriptorVec2 stage_camera_p4{};
    float script_camera_x{};
    float script_camera_y{};
    float script_camera_zoom{1.0F};
    double script_camera_angle{};
    float script_camera_alpha{1.0F};
    float script_hud_x{};
    float script_hud_y{};
    float script_hud_zoom{1.0F};
    double script_hud_angle{};
    float script_hud_alpha{1.0F};
    std::uint64_t next_sprite_insertion_order{};
    bool pixel_stage{};
    bool four_lane_chart{};

    Implementation(
        SDL_Renderer* input_renderer,
        const Chart& chart,
        const std::span<const std::filesystem::path> roots,
        RuntimeSceneLimits input_limits
    )
        : renderer(input_renderer),
          limits(input_limits),
          timing(chart.tempos) {
        normalize_limits();
        diagnostics.reserve(std::min<std::size_t>(limits.maximum_diagnostics, 64U));
        textures.reserve(std::min<std::size_t>(limits.maximum_textures, 256U));
        sprites.reserve(std::min<std::size_t>(limits.maximum_sprites, 1'024U));
        texture_cache.reserve(std::min<std::size_t>(limits.maximum_textures * 2U, 512U));
        cpu_color_variants.reserve(psych_cpu_color_max_variants);
        note_skin_profiles.reserve(maximum_runtime_note_skin_profiles);
        // Enough for a dense 4-lane frame in normal conditions. Buffers may
        // grow once if needed, then are retained and reused.
        note_skin_batch_vertices.reserve(8'192U);
        note_skin_batch_indices.reserve(12'288U);
        note_skin_batch_fallback_draws.reserve(2'048U);
        if (renderer == nullptr) {
            diagnose(RuntimeSceneDiagnosticSeverity::error, "runtime scene received a null SDL renderer");
            return;
        }
        mount_roots(roots);
        load(chart);
    }

    void normalize_limits() noexcept {
        limits.maximum_roots = std::clamp<std::size_t>(limits.maximum_roots, 1U, 64U);
        limits.maximum_descriptor_bytes = std::clamp<std::uintmax_t>(
            limits.maximum_descriptor_bytes,
            1'024U,
            16U * 1024U * 1024U
        );
        limits.maximum_image_bytes = std::clamp<std::uintmax_t>(
            limits.maximum_image_bytes,
            1'024U,
            128U * 1024U * 1024U
        );
        limits.maximum_atlas_bytes = std::clamp<std::uintmax_t>(
            limits.maximum_atlas_bytes,
            1'024U,
            16U * 1024U * 1024U
        );
        limits.maximum_image_dimension = std::clamp<std::uint32_t>(
            limits.maximum_image_dimension,
            64U,
            16'384U
        );
        limits.maximum_image_pixels = std::clamp<std::uint64_t>(
            limits.maximum_image_pixels,
            4'096U,
            64U * 1024U * 1024U
        );
        limits.maximum_decoded_texture_bytes = std::clamp<std::uint64_t>(
            limits.maximum_decoded_texture_bytes,
            4U * 1'024U * 1'024U,
            1'024ULL * 1'024ULL * 1'024ULL
        );
        limits.maximum_textures = std::clamp<std::size_t>(limits.maximum_textures, 1U, 1'024U);
        limits.maximum_sprites = std::clamp<std::size_t>(limits.maximum_sprites, 4U, 4'096U);
        limits.maximum_atlas_frames = std::clamp<std::size_t>(
            limits.maximum_atlas_frames,
            1U,
            131'072U
        );
        limits.maximum_animation_clips = std::clamp<std::size_t>(
            limits.maximum_animation_clips,
            1U,
            65'536U
        );
        limits.maximum_animation_frame_references = std::clamp<std::size_t>(
            limits.maximum_animation_frame_references,
            1U,
            1'048'576U
        );
        limits.maximum_diagnostics = std::clamp<std::size_t>(
            limits.maximum_diagnostics,
            1U,
            1'024U
        );
    }

    void diagnose(
        const RuntimeSceneDiagnosticSeverity severity,
        std::string message
    ) {
        if (diagnostics.size() < limits.maximum_diagnostics) {
            diagnostics.push_back({severity, std::move(message)});
        }
    }

    void mount_roots(
        const std::span<const std::filesystem::path> input_roots
    ) {
        VirtualFileSystemOptions options;
        options.max_mounts = limits.maximum_roots;
        options.max_read_bytes = std::max({
            limits.maximum_descriptor_bytes,
            limits.maximum_image_bytes,
            limits.maximum_atlas_bytes,
        });
        const auto count = std::min(input_roots.size(), limits.maximum_roots);
        options.mounts.reserve(count);
        for (std::size_t index = 0U; index < count; ++index) {
            if (input_roots[index].empty()) {
                continue;
            }
            std::error_code error;
            if (!std::filesystem::is_directory(input_roots[index], error)
                || error) {
                diagnose(
                    RuntimeSceneDiagnosticSeverity::warning,
                    "ignored missing or inaccessible scene root"
                );
                continue;
            }
            options.mounts.push_back(DirectoryMount{
                "scene-root-" + std::to_string(index),
                input_roots[index],
                static_cast<std::int32_t>(index),
            });
        }
        if (input_roots.size() > count) {
            diagnose(
                RuntimeSceneDiagnosticSeverity::warning,
                "scene root count exceeded its configured limit"
            );
        }
        try {
            files = std::make_unique<VirtualFileSystem>(std::move(options));
        } catch (const VirtualFileSystemError& error) {
            diagnose(
                RuntimeSceneDiagnosticSeverity::error,
                "failed to create the scene filesystem: "
                    + std::string(error.what())
            );
        }
    }

    [[nodiscard]] std::optional<std::string> read_text(
        const std::string_view path,
        const std::uintmax_t limit
    ) {
        if (files == nullptr) {
            return std::nullopt;
        }
        try {
            if (!files->resolve(path).has_value()) {
                return std::nullopt;
            }
            return files->read_text(path, limit).text;
        } catch (const VirtualFileSystemError& error) {
            diagnose(
                RuntimeSceneDiagnosticSeverity::warning,
                "rejected scene text asset: " + std::string(error.what())
            );
            return std::nullopt;
        }
    }

    [[nodiscard]] std::optional<std::string> read_text_from_mount(
        const std::string_view path,
        const std::uintmax_t limit,
        const std::string_view required_mount_id
    ) {
        if (files == nullptr) {
            return std::nullopt;
        }
        try {
            if (!files->resolve(path).has_value()) {
                return std::nullopt;
            }
            auto loaded = files->read_text(path, limit);
            if (loaded.source.provenance.mount_id != required_mount_id) {
                return std::nullopt;
            }
            return std::move(loaded.text);
        } catch (const VirtualFileSystemError& error) {
            diagnose(
                RuntimeSceneDiagnosticSeverity::warning,
                "rejected scene text asset: " + std::string(error.what())
            );
            return std::nullopt;
        }
    }

    void append_parser_diagnostics(
        const std::string_view kind,
        const std::span<const DescriptorDiagnostic> parsed
    ) {
        for (const auto& item : parsed) {
            const auto severity = item.severity == DescriptorDiagnosticSeverity::error
                ? RuntimeSceneDiagnosticSeverity::error
                : RuntimeSceneDiagnosticSeverity::warning;
            diagnose(
                severity,
                std::string(kind) + " descriptor " + item.path + ": "
                    + item.message
            );
        }
    }

    [[nodiscard]] DescriptorParseOptions descriptor_options() const noexcept {
        DescriptorParseOptions options;
        options.mode = DescriptorParseMode::permissive;
        options.limits.max_input_bytes = static_cast<std::size_t>(
            std::min<std::uintmax_t>(
                limits.maximum_descriptor_bytes,
                std::numeric_limits<std::size_t>::max()
            )
        );
        options.limits.max_array_items = std::max<std::size_t>(
            limits.maximum_sprites,
            64U
        );
        options.limits.max_total_nodes = std::min<std::size_t>(
            100'000U,
            options.limits.max_array_items * 32U
        );
        options.limits.max_diagnostics = limits.maximum_diagnostics;
        return options;
    }

    [[nodiscard]] StageDescriptor load_stage(const std::string_view raw_id) {
        const std::string id = raw_id.empty() ? "stage" : std::string(raw_id);
        StageDescriptor fallback;
        fallback.id = id;
        std::optional<StageDescriptor> json_stage;
        const auto path = descriptor_path("stages", id);
        if (!path.has_value()) {
            diagnose(RuntimeSceneDiagnosticSeverity::warning, "unsafe stage id was rejected");
            return fallback;
        }
        if (const auto text = read_text(*path, limits.maximum_descriptor_bytes);
            text.has_value()) {
            auto parsed = ContentDescriptorParser::parse_psych_stage(
                *text,
                id,
                descriptor_options()
            );
            append_parser_diagnostics("stage", parsed.diagnostics);
            if (parsed.value.has_value()) {
                json_stage = std::move(*parsed.value);
            } else {
                diagnose(
                    RuntimeSceneDiagnosticSeverity::warning,
                    "stage JSON descriptor could not be used; trying static Lua"
                );
            }
        }

        const auto lua_path = descriptor_path("stages", id);
        if (!lua_path.has_value()) return json_stage.value_or(fallback);
        auto lua_name = *lua_path;
        if (ends_with_ascii_insensitive(lua_name, ".json")) {
            lua_name.resize(lua_name.size() - 5U);
        }
        lua_name.append(".lua");
        const auto lua = read_text(lua_name, limits.maximum_descriptor_bytes);
        if (!lua.has_value()) return json_stage.value_or(fallback);

        StaticStageLuaLimits lua_limits;
        lua_limits.maximum_source_bytes = static_cast<std::size_t>(
            std::min<std::uintmax_t>(
                limits.maximum_descriptor_bytes,
                std::numeric_limits<std::size_t>::max()
            )
        );
        lua_limits.maximum_sprites = limits.maximum_sprites;
        lua_limits.maximum_animations = std::min(
            limits.maximum_atlas_frames,
            limits.maximum_animation_clips
        );
        lua_limits.maximum_diagnostics = limits.maximum_diagnostics;
        auto parsed_lua = parse_static_psych_stage_lua(*lua, id, lua_limits);
        for (const auto& item : parsed_lua.diagnostics) {
            diagnose(
                RuntimeSceneDiagnosticSeverity::warning,
                "stage Lua line " + std::to_string(item.line) + ": "
                    + item.message
            );
        }
        if (!parsed_lua.stage.has_value()) {
            diagnose(
                RuntimeSceneDiagnosticSeverity::warning,
                "stage Lua contained no safe static scene; fallback layout selected"
            );
            return json_stage.value_or(fallback);
        }
        diagnose(
            RuntimeSceneDiagnosticSeverity::warning,
            "loaded safe static scene declarations from stage Lua"
        );
        if (!json_stage.has_value()) return std::move(*parsed_lua.stage);

        // Psych commonly splits stage configuration (positions/camera/zoom)
        // into JSON and scene objects into Lua. Preserve the JSON as the base
        // and append only the bounded, statically proven Lua declarations.
        auto& base = *json_stage;
        auto& lua_objects = parsed_lua.stage->objects;
        const auto available = limits.maximum_sprites - std::min(
            limits.maximum_sprites,
            base.objects.size()
        );
        const auto append_count = std::min(available, lua_objects.size());
        base.objects.reserve(base.objects.size() + append_count);
        for (std::size_t index = 0U; index < append_count; ++index) {
            base.objects.push_back(std::move(lua_objects[index]));
        }
        if (append_count != lua_objects.size()) {
            diagnose(
                RuntimeSceneDiagnosticSeverity::warning,
                "stage JSON plus Lua objects exceeded the configured sprite limit"
            );
        }
        base.hide_girlfriend = base.hide_girlfriend
            || parsed_lua.stage->hide_girlfriend;
        return std::move(base);
    }

    [[nodiscard]] CharacterDescriptor load_character(
        const std::string_view raw_id,
        const std::string_view fallback_id
    ) {
        const std::string id = raw_id.empty()
            ? std::string(fallback_id)
            : std::string(raw_id);
        CharacterDescriptor fallback;
        fallback.id = id;
        fallback.image = "characters/" + id;
        const auto path = descriptor_path("characters", id);
        if (!path.has_value()) {
            diagnose(RuntimeSceneDiagnosticSeverity::warning, "unsafe character id was rejected");
            return fallback;
        }
        const auto text = read_text(*path, limits.maximum_descriptor_bytes);
        if (!text.has_value()) {
            return fallback;
        }
        auto parsed = ContentDescriptorParser::parse_psych_character(
            *text,
            id,
            descriptor_options()
        );
        append_parser_diagnostics("character", parsed.diagnostics);
        if (!parsed.value.has_value()) {
            diagnose(RuntimeSceneDiagnosticSeverity::warning, "character descriptor could not be used; fallback actor selected");
            return fallback;
        }
        if (parsed.value->image.empty()) {
            parsed.value->image = fallback.image;
        }
        return std::move(*parsed.value);
    }

    [[nodiscard]] std::optional<CharacterDescriptor>
    load_character_for_change(const std::string_view raw_id) {
        // PULSEFORGE_P1_1_5_DYNAMIC_CHANGE_CHARACTER_V1
        if (raw_id.empty() || !safe_asset_id(raw_id)) {
            diagnose(
                RuntimeSceneDiagnosticSeverity::warning,
                "Change Character rejected an empty or unsafe character id"
            );
            return std::nullopt;
        }

        const std::string id(raw_id);
        const auto path = descriptor_path("characters", id);
        if (!path.has_value()) {
            diagnose(
                RuntimeSceneDiagnosticSeverity::warning,
                std::string{"Change Character rejected unsafe descriptor: "} + id
            );
            return std::nullopt;
        }

        const auto descriptor_text =
            read_text(*path, limits.maximum_descriptor_bytes);
        if (!descriptor_text.has_value()) {
            diagnose(
                RuntimeSceneDiagnosticSeverity::warning,
                std::string{"Change Character descriptor was not found: "} + id
            );
            return std::nullopt;
        }

        auto parsed = ContentDescriptorParser::parse_psych_character(
            *descriptor_text,
            id,
            descriptor_options()
        );
        append_parser_diagnostics("character", parsed.diagnostics);
        if (!parsed.value.has_value()) {
            diagnose(
                RuntimeSceneDiagnosticSeverity::warning,
                std::string{"Change Character descriptor is invalid: "} + id
            );
            return std::nullopt;
        }
        if (parsed.value->image.empty()) {
            parsed.value->image = "characters/" + id;
        }
        return std::move(*parsed.value);
    }


    [[nodiscard]] static std::uint32_t png_u32(
        const std::byte* bytes
    ) noexcept {
        const auto* values = reinterpret_cast<const unsigned char*>(bytes);
        return (static_cast<std::uint32_t>(values[0]) << 24U)
            | (static_cast<std::uint32_t>(values[1]) << 16U)
            | (static_cast<std::uint32_t>(values[2]) << 8U)
            | static_cast<std::uint32_t>(values[3]);
    }

    [[nodiscard]] bool valid_png_header(
        const std::span<const std::byte> bytes,
        std::uint32_t& width,
        std::uint32_t& height
    ) const noexcept {
        constexpr std::array<unsigned char, 8U> signature{
            137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U,
        };
        if (bytes.size() < 33U) {
            return false;
        }
        for (std::size_t index = 0U; index < signature.size(); ++index) {
            if (std::to_integer<unsigned char>(bytes[index]) != signature[index]) {
                return false;
            }
        }
        if (png_u32(bytes.data() + 8U) != 13U
            || std::to_integer<unsigned char>(bytes[12U]) != 'I'
            || std::to_integer<unsigned char>(bytes[13U]) != 'H'
            || std::to_integer<unsigned char>(bytes[14U]) != 'D'
            || std::to_integer<unsigned char>(bytes[15U]) != 'R') {
            return false;
        }
        width = png_u32(bytes.data() + 16U);
        height = png_u32(bytes.data() + 20U);
        return width != 0U && height != 0U;
    }

    [[nodiscard]] std::size_t load_texture(const std::string_view raw_id) {
        const auto virtual_path = image_path(raw_id);
        if (!virtual_path.has_value()) {
            diagnose(RuntimeSceneDiagnosticSeverity::warning, "unsafe image id was rejected");
            return no_texture;
        }
        if (const auto cached = texture_cache.find(*virtual_path);
            cached != texture_cache.end()) {
            return cached->second;
        }
        if (textures.size() >= limits.maximum_textures || files == nullptr) {
            texture_cache.emplace(*virtual_path, no_texture);
            if (textures.size() >= limits.maximum_textures) {
                diagnose(RuntimeSceneDiagnosticSeverity::warning, "scene texture count exceeded its configured limit");
            }
            return no_texture;
        }

        BinaryVirtualFile encoded;
        try {
            if (!files->resolve(*virtual_path).has_value()) {
                texture_cache.emplace(*virtual_path, no_texture);
                return no_texture;
            }
            encoded = files->read_binary(*virtual_path, limits.maximum_image_bytes);
        } catch (const VirtualFileSystemError& error) {
            diagnose(
                RuntimeSceneDiagnosticSeverity::warning,
                "rejected scene image: " + std::string(error.what())
            );
            texture_cache.emplace(*virtual_path, no_texture);
            return no_texture;
        }
        std::uint32_t width{};
        std::uint32_t height{};
        if (!valid_png_header(encoded.bytes, width, height)
            || encoded.bytes.size() > static_cast<std::size_t>(
                std::numeric_limits<int>::max()
            )) {
            diagnose(
                RuntimeSceneDiagnosticSeverity::warning,
                "malformed PNG was rejected: " + *virtual_path
            );
            texture_cache.emplace(*virtual_path, no_texture);
            return no_texture;
        }
        // PULSEFORGE_P1_1_9_TEXTURE_ADMISSION_DIAGNOSTICS_V1
        const auto texture_admission = evaluate_runtime_texture_admission(
            width,
            height,
            decoded_texture_bytes,
            RuntimeTextureAdmissionPolicy{
                limits.maximum_image_dimension,
                limits.maximum_image_pixels,
                limits.maximum_decoded_texture_bytes_per_texture,
                limits.maximum_decoded_texture_bytes,
            }
        );
        if (!texture_admission.accepted()) {
            diagnose(
                RuntimeSceneDiagnosticSeverity::warning,
                "scene image rejected '" + *virtual_path
                    + "': "
                    + runtime_texture_admission_failure_name(
                        texture_admission.failure
                    )
                    + " (" + std::to_string(width) + "x"
                    + std::to_string(height)
                    + ", decoded "
                    + std::to_string(texture_admission.decoded_bytes)
                    + " bytes, scene "
                    + std::to_string(decoded_texture_bytes)
                    + "/"
                    + std::to_string(
                        limits.maximum_decoded_texture_bytes
                    )
                    + " bytes)"
            );
            texture_cache.emplace(*virtual_path, no_texture);
            return no_texture;
        }
        const std::uint64_t decoded_bytes =
            texture_admission.decoded_bytes;

        int decoded_width{};
        int decoded_height{};
        int components{};
        auto* pixels = stbi_load_from_memory(
            reinterpret_cast<const stbi_uc*>(encoded.bytes.data()),
            static_cast<int>(encoded.bytes.size()),
            &decoded_width,
            &decoded_height,
            &components,
            STBI_rgb_alpha
        );
        if (pixels == nullptr || decoded_width != static_cast<int>(width)
            || decoded_height != static_cast<int>(height)) {
            stbi_image_free(pixels);
            diagnose(RuntimeSceneDiagnosticSeverity::warning, "PNG decoding failed or dimensions changed during decoding");
            texture_cache.emplace(*virtual_path, no_texture);
            return no_texture;
        }
        SDL_Surface* surface = SDL_CreateSurfaceFrom(
            decoded_width,
            decoded_height,
            SDL_PIXELFORMAT_RGBA32,
            pixels,
            decoded_width * 4
        );
        SDL_Texture* raw_texture = surface != nullptr
            ? SDL_CreateTextureFromSurface(renderer, surface)
            : nullptr;
        SDL_DestroySurface(surface);
        stbi_image_free(pixels);
        if (raw_texture == nullptr) {
            diagnose(
                RuntimeSceneDiagnosticSeverity::warning,
                "SDL texture creation failed: " + std::string(SDL_GetError())
            );
            texture_cache.emplace(*virtual_path, no_texture);
            return no_texture;
        }
        TextureResource resource;
        resource.texture.reset(raw_texture);
        resource.width = static_cast<float>(width);
        resource.height = static_cast<float>(height);
        resource.source_virtual_path = *virtual_path;
        resource.source_mount_id = encoded.source.provenance.mount_id;
        static_cast<void>(SDL_SetTextureBlendMode(
            resource.texture.get(),
            SDL_BLENDMODE_BLEND
        ));

        const auto remaining_frames = limits.maximum_atlas_frames - std::min(
            atlas_frames,
            limits.maximum_atlas_frames
        );
        if (remaining_frames > 0U) {
            const auto atlas_path = atlas_path_from_image(*virtual_path);
            if (const auto xml = read_text_from_mount(
                    atlas_path,
                    limits.maximum_atlas_bytes,
                    encoded.source.provenance.mount_id
                );
                xml.has_value()) {
                resource.atlas = parse_sparrow_atlas(
                    *xml,
                    resource.width,
                    resource.height,
                    remaining_frames
                );
                atlas_frames += resource.atlas.size();
            }
        }
        resource.atlas_name_order.resize(resource.atlas.size());
        std::iota(
            resource.atlas_name_order.begin(),
            resource.atlas_name_order.end(),
            std::uint32_t{0U}
        );
        std::stable_sort(
            resource.atlas_name_order.begin(),
            resource.atlas_name_order.end(),
            [&](const std::uint32_t left, const std::uint32_t right) {
                return ascii_less_insensitive(
                    resource.atlas[left].name,
                    resource.atlas[right].name
                );
            }
        );

        const auto index = textures.size();
        textures.push_back(std::move(resource));
        texture_cache.emplace(*virtual_path, index);
        decoded_texture_bytes += decoded_bytes;
        return index;
    }

    struct ResolvedNoteImage {
        std::string raw_id;
        std::string mount_id;
        std::int32_t mount_priority{};
        std::size_t mount_declaration_order{};
    };

    [[nodiscard]] std::optional<ResolvedNoteImage> resolve_note_image(
        const std::string_view raw_id
    ) {
        const auto virtual_path = image_path(raw_id);
        if (!virtual_path.has_value() || files == nullptr) {
            return std::nullopt;
        }
        try {
            const auto resolved = files->resolve(*virtual_path);
            if (!resolved.has_value()) {
                return std::nullopt;
            }
            return ResolvedNoteImage{
                std::string(raw_id),
                resolved->provenance.mount_id,
                resolved->provenance.mount_priority,
                resolved->provenance.mount_declaration_order,
            };
        } catch (const VirtualFileSystemError& error) {
            diagnose(
                RuntimeSceneDiagnosticSeverity::warning,
                "rejected note-skin image: " + std::string(error.what())
            );
            return std::nullopt;
        }
    }

    [[nodiscard]] std::optional<ResolvedNoteImage> best_note_image(
        const std::span<const std::string> raw_candidates
    ) {
        std::optional<ResolvedNoteImage> best;
        for (const auto& candidate : raw_candidates) {
            auto resolved = resolve_note_image(candidate);
            if (!resolved.has_value()) {
                continue;
            }
            if (!best.has_value()
                || resolved->mount_priority > best->mount_priority
                || (resolved->mount_priority == best->mount_priority
                    && resolved->mount_declaration_order
                        > best->mount_declaration_order)) {
                best = std::move(resolved);
            }
        }
        return best;
    }

    [[nodiscard]] std::size_t note_skin_frame_count() const noexcept {
        std::size_t count{};
        for (const auto& element : note_skin_frames) {
            for (const auto& frame : element) {
                count += frame.valid ? 1U : 0U;
            }
        }
        return count;
    }

    void assign_atlas_note_skin(const std::size_t texture_index) noexcept {
        if (texture_index == no_texture || texture_index >= textures.size()) {
            return;
        }
        const auto& resource = textures[texture_index];
        constexpr std::array elements{
            RuntimeNoteSkinElement::receptor,
            RuntimeNoteSkinElement::note_head,
            RuntimeNoteSkinElement::sustain_body,
            RuntimeNoteSkinElement::sustain_end,
        };
        for (const auto element : elements) {
            const auto element_index = note_skin_element_index(element);
            if (!element_index.has_value()) {
                continue;
            }
            for (std::size_t lane = 0U; lane < note_skin_lane_count; ++lane) {
                const auto* frame = select_note_atlas_frame(
                    resource,
                    element,
                    lane
                );
                if (frame == nullptr) {
                    continue;
                }
                note_skin_frames[*element_index][lane] = NoteSkinFrame{
                    texture_index,
                    frame->source,
                    frame->frame_width,
                    frame->frame_height,
                    frame->content_offset_x,
                    frame->content_offset_y,
                    pixel_stage ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR,
                    true,
                };
            }
        }
    }

    void assign_grid_note_frame(
        const RuntimeNoteSkinElement element,
        const std::size_t lane,
        const std::size_t texture_index,
        const std::uint32_t columns,
        const std::uint32_t rows,
        const std::uint32_t frame_index
    ) noexcept {
        const auto element_index = note_skin_element_index(element);
        if (!element_index.has_value() || lane >= note_skin_lane_count
            || texture_index == no_texture || texture_index >= textures.size()
            || columns == 0U || rows == 0U
            || frame_index >= columns * rows) {
            return;
        }
        const auto& resource = textures[texture_index];
        const float cell_width = resource.width / static_cast<float>(columns);
        const float cell_height = resource.height / static_cast<float>(rows);
        if (!std::isfinite(cell_width) || !std::isfinite(cell_height)
            || cell_width <= 0.0F || cell_height <= 0.0F) {
            return;
        }
        const auto column = frame_index % columns;
        const auto row = frame_index / columns;
        note_skin_frames[*element_index][lane] = NoteSkinFrame{
            texture_index,
            SDL_FRect{
                static_cast<float>(column) * cell_width,
                static_cast<float>(row) * cell_height,
                cell_width,
                cell_height,
            },
            cell_width,
            cell_height,
            0.0F,
            0.0F,
            SDL_SCALEMODE_NEAREST,
            true,
        };
    }

    [[nodiscard]] std::uint32_t assign_pixel_arrow_frames(
        const std::size_t texture_index
    ) noexcept {
        if (texture_index == no_texture || texture_index >= textures.size()) {
            return 0U;
        }
        const auto& resource = textures[texture_index];
        const auto width = static_cast<std::uint32_t>(resource.width);
        const auto height = static_cast<std::uint32_t>(resource.height);
        constexpr std::array candidate_rows{5U, 3U};
        for (const auto rows : candidate_rows) {
            if (height < rows || height % rows != 0U) {
                continue;
            }
            const auto cell = height / rows;
            if (cell == 0U || width % cell != 0U) {
                continue;
            }
            const auto columns = width / cell;
            if (columns < note_skin_lane_count || columns > 64U) {
                continue;
            }
            for (std::size_t lane = 0U; lane < note_skin_lane_count; ++lane) {
                assign_grid_note_frame(
                    RuntimeNoteSkinElement::receptor,
                    lane,
                    texture_index,
                    columns,
                    rows,
                    static_cast<std::uint32_t>(lane)
                );
                assign_grid_note_frame(
                    RuntimeNoteSkinElement::note_head,
                    lane,
                    texture_index,
                    columns,
                    rows,
                    columns + static_cast<std::uint32_t>(lane)
                );
            }
            return columns;
        }
        return 0U;
    }

    void assign_pixel_sustain_frames(
        const std::size_t texture_index,
        const std::uint32_t columns
    ) noexcept {
        if (texture_index == no_texture || texture_index >= textures.size()
            || columns < note_skin_lane_count) {
            return;
        }
        const auto& resource = textures[texture_index];
        const auto width = static_cast<std::uint32_t>(resource.width);
        const auto height = static_cast<std::uint32_t>(resource.height);
        if (width == 0U || height < 2U || width % columns != 0U
            || height % 2U != 0U) {
            return;
        }
        for (std::size_t lane = 0U; lane < note_skin_lane_count; ++lane) {
            assign_grid_note_frame(
                RuntimeNoteSkinElement::sustain_body,
                lane,
                texture_index,
                columns,
                2U,
                static_cast<std::uint32_t>(lane)
            );
            assign_grid_note_frame(
                RuntimeNoteSkinElement::sustain_end,
                lane,
                texture_index,
                columns,
                2U,
                columns + static_cast<std::uint32_t>(lane)
            );
        }
    }

    [[nodiscard]] static std::string pixel_end_style(
        const std::string_view style
    ) {
        if (equals_ascii_insensitive(style, "arrows-pixels")) {
            return "arrowEnds";
        }
        constexpr std::string_view note_assets{"NOTE_assets"};
        if (starts_with_ascii_insensitive(style, note_assets)) {
            std::string result{"NOTE_assetsENDS"};
            result.append(style.substr(note_assets.size()));
            return result;
        }
        std::string result(style);
        result.append("ENDS");
        return result;
    }

    [[nodiscard]] bool try_pixel_note_skin(
        const std::string_view style
    ) {
        if (!safe_asset_id(style) || style.find('/') != std::string_view::npos) {
            return false;
        }
        constexpr std::array roots{
            std::string_view{},
            std::string_view{"shared/images/"},
            std::string_view{"assets/shared/images/"},
        };
        constexpr std::array directories{
            std::string_view{"pixelUI/"},
            std::string_view{"pixelUI/noteSkins/"},
            std::string_view{"pixelUI/noteskins/"},
        };
        std::vector<std::string> candidates;
        candidates.reserve(roots.size() * directories.size());
        for (const auto root : roots) {
            for (const auto directory : directories) {
                std::string arrow;
                arrow.reserve(root.size() + directory.size() + style.size());
                arrow.append(root);
                arrow.append(directory);
                arrow.append(style);
                candidates.push_back(std::move(arrow));
            }
        }
        const auto selected = best_note_image(candidates);
        if (!selected.has_value()) {
            return false;
        }

        const auto end_style = pixel_end_style(style);
        const auto arrow_texture = load_texture(selected->raw_id);
        if (arrow_texture == no_texture) {
            return true;
        }
        const auto columns = assign_pixel_arrow_frames(arrow_texture);
        if (columns == 0U) {
            return true;
        }

        std::string ends = selected->raw_id.substr(
            0U,
            selected->raw_id.size() - style.size()
        );
        ends.append(end_style);
        const auto resolved_ends = resolve_note_image(ends);
        if (resolved_ends.has_value()
            && resolved_ends->mount_id == selected->mount_id) {
            const auto end_texture = load_texture(ends);
            assign_pixel_sustain_frames(end_texture, columns);
        }
        return true;
    }

    [[nodiscard]] bool try_atlas_note_skin(
        const std::string_view style
    ) {
        if (!safe_asset_id(style)) {
            return false;
        }
        if (style.find('/') != std::string_view::npos) {
            const auto selected = resolve_note_image(style);
            if (!selected.has_value()) {
                return false;
            }
            assign_atlas_note_skin(load_texture(selected->raw_id));
            return true;
        }
        constexpr std::array roots{
            std::string_view{},
            std::string_view{"shared/images/"},
            std::string_view{"assets/shared/images/"},
        };
        constexpr std::array directories{
            std::string_view{},
            std::string_view{"noteSkins/"},
            std::string_view{"noteskins/"},
        };
        std::vector<std::string> candidates;
        candidates.reserve(roots.size() * directories.size());
        for (const auto root : roots) {
            for (const auto directory : directories) {
                std::string candidate;
                candidate.reserve(root.size() + directory.size() + style.size());
                candidate.append(root);
                candidate.append(directory);
                candidate.append(style);
                candidates.push_back(std::move(candidate));
            }
        }
        const auto selected = best_note_image(candidates);
        if (!selected.has_value()) {
            return false;
        }
        assign_atlas_note_skin(load_texture(selected->raw_id));
        return true;
    }

    void finalize_note_skin(const bool selected) {
        if (!selected) {
            return;
        }
        const auto frame_count = note_skin_frame_count();
        if (frame_count < note_skin_lane_count * note_skin_element_count) {
            diagnose(
                RuntimeSceneDiagnosticSeverity::warning,
                "selected note skin is incomplete (" + std::to_string(frame_count)
                    + "/16 frames); geometric fallbacks remain active"
            );
        }
    }

    void load_note_skin(
        const std::string_view raw_style,
        const bool prefer_pixel
    ) {
        if (!four_lane_chart) {
            return;
        }
        std::string_view style = raw_style;
        if (!style.empty() && !safe_asset_id(style)) {
            diagnose(
                RuntimeSceneDiagnosticSeverity::warning,
                "unsafe note-style id was rejected"
            );
            style = {};
        }

        const bool pixel_alias = equals_ascii_insensitive(style, "pixel")
            || equals_ascii_insensitive(style, "pixelUI");
        bool selected{};
        if (prefer_pixel || pixel_alias) {
            if (!style.empty() && !pixel_alias) {
                selected = try_pixel_note_skin(style);
            }
            if (!selected) {
                selected = try_pixel_note_skin("arrows-pixels");
            }
            if (!selected) {
                selected = try_pixel_note_skin("NOTE_assets");
            }
            if (selected) {
                finalize_note_skin(true);
                return;
            }
        }

        if (!style.empty() && !pixel_alias) {
            selected = try_atlas_note_skin(style);
        }
        if (!selected) {
            selected = try_atlas_note_skin("NOTE_assets");
        }
        finalize_note_skin(selected);
    }

    [[nodiscard]] bool load_exact_note_skin(
        const std::string_view raw_style,
        const bool force_pixel
    ) {
        if (!four_lane_chart || raw_style.empty() || !safe_asset_id(raw_style)) {
            return false;
        }

        const bool pixel_alias = equals_ascii_insensitive(raw_style, "pixel")
            || equals_ascii_insensitive(raw_style, "pixelUI");
        bool selected{};
        if (force_pixel || pixel_alias) {
            selected = pixel_alias
                ? try_pixel_note_skin("arrows-pixels")
                : try_pixel_note_skin(raw_style);
        } else {
            selected = try_atlas_note_skin(raw_style);
        }
        finalize_note_skin(selected);
        return selected && note_skin_frame_count() != 0U;
    }

    [[nodiscard]] std::optional<RuntimeNoteSkinProfile> resolve_note_skin_profile(
        const std::string_view style,
        const bool force_pixel
    ) {
        if (!four_lane_chart || style.empty() || !safe_asset_id(style)) {
            return std::nullopt;
        }

        const auto found = std::find_if(
            note_skin_profiles.begin(),
            note_skin_profiles.end(),
            [&](const NoteSkinProfileEntry& profile) {
                return profile.force_pixel == force_pixel
                    && profile.style == style;
            }
        );
        if (found != note_skin_profiles.end()) {
            return static_cast<RuntimeNoteSkinProfile>(
                std::distance(note_skin_profiles.begin(), found) + 1
            );
        }
        if (note_skin_profiles.size() >= maximum_runtime_note_skin_profiles) {
            diagnose(
                RuntimeSceneDiagnosticSeverity::warning,
                "custom note-skin profile budget exhausted"
            );
            return std::nullopt;
        }

        const auto default_frames = note_skin_frames;
        note_skin_frames = {};
        bool selected{};
        try {
            selected = load_exact_note_skin(style, force_pixel);
        } catch (...) {
            note_skin_frames = default_frames;
            throw;
        }
        auto profile_frames = note_skin_frames;
        note_skin_frames = default_frames;
        if (!selected) {
            return std::nullopt;
        }

        note_skin_profiles.push_back(NoteSkinProfileEntry{
            std::string(style),
            force_pixel,
            std::move(profile_frames),
        });
        return static_cast<RuntimeNoteSkinProfile>(note_skin_profiles.size());
    }

    [[nodiscard]] std::optional<RuntimeNoteSplashProfile>
    resolve_note_splash_profile(const std::string_view style) {
        if (style.empty() || !safe_asset_id(style)) {
            return std::nullopt;
        }
        const auto found = std::find_if(
            note_splash_profiles.begin(),
            note_splash_profiles.end(),
            [&](const NoteSplashProfileEntry& profile) {
                return profile.style == style;
            }
        );
        if (found != note_splash_profiles.end()) {
            return static_cast<RuntimeNoteSplashProfile>(
                std::distance(note_splash_profiles.begin(), found) + 1
            );
        }
        if (note_splash_profiles.size() >= maximum_runtime_note_splash_profiles) {
            diagnose(
                RuntimeSceneDiagnosticSeverity::warning,
                "custom note-splash profile budget exhausted"
            );
            return std::nullopt;
        }

        std::optional<ResolvedNoteImage> selected;
        if (style.find('/') != std::string_view::npos) {
            selected = resolve_note_image(style);
        } else {
            constexpr std::array roots{
                std::string_view{},
                std::string_view{"shared/images/"},
                std::string_view{"assets/shared/images/"},
            };
            constexpr std::array directories{
                std::string_view{},
                std::string_view{"noteSplashes/"},
                std::string_view{"notesplashes/"},
            };
            std::vector<std::string> candidates;
            candidates.reserve(roots.size() * directories.size());
            for (const auto root : roots) {
                for (const auto directory : directories) {
                    std::string candidate;
                    candidate.reserve(root.size() + directory.size() + style.size());
                    candidate.append(root);
                    candidate.append(directory);
                    candidate.append(style);
                    candidates.push_back(std::move(candidate));
                }
            }
            selected = best_note_image(candidates);
        }
        if (!selected.has_value()) {
            return std::nullopt;
        }
        const auto texture_index = load_texture(selected->raw_id);
        if (texture_index == no_texture || texture_index >= textures.size()) {
            return std::nullopt;
        }
        const auto& resource = textures[texture_index];
        if (resource.atlas.empty()) {
            return std::nullopt;
        }
        NoteSplashProfileEntry profile;
        profile.style.assign(style);
        profile.texture_index = texture_index;
        profile.frame_indices.reserve(std::min<std::size_t>(
            resource.atlas.size(), maximum_runtime_note_splash_frames_per_profile
        ));
        for (std::size_t index = 0U;
             index < resource.atlas.size()
                 && profile.frame_indices.size()
                    < maximum_runtime_note_splash_frames_per_profile;
             ++index) {
            const auto& name = resource.atlas[index].name;
            if (starts_with_ascii_insensitive(name, "note splash")
                || starts_with_ascii_insensitive(name, "notesplash")
                || contains_ascii_insensitive(name, "plash")) {
                profile.frame_indices.push_back(static_cast<std::uint32_t>(index));
            }
        }
        // Forks use many naming conventions. A safe Sparrow atlas is still a
        // valid splash source even when it omits the stock Psych prefix.
        if (profile.frame_indices.empty()) {
            for (std::size_t index = 0U;
                 index < resource.atlas.size()
                     && profile.frame_indices.size()
                        < maximum_runtime_note_splash_frames_per_profile;
                 ++index) {
                profile.frame_indices.push_back(static_cast<std::uint32_t>(index));
            }
        }
        if (profile.frame_indices.empty()) {
            return std::nullopt;
        }
        note_splash_profiles.push_back(std::move(profile));
        return static_cast<RuntimeNoteSplashProfile>(note_splash_profiles.size());
    }

    [[nodiscard]] std::size_t note_splash_frame_count(
        const RuntimeNoteSplashProfile profile
    ) const noexcept {
        if (profile == runtime_note_splash_invalid_profile
            || profile > note_splash_profiles.size()) {
            return 0U;
        }
        return note_splash_profiles[static_cast<std::size_t>(profile - 1U)]
            .frame_indices.size();
    }

    [[nodiscard]] bool render_note_splash(
        const RuntimeNoteSplashDraw& draw
    ) noexcept {
        const auto frame_count = note_splash_frame_count(draw.profile);
        if (renderer == nullptr || frame_count == 0U
            || !std::isfinite(draw.destination.x)
            || !std::isfinite(draw.destination.y)
            || !std::isfinite(draw.destination.width)
            || !std::isfinite(draw.destination.height)
            || draw.destination.width <= 0.0F
            || draw.destination.height <= 0.0F) {
            return false;
        }
        const auto& profile = note_splash_profiles[
            static_cast<std::size_t>(draw.profile - 1U)
        ];
        if (profile.texture_index == no_texture
            || profile.texture_index >= textures.size()) {
            return false;
        }
        auto& resource = textures[profile.texture_index];
        if (resource.texture == nullptr) {
            return false;
        }
        const auto atlas_index = profile.frame_indices[
            static_cast<std::size_t>(draw.frame_index) % frame_count
        ];
        if (atlas_index >= resource.atlas.size()) {
            return false;
        }
        const auto& frame = resource.atlas[atlas_index];
        if (frame.frame_width <= 0.0F || frame.frame_height <= 0.0F) {
            return false;
        }
        const float scale_x = draw.destination.width / frame.frame_width;
        const float scale_y = draw.destination.height / frame.frame_height;
        SDL_FRect destination{
            draw.destination.x + frame.content_offset_x * scale_x,
            draw.destination.y + frame.content_offset_y * scale_y,
            frame.source.w * scale_x,
            frame.source.h * scale_y,
        };
        if (!std::isfinite(destination.x) || !std::isfinite(destination.y)
            || !std::isfinite(destination.w) || !std::isfinite(destination.h)
            || destination.w <= 0.0F || destination.h <= 0.0F) {
            return false;
        }
        auto* texture = resource.texture.get();
        static_cast<void>(SDL_SetTextureColorMod(texture, 255U, 255U, 255U));
        static_cast<void>(SDL_SetTextureAlphaMod(texture, draw.alpha));
        static_cast<void>(SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR));
        return SDL_RenderTextureRotated(
            renderer,
            texture,
            &frame.source,
            &destination,
            std::isfinite(draw.angle) ? draw.angle : 0.0,
            nullptr,
            SDL_FLIP_NONE
        );
    }

    void override_note_skin(
        const std::string_view style,
        const bool force_pixel
    ) {
        if (!four_lane_chart || style.empty()) {
            return;
        }

        // Discard only the frame bindings. Texture resources already loaded for
        // the chart-default skin remain owned by the scene cache, while the
        // selected skin becomes authoritative for all subsequent note draws.
        note_skin_frames = {};
        load_note_skin(style, force_pixel);
    }

    void configure_animations(
        SceneSprite& sprite,
        const std::vector<AnimationDescriptor>& animations,
        const std::string_view requested
    ) {
        if (sprite.texture_index == no_texture
            || sprite.texture_index >= textures.size()) {
            return;
        }
        const auto& resource = textures[sprite.texture_index];
        const auto& atlas = resource.atlas;
        const auto& name_order = resource.atlas_name_order;
        const auto remaining_clips = limits.maximum_animation_clips - std::min(
            animation_clips,
            limits.maximum_animation_clips
        );
        sprite.animations.reserve(std::min(animations.size(), remaining_clips));
        const auto report_budget = [&]() {
            if (animation_budget_diagnosed) return;
            animation_budget_diagnosed = true;
            diagnose(
                RuntimeSceneDiagnosticSeverity::warning,
                "scene animation clip/frame-reference budget was exhausted"
            );
        };
        for (const auto& animation : animations) {
            if (animation.name.empty() || name_order.empty()) continue;
            if (animation_clips >= limits.maximum_animation_clips) {
                report_budget();
                break;
            }
            const auto lower = std::lower_bound(
                name_order.begin(),
                name_order.end(),
                std::string_view(animation.name),
                [&](const std::uint32_t index, const std::string_view value) {
                    return ascii_less_insensitive(atlas[index].name, value);
                }
            );
            std::string upper_prefix = animation.name;
            upper_prefix.push_back(static_cast<char>(0xFF));
            const auto upper = std::lower_bound(
                lower,
                name_order.end(),
                std::string_view(upper_prefix),
                [&](const std::uint32_t index, const std::string_view value) {
                    return ascii_less_insensitive(atlas[index].name, value);
                }
            );
            if (lower == upper
                || !starts_with_ascii_insensitive(
                    atlas[*lower].name,
                    animation.name
                )) {
                continue;
            }
            const auto remaining_references =
                limits.maximum_animation_frame_references - std::min(
                    animation_frame_references,
                    limits.maximum_animation_frame_references
                );
            if (remaining_references == 0U) {
                report_budget();
                break;
            }
            SceneSprite::AnimationClip clip;
            clip.id = animation.id;
            clip.fps = std::clamp<double>(animation.fps, 1.0, 1'000.0);
            clip.loop = animation.loop;
            clip.offset_x = finite_float(
                animation.offsets.x,
                0.0F,
                -100'000.0F,
                100'000.0F
            );
            clip.offset_y = finite_float(
                animation.offsets.y,
                0.0F,
                -100'000.0F,
                100'000.0F
            );
            const auto match_count = static_cast<std::size_t>(
                std::distance(lower, upper)
            );
            if (animation.indices.empty()) {
                const auto count = std::min(
                    match_count,
                    remaining_references
                );
                clip.frames.assign(
                    lower,
                    lower + static_cast<std::ptrdiff_t>(count)
                );
                if (count != match_count) report_budget();
            } else {
                clip.frames.reserve(std::min(
                    animation.indices.size(),
                    remaining_references
                ));
                for (const auto index : animation.indices) {
                    if (clip.frames.size() >= remaining_references) {
                        report_budget();
                        break;
                    }
                    if (index >= 0 && static_cast<std::size_t>(index)
                            < match_count) {
                        clip.frames.push_back(
                            *(lower + static_cast<std::ptrdiff_t>(index))
                        );
                    }
                }
            }
            if (!clip.frames.empty()) {
                animation_frame_references += clip.frames.size();
                ++animation_clips;
                sprite.animations.push_back(std::move(clip));
            }
        }
        const auto preferred = preferred_animation_descriptor(
            animations,
            requested
        );
        if (preferred == nullptr) return;
        const auto found = std::find_if(
            sprite.animations.begin(),
            sprite.animations.end(),
            [&](const SceneSprite::AnimationClip& clip) {
                return animation_id_equal(clip.id, preferred->id);
            }
        );
        if (found != sprite.animations.end()) {
            sprite.default_animation = static_cast<std::size_t>(
                std::distance(sprite.animations.begin(), found)
            );
        }
    }

    // PULSEFORGE_P1_1_3_EXACT_TAG_PRECEDENCE_V1
    [[nodiscard]] SceneSprite* find_script_sprite_exact(
        const std::string_view tag
    ) noexcept {
        if (tag.empty()) return nullptr;
        for (auto& sprite : sprites) {
            if (!sprite.tag.empty()
                && equals_ascii_insensitive(sprite.tag, tag)) {
                return std::addressof(sprite);
            }
        }
        return nullptr;
    }

    [[nodiscard]] SceneSprite* find_script_sprite(
        const std::string_view tag
    ) noexcept {
        if (auto* exact = find_script_sprite_exact(tag); exact != nullptr) {
            return exact;
        }
        if (tag.empty()) return nullptr;
        const auto role_match = [&](const SceneSprite& sprite) noexcept {
            if (sprite.role == SpriteRole::player) {
                return equals_ascii_insensitive(tag, "boyfriend")
                    || equals_ascii_insensitive(tag, "bf")
                    || equals_ascii_insensitive(tag, "boyfriendGroup");
            }
            if (sprite.role == SpriteRole::opponent) {
                return equals_ascii_insensitive(tag, "dad")
                    || equals_ascii_insensitive(tag, "opponent")
                    || equals_ascii_insensitive(tag, "dadGroup");
            }
            if (sprite.role == SpriteRole::secondary_opponent) {
                return equals_ascii_insensitive(tag, "player4")
                    || equals_ascii_insensitive(tag, "p4")
                    || equals_ascii_insensitive(tag, "secondaryOpponent")
                    || equals_ascii_insensitive(tag, "player4Group");
            }
            if (sprite.role == SpriteRole::girlfriend) {
                return equals_ascii_insensitive(tag, "gf")
                    || equals_ascii_insensitive(tag, "girlfriend")
                    || equals_ascii_insensitive(tag, "gfGroup");
            }
            return false;
        };
        for (auto& sprite : sprites) {
            if (role_match(sprite)) {
                return std::addressof(sprite);
            }
        }
        return nullptr;
    }

    [[nodiscard]] const SceneSprite* find_script_sprite(
        const std::string_view tag
    ) const noexcept {
        return const_cast<Implementation*>(this)->find_script_sprite(tag);
    }

    void normalize_sprite_order() {
        std::stable_sort(
            sprites.begin(),
            sprites.end(),
            [](const SceneSprite& left, const SceneSprite& right) {
                if (left.order != right.order) return left.order < right.order;
                return left.insertion_order < right.insertion_order;
            }
        );
    }

    void initialize_sprite_identity(
        SceneSprite& sprite,
        std::string tag = {}
    ) {
        sprite.tag = std::move(tag);
        sprite.order = static_cast<std::int64_t>(next_sprite_insertion_order);
        sprite.insertion_order = next_sprite_insertion_order++;
    }

    void add_stage_objects(
        const StageDescriptor& stage,
        const StageObjectLayer layer
    ) {
        for (const auto& object : stage.objects) {
            const auto effective_layer = object.layer == StageObjectLayer::background
                    && object.foreground
                ? StageObjectLayer::foreground
                : object.layer;
            if (effective_layer != layer) continue;
            if (sprites.size() >= limits.maximum_sprites) {
                diagnose(RuntimeSceneDiagnosticSeverity::warning, "stage sprite count exceeded its configured limit");
                return;
            }
            SceneSprite sprite;
            sprite.role = SpriteRole::stage;
            initialize_sprite_identity(
                sprite,
                !object.name.empty() ? object.name : object.image
            );
            sprite.x = finite_float(object.x, 0.0F, -100'000.0F, 100'000.0F);
            sprite.y = finite_float(object.y, 0.0F, -100'000.0F, 100'000.0F);
            sprite.scale_x = finite_float(object.scale.x, 1.0F, 0.001F, 128.0F);
            sprite.scale_y = finite_float(object.scale.y, 1.0F, 0.001F, 128.0F);
            sprite.alpha = finite_float(object.alpha, 1.0F, 0.0F, 1.0F);
            sprite.fallback_width = finite_float(
                object.width, 256.0F, 1.0F, 100'000.0F
            );
            sprite.fallback_height = finite_float(
                object.height, 160.0F, 1.0F, 100'000.0F
            );
            sprite.angle = std::isfinite(object.angle)
                ? std::clamp(object.angle, -360'000.0, 360'000.0)
                : 0.0;
            sprite.flip = make_flip(object.flip_x, object.flip_y);
            sprite.screen_space = object.screen_space;
            sprite.blend_mode = blend_mode(object.blend);
            sprite.scale_mode = object.antialiasing && !pixel_stage
                ? SDL_SCALEMODE_LINEAR
                : SDL_SCALEMODE_NEAREST;
            if (const auto color = parse_color(object.color); color.has_value()) {
                sprite.color = *color;
            }
            if (!object.image.empty()) {
                sprite.texture_index = load_texture(object.image);
                if (sprite.texture_index != no_texture) {
                    sprite.frame = select_atlas_frame(
                        textures[sprite.texture_index],
                        preferred_animation(object.animations, object.first_animation)
                    );
                    configure_animations(
                        sprite,
                        object.animations,
                        object.first_animation
                    );
                }
            }
            sprites.push_back(std::move(sprite));
        }
    }

    void add_character(
        const CharacterDescriptor& character,
        const DescriptorVec2 anchor,
        const SpriteRole role
    ) {
        if (sprites.size() >= limits.maximum_sprites) {
            diagnose(RuntimeSceneDiagnosticSeverity::warning, "character sprite count exceeded its configured limit");
            return;
        }
        SceneSprite sprite;
        sprite.role = role;
        initialize_sprite_identity(
            sprite,
            role == SpriteRole::player ? "boyfriend"
                : role == SpriteRole::opponent ? "dad"
                : role == SpriteRole::secondary_opponent ? "player4"
                : role == SpriteRole::girlfriend ? "gf"
                : "character"
        );
        sprite.sing_duration_steps = std::isfinite(character.sing_duration)
            ? std::clamp(character.sing_duration, 0.0, 64.0)
            : 4.0;
        sprite.x = finite_float(anchor.x + character.position.x, 0.0F, -100'000.0F, 100'000.0F);
        sprite.y = finite_float(anchor.y + character.position.y, 0.0F, -100'000.0F, 100'000.0F);
        sprite.camera_position_x = finite_float(
            character.camera_position.x, 0.0F, -100'000.0F, 100'000.0F
        );
        sprite.camera_position_y = finite_float(
            character.camera_position.y, 0.0F, -100'000.0F, 100'000.0F
        );
        const DescriptorVec2 stage_camera_offset =
            role == SpriteRole::player ? stage_camera_boyfriend
            : role == SpriteRole::secondary_opponent ? stage_camera_p4
            : role == SpriteRole::opponent ? stage_camera_opponent
            : role == SpriteRole::girlfriend ? stage_camera_girlfriend
            : DescriptorVec2{};
        sprite.stage_camera_offset_x = finite_float(
            stage_camera_offset.x, 0.0F, -100'000.0F, 100'000.0F
        );
        sprite.stage_camera_offset_y = finite_float(
            stage_camera_offset.y, 0.0F, -100'000.0F, 100'000.0F
        );
        const float scale = finite_float(character.scale, 1.0F, 0.001F, 128.0F);
        sprite.scale_x = scale;
        sprite.scale_y = scale;
        // PULSEFORGE_P1_1_7_1_PLAYER_FACING_V1
        sprite.flip = make_flip(
            psych_character_effective_flip_x(
                character.flip_x,
                role == SpriteRole::player
            ),
            false
        );
        sprite.scale_mode = character.no_antialiasing || pixel_stage
            ? SDL_SCALEMODE_NEAREST
            : SDL_SCALEMODE_LINEAR;
        sprite.fallback_width = role == SpriteRole::girlfriend ? 150.0F : 180.0F;
        sprite.fallback_height = role == SpriteRole::girlfriend ? 260.0F : 300.0F;
        sprite.color = role == SpriteRole::player
            ? SDL_Color{66, 204, 255, 255}
            : role == SpriteRole::opponent
                ? SDL_Color{255, 92, 122, 255}
                : role == SpriteRole::secondary_opponent
                    ? SDL_Color{255, 188, 72, 255}
                    : SDL_Color{255, 112, 210, 255};
        sprite.texture_index = load_texture(character.image);
        if (sprite.texture_index != no_texture) {
            sprite.frame = select_atlas_frame(
                textures[sprite.texture_index],
                preferred_animation(character.animations, "idle")
            );
            configure_animations(sprite, character.animations, "idle");
        }
        sprites.push_back(std::move(sprite));
    }

    [[nodiscard]] DescriptorVec2 character_anchor(
        const SpriteRole role
    ) const noexcept {
        switch (role) {
        case SpriteRole::player: return player_anchor;
        case SpriteRole::opponent: return opponent_anchor;
        case SpriteRole::secondary_opponent: return secondary_opponent_anchor;
        case SpriteRole::girlfriend: return girlfriend_anchor;
        case SpriteRole::stage: return {};
        }
        return {};
    }

    [[nodiscard]] bool replace_character(
        const SpriteRole role,
        const std::string_view requested_id
    ) {
        // PULSEFORGE_P1_1_5_DYNAMIC_CHANGE_CHARACTER_V1
        auto character = load_character_for_change(requested_id);
        if (!character.has_value()) return false;

        auto current = std::find_if(
            sprites.begin(),
            sprites.end(),
            [role](const SceneSprite& sprite) { return sprite.role == role; }
        );
        if (current == sprites.end()) {
            diagnose(
                RuntimeSceneDiagnosticSeverity::warning,
                "Change Character could not find the requested role in the scene"
            );
            return false;
        }

        const auto texture_index = load_texture(character->image);
        if (texture_index == no_texture || texture_index >= textures.size()) {
            diagnose(
                RuntimeSceneDiagnosticSeverity::warning,
                std::string{"Change Character image could not be loaded: "}
                    + character->image
            );
            // Do not destroy a valid actor and replace it with a fallback.
            return false;
        }

        SceneSprite replacement = *current;
        replacement.role = role;

        if (animation_clips >= replacement.animations.size())
            animation_clips -= replacement.animations.size();
        else
            animation_clips = 0U;
        std::size_t old_references = 0U;
        for (const auto& clip : replacement.animations)
            old_references += clip.frames.size();
        if (animation_frame_references >= old_references)
            animation_frame_references -= old_references;
        else
            animation_frame_references = 0U;

        replacement.animations.clear();
        replacement.default_animation = no_texture;
        replacement.transient_animation = no_texture;
        replacement.transient_started_ms = 0.0;
        replacement.transient_until_ms = 0.0;
        replacement.sustain_animation_tails.fill(0.0);
        replacement.sustain_animation_until_ms = 0.0;
        replacement.frame.reset();

        replacement.camera_position_x = finite_float(
            character->camera_position.x, 0.0F, -100'000.0F, 100'000.0F
        );
        replacement.camera_position_y = finite_float(
            character->camera_position.y, 0.0F, -100'000.0F, 100'000.0F
        );
        replacement.sing_duration_steps = std::isfinite(character->sing_duration)
            ? std::clamp(character->sing_duration, 0.0, 64.0)
            : 4.0;
        const auto anchor = character_anchor(role);
        replacement.x = finite_float(
            anchor.x + character->position.x, 0.0F, -100'000.0F, 100'000.0F
        );
        replacement.y = finite_float(
            anchor.y + character->position.y, 0.0F, -100'000.0F, 100'000.0F
        );
        const float scale = finite_float(character->scale, 1.0F, 0.001F, 128.0F);
        replacement.scale_x = scale;
        replacement.scale_y = scale;
        // PULSEFORGE_P1_1_7_1_PLAYER_FACING_V1
        replacement.flip = make_flip(
            psych_character_effective_flip_x(
                character->flip_x,
                role == SpriteRole::player
            ),
            false
        );
        replacement.scale_mode = character->no_antialiasing || pixel_stage
            ? SDL_SCALEMODE_NEAREST
            : SDL_SCALEMODE_LINEAR;
        replacement.fallback_width =
            role == SpriteRole::girlfriend ? 150.0F : 180.0F;
        replacement.fallback_height =
            role == SpriteRole::girlfriend ? 260.0F : 300.0F;
        replacement.color = role == SpriteRole::player
            ? SDL_Color{66, 204, 255, 255}
            : role == SpriteRole::opponent
                ? SDL_Color{255, 92, 122, 255}
                : role == SpriteRole::secondary_opponent
                    ? SDL_Color{255, 188, 72, 255}
                    : SDL_Color{255, 112, 210, 255};

        replacement.texture_index = texture_index;
        replacement.frame = select_atlas_frame(
            textures[texture_index],
            preferred_animation(character->animations, "idle")
        );
        configure_animations(replacement, character->animations, "idle");
        *current = std::move(replacement);
        return true;
    }


    void load(const Chart& chart) {
        four_lane_chart = chart.key_count
            == static_cast<std::uint16_t>(note_skin_lane_count);

        if (limits.note_skin_only) {
            // Maximum-performance mode deliberately keeps only the selected
            // note texture pipeline. Avoid stage/character descriptors,
            // sprites, animation clips and all corresponding draw work.
            pixel_stage = equals_ascii_insensitive(chart.note_style, "pixel")
                || equals_ascii_insensitive(chart.note_style, "pixelUI");
            load_note_skin(chart.note_style, pixel_stage);
            return;
        }

        const StageDescriptor stage = load_stage(chart.stage_id);
        stage_zoom = finite_float(stage.default_zoom, 0.9F, 0.05F, 8.0F);
        pixel_stage = stage.pixel_stage;
        girlfriend_anchor = stage.girlfriend;
        opponent_anchor = stage.opponent;
        secondary_opponent_anchor = stage.p4;
        player_anchor = stage.boyfriend;
        stage_camera_boyfriend = stage.camera_boyfriend;
        stage_camera_girlfriend = stage.camera_girlfriend;
        stage_camera_opponent = stage.camera_opponent;
        stage_camera_p4 = stage.camera_p4;
        load_note_skin(chart.note_style, pixel_stage);
        add_stage_objects(stage, StageObjectLayer::background);
        add_stage_objects(stage, StageObjectLayer::behind_girlfriend);

        if (!stage.hide_girlfriend) {
            const auto girlfriend = load_character(
                chart.girlfriend_character,
                "gf"
            );
            add_character(girlfriend, stage.girlfriend, SpriteRole::girlfriend);
        }
        add_stage_objects(stage, StageObjectLayer::behind_opponent);
        const auto opponent = load_character(chart.opponent_character, "dad");
        add_character(opponent, stage.opponent, SpriteRole::opponent);
        if (chart.secondary_opponent_enabled) {
            // PULSEFORGE_P1_4_0_SECONDARY_OPPONENT_CHARACTER_V1
            // DenpaEx player4 is a separate actor. Start from the opponent
            // anchor and retain the character descriptor's own position/camera
            // offsets rather than merging it into the primary dad sprite.
            const auto secondary = load_character(
                chart.secondary_opponent_character,
                chart.opponent_character.empty() ? "dad" : chart.opponent_character
            );
            add_character(
                secondary,
                stage.p4,
                SpriteRole::secondary_opponent
            );
        }
        add_stage_objects(stage, StageObjectLayer::behind_player);
        const auto player = load_character(chart.player_character, "bf");
        add_character(player, stage.boyfriend, SpriteRole::player);
        add_stage_objects(stage, StageObjectLayer::foreground);
    }

    [[nodiscard]] static SpriteRole note_animation_role(
        const NoteOwner owner,
        const NoteAnimationTarget target
    ) noexcept {
        switch (target) {
        case NoteAnimationTarget::player:
            return SpriteRole::player;
        case NoteAnimationTarget::opponent:
            return SpriteRole::opponent;
        case NoteAnimationTarget::girlfriend:
            return SpriteRole::girlfriend;
        case NoteAnimationTarget::owner:
            break;
        }
        if (owner == NoteOwner::player) return SpriteRole::player;
        if (owner == NoteOwner::secondary_opponent) {
            return SpriteRole::secondary_opponent;
        }
        return SpriteRole::opponent;
    }

    [[nodiscard]] static SpriteRole note_animation_role(
        const NoteOwner owner,
        const std::string_view note_kind
    ) noexcept {
        const bool girlfriend = animation_id_equal(note_kind, "GF Sing")
            || animation_id_equal(note_kind, "GF Cross Fade");
        return note_animation_role(
            owner,
            girlfriend ? NoteAnimationTarget::girlfriend
                       : NoteAnimationTarget::owner
        );
    }

    static void prune_sustain_animation_holds(
        SceneSprite& sprite,
        const double song_time_ms
    ) noexcept {
        double latest = 0.0;
        for (auto& tail : sprite.sustain_animation_tails) {
            if (!std::isfinite(tail) || tail <= song_time_ms + 0.001) {
                tail = 0.0;
            } else {
                latest = std::max(latest, tail);
            }
        }
        sprite.sustain_animation_until_ms = latest;
    }

    static void add_sustain_animation_hold(
        SceneSprite& sprite,
        const double song_time_ms,
        const double sustain_tail_ms
    ) noexcept {
        if (!std::isfinite(sustain_tail_ms)
            || sustain_tail_ms <= song_time_ms + 0.001) {
            return;
        }
        prune_sustain_animation_holds(sprite, song_time_ms);
        for (auto& tail : sprite.sustain_animation_tails) {
            if (tail == 0.0) {
                tail = sustain_tail_ms;
                sprite.sustain_animation_until_ms = std::max(
                    sprite.sustain_animation_until_ms,
                    sustain_tail_ms
                );
                return;
            }
        }
        // Degenerate charts can overlap more sustains than the bounded visual
        // tracker can represent. Preserve the longest holds so we never return
        // to idle before the latest visible tail merely because the tracker is
        // saturated.
        auto earliest = std::min_element(
            sprite.sustain_animation_tails.begin(),
            sprite.sustain_animation_tails.end()
        );
        if (earliest != sprite.sustain_animation_tails.end()
            && *earliest < sustain_tail_ms) {
            *earliest = sustain_tail_ms;
        }
        sprite.sustain_animation_until_ms = *std::max_element(
            sprite.sustain_animation_tails.begin(),
            sprite.sustain_animation_tails.end()
        );
    }

    static void remove_sustain_animation_hold(
        SceneSprite& sprite,
        const double song_time_ms,
        const double sustain_tail_ms
    ) noexcept {
        prune_sustain_animation_holds(sprite, song_time_ms);
        if (!std::isfinite(sustain_tail_ms)) return;
        auto closest = sprite.sustain_animation_tails.end();
        double closest_distance = std::numeric_limits<double>::infinity();
        for (auto it = sprite.sustain_animation_tails.begin();
             it != sprite.sustain_animation_tails.end(); ++it) {
            if (*it <= 0.0) continue;
            const double distance = std::abs(*it - sustain_tail_ms);
            if (distance < closest_distance) {
                closest = it;
                closest_distance = distance;
            }
        }
        // Tail timestamps are derived from the same chart note in the caller;
        // the epsilon merely absorbs integer-us -> double-ms conversion.
        if (closest != sprite.sustain_animation_tails.end()
            && closest_distance <= 0.01) {
            *closest = 0.0;
        }
        prune_sustain_animation_holds(sprite, song_time_ms);
    }

    void notify_note_animation(
        const NoteOwner owner,
        const std::uint16_t lane,
        const double song_time_ms,
        const std::string_view note_kind,
        const bool missed,
        const double sustain_tail_ms
    ) noexcept {
        if (animation_id_equal(note_kind, "No Animation")) return;
        const auto target = animation_id_equal(note_kind, "GF Sing")
                || animation_id_equal(note_kind, "GF Cross Fade")
            ? NoteAnimationTarget::girlfriend
            : NoteAnimationTarget::owner;
        const auto cue = animation_id_equal(note_kind, "Hey!")
            ? NoteAnimationCue::hey
            : NoteAnimationCue::sing;
        const std::string_view suffix = animation_id_equal(
            note_kind, "Alt Animation"
        ) ? std::string_view{"-alt"} : std::string_view{};
        notify_note_animation_configured(
            owner,
            lane,
            song_time_ms,
            target,
            cue,
            suffix,
            missed,
            sustain_tail_ms
        );
    }

    void notify_note_animation_configured(
        const NoteOwner owner,
        const std::uint16_t lane,
        const double song_time_ms,
        const NoteAnimationTarget target,
        const NoteAnimationCue cue,
        const std::string_view suffix,
        const bool missed,
        const double sustain_tail_ms
    ) noexcept {
        if (!std::isfinite(song_time_ms) || cue == NoteAnimationCue::none) return;
        constexpr std::array directions{
            std::string_view{"singLEFT"},
            std::string_view{"singDOWN"},
            std::string_view{"singUP"},
            std::string_view{"singRIGHT"},
        };
        constexpr std::array miss_directions{
            std::string_view{"singLEFTmiss"},
            std::string_view{"singDOWNmiss"},
            std::string_view{"singUPmiss"},
            std::string_view{"singRIGHTmiss"},
        };
        const auto direction_index = static_cast<std::size_t>(lane)
            % directions.size();
        const auto base = directions[direction_index];
        // PULSEFORGE_P1_5_0C_ALLOCATION_FREE_DECLARATIVE_ANIMATION_NAME_V1
        // The parser already bounds suffixes to 64 bytes. Keep the public API
        // bounded too, and never allocate in the note-animation hot path.
        std::array<char, 96U> requested_storage{};
        std::string_view requested;
        if (cue == NoteAnimationCue::hey) {
            requested = "hey";
        } else if (missed) {
            requested = miss_directions[direction_index];
        } else if (!suffix.empty()
            && base.size() + suffix.size() <= requested_storage.size()) {
            auto out = std::copy(base.begin(), base.end(), requested_storage.begin());
            out = std::copy(suffix.begin(), suffix.end(), out);
            requested = std::string_view{
                requested_storage.data(), base.size() + suffix.size()
            };
        } else {
            requested = base;
        }
        const auto role = note_animation_role(owner, target);
        for (auto& sprite : sprites) {
            if (sprite.role != role) continue;
            auto found = std::find_if(
                sprite.animations.begin(),
                sprite.animations.end(),
                [&](const SceneSprite::AnimationClip& animation) {
                    return animation_id_equal(animation.id, requested);
                }
            );
            if (found == sprite.animations.end() && missed) {
                found = std::find_if(
                    sprite.animations.begin(),
                    sprite.animations.end(),
                    [&](const SceneSprite::AnimationClip& animation) {
                        return animation_id_equal(animation.id, base);
                    }
                );
            }
            if (found == sprite.animations.end()) continue;
            const auto index = static_cast<std::size_t>(
                std::distance(sprite.animations.begin(), found)
            );
            if (song_time_ms + 0.001 < sprite.transient_started_ms) {
                // PULSEFORGE_P1_4_0_CHARACTER_SUSTAIN_SEEK_RESET_V1
                sprite.sustain_animation_tails.fill(0.0);
                sprite.sustain_animation_until_ms = 0.0;
                sprite.transient_until_ms = 0.0;
            }
            const double bpm = timing.bpm_at(song_time_ms);
            // Psych's singDuration is measured in crochet steps. One step is
            // one quarter of a beat, so tempo changes affect only the return
            // to idle, never audio playback.
            const double sing_duration = bpm > 0.0 && std::isfinite(bpm)
                ? sprite.sing_duration_steps * 15'000.0 / bpm
                : 500.0;
            sprite.transient_animation = index;
            sprite.transient_started_ms = song_time_ms;
            const bool starts_sustain_hold = !missed
                && std::isfinite(sustain_tail_ms)
                && sustain_tail_ms > song_time_ms + 0.001;
            if (starts_sustain_hold) {
                // PULSEFORGE_P1_4_0_CHARACTER_SUSTAIN_ANIMATION_HOLD_V2
                add_sustain_animation_hold(
                    sprite, song_time_ms, sustain_tail_ms
                );
            } else {
                prune_sustain_animation_holds(sprite, song_time_ms);
            }
            const double ordinary_sing_end = song_time_ms
                + std::clamp(sing_duration, 16.0, 8'000.0);
            // PULSEFORGE_P1_4_0_CHARACTER_SUSTAIN_TAIL_EXACT_V1
            // A sustain head owns the singing pose until the active sustain
            // tail set ends, not until an unrelated BPM-based singDuration.
            sprite.transient_until_ms = starts_sustain_hold
                ? sprite.sustain_animation_until_ms
                : std::max(ordinary_sing_end, sprite.sustain_animation_until_ms);
        }
    }


    // PULSEFORGE_P1_1_14_CPU_COLOR_TEXTURE_V1
    // SDL_Renderer has no colour-matrix/cross-channel operation. For bounded
    // textures, adjustColor/grayscale/HSV are therefore evaluated exactly on
    // CPU RGBA and uploaded to a per-sprite streaming texture. Source pixels
    // are decoded lazily once per TextureResource and variants are reused.
    [[nodiscard]] bool ensure_cpu_shader_source(
        TextureResource& resource
    ) {
        if (!resource.cpu_color_source_rgba.empty()) {
            return true;
        }
        if (resource.cpu_color_source_failed || files == nullptr
            || resource.source_virtual_path.empty()
            || resource.source_mount_id.empty()
            || resource.width <= 0.0F || resource.height <= 0.0F) {
            return false;
        }

        const auto width = static_cast<std::uint64_t>(resource.width);
        const auto height = static_cast<std::uint64_t>(resource.height);
        if (width == 0U || height == 0U
            || width > std::numeric_limits<std::uint64_t>::max() / height) {
            resource.cpu_color_source_failed = true;
            return false;
        }
        const std::uint64_t pixels = width * height;
        if (pixels > psych_cpu_color_max_pixels_per_texture
            || pixels > std::numeric_limits<std::uint64_t>::max() / 4U) {
            resource.cpu_color_source_failed = true;
            return false;
        }
        const std::uint64_t bytes = pixels * 4U;
        if (bytes > psych_cpu_color_max_source_bytes
            || cpu_color_source_bytes
                > psych_cpu_color_max_source_bytes - bytes) {
            resource.cpu_color_source_failed = true;
            return false;
        }

        BinaryVirtualFile encoded;
        try {
            if (!files->resolve(resource.source_virtual_path).has_value()) {
                resource.cpu_color_source_failed = true;
                return false;
            }
            encoded = files->read_binary(
                resource.source_virtual_path,
                limits.maximum_image_bytes
            );
            if (encoded.source.provenance.mount_id
                != resource.source_mount_id) {
                resource.cpu_color_source_failed = true;
                return false;
            }
        } catch (...) {
            resource.cpu_color_source_failed = true;
            return false;
        }

        if (encoded.bytes.size() > static_cast<std::size_t>(
                std::numeric_limits<int>::max()
            )) {
            resource.cpu_color_source_failed = true;
            return false;
        }

        int decoded_width{};
        int decoded_height{};
        int components{};
        auto* pixels_rgba = stbi_load_from_memory(
            reinterpret_cast<const stbi_uc*>(encoded.bytes.data()),
            static_cast<int>(encoded.bytes.size()),
            &decoded_width,
            &decoded_height,
            &components,
            STBI_rgb_alpha
        );
        if (pixels_rgba == nullptr
            || decoded_width != static_cast<int>(width)
            || decoded_height != static_cast<int>(height)) {
            stbi_image_free(pixels_rgba);
            resource.cpu_color_source_failed = true;
            return false;
        }

        try {
            resource.cpu_color_source_rgba.assign(
                pixels_rgba,
                pixels_rgba + static_cast<std::ptrdiff_t>(bytes)
            );
        } catch (...) {
            stbi_image_free(pixels_rgba);
            resource.cpu_color_source_failed = true;
            return false;
        }
        stbi_image_free(pixels_rgba);
        cpu_color_source_bytes += bytes;
        return true;
    }

    [[nodiscard]] SDL_Texture* resolve_cpu_shader_texture(
        const SceneSprite& sprite,
        const PsychShaderCompatKind shader_kind,
        const PsychShaderCompatState& shader_state
    ) noexcept {
        if (!psych_shader_uses_cpu_texture_transform(shader_kind)
            || psych_cpu_shader_neutral(shader_kind, shader_state)
            || sprite.texture_index == no_texture
            || sprite.texture_index >= textures.size()
            || renderer == nullptr) {
            return nullptr;
        }

        try {
            auto& resource = textures[sprite.texture_index];
            const auto width = static_cast<std::uint64_t>(resource.width);
            const auto height = static_cast<std::uint64_t>(resource.height);
            if (width == 0U || height == 0U
                || width > std::numeric_limits<std::uint64_t>::max() / height) {
                return nullptr;
            }
            const std::uint64_t pixels = width * height;
            const std::uint64_t texture_pixel_limit =
                shader_kind == PsychShaderCompatKind::gaussian_blur
                ? psych_cpu_blur_max_pixels_per_texture
                : psych_cpu_color_max_pixels_per_texture;
            if (pixels > texture_pixel_limit
                || pixels > std::numeric_limits<std::uint64_t>::max() / 4U) {
                return nullptr;
            }
            const std::uint64_t decoded_bytes = pixels * 4U;
            const auto key = psych_cpu_shader_key(
                shader_kind,
                shader_state
            );

            auto found = std::find_if(
                cpu_color_variants.begin(),
                cpu_color_variants.end(),
                [&](const CpuShaderVariant& variant) {
                    return variant.sprite_insertion_order
                        == sprite.insertion_order;
                }
            );

            if (found != cpu_color_variants.end()
                && found->texture_index == sprite.texture_index
                && found->key == key && found->texture != nullptr) {
                return found->texture.get();
            }

            const std::uint64_t frame_pixel_limit =
                shader_kind == PsychShaderCompatKind::gaussian_blur
                ? psych_cpu_blur_max_pixels_per_frame
                : psych_cpu_color_max_pixels_per_frame;
            if (pixels > frame_pixel_limit
                || cpu_color_pixels_this_frame > frame_pixel_limit - pixels) {
                return nullptr;
            }
            if (!ensure_cpu_shader_source(resource)) {
                return nullptr;
            }

            if (found == cpu_color_variants.end()) {
                if (cpu_color_variants.size()
                        >= psych_cpu_color_max_variants
                    || decoded_bytes > psych_cpu_color_max_variant_bytes
                    || cpu_color_variant_bytes
                        > psych_cpu_color_max_variant_bytes - decoded_bytes) {
                    return nullptr;
                }
                CpuShaderVariant variant;
                variant.sprite_insertion_order = sprite.insertion_order;
                cpu_color_variants.push_back(std::move(variant));
                found = cpu_color_variants.end() - 1;
            }

            const bool recreate = found->texture == nullptr
                || found->texture_index != sprite.texture_index
                || found->decoded_bytes != decoded_bytes;
            if (recreate) {
                const std::uint64_t previous_bytes = found->decoded_bytes;
                const std::uint64_t base_bytes = cpu_color_variant_bytes
                    >= previous_bytes
                    ? cpu_color_variant_bytes - previous_bytes
                    : 0U;
                if (decoded_bytes > psych_cpu_color_max_variant_bytes
                    || base_bytes
                        > psych_cpu_color_max_variant_bytes - decoded_bytes) {
                    return nullptr;
                }

                SDL_Texture* raw = SDL_CreateTexture(
                    renderer,
                    SDL_PIXELFORMAT_RGBA32,
                    SDL_TEXTUREACCESS_STREAMING,
                    static_cast<int>(width),
                    static_cast<int>(height)
                );
                if (raw == nullptr) {
                    return nullptr;
                }
                TexturePointer replacement(raw);
                static_cast<void>(SDL_SetTextureBlendMode(
                    replacement.get(),
                    SDL_BLENDMODE_BLEND
                ));
                found->texture = std::move(replacement);
                found->texture_index = sprite.texture_index;
                found->decoded_bytes = decoded_bytes;
                cpu_color_variant_bytes = base_bytes + decoded_bytes;
            }

            cpu_color_scratch.resize(static_cast<std::size_t>(decoded_bytes));
            const auto& source_rgba = resource.cpu_color_source_rgba;
            if (source_rgba.size()
                != static_cast<std::size_t>(decoded_bytes)) {
                return nullptr;
            }

            // PULSEFORGE_P1_1_15_CPU_SHADER_TRANSFORMS_V1
            if (psych_shader_uses_cpu_color_transform(shader_kind)) {
                for (std::size_t offset = 0U;
                     offset < source_rgba.size();
                     offset += 4U) {
                    const PsychShaderRgb input{
                        static_cast<float>(source_rgba[offset]) / 255.0F,
                        static_cast<float>(source_rgba[offset + 1U]) / 255.0F,
                        static_cast<float>(source_rgba[offset + 2U]) / 255.0F,
                    };
                    const auto output = psych_cpu_color_shader_rgb(
                        shader_kind,
                        shader_state,
                        input
                    );
                    cpu_color_scratch[offset] = static_cast<std::uint8_t>(
                        std::clamp(std::lround(output.red * 255.0F), 0L, 255L)
                    );
                    cpu_color_scratch[offset + 1U] = static_cast<std::uint8_t>(
                        std::clamp(std::lround(output.green * 255.0F), 0L, 255L)
                    );
                    cpu_color_scratch[offset + 2U] = static_cast<std::uint8_t>(
                        std::clamp(std::lround(output.blue * 255.0F), 0L, 255L)
                    );
                    cpu_color_scratch[offset + 3U] = source_rgba[offset + 3U];
                }
            } else if (shader_kind == PsychShaderCompatKind::mosaic) {
                const std::size_t block_x = static_cast<std::size_t>(
                    std::clamp(
                        std::lround(shader_state.mosaic_block_x),
                        1L,
                        static_cast<long>(width)
                    )
                );
                const std::size_t block_y = static_cast<std::size_t>(
                    std::clamp(
                        std::lround(shader_state.mosaic_block_y),
                        1L,
                        static_cast<long>(height)
                    )
                );
                const std::size_t row_bytes = static_cast<std::size_t>(width) * 4U;
                for (std::size_t y = 0U; y < static_cast<std::size_t>(height); ++y) {
                    const std::size_t sample_y = (y / block_y) * block_y;
                    for (std::size_t x = 0U; x < static_cast<std::size_t>(width); ++x) {
                        const std::size_t sample_x = (x / block_x) * block_x;
                        const std::size_t dst = y * row_bytes + x * 4U;
                        const std::size_t sample =
                            sample_y * row_bytes + sample_x * 4U;
                        cpu_color_scratch[dst] = source_rgba[sample];
                        cpu_color_scratch[dst + 1U] = source_rgba[sample + 1U];
                        cpu_color_scratch[dst + 2U] = source_rgba[sample + 2U];
                        cpu_color_scratch[dst + 3U] = source_rgba[sample + 3U];
                    }
                }
            } else if (shader_kind == PsychShaderCompatKind::gaussian_blur) {
                const float amount = shader_state.gaussian_blur_amount;
                constexpr std::array<float, 4U> weights{
                    0.1964825501511404F,
                    0.2969069646728344F,
                    0.09447039785044732F,
                    0.010381362401148057F,
                };
                constexpr std::array<float, 3U> offsets{
                    1.411764705882353F,
                    3.2941176470588234F,
                    5.176470588235294F,
                };
                const float direction = amount * 2.0F;
                const int image_width = static_cast<int>(width);
                const int image_height = static_cast<int>(height);
                const auto sample_channel = [&](
                    const float x,
                    const float y,
                    const std::size_t channel
                ) noexcept {
                    const float clamped_x = std::clamp(
                        x, 0.0F, static_cast<float>(image_width - 1)
                    );
                    const float clamped_y = std::clamp(
                        y, 0.0F, static_cast<float>(image_height - 1)
                    );
                    const int x0 = static_cast<int>(std::floor(clamped_x));
                    const int y0 = static_cast<int>(std::floor(clamped_y));
                    const int x1 = std::min(x0 + 1, image_width - 1);
                    const int y1 = std::min(y0 + 1, image_height - 1);
                    const float tx = clamped_x - static_cast<float>(x0);
                    const float ty = clamped_y - static_cast<float>(y0);
                    const auto read = [&](const int px, const int py) noexcept {
                        const auto index =
                            (static_cast<std::size_t>(py)
                                * static_cast<std::size_t>(image_width)
                             + static_cast<std::size_t>(px)) * 4U + channel;
                        return static_cast<float>(source_rgba[index]);
                    };
                    const float top = std::lerp(read(x0, y0), read(x1, y0), tx);
                    const float bottom = std::lerp(read(x0, y1), read(x1, y1), tx);
                    return std::lerp(top, bottom, ty);
                };

                for (int y = 0; y < image_height; ++y) {
                    for (int x = 0; x < image_width; ++x) {
                        const std::size_t dst =
                            (static_cast<std::size_t>(y)
                                * static_cast<std::size_t>(image_width)
                             + static_cast<std::size_t>(x)) * 4U;
                        for (std::size_t channel = 0U; channel < 4U; ++channel) {
                            float horizontal =
                                sample_channel(
                                    static_cast<float>(x),
                                    static_cast<float>(y),
                                    channel
                                ) * weights[0];
                            float vertical = horizontal;
                            for (std::size_t tap = 0U; tap < offsets.size(); ++tap) {
                                const float delta = offsets[tap] * direction;
                                const float weight = weights[tap + 1U];
                                horizontal += (
                                    sample_channel(
                                        static_cast<float>(x) + delta,
                                        static_cast<float>(y),
                                        channel
                                    )
                                    + sample_channel(
                                        static_cast<float>(x) - delta,
                                        static_cast<float>(y),
                                        channel
                                    )
                                ) * weight;
                                vertical += (
                                    sample_channel(
                                        static_cast<float>(x),
                                        static_cast<float>(y) + delta,
                                        channel
                                    )
                                    + sample_channel(
                                        static_cast<float>(x),
                                        static_cast<float>(y) - delta,
                                        channel
                                    )
                                ) * weight;
                            }
                            cpu_color_scratch[dst + channel] =
                                static_cast<std::uint8_t>(std::clamp(
                                    std::lround((horizontal + vertical) * 0.5F),
                                    0L,
                                    255L
                                ));
                        }
                    }
                }
            } else {
                return nullptr;
            }

            if (!SDL_UpdateTexture(
                    found->texture.get(),
                    nullptr,
                    cpu_color_scratch.data(),
                    static_cast<int>(width * 4U)
                )) {
                return nullptr;
            }
            found->key = key;
            cpu_color_pixels_this_frame += pixels;
            return found->texture.get();
        } catch (...) {
            return nullptr;
        }
    }

    // PULSEFORGE_P1_1_11_WAVY_EFFECT_RENDER_V1
    // Draws a bounded horizontal-strip mesh. This stays on SDL_Renderer and
    // therefore works on the current backend without pretending to execute an
    // arbitrary GLSL shader. The fixed arrays guarantee no per-frame heap work.
    [[nodiscard]] bool render_wavy_sprite(
        const SceneSprite& sprite,
        SDL_Texture* texture,
        const SDL_FRect* const source,
        const SDL_FRect& destination,
        const double song_time_ms,
        const SDL_Color draw_color,
        const float draw_alpha,
        const double total_angle
    ) noexcept {
        const auto segment_count = psych_wavy_segment_count(
            sprite.wavy_effect,
            destination.h
        );
        if (segment_count == 0U
            || segment_count > psych_wavy_max_segments) {
            return false;
        }

        constexpr std::size_t maximum_vertices =
            (psych_wavy_max_segments + 1U) * 2U;
        constexpr std::size_t maximum_indices =
            psych_wavy_max_segments * 6U;
        std::array<SDL_Vertex, maximum_vertices> vertices{};
        std::array<int, maximum_indices> indices{};

        float u0 = 0.0F;
        float u1 = 1.0F;
        float v0 = 0.0F;
        float v1 = 1.0F;
        if (texture != nullptr) {
            if (sprite.texture_index == no_texture
                || sprite.texture_index >= textures.size()) {
                return false;
            }
            const auto& resource = textures[sprite.texture_index];
            if (resource.width <= 0.0F || resource.height <= 0.0F) {
                return false;
            }
            if (source != nullptr) {
                u0 = source->x / resource.width;
                u1 = (source->x + source->w) / resource.width;
                v0 = source->y / resource.height;
                v1 = (source->y + source->h) / resource.height;
            }
        }

        const auto flip_bits = static_cast<unsigned int>(sprite.flip);
        if ((flip_bits & static_cast<unsigned int>(SDL_FLIP_HORIZONTAL)) != 0U) {
            std::swap(u0, u1);
        }
        if ((flip_bits & static_cast<unsigned int>(SDL_FLIP_VERTICAL)) != 0U) {
            std::swap(v0, v1);
        }

        const float alpha_scale = texture != nullptr
            ? 1.0F
            : 190.0F / 255.0F;
        const float alpha = std::clamp(
            draw_alpha * alpha_scale,
            0.0F,
            1.0F
        );
        const SDL_FColor vertex_color{
            static_cast<float>(draw_color.r) / 255.0F,
            static_cast<float>(draw_color.g) / 255.0F,
            static_cast<float>(draw_color.b) / 255.0F,
            alpha,
        };
        const float center_x = destination.x + destination.w * 0.5F;
        const float center_y = destination.y + destination.h * 0.5F;
        constexpr double degrees_to_radians =
            0.017453292519943295769236907684886;
        const double bounded_angle = std::remainder(total_angle, 360.0);
        const double radians = bounded_angle * degrees_to_radians;
        const float cosine = static_cast<float>(std::cos(radians));
        const float sine = static_cast<float>(std::sin(radians));
        const auto rotate = [&](const float x, const float y) noexcept {
            const float local_x = x - center_x;
            const float local_y = y - center_y;
            return SDL_FPoint{
                center_x + local_x * cosine - local_y * sine,
                center_y + local_x * sine + local_y * cosine,
            };
        };

        for (std::size_t row = 0U; row <= segment_count; ++row) {
            const float normalized_y = static_cast<float>(row)
                / static_cast<float>(segment_count);
            const float wave_x = psych_wavy_offset(
                sprite.wavy_effect,
                normalized_y,
                song_time_ms,
                destination.w
            );
            const float y = destination.y + destination.h * normalized_y;
            const float texture_v = std::lerp(v0, v1, normalized_y);
            const auto left = rotate(destination.x + wave_x, y);
            const auto right = rotate(
                destination.x + destination.w + wave_x,
                y
            );
            const std::size_t base = row * 2U;
            vertices[base] = SDL_Vertex{
                left,
                vertex_color,
                SDL_FPoint{u0, texture_v},
            };
            vertices[base + 1U] = SDL_Vertex{
                right,
                vertex_color,
                SDL_FPoint{u1, texture_v},
            };
        }

        for (std::size_t segment = 0U; segment < segment_count; ++segment) {
            const int top_left = static_cast<int>(segment * 2U);
            const int top_right = top_left + 1;
            const int bottom_left = top_left + 2;
            const int bottom_right = top_left + 3;
            const std::size_t base = segment * 6U;
            indices[base] = top_left;
            indices[base + 1U] = top_right;
            indices[base + 2U] = bottom_right;
            indices[base + 3U] = top_left;
            indices[base + 4U] = bottom_right;
            indices[base + 5U] = bottom_left;
        }

        if (texture != nullptr) {
            static_cast<void>(SDL_SetTextureScaleMode(texture, sprite.scale_mode));
            static_cast<void>(SDL_SetTextureBlendMode(texture, sprite.blend_mode));
            // Vertex colours carry the sprite modulation on this path.
            static_cast<void>(SDL_SetTextureColorMod(texture, 255U, 255U, 255U));
            static_cast<void>(SDL_SetTextureAlphaMod(texture, 255U));
        } else {
            static_cast<void>(SDL_SetRenderDrawBlendMode(renderer, sprite.blend_mode));
        }

        const bool rendered = SDL_RenderGeometry(
            renderer,
            texture,
            vertices.data(),
            static_cast<int>((segment_count + 1U) * 2U),
            indices.data(),
            static_cast<int>(segment_count * 6U)
        );
        if (!rendered && texture != nullptr) {
            // The caller immediately falls back to SDL_RenderTextureRotated.
            // Restore the modulation that path expects before returning.
            static_cast<void>(SDL_SetTextureColorMod(
                texture, draw_color.r, draw_color.g, draw_color.b
            ));
            static_cast<void>(SDL_SetTextureAlphaMod(
                texture,
                static_cast<std::uint8_t>(
                    std::clamp(draw_alpha, 0.0F, 1.0F) * 255.0F
                )
            ));
        }
        return rendered;
    }

    // PULSEFORGE_P1_1_12_CHROMATIC_RENDER_V1
    // SDL_Renderer cannot execute the source GLSL. This bounded compatibility
    // path reproduces the shader's defining RGB channel separation with three
    // ordinary texture passes. Green establishes the alpha-composited base;
    // red/blue are then added at opposite offsets. Unsupported custom shaders
    // continue through the normal sprite path rather than aborting gameplay.
    [[nodiscard]] bool render_chromatic_sprite(
        const SceneSprite& sprite,
        SDL_Texture* const texture,
        const SDL_FRect* const source,
        const SDL_FRect& destination,
        const SDL_Color draw_color,
        const float draw_alpha,
        const double total_angle,
        const PsychShaderCompatState& shader_state
    ) noexcept {
        if (texture == nullptr || sprite.blend_mode != SDL_BLENDMODE_BLEND) {
            return false;
        }
        const float offset = psych_chromatic_offset_pixels(
            shader_state,
            destination.w
        );
        if (std::abs(offset) < 0.01F) {
            return false;
        }

        const auto alpha = static_cast<std::uint8_t>(
            std::clamp(draw_alpha, 0.0F, 1.0F) * 255.0F
        );
        const SDL_FRect red_destination{
            destination.x - offset,
            destination.y,
            destination.w,
            destination.h,
        };
        const SDL_FRect blue_destination{
            destination.x + offset,
            destination.y,
            destination.w,
            destination.h,
        };

        static_cast<void>(SDL_SetTextureScaleMode(texture, sprite.scale_mode));
        static_cast<void>(SDL_SetTextureAlphaMod(texture, alpha));

        static_cast<void>(SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND));
        static_cast<void>(SDL_SetTextureColorMod(
            texture, 0U, draw_color.g, 0U
        ));
        const bool green = SDL_RenderTextureRotated(
            renderer,
            texture,
            source,
            &destination,
            total_angle,
            nullptr,
            sprite.flip
        );

        static_cast<void>(SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_ADD));
        static_cast<void>(SDL_SetTextureColorMod(
            texture, draw_color.r, 0U, 0U
        ));
        const bool red = SDL_RenderTextureRotated(
            renderer,
            texture,
            source,
            &red_destination,
            total_angle,
            nullptr,
            sprite.flip
        );

        static_cast<void>(SDL_SetTextureColorMod(
            texture, 0U, 0U, draw_color.b
        ));
        const bool blue = SDL_RenderTextureRotated(
            renderer,
            texture,
            source,
            &blue_destination,
            total_angle,
            nullptr,
            sprite.flip
        );

        static_cast<void>(SDL_SetTextureColorMod(
            texture, draw_color.r, draw_color.g, draw_color.b
        ));
        static_cast<void>(SDL_SetTextureAlphaMod(texture, alpha));
        static_cast<void>(SDL_SetTextureBlendMode(texture, sprite.blend_mode));
        return green && red && blue;
    }

    // PULSEFORGE_P1_1_13_WIGGLE_RENDER_V1
    // The source Wiggle shader remaps texture coordinates. SDL_RenderGeometry
    // exposes per-vertex UVs, so a bounded fixed-grid mesh can reproduce all
    // five source effectType modes without a programmable shader backend.
    [[nodiscard]] bool render_wiggle_shader_sprite(
        const SceneSprite& sprite,
        SDL_Texture* const texture,
        const SDL_FRect* const source,
        const SDL_FRect& destination,
        const SDL_Color draw_color,
        const float draw_alpha,
        const double total_angle,
        const PsychShaderCompatState& shader_state,
        const double song_time_ms
    ) noexcept {
        if (texture == nullptr || sprite.texture_index == no_texture
            || sprite.texture_index >= textures.size()) {
            return false;
        }
        const auto segment_count = psych_wiggle_segment_count(
            shader_state,
            destination.w,
            destination.h
        );
        if (segment_count == 0U
            || segment_count > psych_wiggle_max_segments) {
            return false;
        }

        const auto& resource = textures[sprite.texture_index];
        if (resource.width <= 0.0F || resource.height <= 0.0F) {
            return false;
        }

        constexpr std::size_t side = psych_wiggle_max_segments + 1U;
        constexpr std::size_t maximum_vertices = side * side;
        constexpr std::size_t maximum_indices =
            psych_wiggle_max_segments * psych_wiggle_max_segments * 6U;
        std::array<SDL_Vertex, maximum_vertices> vertices{};
        std::array<int, maximum_indices> indices{};

        float u0 = 0.0F;
        float u1 = 1.0F;
        float v0 = 0.0F;
        float v1 = 1.0F;
        if (source != nullptr) {
            u0 = source->x / resource.width;
            u1 = (source->x + source->w) / resource.width;
            v0 = source->y / resource.height;
            v1 = (source->y + source->h) / resource.height;
        }

        const auto flip_bits = static_cast<unsigned int>(sprite.flip);
        if ((flip_bits & static_cast<unsigned int>(SDL_FLIP_HORIZONTAL)) != 0U) {
            std::swap(u0, u1);
        }
        if ((flip_bits & static_cast<unsigned int>(SDL_FLIP_VERTICAL)) != 0U) {
            std::swap(v0, v1);
        }

        const SDL_FColor vertex_color{
            static_cast<float>(draw_color.r) / 255.0F,
            static_cast<float>(draw_color.g) / 255.0F,
            static_cast<float>(draw_color.b) / 255.0F,
            std::clamp(draw_alpha, 0.0F, 1.0F),
        };
        const float center_x = destination.x + destination.w * 0.5F;
        const float center_y = destination.y + destination.h * 0.5F;
        constexpr double degrees_to_radians =
            0.017453292519943295769236907684886;
        const double bounded_angle = std::remainder(total_angle, 360.0);
        const double radians = bounded_angle * degrees_to_radians;
        const float cosine = static_cast<float>(std::cos(radians));
        const float sine = static_cast<float>(std::sin(radians));
        const auto rotate = [&](const float x, const float y) noexcept {
            const float local_x = x - center_x;
            const float local_y = y - center_y;
            return SDL_FPoint{
                center_x + local_x * cosine - local_y * sine,
                center_y + local_x * sine + local_y * cosine,
            };
        };

        const std::size_t row_stride = segment_count + 1U;
        for (std::size_t row = 0U; row <= segment_count; ++row) {
            const float normalized_v = static_cast<float>(row)
                / static_cast<float>(segment_count);
            const float y = destination.y + destination.h * normalized_v;
            for (std::size_t column = 0U; column <= segment_count; ++column) {
                const float normalized_u = static_cast<float>(column)
                    / static_cast<float>(segment_count);
                const float x = destination.x + destination.w * normalized_u;
                const auto warped = psych_wiggle_uv(
                    shader_state,
                    song_time_ms,
                    normalized_u,
                    normalized_v
                );
                const std::size_t vertex_index = row * row_stride + column;
                vertices[vertex_index] = SDL_Vertex{
                    rotate(x, y),
                    vertex_color,
                    SDL_FPoint{
                        std::lerp(u0, u1, warped.u),
                        std::lerp(v0, v1, warped.v),
                    },
                };
            }
        }

        std::size_t index_count{};
        for (std::size_t row = 0U; row < segment_count; ++row) {
            for (std::size_t column = 0U; column < segment_count; ++column) {
                const int top_left = static_cast<int>(
                    row * row_stride + column
                );
                const int top_right = top_left + 1;
                const int bottom_left = static_cast<int>(
                    (row + 1U) * row_stride + column
                );
                const int bottom_right = bottom_left + 1;
                indices[index_count++] = top_left;
                indices[index_count++] = top_right;
                indices[index_count++] = bottom_right;
                indices[index_count++] = top_left;
                indices[index_count++] = bottom_right;
                indices[index_count++] = bottom_left;
            }
        }

        static_cast<void>(SDL_SetTextureScaleMode(texture, sprite.scale_mode));
        static_cast<void>(SDL_SetTextureBlendMode(texture, sprite.blend_mode));
        static_cast<void>(SDL_SetTextureColorMod(texture, 255U, 255U, 255U));
        static_cast<void>(SDL_SetTextureAlphaMod(texture, 255U));

        const bool rendered = SDL_RenderGeometry(
            renderer,
            texture,
            vertices.data(),
            static_cast<int>(row_stride * row_stride),
            indices.data(),
            static_cast<int>(index_count)
        );
        if (!rendered) {
            static_cast<void>(SDL_SetTextureColorMod(
                texture, draw_color.r, draw_color.g, draw_color.b
            ));
            static_cast<void>(SDL_SetTextureAlphaMod(
                texture,
                static_cast<std::uint8_t>(
                    std::clamp(draw_alpha, 0.0F, 1.0F) * 255.0F
                )
            ));
        }
        return rendered;
    }


    // PULSEFORGE_P1_1_15_SNOWSTORM_RENDER_V1
    // The source Snowstorm shader is procedural cellular/simplex noise.
    // SDL_Renderer cannot execute that GLSL, so this native compatibility path
    // draws a deterministic five-layer additive particle field over the same
    // sprite bounds using a bounded five batched rectangle submissions.
    void render_snowstorm_overlay(
        const SDL_FRect& destination,
        const PsychShaderCompatState& shader_state,
        const double song_time_ms,
        const float draw_alpha
    ) noexcept {
        if (renderer == nullptr || destination.w <= 0.0F
            || destination.h <= 0.0F || draw_alpha <= 0.0F) {
            return;
        }

        constexpr std::array<std::size_t, 5U> counts{
            20U, 28U, 36U, 48U, 64U
        };
        constexpr std::size_t maximum_count = 64U;
        std::array<SDL_FRect, maximum_count> rectangles{};

        static_cast<void>(SDL_SetRenderDrawBlendMode(
            renderer,
            SDL_BLENDMODE_ADD
        ));

        // Preserve the shader's constant reddish/pink brightness floor.
        static_cast<void>(SDL_SetRenderDrawColor(
            renderer,
            16U,
            8U,
            10U,
            static_cast<std::uint8_t>(
                std::clamp(draw_alpha * 48.0F, 0.0F, 255.0F)
            )
        ));
        static_cast<void>(SDL_RenderFillRect(renderer, &destination));

        for (std::uint32_t layer = 0U; layer < counts.size(); ++layer) {
            const std::size_t count = counts[layer];
            for (std::size_t index = 0U; index < count; ++index) {
                const auto particle = psych_snowstorm_particle(
                    layer,
                    static_cast<std::uint32_t>(index),
                    shader_state,
                    song_time_ms
                );
                const float size = std::max(
                    1.0F,
                    particle.size
                        * std::max(1.0F, std::min(destination.w, destination.h)
                            / 360.0F)
                );
                rectangles[index] = SDL_FRect{
                    destination.x
                        + particle.x * std::max(0.0F, destination.w - size),
                    destination.y
                        + particle.y * std::max(0.0F, destination.h - size),
                    size,
                    size,
                };
            }

            const float layer_ratio = static_cast<float>(layer) / 4.0F;
            static_cast<void>(SDL_SetRenderDrawColor(
                renderer,
                255U,
                static_cast<std::uint8_t>(
                    std::lerp(150.0F, 205.0F, layer_ratio)
                ),
                static_cast<std::uint8_t>(
                    std::lerp(175.0F, 220.0F, layer_ratio)
                ),
                static_cast<std::uint8_t>(std::clamp(
                    draw_alpha
                        * (0.20F + layer_ratio * 0.32F) * 255.0F,
                    0.0F,
                    255.0F
                ))
            ));
            static_cast<void>(SDL_RenderFillRects(
                renderer,
                rectangles.data(),
                static_cast<int>(count)
            ));
        }

        static_cast<void>(SDL_SetRenderDrawBlendMode(
            renderer,
            SDL_BLENDMODE_BLEND
        ));
    }

    void release_sustain_animation(
        const NoteOwner owner,
        const double song_time_ms,
        const std::string_view note_kind,
        const double sustain_tail_ms
    ) noexcept {
        if (animation_id_equal(note_kind, "No Animation")) return;
        const auto target = animation_id_equal(note_kind, "GF Sing")
                || animation_id_equal(note_kind, "GF Cross Fade")
            ? NoteAnimationTarget::girlfriend
            : NoteAnimationTarget::owner;
        release_sustain_animation_configured(
            owner, song_time_ms, target, sustain_tail_ms
        );
    }

    void release_sustain_animation_configured(
        const NoteOwner owner,
        const double song_time_ms,
        const NoteAnimationTarget target,
        const double sustain_tail_ms
    ) noexcept {
        if (!std::isfinite(song_time_ms)) return;
        const auto role = note_animation_role(owner, target);
        for (auto& sprite : sprites) {
            if (sprite.role != role) continue;
            remove_sustain_animation_hold(
                sprite, song_time_ms, sustain_tail_ms
            );
            // Do not force idle immediately. A drop/miss animation may be
            // installed by the caller in the same event; otherwise the normal
            // transient timeout still controls the return to idle/dance.
            sprite.transient_until_ms = std::max(
                sprite.transient_started_ms,
                std::min(
                    sprite.transient_until_ms,
                    std::max(song_time_ms, sprite.sustain_animation_until_ms)
                )
            );
        }
    }


    void render_sprite(
        const SceneSprite& sprite,
        const float viewport_scale,
        const float viewport_x,
        const float viewport_y,
        const float pulse,
        const double song_time_ms
    ) noexcept {
        if (sprite.texture_index == no_texture
            && !sprite.allow_textureless_draw) {
            return;
        }
        const bool character = sprite.role != SpriteRole::stage;
        const float beat_scale = character ? pulse : 1.0F;
        float source_width = sprite.fallback_width;
        float source_height = sprite.fallback_height;
        float frame_width = source_width;
        float frame_height = source_height;
        float content_offset_x{};
        float content_offset_y{};
        float animation_offset_x{};
        float animation_offset_y{};
        const SDL_FRect* source = nullptr;
        SDL_FRect source_rectangle{};
        if (sprite.texture_index != no_texture
            && sprite.texture_index < textures.size()) {
            const auto& texture = textures[sprite.texture_index];
            source_width = texture.width;
            source_height = texture.height;
            frame_width = source_width;
            frame_height = source_height;
            const AtlasFrame* active_frame = sprite.frame.has_value()
                ? std::addressof(*sprite.frame)
                : nullptr;
            const bool transient_active = sprite.transient_animation
                    < sprite.animations.size()
                && std::isfinite(song_time_ms)
                // A backwards seek must not display a future singing pose
                // before the note that started it.
                && song_time_ms + 0.001 >= sprite.transient_started_ms
                && song_time_ms < sprite.transient_until_ms;
            const std::size_t animation_index = transient_active
                ? sprite.transient_animation
                : sprite.default_animation;
            if (animation_index < sprite.animations.size()
                && std::isfinite(song_time_ms)) {
                const auto& animation = sprite.animations[animation_index];
                animation_offset_x = animation.offset_x;
                animation_offset_y = animation.offset_y;
                const double animation_time = transient_active
                    ? std::max(0.0, song_time_ms - sprite.transient_started_ms)
                    : std::max(0.0, song_time_ms);
                const long double elapsed_frames = std::max(
                    0.0L,
                    static_cast<long double>(animation_time)
                        * static_cast<long double>(animation.fps)
                        / 1'000.0L
                );
                auto frame_index = static_cast<std::uint64_t>(elapsed_frames);
                const bool loop = animation.loop
                    || (!transient_active && character);
                if (loop) {
                    frame_index %= animation.frames.size();
                } else {
                    frame_index = std::min<std::uint64_t>(
                        frame_index,
                        animation.frames.size() - 1U
                    );
                }
                const auto atlas_index = animation.frames[
                    static_cast<std::size_t>(frame_index)
                ];
                if (atlas_index < texture.atlas.size()) {
                    active_frame = std::addressof(texture.atlas[atlas_index]);
                }
            }
            if (active_frame != nullptr) {
                source_rectangle = active_frame->source;
                source = &source_rectangle;
                source_width = source_rectangle.w;
                source_height = source_rectangle.h;
                frame_width = active_frame->frame_width;
                frame_height = active_frame->frame_height;
                content_offset_x = active_frame->content_offset_x;
                content_offset_y = active_frame->content_offset_y;
            }
        }

        const float effective_zoom = sprite.screen_space
            ? script_hud_zoom
            : stage_zoom * script_camera_zoom;
        const float zoomed_scale_x = sprite.scale_x * effective_zoom
            * viewport_scale * beat_scale;
        const float zoomed_scale_y = sprite.scale_y * effective_zoom
            * viewport_scale * beat_scale;
        // Psych/Flixel scrollFactor scales how strongly a world sprite follows
        // the game camera. HUD sprites instead use the independent HUD camera;
        // negate its x/y because application-side camHUD coordinates are
        // expressed as visual translation rather than world-camera scroll.
        const float camera_x = sprite.screen_space
            ? -script_hud_x
            : script_camera_x * sprite.scroll_factor_x;
        const float camera_y = sprite.screen_space
            ? -script_hud_y
            : script_camera_y * sprite.scroll_factor_y;
        const float world_x = (sprite.x - camera_x - logical_width * 0.5F)
                * effective_zoom
            + logical_width * 0.5F;
        const float world_y = (sprite.y - camera_y - logical_height * 0.5F)
                * effective_zoom
            + logical_height * 0.5F;
        const float pulse_adjust_x = character
            ? frame_width * sprite.scale_x * effective_zoom * viewport_scale
                * (beat_scale - 1.0F) * 0.5F
            : 0.0F;
        const float pulse_adjust_y = character
            ? frame_height * sprite.scale_y * effective_zoom * viewport_scale
                * (beat_scale - 1.0F) * 0.5F
            : 0.0F;
        SDL_FRect destination{
            viewport_x + world_x * viewport_scale
                + (content_offset_x - animation_offset_x) * zoomed_scale_x
                - pulse_adjust_x,
            viewport_y + world_y * viewport_scale
                + (content_offset_y - animation_offset_y) * zoomed_scale_y
                - pulse_adjust_y,
            source_width * zoomed_scale_x,
            source_height * zoomed_scale_y,
        };
        const float wave_padding = sprite.wavy_effect.enabled
            ? std::abs(sprite.wavy_effect.amplitude) * destination.w
            : 0.0F;
        if (destination.w <= 0.0F || destination.h <= 0.0F
            || destination.x - wave_padding
                > viewport_x + logical_width * viewport_scale
            || destination.y > viewport_y + logical_height * viewport_scale
            || destination.x + destination.w + wave_padding < viewport_x
            || destination.y + destination.h < viewport_y) {
            return;
        }

        const float camera_alpha = sprite.screen_space
            ? script_hud_alpha
            : script_camera_alpha;
        const double total_angle = sprite.angle + (sprite.screen_space
            ? script_hud_angle
            : script_camera_angle);

        std::array<
            PsychShaderScalarUniformView,
            psych_shader_max_uniforms_per_sprite
        > shader_uniform_views{};
        const std::size_t shader_uniform_count = std::min(
            sprite.shader_uniforms.size(),
            shader_uniform_views.size()
        );
        for (std::size_t index = 0U; index < shader_uniform_count; ++index) {
            shader_uniform_views[index] = PsychShaderScalarUniformView{
                sprite.shader_uniforms[index].name,
                sprite.shader_uniforms[index].value,
            };
        }
        const auto shader_kind = sprite.shader_id.empty()
            ? PsychShaderCompatKind::generic_scalar
            : psych_shader_kind(sprite.shader_id);
        const auto shader_state = psych_shader_compat_state(
            std::span<const PsychShaderScalarUniformView>(
                shader_uniform_views.data(),
                shader_uniform_count
            ),
            shader_kind
        );
        // PULSEFORGE_P1_1_13_PULSE_COLOR_RENDER_V1
        // pulseEffect.frag mutates RGB per pixel. On SDL_Renderer the bounded
        // compatibility path applies a time-driven global RGB approximation.
        const auto pulse_color = shader_kind == PsychShaderCompatKind::pulse_effect
            ? psych_pulse_color_multipliers(shader_state, song_time_ms)
            : PsychShaderColorMultipliers{};
        SDL_Color shader_base_color = sprite.color;
        // Textureless makeGraphic sprites are a single flat colour, so the
        // colour transform can be exact without a texture/cache path.
        if (sprite.texture_index == no_texture
            && psych_shader_uses_cpu_color_transform(shader_kind)) {
            const auto flat = psych_cpu_color_shader_rgb(
                shader_kind,
                shader_state,
                PsychShaderRgb{
                    static_cast<float>(sprite.color.r) / 255.0F,
                    static_cast<float>(sprite.color.g) / 255.0F,
                    static_cast<float>(sprite.color.b) / 255.0F,
                }
            );
            shader_base_color.r = static_cast<std::uint8_t>(
                std::clamp(std::lround(flat.red * 255.0F), 0L, 255L)
            );
            shader_base_color.g = static_cast<std::uint8_t>(
                std::clamp(std::lround(flat.green * 255.0F), 0L, 255L)
            );
            shader_base_color.b = static_cast<std::uint8_t>(
                std::clamp(std::lround(flat.blue * 255.0F), 0L, 255L)
            );
        }
        const SDL_Color draw_color{
            psych_shader_modulated_channel(
                shader_base_color.r,
                shader_state.red_multiplier * pulse_color.red
            ),
            psych_shader_modulated_channel(
                shader_base_color.g,
                shader_state.green_multiplier * pulse_color.green
            ),
            psych_shader_modulated_channel(
                shader_base_color.b,
                shader_state.blue_multiplier * pulse_color.blue
            ),
            shader_base_color.a,
        };
        const float draw_alpha = std::clamp(
            sprite.alpha * camera_alpha * shader_state.alpha_multiplier,
            0.0F,
            1.0F
        );
        const bool chromatic_shader =
            shader_kind == PsychShaderCompatKind::chromatic;
        const bool wiggle_shader =
            shader_kind == PsychShaderCompatKind::wiggle;
        const bool snowstorm_shader =
            shader_kind == PsychShaderCompatKind::snowstorm;

        if (sprite.texture_index != no_texture
            && sprite.texture_index < textures.size()) {
            auto* texture = textures[sprite.texture_index].texture.get();
            // PULSEFORGE_P1_1_14_CPU_COLOR_RENDER_V1
            // PULSEFORGE_P1_1_15_CPU_TEXTURE_SHADER_RENDER_V1
            if (psych_shader_uses_cpu_texture_transform(shader_kind)
                && !psych_cpu_shader_neutral(shader_kind, shader_state)) {
                if (auto* transformed = resolve_cpu_shader_texture(
                        sprite,
                        shader_kind,
                        shader_state
                    );
                    transformed != nullptr) {
                    texture = transformed;
                }
            }
            static_cast<void>(SDL_SetTextureColorMod(
                texture,
                draw_color.r,
                draw_color.g,
                draw_color.b
            ));
            static_cast<void>(SDL_SetTextureAlphaMod(
                texture,
                static_cast<std::uint8_t>(draw_alpha * 255.0F)
            ));
            static_cast<void>(SDL_SetTextureScaleMode(texture, sprite.scale_mode));
            static_cast<void>(SDL_SetTextureBlendMode(texture, sprite.blend_mode));
            if (sprite.wavy_effect.enabled
                && render_wavy_sprite(
                    sprite,
                    texture,
                    source,
                    destination,
                    song_time_ms,
                    draw_color,
                    draw_alpha,
                    total_angle
                )) {
                if (snowstorm_shader) {
                    render_snowstorm_overlay(
                        destination, shader_state, song_time_ms, draw_alpha
                    );
                }
                return;
            }
            if (wiggle_shader
                && render_wiggle_shader_sprite(
                    sprite,
                    texture,
                    source,
                    destination,
                    draw_color,
                    draw_alpha,
                    total_angle,
                    shader_state,
                    song_time_ms
                )) {
                if (snowstorm_shader) {
                    render_snowstorm_overlay(
                        destination, shader_state, song_time_ms, draw_alpha
                    );
                }
                return;
            }
            if (chromatic_shader
                && render_chromatic_sprite(
                    sprite,
                    texture,
                    source,
                    destination,
                    draw_color,
                    draw_alpha,
                    total_angle,
                    shader_state
                )) {
                if (snowstorm_shader) {
                    render_snowstorm_overlay(
                        destination, shader_state, song_time_ms, draw_alpha
                    );
                }
                return;
            }
            // Mesh submission failure is visual-only: preserve gameplay by
            // falling back to the exact pre-P1.1.11 sprite path.
            static_cast<void>(SDL_RenderTextureRotated(
                renderer,
                texture,
                source,
                &destination,
                total_angle,
                nullptr,
                sprite.flip
            ));
            if (snowstorm_shader) {
                render_snowstorm_overlay(
                    destination, shader_state, song_time_ms, draw_alpha
                );
            }
            return;
        }

        if (sprite.wavy_effect.enabled
            && render_wavy_sprite(
                sprite,
                nullptr,
                nullptr,
                destination,
                song_time_ms,
                draw_color,
                draw_alpha,
                total_angle
            )) {
            if (snowstorm_shader) {
                render_snowstorm_overlay(
                    destination, shader_state, song_time_ms, draw_alpha
                );
            }
            return;
        }

        SDL_SetRenderDrawBlendMode(renderer, sprite.blend_mode);
        SDL_SetRenderDrawColor(
            renderer,
            draw_color.r,
            draw_color.g,
            draw_color.b,
            static_cast<std::uint8_t>(draw_alpha * 190.0F)
        );
        static_cast<void>(SDL_RenderFillRect(renderer, &destination));
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 180);
        static_cast<void>(SDL_RenderRect(renderer, &destination));
        if (snowstorm_shader) {
            render_snowstorm_overlay(
                destination, shader_state, song_time_ms, draw_alpha
            );
        }
    }

    [[nodiscard]] const NoteSkinFrame* note_skin_frame(
        const RuntimeNoteSkinElement element,
        const std::uint8_t lane,
        const RuntimeNoteSkinProfile profile = runtime_note_skin_default_profile
    ) const noexcept {
        const auto element_index = note_skin_element_index(element);
        if (!four_lane_chart || !element_index.has_value()
            || static_cast<std::size_t>(lane) >= note_skin_lane_count) {
            return nullptr;
        }
        const NoteSkinFrames* frames = std::addressof(note_skin_frames);
        if (profile != runtime_note_skin_default_profile) {
            const auto profile_index = static_cast<std::size_t>(profile - 1U);
            if (profile_index >= note_skin_profiles.size()) {
                return nullptr;
            }
            frames = std::addressof(note_skin_profiles[profile_index].frames);
        }
        const auto& frame = (*frames)[*element_index][
            static_cast<std::size_t>(lane)
        ];
        if (!frame.valid || frame.texture_index == no_texture
            || frame.texture_index >= textures.size()
            || textures[frame.texture_index].texture == nullptr) {
            return nullptr;
        }
        return std::addressof(frame);
    }

    [[nodiscard]] bool note_skin_available(
        const RuntimeNoteSkinElement element,
        const std::uint8_t lane,
        const RuntimeNoteSkinProfile profile = runtime_note_skin_default_profile
    ) const noexcept {
        return note_skin_frame(element, lane, profile) != nullptr;
    }

    [[nodiscard]] bool render_note_skin(
        const RuntimeNoteSkinDraw& draw
    ) noexcept {
        const auto* frame = note_skin_frame(
            draw.element, draw.lane, draw.profile
        );
        const auto& rectangle = draw.destination;
        if (renderer == nullptr || frame == nullptr
            || !std::isfinite(rectangle.x) || !std::isfinite(rectangle.y)
            || !std::isfinite(rectangle.width)
            || !std::isfinite(rectangle.height)
            || rectangle.width <= 0.0F || rectangle.height <= 0.0F
            || !std::isfinite(frame->frame_width)
            || !std::isfinite(frame->frame_height)
            || frame->frame_width <= 0.0F || frame->frame_height <= 0.0F) {
            return false;
        }

        const float scale_x = rectangle.width / frame->frame_width;
        const float scale_y = rectangle.height / frame->frame_height;
        SDL_FRect destination{
            rectangle.x + frame->content_offset_x * scale_x,
            rectangle.y + frame->content_offset_y * scale_y,
            frame->source.w * scale_x,
            frame->source.h * scale_y,
        };
        if (!std::isfinite(destination.x) || !std::isfinite(destination.y)
            || !std::isfinite(destination.w) || !std::isfinite(destination.h)
            || destination.w <= 0.0F || destination.h <= 0.0F) {
            return false;
        }

        auto* texture = textures[frame->texture_index].texture.get();
        static_cast<void>(SDL_SetTextureColorMod(
            texture, draw.rgb[0], draw.rgb[1], draw.rgb[2]
        ));
        static_cast<void>(SDL_SetTextureAlphaMod(texture, draw.alpha));
        static_cast<void>(SDL_SetTextureScaleMode(texture, frame->scale_mode));
        return SDL_RenderTextureRotated(
            renderer,
            texture,
            &frame->source,
            &destination,
            std::isfinite(draw.angle) ? draw.angle : 0.0,
            nullptr,
            draw.flip_vertical ? SDL_FLIP_VERTICAL : SDL_FLIP_NONE
        );
    }

    void begin_note_skin_profile_frame(const bool enabled) noexcept {
        note_skin_profiling = enabled;
        note_skin_profile = {};
    }

    [[nodiscard]] RuntimeNoteSkinProfileStats note_skin_profile_stats()
        const noexcept {
        return note_skin_profile;
    }

    [[nodiscard]] bool render_note_skin_batch(
        const std::span<const RuntimeNoteSkinDraw> draws
    ) noexcept {
        const auto batch_started_ns = note_skin_profiling
            ? SDL_GetTicksNS()
            : std::uint64_t{0U};
        if (renderer == nullptr || draws.empty()) {
            if (note_skin_profiling) {
                note_skin_profile.batch_total_ns +=
                    SDL_GetTicksNS() - batch_started_ns;
            }
            return renderer != nullptr;
        }

        struct BatchKey final {
            std::size_t texture_index{no_texture};
            SDL_ScaleMode scale_mode{SDL_SCALEMODE_LINEAR};
        };
        std::array<BatchKey, maximum_runtime_note_skin_batch_keys> keys{};
        std::size_t key_count{};

        // PULSEFORGE_P1_5_0B_PROFILE_AWARE_NOTE_BATCH_KEYS_V1
        // Discover only textures referenced by this frame's bounded draw batch.
        // This preserves batching when default and custom note skins coexist.
        for (const auto& draw : draws) {
            const auto* frame = note_skin_frame(
                draw.element, draw.lane, draw.profile
            );
            if (frame == nullptr) {
                continue;
            }
            const auto duplicate = std::find_if(
                keys.begin(),
                keys.begin() + static_cast<std::ptrdiff_t>(key_count),
                [&](const BatchKey& key) {
                    return key.texture_index == frame->texture_index
                        && key.scale_mode == frame->scale_mode;
                }
            );
            if (duplicate == keys.begin()
                    + static_cast<std::ptrdiff_t>(key_count)
                && key_count < keys.size()) {
                keys[key_count++] = {
                    frame->texture_index,
                    frame->scale_mode,
                };
            }
        }

        constexpr std::size_t maximum_quads_per_submission = 2'048U;
        bool success = true;

        for (std::size_t key_index = 0U;
             key_index < key_count;
             ++key_index) {
            const auto key = keys[key_index];
            auto& resource = textures[key.texture_index];
            auto* texture = resource.texture.get();
            if (texture == nullptr || resource.width <= 0.0F
                || resource.height <= 0.0F) {
                success = false;
                continue;
            }

            static_cast<void>(SDL_SetTextureColorMod(
                texture, 255U, 255U, 255U
            ));
            static_cast<void>(SDL_SetTextureAlphaMod(texture, 255U));
            static_cast<void>(SDL_SetTextureScaleMode(
                texture,
                key.scale_mode
            ));

            note_skin_batch_vertices.clear();
            note_skin_batch_indices.clear();
            note_skin_batch_fallback_draws.clear();

            const auto submit = [&]() noexcept {
                if (note_skin_batch_vertices.empty()) {
                    return true;
                }
                const auto submitted_quads =
                    note_skin_batch_vertices.size() / 4U;
                const auto submit_started_ns = note_skin_profiling
                    ? SDL_GetTicksNS()
                    : std::uint64_t{0U};
                bool rendered = SDL_RenderGeometry(
                    renderer,
                    texture,
                    note_skin_batch_vertices.data(),
                    static_cast<int>(note_skin_batch_vertices.size()),
                    note_skin_batch_indices.data(),
                    static_cast<int>(note_skin_batch_indices.size())
                );
                if (note_skin_profiling) {
                    note_skin_profile.geometry_submit_ns +=
                        SDL_GetTicksNS() - submit_started_ns;
                    note_skin_profile.quads += submitted_quads;
                    ++note_skin_profile.submissions;
                    if (!rendered) {
                        ++note_skin_profile.failed_submissions;
                    }
                }
                if (!rendered) {
                    const auto fallback_started_ns = note_skin_profiling
                        ? SDL_GetTicksNS()
                        : std::uint64_t{0U};
                    // Fall back only for the failed chunk. The previous
                    // implementation redrew the COMPLETE frame batch after a
                    // partial geometry failure, causing brightness flashes and
                    // catastrophic per-note draw-call spikes on dense custom
                    // skins.
                    rendered = true;
                    for (const auto& fallback_draw
                         : note_skin_batch_fallback_draws) {
                        rendered = render_note_skin(fallback_draw) && rendered;
                    }
                    if (note_skin_profiling) {
                        note_skin_profile.fallback_ns +=
                            SDL_GetTicksNS() - fallback_started_ns;
                        note_skin_profile.fallback_draws +=
                            note_skin_batch_fallback_draws.size();
                    }
                    // Individual fallback calls alter texture modulation.
                    // Restore neutral state before a later geometry chunk.
                    static_cast<void>(SDL_SetTextureColorMod(
                        texture, 255U, 255U, 255U
                    ));
                    static_cast<void>(SDL_SetTextureAlphaMod(texture, 255U));
                    static_cast<void>(SDL_SetTextureScaleMode(
                        texture,
                        key.scale_mode
                    ));
                }
                note_skin_batch_vertices.clear();
                note_skin_batch_indices.clear();
                note_skin_batch_fallback_draws.clear();
                return rendered;
            };

            for (const auto& draw : draws) {
                const auto* frame = note_skin_frame(
                    draw.element,
                    draw.lane,
                    draw.profile
                );
                if (frame == nullptr
                    || frame->texture_index != key.texture_index
                    || frame->scale_mode != key.scale_mode) {
                    continue;
                }

                const auto& rectangle = draw.destination;
                if (!std::isfinite(rectangle.x)
                    || !std::isfinite(rectangle.y)
                    || !std::isfinite(rectangle.width)
                    || !std::isfinite(rectangle.height)
                    || rectangle.width <= 0.0F
                    || rectangle.height <= 0.0F
                    || !std::isfinite(frame->frame_width)
                    || !std::isfinite(frame->frame_height)
                    || frame->frame_width <= 0.0F
                    || frame->frame_height <= 0.0F) {
                    continue;
                }

                const float scale_x =
                    rectangle.width / frame->frame_width;
                const float scale_y =
                    rectangle.height / frame->frame_height;
                const SDL_FRect destination{
                    rectangle.x + frame->content_offset_x * scale_x,
                    rectangle.y + frame->content_offset_y * scale_y,
                    frame->source.w * scale_x,
                    frame->source.h * scale_y,
                };
                if (!std::isfinite(destination.x)
                    || !std::isfinite(destination.y)
                    || !std::isfinite(destination.w)
                    || !std::isfinite(destination.h)
                    || destination.w <= 0.0F
                    || destination.h <= 0.0F) {
                    continue;
                }

                if (note_skin_batch_vertices.size()
                        / 4U >= maximum_quads_per_submission
                    && !submit()) {
                    success = false;
                }

                const float u0 = frame->source.x / resource.width;
                const float u1 =
                    (frame->source.x + frame->source.w) / resource.width;
                float v0 = frame->source.y / resource.height;
                float v1 =
                    (frame->source.y + frame->source.h) / resource.height;
                if (draw.flip_vertical) {
                    std::swap(v0, v1);
                }

                const float alpha =
                    static_cast<float>(draw.alpha) / 255.0F;
                const SDL_FColor color{
                    static_cast<float>(draw.rgb[0]) / 255.0F,
                    static_cast<float>(draw.rgb[1]) / 255.0F,
                    static_cast<float>(draw.rgb[2]) / 255.0F,
                    alpha,
                };
                const auto base = static_cast<int>(
                    note_skin_batch_vertices.size()
                );
                note_skin_batch_fallback_draws.push_back(draw);

                std::array<SDL_FPoint, 4U> positions{{
                    {destination.x, destination.y},
                    {destination.x + destination.w, destination.y},
                    {destination.x + destination.w, destination.y + destination.h},
                    {destination.x, destination.y + destination.h},
                }};
                if (std::isfinite(draw.angle) && std::abs(draw.angle) > 1.0e-9) {
                    constexpr double degrees_to_radians =
                        3.14159265358979323846 / 180.0;
                    const double radians = draw.angle * degrees_to_radians;
                    const double cosine = std::cos(radians);
                    const double sine = std::sin(radians);
                    const double center_x = destination.x + destination.w * 0.5;
                    const double center_y = destination.y + destination.h * 0.5;
                    for (auto& point : positions) {
                        const double dx = static_cast<double>(point.x) - center_x;
                        const double dy = static_cast<double>(point.y) - center_y;
                        point.x = static_cast<float>(center_x + dx * cosine - dy * sine);
                        point.y = static_cast<float>(center_y + dx * sine + dy * cosine);
                    }
                }
                note_skin_batch_vertices.push_back({positions[0], color, {u0, v0}});
                note_skin_batch_vertices.push_back({positions[1], color, {u1, v0}});
                note_skin_batch_vertices.push_back({positions[2], color, {u1, v1}});
                note_skin_batch_vertices.push_back({positions[3], color, {u0, v1}});

                note_skin_batch_indices.insert(
                    note_skin_batch_indices.end(),
                    {
                        base,
                        base + 1,
                        base + 2,
                        base,
                        base + 2,
                        base + 3,
                    }
                );
            }

            if (!submit()) {
                success = false;
            }
        }

        if (note_skin_profiling) {
            note_skin_profile.batch_total_ns +=
                SDL_GetTicksNS() - batch_started_ns;
        }
        return success;
    }

    [[nodiscard]] bool script_get_camera_target(
        const std::string_view object,
        double& target_x,
        double& target_y
    ) const noexcept {
        const auto* sprite = find_script_sprite(object);
        if (sprite == nullptr || sprite->role == SpriteRole::stage) {
            return false;
        }

        double width = static_cast<double>(sprite->fallback_width)
            * std::abs(static_cast<double>(sprite->scale_x));
        double height = static_cast<double>(sprite->fallback_height)
            * std::abs(static_cast<double>(sprite->scale_y));

        if (sprite->frame.has_value()) {
            width = static_cast<double>(sprite->frame->frame_width)
                * std::abs(static_cast<double>(sprite->scale_x));
            height = static_cast<double>(sprite->frame->frame_height)
                * std::abs(static_cast<double>(sprite->scale_y));
        } else if (sprite->texture_index != no_texture
                   && sprite->texture_index < textures.size()) {
            const auto& texture = textures[sprite->texture_index];
            width = static_cast<double>(texture.width)
                * std::abs(static_cast<double>(sprite->scale_x));
            height = static_cast<double>(texture.height)
                * std::abs(static_cast<double>(sprite->scale_y));
        }

        const PsychCameraRole role =
            sprite->role == SpriteRole::girlfriend
                ? PsychCameraRole::girlfriend
                : (sprite->role == SpriteRole::opponent
                    || sprite->role == SpriteRole::secondary_opponent)
                    ? PsychCameraRole::opponent
                    : PsychCameraRole::player;

        const auto target = psych_camera_target({
            role,
            static_cast<double>(sprite->x) + width * 0.5,
            static_cast<double>(sprite->y) + height * 0.5,
            static_cast<double>(sprite->camera_position_x),
            static_cast<double>(sprite->camera_position_y),
            static_cast<double>(sprite->stage_camera_offset_x),
            static_cast<double>(sprite->stage_camera_offset_y),
        });

        target_x = target.x;
        target_y = target.y;
        return std::isfinite(target_x) && std::isfinite(target_y);
    }

    [[nodiscard]] bool script_get_number(
        const std::string_view object,
        const std::string_view property,
        double& value
    ) const noexcept {
        const auto* sprite = find_script_sprite(object);
        if (sprite == nullptr) return false;
        if (equals_ascii_insensitive(property, "x")) value = sprite->x;
        else if (equals_ascii_insensitive(property, "y")) value = sprite->y;
        else if (equals_ascii_insensitive(property, "alpha")) value = sprite->alpha;
        else if (equals_ascii_insensitive(property, "angle")) value = sprite->angle;
        else if (equals_ascii_insensitive(property, "scale.x")
            || equals_ascii_insensitive(property, "scaleX")) value = sprite->scale_x;
        else if (equals_ascii_insensitive(property, "scale.y")
            || equals_ascii_insensitive(property, "scaleY")) value = sprite->scale_y;
        else if (equals_ascii_insensitive(property, "width")) value = sprite->fallback_width * sprite->scale_x;
        else if (equals_ascii_insensitive(property, "height")) value = sprite->fallback_height * sprite->scale_y;
        else if (equals_ascii_insensitive(property, "color")) {
            value = static_cast<double>(
                0xFF000000U
                | (static_cast<std::uint32_t>(sprite->color.r) << 16U)
                | (static_cast<std::uint32_t>(sprite->color.g) << 8U)
                | static_cast<std::uint32_t>(sprite->color.b)
            );
        }
        else if (equals_ascii_insensitive(property, "visible")) value = sprite->visible ? 1.0 : 0.0;
        else return false;
        return true;
    }

    [[nodiscard]] bool script_set_number(
        const std::string_view object,
        const std::string_view property,
        const double value
    ) noexcept {
        if (!std::isfinite(value)) return false;
        auto* sprite = find_script_sprite(object);
        if (sprite == nullptr) return false;
        if (equals_ascii_insensitive(property, "x")) {
            sprite->x = finite_float(value, sprite->x, -100'000.0F, 100'000.0F);
        } else if (equals_ascii_insensitive(property, "y")) {
            sprite->y = finite_float(value, sprite->y, -100'000.0F, 100'000.0F);
        } else if (equals_ascii_insensitive(property, "alpha")) {
            sprite->alpha = finite_float(value, sprite->alpha, 0.0F, 1.0F);
        } else if (equals_ascii_insensitive(property, "angle")) {
            sprite->angle = std::clamp(value, -360'000.0, 360'000.0);
        } else if (equals_ascii_insensitive(property, "scale.x")
            || equals_ascii_insensitive(property, "scaleX")) {
            sprite->scale_x = finite_float(value, sprite->scale_x, 0.001F, 128.0F);
        } else if (equals_ascii_insensitive(property, "scale.y")
            || equals_ascii_insensitive(property, "scaleY")) {
            sprite->scale_y = finite_float(value, sprite->scale_y, 0.001F, 128.0F);
        } else if (equals_ascii_insensitive(property, "color")) {
            const auto raw = static_cast<std::uint32_t>(
                std::clamp(std::llround(value), 0LL, 0xFFFFFFFFLL)
            );
            sprite->color.r = static_cast<std::uint8_t>((raw >> 16U) & 0xFFU);
            sprite->color.g = static_cast<std::uint8_t>((raw >> 8U) & 0xFFU);
            sprite->color.b = static_cast<std::uint8_t>(raw & 0xFFU);
        } else if (equals_ascii_insensitive(property, "visible")) {
            sprite->visible = value != 0.0;
        } else {
            return false;
        }
        return true;
    }

    [[nodiscard]] bool script_set_visible(
        const std::string_view object,
        const bool visible
    ) noexcept {
        auto* sprite = find_script_sprite(object);
        if (sprite == nullptr) return false;
        sprite->visible = visible;
        return true;
    }

    [[nodiscard]] bool script_create_sprite(
        const std::string_view tag,
        const std::string_view image,
        const double x,
        const double y,
        const bool animated,
        std::string* const error
    ) {
        (void)animated;
        if (tag.empty() || tag.size() > 128U || image.size() > 512U) {
            if (error != nullptr) *error = "Lua sprite tag/image is outside the bounded scene limits";
            return false;
        }
        if (auto* existing = find_script_sprite_exact(tag); existing != nullptr) {
            // RuntimeScene statically pre-parses safe declarations from Psych
            // stage Lua so a stage can be visible even without the VM. When the
            // live Lua runtime is enabled, onCreate executes the same
            // makeLuaSprite/makeAnimatedLuaSprite calls. Treat that stage-sprite
            // collision as an idempotent rebind instead of a fatal duplicate;
            // otherwise the very first makeLuaSprite aborts onCreate and all
            // later HUD/background/character motion callbacks disappear.
            if (existing->role != SpriteRole::stage) {
                if (error != nullptr) {
                    *error = "Lua sprite tag collides with a character object";
                }
                return false;
            }
            existing->script_created = true;
            existing->visible = false; // Psych requires addLuaSprite afterwards.
            existing->allow_textureless_draw = false;
            existing->x = finite_float(x, existing->x, -100'000.0F, 100'000.0F);
            existing->y = finite_float(y, existing->y, -100'000.0F, 100'000.0F);
            existing->animations.clear();
            existing->default_animation = no_texture;
            existing->transient_animation = no_texture;
            existing->frame.reset();
            if (!image.empty()) {
                existing->texture_index = load_texture(image);
                if (existing->texture_index != no_texture
                    && existing->texture_index < textures.size()) {
                    const auto& texture = textures[existing->texture_index];
                    existing->fallback_width = texture.width;
                    existing->fallback_height = texture.height;
                    if (!texture.atlas.empty()) {
                        existing->frame = texture.atlas.front();
                    }
                    existing->allow_textureless_draw = true;
                }
            }
            if (error != nullptr) error->clear();
            return true;
        }
        if (sprites.size() >= limits.maximum_sprites) {
            if (error != nullptr) *error = "Lua sprite count exceeded RuntimeScene limit";
            return false;
        }
        SceneSprite sprite;
        initialize_sprite_identity(sprite, std::string(tag));
        sprite.role = SpriteRole::stage;
        sprite.script_created = true;
        sprite.visible = false; // Psych makeLuaSprite requires addLuaSprite.
        sprite.allow_textureless_draw = false;
        sprite.x = finite_float(x, 0.0F, -100'000.0F, 100'000.0F);
        sprite.y = finite_float(y, 0.0F, -100'000.0F, 100'000.0F);
        if (!image.empty()) {
            sprite.texture_index = load_texture(image);
            if (sprite.texture_index != no_texture
                && sprite.texture_index < textures.size()) {
                const auto& texture = textures[sprite.texture_index];
                sprite.fallback_width = texture.width;
                sprite.fallback_height = texture.height;
                if (!texture.atlas.empty()) sprite.frame = texture.atlas.front();
                sprite.allow_textureless_draw = true;
            }
        }
        sprites.push_back(std::move(sprite));
        if (error != nullptr) error->clear();
        return true;
    }


// PULSEFORGE_P1_1_16_PSYCH_SPRITE_UTILITY_IMPL_V1
[[nodiscard]] bool script_load_graphic(
    const std::string_view tag,
    const std::string_view image,
    std::string* const error
) {
    auto* sprite = find_script_sprite(tag);
    if (sprite == nullptr || image.empty() || image.size() > 512U) {
        if (error != nullptr) {
            *error = sprite == nullptr
                ? "loadGraphic sprite tag was not found"
                : "loadGraphic image id is outside the bounded scene limits";
        }
        return false;
    }

    const auto texture_index = load_texture(image);
    if (texture_index == no_texture || texture_index >= textures.size()) {
        if (error != nullptr) {
            *error = "loadGraphic could not resolve the image";
        }
        return false;
    }

    const auto& texture = textures[texture_index];
    sprite->texture_index = texture_index;
    sprite->fallback_width = texture.width;
    sprite->fallback_height = texture.height;
    sprite->frame = texture.atlas.empty()
        ? std::optional<AtlasFrame>{}
        : std::optional<AtlasFrame>{texture.atlas.front()};
    // A graphic replacement invalidates frame indexes from the previous atlas.
    // Psych scripts may add fresh animations immediately after loadGraphic().
    sprite->animations.clear();
    sprite->default_animation = no_texture;
    sprite->transient_animation = no_texture;
    sprite->transient_started_ms = 0.0;
    sprite->transient_until_ms = 0.0;
    sprite->allow_textureless_draw = true;
    if (error != nullptr) error->clear();
    return true;
}

[[nodiscard]] bool script_precache_image(
    const std::string_view image,
    std::string* const error
) {
    if (image.empty() || image.size() > 512U) {
        if (error != nullptr) {
            *error = "precacheImage image id is outside the bounded scene limits";
        }
        return false;
    }
    const auto texture_index = load_texture(image);
    const bool ok = texture_index != no_texture && texture_index < textures.size();
    if (error != nullptr) {
        if (ok) error->clear();
        else *error = "precacheImage could not resolve the image";
    }
    return ok;
}

[[nodiscard]] bool script_set_blend_mode(
    const std::string_view tag,
    const std::string_view mode
) noexcept {
    auto* sprite = find_script_sprite(tag);
    if (sprite == nullptr || mode.empty() || mode.size() > 64U) {
        return false;
    }

    const bool supported =
        equals_ascii_insensitive(mode, "normal")
        || equals_ascii_insensitive(mode, "blend")
        || equals_ascii_insensitive(mode, "add")
        || equals_ascii_insensitive(mode, "screen")
        || equals_ascii_insensitive(mode, "multiply")
        || equals_ascii_insensitive(mode, "darken")
        || equals_ascii_insensitive(mode, "mod");
    if (!supported) {
        return false;
    }
    sprite->blend_mode = blend_mode(mode);
    return true;
}

    [[nodiscard]] bool script_make_graphic(
        const std::string_view tag,
        const double width,
        const double height,
        const std::uint8_t r,
        const std::uint8_t g,
        const std::uint8_t b,
        const std::uint8_t a
    ) noexcept {
        auto* sprite = find_script_sprite(tag);
        if (sprite == nullptr || !std::isfinite(width) || !std::isfinite(height)
            || width <= 0.0 || height <= 0.0) {
            return false;
        }
        sprite->texture_index = no_texture;
        sprite->frame.reset();
        sprite->allow_textureless_draw = true;
        sprite->fallback_width = finite_float(width, sprite->fallback_width, 1.0F, 100'000.0F);
        sprite->fallback_height = finite_float(height, sprite->fallback_height, 1.0F, 100'000.0F);
        sprite->color = SDL_Color{r, g, b, a};
        return true;
    }

    [[nodiscard]] bool script_has_sprite(
        const std::string_view tag
    ) const noexcept {
        return find_script_sprite(tag) != nullptr;
    }

    // PULSEFORGE_P1_1_11_WAVY_EFFECT_API_V1
    [[nodiscard]] bool script_set_wavy_effect(
        const std::string_view tag,
        const double amplitude,
        const double frequency,
        const double speed
    ) noexcept {
        auto* sprite = find_script_sprite(tag);
        if (sprite == nullptr) return false;
        sprite->wavy_effect = psych_wavy_effect(amplitude, frequency, speed);
        return true;
    }

    [[nodiscard]] bool script_add_animation(
        const std::string_view tag,
        const std::string_view animation,
        const std::string_view prefix,
        const double fps,
        const bool loop
    ) {
        auto* sprite = find_script_sprite(tag);
        if (sprite == nullptr || animation.empty() || prefix.empty()) return false;
        AnimationDescriptor descriptor;
        descriptor.id = std::string(animation);
        descriptor.name = std::string(prefix);
        descriptor.fps = static_cast<std::int32_t>(
            std::isfinite(fps)
                ? std::clamp(std::lround(fps), 1L, 1'000L)
                : 24L
        );
        descriptor.loop = loop;
        const std::vector<AnimationDescriptor> descriptors{descriptor};
        configure_animations(*sprite, descriptors, animation);
        const auto found = std::find_if(
            sprite->animations.begin(),
            sprite->animations.end(),
            [&](const SceneSprite::AnimationClip& clip) {
                return animation_id_equal(clip.id, animation);
            }
        );
        if (found == sprite->animations.end()) return false;
        sprite->default_animation = static_cast<std::size_t>(
            std::distance(sprite->animations.begin(), found)
        );
        return true;
    }

    [[nodiscard]] bool script_add_sprite(
        const std::string_view tag,
        const bool front
    ) noexcept {
        auto* sprite = find_script_sprite(tag);
        if (sprite == nullptr) return false;
        sprite->visible = true;
        if (front) {
            sprite->order = sprites.empty() ? 0
                : std::max_element(
                    sprites.begin(), sprites.end(),
                    [](const SceneSprite& a, const SceneSprite& b) {
                        return a.order < b.order;
                    }
                )->order + 1;
            normalize_sprite_order();
        }
        return true;
    }

    [[nodiscard]] bool script_remove_sprite(
        const std::string_view tag
    ) noexcept {
        auto* sprite = find_script_sprite(tag);
        if (sprite == nullptr) return false;
        sprite->visible = false;
        return true;
    }

    [[nodiscard]] bool script_set_camera(
        const std::string_view tag,
        const std::string_view camera
    ) noexcept {
        auto* sprite = find_script_sprite(tag);
        if (sprite == nullptr) return false;
        sprite->screen_space = equals_ascii_insensitive(camera, "camHUD")
            || equals_ascii_insensitive(camera, "hud")
            || equals_ascii_insensitive(camera, "camOther")
            || equals_ascii_insensitive(camera, "other");
        return true;
    }

    [[nodiscard]] bool script_set_scroll_factor(
        const std::string_view tag,
        const double x,
        const double y
    ) noexcept {
        auto* sprite = find_script_sprite(tag);
        if (sprite == nullptr || !std::isfinite(x) || !std::isfinite(y)) {
            return false;
        }
        sprite->scroll_factor_x = finite_float(x, sprite->scroll_factor_x, -32.0F, 32.0F);
        sprite->scroll_factor_y = finite_float(y, sprite->scroll_factor_y, -32.0F, 32.0F);
        return true;
    }

    // PULSEFORGE_P1_1_12_PSYCH_SHADER_SCENE_API_V1
    [[nodiscard]] bool script_set_shader(
        const std::string_view tag,
        const std::string_view shader_id
    ) noexcept {
        auto* sprite = find_script_sprite(tag);
        if (sprite == nullptr || shader_id.empty()
            || shader_id.size() > psych_shader_max_identifier_bytes) {
            return false;
        }
        try {
            sprite->shader_id.assign(shader_id);
            sprite->shader_uniforms.clear();
        } catch (...) {
            return false;
        }
        return true;
    }

    [[nodiscard]] bool script_remove_shader(
        const std::string_view tag
    ) noexcept {
        auto* sprite = find_script_sprite(tag);
        if (sprite == nullptr) {
            return false;
        }
        sprite->shader_id.clear();
        sprite->shader_uniforms.clear();
        return true;
    }

    [[nodiscard]] bool script_set_shader_uniform(
        const std::string_view tag,
        const std::string_view uniform,
        const double value
    ) noexcept {
        auto* sprite = find_script_sprite(tag);
        if (sprite == nullptr || sprite->shader_id.empty()
            || uniform.empty()
            || uniform.size() > psych_shader_max_identifier_bytes
            || !std::isfinite(value)) {
            return false;
        }
        const auto found = std::find_if(
            sprite->shader_uniforms.begin(),
            sprite->shader_uniforms.end(),
            [&](const SceneShaderScalarUniform& entry) {
                return equals_ascii_insensitive(entry.name, uniform);
            }
        );
        if (found != sprite->shader_uniforms.end()) {
            found->value = value;
            return true;
        }
        if (sprite->shader_uniforms.size()
            >= psych_shader_max_uniforms_per_sprite) {
            return false;
        }
        try {
            sprite->shader_uniforms.push_back(
                SceneShaderScalarUniform{std::string(uniform), value}
            );
        } catch (...) {
            return false;
        }
        return true;
    }

    [[nodiscard]] bool script_get_shader_uniform(
        const std::string_view tag,
        const std::string_view uniform,
        double& value
    ) const noexcept {
        const auto* sprite = find_script_sprite(tag);
        if (sprite == nullptr || sprite->shader_id.empty()
            || uniform.empty()) {
            return false;
        }
        const auto found = std::find_if(
            sprite->shader_uniforms.begin(),
            sprite->shader_uniforms.end(),
            [&](const SceneShaderScalarUniform& entry) {
                return equals_ascii_insensitive(entry.name, uniform);
            }
        );
        if (found == sprite->shader_uniforms.end()) {
            return false;
        }
        value = found->value;
        return true;
    }

    [[nodiscard]] bool script_set_order(
        const std::string_view tag,
        const std::int64_t order
    ) noexcept {
        auto* sprite = find_script_sprite(tag);
        if (sprite == nullptr) return false;
        sprite->order = std::clamp<std::int64_t>(order, -1'000'000, 1'000'000);
        normalize_sprite_order();
        return true;
    }

    [[nodiscard]] bool script_get_order(
        const std::string_view tag,
        std::int64_t& order
    ) const noexcept {
        const auto* sprite = find_script_sprite(tag);
        if (sprite == nullptr) return false;
        order = sprite->order;
        return true;
    }

    [[nodiscard]] bool script_screen_center(
        const std::string_view tag,
        const bool horizontal,
        const bool vertical
    ) noexcept {
        auto* sprite = find_script_sprite(tag);
        if (sprite == nullptr) return false;
        float width = sprite->fallback_width * sprite->scale_x;
        float height = sprite->fallback_height * sprite->scale_y;
        if (sprite->texture_index != no_texture
            && sprite->texture_index < textures.size()) {
            width = textures[sprite->texture_index].width * sprite->scale_x;
            height = textures[sprite->texture_index].height * sprite->scale_y;
        }
        if (horizontal) sprite->x = (logical_width - width) * 0.5F;
        if (vertical) sprite->y = (logical_height - height) * 0.5F;
        return true;
    }

    [[nodiscard]] bool script_get_animation_name(
        const std::string_view tag,
        const double song_time_ms,
        std::string& animation
    ) const noexcept {
        const auto* sprite = find_script_sprite(tag);
        if (sprite == nullptr) return false;
        const bool transient_active = sprite->transient_animation
                < sprite->animations.size()
            && std::isfinite(song_time_ms)
            && song_time_ms + 0.001 >= sprite->transient_started_ms
            && song_time_ms < sprite->transient_until_ms;
        const std::size_t index = transient_active
            ? sprite->transient_animation
            : sprite->default_animation;
        if (index >= sprite->animations.size()) return false;
        animation = sprite->animations[index].id;
        return true;
    }

    [[nodiscard]] bool script_play_animation(
        const std::string_view tag,
        const std::string_view animation,
        const bool force,
        const double song_time_ms
    ) noexcept {
        auto* sprite = find_script_sprite(tag);
        if (sprite == nullptr || animation.empty() || !std::isfinite(song_time_ms)) return false;
        const auto found = std::find_if(
            sprite->animations.begin(),
            sprite->animations.end(),
            [&](const SceneSprite::AnimationClip& clip) {
                return animation_id_equal(clip.id, animation);
            }
        );
        if (found == sprite->animations.end()) return false;
        const auto index = static_cast<std::size_t>(
            std::distance(sprite->animations.begin(), found)
        );
        if (!force && sprite->transient_animation == index
            && song_time_ms < sprite->transient_until_ms) return true;
        sprite->transient_animation = index;
        sprite->transient_started_ms = song_time_ms;
        const auto& clip = *found;
        const double duration = clip.loop
            ? 60'000.0
            : std::clamp(
                static_cast<double>(clip.frames.size()) / std::max(clip.fps, 1.0)
                    * 1'000.0,
                16.0,
                60'000.0
            );
        sprite->transient_until_ms = song_time_ms + duration;
        return true;
    }

    [[nodiscard]] double game_camera_base_zoom() const noexcept {
        return static_cast<double>(stage_zoom);
    }

    void script_set_game_camera(
        const double x,
        const double y,
        const double zoom,
        const double angle,
        const double alpha
    ) noexcept {
        script_camera_x = finite_float(x, script_camera_x, -100'000.0F, 100'000.0F);
        script_camera_y = finite_float(y, script_camera_y, -100'000.0F, 100'000.0F);
        script_camera_zoom = finite_float(zoom, script_camera_zoom, 0.05F, 8.0F);
        script_camera_angle = std::isfinite(angle)
            ? std::clamp(angle, -360'000.0, 360'000.0)
            : script_camera_angle;
        script_camera_alpha = finite_float(alpha, script_camera_alpha, 0.0F, 1.0F);
    }


    void script_set_hud_camera(
        const double x,
        const double y,
        const double zoom,
        const double angle,
        const double alpha
    ) noexcept {
        script_hud_x = finite_float(x, script_hud_x, -100'000.0F, 100'000.0F);
        script_hud_y = finite_float(y, script_hud_y, -100'000.0F, 100'000.0F);
        script_hud_zoom = finite_float(zoom, script_hud_zoom, 0.05F, 8.0F);
        script_hud_angle = std::isfinite(angle) ? std::clamp(angle, -360'000.0, 360'000.0) : script_hud_angle;
        script_hud_alpha = finite_float(alpha, script_hud_alpha, 0.0F, 1.0F);
    }

    [[nodiscard]] bool handle_visual_event(
        const std::string_view name,
        const std::string_view value1,
        const std::string_view value2,
        const double song_time_ms
    ) noexcept {
        const auto event_number = [](
            const std::string_view text,
            const double fallback
        ) noexcept {
            if (text.empty()) {
                return fallback;
            }
            double parsed{};
  return parse_ascii_floating<double>(text, parsed)
      ? parsed
      : fallback;
        };

        if (equals_ascii_insensitive(name, "Play Animation")
            || equals_ascii_insensitive(name, "PlayAnimation")) {
            std::string_view target = value2;
            if (target.empty() || equals_ascii_insensitive(target, "1")) {
                target = "dad";
            } else if (equals_ascii_insensitive(target, "0")) {
                target = "boyfriend";
            } else if (equals_ascii_insensitive(target, "2")) {
                target = "gf";
            }
            return script_play_animation(
                target,
                value1,
                true,
                std::isfinite(song_time_ms) ? song_time_ms : 0.0
            );
        }

        if (equals_ascii_insensitive(name, "Hey!")) {
            const auto now = std::isfinite(song_time_ms) ? song_time_ms : 0.0;
            if (value1.empty() || equals_ascii_insensitive(value1, "both")) {
                const bool player = script_play_animation(
                    "boyfriend", "hey", true, now
                );
                const bool girlfriend = script_play_animation(
                    "gf", "cheer", true, now
                );
                return player || girlfriend;
            }
            if (equals_ascii_insensitive(value1, "bf")
                || equals_ascii_insensitive(value1, "boyfriend")
                || equals_ascii_insensitive(value1, "0")) {
                return script_play_animation(
                    "boyfriend", "hey", true, now
                );
            }
            if (equals_ascii_insensitive(value1, "gf")
                || equals_ascii_insensitive(value1, "girlfriend")
                || equals_ascii_insensitive(value1, "1")) {
                return script_play_animation("gf", "cheer", true, now);
            }
            return script_play_animation(value1, "hey", true, now);
        }

        // PULSEFORGE_P1_1_5_DYNAMIC_CHANGE_CHARACTER_V1
        if (equals_ascii_insensitive(name, "Change Character")
            || equals_ascii_insensitive(name, "ChangeCharacter")) {
            SpriteRole role = SpriteRole::player;
            if (value1.empty()
                || equals_ascii_insensitive(value1, "0")
                || equals_ascii_insensitive(value1, "bf")
                || equals_ascii_insensitive(value1, "boyfriend")
                || equals_ascii_insensitive(value1, "player")) {
                role = SpriteRole::player;
            } else if (equals_ascii_insensitive(value1, "1")
                       || equals_ascii_insensitive(value1, "dad")
                       || equals_ascii_insensitive(value1, "opponent")) {
                role = SpriteRole::opponent;
            } else if (equals_ascii_insensitive(value1, "2")
                       || equals_ascii_insensitive(value1, "gf")
                       || equals_ascii_insensitive(value1, "girlfriend")) {
                role = SpriteRole::girlfriend;
            } else if (equals_ascii_insensitive(value1, "3")
                       || equals_ascii_insensitive(value1, "player4")
                       || equals_ascii_insensitive(value1, "p4")
                       || equals_ascii_insensitive(value1, "secondaryOpponent")) {
                role = SpriteRole::secondary_opponent;
            } else {
                diagnose(
                    RuntimeSceneDiagnosticSeverity::warning,
                    std::string{"Change Character role is unsupported: "}
                        + std::string(value1)
                );
                return false;
            }
            return replace_character(role, value2);
        }

        if (equals_ascii_insensitive(name, "Add Camera Zoom")) {
            const double game_delta = event_number(value1, 0.015);
            const double hud_delta = event_number(value2, 0.03);
            script_camera_zoom = finite_float(
                static_cast<double>(script_camera_zoom) + game_delta,
                script_camera_zoom,
                0.05F,
                8.0F
            );
            script_hud_zoom = finite_float(
                static_cast<double>(script_hud_zoom) + hud_delta,
                script_hud_zoom,
                0.05F,
                8.0F
            );
            return true;
        }

        if (equals_ascii_insensitive(name, "ZoomCamera")) {
            const double requested_zoom = event_number(
                value1,
                static_cast<double>(script_camera_zoom)
            );
            script_camera_zoom = finite_float(
                requested_zoom, script_camera_zoom, 0.05F, 8.0F
            );
            return true;
        }

        return false;
    }

    void render(
        const float viewport_width,
        const float viewport_height,
        const double beat_position,
        const double song_time_ms
    ) noexcept {
        if (limits.note_skin_only) {
            return;
        }
        if (renderer == nullptr || !std::isfinite(viewport_width)
            || !std::isfinite(viewport_height) || viewport_width <= 0.0F
            || viewport_height <= 0.0F) {
            return;
        }
        cpu_color_pixels_this_frame = 0U;
        const float viewport_scale = std::min(
            viewport_width / logical_width,
            viewport_height / logical_height
        );
        const float viewport_x = (viewport_width - logical_width * viewport_scale)
            * 0.5F;
        const float viewport_y = (viewport_height - logical_height * viewport_scale)
            * 0.5F;

        // A missing or incomplete stage must remain neutral and cheap.  Real
        // stage sprites are rendered on top when their descriptors/assets are
        // available; otherwise this is the explicit black placement backdrop
        // used for compatibility testing and very large charts.
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        const SDL_FRect backdrop{
            viewport_x,
            viewport_y,
            logical_width * viewport_scale,
            logical_height * viewport_scale,
        };
        static_cast<void>(SDL_RenderFillRect(renderer, &backdrop));

        const float pulse = static_cast<float>(character_pulse(beat_position));
        for (const auto& sprite : sprites) {
            if (!sprite.visible) {
                continue;
            }
            render_sprite(
                sprite,
                viewport_scale,
                viewport_x,
                viewport_y,
                pulse,
                song_time_ms
            );
        }
    }
};

RuntimeScene::RuntimeScene(
    SDL_Renderer* renderer,
    const Chart& chart,
    const std::span<const std::filesystem::path> roots,
    RuntimeSceneLimits limits
)
    : implementation_(std::make_unique<Implementation>(
          renderer,
          chart,
          roots,
          limits
      )) {}

RuntimeScene::~RuntimeScene() = default;

bool RuntimeScene::ready() const noexcept {
    return implementation_ != nullptr && implementation_->renderer != nullptr;
}

std::span<const RuntimeSceneDiagnostic> RuntimeScene::diagnostics()
    const noexcept {
    return implementation_ != nullptr
        ? std::span<const RuntimeSceneDiagnostic>(implementation_->diagnostics)
        : std::span<const RuntimeSceneDiagnostic>{};
}

void RuntimeScene::override_note_skin(
    const std::string_view style,
    const bool force_pixel
) {
    if (implementation_ != nullptr) {
        implementation_->override_note_skin(style, force_pixel);
    }
}

std::optional<RuntimeNoteSkinProfile> RuntimeScene::resolve_note_skin_profile(
    const std::string_view style,
    const bool force_pixel
) {
    return implementation_ != nullptr
        ? implementation_->resolve_note_skin_profile(style, force_pixel)
        : std::nullopt;
}

std::optional<RuntimeNoteSplashProfile> RuntimeScene::resolve_note_splash_profile(
    const std::string_view style
) {
    return implementation_ != nullptr
        ? implementation_->resolve_note_splash_profile(style)
        : std::nullopt;
}

std::size_t RuntimeScene::note_splash_frame_count(
    const RuntimeNoteSplashProfile profile
) const noexcept {
    return implementation_ != nullptr
        ? implementation_->note_splash_frame_count(profile)
        : 0U;
}

bool RuntimeScene::render_note_splash(
    const RuntimeNoteSplashDraw& draw
) noexcept {
    return implementation_ != nullptr
        && implementation_->render_note_splash(draw);
}

bool RuntimeScene::note_skin_available(
    const RuntimeNoteSkinElement element,
    const std::uint8_t lane,
    const RuntimeNoteSkinProfile profile
) const noexcept {
    return implementation_ != nullptr
        && implementation_->note_skin_available(element, lane, profile);
}

bool RuntimeScene::render_note_skin(
    const RuntimeNoteSkinDraw& draw
) noexcept {
    return implementation_ != nullptr
        && implementation_->render_note_skin(draw);
}

bool RuntimeScene::render_note_skin_batch(
    const std::span<const RuntimeNoteSkinDraw> draws
) noexcept {
    return implementation_ != nullptr
        && implementation_->render_note_skin_batch(draws);
}

void RuntimeScene::begin_note_skin_profile_frame(const bool enabled) noexcept {
    if (implementation_ != nullptr) {
        implementation_->begin_note_skin_profile_frame(enabled);
    }
}

RuntimeNoteSkinProfileStats RuntimeScene::note_skin_profile_stats()
    const noexcept {
    return implementation_ != nullptr
        ? implementation_->note_skin_profile_stats()
        : RuntimeNoteSkinProfileStats{};
}

void RuntimeScene::notify_note_animation(
    const NoteOwner owner,
    const std::uint16_t lane,
    const double song_time_ms,
    const std::string_view note_kind,
    const bool missed,
    const double sustain_tail_ms
) noexcept {
    if (implementation_ != nullptr) {
        implementation_->notify_note_animation(
            owner,
            lane,
            song_time_ms,
            note_kind,
            missed,
            sustain_tail_ms
        );
    }
}

void RuntimeScene::notify_note_animation_configured(
    const NoteOwner owner,
    const std::uint16_t lane,
    const double song_time_ms,
    const NoteAnimationTarget target,
    const NoteAnimationCue cue,
    const std::string_view suffix,
    const bool missed,
    const double sustain_tail_ms
) noexcept {
    if (implementation_ != nullptr) {
        implementation_->notify_note_animation_configured(
            owner, lane, song_time_ms, target, cue, suffix, missed,
            sustain_tail_ms
        );
    }
}

bool RuntimeScene::script_get_camera_target(
    const std::string_view object,
    double& target_x,
    double& target_y
) const noexcept {
    return implementation_ != nullptr
        && implementation_->script_get_camera_target(
            object, target_x, target_y
        );
}

bool RuntimeScene::script_get_number(
    const std::string_view object,
    const std::string_view property,
    double& value
) const noexcept {
    return implementation_ != nullptr
        && implementation_->script_get_number(object, property, value);
}

bool RuntimeScene::script_set_number(
    const std::string_view object,
    const std::string_view property,
    const double value
) noexcept {
    return implementation_ != nullptr
        && implementation_->script_set_number(object, property, value);
}

bool RuntimeScene::script_set_visible(
    const std::string_view object,
    const bool visible
) noexcept {
    return implementation_ != nullptr
        && implementation_->script_set_visible(object, visible);
}

bool RuntimeScene::script_create_sprite(
    const std::string_view tag,
    const std::string_view image,
    const double x,
    const double y,
    const bool animated,
    std::string* const error
) {
    if (implementation_ == nullptr) {
        if (error != nullptr) *error = "RuntimeScene is unavailable";
        return false;
    }
    return implementation_->script_create_sprite(
        tag, image, x, y, animated, error
    );
}


bool RuntimeScene::script_load_graphic(
    const std::string_view tag,
    const std::string_view image,
    std::string* const error
) {
    return implementation_ != nullptr
        && implementation_->script_load_graphic(tag, image, error);
}

bool RuntimeScene::script_precache_image(
    const std::string_view image,
    std::string* const error
) {
    return implementation_ != nullptr
        && implementation_->script_precache_image(image, error);
}

bool RuntimeScene::script_set_blend_mode(
    const std::string_view tag,
    const std::string_view mode
) noexcept {
    return implementation_ != nullptr
        && implementation_->script_set_blend_mode(tag, mode);
}

bool RuntimeScene::script_make_graphic(
    const std::string_view tag,
    const double width,
    const double height,
    const std::uint8_t r,
    const std::uint8_t g,
    const std::uint8_t b,
    const std::uint8_t a
) noexcept {
    return implementation_ != nullptr
        && implementation_->script_make_graphic(tag, width, height, r, g, b, a);
}

bool RuntimeScene::script_has_sprite(
    const std::string_view tag
) const noexcept {
    return implementation_ != nullptr
        && implementation_->script_has_sprite(tag);
}

bool RuntimeScene::script_set_wavy_effect(
    const std::string_view tag,
    const double amplitude,
    const double frequency,
    const double speed
) noexcept {
    return implementation_ != nullptr
        && implementation_->script_set_wavy_effect(
            tag, amplitude, frequency, speed
        );
}

bool RuntimeScene::script_add_animation(
    const std::string_view tag,
    const std::string_view animation,
    const std::string_view prefix,
    const double fps,
    const bool loop
) {
    return implementation_ != nullptr
        && implementation_->script_add_animation(
            tag, animation, prefix, fps, loop
        );
}

bool RuntimeScene::script_add_sprite(
    const std::string_view tag,
    const bool front
) noexcept {
    return implementation_ != nullptr
        && implementation_->script_add_sprite(tag, front);
}

bool RuntimeScene::script_remove_sprite(
    const std::string_view tag
) noexcept {
    return implementation_ != nullptr
        && implementation_->script_remove_sprite(tag);
}

bool RuntimeScene::script_set_camera(
    const std::string_view tag,
    const std::string_view camera
) noexcept {
    return implementation_ != nullptr
        && implementation_->script_set_camera(tag, camera);
}

bool RuntimeScene::script_set_scroll_factor(
    const std::string_view tag,
    const double x,
    const double y
) noexcept {
    return implementation_ != nullptr
        && implementation_->script_set_scroll_factor(tag, x, y);
}

bool RuntimeScene::script_set_shader(
    const std::string_view tag,
    const std::string_view shader_id
) noexcept {
    return implementation_ != nullptr
        && implementation_->script_set_shader(tag, shader_id);
}

bool RuntimeScene::script_remove_shader(
    const std::string_view tag
) noexcept {
    return implementation_ != nullptr
        && implementation_->script_remove_shader(tag);
}

bool RuntimeScene::script_set_shader_uniform(
    const std::string_view tag,
    const std::string_view uniform,
    const double value
) noexcept {
    return implementation_ != nullptr
        && implementation_->script_set_shader_uniform(tag, uniform, value);
}

bool RuntimeScene::script_get_shader_uniform(
    const std::string_view tag,
    const std::string_view uniform,
    double& value
) const noexcept {
    return implementation_ != nullptr
        && implementation_->script_get_shader_uniform(tag, uniform, value);
}

bool RuntimeScene::script_set_order(
    const std::string_view tag,
    const std::int64_t order
) noexcept {
    return implementation_ != nullptr
        && implementation_->script_set_order(tag, order);
}

bool RuntimeScene::script_get_order(
    const std::string_view tag,
    std::int64_t& order
) const noexcept {
    return implementation_ != nullptr
        && implementation_->script_get_order(tag, order);
}

bool RuntimeScene::script_screen_center(
    const std::string_view tag,
    const bool horizontal,
    const bool vertical
) noexcept {
    return implementation_ != nullptr
        && implementation_->script_screen_center(tag, horizontal, vertical);
}

bool RuntimeScene::script_get_animation_name(
    const std::string_view tag,
    const double song_time_ms,
    std::string& animation
) const noexcept {
    return implementation_ != nullptr
        && implementation_->script_get_animation_name(
            tag, song_time_ms, animation
        );
}

bool RuntimeScene::script_play_animation(
    const std::string_view tag,
    const std::string_view animation,
    const bool force,
    const double song_time_ms
) noexcept {
    return implementation_ != nullptr
        && implementation_->script_play_animation(
            tag, animation, force, song_time_ms
        );
}

double RuntimeScene::game_camera_base_zoom() const noexcept {
    return implementation_ != nullptr
        ? implementation_->game_camera_base_zoom()
        : 1.0;
}

void RuntimeScene::script_set_game_camera(
    const double x,
    const double y,
    const double zoom,
    const double angle,
    const double alpha
) noexcept {
    if (implementation_ != nullptr) {
        implementation_->script_set_game_camera(x, y, zoom, angle, alpha);
    }
}

void RuntimeScene::script_set_hud_camera(
    const double x,
    const double y,
    const double zoom,
    const double angle,
    const double alpha
) noexcept {
    if (implementation_ != nullptr) {
        implementation_->script_set_hud_camera(x, y, zoom, angle, alpha);
    }
}

bool RuntimeScene::handle_visual_event(
    const std::string_view name,
    const std::string_view value1,
    const std::string_view value2,
    const double song_time_ms
) noexcept {
    return implementation_ != nullptr
        && implementation_->handle_visual_event(
            name, value1, value2, song_time_ms
        );
}

void RuntimeScene::release_sustain_animation(
    const NoteOwner owner,
    const double song_time_ms,
    const std::string_view note_kind,
    const double sustain_tail_ms
) noexcept {
    if (implementation_ != nullptr) {
        implementation_->release_sustain_animation(
            owner, song_time_ms, note_kind, sustain_tail_ms
        );
    }
}

void RuntimeScene::release_sustain_animation_configured(
    const NoteOwner owner,
    const double song_time_ms,
    const NoteAnimationTarget target,
    const double sustain_tail_ms
) noexcept {
    if (implementation_ != nullptr) {
        implementation_->release_sustain_animation_configured(
            owner, song_time_ms, target, sustain_tail_ms
        );
    }
}

void RuntimeScene::render(
    const float viewport_width,
    const float viewport_height,
    const double beat_position,
    const double song_time_ms
) noexcept {
    if (implementation_ != nullptr) {
        implementation_->render(
            viewport_width,
            viewport_height,
            beat_position,
            song_time_ms
        );
    }
}

}  // namespace pulseforge::detail
