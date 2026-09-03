#include "pulseforge/packed_chart.hpp"
#include "pulseforge/packed_chart_stream.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint64_t trillion = 1'000'000'000'000ULL;

struct VisitCapture {
    std::array<pulseforge::PackedNote, 8U> notes{};
    std::size_t count{};
};

void capture_note(void* const context, const pulseforge::PackedNote& note) noexcept {
    auto& capture = *static_cast<VisitCapture*>(context);
    if (capture.count < capture.notes.size()) {
        capture.notes[capture.count] = note;
    }
    ++capture.count;
}

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
            const auto name = std::string("pulseforge-packed-chart-test-")
                + std::to_string(nonce) + "-" + std::to_string(attempt);
            path_ = root / name;
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                owned_ = true;
                return;
            }
            if (error) {
                throw std::runtime_error(
                    "failed to create packed chart test directory"
                );
            }
        }
        throw std::runtime_error("failed to reserve packed chart test directory");
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

[[nodiscard]] pulseforge::PackedChartData make_chart() {
    pulseforge::PackedChartData chart;
    chart.key_count = 4U;
    chart.kinds = {"normal", "mine", "alt"};
    chart.notes = {
        {0, 0U, 0U, pulseforge::PackedNoteOwner::player, 0U, 0U},
        {1'000, 0U, 1U, pulseforge::PackedNoteOwner::opponent, 1U, 1U},
        {1'000, 250U, 2U, pulseforge::PackedNoteOwner::player, 2U, 2U},
        {2'000, 500U, 3U, pulseforge::PackedNoteOwner::player, 4U, 0U},
        {3'000, 0U, 0U, pulseforge::PackedNoteOwner::opponent, 8U, 1U},
    };

    pulseforge::PatternRun pattern;
    pattern.start_us = -500;
    pattern.interval_us = 100U;
    pattern.count = trillion;
    pattern.duration_us = 25U;
    pattern.lane_pattern = {0U, 3U, 1U};
    pattern.owner = pulseforge::PackedNoteOwner::player;
    pattern.flags = 16U;
    pattern.kind_id = 2U;
    chart.patterns.push_back(std::move(pattern));
    return chart;
}

void copy_fixture(
    const std::filesystem::path& source,
    const std::filesystem::path& destination
) {
    std::error_code error;
    const auto copied = std::filesystem::copy_file(
        source,
        destination,
        std::filesystem::copy_options::none,
        error
    );
    if (!copied || error) {
        throw std::runtime_error("failed to copy packed chart test fixture");
    }
}

void flip_byte(
    const std::filesystem::path& path,
    const std::uint64_t offset
) {
    if (offset > static_cast<std::uint64_t>(
            std::numeric_limits<std::streamoff>::max()
        )) {
        throw std::runtime_error("packed chart test offset is not seekable");
    }
    const auto stream_offset = static_cast<std::streamoff>(offset);
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) {
        throw std::runtime_error("failed to open packed chart test fixture");
    }
    file.seekg(stream_offset, std::ios::beg);
    char value{};
    file.read(&value, 1);
    if (file.gcount() != 1) {
        throw std::runtime_error("failed to read packed chart test byte");
    }
    const auto changed = static_cast<char>(
        static_cast<unsigned char>(value) ^ 0x5AU
    );
    file.clear();
    file.seekp(stream_offset, std::ios::beg);
    file.write(&changed, 1);
    file.flush();
    if (!file) {
        throw std::runtime_error("failed to corrupt packed chart test byte");
    }
}

void test_pattern_run_constant_space() {
    const auto chart = make_chart();
    const auto& pattern = chart.patterns.front();
    const auto last = pattern.note_at(trillion - 1U);
    require(last.has_value(), "a trillion-note PatternRun supports random access");
    const auto expected_time = pattern.start_us + static_cast<std::int64_t>(
        (trillion - 1U) * pattern.interval_us
    );
    require(last->time_us == expected_time, "PatternRun computes exact time");
    require(
        last->lane == pattern.lane_pattern[static_cast<std::size_t>(
            (trillion - 1U)
                % static_cast<std::uint64_t>(pattern.lane_pattern.size())
        )],
        "PatternRun cycles lanes without expansion"
    );
    require(
        !pattern.note_at(trillion).has_value(),
        "PatternRun rejects an out-of-range index"
    );

    pulseforge::PatternRun overflowing;
    overflowing.start_us = std::numeric_limits<std::int64_t>::max() - 1;
    overflowing.interval_us = 2U;
    overflowing.count = 2U;
    overflowing.lane_pattern = {0U};
    require(
        !overflowing.note_at(1U).has_value(),
        "PatternRun rejects signed timestamp overflow"
    );
}

void test_round_trip_and_ranges(const std::filesystem::path& directory) {
    const auto chart = make_chart();
    const auto path = directory / "round-trip.pfc1";
    pulseforge::PackedChartWriteOptions options;
    options.max_notes_per_chunk = 2U;
    std::string error;
    require(
        pulseforge::write_packed_chart(path, chart, options, &error),
        std::string("packed chart write failed: ") + error
    );
    require(
        std::filesystem::file_size(path) < 100U * 1024U,
        "a trillion-note pattern remains constant-space on disk"
    );

    const auto opened = pulseforge::PackedChartReader::open(path, &error);
    require(
        opened.has_value(),
        std::string("packed chart open failed: ") + error
    );
    const auto& reader = *opened;
    require(reader.key_count() == chart.key_count, "key count round-trips");
    require(
        reader.explicit_note_count() == chart.notes.size(),
        "explicit note count round-trips"
    );
    require(
        reader.logical_note_count() == trillion + chart.notes.size(),
        "logical note count includes compact patterns"
    );
    require(reader.chunk_count() == 3U, "chunk count uses configured boundary");
    require(
        std::vector<std::string>(reader.kinds().begin(), reader.kinds().end())
            == chart.kinds,
        "kind dictionary round-trips"
    );
    require(
        std::vector<pulseforge::PatternRun>(
            reader.patterns().begin(),
            reader.patterns().end()
        ) == chart.patterns,
        "pattern runs round-trip"
    );

    const auto all = reader.read_explicit_notes_in_range(
        std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::int64_t>::max()
    );
    require(static_cast<bool>(all), all.error);
    require(all.notes == chart.notes, "all explicit notes round-trip exactly");

    const auto boundary = reader.read_explicit_notes_in_range(1'000, 2'000);
    require(static_cast<bool>(boundary), boundary.error);
    const std::vector<pulseforge::PackedNote> expected_boundary{
        chart.notes[1U],
        chart.notes[2U],
        chart.notes[3U],
    };
    require(
        boundary.notes == expected_boundary,
        "inclusive range query crosses chunk boundaries"
    );

    pulseforge::PackedChartLimits visitor_limits;
    visitor_limits.max_query_notes = 2U;
    const auto visitor_reader = pulseforge::PackedChartReader::open(
        path,
        &error,
        visitor_limits
    );
    require(visitor_reader.has_value(), error);
    require(
        !static_cast<bool>(
            visitor_reader->read_explicit_notes_in_range(1'000, 2'000)
        ),
        "vector range query retains its configured allocation ceiling"
    );
    VisitCapture capture;
    const auto visited = visitor_reader->visit_explicit_notes_in_range(
        1'000,
        2'000,
        &capture,
        capture_note
    );
    require(static_cast<bool>(visited), visited.error);
    require(
        visited.notes_visited == expected_boundary.size()
            && capture.count == expected_boundary.size(),
        "streaming visitor has no result-count truncation"
    );
    for (std::size_t index = 0U; index < expected_boundary.size(); ++index) {
        require(
            capture.notes[index] == expected_boundary[index],
            "streaming visitor preserves exact note order and contents"
        );
    }

    const auto first_chunk = reader.chunk_info(0U, &error);
    const auto second_chunk = reader.chunk_info(1U, &error);
    const auto third_chunk = reader.chunk_info(2U, &error);
    require(
        first_chunk.has_value()
            && second_chunk.has_value()
            && third_chunk.has_value(),
        std::string("chunk lookup failed: ") + error
    );
    require(
        first_chunk->note_count == 2U
            && second_chunk->note_count == 2U
            && third_chunk->note_count == 1U,
        "chunk boundary counts are exact"
    );
    require(
        first_chunk->file_offset + first_chunk->byte_size
            == second_chunk->file_offset
            && second_chunk->file_offset + second_chunk->byte_size
                == third_chunk->file_offset,
        "chunk payloads are contiguous"
    );

    error.clear();
    require(
        !pulseforge::write_packed_chart(path, chart, options, &error),
        "writer refuses to overwrite an existing destination"
    );
    const auto preserved = pulseforge::PackedChartReader::open(path, &error);
    require(
        preserved.has_value()
            && preserved->explicit_note_count() == chart.notes.size(),
        "failed overwrite leaves the original chart intact"
    );

    const auto corrupt_header = directory / "corrupt-header.pfc1";
    copy_fixture(path, corrupt_header);
    flip_byte(corrupt_header, 20U);
    error.clear();
    require(
        !pulseforge::PackedChartReader::open(corrupt_header, &error).has_value(),
        "header corruption is rejected"
    );

    const auto truncated = directory / "truncated.pfc1";
    copy_fixture(path, truncated);
    const auto truncated_size = std::filesystem::file_size(truncated);
    std::filesystem::resize_file(truncated, truncated_size - 1U);
    error.clear();
    require(
        !pulseforge::PackedChartReader::open(truncated, &error).has_value(),
        "truncated files are rejected"
    );

    const auto corrupt_chunk = directory / "corrupt-chunk.pfc1";
    copy_fixture(path, corrupt_chunk);
    flip_byte(corrupt_chunk, first_chunk->file_offset + 1U);
    error.clear();
    const auto corrupt_reader = pulseforge::PackedChartReader::open(
        corrupt_chunk,
        &error
    );
    require(
        corrupt_reader.has_value(),
        "opening metadata does not eagerly materialize note chunks"
    );
    const auto corrupt_result = corrupt_reader->read_chunk(0U);
    require(
        !static_cast<bool>(corrupt_result),
        "chunk payload CRC32 detects corruption on demand"
    );
}

void test_defaults_follow_format_geometry(
    const std::filesystem::path& directory
) {
    const pulseforge::PackedChartLimits default_limits;
    require(
        default_limits.max_kinds == 1'000'000ULL
            && default_limits.max_patterns == 1'000'000ULL
            && default_limits.max_pattern_lanes == 4'000'000ULL,
        "materialized PFC1 dictionary/pattern counts keep finite default budgets"
    );
    require(
        default_limits.max_dictionary_bytes == 64ULL * 1024ULL * 1024ULL
            && default_limits.max_pattern_bytes == 128ULL * 1024ULL * 1024ULL,
        "materialized PFC1 dictionary/pattern byte budgets remain finite"
    );
    require(
        default_limits.max_file_bytes
            > 64ULL * 1024ULL * 1024ULL * 1024ULL
            && default_limits.max_explicit_notes
                == std::numeric_limits<std::uint64_t>::max()
            && default_limits.max_logical_notes
                == std::numeric_limits<std::uint64_t>::max()
            && default_limits.max_chunks
                == std::numeric_limits<std::uint64_t>::max(),
        "finite allocation budgets do not restore chart-wide legacy ceilings"
    );
    constexpr std::uint64_t legacy_file_limit =
        64ULL * 1024ULL * 1024ULL * 1024ULL;
    constexpr std::uint64_t note_record_bytes = 25U;
    constexpr std::uint64_t directory_record_bytes = 64U;
    constexpr std::uint64_t header_bytes = 192U;
    constexpr std::uint64_t dictionary_bytes = 4U + 6U; // "normal"
    constexpr std::uint64_t notes_per_chunk = 65'536U;

    // Only the small directory prefix is written during create(). This checks
    // the >64 GiB geometry without allocating or materializing the declared
    // multi-billion-note payload.
    const std::uint64_t explicit_notes = legacy_file_limit
        / note_record_bytes + 1U;
    const std::uint64_t chunks = (explicit_notes - 1U)
        / notes_per_chunk + 1U;
    const std::uint64_t declared_file_bytes = header_bytes
        + dictionary_bytes
        + chunks * directory_record_bytes
        + explicit_notes * note_record_bytes;
    require(
        declared_file_bytes > legacy_file_limit,
        "test geometry must exceed the removed 64 GiB ceiling"
    );

    pulseforge::PackedChartStreamSpec spec;
    spec.key_count = 4U;
    spec.explicit_note_count = explicit_notes;
    spec.kinds = {"normal"};
    std::string error;
    auto default_writer = pulseforge::PackedChartStreamWriter::create(
        directory / "above-legacy-64gib.pfc",
        spec,
        {},
        &error
    );
    require(
        default_writer.has_value(),
        std::string("default PFC1 geometry still rejects >64 GiB: ") + error
    );
    default_writer.reset();
    require(
        !std::filesystem::exists(directory / "above-legacy-64gib.pfc"),
        "unfinished geometry probe publishes no destination"
    );

    pulseforge::PackedChartWriteOptions policy_options;
    policy_options.limits.max_file_bytes = legacy_file_limit;
    error.clear();
    auto policy_writer = pulseforge::PackedChartStreamWriter::create(
        directory / "policy-limited-64gib.pfc",
        spec,
        policy_options,
        &error
    );
    require(
        !policy_writer.has_value()
            && error.find("configured file-size policy") != std::string::npos,
        "an explicit 64 GiB caller policy remains available and diagnostic"
    );

    constexpr std::uint64_t beyond_legacy_logical_limit =
        10'000'000'000'000'001ULL;
    pulseforge::PatternRun pattern;
    pattern.start_us = 0;
    pattern.interval_us = 0U;
    pattern.count = beyond_legacy_logical_limit;
    pattern.lane_pattern = {0U, 1U, 2U, 3U};
    spec.explicit_note_count = 0U;
    spec.patterns = {pattern};
    const auto logical_path = directory / "above-legacy-logical-count.pfc";
    auto logical_writer = pulseforge::PackedChartStreamWriter::create(
        logical_path,
        spec,
        {},
        &error
    );
    require(logical_writer.has_value(), error);
    require(logical_writer->finish(&error), error);
    const auto reader = pulseforge::PackedChartReader::open(
        logical_path,
        &error
    );
    require(reader.has_value(), error);
    require(
        reader->logical_note_count() == beyond_legacy_logical_limit,
        "reader accepts a structurally valid count above the former 10^16 cap"
    );
}

}  // namespace

int main() {
    try {
        test_pattern_run_constant_space();
        const TemporaryDirectory temporary;
        test_round_trip_and_ranges(temporary.path());
        test_defaults_follow_format_geometry(temporary.path());
        std::cout << "Packed chart tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "Packed chart tests failed: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
