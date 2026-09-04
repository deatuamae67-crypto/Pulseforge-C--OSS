#pragma once

#include "pulseforge/chart.hpp"
#include "pulseforge/editor_models.hpp"
#include "pulseforge/packed_chart.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulseforge {

using StreamingEditorNoteId = std::uint64_t;

struct StreamingEditorViewportNote {
    // Source ids are one-based global logical PFC1 indices: explicit notes
    // first, followed by every PatternRun occurrence. Overlay-only ids have
    // bit 63 set, so even a trillion-note run remains individually addressable.
    StreamingEditorNoteId id{};
    PackedNote note;
    bool from_source{};
};

// When a zoomed-out viewport contains more individually drawable notes than
// requested, every intersecting PFC1 chunk is still represented by a density
// span. The UI therefore shows that content exists instead of silently hiding
// the remainder; zooming in exposes the individual notes for editing.
struct StreamingEditorDensitySpan {
    std::int64_t first_time_us{};
    std::int64_t last_time_us{};
    std::uint64_t note_count{};
};

struct StreamingEditorViewport {
    std::vector<StreamingEditorViewportNote> notes;
    std::vector<StreamingEditorDensitySpan> density_spans;
    bool dense_lod{};
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error.empty();
    }
};

// PFC1-backed chart editor model. Total chart size is never materialized: the
// immutable source stays indexed on disk while edits are held as a small
// overlay and can be saved as a reloadable patch or streamed to compatible
// Psych JSON. PatternRun queries and counts are arithmetic; only explicit JSON
// export is necessarily O(logical note count), because Psych has no compressed
// run representation.
class StreamingChartEditor final {
public:
    StreamingChartEditor(
        PackedChartReader reader,
        Chart metadata,
        std::filesystem::path source_path,
        std::uint64_t content_end_us,
        std::vector<std::string> scripts = {},
        std::uint64_t source_fingerprint = 0U
    );

    [[nodiscard]] const Chart& metadata() const noexcept;
    [[nodiscard]] const std::filesystem::path& source_path() const noexcept;
    [[nodiscard]] const PackedChartReader& reader() const noexcept;
    // Complete note-type dictionary used by the editor overlay. It begins
    // with the immutable PFC1 source dictionary and may append custom Psych
    // note types without rewriting or materializing the source chart.
    [[nodiscard]] std::span<const std::string> note_kinds() const noexcept;
    [[nodiscard]] std::uint16_t key_count() const noexcept;
    [[nodiscard]] std::uint64_t explicit_source_note_count() const noexcept;
    [[nodiscard]] std::uint64_t source_note_count() const noexcept;
    [[nodiscard]] std::uint64_t note_count() const noexcept;
    [[nodiscard]] std::uint64_t content_end_us() const noexcept;
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] const std::vector<std::string>& scripts() const noexcept;

    // Updates the chart-wide note-skin ID without materializing the PFC1
    // source. Unknown IDs are intentionally preserved for Psych/H-Slice/JS
    // round-trips and can be resolved by a mod at runtime.
    [[nodiscard]] bool set_note_style(
        std::string style,
        std::string* error = nullptr
    );

    // Returns a stable kind id, appending a validated custom type when needed.
    // Exact spelling is preserved because Psych/Lua script filenames are
    // case-sensitive on several supported platforms.
    [[nodiscard]] std::optional<std::uint32_t> ensure_note_kind(
        std::string kind,
        std::string* error = nullptr
    );

    [[nodiscard]] StreamingEditorViewport query(
        std::int64_t first_time_us,
        std::int64_t last_time_us,
        std::size_t maximum_individual_notes = 250'000U
    ) const;

    // Resolves a stable editor id without depending on the current viewport.
    // Modal UI can therefore keep audio playing while a picker is open and
    // still edit the originally selected note after the playhead has moved.
    [[nodiscard]] std::optional<PackedNote> note_by_id(
        StreamingEditorNoteId id,
        std::string* error = nullptr
    ) const;

    [[nodiscard]] std::optional<StreamingEditorNoteId> add_note(
        PackedNote note,
        std::string* error = nullptr
    );
    [[nodiscard]] bool update_note(
        StreamingEditorNoteId id,
        PackedNote note,
        std::string* error = nullptr
    );
    [[nodiscard]] bool remove_note(
        StreamingEditorNoteId id,
        std::string* error = nullptr
    );

    [[nodiscard]] EditorIoResult load_patch(
        const EditorStorage& storage,
        const std::filesystem::path& relative_path
    );
    [[nodiscard]] EditorIoResult save_patch(
        const EditorStorage& storage,
        const std::filesystem::path& relative_path
    );
    [[nodiscard]] EditorIoResult export_psych_json(
        const EditorStorage& storage,
        const std::filesystem::path& relative_path
    ) const;

private:
    static constexpr StreamingEditorNoteId added_id_mask = 1ULL << 63U;

    [[nodiscard]] bool validate_note(
        const PackedNote& note,
        std::string& error
    ) const;
    [[nodiscard]] bool is_added_id(StreamingEditorNoteId id) const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> source_index(
        StreamingEditorNoteId id
    ) const noexcept;

    PackedChartReader reader_;
    Chart metadata_;
    std::filesystem::path source_path_;
    std::vector<std::string> note_kinds_;
    std::vector<std::string> scripts_;
    std::uint64_t source_fingerprint_{};
    std::uint64_t content_end_us_{};
    std::unordered_set<std::uint64_t> deleted_source_notes_;
    std::unordered_map<std::uint64_t, PackedNote> updated_source_notes_;
    std::unordered_map<StreamingEditorNoteId, PackedNote> added_notes_;
    StreamingEditorNoteId next_added_id_{added_id_mask | 1ULL};
    std::uint64_t revision_{};
    std::uint64_t saved_revision_{};
};

}  // namespace pulseforge
