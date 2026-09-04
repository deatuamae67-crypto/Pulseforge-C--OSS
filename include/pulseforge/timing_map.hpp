#pragma once

#include "pulseforge/chart.hpp"

#include <cstddef>
#include <vector>

namespace pulseforge {

class TimingMap {
public:
    explicit TimingMap(std::vector<TempoChange> changes = {});

    void reset(std::vector<TempoChange> changes);

    [[nodiscard]] double beat_at(double time_ms) const noexcept;
    [[nodiscard]] double step_at(double time_ms) const noexcept;
    [[nodiscard]] double time_at_beat(double beat) const noexcept;
    [[nodiscard]] double time_at_step(double step) const noexcept;
    [[nodiscard]] double bpm_at(double time_ms) const noexcept;
    [[nodiscard]] std::size_t segment_count() const noexcept;
    [[nodiscard]] const std::vector<TempoChange>& changes() const noexcept;

private:
    struct Segment {
        TempoChange change;
        double start_beat{};
        double milliseconds_per_beat{500.0};
    };

    [[nodiscard]] std::size_t segment_for_time(double time_ms) const noexcept;
    [[nodiscard]] std::size_t segment_for_beat(double beat) const noexcept;

    std::vector<TempoChange> changes_;
    std::vector<Segment> segments_;
};

}  // namespace pulseforge

