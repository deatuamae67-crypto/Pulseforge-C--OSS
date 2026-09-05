#pragma once

#include "pulseforge/chart.hpp"
#include "pulseforge/note_types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Renderer;

namespace pulseforge::detail {

// Hard limits are applied before decoding so an untrusted mod cannot turn a
// small descriptor or compressed image into unbounded CPU/GPU memory use.
struct RuntimeSceneLimits {
    std::size_t maximum_roots{16U};
    std::uintmax_t maximum_descriptor_bytes{2U * 1024U * 1024U};
    // PULSEFORGE_P1_1_9_GENERIC_LARGE_TEXTURE_POLICY_V1
    // Encoded-file, per-texture decoded, and scene-resident budgets
    // are deliberately separate. This admits large Psych character
    // atlases without making the scene budget unbounded.
    std::uintmax_t maximum_image_bytes{256ULL * 1024ULL * 1024ULL};
    std::uintmax_t maximum_atlas_bytes{4U * 1024U * 1024U};
    std::uint32_t maximum_image_dimension{8'192U};
    // 8192x8192 is the configured per-axis compatibility ceiling.
    // Keep the pixel ceiling coherent with it: one maximum-size RGBA
    // texture is 64 MiPixels / 256 MiB decoded.
    std::uint64_t maximum_image_pixels{64ULL * 1024ULL * 1024ULL};
    std::uint64_t maximum_decoded_texture_bytes_per_texture{
        256ULL * 1024ULL * 1024ULL
    };
    // Scene-wide resident decoded-texture budget. This is deliberately
    // separate from the per-texture pixel ceiling so a legal 8192x8192
    // character atlas does not consume the entire scene budget by itself.
    std::uint64_t maximum_decoded_texture_bytes{1ULL * 1024ULL * 1024ULL * 1024ULL};
    std::size_t maximum_textures{256U};
    std::size_t maximum_sprites{1'024U};
    std::size_t maximum_atlas_frames{16'384U};
    std::size_t maximum_animation_clips{4'096U};
    std::size_t maximum_animation_frame_references{65'536U};
    std::size_t maximum_diagnostics{128U};

    // Maximum-performance mode can request only the note-skin subsystem.
    // RuntimeScene still uses its original constructor ABI; this flag merely
    // skips stage/character sprite construction and stage rendering.
    bool note_skin_only{false};
};

enum class RuntimeSceneDiagnosticSeverity : std::uint8_t {
    warning,
    error,
};

struct RuntimeSceneDiagnostic {
    RuntimeSceneDiagnosticSeverity severity{
        RuntimeSceneDiagnosticSeverity::warning
    };
    std::string message;
};

// Four-lane FNF/Psych note skins expose one static frame for each gameplay
// element.  Animation state (pressed/confirm/splash) deliberately remains a
// gameplay concern and may be added without changing this draw contract.
enum class RuntimeNoteSkinElement : std::uint8_t {
    receptor,
    note_head,
    sustain_body,
    sustain_end,
};

struct RuntimeLogicalRect {
    float x{};
    float y{};
    float width{};
    float height{};
};

using RuntimeNoteSkinProfile = std::uint16_t;
inline constexpr RuntimeNoteSkinProfile runtime_note_skin_default_profile = 0U;

struct RuntimeNoteSkinDraw {
    RuntimeNoteSkinElement element{RuntimeNoteSkinElement::note_head};
    std::uint8_t lane{};
    RuntimeLogicalRect destination;
    std::uint8_t alpha{255U};
    bool flip_vertical{};
    double angle{};
    // PULSEFORGE_P1_5_0B_BOUNDED_NOTE_SKIN_PROFILE_DRAW_V1
    // Zero is the chart/default skin. Non-zero handles are resolved once by
    // RuntimeScene and remain bounded; no asset lookup occurs in the hot path.
    RuntimeNoteSkinProfile profile{runtime_note_skin_default_profile};
    // PULSEFORGE_P1_5_0C_NOTE_SKIN_TINT_DRAW_V1
    // Per-draw modulation uses SDL texture/vertex colour, so custom RGB does
    // not duplicate texture resources or create per-note GPU allocations.
    std::array<std::uint8_t, 3U> rgb{255U, 255U, 255U};
};

using RuntimeNoteSplashProfile = std::uint16_t;
inline constexpr RuntimeNoteSplashProfile runtime_note_splash_invalid_profile = 0U;

struct RuntimeNoteSplashDraw {
    RuntimeNoteSplashProfile profile{runtime_note_splash_invalid_profile};
    std::uint32_t frame_index{};
    RuntimeLogicalRect destination;
    std::uint8_t alpha{255U};
    double angle{};
};
// Per-frame instrumentation for the textured note-skin hot path. Timing is
// collected only while diagnostics/profiling is enabled, so normal gameplay
// pays effectively no profiling-clock overhead.
struct RuntimeNoteSkinProfileStats {
    std::uint64_t batch_total_ns{};
    std::uint64_t geometry_submit_ns{};
    std::uint64_t fallback_ns{};
    std::uint64_t quads{};
    std::uint64_t fallback_draws{};
    std::uint32_t submissions{};
    std::uint32_t failed_submissions{};
};

// roots are ordered from lowest to highest precedence. They are snapshotted
// by the secure VFS during construction; later filesystem changes never alter
// lookup order. All loading happens once in the constructor. render() is a
// main-thread, allocation-free hot path.
class RuntimeScene final {
public:
    RuntimeScene(
        SDL_Renderer* renderer,
        const Chart& chart,
        std::span<const std::filesystem::path> roots,
        RuntimeSceneLimits limits = {}
    );
    ~RuntimeScene();

    RuntimeScene(const RuntimeScene&) = delete;
    RuntimeScene& operator=(const RuntimeScene&) = delete;
    RuntimeScene(RuntimeScene&&) = delete;
    RuntimeScene& operator=(RuntimeScene&&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::span<const RuntimeSceneDiagnostic> diagnostics()
        const noexcept;

    // Applies a user-selected visual note skin after scene construction.
    // This deliberately keeps the original RuntimeScene constructor ABI
    // unchanged. It is an initialization-time operation, never a per-frame
    // hot-path call.
    void override_note_skin(
        std::string_view style,
        bool force_pixel
    );
    // PULSEFORGE_P1_5_0B_BOUNDED_NOTE_SKIN_PROFILE_API_V1
    // Resolve a Psych note texture into a small scene-local profile handle.
    // Profiles are cached and capped internally; no per-note asset lookup.
    [[nodiscard]] std::optional<RuntimeNoteSkinProfile> resolve_note_skin_profile(
        std::string_view style,
        bool force_pixel = false
    );

    // PULSEFORGE_P1_5_0C_BOUNDED_NOTE_SPLASH_PROFILE_API_V1
    // Custom noteSplashTexture assets are resolved once into scene-local atlas
    // handles. The profile count and atlas-frame count remain independently
    // bounded by RuntimeScene.
    [[nodiscard]] std::optional<RuntimeNoteSplashProfile>
        resolve_note_splash_profile(std::string_view style);
    [[nodiscard]] std::size_t note_splash_frame_count(
        RuntimeNoteSplashProfile profile
    ) const noexcept;
    [[nodiscard]] bool render_note_splash(
        const RuntimeNoteSplashDraw& draw
    ) noexcept;

    // These calls never allocate and are safe to use in the per-note render
    // loop.  A false result is intentional: the caller must retain its cheap
    // geometric fallback for unsafe, missing, incomplete, or non-four-lane
    // skins.  destination is expressed in the renderer's logical coordinates.
    [[nodiscard]] bool note_skin_available(
        RuntimeNoteSkinElement element,
        std::uint8_t lane,
        RuntimeNoteSkinProfile profile = runtime_note_skin_default_profile
    ) const noexcept;
    [[nodiscard]] bool render_note_skin(
        const RuntimeNoteSkinDraw& draw
    ) noexcept;

    // Batches many note-skin quads into a small number of SDL geometry
    // submissions. This is used by dense/exact note rendering so custom skins
    // keep the same bounded real-time paths as geometric notes.
    [[nodiscard]] bool render_note_skin_batch(
        std::span<const RuntimeNoteSkinDraw> draws
    ) noexcept;

    // F3 diagnostics enables this instrumentation for the current frame.
    // SDL_RenderGeometry timing measures CPU/driver submission latency, not a
    // hardware GPU timestamp; present/wait is measured separately by the app.
    void begin_note_skin_profile_frame(bool enabled) noexcept;
    [[nodiscard]] RuntimeNoteSkinProfileStats note_skin_profile_stats()
        const noexcept;

    // Switches the matching player/opponent/secondary-opponent atlas to a sing
    // (or miss) animation. Sustain notes may extend the pose to their tail. The lookup is allocation-free after the event and falls back
    // silently when a mod character does not define that direction.
    void notify_note_animation(
        NoteOwner owner,
        std::uint16_t lane,
        double song_time_ms,
        std::string_view note_kind = {},
        bool missed = false,
        double sustain_tail_ms = -1.0
    ) noexcept;

    // PULSEFORGE_P1_5_0C_DECLARATIVE_NOTE_ANIMATION_API_V1
    // This variant consumes the already-parsed NoteTypeDefinition fields. It
    // preserves the legacy helper above for existing callers/built-in tests.
    void notify_note_animation_configured(
        NoteOwner owner,
        std::uint16_t lane,
        double song_time_ms,
        NoteAnimationTarget target,
        NoteAnimationCue cue,
        std::string_view suffix,
        bool missed = false,
        double sustain_tail_ms = -1.0
    ) noexcept;

    // Cancels one previously registered sustain-pose lock (normally on a
    // player hold_drop). The tail timestamp identifies the exact sustain while
    // preserving other overlapping holds for the same character.
    void release_sustain_animation(
        NoteOwner owner,
        double song_time_ms,
        std::string_view note_kind,
        double sustain_tail_ms
    ) noexcept;
    void release_sustain_animation_configured(
        NoteOwner owner,
        double song_time_ms,
        NoteAnimationTarget target,
        double sustain_tail_ms
    ) noexcept;

    // Bounded live-modchart surface used by the Psych Lua compatibility host.
    // Names are logical sprite/character tags; no filesystem path or SDL
    // pointer is ever exposed to a script. These operations may allocate only
    // during explicit create/add calls (normally onCreate), never while render()
    // walks the scene.
    // PULSEFORGE_P1_1_10_CAMERA_TARGET_API_V1
    [[nodiscard]] bool script_get_camera_target(
        std::string_view object,
        double& target_x,
        double& target_y
    ) const noexcept;

    [[nodiscard]] bool script_get_number(
        std::string_view object,
        std::string_view property,
        double& value
    ) const noexcept;
    [[nodiscard]] bool script_set_number(
        std::string_view object,
        std::string_view property,
        double value
    ) noexcept;
    [[nodiscard]] bool script_set_visible(
        std::string_view object,
        bool visible
    ) noexcept;
    [[nodiscard]] bool script_create_sprite(
        std::string_view tag,
        std::string_view image,
        double x,
        double y,
        bool animated,
        std::string* error = nullptr
    );
    // PULSEFORGE_P1_1_16_PSYCH_SPRITE_UTILITY_API_V1
    [[nodiscard]] bool script_load_graphic(
        std::string_view tag,
        std::string_view image,
        std::string* error = nullptr
    );
    [[nodiscard]] bool script_precache_image(
        std::string_view image,
        std::string* error = nullptr
    );
    [[nodiscard]] bool script_set_blend_mode(
        std::string_view tag,
        std::string_view mode
    ) noexcept;
    [[nodiscard]] bool script_add_animation(
        std::string_view tag,
        std::string_view animation,
        std::string_view prefix,
        double fps,
        bool loop
    );
    [[nodiscard]] bool script_make_graphic(
        std::string_view tag,
        double width,
        double height,
        std::uint8_t r,
        std::uint8_t g,
        std::uint8_t b,
        std::uint8_t a = 255U
    ) noexcept;
    [[nodiscard]] bool script_has_sprite(
        std::string_view tag
    ) const noexcept;
    // PULSEFORGE_P1_1_11_WAVY_EFFECT_API_V1
    [[nodiscard]] bool script_set_wavy_effect(
        std::string_view tag,
        double amplitude,
        double frequency,
        double speed
    ) noexcept;
    [[nodiscard]] bool script_add_sprite(
        std::string_view tag,
        bool front
    ) noexcept;
    [[nodiscard]] bool script_remove_sprite(
        std::string_view tag
    ) noexcept;
    [[nodiscard]] bool script_set_camera(
        std::string_view tag,
        std::string_view camera
    ) noexcept;
    [[nodiscard]] bool script_set_scroll_factor(
        std::string_view tag,
        double x,
        double y
    ) noexcept;
    // Bounded Psych-style shader binding. Shader sources are validated by
    // ShaderCatalog in the application host; RuntimeScene stores only the
    // validated identity and scalar uniforms needed by renderer-native fallbacks.
    [[nodiscard]] bool script_set_shader(
        std::string_view tag,
        std::string_view shader_id
    ) noexcept;
    [[nodiscard]] bool script_remove_shader(
        std::string_view tag
    ) noexcept;
    [[nodiscard]] bool script_set_shader_uniform(
        std::string_view tag,
        std::string_view uniform,
        double value
    ) noexcept;
    [[nodiscard]] bool script_get_shader_uniform(
        std::string_view tag,
        std::string_view uniform,
        double& value
    ) const noexcept;

    [[nodiscard]] bool script_set_order(
        std::string_view tag,
        std::int64_t order
    ) noexcept;
    [[nodiscard]] bool script_get_order(
        std::string_view tag,
        std::int64_t& order
    ) const noexcept;
    [[nodiscard]] bool script_screen_center(
        std::string_view tag,
        bool horizontal,
        bool vertical
    ) noexcept;
    // Restricted Psych callMethod compatibility: exposes only the
    // resolved animation identifier for a known scene sprite/character.
    [[nodiscard]] bool script_get_animation_name(
        std::string_view tag,
        double song_time_ms,
        std::string& animation
    ) const noexcept;
    [[nodiscard]] bool script_play_animation(
        std::string_view tag,
        std::string_view animation,
        bool force,
        double song_time_ms
    ) noexcept;
    void script_set_game_camera(
        double x,
        double y,
        double zoom,
        double angle,
        double alpha
    ) noexcept;
    // Base world-camera zoom from the active stage descriptor. Third-strum
    // rendering uses this to share camGame geometry with player4 without
    // exposing any SDL/runtime internals.
    [[nodiscard]] double game_camera_base_zoom() const noexcept;
    void script_set_hud_camera(
        double x,
        double y,
        double zoom,
        double angle,
        double alpha
    ) noexcept;

    // Compatibility/event bridge used by runtime tests and chart/Lua visual
    // events.  Keeping this on RuntimeScene avoids forcing the application to
    // know how character aliases map to concrete scene sprites.
    bool handle_visual_event(
        std::string_view name,
        std::string_view value1 = {},
        std::string_view value2 = {},
        double song_time_ms = 0.0
    ) noexcept;

    // viewport dimensions use SDL render coordinates. beat_position may be
    // fractional and drives a deterministic, subtle scale pulse on characters;
    // song_time_ms selects atlas animation frames without per-frame allocation.
    void render(
        float viewport_width,
        float viewport_height,
        double beat_position,
        double song_time_ms
    ) noexcept;

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

}  // namespace pulseforge::detail
