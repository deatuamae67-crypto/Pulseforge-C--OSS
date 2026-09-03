#pragma once

#include "pulseforge/chart.hpp"
#include "pulseforge/content_descriptors.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pulseforge {

enum class EditorIoStatus : std::uint8_t {
    ok,
    invalid_root,
    unsafe_path,
    not_found,
    wrong_file_type,
    too_large,
    io_error,
    serialization_error,
    validation_error,
};

struct EditorIoResult {
    EditorIoStatus status{EditorIoStatus::ok};
    std::filesystem::path path;
    std::string message;

    [[nodiscard]] explicit operator bool() const noexcept {
        return status == EditorIoStatus::ok;
    }
};

struct EditorStorageLimits {
    std::uint64_t maximum_read_bytes{512ULL * 1024ULL * 1024ULL};
    // Streaming exports may intentionally exceed the JSON parser's in-memory
    // input budget. This is a disk-usage guard, not an allocation request.
    std::uint64_t maximum_write_bytes{16ULL * 1024ULL * 1024ULL * 1024ULL};
    std::size_t maximum_relative_path_characters{4'096U};
};

using EditorStreamWriter =
    std::function<bool(std::ostream&, std::string&)>;

// Confines every read/write to one canonical root. Writes use a sibling
// temporary file followed by an atomic replace, so a crash cannot leave a
// partially-written chart or descriptor at the destination.
class EditorStorage final {
public:
    explicit EditorStorage(
        std::filesystem::path root,
        EditorStorageLimits limits = {}
    );

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] const std::filesystem::path& root() const noexcept;
    [[nodiscard]] const std::string& initialization_error() const noexcept;

    [[nodiscard]] EditorIoResult read_text(
        const std::filesystem::path& relative_path,
        std::string& output
    ) const;

    [[nodiscard]] EditorIoResult write_atomic(
        const std::filesystem::path& relative_path,
        std::string_view content
    ) const;

    [[nodiscard]] EditorIoResult write_atomic(
        const std::filesystem::path& relative_path,
        const EditorStreamWriter& writer
    ) const;

private:
    struct Resolution;

    [[nodiscard]] Resolution resolve(
        const std::filesystem::path& relative_path,
        bool create_parents
    ) const;

    std::filesystem::path root_;
    EditorStorageLimits limits_;
    std::string initialization_error_;
};

[[nodiscard]] std::string_view to_string(EditorIoStatus status) noexcept;

using EditorEntityId = std::uint64_t;

struct ChartEditorMetadata {
    std::string title{"Untitled"};
    std::string artist{"Unknown"};
    std::string charter{"Unknown"};
    std::string difficulty{"normal"};
    std::string stage_id;
    std::string player_character{"bf"};
    std::string opponent_character{"dad"};
    std::string girlfriend_character{"gf"};
    std::string note_style;
    AudioManifest audio;
};

struct ChartEditorLimits {
    std::size_t maximum_history_entries{4'096U};
    std::size_t maximum_scripts{256U};
    std::size_t maximum_script_path_bytes{1'024U};
};

struct ChartEditorAutosaveSettings {
    bool enabled{true};
    std::filesystem::path relative_path{"autosaves/chart.pfchart.json"};
    std::chrono::milliseconds interval{std::chrono::seconds(30)};
};

enum class EditorAutosaveStatus : std::uint8_t {
    disabled,
    not_dirty,
    not_due,
    saved,
    failed,
};

// Headless model for an in-engine chart editor. The renderer/UI can query the
// immutable maps and issue commands without owning file or history logic.
class ChartEditor final {
public:
    explicit ChartEditor(
        Chart chart = {},
        std::vector<std::string> scripts = {},
        ChartEditorLimits limits = {}
    );
    ~ChartEditor();

    ChartEditor(ChartEditor&&) noexcept;
    ChartEditor& operator=(ChartEditor&&) noexcept;
    ChartEditor(const ChartEditor&) = delete;
    ChartEditor& operator=(const ChartEditor&) = delete;

    void reset(Chart chart = {}, std::vector<std::string> scripts = {});

    [[nodiscard]] const ChartEditorMetadata& metadata() const noexcept;
    [[nodiscard]] std::uint16_t key_count() const noexcept;
    [[nodiscard]] double scroll_speed() const noexcept;
    [[nodiscard]] const std::vector<std::string>& scripts() const noexcept;
    [[nodiscard]] const std::vector<std::string>& note_payloads() const noexcept;
    [[nodiscard]] const std::unordered_map<EditorEntityId, Note>& notes()
        const noexcept;
    [[nodiscard]] const std::unordered_map<EditorEntityId, ChartEvent>& events()
        const noexcept;
    [[nodiscard]] const std::unordered_map<EditorEntityId, TempoChange>& tempos()
        const noexcept;

    [[nodiscard]] std::optional<EditorEntityId> add_note(
        Note note,
        std::string* error = nullptr
    );
    [[nodiscard]] bool update_note(
        EditorEntityId id,
        Note note,
        std::string* error = nullptr
    );
    [[nodiscard]] bool remove_note(
        EditorEntityId id,
        std::string* error = nullptr
    );

    [[nodiscard]] std::optional<EditorEntityId> add_event(
        ChartEvent event,
        std::string* error = nullptr
    );
    [[nodiscard]] bool update_event(
        EditorEntityId id,
        ChartEvent event,
        std::string* error = nullptr
    );
    [[nodiscard]] bool remove_event(
        EditorEntityId id,
        std::string* error = nullptr
    );

    [[nodiscard]] std::optional<EditorEntityId> add_tempo(
        TempoChange tempo,
        std::string* error = nullptr
    );
    [[nodiscard]] bool update_tempo(
        EditorEntityId id,
        TempoChange tempo,
        std::string* error = nullptr
    );
    [[nodiscard]] bool remove_tempo(
        EditorEntityId id,
        std::string* error = nullptr
    );

    [[nodiscard]] bool set_metadata(
        ChartEditorMetadata metadata,
        std::string* error = nullptr
    );
    [[nodiscard]] bool set_key_count(
        std::uint16_t key_count,
        std::string* error = nullptr
    );
    [[nodiscard]] bool set_scroll_speed(
        double scroll_speed,
        std::string* error = nullptr
    );
    [[nodiscard]] bool set_scripts(
        std::vector<std::string> scripts,
        std::string* error = nullptr
    );
    [[nodiscard]] std::optional<std::uint32_t> intern_note_payload(
        std::string payload_json,
        std::string* error = nullptr
    );

    [[nodiscard]] bool can_undo() const noexcept;
    [[nodiscard]] bool can_redo() const noexcept;
    [[nodiscard]] bool undo(std::string* label = nullptr);
    [[nodiscard]] bool redo(std::string* label = nullptr);
    void clear_history() noexcept;

    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    void mark_saved() noexcept;

    [[nodiscard]] Chart to_chart() const;
    [[nodiscard]] std::vector<ValidationIssue> validate() const;

    [[nodiscard]] EditorIoResult save_project(
        const EditorStorage& storage,
        const std::filesystem::path& relative_path
    );
    [[nodiscard]] EditorIoResult load_project(
        const EditorStorage& storage,
        const std::filesystem::path& relative_path
    );
    [[nodiscard]] EditorIoResult save_psych_json(
        const EditorStorage& storage,
        const std::filesystem::path& relative_path
    ) const;
    [[nodiscard]] bool export_psych_json(
        std::ostream& output,
        std::string& error
    ) const;

    void configure_autosave(ChartEditorAutosaveSettings settings);
    [[nodiscard]] const ChartEditorAutosaveSettings& autosave_settings()
        const noexcept;
    [[nodiscard]] EditorAutosaveStatus autosave_if_due(
        const EditorStorage& storage,
        std::chrono::steady_clock::time_point now,
        std::string* error = nullptr
    );
    [[nodiscard]] EditorAutosaveStatus force_autosave(
        const EditorStorage& storage,
        std::string* error = nullptr
    );

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::vector<std::string> validate_character_editor_document(
    const CharacterDescriptor& descriptor
);
[[nodiscard]] std::vector<std::string> validate_week_editor_document(
    const WeekDescriptor& descriptor
);

// Character and week descriptors are small, so their undo entries are
// validated snapshots. Unknown JSON fields survive through extensions_json.
class CharacterEditor final {
public:
    explicit CharacterEditor(
        CharacterDescriptor descriptor = {},
        std::size_t maximum_history_entries = 256U
    );
    ~CharacterEditor();

    CharacterEditor(CharacterEditor&&) noexcept;
    CharacterEditor& operator=(CharacterEditor&&) noexcept;
    CharacterEditor(const CharacterEditor&) = delete;
    CharacterEditor& operator=(const CharacterEditor&) = delete;

    [[nodiscard]] const CharacterDescriptor& document() const noexcept;
    [[nodiscard]] bool replace(
        CharacterDescriptor descriptor,
        std::string label,
        std::string* error = nullptr
    );
    [[nodiscard]] bool undo(std::string* label = nullptr);
    [[nodiscard]] bool redo(std::string* label = nullptr);
    [[nodiscard]] bool dirty() const noexcept;
    void mark_saved() noexcept;

    [[nodiscard]] EditorIoResult load_psych_json(
        const EditorStorage& storage,
        const std::filesystem::path& relative_path,
        std::string_view id
    );
    [[nodiscard]] EditorIoResult save_psych_json(
        const EditorStorage& storage,
        const std::filesystem::path& relative_path
    );

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class WeekEditor final {
public:
    explicit WeekEditor(
        WeekDescriptor descriptor = {},
        std::size_t maximum_history_entries = 256U
    );
    ~WeekEditor();

    WeekEditor(WeekEditor&&) noexcept;
    WeekEditor& operator=(WeekEditor&&) noexcept;
    WeekEditor(const WeekEditor&) = delete;
    WeekEditor& operator=(const WeekEditor&) = delete;

    [[nodiscard]] const WeekDescriptor& document() const noexcept;
    [[nodiscard]] bool replace(
        WeekDescriptor descriptor,
        std::string label,
        std::string* error = nullptr
    );
    [[nodiscard]] bool undo(std::string* label = nullptr);
    [[nodiscard]] bool redo(std::string* label = nullptr);
    [[nodiscard]] bool dirty() const noexcept;
    void mark_saved() noexcept;

    [[nodiscard]] EditorIoResult load_psych_json(
        const EditorStorage& storage,
        const std::filesystem::path& relative_path,
        std::string_view id
    );
    [[nodiscard]] EditorIoResult save_psych_json(
        const EditorStorage& storage,
        const std::filesystem::path& relative_path
    );

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pulseforge
