from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    if old not in text:
        raise SystemExit(f"anchor missing in {path}: {old[:100]!r}")
    if text.count(old) != 1:
        raise SystemExit(f"anchor not unique in {path}: {text.count(old)} matches")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


# 1) Keep Lua pattern execution disabled, but restore the subset of string.find
# used by Psych mods as a bounded literal-only search.
replace_once(
    "src/script/lua_runtime.cpp",
    '''[[nodiscard]] static int host_string_trim(lua_State* lua) noexcept {''',
    '''[[nodiscard]] static int host_string_find_plain(lua_State* lua) noexcept {
    auto* self = from_state(lua);
    std::size_t value_length{}, needle_length{};
    const char* value_data = lua_tolstring(lua, 1, &value_length);
    const char* needle_data = lua_tolstring(lua, 2, &needle_length);
    if (value_data == nullptr || needle_data == nullptr) {
        lua_pushnil(lua);
        return 1;
    }

    const std::size_t limit = self != nullptr
        ? self->config.max_host_string_bytes
        : 4'096U;
    value_length = std::min(value_length, limit);
    needle_length = std::min(needle_length, limit);
    const std::string_view value{value_data, value_length};
    const std::string_view needle{needle_data, needle_length};

    lua_Integer initial = 1;
    if (lua_gettop(lua) >= 3 && lua_isinteger(lua, 3)) {
        initial = lua_tointeger(lua, 3);
    }
    std::size_t start = 0U;
    if (initial < 0) {
        const auto magnitude = static_cast<std::uint64_t>(-(initial + 1)) + 1U;
        start = magnitude >= value.size()
            ? 0U
            : value.size() - static_cast<std::size_t>(magnitude);
    } else if (initial > 1) {
        const auto requested = static_cast<std::uint64_t>(initial - 1);
        if (requested > value.size()) {
            lua_pushnil(lua);
            return 1;
        }
        start = static_cast<std::size_t>(requested);
    }

    const auto found = value.find(needle, start);
    if (found == std::string_view::npos) {
        lua_pushnil(lua);
        return 1;
    }
    lua_pushinteger(lua, static_cast<lua_Integer>(found + 1U));
    lua_pushinteger(
        lua,
        static_cast<lua_Integer>(found + needle.size())
    );
    return 2;
}

[[nodiscard]] static int host_string_trim(lua_State* lua) noexcept {'''
)

replace_once(
    "src/script/lua_runtime.cpp",
    '''            for (const char* name : removed_string_functions) {
                lua_pushnil(state);
                lua_setfield(state, -2, name);
            }
        }
        lua_pop(state, 1);''',
    '''            for (const char* name : removed_string_functions) {
                lua_pushnil(state);
                lua_setfield(state, -2, name);
            }
            // PULSEFORGE_1_0_0_SAFE_LITERAL_STRING_FIND_V1
            // Psych corpus scripts use string.find for short animation-name
            // probes. Re-expose only literal search: Lua patterns remain
            // disabled so untrusted patterns cannot execute inside the C
            // library outside the VM instruction budget.
            lua_pushcfunction(state, &Impl::host_string_find_plain);
            lua_setfield(state, -2, "find");
        }
        lua_pop(state, 1);'''
)

# 2) Register the two legacy Psych surfaces used by Timeless/Overkill.
replace_once(
    "src/script/lua_runtime.cpp",
    '''            "getPropertyLuaSprite", "setPropertyLuaSprite",
            "setScrollFactor", "scaleObject",''',
    '''            "getPropertyLuaSprite", "setPropertyLuaSprite",
            "setScrollFactor", "setLuaSpriteScrollFactor", "scaleObject",'''
)
replace_once(
    "src/script/lua_runtime.cpp",
    '''            "getMouseX", "getMouseY",
            // PULSEFORGE_P1_1_12_SHADER_API_RESTORE_V1''',
    '''            "getMouseX", "getMouseY", "callMethod",
            // PULSEFORGE_P1_1_12_SHADER_API_RESTORE_V1'''
)
replace_once(
    "src/script/lua_runtime.cpp",
    '''                    || name == "inGameOver"
                    || name.starts_with("defaultPlayerStrumX")''',
    '''                    || name == "inGameOver"
                    // PULSEFORGE_1_0_0_OVERKILL_TURN_GLOBAL_V1
                    || name == "mustHitSection"
                    || name.starts_with("defaultPlayerStrumX")'''
)

# 3) RuntimeScene exposes only the active animation name, not native pointers.
replace_once(
    "src/app/runtime_scene.hpp",
    '''    [[nodiscard]] bool script_play_animation(
        std::string_view tag,
        std::string_view animation,
        bool force,
        double song_time_ms
    ) noexcept;''',
    '''    // Restricted Psych callMethod compatibility: exposes only the
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
    ) noexcept;'''
)

replace_once(
    "src/app/runtime_scene.cpp",
    '''    [[nodiscard]] bool script_play_animation(
        const std::string_view tag,
        const std::string_view animation,
        const bool force,
        const double song_time_ms
    ) noexcept {''',
    '''    [[nodiscard]] bool script_get_animation_name(
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
    ) noexcept {'''
)
replace_once(
    "src/app/runtime_scene.cpp",
    '''bool RuntimeScene::script_play_animation(
    const std::string_view tag,
    const std::string_view animation,
    const bool force,
    const double song_time_ms
) noexcept {''',
    '''bool RuntimeScene::script_get_animation_name(
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
) noexcept {'''
)

# 4) Application host: turn-global compatibility and one allow-listed method.
replace_once(
    "src/app/application.cpp",
    '''        else if (name == "bfHit" || name == "daHit") value = false;
        else if (name == "crochet" || name == "stepCrochet") {''',
    '''        else if (name == "bfHit" || name == "daHit") value = false;
        else if (name == "mustHitSection") {
            // The compact chart model intentionally does not retain Psych
            // section objects. The automatic camera-turn tracker is the
            // authoritative resolved singing owner during gameplay. Before
            // the first singing event, default to the Psych/player side; this
            // also matches Timeless' silent opening sections.
            value = !script_auto_camera_owner_.has_value()
                || *script_auto_camera_owner_ == NoteOwner::player;
        }
        else if (name == "crochet" || name == "stepCrochet") {'''
)
replace_once(
    "src/app/application.cpp",
    '''        if (name == "getCharacterX" || name == "getCharacterY"
            || name == "setCharacterX" || name == "setCharacterY") {''',
    '''        // PULSEFORGE_1_0_0_RESTRICTED_CALL_METHOD_V1
        // Do not expose Haxe/native reflection. The historical corpus only
        // needs getAnimationName() for character-state probes, so permit
        // exactly those bounded read-only calls.
        if (name == "callMethod") {
            std::string_view method;
            if (!string_arg(0U, method) || arguments.size() != 1U
                || method.size() > 128U) {
                error = "callMethod expects one bounded allow-listed method";
                return false;
            }
            std::string_view tag;
            if (method == "dad.getAnimationName") tag = "dad";
            else if (method == "boyfriend.getAnimationName") tag = "boyfriend";
            else if (method == "gf.getAnimationName") tag = "gf";
            else {
                error = "callMethod is restricted to character getAnimationName";
                return false;
            }
            std::string animation{"idle"};
            if (scene_ != nullptr) {
                std::string resolved;
                if (scene_->script_get_animation_name(
                        tag, gameplay_song_time_ms(), resolved
                    )) {
                    animation = std::move(resolved);
                }
            }
            result = std::move(animation);
            error.clear();
            return true;
        }
        if (name == "getCharacterX" || name == "getCharacterY"
            || name == "setCharacterX" || name == "setCharacterY") {'''
)

# 5) Extend the sanitized Overkill regression and keep the sandbox guarantee
# that pattern-based string operations remain unavailable.
replace_once(
    "tests/lua_test.cpp",
    '''    [[nodiscard]] bool get_property(std::string_view, pulseforge::ScriptValue&, std::string& error) const override {
        error = "unused"; return false;
    }''',
    '''    [[nodiscard]] bool get_property(
        std::string_view name,
        pulseforge::ScriptValue& value,
        std::string& error
    ) const override {
        if (name == "mustHitSection") {
            value = true;
            error.clear();
            return true;
        }
        error = "unused";
        return false;
    }'''
)
replace_once(
    "tests/lua_test.cpp",
    '''        if (name != "addGlitchEffect") {
            error = "unexpected command";
            return false;
        }
        glitch_called = true;
        result = true;
        error.clear();
        return true;
    }
    bool glitch_called{};''',
    '''        if (name == "addGlitchEffect") {
            glitch_called = true;
            result = true;
            error.clear();
            return true;
        }
        if (name == "setLuaSpriteScrollFactor") {
            scroll_alias_called = true;
            result = true;
            error.clear();
            return true;
        }
        if (name == "callMethod") {
            method_called = true;
            result = std::string{"singLEFT"};
            error.clear();
            return true;
        }
        error = "unexpected command";
        return false;
    }
    bool glitch_called{};
    bool scroll_alias_called{};
    bool method_called{};'''
)
replace_once(
    "tests/lua_test.cpp",
    '''            assert(math.abs(curDecStep - 12.5) < 0.000001)
            assert(addGlitchEffect('bg', 2.25, 5, 0.1) == true)
        end
    )lua";''',
    '''            assert(math.abs(curDecStep - 12.5) < 0.000001)
            assert(mustHitSection == true)
            assert(type(string.find) == 'function')
            local first, last = string.find(callMethod('dad.getAnimationName'), 'sing')
            assert(first == 1 and last == 4)
            assert(string.find('singLEFT', '.') == nil)
            assert(addGlitchEffect('bg', 2.25, 5, 0.1) == true)
            assert(setLuaSpriteScrollFactor('bg', 0.5, 0.5) == true)
        end
    )lua";'''
)
replace_once(
    "tests/lua_test.cpp",
    '''    require(host.glitch_called, "addGlitchEffect reached the bounded host bridge");
}''',
    '''    require(host.glitch_called, "addGlitchEffect reached the bounded host bridge");
    require(host.scroll_alias_called,
            "setLuaSpriteScrollFactor reached the bounded host bridge");
    require(host.method_called,
            "restricted callMethod reached the bounded host bridge");
}'''
)
replace_once(
    "tests/lua_test.cpp",
    '''            assert(string.dump == nil)
            assert(string.find == nil)
            assert(string.match == nil)''',
    '''            assert(string.dump == nil)
            assert(type(string.find) == 'function')
            local findStart, findEnd = string.find('abc', 'b')
            assert(findStart == 2 and findEnd == 2)
            assert(string.find('abc', '.') == nil)
            assert(string.match == nil)'''
)

# Release documentation should describe the complete compatibility surface.
replace_once(
    "CHANGELOG.md",
    '''- Adds Overkill/Timeless Lua compatibility: `addGlitchEffect` receives a bounded renderer-native fallback, `setScrollFactor(tag)` accepts Psych's omitted factors, and fractional `curDecBeat`/`curDecStep` globals are available.''',
    '''- Adds Overkill/Timeless Lua compatibility: `addGlitchEffect` receives a bounded renderer-native fallback; `setScrollFactor(tag)` and legacy `setLuaSpriteScrollFactor` follow Psych semantics; fractional `curDecBeat`/`curDecStep` and resolved `mustHitSection` are available; a bounded literal-only `string.find` and allow-listed character `getAnimationName` bridge prevent the original scripts from aborting without exposing general Lua patterns or native reflection.'''
)
replace_once(
    "docs/RELEASE_NOTES_1.0.0.md",
    '''- Overkill/Timeless compatibility fixed so unsupported glitch helpers no longer abort stage setup, single-argument `setScrollFactor` follows Psych semantics, and decimal beat/step globals drive its modchart motion.''',
    '''- Overkill/Timeless compatibility fixed end-to-end: glitch helpers no longer abort stage setup; `setScrollFactor`/`setLuaSpriteScrollFactor` follow Psych semantics; decimal beat/step plus resolved `mustHitSection` globals drive the modchart; and the script's animation probe runs through bounded literal `string.find` plus an allow-listed `getAnimationName` bridge rather than unrestricted patterns/reflection.'''
)

print("final Overkill compatibility patch applied")
