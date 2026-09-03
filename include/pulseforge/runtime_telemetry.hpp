#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace pulseforge {

// PULSEFORGE_P1_5_0F_RUNTIME_TELEMETRY_API_V1
// A compact, backend-neutral snapshot shared by Rich Presence, diagnostics,
// benchmarks and future overlays. It intentionally stores no per-note state.
enum class RuntimeActivityKind : std::uint8_t {
    launcher,
    loading,
    story,
    freeplay,
    editor,
    mods,
    rendering,
    options,
    gameplay,
    paused,
    results,
    autochart,
};

enum class DiscordPresencePrivacy : std::uint8_t {
    full,
    reduced,
    minimal,
};

struct DiscordPresenceButtonSettings {
    bool enabled{};
    std::string label;
    std::string url;

    [[nodiscard]] bool operator==(
        const DiscordPresenceButtonSettings&
    ) const = default;
};

struct DiscordPresenceSettings {
    bool enabled{true};
    std::uint64_t application_id{};
    // Empty selects the Discord Social SDK platform default. Desktop supports
    // the documented loopback redirect; Android supports an application URI
    // scheme. OAuth credentials are deliberately never stored in settings.
    std::string oauth_redirect_uri;
    DiscordPresencePrivacy privacy{DiscordPresencePrivacy::full};
    bool show_chart_name{true};
    bool show_difficulty_mania{true};
    bool show_progress{true};
    bool show_note_counter{true};
    bool show_gameplay_stats{true};
    bool show_botplay{true};
    bool show_remaining_time{true};
    bool show_mod_name{false};

    // PULSEFORGE_P1_5_0G1_METROLIST_STYLE_RPC_CUSTOMIZATION_V1
    // Inspired by Metrolist's template-driven activity builder, but implemented
    // directly against Discord's desktop Social SDK. Minimal privacy always
    // ignores templates; reduced privacy renders sensitive placeholders empty.
    bool advanced_customization{};
    std::string activity_name_template{"PulseForge"};
    std::string details_template;
    std::string state_template;
    std::string details_url_template;
    std::string state_url_template;
    std::string large_image_template;
    std::string large_text_template;
    std::string large_url_template;
    std::string small_image_template;
    std::string small_text_template;
    std::string small_url_template;
    DiscordPresenceButtonSettings button1;
    DiscordPresenceButtonSettings button2;

    // Failed SDK sends are retried with bounded exponential backoff. This keeps
    // Discord outages/rate limits completely off the gameplay hot path.
    bool retry_failed_updates{true};

    // Visible-stat changes are coalesced to this interval. Structural state
    // transitions bypass it; identical payloads make zero SDK calls.
    std::uint32_t publish_interval_ms{3'000U};
};

struct RuntimeTelemetrySnapshot {
    RuntimeActivityKind activity{RuntimeActivityKind::launcher};
    std::string chart_title;
    std::string difficulty;
    std::string mod_name;
    std::string status;
    // Optional media metadata shared by menu music, future jukebox providers,
    // and Rich Presence templates. It never changes gameplay scoring/state.
    std::string media_title;
    std::string media_artist;
    std::string media_album;
    std::string media_url;
    std::string media_art_url;
    std::uint16_t key_count{};
    double song_position_ms{};
    double duration_ms{};
    double playback_rate{1.0};
    double task_progress{}; // 0..1 when a non-gameplay job reports progress.

    // chart_total is the authoritative number of resolved logical heads from
    // ScoreSummary. logical_note_total is the bounded/arithmetic chart source
    // total (including PatternRun without expansion) when known.
    std::uint64_t chart_total{};
    std::uint64_t logical_note_total{};
    std::uint64_t successful_hits{};
    std::uint64_t misses{};
    std::uint64_t combo{};
    std::uint64_t max_combo{};
    std::int64_t score{};
    double accuracy_percent{};
    double health{1.0};

    double player_note_multiplier{1.0};
    double opponent_note_multiplier{1.0};

    std::uint64_t render_frame{};
    std::uint64_t render_frame_count{};
    std::uint32_t render_width{};
    std::uint32_t render_height{};
    std::uint32_t render_fps{};

    bool botplay{};
    bool practice{};
    bool paused{};
    bool failed{};
    bool completed{};
    bool streaming{};
    bool third_strum{};
};

struct RuntimePerformanceReport {
    double average_frame_ms{};
    double frame_p50_ms{};
    double frame_p95_ms{};
    double frame_p99_ms{};
    double max_frame_ms{};
    double average_input_age_ms{};
    double input_age_p95_ms{};
    std::uint64_t frame_samples{};
    std::uint64_t input_samples{};
};

class RuntimePerformanceAccumulator final {
public:
    // Fixed-size rings keep multi-hour soak sessions bounded.
    static constexpr std::size_t capacity = 4'096U;

    void record_frame_ms(double value) noexcept;
    void record_input_age_ms(double value) noexcept;
    void reset() noexcept;
    [[nodiscard]] RuntimePerformanceReport report() const noexcept;

private:
    std::array<double, capacity> frame_ms_{};
    std::array<double, capacity> input_age_ms_{};
    std::size_t frame_cursor_{};
    std::size_t frame_size_{};
    std::size_t input_cursor_{};
    std::size_t input_size_{};
    long double frame_sum_{};
    long double input_sum_{};
    std::uint64_t frame_samples_total_{};
    std::uint64_t input_samples_total_{};
};

struct DiscordPresenceButton {
    std::string label;
    std::string url;

    [[nodiscard]] bool operator==(const DiscordPresenceButton&) const = default;
};

struct DiscordPresencePayload {
    std::string activity_name{"PulseForge"};
    std::string details;
    std::string state;
    std::string details_url;
    std::string state_url;
    // Discord Social SDK accepts either uploaded asset keys or external URLs in
    // these image fields, so no Android-style external-asset resolver is needed.
    // No implicit asset key: uploaded Rich Presence assets are application-specific.
    // Advanced customization may populate these fields explicitly.
    std::string large_image_key;
    std::string large_image_text;
    std::string large_image_url;
    std::string small_image_key;
    std::string small_image_text;
    std::string small_image_url;
    std::array<DiscordPresenceButton, 2U> buttons{};
    std::size_t button_count{};
    std::optional<std::uint64_t> start_unix_ms;
    std::optional<std::uint64_t> end_unix_ms;

    [[nodiscard]] bool operator==(const DiscordPresencePayload&) const = default;
};

[[nodiscard]] std::string_view runtime_activity_name(RuntimeActivityKind kind) noexcept;
[[nodiscard]] std::string_view discord_privacy_name(DiscordPresencePrivacy privacy) noexcept;
// Validates the small, explicit redirect surface supported by the official
// Social SDK: its desktop loopback callback or a mobile application scheme.
// Empty is valid and means "use the SDK default".
[[nodiscard]] bool discord_oauth_redirect_uri_valid(std::string_view uri) noexcept;
[[nodiscard]] bool discord_oauth_redirect_uri_compatible(
    std::string_view uri,
    bool mobile_platform
) noexcept;

// PULSEFORGE_P1_5_0G_DISCORD_STRUCTURAL_CHANGE_POLICY_V1
// These helpers keep publish scheduling independent from the Discord SDK so
// launcher/gameplay transitions and live settings changes are regression-testable.
[[nodiscard]] bool discord_presence_structural_change(
    const RuntimeTelemetrySnapshot& previous,
    const RuntimeTelemetrySnapshot& current
) noexcept;
[[nodiscard]] bool discord_presence_configuration_change(
    const DiscordPresenceSettings& previous,
    const DiscordPresenceSettings& current
) noexcept;
[[nodiscard]] bool discord_presence_should_publish(
    bool currently_published,
    bool payload_equal,
    bool structural_change,
    bool configuration_change,
    bool publish_interval_elapsed
) noexcept;

// Pure helpers used by tests and the SDK backend. Template syntax intentionally
// mirrors Metrolist's simple {field.name} model while exposing PulseForge data.
[[nodiscard]] std::string render_discord_presence_template(
    std::string_view text,
    const RuntimeTelemetrySnapshot& snapshot,
    DiscordPresencePrivacy privacy
);
[[nodiscard]] std::uint32_t discord_presence_retry_delay_ms(
    std::uint32_t consecutive_failures,
    std::uint32_t server_retry_after_ms = 0U
) noexcept;

[[nodiscard]] DiscordPresencePayload build_discord_presence_payload(
    const RuntimeTelemetrySnapshot& snapshot,
    const DiscordPresenceSettings& settings,
    std::uint64_t now_unix_ms
);

}  // namespace pulseforge
