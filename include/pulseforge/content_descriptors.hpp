#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulseforge {

enum class DescriptorParseMode : std::uint8_t {
    permissive,
    strict,
};

enum class DescriptorDiagnosticSeverity : std::uint8_t {
    warning,
    error,
};

enum class DescriptorDiagnosticCode : std::uint8_t {
    invalid_limits,
    input_too_large,
    invalid_json,
    root_not_object,
    nesting_too_deep,
    node_limit_exceeded,
    array_too_large,
    object_too_large,
    string_too_long,
    missing_required_field,
    wrong_type,
    invalid_value,
    preserved_json_too_large,
};

struct DescriptorDiagnostic {
    DescriptorDiagnosticSeverity severity{
        DescriptorDiagnosticSeverity::error
    };
    DescriptorDiagnosticCode code{DescriptorDiagnosticCode::invalid_json};
    std::string path;
    std::string message;
};

struct DescriptorParseLimits {
    std::size_t max_input_bytes{2U * 1024U * 1024U};
    std::size_t max_string_bytes{16U * 1024U};
    std::size_t max_array_items{4'096U};
    std::size_t max_object_members{4'096U};
    std::size_t max_total_nodes{100'000U};
    std::size_t max_depth{64U};
    std::size_t max_preserved_json_bytes{256U * 1024U};
    std::size_t max_diagnostics{128U};
};

struct DescriptorParseOptions {
    DescriptorParseMode mode{DescriptorParseMode::permissive};
    DescriptorParseLimits limits;
};

template <typename Descriptor>
struct DescriptorParseResult {
    std::optional<Descriptor> value;
    std::vector<DescriptorDiagnostic> diagnostics;
    bool diagnostics_truncated{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return value.has_value();
    }
};

struct DescriptorVec2 {
    double x{};
    double y{};
};

struct DescriptorRgb {
    std::int32_t red{255};
    std::int32_t green{255};
    std::int32_t blue{255};
};

struct NamedOffsetDescriptor {
    std::string id;
    double offset{};
};

struct AlternateVocalOffsetsDescriptor {
    std::string variation;
    std::vector<NamedOffsetDescriptor> vocals;
};

struct SongOffsetsDescriptor {
    double instrumental{};
    std::vector<NamedOffsetDescriptor> alternate_instrumentals;
    std::vector<NamedOffsetDescriptor> vocals;
    std::vector<AlternateVocalOffsetsDescriptor> alternate_vocals;
};

struct SongCharactersDescriptor {
    std::string player;
    std::string girlfriend;
    std::string opponent;
    std::string instrumental;
    std::vector<std::string> alternate_instrumentals;
    std::vector<std::string> player_vocals;
    std::vector<std::string> opponent_vocals;
};

// V-Slice song metadata only. Chart notes and events intentionally do not live
// in this descriptor and can therefore remain unloaded while browsing menus.
struct SongDescriptor {
    std::string id;
    std::string version;
    std::string name;
    std::string artist;
    std::string charter;
    std::vector<std::string> difficulties;
    std::vector<std::string> variations;
    std::string stage;
    std::string note_style;
    SongCharactersDescriptor characters;
    std::optional<double> preview_start;
    std::optional<double> preview_end;
    SongOffsetsDescriptor offsets;
    // Canonical compact JSON containing unmodelled root/nested metadata.
    std::string extensions_json;
};

struct LevelBackgroundDescriptor {
    std::optional<std::string> color;
    std::optional<std::string> image;
};

struct LevelDescriptor {
    std::string id;
    std::string version;
    std::string name;
    bool visible{true};
    std::vector<std::string> songs;
    std::optional<LevelBackgroundDescriptor> background;
    std::string extensions_json;
};

struct WeekSongDescriptor {
    std::string name;
    std::string character;
    DescriptorRgb color{146, 113, 253};
    std::string extensions_json;
};

// Psych Engine week metadata. An empty difficulties vector means that the
// engine's standard difficulty set should be used.
struct WeekDescriptor {
    std::string id;
    std::vector<WeekSongDescriptor> songs;
    std::array<std::string, 3U> characters{"dad", "bf", "gf"};
    std::string background{"stage"};
    std::string previous_week;
    std::string story_name;
    std::string display_name;
    bool start_unlocked{true};
    bool hidden_until_unlocked{};
    bool hide_story{};
    bool hide_freeplay{};
    std::vector<std::string> difficulties;
    std::string extensions_json;
};

struct AnimationDescriptor {
    std::string id;
    std::string name;
    std::int32_t fps{24};
    bool loop{};
    std::vector<std::int32_t> indices;
    DescriptorVec2 offsets;
    std::string extensions_json;
};

enum class StageObjectLayer : std::uint8_t {
    background,
    behind_girlfriend,
    behind_opponent,
    behind_player,
    foreground,
};

struct StageObjectDescriptor {
    std::string type;
    std::string name;
    std::string image;
    double x{};
    double y{};
    double width{256.0};
    double height{160.0};
    DescriptorVec2 scale{1.0, 1.0};
    DescriptorVec2 scroll{1.0, 1.0};
    double alpha{1.0};
    double angle{};
    std::string color{"FFFFFF"};
    bool antialiasing{true};
    bool flip_x{};
    bool flip_y{};
    // Psych addLuaSprite(tag, true) semantics: draw after characters.
    bool foreground{};
    // Keeps the relative insertion semantics of addBehindGF/Dad/BF.  The
    // legacy foreground flag remains for existing native descriptors.
    StageObjectLayer layer{StageObjectLayer::background};
    bool screen_space{};
    std::string blend{"normal"};
    std::int32_t filters{};
    std::string first_animation;
    std::vector<AnimationDescriptor> animations;
    std::string extensions_json;
};

struct StageDescriptor {
    std::string id;
    std::string directory;
    double default_zoom{0.9};
    bool pixel_stage{};
    std::string stage_ui{"normal"};
    DescriptorVec2 boyfriend{770.0, 100.0};
    DescriptorVec2 girlfriend{400.0, 130.0};
    DescriptorVec2 opponent{100.0, 100.0};
    // PULSEFORGE_P1_4_0_DENPA_STAGE_P4_V1
    // DenpaEx stage JSON defines an independent second-opponent anchor.
    DescriptorVec2 p4{0.0, 0.0};
    bool hide_girlfriend{};
    DescriptorVec2 camera_boyfriend;
    DescriptorVec2 camera_girlfriend;
    DescriptorVec2 camera_opponent;
    DescriptorVec2 camera_p4;
    double camera_speed{1.0};
    std::vector<StageObjectDescriptor> objects;
    std::string preload_json;
    std::string extensions_json;
};

struct CharacterDescriptor {
    std::string id;
    std::string image;
    double scale{1.0};
    double sing_duration{4.0};
    std::string health_icon{"face"};
    DescriptorVec2 position;
    DescriptorVec2 camera_position;
    bool flip_x{};
    bool no_antialiasing{};
    DescriptorRgb healthbar_color{161, 161, 161};
    std::string vocals_file;
    std::optional<bool> editor_is_player;
    std::vector<AnimationDescriptor> animations;
    std::string extensions_json;
};

class ContentDescriptorParser final {
public:
    [[nodiscard]] static DescriptorParseResult<SongDescriptor>
    parse_vslice_song(
        std::string_view json_text,
        std::string_view id,
        const DescriptorParseOptions& options = {}
    );

    [[nodiscard]] static DescriptorParseResult<LevelDescriptor>
    parse_vslice_level(
        std::string_view json_text,
        std::string_view id,
        const DescriptorParseOptions& options = {}
    );

    [[nodiscard]] static DescriptorParseResult<WeekDescriptor>
    parse_psych_week(
        std::string_view json_text,
        std::string_view id,
        const DescriptorParseOptions& options = {}
    );

    [[nodiscard]] static DescriptorParseResult<StageDescriptor>
    parse_psych_stage(
        std::string_view json_text,
        std::string_view id,
        const DescriptorParseOptions& options = {}
    );

    [[nodiscard]] static DescriptorParseResult<CharacterDescriptor>
    parse_psych_character(
        std::string_view json_text,
        std::string_view id,
        const DescriptorParseOptions& options = {}
    );
};

[[nodiscard]] std::string_view to_string(
    DescriptorDiagnosticSeverity severity
) noexcept;
[[nodiscard]] std::string_view to_string(
    DescriptorDiagnosticCode code
) noexcept;

}  // namespace pulseforge
