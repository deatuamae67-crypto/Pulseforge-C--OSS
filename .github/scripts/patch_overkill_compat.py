from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, found {count}: {old!r}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


# Fractional Psych timing globals without making every LuaHost implementation
# reimplement them. Hosts which can provide exact timing override these defaults.
replace_once(
    "include/pulseforge/lua_runtime.hpp",
    "    [[nodiscard]] virtual std::int64_t current_section() const noexcept = 0;\n\n"
    "    [[nodiscard]] virtual bool get_property(\n",
    "    [[nodiscard]] virtual std::int64_t current_section() const noexcept = 0;\n\n"
    "    [[nodiscard]] virtual double current_decimal_beat() const noexcept {\n"
    "        return static_cast<double>(current_beat());\n"
    "    }\n"
    "    [[nodiscard]] virtual double current_decimal_step() const noexcept {\n"
    "        return static_cast<double>(current_step());\n"
    "    }\n\n"
    "    [[nodiscard]] virtual bool get_property(\n",
)
replace_once(
    "include/pulseforge/lua_runtime.hpp",
    "    [[nodiscard]] std::int64_t current_section() const noexcept override;\n\n"
    "    [[nodiscard]] bool get_property(\n",
    "    [[nodiscard]] std::int64_t current_section() const noexcept override;\n"
    "    [[nodiscard]] double current_decimal_beat() const noexcept override;\n"
    "    [[nodiscard]] double current_decimal_step() const noexcept override;\n\n"
    "    [[nodiscard]] bool get_property(\n",
)

replace_once(
    "src/script/lua_runtime.cpp",
    "std::int64_t GameplayLuaHost::current_section() const noexcept {\n"
    "    return floor_to_int64(\n"
    "        session_.timing_map().step_at(session_.song_time_ms()) / 16.0\n"
    "    );\n"
    "}\n\n",
    "std::int64_t GameplayLuaHost::current_section() const noexcept {\n"
    "    return floor_to_int64(\n"
    "        session_.timing_map().step_at(session_.song_time_ms()) / 16.0\n"
    "    );\n"
    "}\n\n"
    "double GameplayLuaHost::current_decimal_beat() const noexcept {\n"
    "    return session_.timing_map().beat_at(session_.song_time_ms());\n"
    "}\n\n"
    "double GameplayLuaHost::current_decimal_step() const noexcept {\n"
    "    return session_.timing_map().step_at(session_.song_time_ms());\n"
    "}\n\n",
)
replace_once(
    "src/script/lua_runtime.cpp",
    "    } else if (matches(name, {\"curSection\"})) {\n"
    "        value = current_section();\n"
    "    } else if (matches(name, {\"bpm\", \"curBpm\"})) {\n",
    "    } else if (matches(name, {\"curSection\"})) {\n"
    "        value = current_section();\n"
    "    } else if (matches(name, {\"curDecBeat\"})) {\n"
    "        value = current_decimal_beat();\n"
    "    } else if (matches(name, {\"curDecStep\"})) {\n"
    "        value = current_decimal_step();\n"
    "    } else if (matches(name, {\"bpm\", \"curBpm\"})) {\n",
)
replace_once(
    "src/script/lua_runtime.cpp",
    "        lua_setglobal(lua, \"curSection\");\n\n"
    "        if (self->pending.update_beat) {\n",
    "        lua_setglobal(lua, \"curSection\");\n"
    "        lua_pushnumber(lua, self->host.current_decimal_beat());\n"
    "        lua_setglobal(lua, \"curDecBeat\");\n"
    "        lua_pushnumber(lua, self->host.current_decimal_step());\n"
    "        lua_setglobal(lua, \"curDecStep\");\n\n"
    "        if (self->pending.update_beat) {\n",
)
replace_once(
    "src/script/lua_runtime.cpp",
    "        lua_pushinteger(state, 0);\n"
    "        lua_setglobal(state, \"curSection\");\n"
    "        lua_pushnumber(state, 0.0);\n"
    "        lua_setglobal(state, \"songPosition\");\n",
    "        lua_pushinteger(state, 0);\n"
    "        lua_setglobal(state, \"curSection\");\n"
    "        lua_pushnumber(state, 0.0);\n"
    "        lua_setglobal(state, \"curDecBeat\");\n"
    "        lua_pushnumber(state, 0.0);\n"
    "        lua_setglobal(state, \"curDecStep\");\n"
    "        lua_pushnumber(state, 0.0);\n"
    "        lua_setglobal(state, \"songPosition\");\n",
)
replace_once(
    "src/script/lua_runtime.cpp",
    '            "wavyEffect", "close",\n',
    '            "wavyEffect", "addGlitchEffect", "close",\n',
)
replace_once(
    "src/script/lua_runtime.cpp",
    '        register_host_command("wavyEffect");\n',
    '        register_host_command("wavyEffect");\n'
    '        // PULSEFORGE_1_0_0_OVERKILL_GLITCH_COMPAT_V1\n'
    '        register_host_command("addGlitchEffect");\n',
)

# Exact fractional timing in the desktop runtime.
app = Path("src/app/application.cpp")
text = app.read_text(encoding="utf-8")
marker = "        [[nodiscard]] bool get_property(\n"
if text.count(marker) != 1:
    raise SystemExit(f"application.cpp get_property marker count={text.count(marker)}")
insert = (
    "        [[nodiscard]] double current_decimal_beat() const noexcept override {\n"
    "            return application_.gameplay_timing().beat_at(\n"
    "                application_.gameplay_song_time_ms()\n"
    "            );\n"
    "        }\n"
    "        [[nodiscard]] double current_decimal_step() const noexcept override {\n"
    "            return application_.gameplay_timing().step_at(\n"
    "                application_.gameplay_song_time_ms()\n"
    "            );\n"
    "        }\n\n"
)
app.write_text(text.replace(marker, insert + marker, 1), encoding="utf-8")

# Psych accepts setScrollFactor(tag) and treats omitted factors as 1,1.
replace_once(
    "src/app/application.cpp",
    "            double x{1.0}, y{1.0};\n"
    "            if (!string_arg(0U, tag) || !number_arg(1U, x)) {\n"
    "                error = \"setScrollFactor/setLuaSpriteScrollFactor expects tag, x, y\";\n"
    "                return false;\n"
    "            }\n"
    "            if (arguments.size() > 2U) {\n",
    "            double x{1.0}, y{1.0};\n"
    "            if (!string_arg(0U, tag)) {\n"
    "                error = \"setScrollFactor/setLuaSpriteScrollFactor expects a tag\";\n"
    "                return false;\n"
    "            }\n"
    "            if (arguments.size() > 1U && !number_arg(1U, x)) {\n"
    "                error = \"setScrollFactor/setLuaSpriteScrollFactor x must be a finite number\";\n"
    "                return false;\n"
    "            }\n"
    "            if (arguments.size() > 2U) {\n",
)

# addGlitchEffect is a shader helper used by the Overkill/Timeless stage. SDL's
# portable renderer has no source-equivalent shader, so use the already bounded
# renderer-native wave mesh as a deterministic approximation instead of aborting
# the Lua callback. Convert legacy intensity units to a small width fraction.
replace_once(
    "src/app/application.cpp",
    "        // PULSEFORGE_P1_1_11_WAVY_EFFECT_BRIDGE_V1\n",
    "        // PULSEFORGE_1_0_0_OVERKILL_GLITCH_COMPAT_V1\n"
    "        if (name == \"addGlitchEffect\") {\n"
    "            std::string_view tag;\n"
    "            double intensity{}, frequency{}, speed{};\n"
    "            if (!string_arg(0U, tag)) {\n"
    "                error = \"addGlitchEffect expects a sprite tag\";\n"
    "                return false;\n"
    "            }\n"
    "            if (arguments.size() > 1U && !number_arg(1U, intensity)) {\n"
    "                error = \"addGlitchEffect intensity must be a finite number\";\n"
    "                return false;\n"
    "            }\n"
    "            if (arguments.size() > 2U && !number_arg(2U, frequency)) {\n"
    "                error = \"addGlitchEffect frequency must be a finite number\";\n"
    "                return false;\n"
    "            }\n"
    "            if (arguments.size() > 3U && !number_arg(3U, speed)) {\n"
    "                error = \"addGlitchEffect speed must be a finite number\";\n"
    "                return false;\n"
    "            }\n"
    "            const double amplitude = std::clamp(intensity * 0.01, -0.12, 0.12);\n"
    "            if (scene_ == nullptr || !scene_->script_set_wavy_effect(\n"
    "                    tag, amplitude, frequency, speed\n"
    "                )) {\n"
    "                error = \"addGlitchEffect sprite tag was not found\";\n"
    "                return false;\n"
    "            }\n"
    "            error.clear();\n"
    "            return true;\n"
    "        }\n\n"
    "        // PULSEFORGE_P1_1_11_WAVY_EFFECT_BRIDGE_V1\n",
)

# These custom booleans are optional hints in Overkill's post-update animation
# script. Returning false is safer and more compatible than a host-property error
# that disables the entire callback.
replace_once(
    "src/app/application.cpp",
    "        else if (name == \"screenWidth\") value = static_cast<double>(logical_width);\n"
    "        else if (name == \"screenHeight\") value = static_cast<double>(logical_height);\n",
    "        else if (name == \"screenWidth\") value = static_cast<double>(logical_width);\n"
    "        else if (name == \"screenHeight\") value = static_cast<double>(logical_height);\n"
    "        else if (name == \"bfHit\" || name == \"daHit\") value = false;\n",
)

# Sanitized corpus regression: no third-party script body is copied. It verifies
# that the critical Overkill host command is registered and that decimal timing
# globals exist and are fractional.
test = Path("tests/lua_test.cpp")
text = test.read_text(encoding="utf-8")
class_marker = "class RealNoteTypeCorpusHost final : public pulseforge::LuaHostInterface {\n"
if text.count(class_marker) != 1:
    raise SystemExit("lua_test.cpp class marker mismatch")
fixture = r'''class OverkillCompatHost final : public pulseforge::LuaHostInterface {
public:
    [[nodiscard]] double song_position_ms() const noexcept override { return 750.0; }
    [[nodiscard]] std::int64_t current_beat() const noexcept override { return 3; }
    [[nodiscard]] std::int64_t current_step() const noexcept override { return 12; }
    [[nodiscard]] std::int64_t current_section() const noexcept override { return 0; }
    [[nodiscard]] double current_decimal_beat() const noexcept override { return 3.125; }
    [[nodiscard]] double current_decimal_step() const noexcept override { return 12.5; }
    [[nodiscard]] bool get_property(std::string_view, pulseforge::ScriptValue&, std::string& error) const override {
        error = "unused"; return false;
    }
    [[nodiscard]] bool set_property(std::string_view, const pulseforge::ScriptValue&, std::string& error) override {
        error.clear(); return true;
    }
    [[nodiscard]] bool add_score(std::int64_t, std::string& error) override { error.clear(); return true; }
    [[nodiscard]] bool set_health(double, std::string& error) override { error.clear(); return true; }
    [[nodiscard]] bool trigger_event(pulseforge::ScriptEventRequest, std::string& error) override { error.clear(); return true; }
    void debug_print(std::string_view) override {}
    [[nodiscard]] bool invoke_function(
        std::string_view name,
        std::span<const pulseforge::ScriptValue>,
        pulseforge::ScriptValue& result,
        std::string& error
    ) override {
        if (name != "addGlitchEffect") {
            error = "unexpected command";
            return false;
        }
        glitch_called = true;
        result = true;
        error.clear();
        return true;
    }
    bool glitch_called{};
};

void test_overkill_compat_surface() {
    OverkillCompatHost host;
    pulseforge::LuaRuntime runtime(host);
    constexpr auto source = R"lua(
        function onCreate()
            assert(type(curDecBeat) == 'number')
            assert(type(curDecStep) == 'number')
            assert(math.abs(curDecBeat - 3.125) < 0.000001)
            assert(math.abs(curDecStep - 12.5) < 0.000001)
            assert(addGlitchEffect('bg', 2.25, 5, 0.1) == true)
        end
    )lua";
    require(runtime.load_script(source, "@overkill_compat.lua"),
            "Overkill compatibility fixture loads");
    require(runtime.on_create().succeeded(),
            "Overkill compatibility fixture completes onCreate");
    require(host.glitch_called, "addGlitchEffect reached the bounded host bridge");
}

'''
test.write_text(text.replace(class_marker, fixture + class_marker, 1), encoding="utf-8")
text = test.read_text(encoding="utf-8")
main_marker = "        test_shader_array_bridge();\n"
if text.count(main_marker) != 1:
    raise SystemExit("lua_test.cpp main marker mismatch")
test.write_text(
    text.replace(main_marker, "        test_overkill_compat_surface();\n" + main_marker, 1),
    encoding="utf-8",
)

# Release notes/changelog mention both compatibility fixes.
replace_once(
    "CHANGELOG.md",
    "### Fixed\n\n- Isolates executable Psych/Denpa/SC:R Lua discovery",
    "### Fixed\n\n"
    "- Adds Overkill/Timeless Lua compatibility: `addGlitchEffect` receives a bounded renderer-native fallback, `setScrollFactor(tag)` accepts Psych's omitted factors, and fractional `curDecBeat`/`curDecStep` globals are available.\n"
    "- Isolates executable Psych/Denpa/SC:R Lua discovery",
)
replace_once(
    "docs/RELEASE_NOTES_1.0.0.md",
    "- SC:R / SCReboot gameplay-start freeze fixed by isolating executable script discovery to the selected mod/content roots.\n",
    "- SC:R / SCReboot gameplay-start freeze fixed by isolating executable script discovery to the selected mod/content roots.\n"
    "- Overkill/Timeless compatibility fixed so unsupported glitch helpers no longer abort stage setup, single-argument `setScrollFactor` follows Psych semantics, and decimal beat/step globals drive its modchart motion.\n",
)
