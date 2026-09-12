#pragma once

#include "pulseforge/chart.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace pulseforge {

// Routing hints, never chart-validity limits. The PFC1 path is substantially
// cheaper for dense gameplay because it can answer viewport queries without
// visiting every logical note in the visible time interval every frame.
inline constexpr std::uintmax_t preferred_streaming_json_bytes =
    8ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t preferred_streaming_total_notes = 250'000U;
inline constexpr double preferred_streaming_density_window_ms = 2'000.0;
inline constexpr std::size_t minimum_dense_window_note_budget = 16'384U;
inline constexpr std::size_t maximum_dense_window_note_budget = 65'536U;

[[nodiscard]] inline bool large_chart_source_prefers_streaming(
    const std::uintmax_t source_bytes
) noexcept {
    return source_bytes >= preferred_streaming_json_bytes;
}

[[nodiscard]] inline bool materialized_note_count_prefers_streaming(
    const std::size_t note_count
) noexcept {
    return note_count >= preferred_streaming_total_notes;
}

[[nodiscard]] inline std::size_t dense_window_note_budget(
    const std::uint32_t configured_visible_notes
) noexcept {
    const auto configured = static_cast<std::size_t>(
        std::max<std::uint32_t>(configured_visible_notes, 1U)
    );
    const auto scaled = configured
            > std::numeric_limits<std::size_t>::max() / 4U
        ? std::numeric_limits<std::size_t>::max()
        : configured * 4U;
    return std::clamp(
        scaled,
        minimum_dense_window_note_budget,
        maximum_dense_window_note_budget
    );
}

// Chart::normalize() and production loaders keep notes ordered by time. This
// two-pointer scan is O(n), allocation-free and runs once at load. It catches
// compact high-NPS charts that do not cross the source-byte hint.
[[nodiscard]] inline bool materialized_chart_density_prefers_streaming(
    const Chart& chart,
    const std::uint32_t configured_visible_notes
) noexcept {
    if (materialized_note_count_prefers_streaming(chart.notes.size())) {
        return true;
    }
    const auto threshold = dense_window_note_budget(configured_visible_notes);
    if (chart.notes.size() < threshold) {
        return false;
    }

    std::size_t left = 0U;
    double previous = -std::numeric_limits<double>::infinity();
    for (std::size_t right = 0U; right < chart.notes.size(); ++right) {
        const double time = chart.notes[right].time_ms;
        if (!std::isfinite(time)) {
            continue;
        }
        // Validation owns malformed/out-of-order input. Do not turn this
        // performance heuristic into a compatibility rejection.
        if (time < previous) {
            return false;
        }
        previous = time;
        while (left < right
            && time - chart.notes[left].time_ms
                > preferred_streaming_density_window_ms) {
            ++left;
        }
        if (right - left + 1U >= threshold) {
            return true;
        }
    }
    return false;
}


// Charts in the multi-million-note class need a second, independent visual
// quality governor. Note/PFC streaming remains exact; only presentation-only
// stage effects are reduced. This is deliberately keyed to logical chart size
// so a 33M/50M chart starts in a sane GPU/draw-call state before the first FPS
// sample exists, then tightens further only if frame pacing still collapses.
inline constexpr std::uint64_t extreme_chart_visual_lod_notes = 5'000'000ULL;
inline constexpr std::uint64_t massive_chart_visual_lod_notes = 25'000'000ULL;

struct DenseChartVisualLod final {
    std::size_t wavy_segment_cap{64U};
    std::size_t wiggle_segment_cap{24U};
    bool allow_cpu_heavy_texture_effects{true};
};

[[nodiscard]] inline DenseChartVisualLod dense_chart_visual_lod(
    const std::uint64_t logical_notes,
    const double smoothed_fps,
    const double smoothed_frame_ms
) noexcept {
    DenseChartVisualLod result;
    if (logical_notes < extreme_chart_visual_lod_notes) {
        return result;
    }

    if (logical_notes >= massive_chart_visual_lod_notes) {
        result.wavy_segment_cap = 24U;
        result.wiggle_segment_cap = 12U;
        result.allow_cpu_heavy_texture_effects = false;
    } else {
        result.wavy_segment_cap = 40U;
        result.wiggle_segment_cap = 18U;
    }

    const bool has_fps = std::isfinite(smoothed_fps) && smoothed_fps > 0.0;
    const bool has_frame = std::isfinite(smoothed_frame_ms)
        && smoothed_frame_ms > 0.0;
    if (!has_fps && !has_frame) {
        return result;
    }

    const auto slower_than = [&](const double fps, const double frame_ms) {
        return (has_fps && smoothed_fps < fps)
            || (has_frame && smoothed_frame_ms > frame_ms);
    };
    if (slower_than(30.0, 33.34)) {
        result.wavy_segment_cap = 8U;
        result.wiggle_segment_cap = 8U;
        result.allow_cpu_heavy_texture_effects = false;
    } else if (slower_than(45.0, 22.23)) {
        result.wavy_segment_cap = std::min<std::size_t>(result.wavy_segment_cap, 12U);
        result.wiggle_segment_cap = 8U;
        result.allow_cpu_heavy_texture_effects = false;
    } else if (slower_than(55.0, 18.19)) {
        result.wavy_segment_cap = std::min<std::size_t>(result.wavy_segment_cap, 16U);
        result.wiggle_segment_cap = std::min<std::size_t>(result.wiggle_segment_cap, 10U);
        result.allow_cpu_heavy_texture_effects = false;
    } else if (slower_than(59.5, 16.81)) {
        result.wavy_segment_cap = std::min<std::size_t>(result.wavy_segment_cap, 24U);
        result.wiggle_segment_cap = std::min<std::size_t>(result.wiggle_segment_cap, 12U);
        result.allow_cpu_heavy_texture_effects = false;
    }
    return result;
}

}  // namespace pulseforge
