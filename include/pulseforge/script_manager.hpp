#pragma once

#include "pulseforge/lua_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulseforge {

// Order mirrors the Psych-family PlayState lifecycle. Scripts inside the same
// scope are ordered by priority and then by their normalized source name.
enum class ScriptOrigin : std::uint8_t {
    global,
    stage,
    girlfriend_character,
    opponent_character,
    player_character,
    note_type,
    event,
    song,
    explicit_file,
};

struct LuaScriptDefinition {
    std::string source_name;
    std::string source;
    ScriptOrigin origin{ScriptOrigin::explicit_file};
    std::int32_t priority{};
};

struct LuaScriptLoadReport {
    std::size_t requested{};
    std::size_t loaded{};
    std::size_t failed{};
    std::size_t duplicates{};

    [[nodiscard]] bool any_loaded() const noexcept {
        return loaded != 0;
    }
};

struct LuaManagerCallReport {
    LuaDispatchReport callbacks;
    bool stop_requested{};
};

struct LuaManagerDiagnostic {
    std::string source_name;
    LuaDiagnostic diagnostic;
};

// Owns one isolated Lua state per script. A broken or over-budget script is
// contained by LuaRuntime and does not disable the other states.
class LuaScriptManager {
public:
    explicit LuaScriptManager(
        LuaHostInterface& host,
        LuaRuntimeConfig per_script_config = {}
    );
    ~LuaScriptManager();

    LuaScriptManager(LuaScriptManager&&) noexcept;
    LuaScriptManager& operator=(LuaScriptManager&&) noexcept;

    LuaScriptManager(const LuaScriptManager&) = delete;
    LuaScriptManager& operator=(const LuaScriptManager&) = delete;

    // Replaces the active snapshot. Input sources are copied, sorted and
    // de-duplicated before any callbacks can run.
    [[nodiscard]] LuaScriptLoadReport load_scripts(
        std::span<const LuaScriptDefinition> definitions
    );

    // PULSEFORGE_P1_1_18_DYNAMIC_SCRIPT_MANAGER_API_V1
    [[nodiscard]] LuaScriptLoadReport add_script(
        LuaScriptDefinition definition
    );
    [[nodiscard]] bool remove_script(std::string_view source_name) noexcept;
    [[nodiscard]] bool contains(std::string_view source_name) const noexcept;
    [[nodiscard]] LuaManagerCallReport initialize_script(
        std::string_view source_name,
        bool song_started
    );

    void unload() noexcept;
    [[nodiscard]] std::size_t loaded_count() const noexcept;

    [[nodiscard]] LuaManagerCallReport on_create();
    [[nodiscard]] LuaManagerCallReport on_create_post();
    // PULSEFORGE_P1_1_4_MANAGER_COUNTDOWN_V1
    [[nodiscard]] LuaManagerCallReport on_start_countdown();
    [[nodiscard]] LuaManagerCallReport on_song_start();
    [[nodiscard]] LuaManagerCallReport on_end_song();
    [[nodiscard]] LuaManagerCallReport on_game_over();
    [[nodiscard]] LuaManagerCallReport on_pause();
    [[nodiscard]] LuaManagerCallReport on_resume();
    [[nodiscard]] LuaManagerCallReport on_tween_completed(
        std::string_view tag,
        std::string_view target = {}
    );
    [[nodiscard]] LuaManagerCallReport on_timer_completed(
        std::string_view tag,
        std::int64_t loops,
        std::int64_t loops_left
    );
    // PULSEFORGE_P1_1_18_RECURSIVE_EVENT_BUS_MANAGER_API_V1
    [[nodiscard]] LuaManagerCallReport on_event(
        std::string_view name,
        std::string_view value1 = {},
        std::string_view value2 = {}
    );
    [[nodiscard]] LuaManagerCallReport on_sound_finished(
        std::string_view tag
    );
    [[nodiscard]] LuaManagerCallReport on_destroy();

    [[nodiscard]] LuaManagerCallReport dispatch_frame(
        const GameplaySession& session,
        double elapsed_seconds
    );

    [[nodiscard]] LuaManagerCallReport dispatch_frame(
        const StreamingGameplaySession& session,
        double elapsed_seconds
    );

    [[nodiscard]] std::vector<LuaManagerDiagnostic> diagnostics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pulseforge
