#pragma once

#include "pulseforge/chart.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace pulseforge {

struct NoteScreenSpan {
    double head_y{};
    double tail_y{};
    bool has_sustain{};
};

struct NoteVisualSpan {
    NoteScreenSpan span;
    bool include_head{true};
};

// Screen-space fragment of one dense LOD row that is still approaching the
// receptor. Dense caches deliberately retain a margin on both sides of the
// viewport, but already-consumed geometry must never travel through the
// receptor when that cache is translated between rebuilds.
struct DenseNoteRowSpan {
    double y{};
    double height{};
};

// Converts chart time to logical screen coordinates. Positive time travels
// down from the top edge in upscroll and up from the bottom edge in
// downscroll, so notes first become visible at the expected viewport edge.
[[nodiscard]] std::optional<NoteScreenSpan> note_screen_span(
    double note_time_ms,
    double duration_ms,
    double visual_time_ms,
    double pixels_per_ms,
    bool downscroll,
    double receptor_y
) noexcept;

// Applies gameplay lifetime to screen geometry. A still-pending head remains
// visible after crossing the receptor so late hits have honest feedback. Once
// the head is resolved it disappears; only an unconsumed sustain tail remains
// and is clipped exactly to the receptor.
[[nodiscard]] std::optional<NoteVisualSpan> visual_note_span(
    double note_time_ms,
    double duration_ms,
    double visual_time_ms,
    double pixels_per_ms,
    bool downscroll,
    double receptor_y,
    bool head_resolved = false
) noexcept;

[[nodiscard]] bool note_intersects_viewport(
    const NoteScreenSpan& span,
    double viewport_height,
    double head_half_height = 19.0
) noexcept;

// Clips a cached dense row to the unconsumed side of the receptor. In
// upscroll, future notes are below the receptor; in downscroll they are above
// it. Returning nullopt means the complete row is already consumed. This is a
// visual lifetime rule only and never participates in input or judgment.
[[nodiscard]] std::optional<DenseNoteRowSpan>
clip_dense_note_row_to_receptor(
    double row_y,
    double row_height,
    double receptor_y,
    bool downscroll
) noexcept;

struct DenseNoteCell {
    std::uint64_t head_count{};
    std::uint64_t hurt_head_count{};
    std::uint64_t sustain_count{};
};

// Fixed-memory visual LOD. Every intersecting note contributes to a head bin
// and/or a sustain coverage interval. Coincident notes are represented by the
// cell's multiplicity instead of being silently discarded. Memory is
// O(owners * lanes * viewport rows), independent of chart density.
class DenseNoteCoverage final {
public:
    DenseNoteCoverage(
        std::uint16_t lane_count,
        double viewport_height,
        double row_height = 2.0
    );

    [[nodiscard]] bool add(
        NoteOwner owner,
        std::uint16_t lane,
        const NoteScreenSpan& span,
        bool hurt_note = false,
        bool include_head = true
    ) noexcept;
    [[nodiscard]] bool add_coincident(
        NoteOwner owner,
        std::uint16_t lane,
        const NoteScreenSpan& span,
        std::uint64_t occurrence_count,
        bool hurt_note = false,
        bool include_head = true
    ) noexcept;
    // Clears accumulated counts without releasing the fixed screen-space
    // storage. Gameplay reuses one coverage grid every frame, avoiding three
    // heap allocations on the render hot path.
    void reset() noexcept;
    void finalize() noexcept;

    [[nodiscard]] const DenseNoteCell& cell(
        NoteOwner owner,
        std::uint16_t lane,
        std::size_t row
    ) const noexcept;
    [[nodiscard]] std::uint16_t lane_count() const noexcept;
    [[nodiscard]] std::size_t row_count() const noexcept;
    [[nodiscard]] double row_height() const noexcept;
    [[nodiscard]] std::uint64_t represented_note_count() const noexcept;

private:
    [[nodiscard]] std::size_t index(
        NoteOwner owner,
        std::uint16_t lane,
        std::size_t row
    ) const noexcept;
    [[nodiscard]] std::size_t row_at(double y) const noexcept;

    std::uint16_t lane_count_{};
    std::size_t row_count_{};
    double viewport_height_{};
    double row_height_{};
    std::vector<DenseNoteCell> cells_;
    std::vector<std::uint64_t> sustain_starts_;
    std::vector<std::uint64_t> sustain_ends_;
    std::uint64_t represented_note_count_{};
    bool finalized_{};
};

}  // namespace pulseforge
