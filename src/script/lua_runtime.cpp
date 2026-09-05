#include "pulseforge/lua_runtime.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace pulseforge {
namespace {

constexpr std::size_t minimum_lua_memory_bytes = 256U * 1024U;
constexpr std::size_t maximum_property_name_bytes = 256;
constexpr std::size_t maximum_event_name_bytes = 256;
constexpr std::size_t maximum_event_value_bytes = 4U * 1024U;
constexpr std::size_t maximum_debug_message_bytes = 4U * 1024U;

[[nodiscard]] std::int64_t saturating_add(
    const std::int64_t left,
    const std::int64_t right
) noexcept {
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if (right > 0 && left > maximum - right) {
        return maximum;
    }
    if (right < 0 && left < minimum - right) {
        return minimum;
    }
    return left + right;
}

void set_gameplay_score(
    GameplaySession& session,
    const std::int64_t target
) noexcept {
    const std::int64_t current = session.summary().score;
    if (current == std::numeric_limits<std::int64_t>::min()) {
        session.add_score(std::numeric_limits<std::int64_t>::max());
        session.add_score(1);
    } else {
        session.add_score(-current);
    }
    session.add_score(target);
}

[[nodiscard]] std::int64_t floor_to_int64(const double value) noexcept {
    if (!std::isfinite(value)) {
        return 0;
    }
    const double floored = std::floor(value);
    const auto minimum = std::numeric_limits<std::int64_t>::min();
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    if (floored <= static_cast<double>(minimum)) {
        return minimum;
    }
    // INT64_MAX rounds to 2^63 as a double, so equality must be handled
    // before the conversion as well.
    if (floored >= static_cast<double>(maximum)) {
        return maximum;
    }
    return static_cast<std::int64_t>(floored);
}

[[nodiscard]] std::int64_t script_counter(
    const std::uint64_t value
) noexcept {
    constexpr auto maximum = static_cast<std::uint64_t>(
        std::numeric_limits<std::int64_t>::max()
    );
    return value > maximum
        ? std::numeric_limits<std::int64_t>::max()
        : static_cast<std::int64_t>(value);
}

[[nodiscard]] bool script_number(
    const ScriptValue& value,
    double& result
) noexcept {
    if (const auto* number = std::get_if<double>(&value)) {
        result = *number;
        return std::isfinite(result);
    }
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        result = static_cast<double>(*integer);
        return true;
    }
    return false;
}

[[nodiscard]] bool script_integer(
    const ScriptValue& value,
    std::int64_t& result
) noexcept {
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        result = *integer;
        return true;
    }
    if (const auto* number = std::get_if<double>(&value)) {
        if (!std::isfinite(*number)
            || std::trunc(*number) != *number
            || *number < static_cast<double>(
                std::numeric_limits<std::int64_t>::min()
            )
            || *number >= static_cast<double>(
                std::numeric_limits<std::int64_t>::max()
            )) {
            return false;
        }
        result = static_cast<std::int64_t>(*number);
        return true;
    }
    return false;
}

[[nodiscard]] bool script_boolean(
    const ScriptValue& value,
    bool& result
) noexcept {
    if (const auto* boolean = std::get_if<bool>(&value)) {
        result = *boolean;
        return true;
    }
    return false;
}

[[nodiscard]] bool matches(
    const std::string_view value,
    const std::initializer_list<std::string_view> choices
) noexcept {
    return std::find(choices.begin(), choices.end(), value) != choices.end();
}

void set_type_error(std::string& error, const std::string_view expected) {
    error = "property expects ";
    error.append(expected);
}

void accumulate(
    LuaDispatchReport& report,
    const LuaCallResult result
) noexcept {
    switch (result.status) {
    case LuaCallStatus::completed:
        ++report.completed;
        break;
    case LuaCallStatus::stop_requested:
        ++report.stop_requests;
        break;
    case LuaCallStatus::failed:
        ++report.failed;
        break;
    case LuaCallStatus::not_loaded:
    case LuaCallStatus::missing:
    case LuaCallStatus::disabled:
        ++report.skipped;
        break;
    }
}

}  // namespace

void GameplayScriptState::reset() noexcept {
    clear_transient_output();
}

void GameplayScriptState::clear_transient_output() noexcept {
    pending_events.clear();
    debug_messages.clear();
}

std::int64_t GameplayScriptState::effective_score(
    const GameplaySession& session
) const noexcept {
    return session.summary().score;
}

double GameplayScriptState::effective_health(
    const GameplaySession& session
) const noexcept {
    return session.summary().health;
}

GameplayLuaHost::GameplayLuaHost(
    GameplaySession& session,
    GameplayScriptState& state
) noexcept
    : session_(session),
      state_(state) {
}

double GameplayLuaHost::song_position_ms() const noexcept {
    return session_.song_time_ms();
}

std::int64_t GameplayLuaHost::current_beat() const noexcept {
    return floor_to_int64(
        session_.timing_map().beat_at(session_.song_time_ms())
    );
}

std::int64_t GameplayLuaHost::current_step() const noexcept {
    return floor_to_int64(
        session_.timing_map().step_at(session_.song_time_ms())
    );
}

std::int64_t GameplayLuaHost::current_section() const noexcept {
    return floor_to_int64(
        session_.timing_map().step_at(session_.song_time_ms()) / 16.0
    );
}

double GameplayLuaHost::current_decimal_beat() const noexcept {
    return session_.timing_map().beat_at(session_.song_time_ms());
}

double GameplayLuaHost::current_decimal_step() const noexcept {
    return session_.timing_map().step_at(session_.song_time_ms());
}

bool GameplayLuaHost::get_property(
    const std::string_view name,
    ScriptValue& value,
    std::string& error
) const {
    const auto& summary = session_.summary();
    const auto& settings = session_.settings();

    if (matches(name, {"health"})) {
        value = state_.effective_health(session_);
    } else if (matches(name, {"score", "songScore"})) {
        value = state_.effective_score(session_);
    } else if (matches(name, {"combo"})) {
        value = script_counter(summary.combo);
    } else if (matches(name, {"maxCombo"})) {
        value = script_counter(summary.max_combo);
    } else if (matches(name, {"misses", "songMisses"})) {
        value = script_counter(summary.misses);
    } else if (matches(name, {"ratingPercent"})) {
        value = summary.accuracy_percent() / 100.0;
    } else if (matches(name, {"accuracy"})) {
        value = summary.accuracy_percent();
    } else if (matches(name, {"ratingFC"})) {
        value = std::string(summary.clear_type());
    } else if (matches(name, {"songPosition"})) {
        value = session_.song_time_ms();
    } else if (matches(name, {"curBeat"})) {
        value = current_beat();
    } else if (matches(name, {"curStep"})) {
        value = current_step();
    } else if (matches(name, {"curSection"})) {
        value = current_section();
    } else if (matches(name, {"curDecBeat"})) {
        value = current_decimal_beat();
    } else if (matches(name, {"curDecStep"})) {
        value = current_decimal_step();
    } else if (matches(name, {"bpm", "curBpm"})) {
        value = session_.timing_map().bpm_at(session_.song_time_ms());
    } else if (matches(name, {"scrollSpeed"})) {
        value = settings.scroll_speed;
    } else if (matches(name, {"inputOffset", "inputOffsetMs"})) {
        value = settings.input_offset_ms;
    } else if (matches(name, {"visualOffset", "visualOffsetMs"})) {
        value = settings.visual_offset_ms;
    } else if (matches(name, {"botPlay", "cpuControlled", "autoplay"})) {
        value = settings.autoplay;
    } else if (matches(name, {"practice", "practiceMode"})) {
        value = settings.practice;
    } else if (matches(name, {"ghostTapping"})) {
        value = settings.ghost_tapping;
    } else if (matches(name, {"downscroll"})) {
        value = settings.downscroll;
    } else if (matches(name, {"middlescroll", "middleScroll"})) {
        value = settings.middle_scroll;
    } else if (matches(name, {"noFail"})) {
        value = settings.no_fail;
    } else if (matches(name, {"keyCount", "playerKeyCount"})) {
        // PULSEFORGE_P1_3_0_DYNAMIC_MANIA_LUA_GLOBALS_V1
        value = static_cast<std::int64_t>(session_.player_key_count());
    } else if (matches(name, {"opponentKeyCount"})) {
        value = static_cast<std::int64_t>(session_.opponent_key_count());
    } else if (matches(
                   name,
                   {"noteMultiplier", "noteMultiplierPlayer"}
               )) {
        value = session_.player_note_multiplier();
    } else if (matches(name, {"noteMultiplierOpponent"})) {
        value = session_.opponent_note_multiplier();
    } else if (matches(name, {"songName"})) {
        value = session_.chart().title;
    } else if (matches(name, {"difficultyName"})) {
        value = session_.chart().difficulty;
    } else {
        error = "property is not exposed by the gameplay sandbox: ";
        error.append(name);
        return false;
    }

    error.clear();
    return true;
}

bool GameplayLuaHost::set_property(
    const std::string_view name,
    const ScriptValue& value,
    std::string& error
) {
    auto& settings = session_.settings();

    if (matches(name, {"health"})) {
        double health = 0.0;
        if (!script_number(value, health)) {
            set_type_error(error, "a finite number");
            return false;
        }
        return set_health(health, error);
    }

    if (matches(name, {"score", "songScore"})) {
        std::int64_t score = 0;
        if (!script_integer(value, score)) {
            set_type_error(error, "an integer");
            return false;
        }
        set_gameplay_score(session_, score);
        error.clear();
        return true;
    }

    if (matches(name, {"scrollSpeed"})) {
        double speed = 0.0;
        if (!script_number(value, speed)) {
            set_type_error(error, "a finite number");
            return false;
        }
        settings.scroll_speed = std::clamp(speed, 0.1, 10.0);
        error.clear();
        return true;
    }

    if (matches(name, {"inputOffset", "inputOffsetMs"})) {
        double offset = 0.0;
        if (!script_number(value, offset)) {
            set_type_error(error, "a finite number");
            return false;
        }
        settings.input_offset_ms = std::clamp(offset, -1'000.0, 1'000.0);
        error.clear();
        return true;
    }

    if (matches(name, {"visualOffset", "visualOffsetMs"})) {
        double offset = 0.0;
        if (!script_number(value, offset)) {
            set_type_error(error, "a finite number");
            return false;
        }
        settings.visual_offset_ms = std::clamp(offset, -1'000.0, 1'000.0);
        error.clear();
        return true;
    }

    bool boolean = false;
    if (matches(name, {"botPlay", "cpuControlled", "autoplay"})) {
        if (!script_boolean(value, boolean)) {
            set_type_error(error, "a boolean");
            return false;
        }
        settings.autoplay = boolean;
    } else if (matches(name, {"practice", "practiceMode"})) {
        if (!script_boolean(value, boolean)) {
            set_type_error(error, "a boolean");
            return false;
        }
        settings.practice = boolean;
    } else if (matches(name, {"ghostTapping"})) {
        if (!script_boolean(value, boolean)) {
            set_type_error(error, "a boolean");
            return false;
        }
        settings.ghost_tapping = boolean;
    } else if (matches(name, {"downscroll"})) {
        if (!script_boolean(value, boolean)) {
            set_type_error(error, "a boolean");
            return false;
        }
        settings.downscroll = boolean;
    } else if (matches(name, {"middlescroll", "middleScroll"})) {
        if (!script_boolean(value, boolean)) {
            set_type_error(error, "a boolean");
            return false;
        }
        settings.middle_scroll = boolean;
    } else if (matches(name, {"noFail"})) {
        if (!script_boolean(value, boolean)) {
            set_type_error(error, "a boolean");
            return false;
        }
        settings.no_fail = boolean;
    } else {
        error = "property is read-only or not exposed by the gameplay sandbox: ";
        error.append(name);
        return false;
    }

    error.clear();
    return true;
}

bool GameplayLuaHost::add_score(
    const std::int64_t amount,
    std::string& error
) {
    const auto target = saturating_add(session_.summary().score, amount);
    set_gameplay_score(session_, target);
    error.clear();
    return true;
}

bool GameplayLuaHost::set_health(
    const double health,
    std::string& error
) {
    if (!std::isfinite(health)) {
        error = "health must be a finite number";
        return false;
    }
    session_.set_health(health);
    error.clear();
    return true;
}

bool GameplayLuaHost::trigger_event(
    ScriptEventRequest event,
    std::string& error
) {
    if (event.name.empty()) {
        error = "event name cannot be empty";
        return false;
    }
    if (event.name.size() > maximum_event_name_bytes
        || event.value1.size() > maximum_event_value_bytes
        || event.value2.size() > maximum_event_value_bytes) {
        error = "event exceeds the gameplay bridge string limit";
        return false;
    }
    if (state_.pending_events.size() >= state_.max_pending_events) {
        error = "pending script event queue is full";
        return false;
    }

    state_.pending_events.push_back(std::move(event));
    error.clear();
    return true;
}

void GameplayLuaHost::debug_print(const std::string_view message) {
    if (state_.debug_messages.size() >= state_.max_debug_messages) {
        return;
    }
    state_.debug_messages.emplace_back(
        message.substr(0, maximum_debug_message_bytes)
    );
}

struct LuaRuntime::Impl {
    enum class Callback : std::uint8_t {
        on_create,
        on_create_post,
        // PULSEFORGE_P1_1_4_ONSTARTCOUNTDOWN_V1
        on_start_countdown,
        on_song_start,
        on_end_song,
        on_game_over,
        on_pause,
        on_resume,
        on_update,
        on_update_post,
        on_beat_hit,
        on_step_hit,
        on_section_hit,
        good_note_hit,
        opponent_note_hit,
        note_miss,
        note_miss_press,
        on_ghost_tap,
        on_event,
        on_tween_completed,
        on_timer_completed,
        // PULSEFORGE_P1_1_18_SOUND_COMPLETION_CALLBACK_V1
        on_sound_finished,
        on_destroy,
        count,
    };

    enum class ArgumentKind : std::uint8_t {
        integer,
        number,
        boolean,
        string,
    };

    struct Argument {
        ArgumentKind kind{ArgumentKind::integer};
        std::int64_t integer{};
        double number{};
        bool boolean{};
        std::string_view string;
    };

    struct PendingCall {
        Callback callback{Callback::on_create};
        std::array<Argument, 4> arguments{};
        std::size_t argument_count{};
        bool update_beat{};
        bool update_step{};
        bool update_section{};
        std::int64_t beat{};
        std::int64_t step{};
        std::int64_t section{};
        bool missing{};
    };

    struct AllocatorState {
        std::size_t limit{};
        std::size_t current{};
        std::size_t peak{};
        bool denied{};
    };

    static constexpr std::array<std::string_view, 23> callback_names{
        "onCreate",
        "onCreatePost",
        "onStartCountdown",
        "onSongStart",
        "onEndSong",
        "onGameOver",
        "onPause",
        "onResume",
        "onUpdate",
        "onUpdatePost",
        "onBeatHit",
        "onStepHit",
        "onSectionHit",
        "goodNoteHit",
        "opponentNoteHit",
        "noteMiss",
        "noteMissPress",
        "onGhostTap",
        "onEvent",
        "onTweenCompleted",
        "onTimerCompleted",
        "onSoundFinished",
        "onDestroy",
    };

    static constexpr std::string_view function_stop{
        "##PSYCHLUA_FUNCTIONSTOP"
    };
    static constexpr std::string_view function_stop_lua{
        "##PSYCHLUA_FUNCTIONSTOPLUA"
    };
    static constexpr std::string_view function_stop_all{
        "##PSYCHLUA_FUNCTIONSTOPALL"
    };
    static constexpr const char* instruction_error_registry_key =
        "pulseforge.instruction_limit";

    LuaHostInterface& host;
    LuaRuntimeConfig config;
    lua_State* state{};
    AllocatorState allocator;
    PendingCall pending;
    ScriptValue scratch_value;
    ScriptEventRequest scratch_event;
    std::string scratch_name;
    std::string scratch_error;
    std::string scratch_text;
    std::vector<ScriptValue> scratch_arguments;
    std::array<bool, static_cast<std::size_t>(Callback::count)> disabled{};
    std::array<bool, static_cast<std::size_t>(Callback::count)> absent{};
    std::vector<LuaDiagnostic> diagnostic_log;
    LuaRuntimeStats runtime_stats;
    std::uint64_t instruction_remaining{};
    std::uint64_t current_instruction_count{};
    std::uint32_t effective_hook_interval{1};
    bool instruction_denied{};
    bool script_loaded{};
    bool create_called{};
    bool destroy_called{};

    explicit Impl(LuaHostInterface& host_value, LuaRuntimeConfig config_value)
        : host(host_value),
          config(std::move(config_value)) {
        allocator.limit = config.memory_limit_bytes;
        if (config.instruction_hook_interval == 0) {
            config.instruction_hook_interval = 1;
        }
        constexpr auto maximum_hook_interval =
            static_cast<std::uint32_t>(std::numeric_limits<int>::max());
        config.instruction_hook_interval = std::min(
            config.instruction_hook_interval,
            maximum_hook_interval
        );
        effective_hook_interval = config.instruction_hook_interval;
        scratch_arguments.reserve(16U);
    }

    ~Impl() {
        close_state();
    }

    [[nodiscard]] static Impl* from_state(lua_State* lua) noexcept {
        void* userdata = nullptr;
        (void)lua_getallocf(lua, &userdata);
        return static_cast<Impl*>(userdata);
    }

    [[nodiscard]] static void* allocate(
        void* userdata,
        void* pointer,
        const std::size_t old_size,
        const std::size_t new_size
    ) noexcept {
        auto* self = static_cast<Impl*>(userdata);
        auto& accounting = self->allocator;

        if (new_size == 0) {
            std::free(pointer);
            if (pointer != nullptr) {
                accounting.current -= std::min(
                    accounting.current,
                    old_size
                );
            }
            return nullptr;
        }

        const std::size_t accounted_old =
            pointer == nullptr ? 0 : std::min(old_size, accounting.current);
        const std::size_t base = accounting.current - accounted_old;
        if (new_size > accounting.limit
            || base > accounting.limit - new_size) {
            accounting.denied = true;
            return nullptr;
        }

        void* replacement = std::realloc(pointer, new_size);
        if (replacement == nullptr) {
            accounting.denied = true;
            return nullptr;
        }

        accounting.current = base + new_size;
        accounting.peak = std::max(accounting.peak, accounting.current);
        return replacement;
    }

    [[nodiscard]] static int panic(lua_State*) noexcept {
        // Lua only invokes panic for an unprotected API error. All script
        // execution and callback argument construction is protected below.
        return 0;
    }

    static void instruction_hook(lua_State* lua, lua_Debug*) {
        auto* self = from_state(lua);
        if (self == nullptr) {
            return;
        }

        if (self->instruction_denied
            || self->instruction_remaining
                <= self->effective_hook_interval) {
            self->instruction_denied = true;
            lua_sethook(lua, &Impl::instruction_hook, LUA_MASKCOUNT, 1);
            lua_getfield(
                lua,
                LUA_REGISTRYINDEX,
                instruction_error_registry_key
            );
            (void)lua_error(lua);
            return;
        }

        self->instruction_remaining -= self->effective_hook_interval;
        self->current_instruction_count += self->effective_hook_interval;
    }

    [[nodiscard]] static int traceback(lua_State* lua) {
        if (lua_type(lua, 1) == LUA_TSTRING) {
            const char* message = lua_tostring(lua, 1);
            luaL_traceback(lua, lua, message, 1);
        } else {
            lua_pushliteral(lua, "Lua raised a non-string error");
        }
        return 1;
    }

    static void push_bounded_string(
        lua_State* lua,
        const std::string_view value,
        const std::size_t limit
    ) {
        const auto bounded = value.substr(0, limit);
        lua_pushlstring(lua, bounded.data(), bounded.size());
    }

    static void push_script_value(
        lua_State* lua,
        const ScriptValue& value,
        const std::size_t string_limit
    ) {
        if (std::holds_alternative<std::monostate>(value)) {
            lua_pushnil(lua);
        } else if (const auto* boolean = std::get_if<bool>(&value)) {
            lua_pushboolean(lua, *boolean ? 1 : 0);
        } else if (const auto* integer = std::get_if<std::int64_t>(&value)) {
            lua_pushinteger(lua, static_cast<lua_Integer>(*integer));
        } else if (const auto* number = std::get_if<double>(&value)) {
            if (std::isfinite(*number)) {
                lua_pushnumber(lua, static_cast<lua_Number>(*number));
            } else {
                lua_pushnil(lua);
            }
        } else if (const auto* string = std::get_if<std::string>(&value)) {
            push_bounded_string(lua, *string, string_limit);
        } else {
            lua_pushnil(lua);
        }
    }

    [[nodiscard]] static bool lua_scalar(
        lua_State* lua,
        const int index,
        const std::size_t string_limit,
        ScriptValue& value,
        std::string& error
    ) {
        switch (lua_type(lua, index)) {
        case LUA_TNIL:
            value = std::monostate{};
            return true;
        case LUA_TBOOLEAN:
            value = lua_toboolean(lua, index) != 0;
            return true;
        case LUA_TNUMBER:
            if (lua_isinteger(lua, index) != 0) {
                value = static_cast<std::int64_t>(
                    lua_tointeger(lua, index)
                );
            } else {
                const double number = static_cast<double>(
                    lua_tonumber(lua, index)
                );
                if (!std::isfinite(number)) {
                    error = "non-finite numbers are not accepted";
                    return false;
                }
                value = number;
            }
            return true;
        case LUA_TSTRING: {
            std::size_t length = 0;
            const char* data = lua_tolstring(lua, index, &length);
            if (length > string_limit) {
                error = "string exceeds the host API limit";
                return false;
            }
            value = std::string(data, length);
            return true;
        }
        default:
            error =
                "only nil, boolean, number and string property values are allowed";
            return false;
        }
    }

    [[nodiscard]] static bool bounded_lua_string(
        lua_State* lua,
        const int index,
        const std::size_t limit,
        std::string& value,
        std::string& error,
        const bool allow_number = false,
        const bool allow_nil_as_empty = false
    ) {
        const int type = lua_type(lua, index);
        if (allow_nil_as_empty
            && (type == LUA_TNIL || type == LUA_TNONE)) {
            value.clear();
            return true;
        }
        if (type != LUA_TSTRING && !(allow_number && type == LUA_TNUMBER)) {
            error = "expected a string";
            return false;
        }
        std::size_t length = 0;
        const char* data = lua_tolstring(lua, index, &length);
        if (length > limit) {
            error = "string exceeds the host API limit";
            return false;
        }
        value.assign(data, length);
        return true;
    }

    static void record_host_error(
        Impl& self,
        const std::string_view message
    ) noexcept {
        self.record_diagnostic(
            LuaDiagnosticKind::host_error,
            self.script_loaded
                ? callback_names[
                    static_cast<std::size_t>(self.pending.callback)
                ]
                : std::string_view{"<chunk>"},
            message
        );
    }

    [[nodiscard]] static int host_debug_print(lua_State* lua) noexcept {
        auto* self = from_state(lua);
        if (self == nullptr) {
            lua_pushboolean(lua, 0);
            lua_pushliteral(lua, "Lua host is unavailable");
            return 2;
        }

        try {
            self->scratch_text.clear();
            const int argument_count = std::min(lua_gettop(lua), 16);
            for (int index = 1; index <= argument_count; ++index) {
                if (index > 1) {
                    self->scratch_text.push_back('\t');
                }

                const int type = lua_type(lua, index);
                if (type == LUA_TSTRING || type == LUA_TNUMBER) {
                    std::size_t length = 0;
                    const char* text = lua_tolstring(lua, index, &length);
                    const auto remaining =
                        self->config.max_host_string_bytes - std::min(
                            self->config.max_host_string_bytes,
                            self->scratch_text.size()
                        );
                    self->scratch_text.append(
                        text,
                        std::min(length, remaining)
                    );
                } else if (type == LUA_TBOOLEAN) {
                    self->scratch_text.append(
                        lua_toboolean(lua, index) != 0 ? "true" : "false"
                    );
                } else if (type == LUA_TNIL) {
                    self->scratch_text.append("nil");
                } else {
                    self->scratch_text.append("<");
                    self->scratch_text.append(lua_typename(lua, type));
                    self->scratch_text.append(">");
                }

                if (self->scratch_text.size()
                    >= self->config.max_host_string_bytes) {
                    break;
                }
            }

            self->host.debug_print(self->scratch_text);
            lua_pushboolean(lua, 1);
            return 1;
        } catch (const std::exception& exception) {
            record_host_error(*self, exception.what());
            lua_pushboolean(lua, 0);
            lua_pushliteral(lua, "host debug logger failed");
            return 2;
        } catch (...) {
            record_host_error(*self, "host debug logger failed");
            lua_pushboolean(lua, 0);
            lua_pushliteral(lua, "host debug logger failed");
            return 2;
        }
    }

    [[nodiscard]] static int host_get_song_position(lua_State* lua) noexcept {
        auto* self = from_state(lua);
        if (self == nullptr) {
            lua_pushnumber(lua, 0.0);
            return 1;
        }
        const double position = self->host.song_position_ms();
        lua_pushnumber(
            lua,
            std::isfinite(position) ? static_cast<lua_Number>(position) : 0.0
        );
        return 1;
    }

    [[nodiscard]] static int host_get_property(lua_State* lua) noexcept {
        auto* self = from_state(lua);
        if (self == nullptr) {
            lua_pushnil(lua);
            lua_pushliteral(lua, "Lua host is unavailable");
            return 2;
        }

        try {
            self->scratch_name.clear();
            self->scratch_error.clear();
            if (!bounded_lua_string(
                    lua,
                    1,
                    maximum_property_name_bytes,
                    self->scratch_name,
                    self->scratch_error
                )) {
                record_host_error(*self, self->scratch_error);
                lua_pushnil(lua);
                push_bounded_string(
                    lua,
                    self->scratch_error,
                    self->config.max_host_string_bytes
                );
                return 2;
            }

            self->scratch_value = std::monostate{};
            if (!self->host.get_property(
                    self->scratch_name,
                    self->scratch_value,
                    self->scratch_error
                )) {
                record_host_error(*self, self->scratch_error);
                lua_pushnil(lua);
                push_bounded_string(
                    lua,
                    self->scratch_error,
                    self->config.max_host_string_bytes
                );
                return 2;
            }

            push_script_value(
                lua,
                self->scratch_value,
                self->config.max_host_string_bytes
            );
            return 1;
        } catch (const std::exception& exception) {
            record_host_error(*self, exception.what());
            lua_pushnil(lua);
            lua_pushliteral(lua, "host getProperty failed");
            return 2;
        } catch (...) {
            record_host_error(*self, "host getProperty failed");
            lua_pushnil(lua);
            lua_pushliteral(lua, "host getProperty failed");
            return 2;
        }
    }

    [[nodiscard]] static int host_set_property(lua_State* lua) noexcept {
        auto* self = from_state(lua);
        if (self == nullptr) {
            lua_pushboolean(lua, 0);
            lua_pushliteral(lua, "Lua host is unavailable");
            return 2;
        }

        try {
            self->scratch_name.clear();
            self->scratch_error.clear();
            if (!bounded_lua_string(
                    lua,
                    1,
                    maximum_property_name_bytes,
                    self->scratch_name,
                    self->scratch_error
                )) {
                record_host_error(*self, self->scratch_error);
                lua_pushboolean(lua, 0);
                push_bounded_string(
                    lua,
                    self->scratch_error,
                    self->config.max_host_string_bytes
                );
                return 2;
            }

            self->scratch_value = std::monostate{};
            if (!lua_scalar(
                    lua,
                    2,
                    self->config.max_host_string_bytes,
                    self->scratch_value,
                    self->scratch_error
                )
                || !self->host.set_property(
                    self->scratch_name,
                    self->scratch_value,
                    self->scratch_error
                )) {
                record_host_error(*self, self->scratch_error);
                lua_pushboolean(lua, 0);
                push_bounded_string(
                    lua,
                    self->scratch_error,
                    self->config.max_host_string_bytes
                );
                return 2;
            }

            lua_pushboolean(lua, 1);
            return 1;
        } catch (const std::exception& exception) {
            record_host_error(*self, exception.what());
            lua_pushboolean(lua, 0);
            lua_pushliteral(lua, "host setProperty failed");
            return 2;
        } catch (...) {
            record_host_error(*self, "host setProperty failed");
            lua_pushboolean(lua, 0);
            lua_pushliteral(lua, "host setProperty failed");
            return 2;
        }
    }

    [[nodiscard]] static int host_add_score(lua_State* lua) noexcept {
        auto* self = from_state(lua);
        if (self == nullptr) {
            lua_pushboolean(lua, 0);
            lua_pushliteral(lua, "Lua host is unavailable");
            return 2;
        }

        try {
            int is_integer = 0;
            const lua_Integer amount = lua_tointegerx(lua, 1, &is_integer);
            if (is_integer == 0) {
                record_host_error(*self, "addScore expects an integer");
                lua_pushboolean(lua, 0);
                lua_pushliteral(lua, "addScore expects an integer");
                return 2;
            }

            self->scratch_error.clear();
            if (!self->host.add_score(
                    static_cast<std::int64_t>(amount),
                    self->scratch_error
                )) {
                record_host_error(*self, self->scratch_error);
                lua_pushboolean(lua, 0);
                push_bounded_string(
                    lua,
                    self->scratch_error,
                    self->config.max_host_string_bytes
                );
                return 2;
            }

            lua_pushboolean(lua, 1);
            return 1;
        } catch (const std::exception& exception) {
            record_host_error(*self, exception.what());
            lua_pushboolean(lua, 0);
            lua_pushliteral(lua, "host addScore failed");
            return 2;
        } catch (...) {
            record_host_error(*self, "host addScore failed");
            lua_pushboolean(lua, 0);
            lua_pushliteral(lua, "host addScore failed");
            return 2;
        }
    }

    [[nodiscard]] static int host_set_health(lua_State* lua) noexcept {
        auto* self = from_state(lua);
        if (self == nullptr) {
            lua_pushboolean(lua, 0);
            lua_pushliteral(lua, "Lua host is unavailable");
            return 2;
        }

        try {
            int is_number = 0;
            const double health = static_cast<double>(
                lua_tonumberx(lua, 1, &is_number)
            );
            if (is_number == 0 || !std::isfinite(health)) {
                record_host_error(
                    *self,
                    "setHealth expects a finite number"
                );
                lua_pushboolean(lua, 0);
                lua_pushliteral(lua, "setHealth expects a finite number");
                return 2;
            }

            self->scratch_error.clear();
            if (!self->host.set_health(health, self->scratch_error)) {
                record_host_error(*self, self->scratch_error);
                lua_pushboolean(lua, 0);
                push_bounded_string(
                    lua,
                    self->scratch_error,
                    self->config.max_host_string_bytes
                );
                return 2;
            }

            lua_pushboolean(lua, 1);
            return 1;
        } catch (const std::exception& exception) {
            record_host_error(*self, exception.what());
            lua_pushboolean(lua, 0);
            lua_pushliteral(lua, "host setHealth failed");
            return 2;
        } catch (...) {
            record_host_error(*self, "host setHealth failed");
            lua_pushboolean(lua, 0);
            lua_pushliteral(lua, "host setHealth failed");
            return 2;
        }
    }

    [[nodiscard]] static int host_trigger_event(lua_State* lua) noexcept {
        auto* self = from_state(lua);
        if (self == nullptr) {
            lua_pushboolean(lua, 0);
            lua_pushliteral(lua, "Lua host is unavailable");
            return 2;
        }

        try {
            self->scratch_event = {};
            self->scratch_error.clear();
            if (!bounded_lua_string(
                    lua,
                    1,
                    maximum_event_name_bytes,
                    self->scratch_event.name,
                    self->scratch_error
                )
                || !bounded_lua_string(
                    lua,
                    2,
                    maximum_event_value_bytes,
                    self->scratch_event.value1,
                    self->scratch_error,
                    true,
                    true
                )
                || !bounded_lua_string(
                    lua,
                    3,
                    maximum_event_value_bytes,
                    self->scratch_event.value2,
                    self->scratch_error,
                    true,
                    true
                )) {
                record_host_error(*self, self->scratch_error);
                lua_pushboolean(lua, 0);
                push_bounded_string(
                    lua,
                    self->scratch_error,
                    self->config.max_host_string_bytes
                );
                return 2;
            }

            if (!self->host.trigger_event(
                    std::move(self->scratch_event),
                    self->scratch_error
                )) {
                record_host_error(*self, self->scratch_error);
                lua_pushboolean(lua, 0);
                push_bounded_string(
                    lua,
                    self->scratch_error,
                    self->config.max_host_string_bytes
                );
                return 2;
            }

            lua_pushboolean(lua, 1);
            return 1;
        } catch (const std::exception& exception) {
            record_host_error(*self, exception.what());
            lua_pushboolean(lua, 0);
            lua_pushliteral(lua, "host triggerEvent failed");
            return 2;
        } catch (...) {
            record_host_error(*self, "host triggerEvent failed");
            lua_pushboolean(lua, 0);
            lua_pushliteral(lua, "host triggerEvent failed");
            return 2;
        }
    }


    // PULSEFORGE_P1_1_15_SHADER_ARRAY_LUA_BRIDGE_V1
    // Psych exposes vector uniforms through Lua tables. Tables never cross the
    // engine boundary: this adapter validates 1..8 numeric values and flattens
    // them into the existing bounded scalar ScriptValue span.

// PULSEFORGE_P1_1_16_PSYCH_LUA_UTILITY_FUNCTIONS_V1
[[nodiscard]] static double deterministic_random_unit(
    lua_State* lua
) noexcept {
    const int original_top = lua_gettop(lua);
    double value = 0.5;
    lua_getglobal(lua, "math");
    if (lua_type(lua, -1) == LUA_TTABLE) {
        lua_getfield(lua, -1, "random");
        if (lua_type(lua, -1) == LUA_TFUNCTION) {
            if (lua_pcall(lua, 0, 1, 0) == LUA_OK) {
                int is_number = 0;
                const double candidate = static_cast<double>(
                    lua_tonumberx(lua, -1, &is_number)
                );
                if (is_number != 0 && std::isfinite(candidate)) {
                    value = std::clamp(candidate, 0.0, 1.0);
                }
            }
        }
    }
    lua_settop(lua, original_top);
    return value;
}

[[nodiscard]] static bool random_integer_excluded(
    const std::string_view exclusions,
    const std::int64_t value
) noexcept {
    std::size_t cursor = 0U;
    while (cursor < exclusions.size()) {
        while (cursor < exclusions.size()
            && (exclusions[cursor] == ' '
                || exclusions[cursor] == '\t'
                || exclusions[cursor] == ',')) {
            ++cursor;
        }
        if (cursor >= exclusions.size()) break;

        const auto end = exclusions.find(',', cursor);
        const auto token = exclusions.substr(
            cursor,
            end == std::string_view::npos
                ? exclusions.size() - cursor
                : end - cursor
        );
        std::array<char, 64U> buffer{};
        if (!token.empty() && token.size() < buffer.size()) {
            std::copy(token.begin(), token.end(), buffer.begin());
            char* parsed_end = nullptr;
            const long long parsed = std::strtoll(
                buffer.data(), &parsed_end, 10
            );
            while (parsed_end != nullptr
                && (*parsed_end == ' ' || *parsed_end == '\t')) {
                ++parsed_end;
            }
            if (parsed_end != buffer.data()
                && parsed_end != nullptr && *parsed_end == '\0'
                && static_cast<std::int64_t>(parsed) == value) {
                return true;
            }
        }
        if (end == std::string_view::npos) break;
        cursor = end + 1U;
    }
    return false;
}

[[nodiscard]] static int host_get_random_int(lua_State* lua) noexcept {
    std::int64_t minimum = 0;
    std::int64_t maximum = 1;
    int is_integer = 0;
    if (lua_gettop(lua) >= 1) {
        minimum = static_cast<std::int64_t>(
            lua_tointegerx(lua, 1, &is_integer)
        );
        if (is_integer == 0) {
            lua_pushinteger(lua, 0);
            return 1;
        }
    }
    if (lua_gettop(lua) >= 2) {
        maximum = static_cast<std::int64_t>(
            lua_tointegerx(lua, 2, &is_integer)
        );
        if (is_integer == 0) {
            lua_pushinteger(lua, 0);
            return 1;
        }
    }
    minimum = std::clamp<std::int64_t>(
        minimum, -1'000'000'000LL, 1'000'000'000LL
    );
    maximum = std::clamp<std::int64_t>(
        maximum, -1'000'000'000LL, 1'000'000'000LL
    );
    if (minimum > maximum) std::swap(minimum, maximum);

    std::string_view exclusions;
    if (lua_gettop(lua) >= 3) {
        std::size_t length{};
        const char* data = lua_tolstring(lua, 3, &length);
        if (data != nullptr) {
            exclusions = std::string_view{data, std::min<std::size_t>(
                length, 4'096U
            )};
        }
    }

    const auto width = static_cast<std::uint64_t>(
        maximum - minimum
    ) + 1ULL;
    std::int64_t candidate = minimum;
    for (std::size_t attempt = 0U; attempt < 64U; ++attempt) {
        const double unit = deterministic_random_unit(lua);
        const auto offset = std::min<std::uint64_t>(
            static_cast<std::uint64_t>(
                std::floor(unit * static_cast<double>(width))
            ),
            width - 1ULL
        );
        candidate = minimum + static_cast<std::int64_t>(offset);
        if (!random_integer_excluded(exclusions, candidate)) {
            lua_pushinteger(lua, static_cast<lua_Integer>(candidate));
            return 1;
        }
    }
    for (std::int64_t value = minimum;
         value <= maximum && value - minimum < 4'096;
         ++value) {
        if (!random_integer_excluded(exclusions, value)) {
            lua_pushinteger(lua, static_cast<lua_Integer>(value));
            return 1;
        }
    }
    lua_pushinteger(lua, static_cast<lua_Integer>(minimum));
    return 1;
}

[[nodiscard]] static int host_get_random_float(lua_State* lua) noexcept {
    double minimum = 0.0;
    double maximum = 1.0;
    int is_number = 0;
    if (lua_gettop(lua) >= 1) {
        minimum = static_cast<double>(
            lua_tonumberx(lua, 1, &is_number)
        );
        if (is_number == 0 || !std::isfinite(minimum)) {
            lua_pushnumber(lua, 0.0);
            return 1;
        }
    }
    if (lua_gettop(lua) >= 2) {
        maximum = static_cast<double>(
            lua_tonumberx(lua, 2, &is_number)
        );
        if (is_number == 0 || !std::isfinite(maximum)) {
            lua_pushnumber(lua, 0.0);
            return 1;
        }
    }
    minimum = std::clamp(minimum, -1.0e9, 1.0e9);
    maximum = std::clamp(maximum, -1.0e9, 1.0e9);
    if (minimum > maximum) std::swap(minimum, maximum);
    const double value = minimum
        + (maximum - minimum) * deterministic_random_unit(lua);
    lua_pushnumber(lua, static_cast<lua_Number>(value));
    return 1;
}

[[nodiscard]] static int host_get_random_bool(lua_State* lua) noexcept {
    double chance = 50.0;
    int is_number = 0;
    if (lua_gettop(lua) >= 1) {
        chance = static_cast<double>(
            lua_tonumberx(lua, 1, &is_number)
        );
        if (is_number == 0 || !std::isfinite(chance)) chance = 50.0;
    }
    chance = std::clamp(chance, 0.0, 100.0);
    if (chance <= 0.0) {
        lua_pushboolean(lua, 0);
        return 1;
    }
    if (chance >= 100.0) {
        lua_pushboolean(lua, 1);
        return 1;
    }
    lua_pushboolean(
        lua,
        deterministic_random_unit(lua) * 100.0 < chance ? 1 : 0
    );
    return 1;
}

[[nodiscard]] static std::optional<std::uint32_t> parse_lua_color(
    std::string_view text
) noexcept {
    if (!text.empty() && text.front() == '#') {
        text.remove_prefix(1U);
    } else if (text.size() > 2U && text[0] == '0'
        && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2U);
    }
    if (text.size() != 6U && text.size() != 8U) {
        return std::nullopt;
    }
    std::array<char, 9U> buffer{};
    std::copy(text.begin(), text.end(), buffer.begin());
    char* end = nullptr;
    const unsigned long raw = std::strtoul(buffer.data(), &end, 16);
    if (end == buffer.data() || end == nullptr || *end != '\0') {
        return std::nullopt;
    }
    if (text.size() == 6U) {
        return 0xFF000000U | static_cast<std::uint32_t>(raw);
    }
    return static_cast<std::uint32_t>(raw);
}

[[nodiscard]] static int host_get_color_from_hex(lua_State* lua) noexcept {
    std::size_t length{};
    const char* data = lua_tolstring(lua, 1, &length);
    const auto color = data == nullptr
        ? std::optional<std::uint32_t>{}
        : parse_lua_color(std::string_view{data, length});
    lua_pushinteger(
        lua,
        static_cast<lua_Integer>(color.value_or(0xFFFFFFFFU))
    );
    return 1;
}

[[nodiscard]] static int host_get_color_from_string(
    lua_State* lua
) noexcept {
    std::size_t length{};
    const char* data = lua_tolstring(lua, 1, &length);
    if (data == nullptr || length == 0U || length > 128U) {
        lua_pushinteger(lua, static_cast<lua_Integer>(0xFFFFFFFFU));
        return 1;
    }
    std::array<char, 129U> name_buffer{};
    for (std::size_t index = 0U; index < length; ++index) {
        const auto value = static_cast<unsigned char>(data[index]);
        name_buffer[index] = value >= 'A' && value <= 'Z'
            ? static_cast<char>(value - 'A' + 'a')
            : static_cast<char>(value);
    }
    const std::string_view name{name_buffer.data(), length};

    constexpr std::array named_colors{
        std::pair{std::string_view{"black"},       0xFF000000U},
        std::pair{std::string_view{"white"},       0xFFFFFFFFU},
        std::pair{std::string_view{"red"},         0xFFFF0000U},
        std::pair{std::string_view{"green"},       0xFF00FF00U},
        std::pair{std::string_view{"blue"},        0xFF0000FFU},
        std::pair{std::string_view{"yellow"},      0xFFFFFF00U},
        std::pair{std::string_view{"cyan"},        0xFF00FFFFU},
        std::pair{std::string_view{"magenta"},     0xFFFF00FFU},
        std::pair{std::string_view{"gray"},        0xFF808080U},
        std::pair{std::string_view{"grey"},        0xFF808080U},
        std::pair{std::string_view{"orange"},      0xFFFFA500U},
        std::pair{std::string_view{"purple"},      0xFF800080U},
        std::pair{std::string_view{"pink"},        0xFFFFC0CBU},
        std::pair{std::string_view{"transparent"}, 0x00000000U},
    };
    for (const auto& [key, color] : named_colors) {
        if (name == key) {
            lua_pushinteger(lua, static_cast<lua_Integer>(color));
            return 1;
        }
    }
    const auto parsed = parse_lua_color(name);
    lua_pushinteger(
        lua,
        static_cast<lua_Integer>(parsed.value_or(0xFFFFFFFFU))
    );
    return 1;
}

[[nodiscard]] static int host_string_starts_with(lua_State* lua) noexcept {
    std::size_t value_length{}, prefix_length{};
    const char* value = lua_tolstring(lua, 1, &value_length);
    const char* prefix = lua_tolstring(lua, 2, &prefix_length);
    const bool matches = value != nullptr && prefix != nullptr
        && prefix_length <= value_length
        && std::string_view{value, value_length}.starts_with(
            std::string_view{prefix, prefix_length}
        );
    lua_pushboolean(lua, matches ? 1 : 0);
    return 1;
}

[[nodiscard]] static int host_string_ends_with(lua_State* lua) noexcept {
    std::size_t value_length{}, suffix_length{};
    const char* value = lua_tolstring(lua, 1, &value_length);
    const char* suffix = lua_tolstring(lua, 2, &suffix_length);
    const bool matches = value != nullptr && suffix != nullptr
        && suffix_length <= value_length
        && std::string_view{value, value_length}.ends_with(
            std::string_view{suffix, suffix_length}
        );
    lua_pushboolean(lua, matches ? 1 : 0);
    return 1;
}

[[nodiscard]] static int host_string_find_plain(lua_State* lua) noexcept {
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

[[nodiscard]] static int host_string_trim(lua_State* lua) noexcept {
    auto* self = from_state(lua);
    std::size_t length{};
    const char* data = lua_tolstring(lua, 1, &length);
    if (data == nullptr) {
        lua_pushliteral(lua, "");
        return 1;
    }
    const std::size_t limit = self != nullptr
        ? self->config.max_host_string_bytes
        : 4'096U;
    std::string_view value{data, std::min(length, limit)};
    while (!value.empty()
        && (value.front() == ' ' || value.front() == '\t'
            || value.front() == '\r' || value.front() == '\n')) {
        value.remove_prefix(1U);
    }
    while (!value.empty()
        && (value.back() == ' ' || value.back() == '\t'
            || value.back() == '\r' || value.back() == '\n')) {
        value.remove_suffix(1U);
    }
    lua_pushlstring(lua, value.data(), value.size());
    return 1;
}

[[nodiscard]] static int host_string_split(lua_State* lua) noexcept {
    auto* self = from_state(lua);
    std::size_t value_length{}, delimiter_length{};
    const char* value_data = lua_tolstring(lua, 1, &value_length);
    const char* delimiter_data = lua_tolstring(lua, 2, &delimiter_length);
    if (value_data == nullptr) {
        lua_newtable(lua);
        return 1;
    }
    const std::size_t limit = self != nullptr
        ? self->config.max_host_string_bytes
        : 4'096U;
    std::string_view value{
        value_data, std::min(value_length, limit)
    };
    const std::string_view delimiter = delimiter_data == nullptr
        ? std::string_view{}
        : std::string_view{delimiter_data, delimiter_length};

    lua_newtable(lua);
    if (delimiter.empty()) {
        for (std::size_t index = 0U;
             index < value.size() && index < 256U;
             ++index) {
            lua_pushlstring(lua, value.data() + index, 1U);
            lua_rawseti(lua, -2, static_cast<lua_Integer>(index + 1U));
        }
        return 1;
    }

    std::size_t cursor = 0U;
    std::size_t output_index = 1U;
    while (cursor <= value.size() && output_index <= 256U) {
        const auto found = value.find(delimiter, cursor);
        const auto part = value.substr(
            cursor,
            found == std::string_view::npos
                ? value.size() - cursor
                : found - cursor
        );
        lua_pushlstring(lua, part.data(), part.size());
        lua_rawseti(
            lua, -2, static_cast<lua_Integer>(output_index++)
        );
        if (found == std::string_view::npos) break;
        cursor = found + delimiter.size();
    }
    return 1;
}

    [[nodiscard]] static int host_invoke_shader_numeric_array(
        lua_State* lua,
        const bool integer_values
    ) noexcept {
        auto* self = from_state(lua);
        if (self == nullptr) {
            lua_pushnil(lua);
            lua_pushliteral(lua, "Lua host is unavailable");
            return 2;
        }

        try {
            if (lua_gettop(lua) != 3 || lua_type(lua, 3) != LUA_TTABLE) {
                lua_pushnil(lua);
                lua_pushliteral(
                    lua,
                    "shader array setter expects tag, uniform and a numeric table"
                );
                return 2;
            }

            std::string tag;
            std::string uniform;
            self->scratch_error.clear();
            if (!bounded_lua_string(
                    lua,
                    1,
                    128U,
                    tag,
                    self->scratch_error
                )
                || !bounded_lua_string(
                    lua,
                    2,
                    160U,
                    uniform,
                    self->scratch_error
                )) {
                record_host_error(*self, self->scratch_error);
                lua_pushnil(lua);
                push_bounded_string(
                    lua,
                    self->scratch_error,
                    self->config.max_host_string_bytes
                );
                return 2;
            }

            const std::size_t count = lua_rawlen(lua, 3);
            if (count == 0U || count > 8U) {
                self->scratch_error =
                    "shader array setter accepts 1..8 numeric values";
                record_host_error(*self, self->scratch_error);
                lua_pushnil(lua);
                push_bounded_string(
                    lua,
                    self->scratch_error,
                    self->config.max_host_string_bytes
                );
                return 2;
            }

            self->scratch_arguments.clear();
            self->scratch_arguments.reserve(count + 2U);
            self->scratch_arguments.emplace_back(std::move(tag));
            self->scratch_arguments.emplace_back(std::move(uniform));

            for (std::size_t index = 1U; index <= count; ++index) {
                lua_rawgeti(lua, 3, static_cast<lua_Integer>(index));
                if (integer_values) {
                    int is_integer = 0;
                    const lua_Integer value = lua_tointegerx(
                        lua,
                        -1,
                        &is_integer
                    );
                    lua_pop(lua, 1);
                    if (is_integer == 0) {
                        self->scratch_error =
                            "setShaderIntArray values must be integers";
                        record_host_error(*self, self->scratch_error);
                        lua_pushnil(lua);
                        push_bounded_string(
                            lua,
                            self->scratch_error,
                            self->config.max_host_string_bytes
                        );
                        return 2;
                    }
                    self->scratch_arguments.emplace_back(
                        static_cast<std::int64_t>(value)
                    );
                } else {
                    int is_number = 0;
                    const double value = static_cast<double>(
                        lua_tonumberx(lua, -1, &is_number)
                    );
                    lua_pop(lua, 1);
                    if (is_number == 0 || !std::isfinite(value)) {
                        self->scratch_error =
                            "setShaderFloatArray values must be finite numbers";
                        record_host_error(*self, self->scratch_error);
                        lua_pushnil(lua);
                        push_bounded_string(
                            lua,
                            self->scratch_error,
                            self->config.max_host_string_bytes
                        );
                        return 2;
                    }
                    self->scratch_arguments.emplace_back(value);
                }
            }

            self->scratch_value = std::monostate{};
            const std::string_view function_name = integer_values
                ? "setShaderIntArray"
                : "setShaderFloatArray";
            if (!self->host.invoke_function(
                    function_name,
                    self->scratch_arguments,
                    self->scratch_value,
                    self->scratch_error
                )) {
                record_host_error(*self, self->scratch_error);
                lua_pushnil(lua);
                push_bounded_string(
                    lua,
                    self->scratch_error,
                    self->config.max_host_string_bytes
                );
                return 2;
            }

            push_script_value(
                lua,
                self->scratch_value,
                self->config.max_host_string_bytes
            );
            return 1;
        } catch (const std::exception& exception) {
            record_host_error(*self, exception.what());
            lua_pushnil(lua);
            lua_pushliteral(lua, "shader array compatibility function failed");
            return 2;
        } catch (...) {
            record_host_error(*self, "shader array compatibility function failed");
            lua_pushnil(lua);
            lua_pushliteral(lua, "shader array compatibility function failed");
            return 2;
        }
    }

    [[nodiscard]] static int host_set_shader_float_array(
        lua_State* lua
    ) noexcept {
        return host_invoke_shader_numeric_array(lua, false);
    }

    [[nodiscard]] static int host_set_shader_int_array(
        lua_State* lua
    ) noexcept {
        return host_invoke_shader_numeric_array(lua, true);
    }

    [[nodiscard]] static int host_invoke_function(lua_State* lua) noexcept {
        auto* self = from_state(lua);
        if (self == nullptr) {
            lua_pushnil(lua);
            lua_pushliteral(lua, "Lua host is unavailable");
            return 2;
        }

        try {
            std::size_t function_length = 0U;
            const char* function_data = lua_tolstring(
                lua,
                lua_upvalueindex(1),
                &function_length
            );
            if (function_data == nullptr || function_length == 0U
                || function_length > maximum_property_name_bytes) {
                lua_pushnil(lua);
                lua_pushliteral(lua, "invalid host function binding");
                return 2;
            }

            self->scratch_name.assign(function_data, function_length);
            self->scratch_error.clear();
            self->scratch_arguments.clear();
            const int argument_count = lua_gettop(lua);
            if (argument_count > 16) {
                self->scratch_error = "host function accepts at most 16 scalar arguments";
                record_host_error(*self, self->scratch_error);
                lua_pushnil(lua);
                push_bounded_string(
                    lua,
                    self->scratch_error,
                    self->config.max_host_string_bytes
                );
                return 2;
            }
            for (int index = 1; index <= argument_count; ++index) {
                ScriptValue argument;
                if (!lua_scalar(
                        lua,
                        index,
                        self->config.max_host_string_bytes,
                        argument,
                        self->scratch_error
                    )) {
                    record_host_error(*self, self->scratch_error);
                    lua_pushnil(lua);
                    push_bounded_string(
                        lua,
                        self->scratch_error,
                        self->config.max_host_string_bytes
                    );
                    return 2;
                }
                self->scratch_arguments.push_back(std::move(argument));
            }

            self->scratch_value = std::monostate{};
            if (!self->host.invoke_function(
                    self->scratch_name,
                    self->scratch_arguments,
                    self->scratch_value,
                    self->scratch_error
                )) {
                record_host_error(*self, self->scratch_error);
                lua_pushnil(lua);
                push_bounded_string(
                    lua,
                    self->scratch_error,
                    self->config.max_host_string_bytes
                );
                return 2;
            }

            push_script_value(
                lua,
                self->scratch_value,
                self->config.max_host_string_bytes
            );
            return 1;
        } catch (const std::exception& exception) {
            record_host_error(*self, exception.what());
            lua_pushnil(lua);
            lua_pushliteral(lua, "host compatibility function failed");
            return 2;
        } catch (...) {
            record_host_error(*self, "host compatibility function failed");
            lua_pushnil(lua);
            lua_pushliteral(lua, "host compatibility function failed");
            return 2;
        }
    }

    [[nodiscard]] static int invoke_pending(lua_State* lua) {
        auto* self = from_state(lua);
        if (self == nullptr) {
            lua_pushliteral(lua, "Lua host is unavailable");
            return lua_error(lua);
        }

        const auto callback_index =
            static_cast<std::size_t>(self->pending.callback);
        const auto callback_name = callback_names[callback_index];

        lua_pushinteger(
            lua,
            static_cast<lua_Integer>(self->host.current_beat())
        );
        lua_setglobal(lua, "curBeat");
        lua_pushinteger(
            lua,
            static_cast<lua_Integer>(self->host.current_step())
        );
        lua_setglobal(lua, "curStep");
        lua_pushinteger(
            lua,
            static_cast<lua_Integer>(self->host.current_section())
        );
        lua_setglobal(lua, "curSection");
        lua_pushnumber(lua, self->host.current_decimal_beat());
        lua_setglobal(lua, "curDecBeat");
        lua_pushnumber(lua, self->host.current_decimal_step());
        lua_setglobal(lua, "curDecStep");

        if (self->pending.update_beat) {
            lua_pushinteger(
                lua,
                static_cast<lua_Integer>(self->pending.beat)
            );
            lua_setglobal(lua, "curBeat");
        }
        if (self->pending.update_step) {
            lua_pushinteger(
                lua,
                static_cast<lua_Integer>(self->pending.step)
            );
            lua_setglobal(lua, "curStep");
        }
        if (self->pending.update_section) {
            lua_pushinteger(
                lua,
                static_cast<lua_Integer>(self->pending.section)
            );
            lua_setglobal(lua, "curSection");
        }

        const double song_position = self->host.song_position_ms();
        lua_pushnumber(
            lua,
            std::isfinite(song_position)
                ? static_cast<lua_Number>(song_position)
                : 0.0
        );
        lua_setglobal(lua, "songPosition");

        lua_getglobal(lua, callback_name.data());
        if (lua_type(lua, -1) != LUA_TFUNCTION) {
            lua_pop(lua, 1);
            self->pending.missing = true;
            lua_pushnil(lua);
            return 1;
        }

        for (std::size_t index = 0;
             index < self->pending.argument_count;
             ++index) {
            const auto& argument = self->pending.arguments[index];
            switch (argument.kind) {
            case ArgumentKind::integer:
                lua_pushinteger(
                    lua,
                    static_cast<lua_Integer>(argument.integer)
                );
                break;
            case ArgumentKind::number:
                lua_pushnumber(
                    lua,
                    std::isfinite(argument.number)
                        ? static_cast<lua_Number>(argument.number)
                        : 0.0
                );
                break;
            case ArgumentKind::boolean:
                lua_pushboolean(lua, argument.boolean ? 1 : 0);
                break;
            case ArgumentKind::string:
                push_bounded_string(
                    lua,
                    argument.string,
                    self->config.max_host_string_bytes
                );
                break;
            }
        }

        lua_call(
            lua,
            static_cast<int>(self->pending.argument_count),
            1
        );
        return 1;
    }

    void record_diagnostic(
        const LuaDiagnosticKind kind,
        const std::string_view callback,
        const std::string_view message
    ) noexcept {
        if (diagnostic_log.size() >= config.max_diagnostics) {
            return;
        }
        try {
            const std::size_t message_limit = std::max<std::size_t>(
                config.max_host_string_bytes,
                256
            );
            diagnostic_log.push_back({
                kind,
                std::string(callback.substr(0, 128)),
                std::string(message.substr(0, message_limit)),
            });
        } catch (...) {
            // Diagnostics must never turn a contained script failure into an
            // engine failure.
        }
    }

    [[nodiscard]] LuaDiagnosticKind failure_kind(
        const LuaDiagnosticKind fallback
    ) const noexcept {
        if (instruction_denied) {
            return LuaDiagnosticKind::instruction_limit;
        }
        if (allocator.denied) {
            return LuaDiagnosticKind::memory_limit;
        }
        return fallback;
    }

    [[nodiscard]] std::string stack_error_message() const {
        if (state == nullptr || lua_type(state, -1) != LUA_TSTRING) {
            return "Lua raised an error without a string message";
        }
        std::size_t length = 0;
        const char* data = lua_tolstring(state, -1, &length);
        const std::size_t limit = std::max<std::size_t>(
            config.max_host_string_bytes,
            256
        );
        return std::string(data, std::min(length, limit));
    }

    void begin_budget() noexcept {
        allocator.denied = false;
        instruction_denied = false;
        instruction_remaining = config.instruction_budget;
        current_instruction_count = 0;
        effective_hook_interval = config.instruction_hook_interval;
        if (config.instruction_budget < effective_hook_interval) {
            effective_hook_interval = static_cast<std::uint32_t>(
                std::max<std::uint64_t>(config.instruction_budget, 1)
            );
        }
        lua_sethook(
            state,
            &Impl::instruction_hook,
            LUA_MASKCOUNT,
            static_cast<int>(effective_hook_interval)
        );
    }

    void end_budget() noexcept {
        lua_sethook(state, nullptr, 0, 0);
        const std::uint64_t estimated_used =
            instruction_denied
                ? config.instruction_budget
                : current_instruction_count;
        runtime_stats.last_instruction_count = estimated_used;
        runtime_stats.total_instruction_count = saturating_add_unsigned(
            runtime_stats.total_instruction_count,
            estimated_used
        );
    }

    [[nodiscard]] static std::uint64_t saturating_add_unsigned(
        const std::uint64_t left,
        const std::uint64_t right
    ) noexcept {
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
        return left > maximum - right ? maximum : left + right;
    }

    [[nodiscard]] bool create_state() {
        if (config.memory_limit_bytes < minimum_lua_memory_bytes) {
            record_diagnostic(
                LuaDiagnosticKind::memory_limit,
                "<runtime>",
                "Lua memory limit must be at least 256 KiB"
            );
            return false;
        }
        if (config.max_script_bytes == 0) {
            record_diagnostic(
                LuaDiagnosticKind::load_error,
                "<runtime>",
                "maximum script size cannot be zero"
            );
            return false;
        }

        allocator = {};
        allocator.limit = config.memory_limit_bytes;
        state = lua_newstate(&Impl::allocate, this);
        if (state == nullptr) {
            record_diagnostic(
                LuaDiagnosticKind::memory_limit,
                "<runtime>",
                "could not create the Lua state within the memory limit"
            );
            return false;
        }

        (void)lua_atpanic(state, &Impl::panic);
        luaL_checkversion(state);
        (void)lua_checkstack(state, 64);

        luaL_requiref(state, LUA_GNAME, luaopen_base, 1);
        lua_pop(state, 1);
        luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1);
        lua_pop(state, 1);
        luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1);
        lua_pop(state, 1);
        luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1);
        lua_pop(state, 1);
        luaL_requiref(state, LUA_UTF8LIBNAME, luaopen_utf8, 1);
        lua_pop(state, 1);

        constexpr std::array<const char*, 13> removed_globals{
            "io",
            "os",
            "package",
            "debug",
            "dofile",
            "loadfile",
            "require",
            "load",
            "collectgarbage",
            "warn",
            "module",
            "setmetatable",
            "getmetatable",
        };
        for (const char* name : removed_globals) {
            lua_pushnil(state);
            lua_setglobal(state, name);
        }

        lua_getglobal(state, "string");
        if (lua_type(state, -1) == LUA_TTABLE) {
            constexpr std::array<const char*, 5> removed_string_functions{
                "dump",
                "find",
                "match",
                "gmatch",
                "gsub",
            };
            for (const char* name : removed_string_functions) {
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
        lua_pop(state, 1);

        lua_getglobal(state, "table");
        if (lua_type(state, -1) == LUA_TTABLE) {
            constexpr std::array<const char*, 2> removed_table_functions{
                "move",
                "concat",
            };
            for (const char* name : removed_table_functions) {
                lua_pushnil(state);
                lua_setfield(state, -2, name);
            }
        }
        lua_pop(state, 1);

        // Lua 5.4 otherwise seeds math.random from wall-clock time and the
        // lua_State address. Seed it explicitly and remove randomseed so a mod
        // cannot reintroduce process entropy with randomseed().
        lua_getglobal(state, "math");
        if (lua_type(state, -1) != LUA_TTABLE) {
            lua_pop(state, 1);
            record_diagnostic(
                LuaDiagnosticKind::load_error,
                "<runtime>",
                "Lua math library is unavailable"
            );
            return false;
        }
        lua_getfield(state, -1, "randomseed");
        lua_pushinteger(
            state,
            static_cast<lua_Integer>(
                static_cast<std::uint32_t>(config.deterministic_seed)
            )
        );
        lua_pushinteger(
            state,
            static_cast<lua_Integer>(
                static_cast<std::uint32_t>(config.deterministic_seed >> 32U)
            )
        );
        if (lua_pcall(state, 2, 0, 0) != LUA_OK) {
            const char* message = lua_tostring(state, -1);
            record_diagnostic(
                LuaDiagnosticKind::load_error,
                "<runtime>",
                message != nullptr ? message : "could not seed Lua PRNG"
            );
            lua_settop(state, 0);
            return false;
        }
        lua_pushnil(state);
        lua_setfield(state, -2, "randomseed");
        lua_pop(state, 1);

        register_host_function("debugPrint", &Impl::host_debug_print);
        register_host_function(
            "getSongPosition",
            &Impl::host_get_song_position
        );
        register_host_function("getProperty", &Impl::host_get_property);
        register_host_function("setProperty", &Impl::host_set_property);
        register_host_function("addScore", &Impl::host_add_score);
        register_host_function("setHealth", &Impl::host_set_health);
        register_host_function("triggerEvent", &Impl::host_trigger_event);
        // PULSEFORGE_P1_1_16_PSYCH_UTILITY_HARD_REGISTER_V1
        register_host_function("getRandomInt", &Impl::host_get_random_int);
        register_host_function("getRandomFloat", &Impl::host_get_random_float);
        register_host_function("getRandomBool", &Impl::host_get_random_bool);
        register_host_function("getColorFromHex", &Impl::host_get_color_from_hex);
        register_host_function(
            "getColorFromString", &Impl::host_get_color_from_string
        );
        register_host_function(
            "stringStartsWith", &Impl::host_string_starts_with
        );
        register_host_function(
            "stringEndsWith", &Impl::host_string_ends_with
        );
        register_host_function("stringTrim", &Impl::host_string_trim);
        register_host_function("stringSplit", &Impl::host_string_split);

        // PULSEFORGE_P1_1_1_LAZY_PSYCH_GLOBALS_V1
        //
        // Do not pull application-specific properties unconditionally before
        // every callback. Resolve the small Psych compatibility-global
        // namespace lazily only when a script actually reads one of the names.
        lua_pushglobaltable(state);
        if (lua_getmetatable(state, -1) == 0) {
            lua_newtable(state);
        }
        // PULSEFORGE_P1_1_1_LAZY_GLOBALS_COMPILE_FIX_V2
        // lua_pushcfunction is a Lua macro. A lambda passed directly to it
        // contains commas that MSVC can interpret as extra macro arguments.
        // Use the underlying API function instead.
        lua_pushcclosure(
            state,
            +[](lua_State* lua) -> int {
                auto* self = from_state(lua);
                if (self == nullptr) {
                    lua_pushnil(lua);
                    return 1;
                }

                std::size_t name_size{};
                const char* const name_data =
                    lua_tolstring(lua, 2, &name_size);
                if (name_data == nullptr || name_size == 0U
                    || name_size > 128U) {
                    lua_pushnil(lua);
                    return 1;
                }

                const std::string_view name{name_data, name_size};
                // PULSEFORGE_P1_1_17_EXPANDED_LAZY_PSYCH_GLOBALS_V1
                const bool psych_lazy_global =
                    name == "bpm"
                    || name == "curBpm"
                    || name == "keyCount"
                    // PULSEFORGE_P1_1_18_NOTE_MULTIPLIER_LAZY_GLOBALS_V1
                    || name == "noteMultiplier"
                    || name == "noteMultiplierPlayer"
                    || name == "noteMultiplierOpponent"
                    || name == "screenWidth"
                    || name == "screenHeight"
                    || name == "crochet"
                    || name == "stepCrochet"
                    || name == "songLength"
                    || name == "songName"
                    || name == "difficultyName"
                    || name == "scrollSpeed"
                    || name == "songSpeed"
                    || name == "score"
                    || name == "songScore"
                    || name == "misses"
                    || name == "songMisses"
                    || name == "hits"
                    || name == "combo"
                    || name == "maxCombo"
                    || name == "rating"
                    || name == "ratingPercent"
                    || name == "accuracy"
                    || name == "ratingFC"
                    || name == "playbackRate"
                    || name == "botPlay"
                    || name == "cpuControlled"
                    || name == "autoplay"
                    || name == "practice"
                    || name == "practiceMode"
                    || name == "ghostTapping"
                    || name == "downscroll"
                    || name == "middlescroll"
                    || name == "middleScroll"
                    || name == "noFail"
                    || name == "startedCountdown"
                    || name == "inCutscene"
                    || name == "inGameOver"
                    // PULSEFORGE_1_0_0_OVERKILL_TURN_GLOBAL_V1
                    || name == "mustHitSection"
                    || name.starts_with("defaultPlayerStrumX")
                    || name.starts_with("defaultPlayerStrumY")
                    || name.starts_with("defaultOpponentStrumX")
                    || name.starts_with("defaultOpponentStrumY");

                if (!psych_lazy_global) {
                    lua_pushnil(lua);
                    return 1;
                }

                ScriptValue value;
                std::string property_error;
                try {
                    if (!self->host.get_property(
                            name, value, property_error
                        )) {
                        lua_pushnil(lua);
                        return 1;
                    }
                } catch (...) {
                    lua_pushnil(lua);
                    return 1;
                }

                push_script_value(
                    lua,
                    value,
                    self->config.max_host_string_bytes
                );
                return 1;
            },
            0
        );
        lua_setfield(state, -2, "__index");
        static_cast<void>(lua_setmetatable(state, -2));
        lua_pop(state, 1);

        // Common Psych Engine compatibility calls. They remain scalar-only and
        // are delegated to the application host, so Lua never receives SDL or
        // native engine pointers. Unsupported calls fail with a diagnostic
        // instead of becoming an unbound nil global.
        constexpr std::array compatibility_functions{
            // PULSEFORGE_P1_1_16_PSYCH_CORE_COMPAT_BATCH_V1
            "getPropertyFromClass", "setPropertyFromClass",
            "getPropertyFromGroup", "setPropertyFromGroup",
            "makeLuaSprite", "makeAnimatedLuaSprite", "makeGraphic",
            "loadGraphic", "precacheImage", "setBlendMode",
            "addLuaSprite", "removeLuaSprite", "luaSpriteExists",
            "getPropertyLuaSprite", "setPropertyLuaSprite",
            "setScrollFactor", "setLuaSpriteScrollFactor", "scaleObject",
            "setGraphicSize", "updateHitbox", "setObjectCamera",
            "setObjectOrder", "getObjectOrder", "screenCenter",
            "characterPlayAnim", "playAnim", "objectPlayAnimation",
            "addAnimationByPrefix", "addAnimationByIndices",
            "doTweenX", "doTweenY", "doTweenAngle", "doTweenAlpha",
            "doTweenZoom", "doTweenScaleX", "doTweenScaleY",
            "noteTweenX", "noteTweenY",
            "noteTweenAngle", "noteTweenAlpha",
            // PULSEFORGE_P1_1_17_MODCHART_RUNTIME_API_V1
            "noteTweenScaleX", "noteTweenScaleY", "cancelTween",
            "runTimer", "cancelTimer",
            "cameraShake", "cameraFlash", "cameraSetTarget",
            "addCameraScroll", "addCameraFollowPoint",
            "getCameraScrollX", "getCameraScrollY",
            "getCameraFollowX", "getCameraFollowY",
            "makeLuaText", "setTextAlignment", "setTextSize", "addLuaText",
            "luaTextExists", "removeLuaText", "getTextString",
            "setTextColor", "setTextBorder", "setTextWidth",
            "setTextItalic", "setTextFont",
            "setHealthBarColors", "setTimeBarColors",
            // PULSEFORGE_P1_1_18_PSYCH_AUDIO_SCRIPT_API_V1
            "precacheSound", "playSound", "stopSound",
            "pauseSound", "resumeSound",
            "soundFadeIn", "soundFadeOut", "soundFadeCancel",
            "getSoundTime", "setSoundTime",
            "getSoundVolume", "setSoundVolume", "soundPlaying",
            "playMusic", "stopMusic", "pauseMusic", "resumeMusic",
            // PULSEFORGE_P1_1_18_DYNAMIC_LUA_SCRIPT_API_V1
            "addLuaScript", "removeLuaScript", "luaScriptExists",
            "setVar", "getVar", "removeVar",
            "restartSong", "endSong", "getSongLength",
            "getCharacterX", "getCharacterY",
            "setCharacterX", "setCharacterY",
            "getMidpointX", "getMidpointY",
            "getGraphicMidpointX", "getGraphicMidpointY",
            "getMouseX", "getMouseY", "callMethod",
            // PULSEFORGE_P1_1_12_SHADER_API_RESTORE_V1
            "initLuaShader", "setSpriteShader", "removeSpriteShader",
            "setShaderFloat", "setShaderInt", "setShaderBool",
            "setShaderFloatArray", "setShaderIntArray",
            "getShaderFloat",
            // PULSEFORGE_P1_1_2_STAGE_UNBLOCK_V1
            "wavyEffect", "addGlitchEffect", "close",
        };
        for (const auto* function_name : compatibility_functions) {
            register_host_command(function_name);
        }

        // PULSEFORGE_P1_1_2_WAVY_HARD_REGISTER_V2
        // Explicit registration is intentional: the previous array-only
        // edit was present in source but wavyEffect was still nil at runtime.
        register_host_command("wavyEffect");
        // PULSEFORGE_1_0_0_OVERKILL_GLITCH_COMPAT_V1
        register_host_command("addGlitchEffect");
        register_host_command("close");

        // PULSEFORGE_P1_1_17_MODCHART_RUNTIME_HARD_REGISTER_V1
        for (const auto* function_name : std::array{
                 "setVar", "getVar", "removeVar",
                 "restartSong", "endSong", "getSongLength",
                 "getCharacterX", "getCharacterY",
                 "setCharacterX", "setCharacterY",
                 "addCameraScroll", "addCameraFollowPoint",
                 "getCameraScrollX", "getCameraScrollY",
                 "getCameraFollowX", "getCameraFollowY",
                 "noteTweenScaleX", "noteTweenScaleY",
             }) {
            register_host_command(function_name);
        }

        // PULSEFORGE_P1_1_18_AUDIO_DYNAMIC_SCRIPT_HARD_REGISTER_V1
        for (const auto* function_name : std::array{
                 "precacheSound", "playSound", "stopSound",
                 "pauseSound", "resumeSound",
                 "soundFadeIn", "soundFadeOut", "soundFadeCancel",
                 "getSoundTime", "setSoundTime",
                 "getSoundVolume", "setSoundVolume", "soundPlaying",
                 "playMusic", "stopMusic", "pauseMusic", "resumeMusic",
                 "addLuaScript", "removeLuaScript", "luaScriptExists",
             }) {
            register_host_command(function_name);
        }

        // PULSEFORGE_P1_1_12_SHADER_API_HARD_REGISTER_V1
        // Keep these explicit as well as array-registered: the wavy bootstrap
        // proved that a compatibility surface must not silently regress to nil.
        register_host_command("initLuaShader");
        register_host_command("setSpriteShader");
        register_host_command("removeSpriteShader");
        register_host_command("setShaderFloat");
        register_host_command("setShaderInt");
        register_host_command("setShaderBool");
        // PULSEFORGE_P1_1_15_SHADER_ARRAY_HARD_REGISTER_V1
        // Override the generic scalar-only closure with table-aware adapters.
        register_host_function(
            "setShaderFloatArray",
            &Impl::host_set_shader_float_array
        );
        register_host_function(
            "setShaderIntArray",
            &Impl::host_set_shader_int_array
        );
        register_host_command("getShaderFloat");

        // PULSEFORGE_P1_1_3_LAZY_GLOBALS_COMMANDS_V1
        // Explicit registration avoids another array-only regression and keeps
        // these Psych compatibility calls visible in every isolated Lua state.
        register_host_command("setTextString");
        register_host_command("keyboardJustPressed");
        register_host_command("startCountdown");
        // Compatibility alias for a typo present in the EZ Edition corpus.
        register_host_command("addLuaSprire");

        lua_getglobal(state, "debugPrint");
        lua_setglobal(state, "print");

        set_global_string("Function_Stop", function_stop);
        set_global_string(
            "Function_StopLua",
            function_stop_lua
        );
        set_global_string(
            "Function_StopHScript",
            "##PSYCHLUA_FUNCTIONSTOPHSCRIPT"
        );
        set_global_string("Function_StopAll", function_stop_all);
        set_global_string(
            "Function_Continue",
            "##PSYCHLUA_FUNCTIONCONTINUE"
        );
        set_global_string("version", "PulseForge Lua 5.4");
        lua_pushboolean(state, 0);
        lua_setglobal(state, "luaDebugMode");
        lua_pushinteger(state, 0);
        lua_setglobal(state, "curBeat");
        lua_pushinteger(state, 0);
        lua_setglobal(state, "curStep");
        lua_pushinteger(state, 0);
        lua_setglobal(state, "curSection");
        lua_pushnumber(state, 0.0);
        lua_setglobal(state, "curDecBeat");
        lua_pushnumber(state, 0.0);
        lua_setglobal(state, "curDecStep");
        lua_pushnumber(state, 0.0);
        lua_setglobal(state, "songPosition");

        lua_pushliteral(
            state,
            "PulseForge Lua instruction budget exceeded"
        );
        lua_setfield(
            state,
            LUA_REGISTRYINDEX,
            instruction_error_registry_key
        );
        return true;
    }

    void register_host_function(
        const char* name,
        const lua_CFunction function
    ) {
        lua_pushcfunction(state, function);
        lua_setglobal(state, name);
    }

    void register_host_command(const char* name) {
        lua_pushstring(state, name);
        lua_pushcclosure(state, &Impl::host_invoke_function, 1);
        lua_setglobal(state, name);
    }

    void set_global_string(
        const char* name,
        const std::string_view value
    ) {
        lua_pushlstring(state, value.data(), value.size());
        lua_setglobal(state, name);
    }

    void close_state() noexcept {
        if (state != nullptr) {
            lua_close(state);
            state = nullptr;
        }
        script_loaded = false;
        create_called = false;
        destroy_called = false;
        disabled.fill(false);
        absent.fill(false);
        allocator.current = 0;
    }

    [[nodiscard]] bool load_script(
        const std::string_view source,
        const std::string_view source_name
    ) {
        if (source.size() > config.max_script_bytes) {
            record_diagnostic(
                LuaDiagnosticKind::load_error,
                "<chunk>",
                "script exceeds the configured source-size limit"
            );
            return false;
        }
        if (!create_state()) {
            return false;
        }

        const std::string chunk_name(
            source_name.substr(0, maximum_property_name_bytes)
        );
        set_global_string(
            "scriptName",
            chunk_name.empty()
                ? std::string_view{"@mod.lua"}
                : std::string_view{chunk_name}
        );
        allocator.denied = false;
        const int load_status = luaL_loadbufferx(
            state,
            source.data(),
            source.size(),
            chunk_name.empty() ? "@mod.lua" : chunk_name.c_str(),
            "t"
        );
        if (load_status != LUA_OK) {
            record_diagnostic(
                failure_kind(LuaDiagnosticKind::load_error),
                "<chunk>",
                stack_error_message()
            );
            close_state();
            return false;
        }

        lua_pushcfunction(state, &Impl::traceback);
        lua_insert(state, -2);
        const int error_function = lua_gettop(state) - 1;
        begin_budget();
        const int call_status = lua_pcall(state, 0, 0, error_function);
        end_budget();

        const bool contained_limit_failure =
            allocator.denied || instruction_denied;
        if (call_status != LUA_OK || contained_limit_failure) {
            const std::string message =
                call_status == LUA_OK
                    ? (instruction_denied
                        ? "PulseForge Lua instruction budget exceeded"
                        : "PulseForge Lua memory limit exceeded")
                    : stack_error_message();
            record_diagnostic(
                failure_kind(LuaDiagnosticKind::runtime_error),
                "<chunk>",
                message
            );
            lua_settop(state, 0);
            close_state();
            return false;
        }

        lua_settop(state, 0);
        script_loaded = true;
        return true;
    }

    [[nodiscard]] bool return_requests_stop() const noexcept {
        if (lua_type(state, -1) == LUA_TSTRING) {
            std::size_t length = 0;
            const char* data = lua_tolstring(state, -1, &length);
            const std::string_view returned(data, length);
            return returned == function_stop
                || returned == function_stop_lua
                || returned == function_stop_all;
        }
        return lua_isinteger(state, -1) != 0
            && lua_tointeger(state, -1) == 1;
    }

    [[nodiscard]] LuaCallResult invoke(PendingCall call) {
        if (!script_loaded || state == nullptr) {
            return {LuaCallStatus::not_loaded};
        }

        const auto index = static_cast<std::size_t>(call.callback);
        if (disabled[index]) {
            return {LuaCallStatus::disabled};
        }
        if (absent[index]) {
            return {LuaCallStatus::missing};
        }
        if (call.callback == Callback::on_destroy && destroy_called) {
            return {LuaCallStatus::disabled};
        }

        lua_settop(state, 0);
        pending = call;
        pending.missing = false;

        lua_pushcfunction(state, &Impl::traceback);
        const int error_function = lua_gettop(state);
        lua_pushcfunction(state, &Impl::invoke_pending);

        begin_budget();
        const int call_status = lua_pcall(
            state,
            0,
            1,
            error_function
        );
        end_budget();

        if (pending.missing && call_status == LUA_OK) {
            absent[index] = true;
            lua_settop(state, 0);
            return {LuaCallStatus::missing};
        }

        const bool contained_limit_failure =
            allocator.denied || instruction_denied;
        if (call_status != LUA_OK || contained_limit_failure) {
            const std::string message =
                call_status == LUA_OK
                    ? (instruction_denied
                        ? "PulseForge Lua instruction budget exceeded"
                        : "PulseForge Lua memory limit exceeded")
                    : stack_error_message();
            record_diagnostic(
                failure_kind(LuaDiagnosticKind::runtime_error),
                callback_names[index],
                message
            );
            if (config.disable_callback_after_error) {
                disabled[index] = true;
            }
            ++runtime_stats.failed_callbacks;
            lua_settop(state, 0);
            return {LuaCallStatus::failed};
        }

        const bool requested_stop = return_requests_stop();
        ++runtime_stats.successful_callbacks;
        lua_settop(state, 0);
        return {
            requested_stop
                ? LuaCallStatus::stop_requested
                : LuaCallStatus::completed
        };
    }

    [[nodiscard]] LuaCallResult no_argument_call(const Callback callback) {
        PendingCall call;
        call.callback = callback;
        return invoke(call);
    }

    [[nodiscard]] LuaCallResult elapsed_call(
        const Callback callback,
        const double elapsed_seconds
    ) {
        PendingCall call;
        call.callback = callback;
        call.argument_count = 1;
        call.arguments[0].kind = ArgumentKind::number;
        call.arguments[0].number =
            std::isfinite(elapsed_seconds)
                ? std::max(elapsed_seconds, 0.0)
                : 0.0;
        return invoke(call);
    }

    [[nodiscard]] LuaCallResult musical_call(
        const Callback callback,
        const std::int64_t index
    ) {
        PendingCall call;
        call.callback = callback;
        call.argument_count = 1;
        call.arguments[0].kind = ArgumentKind::integer;
        call.arguments[0].integer = index;
        if (callback == Callback::on_beat_hit) {
            call.update_beat = true;
            call.beat = index;
        } else if (callback == Callback::on_step_hit) {
            call.update_step = true;
            call.step = index;
        } else if (callback == Callback::on_section_hit) {
            call.update_section = true;
            call.section = index;
        }
        return invoke(call);
    }

    [[nodiscard]] LuaCallResult direction_call(
        const Callback callback,
        const std::uint16_t direction
    ) {
        PendingCall call;
        call.callback = callback;
        call.argument_count = 1;
        call.arguments[0].kind = ArgumentKind::integer;
        call.arguments[0].integer = static_cast<std::int64_t>(direction);
        return invoke(call);
    }

    [[nodiscard]] LuaCallResult note_call(
        const Callback callback,
        const std::size_t note_index,
        const std::uint16_t direction,
        const std::string_view note_type,
        const bool is_sustain_note
    ) {
        PendingCall call;
        call.callback = callback;
        call.argument_count = 4;
        call.arguments[0].kind = ArgumentKind::integer;
        const auto maximum_index =
            static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max());
        call.arguments[0].integer = static_cast<std::int64_t>(
            std::min(note_index, maximum_index)
        );
        call.arguments[1].kind = ArgumentKind::integer;
        call.arguments[1].integer = static_cast<std::int64_t>(direction);
        call.arguments[2].kind = ArgumentKind::string;
        call.arguments[2].string = note_type;
        call.arguments[3].kind = ArgumentKind::boolean;
        call.arguments[3].boolean = is_sustain_note;
        return invoke(call);
    }

    [[nodiscard]] LuaCallResult event_call(
        const std::string_view name,
        const std::string_view value1,
        const std::string_view value2
    ) {
        PendingCall call;
        call.callback = Callback::on_event;
        call.argument_count = 3;
        call.arguments[0].kind = ArgumentKind::string;
        call.arguments[0].string = name;
        call.arguments[1].kind = ArgumentKind::string;
        call.arguments[1].string = value1;
        call.arguments[2].kind = ArgumentKind::string;
        call.arguments[2].string = value2;
        return invoke(call);
    }

    [[nodiscard]] LuaCallResult destroy() {
        if (destroy_called) {
            return {LuaCallStatus::disabled};
        }
        const auto result = no_argument_call(Callback::on_destroy);
        destroy_called = true;
        return result;
    }

    void unload() {
        if (script_loaded && !destroy_called) {
            (void)destroy();
        }
        close_state();
    }
};

LuaRuntime::LuaRuntime(
    LuaHostInterface& host,
    LuaRuntimeConfig config
)
    : impl_(std::make_unique<Impl>(host, std::move(config))) {
}

LuaRuntime::~LuaRuntime() = default;

LuaRuntime::LuaRuntime(LuaRuntime&& other) noexcept = default;

LuaRuntime& LuaRuntime::operator=(LuaRuntime&& other) noexcept = default;

bool LuaRuntime::load_script(
    const std::string_view source,
    const std::string_view source_name
) {
    if (impl_ == nullptr) {
        return false;
    }
    impl_->unload();
    impl_->diagnostic_log.clear();
    return impl_->load_script(source, source_name);
}

void LuaRuntime::unload() {
    if (impl_ != nullptr) {
        impl_->unload();
    }
}

bool LuaRuntime::loaded() const noexcept {
    return impl_ != nullptr && impl_->script_loaded;
}

LuaCallResult LuaRuntime::on_create() {
    if (impl_ == nullptr) {
        return {LuaCallStatus::not_loaded};
    }
    if (impl_->create_called) {
        return {LuaCallStatus::disabled};
    }
    const auto result =
        impl_->no_argument_call(Impl::Callback::on_create);
    impl_->create_called = true;
    return result;
}

LuaCallResult LuaRuntime::on_create_post() {
    if (impl_ == nullptr) {
        return {LuaCallStatus::not_loaded};
    }
    return impl_->no_argument_call(Impl::Callback::on_create_post);
}

LuaCallResult LuaRuntime::on_start_countdown() {
    // PULSEFORGE_P1_1_4_ONSTARTCOUNTDOWN_V1
    if (impl_ == nullptr) {
        return {LuaCallStatus::not_loaded};
    }
    return impl_->no_argument_call(Impl::Callback::on_start_countdown);
}

LuaCallResult LuaRuntime::on_song_start() {
    if (impl_ == nullptr) {
        return {LuaCallStatus::not_loaded};
    }
    return impl_->no_argument_call(Impl::Callback::on_song_start);
}

LuaCallResult LuaRuntime::on_end_song() {
    if (impl_ == nullptr) {
        return {LuaCallStatus::not_loaded};
    }
    return impl_->no_argument_call(Impl::Callback::on_end_song);
}

LuaCallResult LuaRuntime::on_game_over() {
    if (impl_ == nullptr) {
        return {LuaCallStatus::not_loaded};
    }
    return impl_->no_argument_call(Impl::Callback::on_game_over);
}

LuaCallResult LuaRuntime::on_pause() {
    if (impl_ == nullptr) {
        return {LuaCallStatus::not_loaded};
    }
    return impl_->no_argument_call(Impl::Callback::on_pause);
}

LuaCallResult LuaRuntime::on_resume() {
    if (impl_ == nullptr) {
        return {LuaCallStatus::not_loaded};
    }
    return impl_->no_argument_call(Impl::Callback::on_resume);
}

LuaCallResult LuaRuntime::on_update(const double elapsed_seconds) {
    if (impl_ == nullptr) {
        return {LuaCallStatus::not_loaded};
    }
    return impl_->elapsed_call(Impl::Callback::on_update, elapsed_seconds);
}

LuaCallResult LuaRuntime::on_update_post(const double elapsed_seconds) {
    if (impl_ == nullptr) {
        return {LuaCallStatus::not_loaded};
    }
    return impl_->elapsed_call(
        Impl::Callback::on_update_post,
        elapsed_seconds
    );
}

LuaCallResult LuaRuntime::on_beat_hit(const std::int64_t beat) {
    if (impl_ == nullptr) {
        return {LuaCallStatus::not_loaded};
    }
    return impl_->musical_call(Impl::Callback::on_beat_hit, beat);
}

LuaCallResult LuaRuntime::on_step_hit(const std::int64_t step) {
    if (impl_ == nullptr) {
        return {LuaCallStatus::not_loaded};
    }
    return impl_->musical_call(Impl::Callback::on_step_hit, step);
}

LuaCallResult LuaRuntime::on_section_hit(const std::int64_t section) {
    if (impl_ == nullptr) {
        return {LuaCallStatus::not_loaded};
    }
    return impl_->musical_call(Impl::Callback::on_section_hit, section);
}

LuaCallResult LuaRuntime::good_note_hit(
    const std::size_t note_index,
    const std::uint16_t direction,
    const std::string_view note_type,
    const bool is_sustain_note
) {
    if (impl_ == nullptr) {
        return {LuaCallStatus::not_loaded};
    }
    return impl_->note_call(
        Impl::Callback::good_note_hit,
        note_index,
        direction,
        note_type,
        is_sustain_note
    );
}

LuaCallResult LuaRuntime::opponent_note_hit(
    const std::size_t note_index,
    const std::uint16_t direction,
    const std::string_view note_type,
    const bool is_sustain_note
) {
    if (impl_ == nullptr) {
        return {LuaCallStatus::not_loaded};
    }
    return impl_->note_call(
        Impl::Callback::opponent_note_hit,
        note_index,
        direction,
        note_type,
        is_sustain_note
    );
}

LuaCallResult LuaRuntime::note_miss(
    const std::size_t note_index,
    const std::uint16_t direction,
    const std::string_view note_type,
    const bool is_sustain_note
) {
    if (impl_ == nullptr) {
        return {LuaCallStatus::not_loaded};
    }
    return impl_->note_call(
        Impl::Callback::note_miss,
        note_index,
        direction,
        note_type,
        is_sustain_note
    );
}

LuaCallResult LuaRuntime::note_miss_press(const std::uint16_t direction) {
    if (impl_ == nullptr) {
        return {LuaCallStatus::not_loaded};
    }
    return impl_->direction_call(Impl::Callback::note_miss_press, direction);
}

LuaCallResult LuaRuntime::on_ghost_tap(const std::uint16_t direction) {
    if (impl_ == nullptr) {
        return {LuaCallStatus::not_loaded};
    }
    return impl_->direction_call(Impl::Callback::on_ghost_tap, direction);
}

LuaCallResult LuaRuntime::on_event(
    const std::string_view name,
    const std::string_view value1,
    const std::string_view value2
) {
    if (impl_ == nullptr) {
        return {LuaCallStatus::not_loaded};
    }
    return impl_->event_call(name, value1, value2);
}

LuaCallResult LuaRuntime::on_tween_completed(
    const std::string_view tag,
    const std::string_view target
) {
    if (impl_ == nullptr) {
        return {LuaCallStatus::not_loaded};
    }
    LuaRuntime::Impl::PendingCall call;
    call.callback = LuaRuntime::Impl::Callback::on_tween_completed;
    call.argument_count = 2U;
    call.arguments[0].kind = LuaRuntime::Impl::ArgumentKind::string;
    call.arguments[0].string = tag;
    call.arguments[1].kind = LuaRuntime::Impl::ArgumentKind::string;
    call.arguments[1].string = target;
    return impl_->invoke(call);
}

LuaCallResult LuaRuntime::on_timer_completed(
    const std::string_view tag,
    const std::int64_t loops,
    const std::int64_t loops_left
) {
    if (impl_ == nullptr) {
        return {LuaCallStatus::not_loaded};
    }
    LuaRuntime::Impl::PendingCall call;
    call.callback = LuaRuntime::Impl::Callback::on_timer_completed;
    call.argument_count = 3U;
    call.arguments[0].kind = LuaRuntime::Impl::ArgumentKind::string;
    call.arguments[0].string = tag;
    call.arguments[1].kind = LuaRuntime::Impl::ArgumentKind::integer;
    call.arguments[1].integer = loops;
    call.arguments[2].kind = LuaRuntime::Impl::ArgumentKind::integer;
    call.arguments[2].integer = loops_left;
    return impl_->invoke(call);
}

// PULSEFORGE_P1_1_18_SOUND_COMPLETION_CALLBACK_IMPL_V1
LuaCallResult LuaRuntime::on_sound_finished(
    const std::string_view tag
) {
    if (impl_ == nullptr) {
        return {LuaCallStatus::not_loaded};
    }
    LuaRuntime::Impl::PendingCall call;
    call.callback = LuaRuntime::Impl::Callback::on_sound_finished;
    call.argument_count = 1U;
    call.arguments[0].kind = LuaRuntime::Impl::ArgumentKind::string;
    call.arguments[0].string = tag;
    return impl_->invoke(call);
}

LuaCallResult LuaRuntime::on_destroy() {
    if (impl_ == nullptr) {
        return {LuaCallStatus::not_loaded};
    }
    return impl_->destroy();
}

LuaDispatchReport LuaRuntime::dispatch_frame(
    const GameplaySession& session,
    const double elapsed_seconds
) {
    LuaDispatchReport report;
    if (impl_ == nullptr) {
        report.skipped = session.frame_events().size() + 1U;
        return report;
    }

    // PULSEFORGE_P1_1_18_LOGICAL_CALLBACK_FANOUT_V1
    // Note Multiplier changes logical note polyphony. Small/normal values
    // therefore receive one Psych callback per logical note, while the global
    // per-frame callback budget remains the hard containment boundary.
    constexpr std::size_t fixed_callbacks = 2U;  // update + updatePost
    const auto maximum_callbacks = impl_->config.max_callbacks_per_frame;
    if (maximum_callbacks < fixed_callbacks) {
        impl_->record_diagnostic(
            LuaDiagnosticKind::instruction_limit,
            "<frame>",
            "Lua callback budget is too small for update/updatePost"
        );
        report.failed = 1U;
        return report;
    }

    std::size_t planned_callbacks = fixed_callbacks;
    const auto plan = [&](const std::uint64_t count) noexcept {
        const auto available = maximum_callbacks - planned_callbacks;
        const auto addition = static_cast<std::size_t>(
            std::min<std::uint64_t>(count, available)
        );
        planned_callbacks += addition;
    };
    for (const auto& event : session.frame_events()) {
        switch (event.type) {
        case GameplayEventType::note_hit:
        case GameplayEventType::note_miss:
        case GameplayEventType::hold_tick:
        case GameplayEventType::hold_drop:
        case GameplayEventType::opponent_hit:
            plan(event.logical_occurrence_count);
            break;
        case GameplayEventType::ghost_tap:
            plan(2U);
            break;
        case GameplayEventType::step:
            plan(event.musical_index >= 0
                    && event.musical_index % 16 == 0
                ? 2U
                : 1U);
            break;
        case GameplayEventType::chart_event:
        case GameplayEventType::beat:
        case GameplayEventType::song_complete:
        case GameplayEventType::failed:
            plan(1U);
            break;
        case GameplayEventType::hold_complete:
            break;
        }
    }

    const auto original_instruction_budget = impl_->config.instruction_budget;
    const auto aggregate_budget = std::max<std::uint64_t>(
        impl_->config.frame_instruction_budget,
        impl_->config.instruction_hook_interval
    );
    const auto divided_budget = aggregate_budget
        / std::max<std::uint64_t>(
            static_cast<std::uint64_t>(planned_callbacks),
            1U
        );
    impl_->config.instruction_budget = std::max<std::uint64_t>(
        std::min(original_instruction_budget, divided_budget),
        impl_->config.instruction_hook_interval
    );

    accumulate(report, on_update(elapsed_seconds));

    const std::size_t event_callback_budget =
        maximum_callbacks - fixed_callbacks;
    std::size_t event_callbacks_used = 0U;
    bool fanout_truncated = false;

    const auto add_skipped = [&](const std::uint64_t count) noexcept {
        const auto maximum = std::numeric_limits<std::size_t>::max();
        const auto bounded = static_cast<std::size_t>(
            std::min<std::uint64_t>(
                count,
                static_cast<std::uint64_t>(maximum)
            )
        );
        report.skipped = bounded > maximum - report.skipped
            ? maximum
            : report.skipped + bounded;
    };

    const auto repeat = [&](
        const std::uint64_t count,
        auto&& callback
    ) {
        const auto available = event_callbacks_used < event_callback_budget
            ? event_callback_budget - event_callbacks_used
            : 0U;
        const auto runs = static_cast<std::size_t>(
            std::min<std::uint64_t>(count, available)
        );
        for (std::size_t index = 0U; index < runs; ++index) {
            accumulate(report, callback(index));
        }
        event_callbacks_used += runs;
        if (count > runs) {
            add_skipped(count - static_cast<std::uint64_t>(runs));
            fanout_truncated = true;
        }
    };

    for (const auto& event : session.frame_events()) {
        switch (event.type) {
        case GameplayEventType::note_hit:
        case GameplayEventType::hold_tick: {
            if (event.note_index >= session.chart().notes.size()) {
                ++report.skipped;
                break;
            }
            const auto& note = session.chart().notes[event.note_index];
            repeat(
                event.logical_occurrence_count,
                [&](const std::size_t) {
                    return good_note_hit(
                        event.note_index,
                        session.display_lane(event.note_index),
                        note.kind,
                        event.type == GameplayEventType::hold_tick
                    );
                }
            );
            break;
        }
        case GameplayEventType::note_miss:
        case GameplayEventType::hold_drop: {
            if (event.note_index >= session.chart().notes.size()) {
                ++report.skipped;
                break;
            }
            const auto& note = session.chart().notes[event.note_index];
            repeat(
                event.logical_occurrence_count,
                [&](const std::size_t) {
                    return note_miss(
                        event.note_index,
                        session.display_lane(event.note_index),
                        note.kind,
                        event.type == GameplayEventType::hold_drop
                    );
                }
            );
            break;
        }
        case GameplayEventType::chart_event: {
            if (event.chart_event_index >= session.chart().events.size()) {
                ++report.skipped;
                break;
            }
            const auto& chart_event =
                session.chart().events[event.chart_event_index];
            repeat(
                1U,
                [&](const std::size_t) {
                    return on_event(
                        chart_event.name,
                        chart_event.value1,
                        chart_event.value2
                    );
                }
            );
            break;
        }
        case GameplayEventType::beat:
            repeat(
                1U,
                [&](const std::size_t) {
                    return on_beat_hit(event.musical_index);
                }
            );
            break;
        case GameplayEventType::step:
            repeat(
                1U,
                [&](const std::size_t) {
                    return on_step_hit(event.musical_index);
                }
            );
            if (event.musical_index >= 0
                && event.musical_index % 16 == 0) {
                repeat(
                    1U,
                    [&](const std::size_t) {
                        return on_section_hit(event.musical_index / 16);
                    }
                );
            }
            break;
        case GameplayEventType::hold_complete:
            break;
        case GameplayEventType::ghost_tap: {
            const auto raw_direction = std::max<std::int64_t>(
                event.musical_index,
                0
            );
            const auto direction = static_cast<std::uint16_t>(
                std::min<std::int64_t>(
                    raw_direction,
                    std::numeric_limits<std::uint16_t>::max()
                )
            );
            repeat(
                1U,
                [&](const std::size_t) {
                    return note_miss_press(direction);
                }
            );
            repeat(
                1U,
                [&](const std::size_t) {
                    return on_ghost_tap(direction);
                }
            );
            break;
        }
        case GameplayEventType::opponent_hit: {
            if (event.note_index >= session.chart().notes.size()) {
                ++report.skipped;
                break;
            }
            const auto& note = session.chart().notes[event.note_index];
            repeat(
                event.logical_occurrence_count,
                [&](const std::size_t) {
                    return opponent_note_hit(
                        event.note_index,
                        session.display_lane(event.note_index),
                        note.kind,
                        false
                    );
                }
            );
            break;
        }
        case GameplayEventType::song_complete:
            repeat(
                1U,
                [&](const std::size_t) {
                    return on_end_song();
                }
            );
            break;
        case GameplayEventType::failed:
            repeat(
                1U,
                [&](const std::size_t) {
                    return on_game_over();
                }
            );
            break;
        }
    }

    if (fanout_truncated) {
        impl_->record_diagnostic(
            LuaDiagnosticKind::instruction_limit,
            "<frame>",
            "logical note callback fan-out was truncated by the per-frame safety limit"
        );
    }

    accumulate(report, on_update_post(elapsed_seconds));
    impl_->config.instruction_budget = original_instruction_budget;
    return report;
}

LuaDispatchReport LuaRuntime::dispatch_frame(
    const StreamingGameplaySession& session,
    const double elapsed_seconds
) {
    LuaDispatchReport report;
    const auto events = session.frame_events();
    if (impl_ == nullptr) {
        report.skipped = events.size() + 1U;
        return report;
    }

    // PULSEFORGE_P1_1_18_STREAMING_LOGICAL_CALLBACK_FANOUT_V1
    constexpr std::size_t fixed_callbacks = 2U;
    const auto maximum_callbacks = impl_->config.max_callbacks_per_frame;
    if (maximum_callbacks < fixed_callbacks) {
        impl_->record_diagnostic(
            LuaDiagnosticKind::instruction_limit,
            "<frame>",
            "Lua callback budget is too small for update/updatePost"
        );
        report.failed = 1U;
        return report;
    }

    std::size_t planned_callbacks = fixed_callbacks;
    const auto plan = [&](const std::uint64_t count) noexcept {
        const auto available = maximum_callbacks - planned_callbacks;
        const auto addition = static_cast<std::size_t>(
            std::min<std::uint64_t>(count, available)
        );
        planned_callbacks += addition;
    };
    for (const auto& event : events) {
        switch (event.type) {
        case GameplayEventType::note_hit:
        case GameplayEventType::note_miss:
        case GameplayEventType::hold_tick:
        case GameplayEventType::hold_drop:
        case GameplayEventType::opponent_hit:
            plan(event.logical_occurrence_count);
            break;
        case GameplayEventType::beat:
            plan(event.occurrence_count);
            break;
        case GameplayEventType::step: {
            // Each aggregated step can also cross a section boundary.
            const auto maximum =
                static_cast<std::uint64_t>(maximum_callbacks);
            const auto doubled = event.occurrence_count > maximum / 2U
                ? maximum
                : event.occurrence_count * 2U;
            plan(std::min(doubled, maximum));
            break;
        }
        case GameplayEventType::ghost_tap:
            plan(2U);
            break;
        case GameplayEventType::chart_event:
        case GameplayEventType::song_complete:
        case GameplayEventType::failed:
            plan(1U);
            break;
        case GameplayEventType::hold_complete:
            break;
        }
    }

    const auto original_instruction_budget = impl_->config.instruction_budget;
    const auto aggregate_budget = std::max<std::uint64_t>(
        impl_->config.frame_instruction_budget,
        impl_->config.instruction_hook_interval
    );
    const auto divided_budget = aggregate_budget
        / std::max<std::uint64_t>(
            static_cast<std::uint64_t>(planned_callbacks),
            1U
        );
    impl_->config.instruction_budget = std::max<std::uint64_t>(
        std::min(original_instruction_budget, divided_budget),
        impl_->config.instruction_hook_interval
    );

    const auto kinds = session.reader().kinds();
    const auto bounded_note_index = [](const StreamingNoteId& id) noexcept {
        const auto maximum = static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max()
        );
        return static_cast<std::size_t>(std::min(id.note_index, maximum));
    };
    const auto event_kind = [&](const StreamingGameplayEvent& event) {
        if (!event.has_visual_note || event.visual_note.kind_id >= kinds.size()) {
            return std::string_view{"normal"};
        }
        return std::string_view{kinds[event.visual_note.kind_id]};
    };

    accumulate(report, on_update(elapsed_seconds));

    const std::size_t event_callback_budget =
        maximum_callbacks - fixed_callbacks;
    std::size_t event_callbacks_used = 0U;
    bool fanout_truncated = false;

    const auto add_skipped = [&](const std::uint64_t count) noexcept {
        const auto maximum = std::numeric_limits<std::size_t>::max();
        const auto bounded = static_cast<std::size_t>(
            std::min<std::uint64_t>(
                count,
                static_cast<std::uint64_t>(maximum)
            )
        );
        report.skipped = bounded > maximum - report.skipped
            ? maximum
            : report.skipped + bounded;
    };

    const auto repeat = [&](
        const std::uint64_t count,
        auto&& callback
    ) {
        const auto available = event_callbacks_used < event_callback_budget
            ? event_callback_budget - event_callbacks_used
            : 0U;
        const auto runs = static_cast<std::size_t>(
            std::min<std::uint64_t>(count, available)
        );
        for (std::size_t index = 0U; index < runs; ++index) {
            accumulate(report, callback(index));
        }
        event_callbacks_used += runs;
        if (count > runs) {
            add_skipped(count - static_cast<std::uint64_t>(runs));
            fanout_truncated = true;
        }
    };

    for (const auto& event : events) {
        const auto note_index = bounded_note_index(event.note_id);
        const auto direction = event.has_visual_note
            ? event.visual_display_lane
            : std::uint16_t{0U};
        const auto kind = event_kind(event);

        switch (event.type) {
        case GameplayEventType::note_hit:
            repeat(
                event.logical_occurrence_count,
                [&](const std::size_t) {
                    return good_note_hit(note_index, direction, kind, false);
                }
            );
            break;
        case GameplayEventType::hold_tick:
            repeat(
                event.logical_occurrence_count,
                [&](const std::size_t) {
                    return good_note_hit(note_index, direction, kind, true);
                }
            );
            break;
        case GameplayEventType::note_miss:
            repeat(
                event.logical_occurrence_count,
                [&](const std::size_t) {
                    return note_miss(note_index, direction, kind, false);
                }
            );
            break;
        case GameplayEventType::hold_drop:
            repeat(
                event.logical_occurrence_count,
                [&](const std::size_t) {
                    return note_miss(note_index, direction, kind, true);
                }
            );
            break;
        case GameplayEventType::opponent_hit:
            repeat(
                event.logical_occurrence_count,
                [&](const std::size_t) {
                    return opponent_note_hit(
                        note_index,
                        direction,
                        kind,
                        false
                    );
                }
            );
            break;
        case GameplayEventType::beat:
            repeat(
                event.occurrence_count,
                [&](const std::size_t index) {
                    const auto offset = static_cast<std::int64_t>(
                        std::min<std::uint64_t>(
                            index,
                            static_cast<std::uint64_t>(
                                std::numeric_limits<std::int64_t>::max()
                            )
                        )
                    );
                    return on_beat_hit(
                        event.musical_index > std::numeric_limits<std::int64_t>::max()
                                - offset
                            ? std::numeric_limits<std::int64_t>::max()
                            : event.musical_index + offset
                    );
                }
            );
            break;
        case GameplayEventType::step:
            for (std::uint64_t offset = 0U;
                 offset < event.occurrence_count;
                 ++offset) {
                const auto available =
                    event_callbacks_used < event_callback_budget
                    ? event_callback_budget - event_callbacks_used
                    : 0U;
                if (available == 0U) {
                    add_skipped(event.occurrence_count - offset);
                    fanout_truncated = true;
                    break;
                }
                const auto bounded_offset = static_cast<std::int64_t>(
                    std::min<std::uint64_t>(
                        offset,
                        static_cast<std::uint64_t>(
                            std::numeric_limits<std::int64_t>::max()
                        )
                    )
                );
                const auto step =
                    event.musical_index
                        > std::numeric_limits<std::int64_t>::max()
                            - bounded_offset
                    ? std::numeric_limits<std::int64_t>::max()
                    : event.musical_index + bounded_offset;
                repeat(
                    1U,
                    [&](const std::size_t) {
                        return on_step_hit(step);
                    }
                );
                if (step >= 0 && step % 16 == 0) {
                    repeat(
                        1U,
                        [&](const std::size_t) {
                            return on_section_hit(step / 16);
                        }
                    );
                }
            }
            break;
        case GameplayEventType::ghost_tap: {
            const auto raw = std::max<std::int64_t>(event.musical_index, 0);
            const auto lane = static_cast<std::uint16_t>(
                std::min<std::int64_t>(
                    raw,
                    std::numeric_limits<std::uint16_t>::max()
                )
            );
            repeat(
                1U,
                [&](const std::size_t) {
                    return note_miss_press(lane);
                }
            );
            repeat(
                1U,
                [&](const std::size_t) {
                    return on_ghost_tap(lane);
                }
            );
            break;
        }
        case GameplayEventType::song_complete:
            repeat(
                1U,
                [&](const std::size_t) {
                    return on_end_song();
                }
            );
            break;
        case GameplayEventType::failed:
            repeat(
                1U,
                [&](const std::size_t) {
                    return on_game_over();
                }
            );
            break;
        case GameplayEventType::chart_event:
            if (event.chart_event_index >= session.chart_events().size()) {
                ++report.skipped;
                break;
            } else {
                const auto& chart_event =
                    session.chart_events()[event.chart_event_index];
                repeat(
                    1U,
                    [&](const std::size_t) {
                        return on_event(
                            chart_event.name,
                            chart_event.value1,
                            chart_event.value2
                        );
                    }
                );
            }
            break;
        case GameplayEventType::hold_complete:
            break;
        }
    }

    if (fanout_truncated) {
        impl_->record_diagnostic(
            LuaDiagnosticKind::instruction_limit,
            "<frame>",
            "logical/streaming callback fan-out was truncated by the per-frame safety limit"
        );
    }

    accumulate(report, on_update_post(elapsed_seconds));
    impl_->config.instruction_budget = original_instruction_budget;
    return report;
}

std::span<const LuaDiagnostic> LuaRuntime::diagnostics() const noexcept {
    if (impl_ == nullptr) {
        return {};
    }
    return impl_->diagnostic_log;
}

void LuaRuntime::clear_diagnostics() noexcept {
    if (impl_ != nullptr) {
        impl_->diagnostic_log.clear();
    }
}

LuaRuntimeStats LuaRuntime::stats() const noexcept {
    if (impl_ == nullptr) {
        return {};
    }
    auto result = impl_->runtime_stats;
    result.current_memory_bytes = impl_->allocator.current;
    result.peak_memory_bytes = impl_->allocator.peak;
    return result;
}

}  // namespace pulseforge
