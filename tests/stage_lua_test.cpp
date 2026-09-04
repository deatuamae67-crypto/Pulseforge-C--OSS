#include "pulseforge/stage_lua.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void common_subset() {
    constexpr std::string_view source = R"lua(
function onCreate()
  makeLuaSprite('sky', 'farm/sky', -700, -500)
  setScrollFactor('sky', 0.9, 0.8)
  scaleObject('sky', 2, 2)
  addLuaSprite('sky', false)
  makeAnimatedLuaSprite('fire', 'farm/fire', -50, -1195)
  addAnimationByPrefix('fire', 'idle', 'Fire', 24, true)
  addAnimationByIndices('fire', 'pose', 'Fire', '0, 2,4', 12)
  objectPlayAnimation('fire', 'idle', true)
  setProperty('fire.antialiasing', false)
  setPropertyLuaSprite('fire', 'flipX', true)
  setProperty('fire.alpha', 0.75)
  setObjectOrder('fire', 3)
  addLuaSprite('fire', true)
  setProperty('gf.visible', false)
  makeLuaSprite('bar', '', 0, 620)
  makeGraphic('bar', 1280, 100, '101010')
  setObjectCamera('bar', 'hud')
  setBlendMode('bar', 'add')
  addLuaSprite('bar', true)
end
)lua";
    const auto result = pulseforge::parse_static_psych_stage_lua(source, "farm");
    require(result.stage.has_value(), "static stage should parse");
    require(result.stage->id == "farm", "stage id should round-trip");
    require(result.stage->hide_girlfriend, "gf visibility should be imported");
    require(result.stage->objects.size() == 3U, "three added sprites expected");
    const auto find_object = [&](const std::string_view name) -> const auto& {
        const auto found = std::ranges::find(
            result.stage->objects,
            name,
            &pulseforge::StageObjectDescriptor::name
        );
        require(found != result.stage->objects.end(), "stage object missing");
        return *found;
    };
    const auto& sky = find_object("sky");
    const auto& fire = find_object("fire");
    require(sky.name == "sky" && sky.image == "farm/sky", "sky identity");
    require(sky.x == -700.0 && sky.y == -500.0, "sky position");
    require(sky.scale.x == 2.0 && sky.scroll.y == 0.8, "sky transforms");
    require(fire.name == "fire" && fire.type == "animated", "animated sprite");
    require(!fire.antialiasing && fire.flip_x && fire.alpha == 0.75,
            "sprite properties");
    require(fire.foreground, "foreground addLuaSprite flag");
    require(fire.first_animation == "idle", "active animation");
    require(fire.animations.size() == 2U && fire.animations[0].fps == 24
            && fire.animations[0].loop, "animation data");
    require(fire.animations[1].indices.size() == 3U
            && fire.animations[1].indices[1] == 2
            && fire.animations[1].fps == 12,
            "indexed animation data");
    const auto& bar = find_object("bar");
    require(bar.type == "solid" && bar.width == 1280.0
            && bar.height == 100.0 && bar.color == "101010",
            "makeGraphic data");
    require(bar.screen_space && bar.blend == "add",
            "camera and blend data");
}

void dynamic_and_unadded_ignored() {
    constexpr std::string_view source = R"lua(
makeLuaSprite('kept', 'kept', 1, 2)
addLuaSprite('kept', false)
makeLuaSprite('not-added', 'hidden', 3, 4)
makeLuaSprite(dynamicTag, 'dynamic', 0, 0)
makeLuaSprite('expression', 'x', screenWidth / 2, 0)
setProperty('kept.visible', false)
)lua";
    const auto result = pulseforge::parse_static_psych_stage_lua(source, "x");
    require(result.stage.has_value(), "bounded parse should succeed");
    require(result.stage->objects.empty(), "hidden/unadded/dynamic sprites ignored");
}

void budgets_fail_closed() {
    pulseforge::StaticStageLuaLimits limits;
    limits.maximum_sprites = 1U;
    const auto result = pulseforge::parse_static_psych_stage_lua(
        "makeLuaSprite('a','a',0,0) addLuaSprite('a',false) "
        "makeLuaSprite('b','b',0,0) addLuaSprite('b',false)",
        "bounded", limits
    );
    require(result.stage.has_value(), "budget keeps safe subset");
    require(result.stage->objects.size() == 1U, "sprite limit enforced");
    require(result.truncated, "budget truncation reported");
}

void callbacks_and_dynamic_branches_fail_closed() {
    constexpr std::string_view source = R"lua(
makeLuaSprite('top', 'top', 0, 0)
addLuaSprite('top', false)
function onUpdate(elapsed)
  makeLuaSprite('updated', 'bad', 0, 0)
  addLuaSprite('updated', false)
end
function onEvent(name, value1, value2)
  makeLuaSprite('evented', 'bad', 0, 0)
  addLuaSprite('evented', false)
end
function onCreate()
  if getProperty('health') > 1 then
    do
      makeLuaSprite('conditional', 'bad', 0, 0)
      addLuaSprite('conditional', false)
    end
    makeLuaSprite('conditional-after-do', 'bad', 0, 0)
    addLuaSprite('conditional-after-do', false)
  end
  makeLuaSprite('created', 'created', 0, 0)
  addLuaSprite('created', false)
end
function onCreatePost()
  makeLuaSprite('post', 'post', 0, 0)
  addLuaSprite('post', false)
end
)lua";
    const auto result = pulseforge::parse_static_psych_stage_lua(
        source,
        "scoped"
    );
    require(result.stage.has_value(), "scoped static subset should parse");
    require(result.stage->objects.size() == 3U,
            "only top-level/onCreate/onCreatePost objects are imported");
    const auto has = [&](const std::string_view name) {
        return std::ranges::find(
            result.stage->objects,
            name,
            &pulseforge::StageObjectDescriptor::name
        ) != result.stage->objects.end();
    };
    require(has("top") && has("created") && has("post"),
            "safe declarations remain available");
    require(!has("updated") && !has("evented") && !has("conditional")
                && !has("conditional-after-do"),
            "runtime callbacks and conditional declarations are ignored");
    require(!result.diagnostics.empty(), "ignored dynamic Lua is diagnosed");
}

void relative_character_layers_are_preserved() {
    constexpr std::string_view source = R"lua(
function onCreate()
  makeLuaSprite('bg', 'bg', 0, 0)
  addLuaSprite('bg', false)
  makeLuaSprite('gf-layer', 'gf-layer', 0, 0)
  addBehindGF('gf-layer')
  makeLuaSprite('dad-layer', 'dad-layer', 0, 0)
  addBehindDad('dad-layer')
  makeLuaSprite('bf-layer', 'bf-layer', 0, 0)
  addBehindBF('bf-layer')
  makeLuaSprite('front', 'front', 0, 0)
  addLuaSprite('front', true)
end
)lua";
    const auto result = pulseforge::parse_static_psych_stage_lua(
        source,
        "layers"
    );
    require(result.stage.has_value() && result.stage->objects.size() == 5U,
            "all layered objects should parse");
    const auto layer = [&](const std::string_view name) {
        const auto found = std::ranges::find(
            result.stage->objects,
            name,
            &pulseforge::StageObjectDescriptor::name
        );
        require(found != result.stage->objects.end(), "layered object missing");
        return found->layer;
    };
    require(layer("bg") == pulseforge::StageObjectLayer::background,
            "background layer preserved");
    require(layer("gf-layer") == pulseforge::StageObjectLayer::behind_girlfriend,
            "addBehindGF layer preserved");
    require(layer("dad-layer") == pulseforge::StageObjectLayer::behind_opponent,
            "addBehindDad layer preserved");
    require(layer("bf-layer") == pulseforge::StageObjectLayer::behind_player,
            "addBehindBF layer preserved");
    require(layer("front") == pulseforge::StageObjectLayer::foreground,
            "foreground layer preserved");
}

void literals_never_become_executable_stage_calls() {
    constexpr std::string_view source = R"stage(
local quoted = "makeLuaSprite('quoted','bad',0,0) addLuaSprite('quoted',false) function onCreate() end"
local single = 'if enabled then makeLuaSprite("single","bad",0,0) end'
local long = [==[
function onCreate()
  makeLuaSprite('long', 'bad', 0, 0)
  addLuaSprite('long', false)
end
]==]
--[=[ makeLuaSprite('commented','bad',0,0) addLuaSprite('commented',false) ]=]
function onCreate()
  makeLuaSprite('real', 'real', 0, 0)
  addLuaSprite('real', false)
end
)stage";
    const auto result = pulseforge::parse_static_psych_stage_lua(
        source,
        "literal-safety"
    );
    require(result.stage.has_value(), "literal safety fixture should parse");
    require(result.stage->objects.size() == 1U
                && result.stage->objects.front().name == "real",
            "quoted, long-bracket, and comment contents stay inert");
}

void short_circuit_calls_are_not_imported_unconditionally() {
    constexpr std::string_view source = R"lua(
function onCreate()
  makeLuaSprite('and-candidate', 'bad', 0, 0)
  enabled and addLuaSprite('and-candidate', false)
  makeLuaSprite('or-candidate', 'bad', 0, 0)
  disabled or addLuaSprite('or-candidate', false)
  makeLuaSprite('multiline-candidate', 'bad', 0, 0)
  enabled and
    addLuaSprite('multiline-candidate', false)
  makeLuaSprite('real', 'real', 0, 0)
  addLuaSprite('real', false)
end
)lua";
    const auto result = pulseforge::parse_static_psych_stage_lua(
        source,
        "short-circuit"
    );
    require(result.stage.has_value(), "short-circuit fixture should parse");
    require(result.stage->objects.size() == 1U
                && result.stage->objects.front().name == "real",
            "and/or calls must not be applied as unconditional declarations");
    require(!result.diagnostics.empty(),
            "ignored short-circuit calls should be diagnosed");
}

void repeat_until_condition_stays_dynamic_after_scope_close() {
    constexpr std::string_view source = R"lua(
function onCreate()
  makeLuaSprite('until-candidate', 'bad', 0, 0)
  repeat
  until addLuaSprite('until-candidate', false)
  makeLuaSprite('real', 'real', 0, 0)
  addLuaSprite('real', false)
end
)lua";
    const auto result = pulseforge::parse_static_psych_stage_lua(
        source,
        "repeat-until"
    );
    require(result.stage.has_value(), "repeat-until fixture should parse");
    require(result.stage->objects.size() == 1U
                && result.stage->objects.front().name == "real",
            "call in the until condition must remain dynamic");
    require(!result.diagnostics.empty(),
            "ignored until-condition call should be diagnosed");
}

void conditional_return_makes_callback_tail_fail_closed() {
    const auto result = pulseforge::parse_static_psych_stage_lua(R"(
function onCreate()
    if lowQuality then
        return
    end
    makeLuaSprite('unreachable', 'stages/ghost', 0, 0)
    addLuaSprite('unreachable', false)
end
)", "return-stage");
    require(result.stage.has_value(), "return fixture did not parse");
    require(
        result.stage->objects.empty(),
        "calls after a potentially conditional return were imported"
    );
}

}  // namespace

int main() {
    try {
        common_subset();
        dynamic_and_unadded_ignored();
        budgets_fail_closed();
        callbacks_and_dynamic_branches_fail_closed();
        relative_character_layers_are_preserved();
        literals_never_become_executable_stage_calls();
        short_circuit_calls_are_not_imported_unconditionally();
        repeat_until_condition_stays_dynamic_after_scope_close();
        conditional_return_makes_callback_tail_fail_closed();
        std::cout << "static stage Lua tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "static stage Lua test failed: " << error.what() << '\n';
        return 1;
    }
}
