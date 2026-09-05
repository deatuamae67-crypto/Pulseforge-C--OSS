#include "pulseforge/chart.hpp"
#include "pulseforge/gameplay.hpp"
#include "pulseforge/lua_runtime.hpp"
#include "pulseforge/script_manager.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

[[nodiscard]] pulseforge::Chart make_chart() {
    pulseforge::Chart chart;
    chart.title = "Lua sandbox test";
    chart.tempos = {{0.0, 120.0, 4, 4}};
    chart.notes = {
        {1'000.0, 0.0, 0, pulseforge::NoteOwner::player, "normal"},
    };
    chart.normalize();
    return chart;
}


class RecordingShaderHost final : public pulseforge::LuaHostInterface {
public:
    struct Invocation {
        std::string name;
        std::vector<pulseforge::ScriptValue> arguments;
    };

    [[nodiscard]] double song_position_ms() const noexcept override {
        return 0.0;
    }
    [[nodiscard]] std::int64_t current_beat() const noexcept override {
        return 0;
    }
    [[nodiscard]] std::int64_t current_step() const noexcept override {
        return 0;
    }
    [[nodiscard]] std::int64_t current_section() const noexcept override {
        return 0;
    }

    [[nodiscard]] bool get_property(
        std::string_view,
        pulseforge::ScriptValue& value,
        std::string& error
    ) const override {
        value = std::monostate{};
        error = "property unsupported";
        return false;
    }

    [[nodiscard]] bool set_property(
        std::string_view,
        const pulseforge::ScriptValue&,
        std::string& error
    ) override {
        error = "property unsupported";
        return false;
    }

    [[nodiscard]] bool add_score(
        std::int64_t,
        std::string& error
    ) override {
        error.clear();
        return true;
    }

    [[nodiscard]] bool set_health(
        double,
        std::string& error
    ) override {
        error.clear();
        return true;
    }

    [[nodiscard]] bool trigger_event(
        pulseforge::ScriptEventRequest,
        std::string& error
    ) override {
        error.clear();
        return true;
    }

    void debug_print(std::string_view) override {}

    [[nodiscard]] bool invoke_function(
        std::string_view name,
        std::span<const pulseforge::ScriptValue> arguments,
        pulseforge::ScriptValue& result,
        std::string& error
    ) override {
        invocations.push_back(Invocation{
            std::string(name),
            std::vector<pulseforge::ScriptValue>(
                arguments.begin(),
                arguments.end()
            ),
        });
        result = true;
        error.clear();
        return true;
    }

    std::vector<Invocation> invocations;
};

class OverkillCompatHost final : public pulseforge::LuaHostInterface {
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

class RealNoteTypeCorpusHost final : public pulseforge::LuaHostInterface {
public:
    struct NotePrototype final {
        std::string note_type;
        std::string texture;
        double miss_health{0.08};
    };

    struct Invocation final {
        std::string name;
        std::vector<pulseforge::ScriptValue> arguments;
    };

    RealNoteTypeCorpusHost() {
        notes.push_back({"the note", {}, 0.08});
        notes.push_back({"normal", {}, 0.08});
    }

    [[nodiscard]] double song_position_ms() const noexcept override { return 0.0; }
    [[nodiscard]] std::int64_t current_beat() const noexcept override { return 0; }
    [[nodiscard]] std::int64_t current_step() const noexcept override { return 0; }
    [[nodiscard]] std::int64_t current_section() const noexcept override { return 0; }

    [[nodiscard]] bool get_property(
        const std::string_view name,
        pulseforge::ScriptValue& value,
        std::string& error
    ) const override {
        if (name == "unspawnNotes.length") {
            value = static_cast<std::int64_t>(notes.size());
            error.clear();
            return true;
        }
        if (name == "health") {
            value = health;
            error.clear();
            return true;
        }
        error = "unsupported corpus property";
        return false;
    }

    [[nodiscard]] bool set_property(
        const std::string_view name,
        const pulseforge::ScriptValue& value,
        std::string& error
    ) override {
        if (name == "boyfriend.specialAnim" || name == "dad.specialAnim") {
            const auto* boolean = std::get_if<bool>(&value);
            if (boolean == nullptr) {
                error = "specialAnim expects boolean";
                return false;
            }
            if (name == "boyfriend.specialAnim") boyfriend_special = *boolean;
            else dad_special = *boolean;
            error.clear();
            return true;
        }
        if (name == "health") {
            if (const auto* number = std::get_if<double>(&value)) {
                health = *number;
            } else if (const auto* integer = std::get_if<std::int64_t>(&value)) {
                health = static_cast<double>(*integer);
            } else {
                error = "health expects number";
                return false;
            }
            error.clear();
            return true;
        }
        error = "unsupported corpus property";
        return false;
    }

    [[nodiscard]] bool add_score(std::int64_t, std::string& error) override {
        error.clear();
        return true;
    }

    [[nodiscard]] bool set_health(double value, std::string& error) override {
        health = value;
        error.clear();
        return true;
    }

    [[nodiscard]] bool trigger_event(
        pulseforge::ScriptEventRequest,
        std::string& error
    ) override {
        error.clear();
        return true;
    }

    void debug_print(std::string_view) override {}

    [[nodiscard]] bool invoke_function(
        const std::string_view name,
        const std::span<const pulseforge::ScriptValue> arguments,
        pulseforge::ScriptValue& result,
        std::string& error
    ) override {
        const auto string_argument = [&](const std::size_t index)
            -> const std::string* {
            return index < arguments.size()
                ? std::get_if<std::string>(&arguments[index])
                : nullptr;
        };
        const auto index_argument = [&](const std::size_t index)
            -> std::optional<std::size_t> {
            if (index >= arguments.size()) return std::nullopt;
            if (const auto* integer = std::get_if<std::int64_t>(&arguments[index]);
                integer != nullptr && *integer >= 0) {
                return static_cast<std::size_t>(*integer);
            }
            if (const auto* number = std::get_if<double>(&arguments[index]);
                number != nullptr && std::isfinite(*number) && *number >= 0.0
                    && std::trunc(*number) == *number) {
                return static_cast<std::size_t>(*number);
            }
            return std::nullopt;
        };

        if (name == "getPropertyFromGroup") {
            const auto* group = string_argument(0U);
            const auto index = index_argument(1U);
            const auto* property = string_argument(2U);
            if (group == nullptr || *group != "unspawnNotes"
                || !index.has_value() || *index >= notes.size()
                || property == nullptr || *property != "noteType") {
                error = "unsupported corpus group read";
                return false;
            }
            result = notes[*index].note_type;
            error.clear();
            return true;
        }

        if (name == "setPropertyFromGroup") {
            const auto* group = string_argument(0U);
            const auto index = index_argument(1U);
            const auto* property = string_argument(2U);
            if (group == nullptr || *group != "unspawnNotes"
                || !index.has_value() || *index >= notes.size()
                || property == nullptr || arguments.size() < 4U) {
                error = "unsupported corpus group write";
                return false;
            }
            if (*property == "texture") {
                const auto* texture = string_argument(3U);
                if (texture == nullptr) {
                    error = "texture expects string";
                    return false;
                }
                notes[*index].texture = *texture;
            } else if (*property == "missHealth") {
                if (const auto* number = std::get_if<double>(&arguments[3U])) {
                    notes[*index].miss_health = *number;
                } else if (const auto* integer = std::get_if<std::int64_t>(&arguments[3U])) {
                    notes[*index].miss_health = static_cast<double>(*integer);
                } else {
                    error = "missHealth expects number";
                    return false;
                }
            } else {
                error = "unsupported corpus note write";
                return false;
            }
            result = true;
            error.clear();
            return true;
        }

        if (name == "characterPlayAnim" || name == "playSound") {
            action_invocations.push_back({
                std::string(name),
                std::vector<pulseforge::ScriptValue>(
                    arguments.begin(), arguments.end()
                ),
            });
            result = true;
            error.clear();
            return true;
        }

        error = "unsupported corpus host function";
        return false;
    }

    std::vector<NotePrototype> notes;
    std::vector<Invocation> action_invocations;
    double health{1.0};
    bool boyfriend_special{};
    bool dad_special{};
};

void test_shader_array_bridge() {
    // PULSEFORGE_P1_1_15_SHADER_ARRAY_TEST_V1
    RecordingShaderHost host;
    pulseforge::LuaRuntime runtime(host);

    constexpr auto script = R"lua(
        function onCreate()
            assert(setShaderFloatArray ~= nil)
            assert(setShaderIntArray ~= nil)
            assert(setShaderFloatArray('pixel', 'uBlocksize', {8.5, 12.25}) == true)
            assert(setShaderIntArray('other', 'indices', {1, 2, 3}) == true)
        end
    )lua";

    require(runtime.load_script(script, "@shader_array_test.lua"),
            "shader array script loads");
    require(runtime.on_create().succeeded(),
            "shader array compatibility calls succeed");
    require(host.invocations.size() == 2U,
            "two shader array host calls were recorded");

    const auto& floats = host.invocations[0];
    require(floats.name == "setShaderFloatArray",
            "float array host function name is preserved");
    require(floats.arguments.size() == 4U,
            "float array is flattened to tag/uniform plus two scalars");
    require(std::get<std::string>(floats.arguments[0]) == "pixel"
            && std::get<std::string>(floats.arguments[1]) == "uBlocksize",
            "float array tag/uniform are preserved");
    require(std::abs(std::get<double>(floats.arguments[2]) - 8.5) < 0.000001
            && std::abs(std::get<double>(floats.arguments[3]) - 12.25) < 0.000001,
            "float array values are preserved");

    const auto& integers = host.invocations[1];
    require(integers.name == "setShaderIntArray",
            "int array host function name is preserved");
    require(integers.arguments.size() == 5U,
            "int array is flattened to tag/uniform plus three scalars");
    require(std::get<std::int64_t>(integers.arguments[2]) == 1
            && std::get<std::int64_t>(integers.arguments[4]) == 3,
            "integer array values are preserved");
}

void test_sandbox_and_callbacks() {
    auto chart = make_chart();
    pulseforge::GameplaySession session(chart);
    pulseforge::GameplayScriptState state;
    pulseforge::GameplayLuaHost host(session, state);
    pulseforge::LuaRuntimeConfig config;
    config.memory_limit_bytes = 2U * 1024U * 1024U;
    config.instruction_budget = 100'000;
    config.instruction_hook_interval = 100;
    pulseforge::LuaRuntime runtime(host, config);

    constexpr auto script = R"lua(
        function onCreate()
            assert(os == nil)
            assert(io == nil)
            assert(package == nil)
            assert(debug == nil)
            assert(dofile == nil)
            assert(loadfile == nil)
            assert(require == nil)
            assert(setmetatable == nil)
            assert(getmetatable == nil)
            assert(table.move == nil)
            assert(table.concat == nil)
            assert(string.dump == nil)
            assert(string.find == nil)
            assert(string.match == nil)
            assert(string.gmatch == nil)
            assert(string.gsub == nil)
            assert(math.randomseed == nil)
            addScore(125)
            setHealth(1.5)
            setProperty("scrollSpeed", 2.0)
            triggerEvent("SandboxReady", "yes", "")
            debugPrint("sandbox ready")
        end

        function onUpdate(elapsed)
            assert(type(elapsed) == "number")
        end

        function onCreatePost()
            addScore(32)
        end

        function onUpdatePost(elapsed)
            assert(type(elapsed) == "number")
            addScore(64)
        end

        function onSongStart()
            addScore(1)
        end

        function onBeatHit(beat)
            if beat == 7 then
                addScore(2)
            end
        end

        function onStepHit(step)
            if step == 28 then
                addScore(4)
            end
        end


        function onSectionHit(section)
            if section == 3 then
                addScore(8)
            end
        end

        function goodNoteHit(noteId, lane, noteType, isSustain)
            assert(noteId == 0)
            assert(lane == 0)
            assert(noteType == "normal")
            assert(isSustain == false)
            addScore(10)
        end


        function opponentNoteHit(noteId, lane, noteType, isSustain)
            assert(noteId == 0)
            assert(lane == 0)
            assert(noteType == "normal")
            assert(isSustain == false)
            addScore(16)
        end

        function noteMiss(noteId, lane, noteType, isSustain)
            assert(noteId == 0)
            assert(lane == 0)
            assert(noteType == "normal")
            assert(isSustain == false)
            addScore(8)
        end


        function noteMissPress(lane)
            assert(lane == 2)
            addScore(4)
        end

        function onGhostTap(lane)
            assert(lane == 2)
            addScore(2)
        end

        function onEndSong()
            addScore(1)
        end

        function onGameOver()
            addScore(1)
        end

        function onPause()
            return Function_StopLua
        end

        function onResume()
            addScore(1)
        end

        function onEvent(name, value1, value2)
            assert(name == "TestEvent")
            assert(value1 == "alpha")
            assert(value2 == "beta")
            addScore(16)
        end

        function onDestroy()
            debugPrint("destroyed")
        end
    )lua";

    require(runtime.load_script(script, "@sandbox_test.lua"), "Lua script loads");
    require(runtime.on_create().succeeded(), "onCreate succeeds");
    require(runtime.on_create_post().succeeded(), "onCreatePost succeeds");
    require(session.summary().score == 157, "Lua score mutation is authoritative");
    require(session.summary().health == 1.5, "Lua health mutation is authoritative");
    require(session.settings().scroll_speed == 2.0, "whitelisted setting mutation");
    require(state.pending_events.size() == 1, "triggerEvent enters bounded queue");
    require(state.debug_messages.size() == 1, "debugPrint enters bounded queue");

    require(runtime.on_song_start().succeeded(), "onSongStart succeeds");
    require(runtime.on_beat_hit(7).succeeded(), "onBeatHit succeeds");
    require(runtime.on_step_hit(28).succeeded(), "onStepHit succeeds");
    require(runtime.on_section_hit(3).succeeded(), "onSectionHit succeeds");
    require(
        runtime.opponent_note_hit(0, 0, "normal", false).succeeded(),
        "opponentNoteHit succeeds"
    );
    require(
        runtime.note_miss(0, 0, "normal", false).succeeded(),
        "noteMiss succeeds"
    );
    require(runtime.note_miss_press(2).succeeded(), "noteMissPress succeeds");
    require(runtime.on_ghost_tap(2).succeeded(), "onGhostTap succeeds");
    require(
        runtime.on_event("TestEvent", "alpha", "beta").succeeded(),
        "onEvent succeeds"
    );
    require(runtime.on_end_song().succeeded(), "onEndSong succeeds");
    require(runtime.on_game_over().succeeded(), "onGameOver succeeds");
    require(
        runtime.on_pause().status == pulseforge::LuaCallStatus::stop_requested,
        "onPause stop request is preserved"
    );
    require(runtime.on_resume().succeeded(), "onResume succeeds");
    require(session.summary().score == 221, "all direct callbacks ran");

    state.clear_transient_output();
    session.begin_frame();
    session.update(1'000.0);
    session.press(0, 1'000.0);
    const auto report = runtime.dispatch_frame(session, 1.0 / 240.0);
    require(report.failed == 0, "frame dispatch succeeds");
    require(
        session.summary().score == 645,
        "goodNoteHit and onUpdatePost callbacks ran"
    );

    require(runtime.on_destroy().succeeded(), "onDestroy succeeds");
    require(
        state.debug_messages.size() == 1
            && state.debug_messages.front() == "destroyed",
        "onDestroy output is preserved"
    );
}

void test_real_custom_notetype_corpus() {
    // PULSEFORGE_P1_5_0D_REAL_CUSTOM_NOTETYPE_CORPUS_TEST_V1
    // Exact real-world custom_notetypes/the note.lua fixture supplied in the
    // corpus. Preserve its in-source `the note` vs `Sex Note` mismatch.
    RealNoteTypeCorpusHost host;
    pulseforge::LuaRuntime runtime(host);

    constexpr auto source = R"lua(function onCreate()
	--Iterate over all notes
	for i = 0, getProperty('unspawnNotes.length')-1 do
		--Check if the note is a Bullet Note
		if getPropertyFromGroup('unspawnNotes', i, 'noteType') == 'the note' then
			setPropertyFromGroup('unspawnNotes', i, 'texture', 'BULLET'); --Change texture
			setPropertyFromGroup('unspawnNotes', i, 'missHealth', 0.6); --Change amount of health to take when you miss like a fucking moron
		end
	end
	--debugPrint('Script started!')
end

-- Function called when you hit a note (after note hit calculations)
-- id: The note member id, you can get whatever variable you want from this note, example: "getPropertyFromGroup('notes', id, 'strumTime')"
-- noteData: 0 = Left, 1 = Down, 2 = Up, 3 = Right
-- noteType: The note type string/tag
-- isSustainNote: If it's a hold note, can be either true or false
-- taken from Fanmade Gunfight Hank mod

dodgeAnimations = {'dodge', 'dodge', 'dodge', 'dodge'}
function goodNoteHit(id, noteData, noteType, isSustainNote)
	if noteType == 'the note' then
		characterPlayAnim('boyfriend', dodgeAnimations[noteData+1], true);
		setProperty('boyfriend.specialAnim', true);

		local animToPlay = '';
		if noteData == 0 then
			animToPlay = 'Shoot';
		elseif noteData == 1 then
			animToPlay = 'Shoot';
		elseif noteData == 2 then
			animToPlay = 'Shoot';
		elseif noteData == 3 then
			animToPlay = 'Shoot';
		end
		characterPlayAnim('dad', animToPlay, true);
		setProperty('dad.specialAnim', true);
		playSound('gunshot', 1)
	end
end

local healthDrain = 0;
function noteMiss(id, noteData, noteType, isSustainNote)
	if noteType == 'Sex Note' then
		-- bf anim
		characterPlayAnim('boyfriend', 'hurt', true);
		setProperty('boyfriend.specialAnim', true);

		-- dad anim
		characterPlayAnim('dad', animToPlay, true);
		setProperty('dad.specialAnim', true);

		-- health loss | || || |_
		--setProperty('health', getProperty('health') - 0.6);
		healthDrain = healthDrain + 0.6;
		--sex sounds
		playSound('ouch', 1)
	end
end

function onUpdate(elapsed)
	if healthDrain > 0 then
		healthDrain = healthDrain - 0.2 * elapsed;
		setProperty('health', getProperty('health') - 0.2 * elapsed);
		if healthDrain < 0 then
			healthDrain = 0;
		end
	end
end)lua";

    require(runtime.load_script(source, "@the note.lua"),
            "real custom NoteType corpus script loads");
    require(runtime.on_create().succeeded(),
            "real custom NoteType onCreate executes");
    require(
        host.notes[0].texture == "BULLET"
            && std::abs(host.notes[0].miss_health - 0.6) < 0.000001,
        "the note.lua applies BULLET texture and missHealth=0.6"
    );
    require(
        host.notes[1].texture.empty()
            && std::abs(host.notes[1].miss_health - 0.08) < 0.000001,
        "the note.lua does not mutate unrelated note types"
    );

    require(
        runtime.good_note_hit(17, 2, "the note", false).succeeded(),
        "real the note goodNoteHit callback succeeds"
    );
    require(
        host.boyfriend_special && host.dad_special,
        "real the note callback preserves both specialAnim writes"
    );
    require(
        host.action_invocations.size() == 3U
            && host.action_invocations[0].name == "characterPlayAnim"
            && host.action_invocations[1].name == "characterPlayAnim"
            && host.action_invocations[2].name == "playSound",
        "real the note callback preserves boyfriend/dad animation and gunshot order"
    );
    require(
        std::get<std::string>(host.action_invocations[0].arguments[0]) == "boyfriend"
            && std::get<std::string>(host.action_invocations[0].arguments[1]) == "dodge"
            && std::get<std::string>(host.action_invocations[1].arguments[0]) == "dad"
            && std::get<std::string>(host.action_invocations[1].arguments[1]) == "Shoot"
            && std::get<std::string>(host.action_invocations[2].arguments[0]) == "gunshot",
        "real the note callback arguments are preserved exactly"
    );

    host.action_invocations.clear();
    host.boyfriend_special = false;
    host.dad_special = false;
    require(
        runtime.note_miss(17, 2, "the note", false).succeeded(),
        "real the note miss callback accepts the exact noteType"
    );
    require(
        host.action_invocations.empty()
            && !host.boyfriend_special && !host.dad_special,
        "source inconsistency is preserved: noteMiss branches on Sex Note, not the note"
    );

    // PULSEFORGE_P1_5_0E_REAL_NOTETYPE_MANAGER_GAMEPLAY_E2E_TEST_V1
    // Re-run the exact corpus source through the same manager/frame bridge used
    // by gameplay.  This verifies that noteType/isSustain are obtained from real
    // GameplayEvents rather than being supplied manually by this test.
    RealNoteTypeCorpusHost manager_host;
    pulseforge::LuaScriptManager manager(manager_host);
    const std::array definitions{pulseforge::LuaScriptDefinition{
        "custom_notetypes/the note.lua",
        source,
        pulseforge::ScriptOrigin::note_type,
        0,
    }};
    const auto loaded = manager.load_scripts(definitions);
    require(
        loaded.loaded == 1U && loaded.failed == 0U,
        "real custom_notetypes/the note.lua loads through LuaScriptManager"
    );
    require(
        manager.on_create().callbacks.failed == 0U,
        "real NoteType manager onCreate succeeds"
    );

    pulseforge::Chart e2e_chart;
    e2e_chart.title = "Real NoteType manager E2E";
    e2e_chart.tempos = {{0.0, 120.0, 4, 4}};
    e2e_chart.notes = {
        {100.0, 0.0, 2, pulseforge::NoteOwner::player, "the note"},
        {200.0, 0.0, 1, pulseforge::NoteOwner::opponent, "the note"},
        {300.0, 0.0, 0, pulseforge::NoteOwner::player, "the note"},
    };
    e2e_chart.normalize();
    pulseforge::GameplaySettings e2e_settings;
    e2e_settings.no_fail = true;
    pulseforge::GameplaySession e2e_session(e2e_chart, e2e_settings);

    manager_host.action_invocations.clear();
    manager_host.boyfriend_special = false;
    manager_host.dad_special = false;
    e2e_session.begin_frame();
    e2e_session.press(2U, 100.0);
    const auto hit_dispatch = manager.dispatch_frame(e2e_session, 1.0 / 240.0);
    require(
        hit_dispatch.callbacks.failed == 0U
            && manager_host.action_invocations.size() == 3U
            && manager_host.action_invocations[2].name == "playSound"
            && std::get<std::string>(
                manager_host.action_invocations[2].arguments[0]
            ) == "gunshot",
        "real gameplay note_hit reaches the NoteType manager with the exact corpus semantics"
    );

    manager_host.action_invocations.clear();
    manager_host.boyfriend_special = false;
    manager_host.dad_special = false;
    e2e_session.begin_frame();
    e2e_session.update(600.0);
    const auto tail_dispatch = manager.dispatch_frame(e2e_session, 1.0 / 240.0);
    require(
        tail_dispatch.callbacks.failed == 0U
            && manager_host.action_invocations.empty()
            && !manager_host.boyfriend_special && !manager_host.dad_special,
        "opponent/miss gameplay callbacks preserve the corpus script's exact branch behavior"
    );
}

void test_start_countdown_lifecycle() {
    // PULSEFORGE_P1_1_4_COUNTDOWN_TEST_V1
    auto chart = make_chart();
    pulseforge::GameplaySession session(chart);
    pulseforge::GameplayScriptState state;
    pulseforge::GameplayLuaHost host(session, state);
    pulseforge::LuaRuntime runtime(host);

    constexpr auto script = R"lua(
        local allowCountdown = false

        function onCreate()
            function onStartCountdown()
                if not allowCountdown then
                    allowCountdown = true
                    return Function_Stop
                end
                return Function_Continue
            end
        end
    )lua";

    require(runtime.load_script(script, "@countdown_test.lua"),
            "countdown script loads");
    require(runtime.on_create().succeeded(), "countdown onCreate succeeds");
    require(
        runtime.on_start_countdown().status
            == pulseforge::LuaCallStatus::stop_requested,
        "first onStartCountdown preserves Function_Stop"
    );
    require(
        runtime.on_start_countdown().status
            == pulseforge::LuaCallStatus::completed,
        "second onStartCountdown continues"
    );

    pulseforge::GameplayScriptState manager_state;
    pulseforge::GameplayLuaHost manager_host(session, manager_state);
    pulseforge::LuaScriptManager manager(manager_host);
    const std::vector<pulseforge::LuaScriptDefinition> definitions{{
        "@countdown_manager.lua",
        script,
        pulseforge::ScriptOrigin::global,
        0,
    }};
    const auto loaded = manager.load_scripts(definitions);
    require(loaded.loaded == 1, "countdown manager loads script");
    require(manager.on_create().callbacks.failed == 0,
            "countdown manager onCreate succeeds");
    require(manager.on_start_countdown().stop_requested,
            "countdown manager preserves stop request");
}

void test_deterministic_random() {
    auto chart_a = make_chart();
    auto chart_b = make_chart();
    pulseforge::GameplaySession session_a(chart_a);
    pulseforge::GameplaySession session_b(chart_b);
    pulseforge::GameplayScriptState state_a;
    pulseforge::GameplayScriptState state_b;
    pulseforge::GameplayLuaHost host_a(session_a, state_a);
    pulseforge::GameplayLuaHost host_b(session_b, state_b);
    pulseforge::LuaRuntimeConfig config;
    config.memory_limit_bytes = 2U * 1024U * 1024U;
    config.deterministic_seed = 0x123456789ABCDEF0ULL;
    pulseforge::LuaRuntime runtime_a(host_a, config);
    pulseforge::LuaRuntime runtime_b(host_b, config);
    constexpr auto script = R"lua(
        function onCreate()
            assert(math.randomseed == nil)
            debugPrint(tostring(math.random()) .. ":" .. tostring(math.random(1000000)))
        end
    )lua";

    require(runtime_a.load_script(script, "@random_a.lua"), "first PRNG script loads");
    require(runtime_b.load_script(script, "@random_b.lua"), "second PRNG script loads");
    require(runtime_a.on_create().succeeded(), "first PRNG callback succeeds");
    require(runtime_b.on_create().succeeded(), "second PRNG callback succeeds");
    require(
        state_a.debug_messages == state_b.debug_messages
            && state_a.debug_messages.size() == 1,
        "equal seeds produce equal Lua random sequences"
    );
}

void test_instruction_budget() {
    auto chart = make_chart();
    pulseforge::GameplaySession session(chart);
    pulseforge::GameplayScriptState state;
    pulseforge::GameplayLuaHost host(session, state);
    pulseforge::LuaRuntimeConfig config;
    config.memory_limit_bytes = 512U * 1024U;
    config.instruction_budget = 10'000;
    config.instruction_hook_interval = 100;
    pulseforge::LuaRuntime runtime(host, config);

    constexpr auto script = R"lua(
        function onUpdate(elapsed)
            local total = 0
            while true do
                total = total + 1
            end
        end
    )lua";
    require(runtime.load_script(script, "@budget_test.lua"), "budget script loads");
    const auto result = runtime.on_update(1.0 / 60.0);
    require(result.status == pulseforge::LuaCallStatus::failed, "infinite callback stopped");
    require(!runtime.diagnostics().empty(), "budget violation is diagnosed");
}

void test_multi_script_manager() {
    auto chart = make_chart();
    pulseforge::GameplaySession session(chart);
    pulseforge::GameplayScriptState state;
    pulseforge::GameplayLuaHost host(session, state);
    pulseforge::LuaRuntimeConfig config;
    config.memory_limit_bytes = 512U * 1024U;
    config.instruction_budget = 50'000;
    config.instruction_hook_interval = 100;
    pulseforge::LuaScriptManager manager(host, config);

    const std::vector<pulseforge::LuaScriptDefinition> scripts{
        {
            "@song.lua",
            "function onCreate() debugPrint('song') end\n"
            "function onPause() return Function_StopLua end\n",
            pulseforge::ScriptOrigin::song,
            0,
        },
        {
            "@global.lua",
            "function onCreate() debugPrint('global') end\n",
            pulseforge::ScriptOrigin::global,
            0,
        },
        {
            "@broken.lua",
            "function onCreate( this is not valid Lua",
            pulseforge::ScriptOrigin::stage,
            0,
        },
        {
            "@GLOBAL.lua",
            "function onCreate() debugPrint('duplicate') end\n",
            pulseforge::ScriptOrigin::global,
            0,
        },
    };

    const auto loaded = manager.load_scripts(scripts);
    require(loaded.requested == 4, "manager counts requested scripts");
    require(loaded.loaded == 2, "manager keeps valid isolated scripts");
    require(loaded.failed == 1, "manager isolates load failure");
    require(loaded.duplicates == 1, "manager de-duplicates source names");
    require(manager.loaded_count() == 2, "manager exposes active script count");
    require(!manager.diagnostics().empty(), "manager attributes load diagnostics");

    const auto created = manager.on_create();
    require(created.callbacks.failed == 0, "manager lifecycle succeeds");
    require(
        state.debug_messages.size() == 2
            && state.debug_messages[0] == "global"
            && state.debug_messages[1] == "song",
        "manager applies deterministic Psych scope order"
    );
    require(manager.on_pause().stop_requested, "manager preserves stop semantics");
    static_cast<void>(manager.on_destroy());
    manager.unload();
    require(manager.loaded_count() == 0, "manager unload releases all states");
}


void test_psych_core_compat_batch() {
    // PULSEFORGE_P1_1_16_PSYCH_CORE_COMPAT_TEST_V1
    auto chart = make_chart();
    pulseforge::GameplaySession session(chart);
    pulseforge::GameplayScriptState state;
    pulseforge::GameplayLuaHost host(session, state);

    pulseforge::LuaRuntimeConfig config;
    config.memory_limit_bytes = 2U * 1024U * 1024U;
    config.instruction_budget = 150'000;
    config.instruction_hook_interval = 100;
    config.deterministic_seed = 0x123456789ABCDEF0ULL;
    pulseforge::LuaRuntime runtime(host, config);

    constexpr auto script = R"lua(
        function onCreate()
            assert(type(getRandomInt) == 'function')
            assert(type(getRandomFloat) == 'function')
            assert(type(getRandomBool) == 'function')
            assert(type(getColorFromHex) == 'function')
            assert(type(getColorFromString) == 'function')
            assert(type(stringStartsWith) == 'function')
            assert(type(stringEndsWith) == 'function')
            assert(type(stringTrim) == 'function')
            assert(type(stringSplit) == 'function')

            local ri = getRandomInt(4, 9, '5,7')
            assert(ri >= 4 and ri <= 9 and ri ~= 5 and ri ~= 7)
            local rf = getRandomFloat(-2.0, 3.0)
            assert(rf >= -2.0 and rf <= 3.0)
            assert(getRandomBool(100) == true)
            assert(getRandomBool(0) == false)

            assert(getColorFromHex('FF00AA') == 0xFFFF00AA)
            assert(getColorFromString('red') == 0xFFFF0000)
            assert(stringStartsWith('PulseForge', 'Pulse') == true)
            assert(stringEndsWith('PulseForge', 'Forge') == true)
            assert(stringTrim('  abc \n') == 'abc')
            local parts = stringSplit('a,b,c', ',')
            assert(parts[1] == 'a' and parts[2] == 'b' and parts[3] == 'c')

            -- Application-side commands are registered even when this isolated
            -- unit-test host intentionally does not implement their renderer.
            assert(type(getPropertyFromClass) == 'function')
            assert(type(setPropertyFromClass) == 'function')
            assert(type(loadGraphic) == 'function')
            assert(type(precacheImage) == 'function')
            assert(type(setBlendMode) == 'function')
            assert(type(luaTextExists) == 'function')
            assert(type(removeLuaText) == 'function')
            assert(type(getTextString) == 'function')
            assert(type(setTextColor) == 'function')
            assert(type(setTextBorder) == 'function')
            assert(type(setTextWidth) == 'function')
            assert(type(setTextItalic) == 'function')
            assert(type(setTextFont) == 'function')
            assert(type(setHealthBarColors) == 'function')
            assert(type(setTimeBarColors) == 'function')
            assert(type(doTweenScaleX) == 'function')
            assert(type(doTweenScaleY) == 'function')
            assert(type(getGraphicMidpointX) == 'function')
            assert(type(getGraphicMidpointY) == 'function')

            -- P1.1.17: the lazy Psych namespace is no longer numeric-only.
            assert(songName == 'Lua sandbox test')
            assert(type(difficultyName) == 'string')
            assert(type(scrollSpeed) == 'number')
            assert(type(downscroll) == 'boolean')
            assert(type(ratingFC) == 'string')
        end
    )lua";

    require(
        runtime.load_script(script, "@psych_core_compat_batch.lua"),
        "Psych core compatibility batch script loads"
    );
    require(
        runtime.on_create().succeeded(),
        "Psych core compatibility utility functions succeed"
    );
}


void test_psych_modchart_runtime_batch() {
    // PULSEFORGE_P1_1_17_MODCHART_RUNTIME_TEST_V1
    auto chart = make_chart();
    pulseforge::GameplaySession session(chart);
    pulseforge::GameplayScriptState state;
    pulseforge::GameplayLuaHost host(session, state);
    pulseforge::LuaRuntime runtime(host);

    constexpr auto script = R"lua(
        function onCreate()
            assert(type(setVar) == 'function')
            assert(type(getVar) == 'function')
            assert(type(removeVar) == 'function')
            assert(type(restartSong) == 'function')
            assert(type(endSong) == 'function')
            assert(type(getSongLength) == 'function')

            assert(type(getCharacterX) == 'function')
            assert(type(getCharacterY) == 'function')
            assert(type(setCharacterX) == 'function')
            assert(type(setCharacterY) == 'function')

            assert(type(addCameraScroll) == 'function')
            assert(type(addCameraFollowPoint) == 'function')
            assert(type(getCameraScrollX) == 'function')
            assert(type(getCameraScrollY) == 'function')
            assert(type(getCameraFollowX) == 'function')
            assert(type(getCameraFollowY) == 'function')

            assert(type(noteTweenScaleX) == 'function')
            assert(type(noteTweenScaleY) == 'function')
        end
    )lua";

    require(
        runtime.load_script(script, "@psych_modchart_runtime_batch.lua"),
        "Psych modchart/runtime batch script loads"
    );
    require(
        runtime.on_create().succeeded(),
        "Psych modchart/runtime functions are registered"
    );
}


void test_psych_audio_dynamic_script_batch() {
    // PULSEFORGE_P1_1_18_AUDIO_DYNAMIC_SCRIPT_TEST_V1
    RecordingShaderHost recording_host;
    pulseforge::LuaRuntime registration_runtime(recording_host);

    constexpr auto registration_script = R"lua(
        function onCreate()
            assert(type(precacheSound) == 'function')
            assert(type(playSound) == 'function')
            assert(type(stopSound) == 'function')
            assert(type(pauseSound) == 'function')
            assert(type(resumeSound) == 'function')
            assert(type(soundFadeIn) == 'function')
            assert(type(soundFadeOut) == 'function')
            assert(type(soundFadeCancel) == 'function')
            assert(type(getSoundTime) == 'function')
            assert(type(setSoundTime) == 'function')
            assert(type(getSoundVolume) == 'function')
            assert(type(setSoundVolume) == 'function')
            assert(type(soundPlaying) == 'function')
            assert(type(playMusic) == 'function')
            assert(type(stopMusic) == 'function')
            assert(type(pauseMusic) == 'function')
            assert(type(resumeMusic) == 'function')
            assert(type(addLuaScript) == 'function')
            assert(type(removeLuaScript) == 'function')
            assert(type(luaScriptExists) == 'function')
        end
    )lua";

    require(
        registration_runtime.load_script(
            registration_script,
            "@audio_dynamic_registration.lua"
        ),
        "Psych audio/dynamic registration script loads"
    );
    require(
        registration_runtime.on_create().succeeded(),
        "Psych audio/dynamic functions are registered"
    );

    auto chart = make_chart();
    pulseforge::GameplaySession session(chart);
    pulseforge::GameplayScriptState state;
    pulseforge::GameplayLuaHost host(session, state);
    pulseforge::LuaScriptManager manager(host);

    constexpr auto dynamic_source = R"lua(
        function onCreate()
            addScore(10)
        end

        function onCreatePost()
            addScore(20)
        end

        function onSongStart()
            addScore(40)
        end

        function onSoundFinished(tag)
            if tag == 'ding' then
                addScore(8)
            end
        end

        function onDestroy()
            addScore(80)
        end
    )lua";

    const std::string dynamic_name{"@dynamic-audio-test.lua"};
    const auto added = manager.add_script({
        dynamic_name,
        dynamic_source,
        pulseforge::ScriptOrigin::explicit_file,
        0,
    });
    require(added.loaded == 1U, "dynamic Lua state can be added");
    require(manager.contains(dynamic_name), "dynamic Lua state is discoverable");

    const auto initialized = manager.initialize_script(dynamic_name, true);
    require(
        initialized.callbacks.failed == 0U,
        "dynamic Lua lifecycle initializes without failures"
    );
    require(
        session.summary().score == 70,
        "dynamic Lua receives onCreate/onCreatePost/onSongStart once"
    );

    const auto sound_finished = manager.on_sound_finished("ding");
    require(
        sound_finished.callbacks.failed == 0U
            && session.summary().score == 78,
        "onSoundFinished dispatch reaches dynamic scripts"
    );

    require(
        manager.remove_script(dynamic_name),
        "dynamic Lua state can be removed"
    );
    require(
        !manager.contains(dynamic_name),
        "removed dynamic Lua state is no longer active"
    );
    require(
        session.summary().score == 158,
        "dynamic Lua removal invokes onDestroy exactly once"
    );
}


void test_note_multiplier_consistency_and_lua_fanout() {
    // PULSEFORGE_P1_1_18_NOTE_MULTIPLIER_REGRESSION_V1
    pulseforge::Chart chart;
    chart.title = "Note multiplier regression";
    chart.tempos = {{0.0, 120.0, 4, 4}};
    chart.events = {
        {1'000.0, "Change Note Multiplier", "1.5", "2", {}},
        {1'100.0, "Note Multiplier", "3", "1", {}},
    };
    chart.notes = {
        {1'000.0, 0.0, 0, pulseforge::NoteOwner::player, "normal"},
        {1'100.0, 0.0, 1, pulseforge::NoteOwner::opponent, "normal"},
    };
    chart.normalize();

    pulseforge::GameplaySettings settings;
    settings.no_fail = true;
    pulseforge::GameplaySession session(chart, settings);
    require(
        session.summary().chart_total == 0U,
        "Chart Total starts at zero before either side resolves a note"
    );

    session.begin_frame();
    session.press(0U, 1'000.0);
    require(
        session.player_note_multiplier() == 2.0,
        "fractional multiplier normalizes once to integral logical polyphony"
    );
    require(
        session.opponent_note_multiplier() == 1.0,
        "player-only multiplier does not alter opponent polyphony"
    );
    require(
        session.summary().score == 700
            && session.summary().combo == 2U
            && session.summary().marvelous == 2U
            && session.summary().chart_total == 2U
            && std::abs(session.summary().judged_notes - 2.0) < 0.000001,
        "score/combo/rating/chart-total/judged counts share one normalized multiplier"
    );

    const auto first_events = session.frame_events();
    require(first_events.size() >= 2U, "same-time multiplier emits chart and note events");
    require(
        first_events[0].type == pulseforge::GameplayEventType::chart_event,
        "same-time chart event is emitted before the note judgment"
    );
    const auto hit = std::find_if(
        first_events.begin(),
        first_events.end(),
        [](const pulseforge::GameplayEvent& event) {
            return event.type == pulseforge::GameplayEventType::note_hit;
        }
    );
    require(hit != first_events.end(), "multiplied player hit event is emitted");
    require(
        hit->logical_occurrence_count == 2U,
        "player hit event carries normalized logical occurrence count"
    );

    pulseforge::GameplayScriptState state;
    pulseforge::GameplayLuaHost host(session, state);
    pulseforge::LuaRuntime runtime(host);
    constexpr auto script = R"lua(
        function onUpdate(elapsed)
            assert(type(noteMultiplier) == 'number')
            assert(type(noteMultiplierPlayer) == 'number')
            assert(type(noteMultiplierOpponent) == 'number')
        end

        function onEvent(name, value1, value2)
            if name == 'Change Note Multiplier' or name == 'Note Multiplier' then
                addScore(100)
            end
        end

        function goodNoteHit(index, direction, noteType, isSustain)
            addScore(1)
        end

        function opponentNoteHit(index, direction, noteType, isSustain)
            addScore(10)
        end
    )lua";
    require(runtime.load_script(script, "@note_multiplier_regression.lua"),
            "note multiplier Lua regression script loads");
    const auto first_dispatch = runtime.dispatch_frame(session, 1.0 / 60.0);
    require(first_dispatch.failed == 0U, "multiplied player Lua fan-out succeeds");
    require(
        session.summary().score == 802,
        "chart onEvent fires once while goodNoteHit follows logical multiplier"
    );

    session.begin_frame();
    session.update(1'100.0);
    require(
        session.opponent_note_multiplier() == 3.0,
        "opponent-only event updates opponent multiplier"
    );
    const auto second_events = session.frame_events();
    const auto opponent = std::find_if(
        second_events.begin(),
        second_events.end(),
        [](const pulseforge::GameplayEvent& event) {
            return event.type == pulseforge::GameplayEventType::opponent_hit;
        }
    );
    require(opponent != second_events.end(), "opponent hit event is emitted");
    require(
        opponent->logical_occurrence_count == 3U
            && session.summary().chart_total == 5U,
        "opponent hit and Chart Total use opponent-side logical multiplier"
    );
    const auto second_dispatch = runtime.dispatch_frame(session, 1.0 / 60.0);
    require(second_dispatch.failed == 0U, "multiplied opponent Lua fan-out succeeds");
    require(
        session.summary().score == 932,
        "opponentNoteHit fan-out follows opponent multiplier without changing player score semantics"
    );

    pulseforge::LuaScriptManager event_manager(host);
    constexpr auto manager_event_source = R"lua(
        function onEvent(name, value1, value2)
            if name == 'ManagerProbe' then
                addScore(7)
            end
        end
    )lua";
    const std::array manager_scripts{pulseforge::LuaScriptDefinition{
        "@note-multiplier-manager-event.lua",
        manager_event_source,
        pulseforge::ScriptOrigin::explicit_file,
        0,
    }};
    const auto manager_load = event_manager.load_scripts(manager_scripts);
    require(manager_load.loaded == 1U, "event manager probe script loads");
    const auto manager_event = event_manager.on_event("ManagerProbe", "", "");
    require(
        manager_event.callbacks.failed == 0U
            && session.summary().score == 939,
        "LuaScriptManager exposes bounded onEvent dispatch for the engine event bus"
    );

    pulseforge::Chart hold_chart;
    hold_chart.title = "Note multiplier sustain regression";
    hold_chart.tempos = {{0.0, 120.0, 4, 4}};
    hold_chart.events = {
        {0.0, "Change Note Multiplier", "2", "player", {}},
    };
    hold_chart.notes = {
        {100.0, 500.0, 0, pulseforge::NoteOwner::player, "normal"},
    };
    hold_chart.normalize();

    pulseforge::GameplaySession hold_session(hold_chart, settings);
    hold_session.press(0U, 100.0);
    hold_session.update(400.0);
    require(
        hold_session.summary().hold_ticks == 6U
            && hold_session.summary().score == 760,
        "sustain ticks and tick score follow logical multiplier"
    );
    hold_session.begin_frame();
    hold_session.release(0U, 400.0);
    require(
        hold_session.summary().hold_drops == 2U
            && hold_session.summary().misses == 2U
            && hold_session.summary().chart_total == 2U,
        "sustain ticks/drops use logical multiplier without double-counting Chart Total"
    );
    const auto drop = std::find_if(
        hold_session.frame_events().begin(),
        hold_session.frame_events().end(),
        [](const pulseforge::GameplayEvent& event) {
            return event.type == pulseforge::GameplayEventType::hold_drop;
        }
    );
    require(
        drop != hold_session.frame_events().end()
            && drop->logical_occurrence_count == 2U,
        "hold-drop event carries logical multiplier"
    );

    // PULSEFORGE_P1_1_19_EYE_OF_GOD_MULTIPLIER_REGRESSION_V1
    constexpr std::uint64_t eye_of_god_multiplier = 21'447'891U;
    pulseforge::Chart huge_chart;
    huge_chart.title = "Eye of God multiplier regression";
    huge_chart.tempos = {{0.0, 120.0, 4, 4}};
    huge_chart.events = {
        {0.0, "Change Note Multiplier", "21447891", "player", {}},
    };
    huge_chart.notes = {
        {100.0, 0.0, 0, pulseforge::NoteOwner::player, "normal"},
    };
    huge_chart.normalize();

    pulseforge::GameplaySession huge_session(huge_chart, settings);
    huge_session.begin_frame();
    huge_session.press(0U, 100.0);
    require(
        huge_session.player_note_multiplier()
            == static_cast<double>(eye_of_god_multiplier),
        "Eye of God multiplier is not clamped to the old one-million ceiling"
    );
    require(
        huge_session.summary().combo == eye_of_god_multiplier
            && huge_session.summary().marvelous == eye_of_god_multiplier
            && huge_session.summary().chart_total == eye_of_god_multiplier
            && huge_session.summary().score == 7'506'761'850LL,
        "large logical multiplier reaches gameplay, Chart Total and score exactly"
    );
    const auto huge_hit = std::find_if(
        huge_session.frame_events().begin(),
        huge_session.frame_events().end(),
        [](const pulseforge::GameplayEvent& event) {
            return event.type == pulseforge::GameplayEventType::note_hit;
        }
    );
    require(
        huge_hit != huge_session.frame_events().end()
            && huge_hit->logical_occurrence_count == eye_of_god_multiplier,
        "large logical multiplier is preserved on gameplay events"
    );

    pulseforge::GameplayScriptState huge_state;
    pulseforge::GameplayLuaHost huge_host(huge_session, huge_state);
    pulseforge::LuaRuntimeConfig huge_config;
    huge_config.max_callbacks_per_frame = 32U;
    pulseforge::LuaRuntime huge_runtime(huge_host, huge_config);
    constexpr auto huge_script = R"lua(
        function goodNoteHit(index, direction, noteType, isSustain)
            addScore(1)
        end
    )lua";
    require(
        huge_runtime.load_script(huge_script, "@eye_of_god_multiplier.lua"),
        "large-multiplier Lua regression script loads"
    );
    const auto before_fanout_score = huge_session.summary().score;
    const auto huge_dispatch = huge_runtime.dispatch_frame(
        huge_session, 1.0 / 60.0
    );
    require(
        huge_dispatch.failed == 0U
            && huge_dispatch.skipped > 0U
            && huge_session.summary().score > before_fanout_score
            && huge_session.summary().score
                <= before_fanout_score + 30,
        "Lua callback fan-out stays bounded independently of logical multiplier"
    );
}

}  // namespace

int main() {
    try {
        test_sandbox_and_callbacks();
        std::cout << "[PASS] Lua sandbox and callbacks\n";
        test_real_custom_notetype_corpus();
        std::cout << "[PASS] real custom NoteType corpus\n";
        test_overkill_compat_surface();
        test_shader_array_bridge();
        std::cout << "[PASS] Lua shader array bridge\n";
        test_start_countdown_lifecycle();
        std::cout << "[PASS] Lua onStartCountdown lifecycle\n";
        test_deterministic_random();
        std::cout << "[PASS] Lua deterministic random\n";
        test_instruction_budget();
        std::cout << "[PASS] Lua instruction budget\n";
        test_multi_script_manager();
        std::cout << "[PASS] Lua multi-script manager\n";
        test_psych_core_compat_batch();
        std::cout << "[PASS] Psych Lua core compatibility batch\n";
        test_psych_modchart_runtime_batch();
        std::cout << "[PASS] Psych modchart/runtime compatibility batch\n";
        test_psych_audio_dynamic_script_batch();
        std::cout << "[PASS] Psych audio/dynamic script lifecycle batch\n";
        test_note_multiplier_consistency_and_lua_fanout();
        std::cout << "[PASS] Note multiplier consistency and Lua fan-out\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
