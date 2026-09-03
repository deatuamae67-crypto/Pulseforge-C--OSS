#include "pulseforge/packed_chart.hpp"
#include "pulseforge/streaming_gameplay.hpp"
#include "pulseforge/visual_density_index.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using Clock = std::chrono::steady_clock;

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path()
            / ("pulseforge-scale-bench-" + std::to_string(
                static_cast<std::uint64_t>(
                    Clock::now().time_since_epoch().count()
                )
            ));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }
private:
    std::filesystem::path path_;
};

[[nodiscard]] double milliseconds(const Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

[[nodiscard]] pulseforge::PackedChartReader write_open(
    const std::filesystem::path& path,
    const pulseforge::PackedChartData& chart,
    const std::uint32_t chunk_notes
) {
    pulseforge::PackedChartWriteOptions options;
    options.max_notes_per_chunk = chunk_notes;
    std::string error;
    if (!pulseforge::write_packed_chart(path, chart, options, &error)) {
        throw std::runtime_error(error);
    }
    auto reader = pulseforge::PackedChartReader::open(path, &error);
    if (!reader.has_value()) {
        throw std::runtime_error(error);
    }
    return std::move(*reader);
}

void pattern_benchmark(
    const std::filesystem::path& directory,
    const std::uint64_t count,
    const std::string& label
) {
    pulseforge::PackedChartData chart;
    chart.key_count = 4U;
    chart.kinds = {"normal"};
    chart.patterns.push_back({
        0,
        100U,
        count,
        0U,
        {0U, 1U, 2U, 3U},
        pulseforge::PackedNoteOwner::player,
        0U,
        0U,
    });
    const auto path = directory / (label + ".pfc");
    auto reader = write_open(path, chart, 65'536U);
    pulseforge::GameplaySettings settings;
    settings.no_fail = true;
    settings.stacked_note_tolerance_ms = 0.0;
    pulseforge::StreamingGameplayOptions options;
    options.max_window_notes = 64U;
    options.max_active_holds = 2U;
    options.max_events_per_frame = 16U;
    options.max_recorded_inputs = 4U;
    std::string error;
    auto session = pulseforge::StreamingGameplaySession::create(
        reader,
        settings,
        options,
        {},
        &error
    );
    if (!session.has_value()) {
        throw std::runtime_error(error);
    }
    const auto last_us = (count - 1U) * 100U;
    const auto target_ms = static_cast<double>(last_us) / 1'000.0 + 3'000.0;
    const auto begin = Clock::now();
    if (!session->update(target_ms)) {
        throw std::runtime_error(std::string(session->error()));
    }
    const auto elapsed = Clock::now() - begin;
    if (session->total_resolved_notes() != count) {
        throw std::runtime_error("PatternRun did not resolve every logical note");
    }
    std::cout << label
              << ",logical=" << count
              << ",pfc_bytes=" << std::filesystem::file_size(path)
              << ",update_ms=" << std::fixed << std::setprecision(3)
              << milliseconds(elapsed)
              << ",chunks=" << reader.chunk_count()
              << ",window_notes=" << session->memory_stats().window_notes
              << ",active_holds=" << session->memory_stats().active_holds
              << ",dynamic_bytes="
              << session->memory_stats().approximate_dynamic_bytes << '\n';
}

void irregular_benchmark(const std::filesystem::path& directory) {
    constexpr std::uint64_t count = 1'000'000U;
    pulseforge::PackedChartData chart;
    chart.key_count = 4U;
    chart.kinds = {"normal"};
    chart.notes.reserve(static_cast<std::size_t>(count));
    std::int64_t time_us{};
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    for (std::uint64_t index = 0U; index < count; ++index) {
        state ^= state >> 12U;
        state ^= state << 25U;
        state ^= state >> 27U;
        time_us += static_cast<std::int64_t>(7U + (state % 47U));
        chart.notes.push_back({
            time_us,
            0U,
            static_cast<std::uint16_t>((state >> 32U) % 4U),
            index % 5U == 0U
                ? pulseforge::PackedNoteOwner::opponent
                : pulseforge::PackedNoteOwner::player,
            0U,
            0U,
        });
    }
    const auto path = directory / "irregular-1m.pfc";
    const auto write_begin = Clock::now();
    auto reader = write_open(path, chart, 65'536U);
    const auto write_elapsed = Clock::now() - write_begin;
    chart.notes.clear();
    chart.notes.shrink_to_fit();

    pulseforge::GameplaySettings settings;
    settings.no_fail = true;
    pulseforge::StreamingGameplayOptions options;
    options.max_window_notes = 262'144U;
    options.max_explicit_chunk_notes = 65'536U;
    options.max_explicit_catchup_notes_per_update = 131'072U;
    std::string error;
    auto session = pulseforge::StreamingGameplaySession::create(
        reader,
        settings,
        options,
        {},
        &error
    );
    if (!session.has_value()) {
        throw std::runtime_error(error);
    }
    const auto target_ms = static_cast<double>(time_us) / 1'000.0 + 3'000.0;
    const auto update_begin = Clock::now();
    std::uint64_t updates{};
    do {
        if (!session->update(target_ms)) {
            throw std::runtime_error(std::string(session->error()));
        }
        ++updates;
    } while (session->catchup_pending());
    const auto update_elapsed = Clock::now() - update_begin;
    if (session->total_resolved_notes() != count) {
        throw std::runtime_error("irregular stream did not resolve every note");
    }
    const auto update_ms = milliseconds(update_elapsed);
    std::cout << "irregular-explicit"
              << ",logical=" << count
              << ",pfc_bytes=" << std::filesystem::file_size(path)
              << ",write_ms=" << std::fixed << std::setprecision(3)
              << milliseconds(write_elapsed)
              << ",update_ms=" << update_ms
              << ",million_notes_per_second="
              << (static_cast<double>(count) / update_ms / 1'000.0)
              << ",updates=" << updates
              << ",chunks=" << reader.chunk_count()
              << ",window_notes=" << session->memory_stats().window_notes
              << ",active_holds=" << session->memory_stats().active_holds
              << ",dynamic_bytes=" << session->memory_stats().approximate_dynamic_bytes
              << '\n';
}

struct DensityTotals final {
    std::uint64_t records{};
    std::uint64_t heads{};
    std::uint64_t active_sustains{};
};

void collect_density(
    void* raw,
    const pulseforge::VisualDensityBucket& bucket
) noexcept {
    auto& totals = *static_cast<DensityTotals*>(raw);
    ++totals.records;
    totals.heads += bucket.normal_heads + bucket.hurt_heads;
    totals.active_sustains += bucket.active_sustains_at_bucket_start;
}

void real_pfc_benchmark(
    const std::filesystem::path& pfc_path,
    const std::filesystem::path& pvd_path
) {
    std::string error;
    const auto open_begin = Clock::now();
    auto reader = pulseforge::PackedChartReader::open(pfc_path, &error);
    if (!reader.has_value()) {
        throw std::runtime_error(error);
    }
    const auto open_elapsed = Clock::now() - open_begin;
    const auto warm_open_begin = Clock::now();
    auto warm_reader = pulseforge::PackedChartReader::open(pfc_path, &error);
    if (!warm_reader.has_value()) {
        throw std::runtime_error(error);
    }
    const auto warm_open_elapsed = Clock::now() - warm_open_begin;

    std::error_code filesystem_error;
    std::filesystem::remove(pvd_path, filesystem_error);
    pulseforge::VisualDensityIndexBuilder builder(
        reader->key_count(),
        reader->kinds(),
        pvd_path.parent_path()
    );
    const auto build_begin = Clock::now();
    for (std::uint64_t chunk = 0U; chunk < reader->chunk_count(); ++chunk) {
        const auto decoded = reader->read_chunk(chunk);
        if (!decoded) {
            throw std::runtime_error(decoded.error);
        }
        for (const auto& note : decoded.notes) {
            if (!builder.add(note, &error)) {
                throw std::runtime_error(error);
            }
        }
    }
    if (!builder.finish(pvd_path, &error)) {
        throw std::runtime_error(error);
    }
    const auto build_elapsed = Clock::now() - build_begin;
    const auto pvd_open_begin = Clock::now();
    const auto visual = pulseforge::VisualDensityIndexReader::open(
        pvd_path,
        &error
    );
    if (!visual.has_value()) {
        throw std::runtime_error(error);
    }
    const auto pvd_open_elapsed = Clock::now() - pvd_open_begin;

    std::int64_t first_us{};
    std::int64_t last_us{};
    if (reader->chunk_count() != 0U) {
        const auto first = reader->chunk_info(0U, &error);
        const auto last = reader->chunk_info(reader->chunk_count() - 1U, &error);
        if (!first.has_value() || !last.has_value()) {
            throw std::runtime_error(error);
        }
        first_us = first->first_time_us;
        last_us = last->last_time_us;
    }
    constexpr std::int64_t cache_step_us = 250'000;
    constexpr std::int64_t half_viewport_us = 3'000'000;
    DensityTotals totals;
    std::uint64_t queries{};
    const auto visit_begin = Clock::now();
    for (auto center = first_us; center <= last_us;) {
        const auto visited = visual->visit(
            center - half_viewport_us,
            center + half_viewport_us,
            4'000U,
            &totals,
            collect_density
        );
        if (!visited) {
            throw std::runtime_error(visited.error);
        }
        ++queries;
        if (center > last_us - cache_step_us) {
            break;
        }
        center += cache_step_us;
    }
    const auto visit_elapsed = Clock::now() - visit_begin;
    std::cout << "real-pfc"
              << ",logical=" << reader->logical_note_count()
              << ",explicit=" << reader->explicit_note_count()
              << ",patterns=" << reader->patterns().size()
              << ",pfc_bytes=" << std::filesystem::file_size(pfc_path)
              << ",pvd_bytes=" << std::filesystem::file_size(pvd_path)
              << ",chunks=" << reader->chunk_count()
              << ",pfc_open_cold_ms=" << std::fixed << std::setprecision(3)
              << milliseconds(open_elapsed)
              << ",pfc_open_warm_ms=" << milliseconds(warm_open_elapsed)
              << ",pvd_build_ms=" << milliseconds(build_elapsed)
              << ",pvd_open_ms=" << milliseconds(pvd_open_elapsed)
              << ",queries=" << queries
              << ",pvd_query_total_ms=" << milliseconds(visit_elapsed)
              << ",pvd_query_avg_ms="
              << (milliseconds(visit_elapsed) / static_cast<double>(queries))
              << ",records=" << totals.records << '\n';
}

}  // namespace

int main(const int count, char** values) {
    try {
        if (count == 3) {
            real_pfc_benchmark(values[1], values[2]);
            return 0;
        }
        TemporaryDirectory temporary;
        pattern_benchmark(temporary.path(), 57'000'000U, "pattern-57m");
        pattern_benchmark(temporary.path(), 10'000'000'000ULL, "pattern-10b");
        irregular_benchmark(temporary.path());
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "streaming scale benchmark failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
