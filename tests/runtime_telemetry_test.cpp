#include "pulseforge/runtime_telemetry.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void test_full_gameplay_payload_uses_authoritative_counter() {
    // PULSEFORGE_P1_5_0F_DISCORD_PAYLOAD_REGRESSION_V1
    pulseforge::RuntimeTelemetrySnapshot snapshot;
    snapshot.activity = pulseforge::RuntimeActivityKind::gameplay;
    snapshot.chart_title = "Foolhardy";
    snapshot.difficulty = "Hard";
    snapshot.key_count = 7U;
    snapshot.song_position_ms = 60'000.0;
    snapshot.duration_ms = 240'000.0;
    snapshot.playback_rate = 1.0;
    snapshot.chart_total = 12'483U;
    snapshot.logical_note_total = 19'802U;
    snapshot.successful_hits = 12'476U;
    snapshot.misses = 7U;
    snapshot.combo = 842U;
    snapshot.max_combo = 1'842U;
    snapshot.score = 247'830;
    snapshot.accuracy_percent = 99.84;
    snapshot.health = 0.95;

    pulseforge::DiscordPresenceSettings settings;
    const auto payload = pulseforge::build_discord_presence_payload(
        snapshot,
        settings,
        1'700'000'000'000ULL
    );
    require(payload.details.find("Foolhardy") != std::string::npos, "full privacy keeps chart name");
    require(payload.details.find("Hard") != std::string::npos, "full privacy keeps difficulty");
    require(payload.details.find("7K") != std::string::npos, "full privacy keeps mania");
    require(payload.state.find("12,483") == std::string::npos, "formatter is locale-independent");
    require(payload.state.find("12483 / 19802 notes") != std::string::npos, "payload reads the supplied authoritative Chart Total");
    require(payload.state.find("7 misses") != std::string::npos, "payload carries miss stats");
    require(payload.state.find("842x combo") != std::string::npos, "full detail exposes current combo");
    require(payload.state.find("247830 pts") != std::string::npos, "full detail exposes score");
    require(payload.end_unix_ms.has_value(), "active gameplay uses an end timestamp");
    require(*payload.end_unix_ms == 1'700'000'180'000ULL, "end timestamp accounts for remaining song duration");
}

void test_botplay_and_privacy() {
    pulseforge::RuntimeTelemetrySnapshot snapshot;
    snapshot.activity = pulseforge::RuntimeActivityKind::gameplay;
    snapshot.chart_title = "Private Chart Name";
    snapshot.difficulty = "Expert";
    snapshot.key_count = 9U;
    snapshot.song_position_ms = 63'000.0;
    snapshot.duration_ms = 100'000.0;
    snapshot.chart_total = 50U;
    snapshot.logical_note_total = 100U;
    snapshot.misses = 2U;
    snapshot.accuracy_percent = 98.2;
    snapshot.botplay = true;

    pulseforge::DiscordPresenceSettings full;
    auto payload = pulseforge::build_discord_presence_payload(snapshot, full, 1'000U);
    require(payload.details.find("BOTPLAY") != std::string::npos, "full detail exposes BOTPLAY in compact details");
    require(payload.small_image_key.empty(), "built-in RPC assets stay empty unless explicitly configured");

    pulseforge::DiscordPresenceSettings reduced;
    reduced.privacy = pulseforge::DiscordPresencePrivacy::reduced;
    payload = pulseforge::build_discord_presence_payload(snapshot, reduced, 1'000U);
    require(payload.details.find("Private Chart Name") == std::string::npos, "reduced privacy hides chart name");
    require(payload.state.find("BOTPLAY") == std::string::npos, "reduced privacy hides BOTPLAY");
    require(payload.state.find("2 misses") == std::string::npos, "reduced privacy hides stats");

    pulseforge::DiscordPresenceSettings minimal;
    minimal.privacy = pulseforge::DiscordPresencePrivacy::minimal;
    payload = pulseforge::build_discord_presence_payload(snapshot, minimal, 1'000U);
    require(payload.details == "Playing PulseForge", "minimal privacy exposes only engine context");
    require(payload.state == "In Gameplay", "minimal privacy exposes only coarse activity");
    require(!payload.end_unix_ms.has_value(), "minimal privacy hides remaining-time timestamp");
}

void test_result_payload_and_long_names_are_bounded() {
    pulseforge::RuntimeTelemetrySnapshot snapshot;
    snapshot.activity = pulseforge::RuntimeActivityKind::results;
    snapshot.chart_title.assign(500U, 'x');
    snapshot.difficulty = "Hard";
    snapshot.completed = true;
    snapshot.accuracy_percent = 100.0;

    const auto payload = pulseforge::build_discord_presence_payload(
        snapshot,
        {},
        std::numeric_limits<std::uint64_t>::max() - 10U
    );
    require(payload.details.size() <= 128U, "Discord details obey the documented 128-byte budget");
    require(payload.state.size() <= 128U, "Discord state obeys the documented 128-byte budget");
    snapshot.chart_title = std::string(126U, 'x') + "\xE2\x98\x85" + "tail";
    const auto utf8_payload = pulseforge::build_discord_presence_payload(snapshot, {}, 0U);
    require(
        utf8_payload.details.find("\xE2\x98") == std::string::npos,
        "Discord byte truncation never leaves a partial UTF-8 code point"
    );
    require(payload.small_image_key.empty(), "completed result uses the application icon fallback by default");
    require(!payload.end_unix_ms.has_value(), "completed result has no countdown");
}

void test_render_and_autochart_payloads() {
    pulseforge::RuntimeTelemetrySnapshot render;
    render.activity = pulseforge::RuntimeActivityKind::rendering;
    render.chart_title = "Foolhardy";
    render.render_width = 3840U;
    render.render_height = 2160U;
    render.render_fps = 240U;
    render.render_frame = 24'820U;
    render.render_frame_count = 39'600U;
    auto payload = pulseforge::build_discord_presence_payload(render, {}, 0U);
    require(payload.details.find("Foolhardy") != std::string::npos, "render payload keeps chart title at full privacy");
    require(payload.state.find("3840x2160") != std::string::npos, "render payload exposes resolution");
    require(payload.state.find("240 FPS") != std::string::npos, "render payload exposes target FPS");
    require(payload.small_image_key.empty(), "render payload uses the application icon fallback by default");

    pulseforge::RuntimeTelemetrySnapshot autochart;
    autochart.activity = pulseforge::RuntimeActivityKind::autochart;
    autochart.status = "ML + DSP";
    autochart.task_progress = 0.73;
    payload = pulseforge::build_discord_presence_payload(autochart, {}, 0U);
    require(payload.details == "Generating a chart", "AutoChart uses dedicated details");
    require(payload.state.find("73%") != std::string::npos, "AutoChart exposes coarse progress");
    require(payload.small_image_key.empty(), "AutoChart uses the application icon fallback by default");
}

void test_engine_context_mapping_and_oauth_redirects() {
    struct ExpectedContext final {
        pulseforge::RuntimeActivityKind kind;
        const char* details;
        const char* state;
        const char* asset;
    };
    constexpr ExpectedContext contexts[]{
        {pulseforge::RuntimeActivityKind::launcher, "Main Menu", "Exploring PulseForge", "launcher"},
        {pulseforge::RuntimeActivityKind::loading, "Loading a chart", "Preparing the chart", "loading"},
        {pulseforge::RuntimeActivityKind::story, "Story Mode", "Choosing a week", "story"},
        {pulseforge::RuntimeActivityKind::freeplay, "Freeplay", "Choosing a chart", "freeplay"},
        {pulseforge::RuntimeActivityKind::mods, "Mods", "Managing installed mods", "mods"},
        {pulseforge::RuntimeActivityKind::options, "Options", "Configuring the engine", "options"},
    };
    for (const auto& expected : contexts) {
        pulseforge::RuntimeTelemetrySnapshot snapshot;
        snapshot.activity = expected.kind;
        const auto payload = pulseforge::build_discord_presence_payload(snapshot, {}, 0U);
        require(payload.details == expected.details, "engine context maps to its real Discord details");
        require(payload.state == expected.state, "engine context maps to its real Discord state");
        require(payload.small_image_key.empty(), "engine context leaves optional Discord assets unset by default");
    }

    require(
        pulseforge::discord_oauth_redirect_uri_valid(""),
        "an empty Discord redirect selects the SDK platform default"
    );
    require(
        pulseforge::discord_oauth_redirect_uri_compatible(
            "http://127.0.0.1/callback",
            false
        ),
        "the documented desktop loopback callback is accepted"
    );
    require(
        pulseforge::discord_oauth_redirect_uri_compatible(
            "pulseforge:/authorize/callback",
            true
        ),
        "a registered mobile application scheme is accepted"
    );
    require(
        !pulseforge::discord_oauth_redirect_uri_valid(
            "https://attacker.example/callback"
        ),
        "arbitrary remote OAuth callbacks are rejected"
    );
    require(
        !pulseforge::discord_oauth_redirect_uri_valid(
            "pulseforge:/authorize/callback?code=leak"
        ),
        "OAuth redirects reject query/fragment injection"
    );
    require(
        !pulseforge::discord_oauth_redirect_uri_compatible(
            "pulseforge:/authorize/callback",
            false
        ),
        "a mobile custom scheme is never used as a desktop callback"
    );
}

void test_discord_publish_change_policy() {
    // PULSEFORGE_P1_5_0G_DISCORD_TRANSITION_REGRESSION_V1
    pulseforge::RuntimeTelemetrySnapshot launcher;
    launcher.activity = pulseforge::RuntimeActivityKind::launcher;
    launcher.status = "Browsing charts";

    auto gameplay = launcher;
    gameplay.activity = pulseforge::RuntimeActivityKind::gameplay;
    gameplay.status = "Materialized";
    gameplay.chart_title = "Stress Chart";
    gameplay.difficulty = "Expert";
    gameplay.key_count = 7U;

    require(
        pulseforge::discord_presence_structural_change(launcher, gameplay),
        "launcher to gameplay is an immediate structural presence transition"
    );

    auto stat_only = gameplay;
    stat_only.chart_total = 5'000U;
    stat_only.successful_hits = 4'999U;
    stat_only.combo = 1'024U;
    stat_only.score = 8'000'000;
    stat_only.song_position_ms = 90'000.0;
    require(
        !pulseforge::discord_presence_structural_change(gameplay, stat_only),
        "high-frequency gameplay counters stay coalescible"
    );

    auto paused = stat_only;
    paused.activity = pulseforge::RuntimeActivityKind::paused;
    paused.paused = true;
    require(
        pulseforge::discord_presence_structural_change(stat_only, paused),
        "gameplay to pause publishes immediately"
    );

    auto resumed = paused;
    resumed.activity = pulseforge::RuntimeActivityKind::gameplay;
    resumed.paused = false;
    require(
        pulseforge::discord_presence_structural_change(paused, resumed),
        "pause to gameplay publishes immediately"
    );

    auto results = resumed;
    results.activity = pulseforge::RuntimeActivityKind::results;
    results.completed = true;
    require(
        pulseforge::discord_presence_structural_change(resumed, results),
        "gameplay to results publishes immediately"
    );

    auto back_to_launcher = launcher;
    require(
        pulseforge::discord_presence_structural_change(results, back_to_launcher),
        "results to launcher publishes immediately"
    );

    auto progress_only = launcher;
    progress_only.status = "Opening Freeplay";
    progress_only.task_progress = 0.75;
    require(
        !pulseforge::discord_presence_structural_change(launcher, progress_only),
        "status/progress-only updates remain coalescible instead of spamming the SDK"
    );

    auto mod_changed = gameplay;
    mod_changed.mod_name = "Real Mod Corpus";
    require(
        pulseforge::discord_presence_structural_change(gameplay, mod_changed),
        "mod identity changes are structural"
    );

    auto render_changed = gameplay;
    render_changed.activity = pulseforge::RuntimeActivityKind::rendering;
    render_changed.render_width = 3'840U;
    render_changed.render_height = 2'160U;
    render_changed.render_fps = 240U;
    require(
        pulseforge::discord_presence_structural_change(gameplay, render_changed),
        "render configuration transition is structural"
    );

    pulseforge::DiscordPresenceSettings settings;
    settings.application_id = 111U;

    auto new_app_id = settings;
    new_app_id.application_id = 222U;
    require(
        pulseforge::discord_presence_configuration_change(settings, new_app_id),
        "Discord Application ID changes force a backend refresh even for identical payloads"
    );

    auto private_mode = settings;
    private_mode.privacy = pulseforge::DiscordPresencePrivacy::minimal;
    require(
        pulseforge::discord_presence_configuration_change(settings, private_mode),
        "privacy changes publish immediately"
    );

    auto hide_stats = settings;
    hide_stats.show_gameplay_stats = false;
    require(
        pulseforge::discord_presence_configuration_change(settings, hide_stats),
        "visible-field toggles publish immediately"
    );

    auto interval_only = settings;
    interval_only.publish_interval_ms = 5'000U;
    require(
        !pulseforge::discord_presence_configuration_change(settings, interval_only),
        "publish interval alone changes scheduling, not backend identity or visible payload"
    );

    auto retry_policy = settings;
    retry_policy.retry_failed_updates = false;
    require(
        pulseforge::discord_presence_configuration_change(settings, retry_policy),
        "retry policy changes invalidate a suppressed failed publication immediately"
    );

    require(
        pulseforge::discord_presence_should_publish(
            true,
            true,
            false,
            true,
            false
        ),
        "identical payload plus Application ID/configuration change still republishes"
    );
    require(
        !pulseforge::discord_presence_should_publish(
            true,
            true,
            false,
            false,
            true
        ),
        "identical payload with unchanged configuration performs no SDK call"
    );
    require(
        !pulseforge::discord_presence_should_publish(
            true,
            false,
            false,
            false,
            false
        ),
        "stat-only payload changes remain coalesced inside the interval"
    );
    require(
        pulseforge::discord_presence_should_publish(
            true,
            false,
            true,
            false,
            false
        ),
        "structural transitions bypass the publish interval"
    );
}

void test_metrolist_style_advanced_presence() {
    // PULSEFORGE_P1_5_0G1_METROLIST_STYLE_RPC_REGRESSION_V1
    pulseforge::RuntimeTelemetrySnapshot snapshot;
    snapshot.activity = pulseforge::RuntimeActivityKind::gameplay;
    snapshot.status = "Streaming";
    snapshot.chart_title = "Secret Song";
    snapshot.difficulty = "Expert";
    snapshot.mod_name = "Corpus Mod";
    snapshot.media_title = "Launcher Theme";
    snapshot.media_artist = "PulseForge OST";
    snapshot.media_url = "https://example.test/media";
    snapshot.media_art_url = "https://example.test/art.png";
    snapshot.key_count = 7U;
    snapshot.song_position_ms = 25'000.0;
    snapshot.duration_ms = 100'000.0;
    snapshot.chart_total = 1'234U;
    snapshot.logical_note_total = 4'000U;
    snapshot.combo = 321U;
    snapshot.max_combo = 777U;
    snapshot.score = 987'654;
    snapshot.misses = 3U;
    snapshot.accuracy_percent = 99.12;
    snapshot.botplay = true;

    pulseforge::DiscordPresenceSettings settings;
    settings.advanced_customization = true;
    settings.activity_name_template = "{engine} / {activity}";
    settings.details_template = "{chart.name} • {difficulty} • {mania} • {progress}";
    settings.state_template = "{accuracy} • {misses} misses • {combo} combo • {media.title}";
    settings.details_url_template = "https://example.test/charts/{notes.total}";
    settings.large_image_template = "{media.art}";
    settings.large_text_template = "{media.artist}";
    settings.large_url_template = "{media.url}";
    settings.button1 = {true, "Open {chart.name}", "https://example.test/play/{notes.resolved}"};
    settings.button2 = {true, "Invalid", "javascript:alert(1)"};

    const auto rendered = pulseforge::render_discord_presence_template(
        "{chart.name}|{difficulty}|{media.title}|{notes.resolved}",
        snapshot,
        pulseforge::DiscordPresencePrivacy::full
    );
    require(
        rendered == "Secret Song|Expert|Launcher Theme|1234",
        "advanced template renderer substitutes PulseForge gameplay/media fields"
    );

    auto payload = pulseforge::build_discord_presence_payload(snapshot, settings, 0U);
    require(payload.activity_name == "PulseForge / gameplay", "advanced mode renders a custom activity name");
    require(payload.details.find("Secret Song") != std::string::npos, "advanced details expose full-privacy chart metadata");
    require(payload.state.find("Launcher Theme") != std::string::npos, "advanced state can expose media metadata");
    require(payload.details_url == "https://example.test/charts/4000", "advanced details URL is rendered and validated");
    require(payload.large_image_key == "https://example.test/art.png", "external HTTPS artwork can be passed directly as the Discord image field");
    require(payload.large_image_url == "https://example.test/media", "large-image click URL is retained");
    require(payload.button_count == 1U, "only valid HTTP(S) custom buttons are retained");
    require(payload.buttons[0].label.find("Secret Song") != std::string::npos, "button labels support templates");

    auto reduced = settings;
    reduced.privacy = pulseforge::DiscordPresencePrivacy::reduced;
    payload = pulseforge::build_discord_presence_payload(snapshot, reduced, 0U);
    require(payload.details.find("Secret Song") == std::string::npos, "reduced privacy strips sensitive chart placeholders from advanced templates");
    require(payload.state.find("Launcher Theme") == std::string::npos, "reduced privacy strips media identity from advanced templates");
    require(payload.large_image_key != "https://example.test/art.png", "reduced privacy does not expose media artwork placeholders");
    require(payload.button_count == 1U, "safe static/template buttons remain available in reduced privacy when their sensitive placeholders render empty");

    auto minimal = settings;
    minimal.privacy = pulseforge::DiscordPresencePrivacy::minimal;
    payload = pulseforge::build_discord_presence_payload(snapshot, minimal, 0U);
    require(payload.activity_name == "PulseForge", "minimal privacy ignores custom activity-name templates");
    require(payload.details == "Playing PulseForge", "minimal privacy ignores advanced details templates");
    require(payload.button_count == 0U, "minimal privacy suppresses advanced custom buttons");

    auto changed = settings;
    changed.details_template = "{chart.name}";
    require(
        pulseforge::discord_presence_configuration_change(settings, changed),
        "template changes invalidate Discord payload deduplication"
    );
    auto media_changed = snapshot;
    media_changed.media_title = "Another Theme";
    require(
        pulseforge::discord_presence_structural_change(snapshot, media_changed),
        "media identity changes are structural for template-driven presence"
    );

    require(pulseforge::discord_presence_retry_delay_ms(0U) == 0U, "zero failures have no retry delay");
    require(pulseforge::discord_presence_retry_delay_ms(1U) == 1'000U, "first retry uses one-second backoff");
    require(pulseforge::discord_presence_retry_delay_ms(2U) == 2'000U, "second retry doubles the backoff");
    require(pulseforge::discord_presence_retry_delay_ms(6U) == 30'000U, "retry backoff is bounded at thirty seconds");
    require(
        pulseforge::discord_presence_retry_delay_ms(2U, 45'000U) == 45'000U,
        "server Retry-After overrides the smaller local backoff"
    );
    require(
        pulseforge::discord_presence_retry_delay_ms(9U, 120'000U) == 60'000U,
        "server Retry-After is bounded so failures cannot schedule unbounded sleeps"
    );
}

void test_bounded_performance_accumulator() {
    // PULSEFORGE_P1_5_0F_BOUNDED_PERFORMANCE_ACCUMULATOR_TEST_V1
    pulseforge::RuntimePerformanceAccumulator accumulator;
    for (std::size_t index = 1U; index <= 10'000U; ++index) {
        accumulator.record_frame_ms(static_cast<double>(index % 100U));
        if ((index % 4U) == 0U) accumulator.record_input_age_ms(0.25 * static_cast<double>(index % 40U));
    }
    const auto report = accumulator.report();
    require(report.frame_samples == 10'000U, "total frame sample count remains monotonic beyond ring capacity");
    require(report.input_samples == 2'500U, "input sample count remains monotonic");
    require(report.frame_p50_ms >= 0.0 && report.frame_p50_ms <= report.frame_p95_ms, "frame percentiles are ordered");
    require(report.frame_p95_ms <= report.frame_p99_ms, "high frame percentiles are ordered");
    require(report.frame_p99_ms <= report.max_frame_ms, "max frame time bounds p99");
    require(report.average_frame_ms >= 0.0, "average frame time remains finite");

    accumulator.record_frame_ms(-1.0);
    accumulator.record_frame_ms(std::numeric_limits<double>::quiet_NaN());
    require(accumulator.report().frame_samples == 10'000U, "invalid samples are ignored");
    accumulator.reset();
    require(accumulator.report().frame_samples == 0U, "reset clears bounded telemetry state");
}

}  // namespace

int main() {
    try {
        test_full_gameplay_payload_uses_authoritative_counter();
        test_botplay_and_privacy();
        test_result_payload_and_long_names_are_bounded();
        test_render_and_autochart_payloads();
        test_engine_context_mapping_and_oauth_redirects();
        test_discord_publish_change_policy();
        test_metrolist_style_advanced_presence();
        test_bounded_performance_accumulator();
        std::cout << "8/8 runtime telemetry tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
