#include "pulseforge/timing_map.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace pulseforge {

TimingMap::TimingMap(std::vector<TempoChange> changes) {
    reset(std::move(changes));
}

void TimingMap::reset(std::vector<TempoChange> changes) {
    if (changes.empty()) {
        changes.push_back(TempoChange{});
    }

    for (auto& change : changes) {
        if (!std::isfinite(change.time_ms)) {
            change.time_ms = 0.0;
        }
        if (!std::isfinite(change.bpm) || change.bpm <= 0.0) {
            change.bpm = 120.0;
        }
    }
    std::stable_sort(changes.begin(), changes.end(), [](const auto& left, const auto& right) {
        return left.time_ms < right.time_ms;
    });

    std::vector<TempoChange> unique;
    unique.reserve(changes.size() + 1);
    for (const auto& change : changes) {
        if (!unique.empty() && std::abs(unique.back().time_ms - change.time_ms) < 0.0001) {
            unique.back() = change;
        } else {
            unique.push_back(change);
        }
    }
    if (unique.front().time_ms > 0.0) {
        auto initial = unique.front();
        initial.time_ms = 0.0;
        unique.insert(unique.begin(), initial);
    }

    changes_ = std::move(unique);
    segments_.clear();
    segments_.reserve(changes_.size());

    double start_beat = 0.0;
    for (std::size_t index = 0; index < changes_.size(); ++index) {
        const auto& change = changes_[index];
        if (index > 0) {
            const auto& previous = segments_.back();
            start_beat = previous.start_beat
                + (change.time_ms - previous.change.time_ms)
                    / previous.milliseconds_per_beat;
        }
        segments_.push_back({
            change,
            start_beat,
            60'000.0 / change.bpm,
        });
    }
}

double TimingMap::beat_at(const double time_ms) const noexcept {
    const auto& segment = segments_[segment_for_time(time_ms)];
    return segment.start_beat
        + (time_ms - segment.change.time_ms) / segment.milliseconds_per_beat;
}

double TimingMap::step_at(const double time_ms) const noexcept {
    return beat_at(time_ms) * 4.0;
}

double TimingMap::time_at_beat(const double beat) const noexcept {
    const auto& segment = segments_[segment_for_beat(beat)];
    return segment.change.time_ms
        + (beat - segment.start_beat) * segment.milliseconds_per_beat;
}

double TimingMap::time_at_step(const double step) const noexcept {
    return time_at_beat(step * 0.25);
}

double TimingMap::bpm_at(const double time_ms) const noexcept {
    return segments_[segment_for_time(time_ms)].change.bpm;
}

std::size_t TimingMap::segment_count() const noexcept {
    return segments_.size();
}

const std::vector<TempoChange>& TimingMap::changes() const noexcept {
    return changes_;
}

std::size_t TimingMap::segment_for_time(const double time_ms) const noexcept {
    const auto it = std::upper_bound(
        segments_.begin(),
        segments_.end(),
        time_ms,
        [](const double value, const Segment& segment) {
            return value < segment.change.time_ms;
        }
    );
    if (it == segments_.begin()) {
        return 0;
    }
    return static_cast<std::size_t>(std::distance(segments_.begin(), it) - 1);
}

std::size_t TimingMap::segment_for_beat(const double beat) const noexcept {
    const auto it = std::upper_bound(
        segments_.begin(),
        segments_.end(),
        beat,
        [](const double value, const Segment& segment) {
            return value < segment.start_beat;
        }
    );
    if (it == segments_.begin()) {
        return 0;
    }
    return static_cast<std::size_t>(std::distance(segments_.begin(), it) - 1);
}

}  // namespace pulseforge

