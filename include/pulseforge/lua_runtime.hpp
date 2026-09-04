#pragma once

#include "pulseforge/gameplay.hpp"
#include "pulseforge/streaming_gameplay.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace pulseforge {

// Deliberately limited to scalar values. Tables, functions, userdata and native
// pointers never cross the Lua/engine boundary.
using ScriptValue =
    std::variant<std::monostate, bool, std::int64_t, double, std::string>;

struct ScriptEventRequest {
    std::string name;
    std::string value1;
    std::string value2;
};

// Safe host surface used by LuaRuntime. Implementations are expected to
// whitelist property names rather than reflect arbitrary engine objects.
class LuaHostInterface {
public:
    virtual ~LuaHostInterface() = default;

    [[nodiscard]] virtual double song_position_ms() const noexcept = 0;
    [[nodiscard]] virtual std::int64_t current_beat() const noexcept = 0;
    [[nodiscard]] virtual std::int64_t current_step() const noexcept = 0;
    [[nodiscard]] virtual std::int64_t current_section() const noexcept = 0;

    [[nodiscard]] virtual bool get_property(
        std::string_view name,
        ScriptValue& value,
        std::string& error
    ) const = 0;

    [[nodiscard]] virtual bool set_property(
        std::string_view name,
        const ScriptValue& value,
        std::string& error
    ) = 0;

    [[nodiscard]] virtual bool add_score(
        std::int64_t amount,
        std::string& error
    ) = 0;

    [[nodiscard]] virtual bool set_health(
        double health,
        std::string& error
    ) = 0;

    [[nodiscard]] virtual bool trigger_event(
        ScriptEventRequest event,
        std::string& error
    ) = 0;

    virtual void debug_print(std::string_view message) = 0;

    // Extensible Psych-style compatibility surface.  The Lua runtime keeps
    // arguments/results scalar and bounded; application hosts may implement
    // visual/group commands without exposing engine pointers or reflection.
    [[nodiscard]] virtual bool invoke_function(
        std::string_view name,
        std::span<const ScriptValue> arguments,
        ScriptValue& result,
        std::string& error
    ) {
        result = std::monostate{};
        error = "host function is not supported: ";
        error.append(name);
        (void)arguments;
        return false;
    }
};

// Output that must be consumed by application code at a deterministic frame
// boundary. Score and health are applied through GameplaySession's public
// mutators; events and logs stay queued here to avoid re-entrant engine calls.
struct GameplayScriptState {
    std::vector<ScriptEventRequest> pending_events;
    std::vector<std::string> debug_messages;
    std::size_t max_pending_events{256};
    std::size_t max_debug_messages{256};

    void reset() noexcept;
    void clear_transient_output() noexcept;

    [[nodiscard]] std::int64_t effective_score(
        const GameplaySession& session
    ) const noexcept;

    [[nodiscard]] double effective_health(
        const GameplaySession& session
    ) const noexcept;
};

// Whitelisted GameplaySession adapter. It is an engine-side object only; its
// address is retained solely as C allocator context and is never pushed to Lua
// as lightuserdata or userdata.
class GameplayLuaHost final : public LuaHostInterface {
public:
    GameplayLuaHost(
        GameplaySession& session,
        GameplayScriptState& state
    ) noexcept;

    [[nodiscard]] double song_position_ms() const noexcept override;
    [[nodiscard]] std::int64_t current_beat() const noexcept override;
    [[nodiscard]] std::int64_t current_step() const noexcept override;
    [[nodiscard]] std::int64_t current_section() const noexcept override;

    [[nodiscard]] bool get_property(
        std::string_view name,
        ScriptValue& value,
        std::string& error
    ) const override;

    [[nodiscard]] bool set_property(
        std::string_view name,
        const ScriptValue& value,
        std::string& error
    ) override;

    [[nodiscard]] bool add_score(
        std::int64_t amount,
        std::string& error
    ) override;

    [[nodiscard]] bool set_health(
        double health,
        std::string& error
    ) override;

    [[nodiscard]] bool trigger_event(
        ScriptEventRequest event,
        std::string& error
    ) override;

    void debug_print(std::string_view message) override;

private:
    GameplaySession& session_;
    GameplayScriptState& state_;
};

struct LuaRuntimeConfig {
    std::size_t memory_limit_bytes{16U * 1024U * 1024U};
    std::uint64_t deterministic_seed{0x50554C5345464F52ULL};
    std::uint64_t instruction_budget{1'000'000};
    std::uint64_t frame_instruction_budget{8'000'000};
    std::uint32_t instruction_hook_interval{1'000};
    std::size_t max_callbacks_per_frame{4'096};
    std::size_t max_script_bytes{2U * 1024U * 1024U};
    std::size_t max_host_string_bytes{4U * 1024U};
    std::size_t max_diagnostics{64};
    bool disable_callback_after_error{true};
};

enum class LuaDiagnosticKind : std::uint8_t {
    load_error,
    runtime_error,
    memory_limit,
    instruction_limit,
    host_error,
};

struct LuaDiagnostic {
    LuaDiagnosticKind kind{LuaDiagnosticKind::runtime_error};
    std::string callback;
    std::string message;
};

enum class LuaCallStatus : std::uint8_t {
    not_loaded,
    missing,
    disabled,
    completed,
    stop_requested,
    failed,
};

struct LuaCallResult {
    LuaCallStatus status{LuaCallStatus::not_loaded};

    [[nodiscard]] bool called() const noexcept {
        return status == LuaCallStatus::completed
            || status == LuaCallStatus::stop_requested
            || status == LuaCallStatus::failed;
    }

    [[nodiscard]] bool succeeded() const noexcept {
        return status == LuaCallStatus::completed
            || status == LuaCallStatus::stop_requested;
    }
};

struct LuaDispatchReport {
    std::size_t completed{};
    std::size_t stop_requests{};
    std::size_t failed{};
    std::size_t skipped{};
};

struct LuaRuntimeStats {
    std::size_t current_memory_bytes{};
    std::size_t peak_memory_bytes{};
    std::uint64_t last_instruction_count{};
    std::uint64_t total_instruction_count{};
    std::uint64_t successful_callbacks{};
    std::uint64_t failed_callbacks{};
};

class LuaRuntime {
public:
    // LuaRuntime is single-threaded. The host must remain alive, and all
    // methods must be called from the same gameplay thread, until destruction.
    explicit LuaRuntime(
        LuaHostInterface& host,
        LuaRuntimeConfig config = {}
    );
    ~LuaRuntime();

    LuaRuntime(LuaRuntime&& other) noexcept;
    LuaRuntime& operator=(LuaRuntime&& other) noexcept;

    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;

    // Replaces any previous script. Only textual Lua chunks are accepted.
    [[nodiscard]] bool load_script(
        std::string_view source,
        std::string_view source_name = "@mod.lua"
    );

    // Calls onDestroy once, if appropriate, and releases all Lua allocations.
    void unload();

    [[nodiscard]] bool loaded() const noexcept;

    [[nodiscard]] LuaCallResult on_create();
    [[nodiscard]] LuaCallResult on_create_post();
    // PULSEFORGE_P1_1_4_ONSTARTCOUNTDOWN_V1
    [[nodiscard]] LuaCallResult on_start_countdown();
    [[nodiscard]] LuaCallResult on_song_start();
    [[nodiscard]] LuaCallResult on_end_song();
    [[nodiscard]] LuaCallResult on_game_over();
    [[nodiscard]] LuaCallResult on_pause();
    [[nodiscard]] LuaCallResult on_resume();
    [[nodiscard]] LuaCallResult on_update(double elapsed_seconds);
    [[nodiscard]] LuaCallResult on_update_post(double elapsed_seconds);
    [[nodiscard]] LuaCallResult on_beat_hit(std::int64_t beat);
    [[nodiscard]] LuaCallResult on_step_hit(std::int64_t step);
    [[nodiscard]] LuaCallResult on_section_hit(std::int64_t section);

    [[nodiscard]] LuaCallResult good_note_hit(
        std::size_t note_index,
        std::uint16_t direction,
        std::string_view note_type,
        bool is_sustain_note
    );

    [[nodiscard]] LuaCallResult opponent_note_hit(
        std::size_t note_index,
        std::uint16_t direction,
        std::string_view note_type,
        bool is_sustain_note
    );

    [[nodiscard]] LuaCallResult note_miss(
        std::size_t note_index,
        std::uint16_t direction,
        std::string_view note_type,
        bool is_sustain_note
    );

    [[nodiscard]] LuaCallResult note_miss_press(std::uint16_t direction);
    [[nodiscard]] LuaCallResult on_ghost_tap(std::uint16_t direction);

    [[nodiscard]] LuaCallResult on_event(
        std::string_view name,
        std::string_view value1,
        std::string_view value2
    );

    [[nodiscard]] LuaCallResult on_tween_completed(
        std::string_view tag,
        std::string_view target = {}
    );
    [[nodiscard]] LuaCallResult on_timer_completed(
        std::string_view tag,
        std::int64_t loops,
        std::int64_t loops_left
    );
    // PULSEFORGE_P1_1_18_SOUND_COMPLETION_CALLBACK_API_V1
    [[nodiscard]] LuaCallResult on_sound_finished(std::string_view tag);

    [[nodiscard]] LuaCallResult on_destroy();

    // Maps GameplaySession frame events to the supported Psych-style
    // callbacks. onUpdate is issued first, onUpdatePost last, and event order
    // otherwise matches GameplaySession::frame_events().
    [[nodiscard]] LuaDispatchReport dispatch_frame(
        const GameplaySession& session,
        double elapsed_seconds
    );

    // PFC1/streaming equivalent.  Dense aggregated events intentionally map to
    // one bounded Lua callback rather than expanding occurrence_count.
    [[nodiscard]] LuaDispatchReport dispatch_frame(
        const StreamingGameplaySession& session,
        double elapsed_seconds
    );

    [[nodiscard]] std::span<const LuaDiagnostic> diagnostics() const noexcept;
    void clear_diagnostics() noexcept;

    [[nodiscard]] LuaRuntimeStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pulseforge
