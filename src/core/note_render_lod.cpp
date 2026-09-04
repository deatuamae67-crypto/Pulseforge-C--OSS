#include "pulseforge/note_render_lod.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pulseforge {
namespace {

[[nodiscard]] constexpr std::uint64_t saturated_add(
    const std::uint64_t left,
    const std::uint64_t right
) noexcept {
    return right > std::numeric_limits<std::uint64_t>::max() - left
        ? std::numeric_limits<std::uint64_t>::max()
        : left + right;
}

// PULSEFORGE_P1_4_0_DENSE_THREE_OWNER_COVERAGE_V1
[[nodiscard]] constexpr std::size_t owner_index(
    const NoteOwner owner
) noexcept {
    switch (owner) {
    case NoteOwner::opponent: return 0U;
    case NoteOwner::player: return 1U;
    case NoteOwner::secondary_opponent: return 2U;
    }
    return 0U;
}

}  // namespace

std::optional<NoteScreenSpan> note_screen_span(
    const double note_time_ms,
    const double duration_ms,
    const double visual_time_ms,
    const double pixels_per_ms,
    const bool downscroll,
    const double receptor_y
) noexcept {
    if (!std::isfinite(note_time_ms) || !std::isfinite(duration_ms)
        || !std::isfinite(visual_time_ms) || !std::isfinite(pixels_per_ms)
        || !std::isfinite(receptor_y) || duration_ms < 0.0
        || pixels_per_ms <= 0.0) {
        return std::nullopt;
    }
    const double direction = downscroll ? -1.0 : 1.0;
    const double head = receptor_y
        + direction * (note_time_ms - visual_time_ms) * pixels_per_ms;
    const double tail = receptor_y
        + direction
            * (note_time_ms + duration_ms - visual_time_ms)
            * pixels_per_ms;
    if (!std::isfinite(head) || !std::isfinite(tail)) {
        return std::nullopt;
    }
    return NoteScreenSpan{head, tail, duration_ms > 0.0};
}

std::optional<NoteVisualSpan> visual_note_span(
    const double note_time_ms,
    const double duration_ms,
    const double visual_time_ms,
    const double pixels_per_ms,
    const bool downscroll,
    const double receptor_y,
    const bool head_resolved
) noexcept {
    auto span = note_screen_span(
        note_time_ms,
        duration_ms,
        visual_time_ms,
        pixels_per_ms,
        downscroll,
        receptor_y
    );
    if (!span.has_value()) {
        return std::nullopt;
    }
    if (!head_resolved) {
        return NoteVisualSpan{*span, true};
    }
    if (!span->has_sustain) {
        return std::nullopt;
    }
    const bool tail_remains = downscroll
        ? span->tail_y < receptor_y
        : span->tail_y > receptor_y;
    if (!tail_remains) {
        return std::nullopt;
    }
    span->head_y = receptor_y;
    return NoteVisualSpan{*span, false};
}

bool note_intersects_viewport(
    const NoteScreenSpan& span,
    const double viewport_height,
    const double head_half_height
) noexcept {
    if (!std::isfinite(span.head_y) || !std::isfinite(span.tail_y)
        || !std::isfinite(viewport_height)
        || !std::isfinite(head_half_height) || viewport_height <= 0.0
        || head_half_height < 0.0) {
        return false;
    }
    const bool head_visible = span.head_y + head_half_height >= 0.0
        && span.head_y - head_half_height <= viewport_height;
    if (head_visible) {
        return true;
    }
    return span.has_sustain
        && std::max(span.head_y, span.tail_y) >= 0.0
        && std::min(span.head_y, span.tail_y) <= viewport_height;
}

std::optional<DenseNoteRowSpan> clip_dense_note_row_to_receptor(
    const double row_y,
    const double row_height,
    const double receptor_y,
    const bool downscroll
) noexcept {
    if (!std::isfinite(row_y) || !std::isfinite(row_height)
        || !std::isfinite(receptor_y) || row_height <= 0.0) {
        return std::nullopt;
    }
    const double row_end = row_y + row_height;
    if (!std::isfinite(row_end)) {
        return std::nullopt;
    }

    const double clipped_begin = downscroll
        ? row_y
        : std::max(row_y, receptor_y);
    const double clipped_end = downscroll
        ? std::min(row_end, receptor_y)
        : row_end;
    if (clipped_end <= clipped_begin) {
        return std::nullopt;
    }
    return DenseNoteRowSpan{
        clipped_begin,
        clipped_end - clipped_begin,
    };
}

DenseNoteCoverage::DenseNoteCoverage(
    const std::uint16_t lane_count,
    const double viewport_height,
    const double row_height
) : lane_count_(lane_count),
    viewport_height_(viewport_height),
    row_height_(row_height) {
    if (lane_count_ == 0U || !std::isfinite(viewport_height_)
        || !std::isfinite(row_height_) || viewport_height_ <= 0.0
        || row_height_ <= 0.0) {
        lane_count_ = 0U;
        viewport_height_ = 0.0;
        row_height_ = 1.0;
        return;
    }
    row_count_ = static_cast<std::size_t>(
        std::ceil(viewport_height_ / row_height_)
    );
    const auto cell_count = static_cast<std::size_t>(lane_count_)
        * 3U * row_count_;
    cells_.resize(cell_count);
    sustain_starts_.resize(cell_count);
    sustain_ends_.resize(cell_count);
}

bool DenseNoteCoverage::add(
    const NoteOwner owner,
    const std::uint16_t lane,
    const NoteScreenSpan& span,
    const bool hurt_note,
    const bool include_head
) noexcept {
    return add_coincident(
        owner,
        lane,
        span,
        1U,
        hurt_note,
        include_head
    );
}

bool DenseNoteCoverage::add_coincident(
    const NoteOwner owner,
    const std::uint16_t lane,
    const NoteScreenSpan& span,
    const std::uint64_t occurrence_count,
    const bool hurt_note,
    const bool include_head
) noexcept {
    if (finalized_ || lane >= lane_count_
        || occurrence_count == 0U
        || !note_intersects_viewport(span, viewport_height_)) {
        return false;
    }
    represented_note_count_ = saturated_add(
        represented_note_count_,
        occurrence_count
    );

    if (include_head && span.head_y + 19.0 >= 0.0
        && span.head_y - 19.0 <= viewport_height_) {
        const auto head_row = row_at(span.head_y);
        auto& cell_value = cells_[index(owner, lane, head_row)];
        cell_value.head_count = saturated_add(
            cell_value.head_count,
            occurrence_count
        );
        if (hurt_note) {
            cell_value.hurt_head_count = saturated_add(
                cell_value.hurt_head_count,
                occurrence_count
            );
        }
    }

    if (span.has_sustain) {
        const auto clipped_begin = std::clamp(
            std::min(span.head_y, span.tail_y),
            0.0,
            viewport_height_
        );
        const auto clipped_end = std::clamp(
            std::max(span.head_y, span.tail_y),
            0.0,
            viewport_height_
        );
        const auto first_row = row_at(clipped_begin);
        const auto last_row = row_at(clipped_end);
        auto& starts = sustain_starts_[index(owner, lane, first_row)];
        auto& ends = sustain_ends_[index(owner, lane, last_row)];
        starts = saturated_add(starts, occurrence_count);
        ends = saturated_add(ends, occurrence_count);
    }
    return true;
}

void DenseNoteCoverage::reset() noexcept {
    std::fill(cells_.begin(), cells_.end(), DenseNoteCell{});
    std::fill(sustain_starts_.begin(), sustain_starts_.end(), std::uint64_t{0U});
    std::fill(sustain_ends_.begin(), sustain_ends_.end(), std::uint64_t{0U});
    represented_note_count_ = 0U;
    finalized_ = false;
}

void DenseNoteCoverage::finalize() noexcept {
    if (finalized_) {
        return;
    }
    for (std::size_t owner = 0U; owner < 3U; ++owner) {
        const auto note_owner = owner == 0U
            ? NoteOwner::opponent
            : owner == 1U
                ? NoteOwner::player
                : NoteOwner::secondary_opponent;
        for (std::uint16_t lane = 0U; lane < lane_count_; ++lane) {
            std::uint64_t active = 0U;
            for (std::size_t row = 0U; row < row_count_; ++row) {
                const auto current = index(note_owner, lane, row);
                active = saturated_add(active, sustain_starts_[current]);
                cells_[current].sustain_count = active;
                active = sustain_ends_[current] > active
                    ? 0U
                    : active - sustain_ends_[current];
            }
        }
    }
    finalized_ = true;
}

const DenseNoteCell& DenseNoteCoverage::cell(
    const NoteOwner owner,
    const std::uint16_t lane,
    const std::size_t row
) const noexcept {
    static constexpr DenseNoteCell empty{};
    if (lane >= lane_count_ || row >= row_count_) {
        return empty;
    }
    return cells_[index(owner, lane, row)];
}

std::uint16_t DenseNoteCoverage::lane_count() const noexcept {
    return lane_count_;
}

std::size_t DenseNoteCoverage::row_count() const noexcept {
    return row_count_;
}

double DenseNoteCoverage::row_height() const noexcept {
    return row_height_;
}

std::uint64_t DenseNoteCoverage::represented_note_count() const noexcept {
    return represented_note_count_;
}

std::size_t DenseNoteCoverage::index(
    const NoteOwner owner,
    const std::uint16_t lane,
    const std::size_t row
) const noexcept {
    return (owner_index(owner) * static_cast<std::size_t>(lane_count_)
        + static_cast<std::size_t>(lane)) * row_count_ + row;
}

std::size_t DenseNoteCoverage::row_at(const double y) const noexcept {
    if (row_count_ == 0U) {
        return 0U;
    }
    const auto clipped = std::clamp(
        std::isfinite(y) ? y : 0.0,
        0.0,
        std::nextafter(viewport_height_, 0.0)
    );
    return std::min(
        static_cast<std::size_t>(clipped / row_height_),
        row_count_ - 1U
    );
}

}  // namespace pulseforge
