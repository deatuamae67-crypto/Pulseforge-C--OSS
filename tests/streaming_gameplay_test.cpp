#include "pulseforge/gameplay.hpp"
#include "pulseforge/packed_chart.hpp"
#include "pulseforge/streaming_gameplay.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t trillion = 1'000'000'000'000ULL;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto nonce = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
        const auto root = std::filesystem::temp_directory_path();
        for (std::uint32_t attempt = 0U; attempt < 1'024U; ++attempt) {
            path_ = root / (
                "pulseforge-streaming-gameplay-test-"
                + std::to_string(nonce) + "-" + std::to_string(attempt)
            );
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                owned_ = true;
                return;
            }
            if (error) {
                throw std::runtime_error("cannot create streaming gameplay test directory");
            }
        }
        throw std::runtime_error("cannot reserve streaming gameplay test directory");
    }

    ~TemporaryDirectory() {
        if (owned_) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
    bool owned_{};
};

[[nodiscard]] pulseforge::PackedChartReader write_and_open(
    const std::filesystem::path& path,
    const pulseforge::PackedChartData& chart,
    const std::uint32_t notes_per_chunk = 64U
) {
    pulseforge::PackedChartWriteOptions options;
    options.max_notes_per_chunk = notes_per_chunk;
    std::string error;
    require(
        pulseforge::write_packed_chart(path, chart, options, &error),
        std::string("cannot write gameplay fixture: ") + error
    );
    auto reader = pulseforge::PackedChartReader::open(path, &error);
    require(
        reader.has_value(),
        std::string("cannot open gameplay fixture: ") + error
    );
    return std::move(*reader);
}

void test_real_input_holds_and_score(const std::filesystem::path& directory) {
    pulseforge::PackedChartData chart;
    chart.key_count = 4U;
    chart.kinds = {"normal", "mine"};
    chart.notes = {
        {100'000, 0U, 0U, pulseforge::PackedNoteOwner::player, 0U, 0U},
        {200'000, 500'000U, 1U, pulseforge::PackedNoteOwner::player, 0U, 0U},
        {250'000, 0U, 3U, pulseforge::PackedNoteOwner::opponent, 0U, 0U},
        {400'000, 0U, 2U, pulseforge::PackedNoteOwner::player, 0U, 1U},
    };
    const auto reader = write_and_open(directory / "input.pfc", chart, 2U);

    pulseforge::StreamingGameplayOptions options;
    options.max_window_notes = 16U;
    options.max_active_holds = 4U;
    options.max_events_per_frame = 32U;
    options.max_recorded_inputs = 16U;
    options.max_explicit_chunk_notes = 4U;
    std::string error;
    auto session = pulseforge::StreamingGameplaySession::create(
        reader,
        {},
        options,
        {{0.0, 120.0, 4U, 4U}},
        &error
    );
    require(session.has_value(), error);
    require(session->update(0.0), session->error());
    require(session->press(0U, 100.0), session->error());
    require(session->press(1U, 200.0), session->error());
    require(session->update(300.0), session->error());
    require(session->update(400.0), session->error());
    require(session->update(500.0), session->error());
    require(session->update(600.0), session->error());
    require(session->release(1U, 700.0), session->error());
    require(session->update(1'000.0), session->error());
    require(session->update(3'000.0), session->error());

    const auto& score = session->summary();
    require(score.score == 740, "tap, hold head and four sustain ticks score exactly");
    require(score.marvelous == 2U, "two player heads receive marvelous");
    require(score.hold_ticks == 4U, "tempo-aware sustain ticks are deterministic");
    require(score.hold_drops == 0U && score.misses == 0U, "clean hold has no miss");
    require(score.combo == 2U && score.max_combo == 2U, "head combo matches core semantics");
    require(session->total_resolved_notes() == chart.notes.size(), "all note owners resolve");
    require(!session->complete(), "resolved PFC content waits for the media end");
    require(session->finish_song(4'000.0), session->error());
    require(session->complete(), "media end completes a drained streamed chart");
    require(session->recorded_inputs().size() == 3U, "press/release input is recorded");
}

void test_explicit_catchup_budget(const std::filesystem::path& directory) {
    pulseforge::PackedChartData chart;
    chart.key_count = 4U;
    chart.kinds = {"normal"};
    constexpr std::size_t note_count = 1'000U;
    chart.notes.reserve(note_count);
    for (std::size_t index = 0U; index < note_count; ++index) {
        chart.notes.push_back({
            static_cast<std::int64_t>(index * 1'000U),
            0U,
            static_cast<std::uint16_t>(index % 4U),
            pulseforge::PackedNoteOwner::player,
            0U,
            0U,
        });
    }
    const auto reader = write_and_open(directory / "catchup.pfc", chart, 64U);

    pulseforge::GameplaySettings settings;
    settings.no_fail = true;
    pulseforge::StreamingGameplayOptions options;
    options.max_window_notes = 32U;
    options.max_active_holds = 2U;
    options.max_events_per_frame = 8U;
    options.max_recorded_inputs = 4U;
    options.max_explicit_chunk_notes = 64U;
    options.max_explicit_catchup_notes_per_update = 100U;
    std::string error;
    auto session = pulseforge::StreamingGameplaySession::create(
        reader,
        settings,
        options,
        {},
        &error
    );
    require(session.has_value(), error);

    require(session->update(5'000.0), session->error());
    require(session->catchup_pending(), "large explicit jump reports pending catch-up");
    require(
        session->summary().misses < note_count,
        "one update does not exceed explicit sequential work budget"
    );
    std::size_t iterations = 1U;
    while (session->catchup_pending() && iterations < 32U) {
        require(session->update(5'000.0), session->error());
        ++iterations;
    }
    require(!session->catchup_pending(), "explicit catch-up eventually drains");
    require(session->summary().misses == note_count, "every explicit player note is judged");
    require(session->total_resolved_notes() == note_count, "explicit resolved count is exact");
    require(!session->complete(), "catch-up alone does not end streamed playback");
    require(session->finish_song(5'000.0), session->error());
    require(session->complete(), "media end completes after explicit catch-up");
    const auto memory = session->memory_stats();
    require(memory.window_notes <= 32U, "explicit gameplay window obeys hard cap");
    require(
        memory.window_capacity <= 32U * 3U,
        "explicit sliding window keeps only its bounded amortization tail"
    );
    require(memory.cached_explicit_notes <= 64U, "only one bounded PFC chunk is cached");
}

void test_large_explicit_chunk_uses_decoded_byte_budget(
    const std::filesystem::path& directory
) {
    // Regression for the historical 262,144-explicit-notes-per-chunk ceiling.
    // The cursor still decodes one full PFC1 chunk, so compatibility is governed
    // by a byte budget rather than by an arbitrary note-count constant.
    constexpr std::uint32_t note_count = 270'000U;

    pulseforge::PackedChartData chart;
    chart.key_count = 4U;
    chart.kinds = {"normal"};
    chart.notes.reserve(note_count);
    for (std::uint32_t index = 0U; index < note_count; ++index) {
        chart.notes.push_back({
            static_cast<std::int64_t>(index) * 10'000LL,
            0U,
            static_cast<std::uint16_t>(index % 4U),
            pulseforge::PackedNoteOwner::player,
            0U,
            0U,
        });
    }

    const auto reader = write_and_open(
        directory / "large-explicit-single-chunk.pfc",
        chart,
        note_count
    );
    require(reader.chunk_count() == 1U, "large explicit fixture is one PFC1 chunk");

    pulseforge::GameplaySettings settings;
    settings.no_fail = true;

    pulseforge::StreamingGameplayOptions options;
    options.max_window_notes = 1'024U;
    options.max_events_per_frame = 64U;
    options.max_recorded_inputs = 4U;
    require(
        options.max_explicit_chunk_notes == 0U,
        "default streaming policy has no arbitrary explicit-note chunk ceiling"
    );

    std::string error;
    auto session = pulseforge::StreamingGameplaySession::create(
        reader,
        settings,
        options,
        {},
        &error
    );
    require(session.has_value(), error);
    require(
        session->update(0.0),
        std::string("large PFC1 chunk should fit the decoded-byte budget: ")
            + std::string(session->error())
    );
    require(
        session->memory_stats().cached_explicit_notes >= note_count,
        "large single PFC1 chunk is actually decoded, not merely accepted by metadata"
    );

    auto constrained = options;
    constrained.max_explicit_chunk_decoded_bytes =
        (static_cast<std::size_t>(note_count) - 1U) * sizeof(pulseforge::PackedNote);
    auto constrained_session = pulseforge::StreamingGameplaySession::create(
        reader,
        settings,
        constrained,
        {},
        &error
    );
    require(constrained_session.has_value(), error);
    require(
        !constrained_session->update(0.0),
        "decoded-byte budget rejects a chunk before an oversized vector allocation"
    );
    require(
        constrained_session->error().find("decoded bytes") != std::string::npos
            && constrained_session->error().find("Repack PFC1") != std::string::npos,
        "decoded-byte rejection provides an actionable PFC1 diagnostic"
    );

    auto legacy_count_policy = options;
    legacy_count_policy.max_explicit_chunk_notes = 262'144U;
    auto legacy_session = pulseforge::StreamingGameplaySession::create(
        reader,
        settings,
        legacy_count_policy,
        {},
        &error
    );
    require(legacy_session.has_value(), error);
    require(
        !legacy_session->update(0.0),
        "callers can still opt into an explicit note-count policy"
    );
}

void test_event_keeps_visual_note_after_window_compaction(
    const std::filesystem::path& directory
) {
    pulseforge::PackedChartData chart;
    chart.key_count = 4U;
    chart.kinds = {"normal", "Alt Animation"};
    chart.notes = {
        {100'000, 0U, 2U, pulseforge::PackedNoteOwner::player, 0U, 1U},
        {5'000'000, 0U, 1U, pulseforge::PackedNoteOwner::player, 0U, 0U},
    };
    const auto reader = write_and_open(directory / "event-snapshot.pfc", chart, 1U);

    pulseforge::StreamingGameplayOptions options;
    options.look_ahead_us = 6'000'000;
    options.terminal_retention_us = 0;
    options.max_window_notes = 1U;
    options.max_active_holds = 1U;
    options.max_events_per_frame = 16U;
    options.max_recorded_inputs = 4U;
    options.max_explicit_chunk_notes = 1U;
    std::string error;
    auto session = pulseforge::StreamingGameplaySession::create(
        reader,
        {},
        options,
        {},
        &error
    );
    require(session.has_value(), error);
    require(session->update(100.0), session->error());
    require(session->press(2U, 100.0), session->error());
    require(session->update(1'000.0), session->error());

    const auto events = session->frame_events();
    const auto hit = std::find_if(
        events.begin(),
        events.end(),
        [](const pulseforge::StreamingGameplayEvent& event) {
            return event.type == pulseforge::GameplayEventType::note_hit;
        }
    );
    require(hit != events.end(), "compacted hit event remains observable");
    require(hit->has_visual_note, "hit event owns a bounded visual note snapshot");
    require(
        hit->visual_note.time_us == 100'000
            && hit->visual_note.lane == 2U
            && hit->visual_note.kind_id == 1U
            && hit->visual_display_lane == 2U,
        "event metadata stays exact after its gameplay window entry is compacted"
    );
}

void test_trillion_note_pattern_constant_memory(
    const std::filesystem::path& directory
) {
    pulseforge::PackedChartData chart;
    chart.key_count = 4U;
    chart.kinds = {"normal"};
    pulseforge::PatternRun pattern;
    pattern.start_us = 0;
    pattern.interval_us = 100U;
    pattern.count = trillion;
    pattern.lane_pattern = {0U, 1U, 2U, 3U};
    pattern.owner = pulseforge::PackedNoteOwner::player;
    chart.patterns.push_back(pattern);
    const auto path = directory / "trillion.pfc";
    const auto reader = write_and_open(path, chart);
    require(
        std::filesystem::file_size(path) < 100U * 1024U,
        "trillion-note gameplay fixture remains constant-size"
    );

    pulseforge::GameplaySettings settings;
    settings.no_fail = true;
    settings.stacked_note_tolerance_ms = 0.0;
    pulseforge::StreamingGameplayOptions options;
    options.max_window_notes = 64U;
    options.max_active_holds = 2U;
    options.max_events_per_frame = 16U;
    options.max_recorded_inputs = 4U;
    options.max_explicit_chunk_notes = 64U;
    options.max_explicit_catchup_notes_per_update = 64U;
    std::string error;
    auto session = pulseforge::StreamingGameplaySession::create(
        reader,
        settings,
        options,
        {},
        &error
    );
    require(session.has_value(), error);

    require(session->update(0.0), session->error());
    require(session->press(0U, 0.0), session->error());
    require(
        session->summary().marvelous == 1U,
        "a procedural PatternRun note is a genuinely hittable gameplay note"
    );

    const auto final_time_us = static_cast<std::uint64_t>(
        pattern.start_us
    ) + (trillion - 1U) * pattern.interval_us;
    const auto after_tail_ms = static_cast<double>(final_time_us) / 1'000.0
        + 3'000.0;
    require(session->update(after_tail_ms), session->error());
    require(!session->catchup_pending(), "PatternRun catch-up is arithmetic, not iterative");
    require(
        session->summary().misses == trillion - 1U,
        "all remaining procedural notes aggregate exactly after a real hit"
    );
    require(
        session->total_resolved_notes() == trillion
            && session->summary().chart_total == trillion,
        "trillion PatternRun heads resolve into exact bounded Chart Total"
    );
    require(!session->complete(), "procedural resolution alone does not end playback");
    require(session->finish_song(after_tail_ms), session->error());
    require(session->complete(), "media end completes the trillion-note chart");

    bool saw_aggregate = false;
    for (const auto& event : session->frame_events()) {
        if (event.type == pulseforge::GameplayEventType::note_miss
            && event.occurrence_count > 1U) {
            saw_aggregate = true;
        }
    }
    require(saw_aggregate, "dense PatternRun produces a bounded aggregate event");
    const auto memory = session->memory_stats();
    require(memory.window_notes <= 64U, "trillion-note window remains bounded");
    require(
        memory.window_capacity <= 64U * 3U,
        "procedural sliding window keeps only its bounded amortization tail"
    );
    require(memory.cached_explicit_notes == 0U, "procedural chart caches no explicit chunk");
    require(memory.pattern_cursors == 1U, "one constant-size PatternRun cursor is kept");
    require(
        memory.approximate_dynamic_bytes < 256U * 1024U,
        "trillion-note session state remains comfortably below 256 KiB"
    );
}

void test_dense_autoplay_sustain_overflow(
    const std::filesystem::path& directory
) {
    pulseforge::PackedChartData chart;
    chart.key_count = 4U;
    chart.kinds = {"normal"};
    constexpr std::uint64_t tap_count = 200U;
    chart.notes.reserve(static_cast<std::size_t>(tap_count + 1U));
    for (std::uint64_t index = 0U; index < tap_count; ++index) {
        chart.notes.push_back({
            0,
            0U,
            0U,
            pulseforge::PackedNoteOwner::player,
            0U,
            0U,
        });
    }
    chart.notes.push_back({
        0,
        1'000'000U,
        0U,
        pulseforge::PackedNoteOwner::player,
        0U,
        0U,
    });
    const auto reader = write_and_open(
        directory / "dense-autoplay-sustain.pfc",
        chart,
        64U
    );

    pulseforge::GameplaySettings settings;
    settings.autoplay = true;
    settings.practice = true;
    settings.no_fail = true;
    settings.stacked_note_tolerance_ms = 0.0;

    const auto run = [&](const std::size_t maximum_window) {
        pulseforge::StreamingGameplayOptions options;
        options.max_window_notes = maximum_window;
        options.max_active_holds = 8U;
        options.max_events_per_frame = 32U;
        options.max_explicit_chunk_notes = 64U;
        options.max_explicit_catchup_notes_per_update = 1'000U;
        std::string error;
        auto session = pulseforge::StreamingGameplaySession::create(
            reader,
            settings,
            options,
            {{0.0, 120.0, 4U, 4U}},
            &error
        );
        require(session.has_value(), error);
        require(session->update(2'000.0), session->error());
        require(session->finish_song(2'000.0), session->error());
        require(session->complete(), "dense autoplay sustain reaches media end");
        require(
            session->total_resolved_notes() == tap_count + 1U,
            "dense autoplay resolves every tap and sustain"
        );
        return session->summary();
    };

    const auto bounded = run(64U);
    const auto materialized = run(512U);
    require(
        bounded.marvelous == materialized.marvelous
            && bounded.hold_ticks == materialized.hold_ticks
            && bounded.score == materialized.score,
        "overflow sustain arithmetic matches the fully buffered autoplay result"
    );
}

void test_default_active_holds_are_allocator_limited(
    const std::filesystem::path& directory
) {
    // The old default hard-failed at 4,096 simultaneous sustains even though
    // enough memory was available and the chart was otherwise valid.
    constexpr std::size_t hold_count = 5'000U;
    pulseforge::PackedChartData chart;
    chart.key_count = 4U;
    chart.kinds = {"normal"};
    chart.notes.reserve(hold_count);
    for (std::size_t index = 0U; index < hold_count; ++index) {
        chart.notes.push_back({
            0,
            10'000'000U,
            static_cast<std::uint16_t>(index % chart.key_count),
            pulseforge::PackedNoteOwner::player,
            0U,
            0U,
        });
    }
    const auto reader = write_and_open(
        directory / "allocator-limited-active-holds.pfc",
        chart,
        1'024U
    );

    pulseforge::GameplaySettings settings;
    settings.autoplay = true;
    settings.practice = true;
    settings.no_fail = true;

    pulseforge::StreamingGameplayOptions options;
    require(
        options.max_active_holds == 0U,
        "default active-hold policy must be allocator-limited"
    );
    options.max_window_notes = hold_count + 16U;
    options.max_events_per_frame = 32U;
    options.max_explicit_chunk_notes = 1'024U;
    options.max_explicit_catchup_notes_per_update = hold_count + 16U;
    std::string error;
    auto session = pulseforge::StreamingGameplaySession::create(
        reader,
        settings,
        options,
        {{0.0, 120.0, 4U, 4U}},
        &error
    );
    require(session.has_value(), error);
    require(session->update(0.0), session->error());
    require(
        session->memory_stats().active_holds == hold_count,
        "more than 4,096 valid simultaneous sustains remain playable"
    );

    options.max_active_holds = 4'096U;
    auto policy_limited = pulseforge::StreamingGameplaySession::create(
        reader,
        settings,
        options,
        {{0.0, 120.0, 4U, 4U}},
        &error
    );
    require(policy_limited.has_value(), error);
    require(
        !policy_limited->update(0.0)
            && policy_limited->error().find("configured streaming memory policy")
                != std::string_view::npos,
        "an explicit active-hold policy remains enforceable with a useful error"
    );
}

void test_pattern_sustain_aggregation_contracts(
    const std::filesystem::path& directory
) {
    const auto run = [&directory](
        const std::string& name,
        const std::uint64_t count,
        const std::uint64_t interval_us,
        const std::uint64_t duration_us,
        const double end_ms,
        std::vector<pulseforge::TempoChange> tempos = {
            {0.0, 120.0, 4U, 4U},
        }
    ) {
        pulseforge::PackedChartData chart;
        chart.key_count = 4U;
        chart.kinds = {"normal"};
        chart.patterns.push_back({
            0,
            interval_us,
            count,
            duration_us,
            {0U},
            pulseforge::PackedNoteOwner::player,
            0U,
            0U,
        });
        const auto reader = write_and_open(directory / (name + ".pfc"), chart);
        pulseforge::GameplaySettings settings;
        settings.autoplay = true;
        settings.practice = true;
        settings.no_fail = true;
        pulseforge::StreamingGameplayOptions options;
        options.max_window_notes = 2U;
        options.max_active_holds = 2U;
        options.max_events_per_frame = 16U;
        std::string error;
        auto session = pulseforge::StreamingGameplaySession::create(
            reader,
            settings,
            options,
            std::move(tempos),
            &error
        );
        require(session.has_value(), error);
        static_cast<void>(session->finish_song(end_ms));
        return session;
    };

    // Exactly the adversarial phase from the audit: endpoint samples both
    // have zero ticks, but the interior sequence is 0,1,1,0,1,0. It must be
    // rejected instead of silently applying an incorrect endpoint shortcut.
    auto phased = run("pattern-sustain-phased", 6U, 50'000U, 100'000U, 500.0);
    require(
        !phased->healthy()
            && phased->error().find("not uniformly aggregatable")
                != std::string_view::npos,
        "non-uniform PatternRun sustain phase fails explicitly, never miscounts"
    );
    require(
        phased->total_resolved_notes() < 6U
            && phased->summary().marvelous
                == phased->total_resolved_notes(),
        "rejected PatternRun sustain batch adds no partial aggregate award"
    );

    auto tempo_round_trip = run(
        "pattern-sustain-tempo-round-trip",
        6U,
        31'250U,
        125'000U,
        500.0,
        {
            {0.0, 120.0, 4U, 4U},
            {100.0, 60.0, 4U, 4U},
            {200.0, 120.0, 4U, 4U},
        }
    );
    require(
        !tempo_round_trip->healthy()
            && tempo_round_trip->error().find("not uniformly aggregatable")
                != std::string_view::npos,
        "internal tempo boundaries reject endpoint-equal PatternRun shortcuts"
    );

    auto half_step_phase = run(
        "pattern-sustain-half-step-phase",
        5U,
        62'500U,
        125'000U,
        500.0
    );
    require(
        !half_step_phase->healthy()
            && half_step_phase->error().find("not uniformly aggregatable")
                != std::string_view::npos,
        "fractional-step interval never uses an endpoint-equality shortcut"
    );

    constexpr std::uint64_t ten_billion = 10'000'000'000ULL;
    const auto begin = std::chrono::steady_clock::now();
    auto uniform = run(
        "pattern-sustain-10b",
        ten_billion,
        500'000U,
        500'000U,
        static_cast<double>(ten_billion) * 500.0 + 1'000.0
    );
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - begin
    ).count();
    require(uniform->healthy() && uniform->complete(), uniform->error());
    require(
        uniform->total_resolved_notes() == ten_billion
            && uniform->summary().hold_ticks == 30'000'000'000ULL,
        "uniform 10B PatternRun sustains aggregate exact heads and ticks"
    );
    require(elapsed < 5.0, "uniform 10B sustain aggregation is count-independent");
}

void test_media_end_drains_dense_exact_stack(
    const std::filesystem::path& directory
) {
    pulseforge::PackedChartData chart;
    chart.key_count = 4U;
    chart.kinds = {"normal"};
    constexpr std::size_t note_count = 10'000U;
    chart.notes.reserve(note_count);
    for (std::size_t index = 0U; index < note_count; ++index) {
        chart.notes.push_back({
            1'000'000,
            0U,
            0U,
            pulseforge::PackedNoteOwner::player,
            0U,
            0U,
        });
    }
    const auto reader = write_and_open(directory / "exact-stack.pfc", chart, 128U);

    pulseforge::GameplaySettings settings;
    settings.no_fail = true;
    pulseforge::StreamingGameplayOptions options;
    options.max_window_notes = 64U;
    options.max_active_holds = 4U;
    options.max_events_per_frame = 16U;
    options.max_explicit_chunk_notes = 128U;
    options.max_explicit_catchup_notes_per_update = 500U;
    std::string error;
    auto session = pulseforge::StreamingGameplaySession::create(
        reader,
        settings,
        options,
        {},
        &error
    );
    require(session.has_value(), error);
    require(session->update(1'000.0), session->error());
    require(session->press(0U, 1'000.0), session->error());

    std::size_t iterations{};
    while (!session->complete() && iterations++ < 64U) {
        session->begin_frame();
        require(session->finish_song(1'000.0), session->error());
    }
    require(session->complete(), "exact-end dense stack completes within bounded passes");
    require(
        session->summary().marvelous == note_count
            && session->total_resolved_notes() == note_count,
        "all explicit coincident notes remain real hits beyond the temporal window cap"
    );

    pulseforge::PackedChartData truncated;
    truncated.key_count = 4U;
    truncated.kinds = {"normal"};
    truncated.notes = {
        {1'000'000, 0U, 0U, pulseforge::PackedNoteOwner::player, 0U, 0U},
        {2'000'000, 0U, 1U, pulseforge::PackedNoteOwner::player, 0U, 0U},
    };
    const auto truncated_reader = write_and_open(
        directory / "truncated-by-media.pfc",
        truncated,
        2U
    );
    auto truncated_session = pulseforge::StreamingGameplaySession::create(
        truncated_reader,
        settings,
        options,
        {},
        &error
    );
    require(truncated_session.has_value(), error);
    require(truncated_session->finish_song(1'000.0), truncated_session->error());
    require(
        truncated_session->complete()
            && truncated_session->summary().misses == 1U
            && truncated_session->total_resolved_notes() == 2U,
        "media end judges the exact note and ignores only future chart data"
    );
}



void test_opponent_sustain_keeps_unconsumed_tail(
    const std::filesystem::path& directory
) {
    // PULSEFORGE_P1_3_0_STREAMING_OPPONENT_SUSTAIN_LIFETIME_TEST_V1
    pulseforge::PackedChartData chart;
    chart.key_count = 4U;
    chart.kinds = {"normal"};
    chart.notes = {
        {1'000'000, 600'000U, 1U, pulseforge::PackedNoteOwner::opponent, 0U, 0U},
    };
    const auto reader = write_and_open(
        directory / "opponent-sustain-lifetime.pfc",
        chart,
        2U
    );

    pulseforge::GameplaySettings settings;
    settings.no_fail = true;
    pulseforge::StreamingGameplayOptions options;
    options.max_window_notes = 16U;
    options.max_active_holds = 4U;
    options.max_events_per_frame = 32U;
    options.max_recorded_inputs = 16U;
    options.max_explicit_chunk_notes = 4U;
    std::string error;
    auto session = pulseforge::StreamingGameplaySession::create(
        reader,
        settings,
        options,
        {{0.0, 120.0, 4U, 4U}},
        {},
        &error
    );
    require(session.has_value(), error);

    require(session->update(1'000.0), session->error());
    auto state_for_only_note = [&]() -> std::optional<pulseforge::NoteState> {
        const auto notes = session->window_notes();
        const auto found = std::find_if(
            notes.begin(),
            notes.end(),
            [](const auto& note) {
                return note.note.owner
                    == pulseforge::PackedNoteOwner::opponent;
            }
        );
        return found == notes.end()
            ? std::nullopt
            : std::optional<pulseforge::NoteState>{found->state};
    };
    require(
        state_for_only_note() == pulseforge::NoteState::holding,
        "streaming opponent sustain remains holding after its head"
    );
    require(
        std::count_if(
            session->frame_events().begin(),
            session->frame_events().end(),
            [](const auto& event) {
                return event.type
                    == pulseforge::GameplayEventType::opponent_hit;
            }
        ) == 1,
        "streaming opponent sustain emits opponent_hit once"
    );

    // Same numbered player lane must not release the opponent's visual hold.
    require(session->press(1U, 1'100.0), session->error());
    require(session->release(1U, 1'150.0), session->error());
    session->begin_frame();
    require(session->update(1'300.0), session->error());
    require(
        state_for_only_note() == pulseforge::NoteState::holding,
        "streaming opponent sustain ignores matching player-lane release"
    );
    require(
        session->summary().score == 0
            && session->summary().hold_ticks == 0U
            && session->summary().hold_drops == 0U,
        "streaming opponent sustain has no player scoring side effects"
    );

    session->begin_frame();
    require(session->update(1'599.0), session->error());
    require(
        state_for_only_note() == pulseforge::NoteState::holding,
        "streaming opponent sustain survives until immediately before tail"
    );
    session->begin_frame();
    require(session->update(1'600.0), session->error());
    const auto final_state = state_for_only_note();
    require(
        !final_state.has_value()
            || *final_state == pulseforge::NoteState::completed,
        "streaming opponent sustain resolves at its tail"
    );
    require(
        std::none_of(
            session->frame_events().begin(),
            session->frame_events().end(),
            [](const auto& event) {
                return event.type
                    == pulseforge::GameplayEventType::hold_complete;
            }
        ),
        "streaming opponent sustain does not emit player hold_complete"
    );
}

void test_dynamic_lane_topology(const std::filesystem::path& directory) {
    // PULSEFORGE_P1_3_0_STREAMING_DYNAMIC_LANE_TOPOLOGY_TEST_V1
    pulseforge::PackedChartData chart;
    chart.key_count = 6U;
    chart.kinds = {"normal"};
    chart.notes = {
        {100'000, 500'000U, 0U, pulseforge::PackedNoteOwner::player, 0U, 0U},
        {200'000, 0U, 0U, pulseforge::PackedNoteOwner::opponent, 0U, 0U},
        {400'000, 0U, 5U, pulseforge::PackedNoteOwner::player, 0U, 0U},
    };
    const auto reader = write_and_open(
        directory / "dynamic-lane-topology.pfc",
        chart,
        2U
    );

    pulseforge::GameplaySettings settings;
    settings.no_fail = true;
    settings.mirror = true;
    pulseforge::StreamingGameplayOptions options;
    options.max_window_notes = 16U;
    options.max_active_holds = 4U;
    options.max_events_per_frame = 32U;
    options.max_recorded_inputs = 16U;
    options.max_explicit_chunk_notes = 4U;
    std::vector<pulseforge::ChartEvent> events{
        {50.0, "Change P2 Mania", "3", "false", {}},
    };
    std::string error;
    auto session = pulseforge::StreamingGameplaySession::create(
        reader,
        settings,
        options,
        {{0.0, 120.0, 4U, 4U}},
        std::move(events),
        &error
    );
    require(session.has_value(), error);

    require(
        session->player_key_count() == 6U
            && session->opponent_key_count() == 6U,
        "streaming dynamic topology starts from reader source domain"
    );
    require(session->update(50.0), session->error());
    require(
        session->player_key_count() == 6U
            && session->opponent_key_count() == 3U,
        "streaming scheduled P2 mania changes only opponent topology"
    );

    require(
        session->apply_event("Change P1 Mania", "2", "true"),
        "streaming P1 mania is recognized"
    );
    require(
        session->player_key_count() == 2U
            && session->opponent_key_count() == 3U,
        "streaming P1/P2 key counts remain independent"
    );
    require(
        session->display_lane(pulseforge::NoteOwner::player, 0U) == 1U
            && session->display_lane(pulseforge::NoteOwner::opponent, 0U) == 2U,
        "streaming owner-aware display projection follows each active topology"
    );
    // PULSEFORGE_P1_5_0C3_STREAMING_COMPLETE_DYNAMIC_LANE_PROJECTION_TEST_V1
    for (std::uint16_t source = 0U; source < chart.key_count; ++source) {
        require(
            session->display_lane(pulseforge::NoteOwner::player, source) < 2U,
            "every streaming player source lane projects inside shrunken 2K"
        );
        require(
            session->display_lane(pulseforge::NoteOwner::opponent, source) < 3U,
            "every streaming opponent source lane projects inside shrunken 3K"
        );
    }
    require(
        session->display_lane(pulseforge::NoteOwner::player, 5U) == 0U,
        "streaming high source lanes use proportional mirrored projection"
    );

    require(session->apply_event("Change Mania", "4", "false"),
            "streaming generic Change Mania is recognized");
    require(
        session->player_key_count() == 4U
            && session->opponent_key_count() == 4U,
        "streaming generic Change Mania updates both sides"
    );
    static_cast<void>(session->apply_event("Change P1 Mania", "19", "true"));
    require(
        session->player_key_count() == 4U,
        "streaming dynamic mania rejects counts above reader source domain"
    );

    // Restore 6K, hit source lane 0 on mirrored display lane 5, then shrink to
    // 2K. The active hold must follow source lane 0 to display lane 1.
    static_cast<void>(session->apply_event("Change P1 Mania", "6", "false"));
    require(session->update(100.0), session->error());
    require(session->press(5U, 100.0), session->error());
    static_cast<void>(session->apply_event("Change P1 Mania", "2", "true"));
    require(
        session->display_lane(pulseforge::NoteOwner::player, 0U) == 1U
            && session->lane_held(1U),
        "streaming held source lane is reprojected after P1 mania"
    );
    require(session->update(300.0), session->error());
    require(
        session->summary().hold_drops == 0U,
        "streaming topology change does not synthesize a hold drop"
    );
    require(session->press(0U, 400.0), session->error());
    require(
        session->summary().marvelous == 2U,
        "streaming high source lane remains judgeable after a 6K-to-2K shrink"
    );
    require(session->release(1U, 600.0), session->error());
    require(
        session->summary().hold_drops == 0U,
        "streaming remapped sustain completes on its new display lane"
    );
}


// PULSEFORGE_P1_5_0_STREAMING_NOTE_KIND_RUNTIME_BEHAVIOR_TEST_V1
void test_note_kind_runtime_behavior(
    const std::filesystem::path& directory
) {
    pulseforge::PackedChartData chart;
    chart.key_count = 4U;
    chart.kinds = {"Custom Heal", "Custom Ignore", "Custom Hazard", "Custom Sustain"};
    chart.notes = {
        {1'000'000, 0U, 0U, pulseforge::PackedNoteOwner::player, 0U, 0U},
        {1'500'000, 0U, 1U, pulseforge::PackedNoteOwner::player, 0U, 1U},
        {2'000'000, 0U, 2U, pulseforge::PackedNoteOwner::player, 0U, 2U},
        {2'500'000, 500'000U, 3U, pulseforge::PackedNoteOwner::player, 0U, 3U},
    };
    const auto reader = write_and_open(
        directory / "note-kind-runtime-behavior.pfc", chart, 2U
    );

    pulseforge::GameplaySettings settings;
    settings.no_fail = true;
    pulseforge::StreamingGameplayOptions options;
    options.max_window_notes = 16U;
    options.max_active_holds = 4U;
    options.max_events_per_frame = 32U;
    options.max_recorded_inputs = 16U;
    options.max_explicit_chunk_notes = 4U;
    std::string error;
    auto session = pulseforge::StreamingGameplaySession::create(
        reader,
        settings,
        options,
        {{0.0, 120.0, 4U, 4U}},
        &error
    );
    require(session.has_value(), error);

    pulseforge::NoteKindRuntimeBehavior heal;
    heal.hit_health = 0.20;
    heal.miss_health = 0.40;
    require(session->set_note_kind_behavior("Custom Heal", heal), "streaming heal behavior installs");

    pulseforge::NoteKindRuntimeBehavior ignore;
    ignore.ignore_note = true;
    require(session->set_note_kind_behavior("Custom Ignore", ignore), "streaming ignore behavior installs");

    pulseforge::NoteKindRuntimeBehavior hazard;
    hazard.hit_causes_miss = true;
    hazard.miss_health = 0.30;
    require(session->set_note_kind_behavior("Custom Hazard", hazard), "streaming hazard behavior installs");

    pulseforge::NoteKindRuntimeBehavior sustain;
    sustain.hit_health = 0.10;
    sustain.miss_health = 0.20;
    sustain.sustain_miss_health = 0.25;
    require(session->set_note_kind_behavior("Custom Sustain", sustain), "streaming sustain behavior installs");

    require(session->update(900.0), session->error());
    require(session->press(0U, 1'000.0), session->error());
    require(
        std::abs(session->summary().health - 1.20) < 0.000001,
        "streaming custom hitHealth changes hit health"
    );

    require(session->update(1'900.0), session->error());
    require(
        session->summary().misses == 0U,
        "streaming ignoreNote suppresses missed-note statistics"
    );

    require(session->press(2U, 2'000.0), session->error());
    require(
        session->summary().misses == 1U
            && std::abs(session->summary().health - 0.90) < 0.000001,
        "streaming hitCausesMiss uses custom missHealth"
    );

    require(session->press(3U, 2'500.0), session->error());
    require(
        std::abs(session->summary().health - 1.00) < 0.000001,
        "streaming custom sustain head uses hitHealth"
    );
    require(session->release(3U, 2'600.0), session->error());
    require(session->update(2'650.0), session->error());
    require(
        session->summary().hold_drops == 1U
            && std::abs(session->summary().health - 0.75) < 0.000001,
        "streaming sustain drop uses sustain missHealth"
    );
}


// PULSEFORGE_P1_5_0C_STREAMING_DECLARATIVE_SUSTAIN_POLICY_TEST_V1
void test_declarative_sustain_policy(
    const std::filesystem::path& directory
) {
    pulseforge::PackedChartData chart;
    chart.key_count = 4U;
    chart.kinds = {"Tap Sustain", "Head Only Sustain"};
    chart.notes = {
        {1'000'000, 500'000U, 0U, pulseforge::PackedNoteOwner::player, 0U, 0U},
        {2'000'000, 500'000U, 1U, pulseforge::PackedNoteOwner::player, 0U, 1U},
    };
    const auto reader = write_and_open(
        directory / "declarative-sustain-policy.pfc", chart, 2U
    );

    pulseforge::GameplaySettings settings;
    settings.no_fail = true;
    pulseforge::StreamingGameplayOptions options;
    options.max_window_notes = 16U;
    options.max_active_holds = 4U;
    options.max_events_per_frame = 32U;
    options.max_recorded_inputs = 16U;
    options.max_explicit_chunk_notes = 4U;
    std::string error;
    auto session = pulseforge::StreamingGameplaySession::create(
        reader,
        settings,
        options,
        {{0.0, 120.0, 4U, 4U}},
        &error
    );
    require(session.has_value(), error);

    pulseforge::NoteKindRuntimeBehavior tap_sustain;
    tap_sustain.hit_health = 0.10;
    tap_sustain.sustain_enabled = false;
    tap_sustain.sustain_hit_causes_miss = true;
    tap_sustain.sustain_miss_health = 0.90;
    require(
        session->set_note_kind_behavior("Tap Sustain", tap_sustain),
        "streaming sustain.enabled behavior installs"
    );

    pulseforge::NoteKindRuntimeBehavior head_only;
    head_only.hit_health = 0.10;
    head_only.miss_health = 0.40;
    head_only.sustain_miss_health = 0.90;
    head_only.hit_causes_miss = false;
    head_only.sustain_hit_causes_miss = true;
    head_only.sustain_inherits_type = false;
    require(
        session->set_note_kind_behavior("Head Only Sustain", head_only),
        "streaming sustain.inheritsType behavior installs"
    );

    require(session->update(900.0), session->error());
    require(session->press(0U, 1'000.0), session->error());
    const auto first = std::find_if(
        session->window_notes().begin(),
        session->window_notes().end(),
        [](const pulseforge::StreamingWindowNote& note) {
            return note.note.time_us == 1'000'000;
        }
    );
    require(
        first == session->window_notes().end()
            || first->state == pulseforge::NoteState::completed,
        "streaming sustain.enabled=false never leaves an active hold"
    );
    require(
        std::abs(session->summary().health - 1.10) < 0.000001,
        "streaming disabled sustain keeps custom head hitHealth"
    );
    require(session->release(0U, 1'050.0), session->error());
    require(session->update(1'200.0), session->error());
    require(
        session->summary().hold_drops == 0U,
        "streaming disabled sustain never drops"
    );

    session->set_health(1.0);
    require(session->update(1'900.0), session->error());
    require(session->press(1U, 2'000.0), session->error());
    require(
        std::abs(session->summary().health - 1.10) < 0.000001,
        "streaming head-only sustain applies custom head semantics"
    );
    require(session->release(1U, 2'050.0), session->error());
    require(
        session->summary().hold_drops == 1U
            && std::abs(session->summary().health - 1.02) < 0.000001,
        "streaming inheritsType=false returns tail/drop health to normal"
    );

    // Giant procedural source: a physical sustain disabled by type semantics
    // must remain an arithmetic tap pattern, not allocate one hold per note.
    constexpr std::uint64_t giant_count = 1'000'000'000ULL;
    pulseforge::PackedChartData pattern_chart;
    pattern_chart.key_count = 4U;
    pattern_chart.kinds = {"Tap Sustain"};
    pattern_chart.patterns.push_back({
        0,
        500'000U,
        giant_count,
        500'000U,
        {0U},
        pulseforge::PackedNoteOwner::player,
        0U,
        0U,
    });
    const auto pattern_reader = write_and_open(
        directory / "declarative-sustain-policy-pattern.pfc", pattern_chart
    );
    auto autoplay_settings = settings;
    autoplay_settings.autoplay = true;
    autoplay_settings.practice = true;
    pulseforge::StreamingGameplayOptions pattern_options;
    pattern_options.max_window_notes = 2U;
    pattern_options.max_active_holds = 2U;
    pattern_options.max_events_per_frame = 16U;
    std::string pattern_error;
    auto pattern_session = pulseforge::StreamingGameplaySession::create(
        pattern_reader,
        autoplay_settings,
        pattern_options,
        {{0.0, 120.0, 4U, 4U}},
        &pattern_error
    );
    require(pattern_session.has_value(), pattern_error);
    require(
        pattern_session->set_note_kind_behavior("Tap Sustain", tap_sustain),
        "giant pattern sustain override installs"
    );
    const double pattern_end_ms = static_cast<double>(giant_count) * 500.0 + 1'000.0;
    require(pattern_session->finish_song(pattern_end_ms), pattern_session->error());
    require(
        pattern_session->complete()
            && pattern_session->total_resolved_notes() == giant_count
            && pattern_session->summary().hold_ticks == 0U
            && pattern_session->memory_stats().active_holds <= 2U,
        "sustain.enabled=false stays bounded and arithmetic for giant PatternRun"
    );
}

void test_real_notetype_materialized_streaming_parity(
    const std::filesystem::path& directory
) {
    // PULSEFORGE_P1_5_0D_REAL_NOTETYPE_RUNTIME_PARITY_TEST_V1
    // This fixture combines the real `the note.lua` gameplay mutation
    // (missHealth = 0.6), typed Hurt Note head/sustain damage, and a 6K -> 2K
    // mirrored topology change. Both backends must produce the same state.
    pulseforge::Chart materialized_chart;
    materialized_chart.key_count = 6U;
    materialized_chart.tempos = {{0.0, 120.0, 4U, 4U}};
    materialized_chart.notes = {
        {1'000.0, 0.0, 0U, pulseforge::NoteOwner::player, "the note"},
        {1'500.0, 0.0, 5U, pulseforge::NoteOwner::player, "the note"},
        {1'800.0, 0.0, 1U, pulseforge::NoteOwner::player, "Hurt Note"},
        {2'000.0, 500.0, 4U, pulseforge::NoteOwner::player, "Hurt Note"},
    };
    materialized_chart.normalize();

    pulseforge::PackedChartData packed_chart;
    packed_chart.key_count = 6U;
    packed_chart.kinds = {"the note", "Hurt Note"};
    packed_chart.notes = {
        {1'000'000, 0U, 0U, pulseforge::PackedNoteOwner::player, 0U, 0U},
        {1'500'000, 0U, 5U, pulseforge::PackedNoteOwner::player, 0U, 0U},
        {1'800'000, 0U, 1U, pulseforge::PackedNoteOwner::player, 0U, 1U},
        {2'000'000, 500'000U, 4U, pulseforge::PackedNoteOwner::player, 0U, 1U},
    };
    const auto reader = write_and_open(
        directory / "real-notetype-runtime-parity.pfc",
        packed_chart,
        4U
    );

    pulseforge::GameplaySettings settings;
    settings.no_fail = true;
    settings.mirror = true;

    pulseforge::GameplaySession materialized(materialized_chart, settings);
    pulseforge::StreamingGameplayOptions options;
    options.max_window_notes = 16U;
    options.max_active_holds = 4U;
    options.max_events_per_frame = 64U;
    options.max_recorded_inputs = 16U;
    options.max_explicit_chunk_notes = 4U;
    std::string error;
    auto streaming = pulseforge::StreamingGameplaySession::create(
        reader,
        settings,
        options,
        {{0.0, 120.0, 4U, 4U}},
        &error
    );
    require(streaming.has_value(), error);

    pulseforge::NoteKindRuntimeBehavior bullet_behavior;
    bullet_behavior.miss_health = 0.6;
    require(
        materialized.set_note_kind_behavior("the note", bullet_behavior),
        "materialized real custom-note behavior installs"
    );
    require(
        streaming->set_note_kind_behavior("the note", bullet_behavior),
        "streaming real custom-note behavior installs"
    );

    pulseforge::NoteKindRuntimeBehavior hurt_behavior;
    hurt_behavior.miss_health = 0.3;
    hurt_behavior.hit_causes_miss = true;
    hurt_behavior.sustain_miss_health = 0.1;
    hurt_behavior.sustain_hit_causes_miss = true;
    hurt_behavior.sustain_enabled = true;
    hurt_behavior.sustain_inherits_type = true;
    require(
        materialized.set_note_kind_behavior("Hurt Note", hurt_behavior),
        "materialized typed Hurt Note behavior installs"
    );
    require(
        streaming->set_note_kind_behavior("Hurt Note", hurt_behavior),
        "streaming typed Hurt Note behavior installs"
    );

    require(
        materialized.apply_event("Change P1 Mania", "2", "true")
            && streaming->apply_event("Change P1 Mania", "2", "true"),
        "both backends accept the same dynamic-mania event"
    );
    require(
        materialized.display_lane(pulseforge::NoteOwner::player, 5U) == 0U
            && streaming->display_lane(pulseforge::NoteOwner::player, 5U) == 0U,
        "real custom-note parity uses the same shrunken mirrored lane projection"
    );

    const auto compare = [&](const std::string_view stage) {
        const auto& left = materialized.summary();
        const auto& right = streaming->summary();
        require(left.score == right.score, std::string(stage) + ": score parity");
        require(left.combo == right.combo, std::string(stage) + ": combo parity");
        require(left.max_combo == right.max_combo, std::string(stage) + ": max combo parity");
        require(left.marvelous == right.marvelous, std::string(stage) + ": marvelous parity");
        require(left.sick == right.sick, std::string(stage) + ": sick parity");
        require(left.good == right.good, std::string(stage) + ": good parity");
        require(left.bad == right.bad, std::string(stage) + ": bad parity");
        require(left.misses == right.misses, std::string(stage) + ": miss parity");
        require(left.hold_ticks == right.hold_ticks, std::string(stage) + ": hold tick parity");
        require(left.hold_drops == right.hold_drops, std::string(stage) + ": hold drop parity");
        require(left.chart_total == right.chart_total, std::string(stage) + ": Chart Total parity");
        require(
            std::abs(left.weighted_hits - right.weighted_hits) < 0.000001,
            std::string(stage) + ": weighted-hit parity"
        );
        require(
            std::abs(left.judged_notes - right.judged_notes) < 0.000001,
            std::string(stage) + ": judged-note parity"
        );
        require(
            std::abs(left.health - right.health) < 0.000001,
            std::string(stage) + ": health parity"
        );
        require(left.failed == right.failed, std::string(stage) + ": failure parity");
    };

    materialized.update(1'250.0);
    require(streaming->update(1'250.0), streaming->error());
    compare("the note miss");
    require(
        materialized.summary().misses == 1U
            && std::abs(materialized.summary().health - 0.4) < 0.000001,
        "real the note.lua missHealth=0.6 is authoritative"
    );

    materialized.update(1'500.0);
    require(streaming->update(1'500.0), streaming->error());
    materialized.press(0U, 1'500.0);
    require(streaming->press(0U, 1'500.0), streaming->error());
    compare("the note projected hit");

    materialized.update(1'800.0);
    require(streaming->update(1'800.0), streaming->error());
    materialized.press(1U, 1'800.0);
    require(streaming->press(1U, 1'800.0), streaming->error());
    compare("Hurt Note head");
    require(
        std::abs(materialized.summary().health - 0.123) < 0.000001,
        "typed Hurt Note head uses the registry 0.3 damage instead of legacy 1.5x"
    );

    materialized.update(2'000.0);
    require(streaming->update(2'000.0), streaming->error());
    materialized.press(0U, 2'000.0);
    require(streaming->press(0U, 2'000.0), streaming->error());
    compare("Hurt Note sustain hazard");
    require(
        std::abs(materialized.summary().health - 0.023) < 0.000001,
        "typed Hurt Note sustain uses the registry 0.1 sustain damage"
    );

    materialized.update(3'000.0);
    require(streaming->update(3'000.0), streaming->error());
    materialized.finish_song(3'000.0);
    require(streaming->finish_song(3'000.0), streaming->error());
    compare("media end");
    require(
        materialized.complete() && streaming->complete(),
        "real NoteType parity fixture completes on both backends"
    );
}

void test_note_multiplier_parity(const std::filesystem::path& directory) {
    // PULSEFORGE_P1_1_18_STREAMING_NOTE_MULTIPLIER_REGRESSION_V1
    pulseforge::PackedChartData chart;
    chart.key_count = 4U;
    chart.kinds = {"normal"};
    chart.notes = {
        {1'000'000, 0U, 0U, pulseforge::PackedNoteOwner::player, 0U, 0U},
        {1'100'000, 0U, 1U, pulseforge::PackedNoteOwner::opponent, 0U, 0U},
    };
    const auto reader = write_and_open(
        directory / "note-multiplier.pfc",
        chart,
        2U
    );

    pulseforge::GameplaySettings settings;
    settings.no_fail = true;
    pulseforge::StreamingGameplayOptions options;
    options.max_window_notes = 16U;
    options.max_active_holds = 4U;
    options.max_events_per_frame = 64U;
    options.max_recorded_inputs = 16U;
    options.max_explicit_chunk_notes = 4U;
    std::vector<pulseforge::ChartEvent> events{
        {1'000.0, "Change Note Multiplier", "1.5", "2", {}},
        {1'100.0, "Note Multiplier", "3", "1", {}},
    };
    std::string error;
    auto session = pulseforge::StreamingGameplaySession::create(
        reader,
        settings,
        options,
        {{0.0, 120.0, 4U, 4U}},
        std::move(events),
        &error
    );
    require(session.has_value(), error);
    require(
        session->summary().chart_total == 0U,
        "streaming Chart Total starts at zero before either side resolves a note"
    );

    session->begin_frame();
    require(session->update(1'000.0), session->error());
    require(session->press(0U, 1'000.0), session->error());
    require(
        session->player_note_multiplier() == 2.0
            && session->opponent_note_multiplier() == 1.0,
        "streaming fractional player multiplier normalizes to logical count"
    );
    require(
        session->summary().score == 700
            && session->summary().combo == 2U
            && session->summary().marvelous == 2U
            && session->summary().chart_total == 2U
            && std::abs(session->summary().judged_notes - 2.0) < 0.000001,
        "streaming score/combo/rating/chart-total/judged counts share normalized multiplier"
    );
    const auto first_events = session->frame_events();
    require(first_events.size() >= 2U, "streaming same-time event and note are emitted");
    require(
        first_events[0].type == pulseforge::GameplayEventType::chart_event,
        "streaming same-time chart event precedes note judgment"
    );
    const auto hit = std::find_if(
        first_events.begin(),
        first_events.end(),
        [](const pulseforge::StreamingGameplayEvent& event) {
            return event.type == pulseforge::GameplayEventType::note_hit;
        }
    );
    require(
        hit != first_events.end()
            && hit->occurrence_count == 1U
            && hit->logical_occurrence_count == 2U,
        "streaming keeps physical/source and logical occurrence counts separate"
    );

    session->begin_frame();
    require(session->update(1'100.0), session->error());
    require(
        session->opponent_note_multiplier() == 3.0,
        "streaming opponent-side event updates opponent multiplier"
    );
    const auto opponent = std::find_if(
        session->frame_events().begin(),
        session->frame_events().end(),
        [](const pulseforge::StreamingGameplayEvent& event) {
            return event.type == pulseforge::GameplayEventType::opponent_hit;
        }
    );
    require(
        opponent != session->frame_events().end()
            && opponent->logical_occurrence_count == 3U
            && session->summary().chart_total == 5U,
        "streaming opponent event and Chart Total carry opponent logical multiplier"
    );

    pulseforge::PackedChartData hold_chart;
    hold_chart.key_count = 4U;
    hold_chart.kinds = {"normal"};
    hold_chart.notes = {
        {100'000, 500'000U, 0U, pulseforge::PackedNoteOwner::player, 0U, 0U},
    };
    const auto hold_reader = write_and_open(
        directory / "note-multiplier-hold.pfc",
        hold_chart,
        1U
    );
    std::vector<pulseforge::ChartEvent> hold_events{
        {0.0, "Change Note Multiplier", "2", "player", {}},
    };
    auto hold_session = pulseforge::StreamingGameplaySession::create(
        hold_reader,
        settings,
        options,
        {{0.0, 120.0, 4U, 4U}},
        std::move(hold_events),
        &error
    );
    require(hold_session.has_value(), error);
    require(hold_session->update(100.0), hold_session->error());
    require(hold_session->press(0U, 100.0), hold_session->error());
    require(hold_session->update(400.0), hold_session->error());
    require(
        hold_session->summary().hold_ticks == 6U
            && hold_session->summary().score == 760,
        "streaming sustain ticks and score use logical multiplier"
    );
    hold_session->begin_frame();
    require(hold_session->release(0U, 400.0), hold_session->error());
    require(
        hold_session->summary().hold_drops == 2U
            && hold_session->summary().misses == 2U
            && hold_session->summary().chart_total == 2U,
        "streaming sustain ticks/drops do not double-count Chart Total"
    );
    const auto drop = std::find_if(
        hold_session->frame_events().begin(),
        hold_session->frame_events().end(),
        [](const pulseforge::StreamingGameplayEvent& event) {
            return event.type == pulseforge::GameplayEventType::hold_drop;
        }
    );
    require(
        drop != hold_session->frame_events().end()
            && drop->logical_occurrence_count == 2U,
        "streaming hold-drop event carries logical multiplier"
    );

    // PULSEFORGE_P1_1_19_STREAMING_EYE_OF_GOD_MULTIPLIER_V1
    constexpr std::uint64_t eye_of_god_multiplier = 21'447'891U;
    pulseforge::PackedChartData huge_chart;
    huge_chart.key_count = 4U;
    huge_chart.kinds = {"normal"};
    huge_chart.notes = {
        {100'000, 0U, 0U, pulseforge::PackedNoteOwner::player, 0U, 0U},
    };
    const auto huge_reader = write_and_open(
        directory / "eye-of-god-note-multiplier.pfc",
        huge_chart,
        1U
    );
    std::vector<pulseforge::ChartEvent> huge_events{
        {0.0, "Change Note Multiplier", "21447891", "player", {}},
    };
    auto huge_session = pulseforge::StreamingGameplaySession::create(
        huge_reader,
        settings,
        options,
        {{0.0, 120.0, 4U, 4U}},
        std::move(huge_events),
        &error
    );
    require(huge_session.has_value(), error);
    require(huge_session->update(100.0), huge_session->error());
    require(huge_session->press(0U, 100.0), huge_session->error());
    require(
        huge_session->player_note_multiplier()
                == static_cast<double>(eye_of_god_multiplier)
            && huge_session->summary().combo == eye_of_god_multiplier
            && huge_session->summary().marvelous == eye_of_god_multiplier
            && huge_session->summary().chart_total == eye_of_god_multiplier
            && huge_session->summary().score == 7'506'761'850LL,
        "streaming path preserves Eye of God multiplier in Chart Total above one million"
    );
    const auto huge_hit = std::find_if(
        huge_session->frame_events().begin(),
        huge_session->frame_events().end(),
        [](const pulseforge::StreamingGameplayEvent& event) {
            return event.type == pulseforge::GameplayEventType::note_hit;
        }
    );
    require(
        huge_hit != huge_session->frame_events().end()
            && huge_hit->occurrence_count == 1U
            && huge_hit->logical_occurrence_count == eye_of_god_multiplier,
        "streaming physical note remains one while logical count stays unbounded"
    );
}


void test_p1_5_0f_adversarial_total_parity(
    const std::filesystem::path& directory
) {
    // PULSEFORGE_P1_5_0F_ADVERSARIAL_TOTAL_PARITY_TEST_V1
    // One deliberately hostile fixture combines: two BPM regions, mirror +
    // deterministic lane randomization, independent P1/P2 Dynamic Mania,
    // independent Note Multipliers, player/opponent/Third Strum owners,
    // custom NoteTypes, a sustain, an unknown Lua-facing chart event and the
    // materialized/PFC1 schedulers. No part of the fixture is expanded from a
    // procedural source and both backends are compared after every phase.
    pulseforge::Chart materialized_chart;
    materialized_chart.title = "P1.5.0f adversarial parity";
    materialized_chart.difficulty = "adversarial";
    materialized_chart.key_count = 6U;
    materialized_chart.secondary_opponent_enabled = true;
    materialized_chart.tempos = {
        {0.0, 120.0, 4U, 4U},
        {2'000.0, 180.0, 4U, 4U},
    };
    materialized_chart.events = {
        {0.0, "Change P1 Mania", "3", "false", {}},
        {0.0, "Change P2 Mania", "2", "true", {}},
        {0.0, "Change Note Multiplier", "2", "player", {}},
        {0.0, "Change Note Multiplier", "3", "opponent", {}},
        {1'300.0, "Change P1 Mania", "2", "true", {}},
        {1'650.0, "Adversarial Lua Probe", "alpha", "beta", {}},
        {2'050.0, "Change P2 Mania", "3", "false", {}},
        {2'100.0, "Change P1 Mania", "4", "false", {}},
    };
    materialized_chart.notes = {
        {500.0, 0.0, 0U, pulseforge::NoteOwner::opponent, "normal"},
        {600.0, 400.0, 5U, pulseforge::NoteOwner::secondary_opponent, "Third Strum"},
        {1'000.0, 0.0, 0U, pulseforge::NoteOwner::player, "the note"},
        {1'400.0, 0.0, 5U, pulseforge::NoteOwner::player, "normal"},
        {1'800.0, 0.0, 1U, pulseforge::NoteOwner::player, "Hurt Note"},
        {2'200.0, 600.0, 4U, pulseforge::NoteOwner::player, "normal"},
        {2'600.0, 0.0, 3U, pulseforge::NoteOwner::opponent, "normal"},
    };
    materialized_chart.normalize();

    pulseforge::PackedChartData packed_chart;
    packed_chart.key_count = 6U;
    packed_chart.kinds = {"normal", "Third Strum", "the note", "Hurt Note"};
    packed_chart.notes = {
        {500'000, 0U, 0U, pulseforge::PackedNoteOwner::opponent, 0U, 0U},
        // PFC1 has a binary AI/player owner bit. Third Strum identity is kept
        // by its exact kind and promoted by the application runtime to the
        // canonical secondary_opponent role; gameplay scheduling remains P2.
        {600'000, 400'000U, 5U, pulseforge::PackedNoteOwner::opponent, 0U, 1U},
        {1'000'000, 0U, 0U, pulseforge::PackedNoteOwner::player, 0U, 2U},
        {1'400'000, 0U, 5U, pulseforge::PackedNoteOwner::player, 0U, 0U},
        {1'800'000, 0U, 1U, pulseforge::PackedNoteOwner::player, 0U, 3U},
        {2'200'000, 600'000U, 4U, pulseforge::PackedNoteOwner::player, 0U, 0U},
        {2'600'000, 0U, 3U, pulseforge::PackedNoteOwner::opponent, 0U, 0U},
    };
    const auto reader = write_and_open(
        directory / "p1-5-0f-adversarial-total-parity.pfc",
        packed_chart,
        3U
    );

    pulseforge::GameplaySettings settings;
    settings.no_fail = true;
    settings.mirror = true;
    settings.randomize_lanes = true;
    settings.random_seed = 0x5F5F5F5FU;

    pulseforge::GameplaySession materialized(materialized_chart, settings);
    pulseforge::StreamingGameplayOptions stream_options;
    stream_options.max_window_notes = 24U;
    stream_options.max_active_holds = 4U;
    stream_options.max_events_per_frame = 64U;
    stream_options.max_recorded_inputs = 32U;
    stream_options.max_explicit_chunk_notes = 4U;
    std::string error;
    auto streaming = pulseforge::StreamingGameplaySession::create(
        reader,
        settings,
        stream_options,
        materialized_chart.tempos,
        materialized_chart.events,
        &error
    );
    require(streaming.has_value(), error);

    pulseforge::NoteKindRuntimeBehavior bullet_behavior;
    bullet_behavior.miss_health = 0.6;
    require(
        materialized.set_note_kind_behavior("the note", bullet_behavior)
            && streaming->set_note_kind_behavior("the note", bullet_behavior),
        "adversarial fixture installs the real the-note miss-health behavior"
    );
    pulseforge::NoteKindRuntimeBehavior hurt_behavior;
    hurt_behavior.miss_health = 0.3;
    hurt_behavior.hit_causes_miss = true;
    hurt_behavior.sustain_miss_health = 0.1;
    hurt_behavior.sustain_hit_causes_miss = true;
    hurt_behavior.sustain_enabled = true;
    hurt_behavior.sustain_inherits_type = true;
    require(
        materialized.set_note_kind_behavior("Hurt Note", hurt_behavior)
            && streaming->set_note_kind_behavior("Hurt Note", hurt_behavior),
        "adversarial fixture installs typed Hurt Note behavior"
    );

    const auto event_weight = [](const auto events, const pulseforge::GameplayEventType type) {
        std::uint64_t total = 0U;
        for (const auto& event : events) {
            if (event.type != type) continue;
            const auto amount = event.logical_occurrence_count;
            const auto maximum = std::numeric_limits<std::uint64_t>::max();
            total = amount > maximum - total ? maximum : total + amount;
        }
        return total;
    };

    const auto compare = [&](const std::string_view stage) {
        const auto& left = materialized.summary();
        const auto& right = streaming->summary();
        require(left.score == right.score, std::string(stage) + ": score parity");
        require(left.combo == right.combo, std::string(stage) + ": combo parity");
        require(left.max_combo == right.max_combo, std::string(stage) + ": max combo parity");
        require(left.marvelous == right.marvelous, std::string(stage) + ": marvelous parity");
        require(left.sick == right.sick, std::string(stage) + ": sick parity");
        require(left.good == right.good, std::string(stage) + ": good parity");
        require(left.bad == right.bad, std::string(stage) + ": bad parity");
        require(left.misses == right.misses, std::string(stage) + ": miss parity");
        require(left.hold_ticks == right.hold_ticks, std::string(stage) + ": hold tick parity");
        require(left.hold_drops == right.hold_drops, std::string(stage) + ": hold drop parity");
        require(left.chart_total == right.chart_total, std::string(stage) + ": Chart Total parity");
        require(
            std::abs(left.weighted_hits - right.weighted_hits) < 0.000001,
            std::string(stage) + ": weighted-hit parity"
        );
        require(
            std::abs(left.judged_notes - right.judged_notes) < 0.000001,
            std::string(stage) + ": judged-note parity"
        );
        require(
            std::abs(left.health - right.health) < 0.000001,
            std::string(stage) + ": health parity"
        );
        require(left.failed == right.failed, std::string(stage) + ": failed parity");
        require(
            materialized.player_key_count() == streaming->player_key_count()
                && materialized.opponent_key_count() == streaming->opponent_key_count(),
            std::string(stage) + ": dynamic-mania key-count parity"
        );
        require(
            materialized.player_note_multiplier() == streaming->player_note_multiplier()
                && materialized.opponent_note_multiplier()
                    == streaming->opponent_note_multiplier(),
            std::string(stage) + ": note-multiplier parity"
        );
        for (std::uint16_t source = 0U; source < 6U; ++source) {
            require(
                materialized.display_lane(pulseforge::NoteOwner::player, source)
                    == streaming->display_lane(pulseforge::NoteOwner::player, source),
                std::string(stage) + ": player lane-projection parity"
            );
            require(
                materialized.display_lane(pulseforge::NoteOwner::opponent, source)
                    == streaming->display_lane(pulseforge::NoteOwner::opponent, source),
                std::string(stage) + ": opponent lane-projection parity"
            );
            require(
                materialized.display_lane(
                    pulseforge::NoteOwner::secondary_opponent,
                    source
                ) == streaming->display_lane(pulseforge::NoteOwner::opponent, source),
                std::string(stage) + ": Third Strum shares exact P2 projection"
            );
        }
        for (const auto type : {
                 pulseforge::GameplayEventType::note_hit,
                 pulseforge::GameplayEventType::note_miss,
                 pulseforge::GameplayEventType::opponent_hit,
                 pulseforge::GameplayEventType::hold_tick,
                 pulseforge::GameplayEventType::hold_drop,
             }) {
            require(
                event_weight(materialized.frame_events(), type)
                    == event_weight(streaming->frame_events(), type),
                std::string(stage) + ": logical event-weight parity"
            );
        }
    };

    require(materialized.summary().chart_total == 0U, "adversarial Chart Total starts at zero");
    require(
        materialized_chart.notes[1].owner == pulseforge::NoteOwner::secondary_opponent,
        "adversarial materialized Third Strum keeps its canonical secondary_opponent owner"
    );

    materialized.begin_frame();
    streaming->begin_frame();
    materialized.update(650.0);
    require(streaming->update(650.0), streaming->error());
    compare("initial AI/Third Strum phase");
    require(
        materialized.summary().chart_total == 6U,
        "opponent and Third Strum heads both use the P2 x3 logical multiplier"
    );

    materialized.begin_frame();
    streaming->begin_frame();
    materialized.update(1'250.0);
    require(streaming->update(1'250.0), streaming->error());
    compare("real the-note miss phase");

    materialized.begin_frame();
    streaming->begin_frame();
    materialized.update(1'400.0);
    require(streaming->update(1'400.0), streaming->error());
    const auto lane_1400 = materialized.display_lane(pulseforge::NoteOwner::player, 5U);
    require(
        lane_1400 == streaming->display_lane(pulseforge::NoteOwner::player, 5U),
        "adversarial high source lane has exact PFC1 projection before hit"
    );
    materialized.press(lane_1400, 1'400.0);
    require(streaming->press(lane_1400, 1'400.0), streaming->error());
    compare("dynamic 6K-to-2K projected hit");

    materialized.begin_frame();
    streaming->begin_frame();
    materialized.update(1'800.0);
    require(streaming->update(1'800.0), streaming->error());
    const auto hurt_lane = materialized.display_lane(pulseforge::NoteOwner::player, 1U);
    materialized.press(hurt_lane, 1'800.0);
    require(streaming->press(hurt_lane, 1'800.0), streaming->error());
    compare("typed Hurt Note hazard phase");

    materialized.begin_frame();
    streaming->begin_frame();
    materialized.update(2'200.0);
    require(streaming->update(2'200.0), streaming->error());
    const auto hold_lane = materialized.display_lane(pulseforge::NoteOwner::player, 4U);
    materialized.press(hold_lane, 2'200.0);
    require(streaming->press(hold_lane, 2'200.0), streaming->error());
    compare("post-BPM-change sustain head");

    materialized.begin_frame();
    streaming->begin_frame();
    materialized.update(2'500.0);
    require(streaming->update(2'500.0), streaming->error());
    compare("post-BPM-change sustain ticks");

    materialized.begin_frame();
    streaming->begin_frame();
    materialized.update(2'800.0);
    require(streaming->update(2'800.0), streaming->error());
    materialized.release(hold_lane, 2'800.0);
    require(streaming->release(hold_lane, 2'800.0), streaming->error());
    compare("sustain tail and final opponent phase");

    materialized.begin_frame();
    streaming->begin_frame();
    materialized.finish_song(4'000.0);
    require(streaming->finish_song(4'000.0), streaming->error());
    compare("authoritative media end");
    require(
        materialized.complete() && streaming->complete(),
        "adversarial fixture completes identically on materialized and PFC1"
    );
    require(
        streaming->memory_stats().window_notes <= stream_options.max_window_notes
            && streaming->memory_stats().active_holds <= stream_options.max_active_holds,
        "adversarial PFC1 fixture remains inside configured bounded runtime state"
    );
}

void test_p1_5_0f_chart_total_uint64_saturation(
    const std::filesystem::path& directory
) {
    // PULSEFORGE_P1_5_0F_CHART_TOTAL_UINT64_SATURATION_TEST_V1
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    constexpr std::string_view maximum_text = "18446744073709551615";

    pulseforge::Chart chart;
    chart.key_count = 4U;
    chart.tempos = {{0.0, 120.0, 4U, 4U}};
    chart.events = {
        {0.0, "Change Note Multiplier", std::string(maximum_text), "opponent", {}},
    };
    chart.notes = {
        {100.0, 0.0, 0U, pulseforge::NoteOwner::opponent, "normal"},
        {200.0, 0.0, 1U, pulseforge::NoteOwner::secondary_opponent, "Third Strum"},
    };
    chart.secondary_opponent_enabled = true;
    chart.normalize();

    pulseforge::GameplaySettings settings;
    settings.no_fail = true;
    pulseforge::GameplaySession materialized(chart, settings);
    materialized.update(300.0);
    require(
        materialized.summary().chart_total == maximum,
        "materialized Chart Total saturates at uint64 max instead of wrapping"
    );
    materialized.update(1'000.0);
    require(
        materialized.summary().chart_total == maximum,
        "materialized Chart Total remains saturated after later updates"
    );

    pulseforge::PackedChartData packed;
    packed.key_count = 4U;
    packed.kinds = {"normal", "Third Strum"};
    packed.notes = {
        {100'000, 0U, 0U, pulseforge::PackedNoteOwner::opponent, 0U, 0U},
        {200'000, 0U, 1U, pulseforge::PackedNoteOwner::opponent, 0U, 1U},
    };
    const auto reader = write_and_open(
        directory / "p1-5-0f-chart-total-saturation.pfc",
        packed,
        2U
    );
    pulseforge::StreamingGameplayOptions options;
    options.max_window_notes = 8U;
    options.max_active_holds = 2U;
    options.max_events_per_frame = 8U;
    options.max_recorded_inputs = 4U;
    options.max_explicit_chunk_notes = 2U;
    std::string error;
    auto streaming = pulseforge::StreamingGameplaySession::create(
        reader,
        settings,
        options,
        chart.tempos,
        chart.events,
        &error
    );
    require(streaming.has_value(), error);
    require(streaming->update(300.0), streaming->error());
    require(
        streaming->summary().chart_total == maximum,
        "PFC1 Chart Total saturates at uint64 max instead of wrapping"
    );
    require(streaming->update(1'000.0), streaming->error());
    require(
        streaming->summary().chart_total == maximum
            && streaming->total_resolved_notes() == 2U,
        "PFC1 saturation preserves two physical heads without logical overflow"
    );
}

}  // namespace

int main() {
    try {
        const TemporaryDirectory directory;
        test_real_input_holds_and_score(directory.path());
        test_explicit_catchup_budget(directory.path());
        test_large_explicit_chunk_uses_decoded_byte_budget(directory.path());
        test_event_keeps_visual_note_after_window_compaction(directory.path());
        test_trillion_note_pattern_constant_memory(directory.path());
        test_dense_autoplay_sustain_overflow(directory.path());
        test_default_active_holds_are_allocator_limited(directory.path());
        test_pattern_sustain_aggregation_contracts(directory.path());
        test_media_end_drains_dense_exact_stack(directory.path());
        test_opponent_sustain_keeps_unconsumed_tail(directory.path());
        test_dynamic_lane_topology(directory.path());
        test_note_kind_runtime_behavior(directory.path());
        test_declarative_sustain_policy(directory.path());
        test_real_notetype_materialized_streaming_parity(directory.path());
        test_note_multiplier_parity(directory.path());
        test_p1_5_0f_adversarial_total_parity(directory.path());
        test_p1_5_0f_chart_total_uint64_saturation(directory.path());
        std::cout << "streaming gameplay tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "streaming gameplay test failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
