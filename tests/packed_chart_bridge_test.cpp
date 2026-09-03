#include "pulseforge/packed_chart_bridge.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <span>
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
            const auto name = std::string("pulseforge-packed-bridge-test-")
                + std::to_string(nonce) + "-" + std::to_string(attempt);
            path_ = root / name;
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                owned_ = true;
                return;
            }
            if (error) {
                throw std::runtime_error(
                    "failed to create packed bridge test directory"
                );
            }
        }
        throw std::runtime_error("failed to reserve bridge test directory");
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

[[nodiscard]] pulseforge::Chart make_chart() {
    pulseforge::Chart chart;
    chart.key_count = 4U;
    chart.tempos = {{0.0, 120.0, 4U, 4U}};
    chart.notes = {
        {
            -0.5,
            0.0,
            0U,
            pulseforge::NoteOwner::player,
            "normal",
            0U,
        },
        {
            1.0,
            0.25,
            2U,
            pulseforge::NoteOwner::opponent,
            "mine",
            0U,
        },
        {
            1.25,
            2.5,
            3U,
            pulseforge::NoteOwner::player,
            "normal",
            0U,
        },
    };
    return chart;
}

void require_notes_equal(
    const pulseforge::Chart& left,
    const pulseforge::Chart& right
) {
    require(left.key_count == right.key_count, "chart key count round-trips");
    require(left.notes.size() == right.notes.size(), "chart note count round-trips");
    for (std::size_t index = 0U; index < left.notes.size(); ++index) {
        const auto& expected = left.notes[index];
        const auto& actual = right.notes[index];
        require(actual.time_ms == expected.time_ms, "note time round-trips exactly");
        require(
            actual.duration_ms == expected.duration_ms,
            "note duration round-trips exactly"
        );
        require(actual.lane == expected.lane, "note lane round-trips");
        require(actual.owner == expected.owner, "note owner round-trips");
        require(actual.kind == expected.kind, "note kind round-trips");
        require(actual.payload_id == 0U, "bridge creates canonical empty payloads");
    }
}

[[nodiscard]] pulseforge::PackedChartReader write_and_open(
    const std::filesystem::path& path,
    const pulseforge::PackedChartData& chart,
    const std::uint32_t notes_per_chunk = 2U
) {
    pulseforge::PackedChartWriteOptions write_options;
    write_options.max_notes_per_chunk = notes_per_chunk;
    std::string error;
    require(
        pulseforge::write_packed_chart(path, chart, write_options, &error),
        std::string("failed to write bridge fixture: ") + error
    );
    auto reader = pulseforge::PackedChartReader::open(path, &error);
    require(
        reader.has_value(),
        std::string("failed to open bridge fixture: ") + error
    );
    return std::move(*reader);
}

void test_round_trip_and_inspection(const std::filesystem::path& directory) {
    const auto source = make_chart();
    const std::vector<std::uint16_t> source_flags{0U, 7U, 65'535U};
    pulseforge::ChartToPackedOptions pack_options;
    pack_options.note_flags = std::span<const std::uint16_t>(source_flags);
    const auto packed = pulseforge::convert_chart_to_packed(
        source,
        pack_options
    );
    require(static_cast<bool>(packed), packed.error);
    require(packed.chart.patterns.empty(), "Chart conversion creates no patterns");
    require(
        packed.chart.kinds == std::vector<std::string>{"normal", "mine"},
        "kind dictionary uses stable first occurrence order"
    );
    require(packed.chart.notes[0U].time_us == -500, "negative time packs exactly");
    require(
        packed.chart.notes[1U].duration_us == 250U,
        "duration packs exactly"
    );
    require(
        packed.statistics.nonzero_flag_note_count == 2U,
        "packing statistics count flags"
    );

    const auto path = directory / "bridge-round-trip.pfc1";
    const auto reader = write_and_open(path, packed.chart);

    const auto metadata = pulseforge::inspect_packed_chart(reader);
    require(static_cast<bool>(metadata), metadata.error);
    require(
        !metadata.statistics.explicit_notes_scanned,
        "metadata inspection does not decode chunks"
    );
    require(
        metadata.statistics.first_explicit_time_us == -500
            && metadata.statistics.last_explicit_time_us == 1'250,
        "metadata inspection reports time extent"
    );

    pulseforge::PackedChartInspectionOptions scan_options;
    scan_options.scan_explicit_notes = true;
    scan_options.max_explicit_notes_to_scan = source.notes.size();
    const auto scanned = pulseforge::inspect_packed_chart(reader, scan_options);
    require(static_cast<bool>(scanned), scanned.error);
    require(scanned.statistics.explicit_notes_scanned, "scan is marked complete");
    require(scanned.statistics.player_note_count == 2U, "scan counts player notes");
    require(
        scanned.statistics.opponent_note_count == 1U,
        "scan counts opponent notes"
    );
    require(scanned.statistics.hold_note_count == 2U, "scan counts hold notes");
    require(
        scanned.statistics.nonzero_flag_note_count == 2U,
        "scan counts non-zero flags"
    );

    const auto unpacked = pulseforge::convert_packed_to_chart(reader);
    require(static_cast<bool>(unpacked), unpacked.error);
    require_notes_equal(source, unpacked.chart);
    require(unpacked.note_flags == source_flags, "flag sidecar round-trips exactly");
    require(
        unpacked.chart.tempos.size() == 1U,
        "unpacked Chart receives a valid default tempo"
    );

    pulseforge::ChartToPackedOptions repack_options;
    repack_options.note_flags = std::span<const std::uint16_t>(
        unpacked.note_flags
    );
    const auto repacked = pulseforge::convert_chart_to_packed(
        unpacked.chart,
        repack_options
    );
    require(static_cast<bool>(repacked), repacked.error);
    require(repacked.chart.kinds == packed.chart.kinds, "dictionary re-packs exactly");
    require(repacked.chart.notes == packed.chart.notes, "packed notes re-pack exactly");

    pulseforge::PackedToChartOptions small_unpack_budget;
    small_unpack_budget.max_notes = source.notes.size() - 1U;
    require(
        !static_cast<bool>(pulseforge::convert_packed_to_chart(
            reader,
            small_unpack_budget
        )),
        "unpack rejects an over-budget explicit note count"
    );

    scan_options.max_explicit_notes_to_scan = source.notes.size() - 1U;
    require(
        !static_cast<bool>(pulseforge::inspect_packed_chart(
            reader,
            scan_options
        )),
        "inspection rejects an over-budget payload scan"
    );
}

void test_chart_side_limits() {
    auto chart = make_chart();
    pulseforge::ChartToPackedOptions options;
    options.max_notes = chart.notes.size() - 1U;
    require(
        !static_cast<bool>(pulseforge::convert_chart_to_packed(chart, options)),
        "packing rejects an over-budget chart"
    );

    options = {};
    const std::vector<std::uint16_t> wrong_flags{1U};
    options.note_flags = std::span<const std::uint16_t>(wrong_flags);
    require(
        !static_cast<bool>(pulseforge::convert_chart_to_packed(chart, options)),
        "packing rejects a misaligned flag sidecar"
    );

    options = {};
    chart.notes[0U].time_ms = 0.0005;
    require(
        !static_cast<bool>(pulseforge::convert_chart_to_packed(chart, options)),
        "packing rejects sub-microsecond precision loss"
    );

    chart = make_chart();
    chart.note_payloads.push_back("{\"custom\":true}");
    chart.notes[0U].payload_id = 1U;
    const auto rejected_payload = pulseforge::convert_chart_to_packed(chart);
    require(
        !static_cast<bool>(rejected_payload),
        "packing rejects a non-representable payload by default"
    );

    options = {};
    options.payload_policy = pulseforge::ChartPayloadPolicy::discard;
    const auto discarded_payload = pulseforge::convert_chart_to_packed(
        chart,
        options
    );
    require(static_cast<bool>(discarded_payload), discarded_payload.error);
    require(
        discarded_payload.statistics.discarded_payload_note_count == 1U,
        "explicit payload discard is reported in statistics"
    );
}

void test_third_strum_round_trip(
    const std::filesystem::path& directory
) {
    // PULSEFORGE_P1_4_0_PACKED_BRIDGE_THIRD_STRUM_TEST_V1
    auto chart = make_chart();
    chart.notes.push_back({
        2.0,
        0.75,
        1U,
        pulseforge::NoteOwner::secondary_opponent,
        "Third Strum",
        0U,
    });
    const auto packed = pulseforge::convert_chart_to_packed(chart);
    require(static_cast<bool>(packed), packed.error);
    require(
        packed.chart.notes.back().owner == pulseforge::PackedNoteOwner::opponent,
        "PFC1 bridge stores Third Strum as physical AI owner"
    );
    require(
        packed.chart.kinds[packed.chart.notes.back().kind_id] == "Third Strum",
        "PFC1 bridge preserves Third Strum kind identity"
    );

    const auto reader = write_and_open(
        directory / "bridge-third-strum.pfc1",
        packed.chart
    );
    const auto unpacked = pulseforge::convert_packed_to_chart(reader);
    require(static_cast<bool>(unpacked), unpacked.error);
    require(
        unpacked.chart.notes.back().owner
            == pulseforge::NoteOwner::secondary_opponent
            && unpacked.chart.notes.back().kind == "Third Strum"
            && unpacked.chart.secondary_opponent_enabled,
        "PFC1 bridge reconstructs the secondary-opponent owner from kind"
    );
}

void test_patterns_are_never_expanded(
    const std::filesystem::path& directory
) {
    const auto source = make_chart();
    const std::vector<std::uint16_t> flags{1U, 2U, 3U};
    pulseforge::ChartToPackedOptions options;
    options.note_flags = std::span<const std::uint16_t>(flags);
    auto packed = pulseforge::convert_chart_to_packed(source, options);
    require(static_cast<bool>(packed), packed.error);

    pulseforge::PatternRun pattern;
    pattern.start_us = 10'000;
    pattern.interval_us = 100U;
    pattern.count = trillion;
    pattern.lane_pattern = {0U, 1U};
    pattern.owner = pulseforge::PackedNoteOwner::opponent;
    pattern.flags = 9U;
    pattern.kind_id = 0U;
    packed.chart.patterns.push_back(std::move(pattern));

    const auto path = directory / "bridge-patterns.pfc1";
    const auto reader = write_and_open(path, packed.chart);
    const auto inspection = pulseforge::inspect_packed_chart(reader);
    require(static_cast<bool>(inspection), inspection.error);
    require(
        inspection.statistics.pattern_run_count == 1U
            && inspection.statistics.pattern_note_count == trillion,
        "inspection reports compact pattern counts without expansion"
    );

    const auto rejected = pulseforge::convert_packed_to_chart(reader);
    require(
        !static_cast<bool>(rejected)
            && rejected.error.find("pattern") != std::string::npos,
        "default conversion explicitly rejects pattern runs"
    );

    pulseforge::PackedToChartOptions ignore_options;
    ignore_options.pattern_policy = pulseforge::PackedPatternPolicy::ignore;
    const auto explicit_only = pulseforge::convert_packed_to_chart(
        reader,
        ignore_options
    );
    require(static_cast<bool>(explicit_only), explicit_only.error);
    require(
        explicit_only.chart.notes.size() == source.notes.size(),
        "explicit ignore policy still never expands patterns"
    );
    require(
        explicit_only.statistics.logical_note_count
            == trillion + source.notes.size(),
        "explicit-only result retains logical-count statistics"
    );
}

void test_unrepresentable_packed_time(
    const std::filesystem::path& directory
) {
    pulseforge::PackedChartData packed;
    packed.key_count = 4U;
    packed.kinds = {"normal"};
    packed.notes.push_back({
        9'007'199'254'740'993LL,
        0U,
        0U,
        pulseforge::PackedNoteOwner::player,
        0U,
        0U,
    });
    const auto reader = write_and_open(
        directory / "bridge-unrepresentable-time.pfc1",
        packed,
        1U
    );
    require(
        !static_cast<bool>(pulseforge::convert_packed_to_chart(reader)),
        "unpacking rejects lossy double timestamp conversion"
    );
}

}  // namespace

int main() {
    try {
        const TemporaryDirectory temporary;
        test_round_trip_and_inspection(temporary.path());
        test_chart_side_limits();
        test_third_strum_round_trip(temporary.path());
        test_patterns_are_never_expanded(temporary.path());
        test_unrepresentable_packed_time(temporary.path());
        std::cout << "Packed chart bridge tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "Packed chart bridge tests failed: "
                  << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
