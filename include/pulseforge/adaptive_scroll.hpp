#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace pulseforge {

// Visual-only controller for pathological note density. Gameplay timing and
// judgment are never modified: only the pixels-per-millisecond render scale is
// increased when the screen would otherwise contain more logical notes than a
// device can sustain at the target frame rate.
class AdaptiveScrollController final {
public:
    void reset() noexcept {
        multiplier_ = 1.0;
        learned_note_budget_ = 0.0;
        elapsed_since_evaluation_ = 0.0;
    }

    void update(
        const double elapsed_seconds,
        const double smoothed_fps,
        const double smoothed_frame_ms,
        const std::uint64_t visible_logical_notes,
        const std::uint32_t configured_note_budget,
        const bool enabled
    ) noexcept {
        if (!enabled || !std::isfinite(elapsed_seconds)
            || elapsed_seconds < 0.0) {
            reset();
            return;
        }

        const double configured = static_cast<double>(std::max<std::uint32_t>(
            configured_note_budget,
            minimum_note_budget
        ));
        if (!(learned_note_budget_ > 0.0)
            || !std::isfinite(learned_note_budget_)) {
            learned_note_budget_ = configured;
        } else {
            learned_note_budget_ = std::clamp(
                learned_note_budget_,
                static_cast<double>(minimum_note_budget),
                configured
            );
        }

        elapsed_since_evaluation_ += elapsed_seconds;
        if (elapsed_since_evaluation_ < evaluation_interval_seconds) {
            return;
        }
        elapsed_since_evaluation_ = std::fmod(
            elapsed_since_evaluation_,
            evaluation_interval_seconds
        );

        constexpr double target_fps = 60.0;
        constexpr double target_frame_ms = 1'000.0 / target_fps;
        const bool fps_valid = std::isfinite(smoothed_fps) && smoothed_fps > 0.0;
        const bool frame_valid = std::isfinite(smoothed_frame_ms)
            && smoothed_frame_ms > 0.0;
        const double measured_fps = fps_valid
            ? smoothed_fps
            : (frame_valid ? 1'000.0 / smoothed_frame_ms : target_fps);
        const double measured_frame_ms = frame_valid
            ? smoothed_frame_ms
            : (fps_valid ? 1'000.0 / smoothed_fps : target_frame_ms);

        // Learn a conservative per-device logical-note budget. A frame-rate
        // miss cuts the budget quickly; recovery raises it slowly to avoid a
        // saw-tooth speed oscillation on mobile GPUs.
        if (visible_logical_notes >= minimum_note_budget
            && measured_frame_ms > target_frame_ms * 1.02) {
            const double performance_ratio = std::clamp(
                target_frame_ms / measured_frame_ms,
                0.25,
                0.95
            );
            learned_note_budget_ = std::max(
                static_cast<double>(minimum_note_budget),
                learned_note_budget_ * performance_ratio * 0.92
            );
        } else if (measured_frame_ms <= target_frame_ms * 1.02
            && static_cast<double>(visible_logical_notes)
                < learned_note_budget_ * 0.60) {
            learned_note_budget_ = std::min(
                configured,
                learned_note_budget_ * 1.08
            );
        }

        const double density_ratio = visible_logical_notes == 0U
            ? 0.0
            : static_cast<double>(visible_logical_notes)
                / std::max(learned_note_budget_, 1.0);
        const double frame_pressure = visible_logical_notes >= minimum_note_budget
            ? std::max(1.0, measured_frame_ms / target_frame_ms)
            : 1.0;

        if (density_ratio > 1.05 || frame_pressure > 1.04) {
            // Logical notes on screen are approximately inverse to scroll
            // speed. Move toward that inverse solution, but cap one control
            // interval to 4x so a single diagnostic spike cannot teleport the
            // chart from normal speed to the maximum.
            const double pressure = std::max(density_ratio, frame_pressure * 1.08);
            const double requested = multiplier_ * std::clamp(pressure, 1.15, 4.0);
            multiplier_ = quantize_up(std::min(requested, maximum_multiplier));
            return;
        }

        // Only relax after the 60 Hz target is healthy and density is clearly
        // sub-budget. 60-Hz VSync/caps therefore remain able to return toward
        // the authored speed; requiring >60 FPS would make the boost sticky.
        if (multiplier_ > 1.0
            && measured_frame_ms <= target_frame_ms * 1.02
            && measured_fps >= target_fps * 0.97
            && static_cast<double>(visible_logical_notes)
                <= learned_note_budget_ * 0.72) {
            multiplier_ = next_lower(multiplier_);
        }
    }

    [[nodiscard]] double multiplier() const noexcept { return multiplier_; }
    [[nodiscard]] std::uint64_t learned_note_budget() const noexcept {
        if (!(learned_note_budget_ > 0.0)
            || !std::isfinite(learned_note_budget_)) {
            return minimum_note_budget;
        }
        return static_cast<std::uint64_t>(std::clamp(
            std::llround(learned_note_budget_),
            static_cast<long long>(minimum_note_budget),
            static_cast<long long>(std::numeric_limits<std::uint32_t>::max())
        ));
    }

private:
    static constexpr std::uint32_t minimum_note_budget = 96U;
    static constexpr double evaluation_interval_seconds = 0.25;
    static constexpr double maximum_multiplier = 64.0;
    static constexpr std::array<double, 18U> levels{
        1.0, 1.125, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0, 4.0,
        5.0, 6.0, 8.0, 10.0, 12.0, 16.0, 24.0, 32.0, 64.0,
    };

    [[nodiscard]] static double quantize_up(const double value) noexcept {
        for (const double level : levels) {
            if (level + 1.0e-9 >= value) return level;
        }
        return levels.back();
    }

    [[nodiscard]] static double next_lower(const double value) noexcept {
        double result = 1.0;
        for (const double level : levels) {
            if (level >= value - 1.0e-9) return result;
            result = level;
        }
        return result;
    }

    double multiplier_{1.0};
    double learned_note_budget_{};
    double elapsed_since_evaluation_{};
};

}  // namespace pulseforge
