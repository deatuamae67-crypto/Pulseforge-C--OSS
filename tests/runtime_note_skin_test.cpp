#include "runtime_scene.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using pulseforge::Chart;
using pulseforge::NoteOwner;
using pulseforge::NoteAnimationCue;
using pulseforge::NoteAnimationTarget;
using pulseforge::detail::RuntimeLogicalRect;
using pulseforge::detail::RuntimeNoteSkinDraw;
using pulseforge::detail::RuntimeNoteSkinElement;
using pulseforge::detail::RuntimeScene;
using pulseforge::detail::RuntimeSceneLimits;

constexpr std::string_view png_4x5_base64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAQAAAAFCAYAAABirU3bAAAAAXNSR0IArs4c6QAAAARn"
    "QU1BAACxjwv8YQUAAAAJcEhZcwAADsMAAA7DAcdvqGQAAABASURBVBhXFcgxEQAwCAAx"
    "HDFhpwLwggZM/Fhv32vGRARWYAduYERiJXbi5o+DdbAP7vkxWIM9uPPjYl3si3vxATuu"
    "L4tNJ+KxAAAAAElFTkSuQmCC";
constexpr std::string_view png_4x2_base64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAQAAAACCAYAAAB/qH1jAAAAAXNSR0IArs4c6QAAAARn"
    "QU1BAACxjwv8YQUAAAAJcEhZcwAADsMAAA7DAcdvqGQAAAAlSURBVBhXBcExAQAwDIAw"
    "HPWaqZqqCfyxBLCHLXYYjL2xHbuxDwTPENFtSENkAAAAAElFTkSuQmCC";
constexpr std::string_view png_4x1_colors_base64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAQAAAABCAYAAAD5PA/NAAAAEklEQVR4nGP4z8DwHwwZ"
    "/oMBAEXLCfcNg/1WAAAAAElFTkSuQmCC";

[[noreturn]] void fail(const std::string_view message) {
    throw std::runtime_error(std::string(message));
}

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

[[nodiscard]] int base64_value(const char value) noexcept {
    if (value >= 'A' && value <= 'Z') {
        return value - 'A';
    }
    if (value >= 'a' && value <= 'z') {
        return value - 'a' + 26;
    }
    if (value >= '0' && value <= '9') {
        return value - '0' + 52;
    }
    if (value == '+') {
        return 62;
    }
    if (value == '/') {
        return 63;
    }
    return -1;
}

[[nodiscard]] std::vector<std::byte> decode_base64(
    const std::string_view encoded
) {
    std::vector<std::byte> decoded;
    decoded.reserve(encoded.size() * 3U / 4U);
    std::uint32_t accumulator{};
    int available_bits{-8};
    for (const char character : encoded) {
        if (character == '=') {
            break;
        }
        const int value = base64_value(character);
        require(value >= 0, "invalid embedded base64 fixture");
        accumulator = (accumulator << 6U)
            | static_cast<std::uint32_t>(value);
        available_bits += 6;
        if (available_bits >= 0) {
            decoded.push_back(std::byte{
                static_cast<unsigned char>(
                    (accumulator >> static_cast<unsigned int>(available_bits))
                    & 0xFFU
                )
            });
            available_bits -= 8;
        }
    }
    return decoded;
}

void write_binary(
    const std::filesystem::path& path,
    const std::string_view encoded
) {
    std::filesystem::create_directories(path.parent_path());
    const auto bytes = decode_base64(encoded);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(output.good(), "could not create PNG fixture");
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );
    require(output.good(), "could not write PNG fixture");
}

void write_text(
    const std::filesystem::path& path,
    const std::string_view text
) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(output.good(), "could not create text fixture");
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(output.good(), "could not write text fixture");
}

class TemporaryDirectory final {
public:
    explicit TemporaryDirectory(const std::string_view label) {
        const auto unique = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
            / ("pulseforge-note-skin-" + std::string(label) + '-'
                + std::to_string(unique));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class SdlFixture final {
public:
    SdlFixture() {
        require(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
        surface_ = SDL_CreateSurface(1'280, 720, SDL_PIXELFORMAT_RGBA32);
        require(surface_ != nullptr, SDL_GetError());
        renderer_ = SDL_CreateSoftwareRenderer(surface_);
        require(renderer_ != nullptr, SDL_GetError());
    }

    ~SdlFixture() {
        SDL_DestroyRenderer(renderer_);
        SDL_DestroySurface(surface_);
        SDL_Quit();
    }

    SdlFixture(const SdlFixture&) = delete;
    SdlFixture& operator=(const SdlFixture&) = delete;

    [[nodiscard]] SDL_Renderer* renderer() const noexcept {
        return renderer_;
    }

    [[nodiscard]] std::array<std::uint8_t, 4U> pixel(
        const int x,
        const int y
    ) const {
        static_cast<void>(SDL_RenderPresent(renderer_));
        std::array<std::uint8_t, 4U> value{};
        require(
            SDL_ReadSurfacePixel(
                surface_,
                x,
                y,
                &value[0U],
                &value[1U],
                &value[2U],
                &value[3U]
            ),
            SDL_GetError()
        );
        return value;
    }

private:
    SDL_Surface* surface_{};
    SDL_Renderer* renderer_{};
};

[[nodiscard]] std::string complete_atlas_xml() {
    constexpr std::array receptor_names{
        std::string_view{"ArRoWlEfT0000"},
        std::string_view{"ARROWDOWN0000"},
        std::string_view{"arrowUP0000"},
        std::string_view{"ArrowRight0000"},
    };
    constexpr std::array head_names{
        std::string_view{"PuRpLe0000"},
        std::string_view{"BLUE0000"},
        std::string_view{"green0000"},
        std::string_view{"Red0000"},
    };
    constexpr std::array body_names{
        std::string_view{"PURPLE HOLD PIECE0000"},
        std::string_view{"Blue Hold Piece0000"},
        std::string_view{"green hold piece0000"},
        std::string_view{"red hold piece0000"},
    };
    constexpr std::array end_names{
        std::string_view{"Purple Hold End0000"},
        std::string_view{"BLUE HOLD END0000"},
        std::string_view{"green hold end0000"},
        std::string_view{"Red Hold End0000"},
    };
    constexpr std::array name_sets{
        receptor_names,
        head_names,
        body_names,
        end_names,
    };

    std::string xml{"<TextureAtlas>\n"};
    for (std::size_t row = 0U; row < name_sets.size(); ++row) {
        for (std::size_t lane = 0U; lane < name_sets[row].size(); ++lane) {
            xml.append("<SubTexture name=\"");
            xml.append(name_sets[row][lane]);
            xml.append("\" x=\"");
            xml.append(std::to_string(lane));
            xml.append("\" y=\"");
            xml.append(std::to_string(row));
            xml.append("\" width=\"1\" height=\"1\"/>\n");
        }
    }
    xml.append("</TextureAtlas>\n");
    return xml;
}


[[nodiscard]] std::string splash_atlas_xml() {
    return R"xml(<TextureAtlas>
<SubTexture name="note splash purple 0000" x="0" y="0" width="1" height="1"/>
<SubTexture name="note splash blue 0000" x="1" y="0" width="1" height="1"/>
<SubTexture name="note splash green 0000" x="2" y="0" width="1" height="1"/>
<SubTexture name="note splash red 0000" x="3" y="0" width="1" height="1"/>
</TextureAtlas>
)xml";
}

[[nodiscard]] Chart four_lane_chart(const std::string_view style) {
    Chart chart;
    chart.key_count = 4U;
    chart.note_style = std::string(style);
    return chart;
}

[[nodiscard]] std::array<std::filesystem::path, 1U> scene_roots(
    const TemporaryDirectory& directory
) {
    return {directory.path()};
}

void require_complete_skin(RuntimeScene& scene) {
    constexpr std::array elements{
        RuntimeNoteSkinElement::receptor,
        RuntimeNoteSkinElement::note_head,
        RuntimeNoteSkinElement::sustain_body,
        RuntimeNoteSkinElement::sustain_end,
    };
    for (const auto element : elements) {
        for (std::uint8_t lane = 0U; lane < 4U; ++lane) {
            require(
                scene.note_skin_available(element, lane),
                "expected complete note-skin frame"
            );
            require(
                scene.render_note_skin(RuntimeNoteSkinDraw{
                    element,
                    lane,
                    RuntimeLogicalRect{
                        20.0F + static_cast<float>(lane) * 24.0F,
                        30.0F,
                        20.0F,
                        24.0F,
                    },
                    192U,
                    element == RuntimeNoteSkinElement::sustain_end,
                }),
                "complete note-skin frame should render"
            );
        }
    }
}

void test_character_animation_events_are_safe(SDL_Renderer* renderer) {
    TemporaryDirectory directory("character-events");
    auto chart = four_lane_chart({});
    chart.player_character = "missing-player";
    chart.opponent_character = "missing-opponent";
    chart.girlfriend_character = "missing-gf";
    const auto roots = scene_roots(directory);
    RuntimeScene scene(renderer, chart, roots);
    scene.notify_note_animation(NoteOwner::player, 0U, 1'000.0, "normal");
    scene.notify_note_animation(
        NoteOwner::player, 1U, 1'100.0, "Alt Animation"
    );
    scene.notify_note_animation(
        NoteOwner::opponent, 2U, 1'200.0, "Hurt Note", true
    );
    scene.notify_note_animation(NoteOwner::player, 3U, 1'300.0, "GF Sing");
    scene.notify_note_animation(
        NoteOwner::player, 0U, 1'400.0, "No Animation"
    );
    scene.notify_note_animation_configured(
        NoteOwner::player, 0U, 1'410.0, NoteAnimationTarget::player,
        NoteAnimationCue::sing, "-alt"
    );
    scene.notify_note_animation_configured(
        NoteOwner::opponent, 1U, 1'420.0, NoteAnimationTarget::girlfriend,
        NoteAnimationCue::hey, {}
    );
    scene.notify_note_animation_configured(
        NoteOwner::player, 2U, 1'430.0, NoteAnimationTarget::owner,
        NoteAnimationCue::none, {}
    );
    scene.release_sustain_animation_configured(
        NoteOwner::player, 1'440.0, NoteAnimationTarget::player, 2'000.0
    );
    scene.render(1'280.0F, 720.0F, 4.5, 1'450.0);
    require(scene.ready(), "missing animation assets must retain safe scene");
}

void test_stage_json_and_static_lua_are_composed(SdlFixture& sdl) {
    TemporaryDirectory directory("stage-json-lua");
    write_text(
        directory.path() / "stages/composed.json",
        R"json({
          "defaultZoom":0.5,
          "boyfriend":[5000,5000],
          "girlfriend":[5000,5000],
          "opponent":[400,310],
          "hide_girlfriend":true
        })json"
    );
    write_text(
        directory.path() / "stages/composed.lua",
        R"lua(function onCreate()
          makeLuaSprite('lua-solid', '', 10, 10)
          makeGraphic('lua-solid', 20, 20, 'FF0000')
          addLuaSprite('lua-solid', false)
        end)lua"
    );
    auto chart = four_lane_chart({});
    chart.stage_id = "composed";
    chart.player_character = "missing-player";
    chart.opponent_character = "missing-opponent";
    chart.girlfriend_character = "missing-gf";
    const auto roots = scene_roots(directory);
    RuntimeScene scene(sdl.renderer(), chart, roots);
    scene.render(1'280.0F, 720.0F, 0.999, 0.0);
    const auto pixel = sdl.pixel(330, 190);
    require(pixel[0U] > 140U && pixel[1U] < 20U && pixel[2U] < 20U,
            "Lua object renders using the JSON base zoom");
    const bool imported_lua = std::ranges::any_of(
        scene.diagnostics(),
        [](const auto& diagnostic) {
            return diagnostic.message.find("loaded safe static scene")
                != std::string::npos;
        }
    );
    require(imported_lua, "JSON stage must not short-circuit its Lua scene");
}

void test_character_offsets_duration_and_animation_budget(SdlFixture& sdl) {
    TemporaryDirectory directory("character-animation-contract");
    write_text(
        directory.path() / "stages/animation-stage.json",
        R"json({
          "defaultZoom":1,
          "boyfriend":[100,100],
          "girlfriend":[5000,5000],
          "opponent":[400,310],
          "hide_girlfriend":true
        })json"
    );
    write_binary(
        directory.path() / "images/characters/test-bf.png",
        png_4x1_colors_base64
    );
    write_text(
        directory.path() / "images/characters/test-bf.xml",
        R"xml(<TextureAtlas>
          <SubTexture name="idle0000" x="0" y="0" width="1" height="1"/>
          <SubTexture name="singLEFT0000" x="1" y="0" width="1" height="1"/>
        </TextureAtlas>)xml"
    );
    write_text(
        directory.path() / "characters/test-bf.json",
        R"json({
          "image":"characters/test-bf",
          "scale":10,
          "sing_duration":8,
          "no_antialiasing":true,
          "animations":[
            {"anim":"idle","name":"idle","fps":24,"loop":true,"offsets":[0,0]},
            {"anim":"singLEFT","name":"singLEFT","fps":24,"loop":false,"offsets":[10,0]}
          ]
        })json"
    );
    auto chart = four_lane_chart({});
    chart.stage_id = "animation-stage";
    chart.player_character = "test-bf";
    chart.opponent_character = "missing-opponent";
    chart.tempos.push_back(pulseforge::TempoChange{0.0, 120.0, 4U, 4U});
    const auto roots = scene_roots(directory);
    RuntimeScene scene(sdl.renderer(), chart, roots);
    // Isolate animation offsets from the normal note-driven camGame focus.
    scene.handle_visual_event("Camera Follow Pos", "640", "360");

    scene.render(1'280.0F, 720.0F, 0.999, 900.0);
    auto pixel = sdl.pixel(105, 105);
    require(pixel[0U] > 40U && pixel[1U] < 20U,
            "idle animation starts at the unshifted character position");

    scene.notify_note_animation(NoteOwner::player, 0U, 1'000.0, "normal");
    scene.render(1'280.0F, 720.0F, 0.999, 1'800.0);
    pixel = sdl.pixel(5, 105);
    require(pixel[1U] > 150U && pixel[0U] < 20U,
            "active animation offset is applied to the rendered sprite");

    scene.render(1'280.0F, 720.0F, 0.999, 2'001.0);
    pixel = sdl.pixel(105, 105);
    require(pixel[0U] > 40U && pixel[1U] < 20U,
            "sing_duration steps return the character to idle at the BPM-derived time");

    RuntimeSceneLimits limits;
    limits.maximum_animation_clips = 1U;
    limits.maximum_animation_frame_references = 1U;
    RuntimeScene bounded(sdl.renderer(), chart, roots, limits);
    const bool budget_reported = std::ranges::any_of(
        bounded.diagnostics(),
        [](const auto& diagnostic) {
            return diagnostic.message.find("animation clip/frame-reference budget")
                != std::string::npos;
        }
    );
    require(budget_reported, "global animation expansion budget is diagnosed");
}

void test_case_insensitive_atlas_and_draw(SDL_Renderer* renderer) {
    TemporaryDirectory directory("atlas");
    write_binary(directory.path() / "images/CuStOm.png", png_4x5_base64);
    write_text(
        directory.path() / "images/CuStOm.xml",
        complete_atlas_xml()
    );
    auto chart = four_lane_chart("CuStOm");
    const auto roots = scene_roots(directory);
    RuntimeScene scene(renderer, chart, roots);
    require_complete_skin(scene);

    require(
        !scene.note_skin_available(RuntimeNoteSkinElement::note_head, 4U),
        "lane outside the four-lane skin must be unavailable"
    );
    require(
        !scene.note_skin_available(
            static_cast<RuntimeNoteSkinElement>(255U),
            0U
        ),
        "unknown skin element must be unavailable"
    );
    require(
        !scene.render_note_skin(RuntimeNoteSkinDraw{
            RuntimeNoteSkinElement::note_head,
            0U,
            RuntimeLogicalRect{0.0F, 0.0F, 0.0F, 20.0F},
        }),
        "zero-width destination must fall back"
    );
    require(
        !scene.render_note_skin(RuntimeNoteSkinDraw{
            RuntimeNoteSkinElement::note_head,
            0U,
            RuntimeLogicalRect{NAN, 0.0F, 20.0F, 20.0F},
        }),
        "non-finite destination must fall back"
    );
}

void test_bounded_custom_note_skin_profiles(SDL_Renderer* renderer) {
    // PULSEFORGE_P1_5_0B_BOUNDED_NOTE_SKIN_PROFILE_TEST_V1
    TemporaryDirectory directory("profiles");
    write_binary(directory.path() / "images/base.png", png_4x5_base64);
    write_text(directory.path() / "images/base.xml", complete_atlas_xml());
    write_binary(directory.path() / "images/alt.png", png_4x5_base64);
    write_text(directory.path() / "images/alt.xml", complete_atlas_xml());

    // RuntimeScene snapshots the VFS at construction. Create every candidate
    // profile before the scene so this test exercises the profile budget, not
    // post-snapshot filesystem mutation.
    for (std::size_t index = 0U; index < 31U; ++index) {
        const auto style = "extra" + std::to_string(index);
        write_binary(
            directory.path() / ("images/" + style + ".png"),
            png_4x5_base64
        );
        write_text(
            directory.path() / ("images/" + style + ".xml"),
            complete_atlas_xml()
        );
    }
    write_binary(directory.path() / "images/overflow.png", png_4x5_base64);
    write_text(
        directory.path() / "images/overflow.xml", complete_atlas_xml()
    );

    auto chart = four_lane_chart("base");
    const auto roots = scene_roots(directory);
    RuntimeScene scene(renderer, chart, roots);
    require_complete_skin(scene);

    const auto alt = scene.resolve_note_skin_profile("alt");
    require(alt.has_value(), "custom note-skin profile should resolve");
    require(
        *alt != pulseforge::detail::runtime_note_skin_default_profile,
        "custom profile handle must not alias the default skin"
    );
    require(
        scene.resolve_note_skin_profile("alt") == alt,
        "identical custom note skin should reuse its bounded profile handle"
    );
    require(
        scene.note_skin_available(
            RuntimeNoteSkinElement::note_head, 0U, *alt
        ),
        "custom profile should expose its note-head frame"
    );
    require(
        !scene.resolve_note_skin_profile("does-not-exist").has_value(),
        "missing custom texture must not silently fall back to NOTE_assets"
    );

    RuntimeNoteSkinDraw custom_draw{
        RuntimeNoteSkinElement::note_head,
        0U,
        RuntimeLogicalRect{140.0F, 30.0F, 20.0F, 24.0F},
        255U,
        false,
        0.0,
    };
    custom_draw.profile = *alt;
    custom_draw.rgb = {31U, 127U, 223U};
    require(
        scene.render_note_skin(custom_draw),
        "resolved custom note-skin profile should render"
    );

    const std::array mixed_draws{
        RuntimeNoteSkinDraw{
            RuntimeNoteSkinElement::note_head,
            0U,
            RuntimeLogicalRect{180.0F, 30.0F, 20.0F, 24.0F},
        },
        custom_draw,
    };
    require(
        scene.render_note_skin_batch(mixed_draws),
        "default and custom profiles should coexist in one batched frame"
    );

    // The scene cache is intentionally capped at 32 custom profiles. Build
    // enough valid unique skins to prove the 33rd is rejected rather than
    // causing unbounded texture/profile growth.
    for (std::size_t index = 0U; index < 31U; ++index) {
        const auto style = "extra" + std::to_string(index);
        require(
            scene.resolve_note_skin_profile(style).has_value(),
            "profile inside the bounded budget should resolve"
        );
    }
    require(
        !scene.resolve_note_skin_profile("overflow").has_value(),
        "33rd custom note-skin profile must be rejected by the fixed budget"
    );
}


// PULSEFORGE_P1_5_0C_NOTE_SPLASH_PROFILE_TEST_V1
void test_bounded_custom_note_splash_profiles(SDL_Renderer* renderer) {
    TemporaryDirectory directory("splash-profiles");
    write_binary(directory.path() / "images/base.png", png_4x5_base64);
    write_text(directory.path() / "images/base.xml", complete_atlas_xml());
    write_binary(
        directory.path() / "images/noteSplashes/customSplash.png",
        png_4x5_base64
    );
    write_text(
        directory.path() / "images/noteSplashes/customSplash.xml",
        splash_atlas_xml()
    );

    auto chart = four_lane_chart("base");
    const auto roots = scene_roots(directory);
    RuntimeScene scene(renderer, chart, roots);
    const auto splash = scene.resolve_note_splash_profile("customSplash");
    require(splash.has_value(), "custom note-splash atlas should resolve");
    require(
        scene.resolve_note_splash_profile("customSplash") == splash,
        "custom note-splash profile reuses its bounded handle"
    );
    require(
        scene.note_splash_frame_count(*splash) == 4U,
        "custom note-splash atlas exposes its bounded animation frames"
    );
    require(
        scene.render_note_splash(pulseforge::detail::RuntimeNoteSplashDraw{
            *splash,
            2U,
            RuntimeLogicalRect{200.0F, 80.0F, 64.0F, 64.0F},
            220U,
            15.0,
        }),
        "resolved custom note-splash frame should render"
    );
    require(
        !scene.resolve_note_splash_profile("../escape").has_value()
            && !scene.resolve_note_splash_profile("missingSplash").has_value(),
        "unsafe or missing custom splash assets keep geometric fallback"
    );
}

void test_incomplete_atlas_falls_back_per_element(SDL_Renderer* renderer) {
    TemporaryDirectory directory("partial");
    write_binary(directory.path() / "images/partial.png", png_4x5_base64);
    write_text(
        directory.path() / "images/partial.xml",
        "<TextureAtlas><SubTexture name=\"PURPLE0000\" x=\"0\" y=\"0\" "
        "width=\"1\" height=\"1\"/></TextureAtlas>"
    );
    auto chart = four_lane_chart("partial");
    const auto roots = scene_roots(directory);
    RuntimeScene scene(renderer, chart, roots);
    require(
        scene.note_skin_available(RuntimeNoteSkinElement::note_head, 0U),
        "available partial frame should remain usable"
    );
    require(
        !scene.note_skin_available(RuntimeNoteSkinElement::note_head, 1U),
        "missing partial frame must use geometric fallback"
    );
    require(
        !scene.render_note_skin(RuntimeNoteSkinDraw{
            RuntimeNoteSkinElement::sustain_body,
            0U,
            RuntimeLogicalRect{0.0F, 0.0F, 10.0F, 30.0F},
        }),
        "missing sustain frame must return false"
    );

    bool reported_incomplete{};
    for (const auto& diagnostic : scene.diagnostics()) {
        if (diagnostic.message.find("incomplete") != std::string::npos) {
            reported_incomplete = true;
        }
    }
    require(reported_incomplete, "incomplete skin should be diagnosed once");
}

void test_missing_and_unsafe_style_fall_back_to_note_assets(
    SDL_Renderer* renderer
) {
    TemporaryDirectory directory("fallback");
    write_binary(
        directory.path() / "shared/images/noteSkins/NOTE_assets.png",
        png_4x5_base64
    );
    write_text(
        directory.path() / "shared/images/noteSkins/NOTE_assets.xml",
        complete_atlas_xml()
    );
    const auto roots = scene_roots(directory);

    auto missing_chart = four_lane_chart("does-not-exist");
    RuntimeScene missing_scene(renderer, missing_chart, roots);
    require_complete_skin(missing_scene);

    auto unsafe_chart = four_lane_chart("../escape");
    RuntimeScene unsafe_scene(renderer, unsafe_chart, roots);
    require_complete_skin(unsafe_scene);
    bool rejected_unsafe{};
    for (const auto& diagnostic : unsafe_scene.diagnostics()) {
        if (diagnostic.message.find("unsafe note-style") != std::string::npos) {
            rejected_unsafe = true;
        }
    }
    require(rejected_unsafe, "unsafe style id should be diagnosed");
}

void test_pixel_conventions_and_texture_budget(SDL_Renderer* renderer) {
    TemporaryDirectory directory("pixel");
    write_binary(
        directory.path() / "images/pixelUI/arrows-pixels.png",
        png_4x5_base64
    );
    write_binary(
        directory.path() / "images/pixelUI/arrowEnds.png",
        png_4x2_base64
    );
    auto chart = four_lane_chart("pixel");
    const auto roots = scene_roots(directory);

    RuntimeScene scene(renderer, chart, roots);
    require_complete_skin(scene);

    RuntimeSceneLimits limits;
    limits.maximum_textures = 1U;
    RuntimeScene bounded_scene(renderer, chart, roots, limits);
    require(
        bounded_scene.note_skin_available(
            RuntimeNoteSkinElement::note_head,
            3U
        ),
        "pixel arrow sheet should fit the first texture slot"
    );
    require(
        !bounded_scene.note_skin_available(
            RuntimeNoteSkinElement::sustain_body,
            0U
        ),
        "exhausted texture budget must retain sustain fallback"
    );
}

void test_mount_precedence_and_no_cross_mount_pairing(SDL_Renderer* renderer) {
    TemporaryDirectory directory("precedence");
    const auto low = directory.path() / "low";
    const auto high = directory.path() / "high";

    write_binary(low / "images/CuStOm.png", png_4x5_base64);
    write_text(low / "images/CuStOm.xml", complete_atlas_xml());
    write_binary(
        high / "shared/images/noteSkins/CuStOm.png",
        png_4x5_base64
    );
    write_text(
        high / "shared/images/noteSkins/CuStOm.xml",
        "<TextureAtlas><SubTexture name=\"PURPLE0000\" x=\"0\" y=\"0\" "
        "width=\"1\" height=\"1\"/></TextureAtlas>"
    );
    auto chart = four_lane_chart("CuStOm");
    const std::array roots{low, high};
    RuntimeScene scene(renderer, chart, roots);
    require(
        scene.note_skin_available(RuntimeNoteSkinElement::note_head, 0U),
        "higher-precedence alternate-layout skin should be selected"
    );
    require(
        !scene.note_skin_available(RuntimeNoteSkinElement::receptor, 0U),
        "lower-precedence direct-layout skin must not override selected mod"
    );

    const auto low_same = directory.path() / "low-same";
    const auto high_same = directory.path() / "high-same";
    write_binary(low_same / "images/split.png", png_4x5_base64);
    write_text(low_same / "images/split.xml", complete_atlas_xml());
    write_binary(high_same / "images/split.png", png_4x5_base64);
    const std::array same_path_roots{low_same, high_same};
    chart.note_style = "split";
    RuntimeScene split_scene(renderer, chart, same_path_roots);
    require(
        !split_scene.note_skin_available(
            RuntimeNoteSkinElement::note_head,
            0U
        ),
        "PNG and Sparrow XML must not be paired across different mounts"
    );

    const auto low_pixel = directory.path() / "low-pixel";
    const auto high_pixel = directory.path() / "high-pixel";
    write_binary(
        low_pixel / "shared/images/pixelUI/arrows-pixels.png",
        png_4x5_base64
    );
    write_binary(
        low_pixel / "shared/images/pixelUI/arrowEnds.png",
        png_4x2_base64
    );
    write_binary(
        high_pixel / "shared/images/pixelUI/arrows-pixels.png",
        png_4x5_base64
    );
    const std::array pixel_roots{low_pixel, high_pixel};
    chart.note_style = "pixel";
    RuntimeScene pixel_scene(renderer, chart, pixel_roots);
    require(
        pixel_scene.note_skin_available(
            RuntimeNoteSkinElement::note_head,
            0U
        ),
        "higher-precedence pixel arrow sheet should load"
    );
    require(
        !pixel_scene.note_skin_available(
            RuntimeNoteSkinElement::sustain_end,
            0U
        ),
        "pixel arrows and ends must not be paired across mounts"
    );
}

void test_atlas_frame_budget_and_non_four_lane(SDL_Renderer* renderer) {
    TemporaryDirectory directory("budgets");
    write_binary(directory.path() / "images/limited.png", png_4x5_base64);
    write_text(
        directory.path() / "images/limited.xml",
        complete_atlas_xml()
    );
    auto chart = four_lane_chart("limited");
    const auto roots = scene_roots(directory);

    RuntimeSceneLimits limits;
    limits.maximum_atlas_frames = 4U;
    RuntimeScene limited_scene(renderer, chart, roots, limits);
    require(
        limited_scene.note_skin_available(
            RuntimeNoteSkinElement::receptor,
            3U
        ),
        "first four atlas frames should fit the frame budget"
    );
    require(
        !limited_scene.note_skin_available(
            RuntimeNoteSkinElement::note_head,
            0U
        ),
        "atlas frame budget must retain head fallback"
    );

    chart.key_count = 6U;
    RuntimeScene six_lane_scene(renderer, chart, roots);
    require(
        !six_lane_scene.note_skin_available(
            RuntimeNoteSkinElement::receptor,
            0U
        ),
        "four-lane note skin must not partially style a six-lane chart"
    );
}

}  // namespace

int main() {
    try {
        SdlFixture sdl;
        test_case_insensitive_atlas_and_draw(sdl.renderer());
        test_bounded_custom_note_skin_profiles(sdl.renderer());
        test_bounded_custom_note_splash_profiles(sdl.renderer());
        test_incomplete_atlas_falls_back_per_element(sdl.renderer());
        test_missing_and_unsafe_style_fall_back_to_note_assets(sdl.renderer());
        test_pixel_conventions_and_texture_budget(sdl.renderer());
        test_mount_precedence_and_no_cross_mount_pairing(sdl.renderer());
        test_atlas_frame_budget_and_non_four_lane(sdl.renderer());
        test_character_animation_events_are_safe(sdl.renderer());
        test_stage_json_and_static_lua_are_composed(sdl);
        test_character_offsets_duration_and_animation_budget(sdl);
        std::cout << "runtime note-skin tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "runtime note-skin test failed: " << error.what() << '\n';
        return 1;
    }
}
