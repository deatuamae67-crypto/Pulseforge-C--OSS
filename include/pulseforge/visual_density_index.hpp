#pragma once

#include "pulseforge/packed_chart.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace pulseforge {

inline constexpr std::uint32_t visual_density_base_bucket_us = 250U;
inline constexpr std::uint16_t visual_density_level_count = 12U;
inline constexpr std::size_t visual_density_max_pending_end_buckets = 65'536U;

struct VisualDensityBucket {
    std::int64_t bucket_index{};
    std::uint64_t bucket_width_us{};
    std::uint16_t lane{};
    PackedNoteOwner owner{PackedNoteOwner::player};
    std::uint64_t normal_heads{};
    std::uint64_t hurt_heads{};
    std::uint64_t sustain_starts{};
    std::uint64_t sustain_ends{};
    // Exact number of sustains already active at this bucket's beginning.
    // visit() also emits sparse, count-free state records at the requested
    // range start when a sustain began before the range.
    std::uint64_t active_sustains_at_bucket_start{};
};

using VisualDensityVisitor = void (*)(
    void* context,
    const VisualDensityBucket& bucket
) noexcept;

struct VisualDensityVisitResult {
    std::uint64_t buckets_visited{};
    std::uint64_t selected_bucket_us{};
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error.empty();
    }
};

// Streaming builder for a sparse, multi-resolution visual-density sidecar.
// Notes must arrive in nondecreasing time order. The builder writes level 0
// incrementally and derives coarser levels from the previous one, so memory use
// does not scale with total note count.
class VisualDensityIndexBuilder final {
public:
    VisualDensityIndexBuilder(
        std::uint16_t key_count,
        std::span<const std::string> kinds,
        const std::filesystem::path& working_directory = {}
    );
    ~VisualDensityIndexBuilder();

    VisualDensityIndexBuilder(VisualDensityIndexBuilder&&) noexcept;
    VisualDensityIndexBuilder& operator=(VisualDensityIndexBuilder&&) noexcept;

    VisualDensityIndexBuilder(const VisualDensityIndexBuilder&) = delete;
    VisualDensityIndexBuilder& operator=(const VisualDensityIndexBuilder&) = delete;

    // false disables only this optional derivative (for example when hostile
    // sustain geometry exceeds its fixed build budget). The authoritative PFC
    // writer must continue normally.
    [[nodiscard]] bool add(
        const PackedNote& note,
        std::string* error = nullptr
    ) noexcept;

    [[nodiscard]] bool finish(
        const std::filesystem::path& destination,
        std::string* error = nullptr
    ) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

class VisualDensityIndexReader final {
public:
    [[nodiscard]] static std::optional<VisualDensityIndexReader> open(
        const std::filesystem::path& path,
        std::string* error = nullptr
    );

    [[nodiscard]] VisualDensityVisitResult visit(
        std::int64_t first_time_us,
        std::int64_t last_time_us,
        std::uint64_t target_bucket_us,
        void* context,
        VisualDensityVisitor visitor
    ) const;

    [[nodiscard]] std::uint16_t key_count() const noexcept;
    [[nodiscard]] std::uint16_t level_count() const noexcept;
    [[nodiscard]] std::uint32_t base_bucket_us() const noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

private:
    struct LevelInfo {
        std::uint64_t bucket_width_us{};
        std::uint64_t file_offset{};
        std::uint64_t record_count{};
        std::uint64_t byte_size{};
    };

    std::filesystem::path path_;
    std::uint16_t key_count_{};
    std::uint16_t level_count_{};
    std::uint32_t base_bucket_us_{};
    std::uint64_t file_size_{};
    std::vector<LevelInfo> levels_;
};

}  // namespace pulseforge
