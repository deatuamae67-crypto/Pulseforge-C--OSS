#include "pulseforge/visual_density_index.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto nonce = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
        path_ = std::filesystem::temp_directory_path()
            / ("pulseforge-pvd-test-" + std::to_string(nonce));
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

struct Counts final {
    std::uint64_t normal{};
    std::uint64_t hurt{};
    std::uint64_t sustain_starts{};
    std::uint64_t sustain_ends{};
    std::uint64_t maximum_active_sustains{};
};

void collect(void* raw, const pulseforge::VisualDensityBucket& bucket) noexcept {
    auto& counts = *static_cast<Counts*>(raw);
    counts.normal += bucket.normal_heads;
    counts.hurt += bucket.hurt_heads;
    counts.sustain_starts += bucket.sustain_starts;
    counts.sustain_ends += bucket.sustain_ends;
    counts.maximum_active_sustains = std::max(
        counts.maximum_active_sustains,
        bucket.active_sustains_at_bucket_start
    );
}

void round_trip_preserves_aggregate_counts() {
    TemporaryDirectory temporary;
    const std::string kinds[]{"normal", "Hurt Note"};
    pulseforge::VisualDensityIndexBuilder builder(
        4U,
        kinds,
        temporary.path()
    );

    constexpr std::uint64_t note_count = 10'001U;
    std::uint64_t expected_normal{};
    std::uint64_t expected_hurt{};
    std::uint64_t expected_sustains{};
    for (std::uint64_t index = 0U; index < note_count; ++index) {
        pulseforge::PackedNote note;
        note.time_us = static_cast<std::int64_t>(index * 125U);
        note.duration_us = index % 1'000U == 0U ? 15'000U : 0U;
        note.lane = static_cast<std::uint16_t>(index % 4U);
        note.owner = index % 3U == 0U
            ? pulseforge::PackedNoteOwner::opponent
            : pulseforge::PackedNoteOwner::player;
        note.kind_id = index % 17U == 0U ? 1U : 0U;
        require(builder.add(note), "PVD accepts ordinary note");
        if (note.kind_id == 1U) {
            ++expected_hurt;
        } else {
            ++expected_normal;
        }
        if (note.duration_us != 0U) {
            ++expected_sustains;
        }
    }

    const auto path = temporary.path() / "chart.pvd";
    std::string error;
    require(builder.finish(path, &error), error);
    auto reader = pulseforge::VisualDensityIndexReader::open(path, &error);
    require(reader.has_value(), error);
    require(reader->key_count() == 4U, "PVD key count changed");

    Counts counts;
    const auto visited = reader->visit(
        0,
        static_cast<std::int64_t>(note_count * 125U + 20'000U),
        8'000U,
        &counts,
        collect
    );
    require(static_cast<bool>(visited), visited.error);
    require(visited.selected_bucket_us <= 8'000U, "PVD chose too coarse a level");
    require(counts.normal == expected_normal, "normal head count changed");
    require(counts.hurt == expected_hurt, "hurt head count changed");
    require(
        counts.sustain_starts == expected_sustains
            && counts.sustain_ends == expected_sustains,
        "sustain event count changed"
    );
}

void long_sustain_prefix_is_queryable() {
    TemporaryDirectory temporary;
    const std::string kinds[]{"normal"};
    pulseforge::VisualDensityIndexBuilder builder(
        4U,
        kinds,
        temporary.path()
    );
    require(builder.add({
        0,
        1'000'000U,
        2U,
        pulseforge::PackedNoteOwner::player,
        0U,
        0U,
    }), "PVD accepts long sustain");
    const auto path = temporary.path() / "long-sustain.pvd";
    std::string error;
    require(builder.finish(path, &error), error);
    const auto reader = pulseforge::VisualDensityIndexReader::open(path, &error);
    require(reader.has_value(), error);

    Counts interior;
    const auto interior_visit = reader->visit(
        400'000,
        600'000,
        8'000U,
        &interior,
        collect
    );
    require(static_cast<bool>(interior_visit), interior_visit.error);
    require(
        interior.maximum_active_sustains == 1U
            && interior.sustain_starts == 0U
            && interior.sustain_ends == 0U,
        "a sustain remains active when its head and tail are outside the query"
    );

    Counts tail;
    const auto tail_visit = reader->visit(
        990'000,
        1'010'000,
        8'000U,
        &tail,
        collect
    );
    require(static_cast<bool>(tail_visit), tail_visit.error);
    require(
        tail.maximum_active_sustains == 1U
            && tail.sustain_starts == 0U
            && tail.sustain_ends == 1U,
        "an end-only query receives the active prefix before the sustain tail"
    );
}

void hostile_sustain_duration_fails_fast() {
    TemporaryDirectory temporary;
    const std::string kinds[]{"normal"};
    pulseforge::VisualDensityIndexBuilder builder(
        4U,
        kinds,
        temporary.path()
    );
    require(builder.add({
        0,
        std::numeric_limits<std::uint64_t>::max(),
        0U,
        pulseforge::PackedNoteOwner::player,
        0U,
        0U,
    }), "PVD accepts hostile sustain start before bounded finish");
    const auto begin = std::chrono::steady_clock::now();
    std::string error;
    require(
        !builder.finish(temporary.path() / "hostile.pvd", &error),
        "hostile sustain must exceed the optional PVD budget"
    );
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - begin
    ).count();
    require(
        elapsed < 5.0,
        "hostile sustain PVD rejection is bounded instead of duration-linear"
    );
    require(
        error.find("sidecar budget") != std::string::npos,
        "hostile sustain reports its optional sidecar budget"
    );
}

void many_unique_sustain_ends_are_bounded() {
    TemporaryDirectory temporary;
    const std::string kinds[]{"normal"};
    pulseforge::VisualDensityIndexBuilder builder(
        4U,
        kinds,
        temporary.path()
    );
    std::string error;
    bool accepted = true;
    for (std::size_t index = 0U;
         index <= pulseforge::visual_density_max_pending_end_buckets;
         ++index) {
        accepted = builder.add({
            0,
            static_cast<std::uint64_t>(index + 1U) * 250U,
            0U,
            pulseforge::PackedNoteOwner::player,
            0U,
            0U,
        }, &error);
        if (!accepted) {
            break;
        }
    }
    require(!accepted, "unique sustain end map is explicitly bounded");
    require(
        error.find("pending sustain ends") != std::string::npos,
        "unique sustain end overflow reports optional sidecar budget"
    );
}

}  // namespace

int main() {
    try {
        round_trip_preserves_aggregate_counts();
        long_sustain_prefix_is_queryable();
        hostile_sustain_duration_fails_fast();
        many_unique_sustain_ends_are_bounded();
        std::cout << "visual density index tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "visual density index tests failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
