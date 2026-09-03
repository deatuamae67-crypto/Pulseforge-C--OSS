#include "pulseforge/editor_models.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <iomanip>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>

namespace pulseforge {
namespace {

using Json = nlohmann::json;

constexpr double minimum_editor_time_ms = -60'000.0;
constexpr double maximum_editor_time_ms = 12.0 * 60.0 * 60.0 * 1'000.0;
constexpr double editor_time_epsilon = 0.0001;
constexpr std::size_t maximum_metadata_bytes = 1'024U;
constexpr std::size_t maximum_vocal_stems = 8U;

void assign_error(std::string* target, std::string message) {
    if (target != nullptr) {
        *target = std::move(message);
    }
}

[[nodiscard]] bool finite(const double value) noexcept {
    return std::isfinite(value);
}

[[nodiscard]] bool safe_logical_path(
    const std::string_view value,
    const std::size_t maximum_bytes
) {
    if (value.empty() || value.size() > maximum_bytes
        || value.find('\0') != std::string_view::npos) {
        return false;
    }
    std::size_t component_start = 0U;
    for (std::size_t index = 0U; index <= value.size(); ++index) {
        const bool separator = index == value.size() || value[index] == '/'
            || value[index] == '\\';
        if (!separator) {
            const auto character = static_cast<unsigned char>(value[index]);
            if (character < 0x20U || value[index] == ':') {
                return false;
            }
            continue;
        }
        const auto length = index - component_start;
        if (length == 0U) {
            return false;
        }
        const auto component = value.substr(component_start, length);
        if (component == "." || component == "..") {
            return false;
        }
        component_start = index + 1U;
    }
    return true;
}

[[nodiscard]] std::string path_to_utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return std::string(
        reinterpret_cast<const char*>(value.data()),
        value.size()
    );
}

[[nodiscard]] std::filesystem::path path_from_utf8(const std::string& value) {
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(value.data()),
        value.size()
    ));
}

[[nodiscard]] std::string quoted(const std::string_view value) {
    return Json(std::string(value)).dump();
}

[[nodiscard]] bool validate_note_value(
    const Note& note,
    const std::uint16_t key_count,
    const std::vector<std::string>& payloads,
    std::string& error
) {
    if (!finite(note.time_ms) || !finite(note.duration_ms)
        || note.time_ms < minimum_editor_time_ms
        || note.time_ms > maximum_editor_time_ms || note.duration_ms < 0.0
        || note.duration_ms > maximum_editor_time_ms
        || !finite(note.end_time_ms())
        || note.end_time_ms() > maximum_editor_time_ms) {
        error = "note time or duration is outside the finite editor range";
        return false;
    }
    if (note.lane >= key_count) {
        error = "note lane is outside the current key count";
        return false;
    }
    // PULSEFORGE_P1_4_0_EDITOR_SECONDARY_OPPONENT_OWNER_V1
    if (note.owner != NoteOwner::player
        && note.owner != NoteOwner::opponent
        && note.owner != NoteOwner::secondary_opponent) {
        error = "note owner is invalid";
        return false;
    }
    if (!valid_chart_note_kind_text(note.kind)) {
        error = "note kind must be valid UTF-8 without control characters and fit 128 bytes";
        return false;
    }
    if (note.payload_id >= payloads.size()) {
        error = "note payload id is outside the editor payload dictionary";
        return false;
    }
    return true;
}

[[nodiscard]] bool validate_event_value(
    const ChartEvent& event,
    std::string& error
) {
    if (!finite(event.time_ms) || event.time_ms < minimum_editor_time_ms
        || event.time_ms > maximum_editor_time_ms) {
        error = "event time is outside the finite editor range";
        return false;
    }
    if (event.name.size() > maximum_chart_event_name_bytes
        || event.value1.size() > maximum_chart_event_value_bytes
        || event.value2.size() > maximum_chart_event_value_bytes
        || event.payload_json.size() > maximum_chart_event_value_bytes) {
        error = "event text exceeds its configured safety limit";
        return false;
    }
    return true;
}

[[nodiscard]] bool validate_tempo_value(
    const TempoChange& tempo,
    std::string& error
) {
    if (!finite(tempo.time_ms) || !finite(tempo.bpm)
        || tempo.time_ms < minimum_editor_time_ms
        || tempo.time_ms > maximum_editor_time_ms || tempo.bpm <= 0.0
        || tempo.numerator == 0U
        || tempo.denominator == 0U) {
        error = "tempo time, BPM, or signature is outside the editor range";
        return false;
    }
    return true;
}

[[nodiscard]] bool validate_metadata_value(
    const ChartEditorMetadata& metadata,
    std::string& error
) {
    const std::string* strings[] = {
        &metadata.title,
        &metadata.artist,
        &metadata.charter,
        &metadata.difficulty,
        &metadata.stage_id,
        &metadata.player_character,
        &metadata.opponent_character,
        &metadata.girlfriend_character,
        &metadata.note_style,
    };
    for (const auto* value : strings) {
        if (value->size() > maximum_metadata_bytes) {
            error = "chart metadata string exceeds the 1024-byte limit";
            return false;
        }
        if (bounded_chart_text_prefix_bytes(*value, value->size())
            != value->size()) {
            error = "chart metadata must be valid UTF-8 without control characters";
            return false;
        }
    }
    if (metadata.title.empty() || metadata.difficulty.empty()) {
        error = "chart title and difficulty cannot be empty";
        return false;
    }
    if (metadata.audio.vocals.size() > maximum_vocal_stems) {
        error = "audio manifest has more than eight vocal stems";
        return false;
    }
    if (!metadata.audio.instrumental.empty()
        && !safe_logical_path(path_to_utf8(metadata.audio.instrumental), 32'768U)) {
        error = "instrumental path is not a safe relative logical path";
        return false;
    }
    for (const auto& vocal : metadata.audio.vocals) {
        if (!safe_logical_path(path_to_utf8(vocal), 32'768U)) {
            error = "vocal path is not a safe relative logical path";
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool validate_scripts_value(
    const std::vector<std::string>& scripts,
    const ChartEditorLimits& limits,
    std::string& error
) {
    if (scripts.size() > limits.maximum_scripts) {
        error = "script list exceeds the configured item limit";
        return false;
    }
    std::unordered_set<std::string> unique;
    unique.reserve(scripts.size());
    for (const auto& script : scripts) {
        if (!safe_logical_path(script, limits.maximum_script_path_bytes)) {
            error = "script path is empty, unsafe, or too long";
            return false;
        }
        if (!unique.insert(script).second) {
            error = "script list contains a duplicate path";
            return false;
        }
    }
    return true;
}

template <typename Map>
[[nodiscard]] EditorEntityId next_available_id(
    EditorEntityId& next_id,
    const Map& map
) {
    while (next_id == 0U || map.contains(next_id)) {
        if (next_id == std::numeric_limits<EditorEntityId>::max()) {
            throw std::overflow_error("editor entity id space is exhausted");
        }
        ++next_id;
    }
    return next_id++;
}

template <typename Map>
[[nodiscard]] auto sorted_entity_pointers(const Map& values) {
    using Value = typename Map::value_type;
    std::vector<const Value*> sorted;
    sorted.reserve(values.size());
    for (const auto& value : values) {
        sorted.push_back(&value);
    }
    std::sort(sorted.begin(), sorted.end(), [](const Value* left, const Value* right) {
        const auto& left_value = left->second;
        const auto& right_value = right->second;
        if (left_value.time_ms != right_value.time_ms) {
            return left_value.time_ms < right_value.time_ms;
        }
        return left->first < right->first;
    });
    return sorted;
}

struct NoteChange {
    EditorEntityId id{};
    std::optional<Note> before;
    std::optional<Note> after;
};

struct EventChange {
    EditorEntityId id{};
    std::optional<ChartEvent> before;
    std::optional<ChartEvent> after;
};

struct TempoChangeCommand {
    EditorEntityId id{};
    std::optional<TempoChange> before;
    std::optional<TempoChange> after;
};

struct MetadataChange {
    ChartEditorMetadata before;
    ChartEditorMetadata after;
};

struct KeyCountChange {
    std::uint16_t before{};
    std::uint16_t after{};
};

struct ScrollChange {
    double before{};
    double after{};
};

struct ScriptsChange {
    std::vector<std::string> before;
    std::vector<std::string> after;
};

using ChartChange = std::variant<
    NoteChange,
    EventChange,
    TempoChangeCommand,
    MetadataChange,
    KeyCountChange,
    ScrollChange,
    ScriptsChange
>;

struct HistoryEntry {
    std::string label;
    ChartChange change;
    std::uint64_t before_state{};
    std::uint64_t after_state{};
};

[[nodiscard]] ChartEditorMetadata metadata_from_chart(const Chart& chart) {
    return ChartEditorMetadata{
        chart.title,
        chart.artist,
        chart.charter,
        chart.difficulty,
        chart.stage_id,
        chart.player_character,
        chart.opponent_character,
        chart.girlfriend_character,
        chart.note_style,
        chart.audio,
    };
}

void write_json_string_array(
    std::ostream& output,
    const std::vector<std::string>& values
) {
    output << '[';
    for (std::size_t index = 0U; index < values.size(); ++index) {
        if (index != 0U) {
            output << ',';
        }
        output << quoted(values[index]);
    }
    output << ']';
}

[[nodiscard]] EditorIoResult validation_failure(
    const std::filesystem::path& path,
    std::string message
) {
    return EditorIoResult{
        EditorIoStatus::validation_error,
        path,
        std::move(message),
    };
}

}  // namespace

struct ChartEditor::Impl {
    explicit Impl(ChartEditorLimits requested_limits)
        : limits(std::move(requested_limits)) {
        if (limits.maximum_history_entries == 0U
            || limits.maximum_scripts == 0U
            || limits.maximum_script_path_bytes == 0U) {
            throw std::invalid_argument(
                "chart editor limits must be greater than zero"
            );
        }
    }

    void apply(const ChartChange& change, const bool forward) {
        std::visit(
            [this, forward](const auto& typed) {
                using Type = std::decay_t<decltype(typed)>;
                if constexpr (std::is_same_v<Type, NoteChange>) {
                    const auto& value = forward ? typed.after : typed.before;
                    if (value.has_value()) {
                        notes[typed.id] = *value;
                    } else {
                        notes.erase(typed.id);
                    }
                } else if constexpr (std::is_same_v<Type, EventChange>) {
                    const auto& value = forward ? typed.after : typed.before;
                    if (value.has_value()) {
                        events[typed.id] = *value;
                    } else {
                        events.erase(typed.id);
                    }
                } else if constexpr (std::is_same_v<Type, TempoChangeCommand>) {
                    const auto& value = forward ? typed.after : typed.before;
                    if (value.has_value()) {
                        tempos[typed.id] = *value;
                    } else {
                        tempos.erase(typed.id);
                    }
                } else if constexpr (std::is_same_v<Type, MetadataChange>) {
                    metadata = forward ? typed.after : typed.before;
                } else if constexpr (std::is_same_v<Type, KeyCountChange>) {
                    key_count = forward ? typed.after : typed.before;
                } else if constexpr (std::is_same_v<Type, ScrollChange>) {
                    scroll_speed = forward ? typed.after : typed.before;
                } else if constexpr (std::is_same_v<Type, ScriptsChange>) {
                    scripts = forward ? typed.after : typed.before;
                }
            },
            change
        );
    }

    void commit(ChartChange change, std::string label) {
        apply(change, true);
        redo_stack.clear();
        const auto after = next_state_id++;
        undo_stack.push_back(HistoryEntry{
            std::move(label),
            std::move(change),
            current_state_id,
            after,
        });
        current_state_id = after;
        ++revision;
        if (undo_stack.size() > limits.maximum_history_entries) {
            undo_stack.pop_front();
        }
    }

    ChartEditorLimits limits;
    ChartEditorMetadata metadata;
    std::uint16_t key_count{4U};
    double scroll_speed{1.0};
    std::vector<std::string> scripts;
    std::vector<std::string> note_payloads{""};
    std::unordered_map<EditorEntityId, Note> notes;
    std::unordered_map<EditorEntityId, ChartEvent> events;
    std::unordered_map<EditorEntityId, TempoChange> tempos;
    EditorEntityId next_entity_id{1U};
    std::deque<HistoryEntry> undo_stack;
    std::deque<HistoryEntry> redo_stack;
    std::uint64_t current_state_id{1U};
    std::uint64_t saved_state_id{1U};
    std::uint64_t next_state_id{2U};
    std::uint64_t revision{};
    ChartEditorAutosaveSettings autosave;
    std::uint64_t last_autosaved_state_id{1U};
    std::chrono::steady_clock::time_point last_autosave_time{};
    bool has_autosave_time{};
};

ChartEditor::ChartEditor(
    Chart chart,
    std::vector<std::string> scripts,
    ChartEditorLimits limits
) : impl_(std::make_unique<Impl>(std::move(limits))) {
    reset(std::move(chart), std::move(scripts));
}

ChartEditor::~ChartEditor() = default;
ChartEditor::ChartEditor(ChartEditor&&) noexcept = default;
ChartEditor& ChartEditor::operator=(ChartEditor&&) noexcept = default;

void ChartEditor::reset(Chart chart, std::vector<std::string> scripts) {
    chart.normalize();
    std::string error;
    if (!validate_scripts_value(scripts, impl_->limits, error)) {
        throw std::invalid_argument(error);
    }
    const auto metadata = metadata_from_chart(chart);
    if (!validate_metadata_value(metadata, error)
        || chart.key_count == 0U || chart.key_count > 18U
        || !finite(chart.chart_scroll_speed)
        || chart.chart_scroll_speed <= 0.0
        || chart.chart_scroll_speed > 100.0
        || chart.tempos.empty()
        || chart.tempos.size() > maximum_chart_tempo_changes
        || chart.notes.size() > maximum_chart_notes
        || chart.events.size() > maximum_chart_events
        || chart.note_payloads.empty()
        || !chart.note_payloads.front().empty()) {
        throw std::invalid_argument(
            error.empty() ? "invalid chart editor document" : error
        );
    }
    for (const auto& payload : chart.note_payloads) {
        if (payload.size() > maximum_chart_note_payload_bytes) {
            throw std::invalid_argument("chart note payload exceeds its safety limit");
        }
    }
    for (const auto& tempo : chart.tempos) {
        if (!validate_tempo_value(tempo, error)) {
            throw std::invalid_argument(error);
        }
    }
    for (const auto& note : chart.notes) {
        if (!validate_note_value(
                note,
                chart.key_count,
                chart.note_payloads,
                error
            )) {
            throw std::invalid_argument(error);
        }
    }
    for (const auto& event : chart.events) {
        if (!validate_event_value(event, error)) {
            throw std::invalid_argument(error);
        }
    }

    impl_->metadata = metadata;
    impl_->key_count = chart.key_count;
    impl_->scroll_speed = chart.chart_scroll_speed;
    impl_->scripts = std::move(scripts);
    impl_->note_payloads = std::move(chart.note_payloads);
    if (impl_->note_payloads.empty()) {
        impl_->note_payloads.emplace_back();
    }
    impl_->notes.clear();
    impl_->events.clear();
    impl_->tempos.clear();
    impl_->notes.reserve(chart.notes.size());
    impl_->events.reserve(chart.events.size());
    impl_->tempos.reserve(chart.tempos.size());
    impl_->next_entity_id = 1U;
    for (auto& tempo : chart.tempos) {
        impl_->tempos.emplace(impl_->next_entity_id++, std::move(tempo));
    }
    for (auto& note : chart.notes) {
        impl_->notes.emplace(impl_->next_entity_id++, std::move(note));
    }
    for (auto& event : chart.events) {
        impl_->events.emplace(impl_->next_entity_id++, std::move(event));
    }
    impl_->undo_stack.clear();
    impl_->redo_stack.clear();
    impl_->current_state_id = impl_->next_state_id++;
    impl_->saved_state_id = impl_->current_state_id;
    impl_->last_autosaved_state_id = impl_->current_state_id;
    impl_->revision = 0U;
    impl_->has_autosave_time = false;
}

const ChartEditorMetadata& ChartEditor::metadata() const noexcept {
    return impl_->metadata;
}

std::uint16_t ChartEditor::key_count() const noexcept {
    return impl_->key_count;
}

double ChartEditor::scroll_speed() const noexcept {
    return impl_->scroll_speed;
}

const std::vector<std::string>& ChartEditor::scripts() const noexcept {
    return impl_->scripts;
}

const std::vector<std::string>& ChartEditor::note_payloads() const noexcept {
    return impl_->note_payloads;
}

const std::unordered_map<EditorEntityId, Note>& ChartEditor::notes()
    const noexcept {
    return impl_->notes;
}

const std::unordered_map<EditorEntityId, ChartEvent>& ChartEditor::events()
    const noexcept {
    return impl_->events;
}

const std::unordered_map<EditorEntityId, TempoChange>& ChartEditor::tempos()
    const noexcept {
    return impl_->tempos;
}

std::optional<EditorEntityId> ChartEditor::add_note(
    Note note,
    std::string* error
) {
    std::string validation_error;
    if (!validate_note_value(
            note,
            impl_->key_count,
            impl_->note_payloads,
            validation_error
        )) {
        assign_error(error, std::move(validation_error));
        return std::nullopt;
    }
    if (impl_->notes.size() >= maximum_chart_notes) {
        assign_error(error, "chart note limit has been reached");
        return std::nullopt;
    }
    const auto id = next_available_id(impl_->next_entity_id, impl_->notes);
    impl_->commit(NoteChange{id, std::nullopt, std::move(note)}, "Add note");
    return id;
}

bool ChartEditor::update_note(
    const EditorEntityId id,
    Note note,
    std::string* error
) {
    const auto found = impl_->notes.find(id);
    if (found == impl_->notes.end()) {
        assign_error(error, "note id does not exist");
        return false;
    }
    std::string validation_error;
    if (!validate_note_value(
            note,
            impl_->key_count,
            impl_->note_payloads,
            validation_error
        )) {
        assign_error(error, std::move(validation_error));
        return false;
    }
    impl_->commit(NoteChange{id, found->second, std::move(note)}, "Edit note");
    return true;
}

bool ChartEditor::remove_note(
    const EditorEntityId id,
    std::string* error
) {
    const auto found = impl_->notes.find(id);
    if (found == impl_->notes.end()) {
        assign_error(error, "note id does not exist");
        return false;
    }
    impl_->commit(NoteChange{id, found->second, std::nullopt}, "Remove note");
    return true;
}

std::optional<EditorEntityId> ChartEditor::add_event(
    ChartEvent event,
    std::string* error
) {
    std::string validation_error;
    if (!validate_event_value(event, validation_error)) {
        assign_error(error, std::move(validation_error));
        return std::nullopt;
    }
    if (impl_->events.size() >= maximum_chart_events) {
        assign_error(error, "chart event limit has been reached");
        return std::nullopt;
    }
    const auto id = next_available_id(impl_->next_entity_id, impl_->events);
    impl_->commit(EventChange{id, std::nullopt, std::move(event)}, "Add event");
    return id;
}

bool ChartEditor::update_event(
    const EditorEntityId id,
    ChartEvent event,
    std::string* error
) {
    const auto found = impl_->events.find(id);
    if (found == impl_->events.end()) {
        assign_error(error, "event id does not exist");
        return false;
    }
    std::string validation_error;
    if (!validate_event_value(event, validation_error)) {
        assign_error(error, std::move(validation_error));
        return false;
    }
    impl_->commit(EventChange{id, found->second, std::move(event)}, "Edit event");
    return true;
}

bool ChartEditor::remove_event(
    const EditorEntityId id,
    std::string* error
) {
    const auto found = impl_->events.find(id);
    if (found == impl_->events.end()) {
        assign_error(error, "event id does not exist");
        return false;
    }
    impl_->commit(EventChange{id, found->second, std::nullopt}, "Remove event");
    return true;
}

std::optional<EditorEntityId> ChartEditor::add_tempo(
    TempoChange tempo,
    std::string* error
) {
    std::string validation_error;
    if (!validate_tempo_value(tempo, validation_error)) {
        assign_error(error, std::move(validation_error));
        return std::nullopt;
    }
    if (impl_->tempos.size() >= maximum_chart_tempo_changes) {
        assign_error(error, "chart tempo-change limit has been reached");
        return std::nullopt;
    }
    for (const auto& [unused_id, existing] : impl_->tempos) {
        (void)unused_id;
        if (std::abs(existing.time_ms - tempo.time_ms) < editor_time_epsilon) {
            assign_error(error, "a tempo change already exists at this time");
            return std::nullopt;
        }
    }
    const auto id = next_available_id(impl_->next_entity_id, impl_->tempos);
    impl_->commit(
        TempoChangeCommand{id, std::nullopt, std::move(tempo)},
        "Add BPM change"
    );
    return id;
}

bool ChartEditor::update_tempo(
    const EditorEntityId id,
    TempoChange tempo,
    std::string* error
) {
    const auto found = impl_->tempos.find(id);
    if (found == impl_->tempos.end()) {
        assign_error(error, "tempo-change id does not exist");
        return false;
    }
    std::string validation_error;
    if (!validate_tempo_value(tempo, validation_error)) {
        assign_error(error, std::move(validation_error));
        return false;
    }
    for (const auto& [existing_id, existing] : impl_->tempos) {
        if (existing_id != id
            && std::abs(existing.time_ms - tempo.time_ms)
                < editor_time_epsilon) {
            assign_error(error, "a tempo change already exists at this time");
            return false;
        }
    }
    impl_->commit(
        TempoChangeCommand{id, found->second, std::move(tempo)},
        "Edit BPM change"
    );
    return true;
}

bool ChartEditor::remove_tempo(
    const EditorEntityId id,
    std::string* error
) {
    const auto found = impl_->tempos.find(id);
    if (found == impl_->tempos.end()) {
        assign_error(error, "tempo-change id does not exist");
        return false;
    }
    if (impl_->tempos.size() <= 1U) {
        assign_error(error, "a chart must retain at least one tempo change");
        return false;
    }
    impl_->commit(
        TempoChangeCommand{id, found->second, std::nullopt},
        "Remove BPM change"
    );
    return true;
}

bool ChartEditor::set_metadata(
    ChartEditorMetadata metadata,
    std::string* error
) {
    std::string validation_error;
    if (!validate_metadata_value(metadata, validation_error)) {
        assign_error(error, std::move(validation_error));
        return false;
    }
    impl_->commit(
        MetadataChange{impl_->metadata, std::move(metadata)},
        "Edit chart metadata"
    );
    return true;
}

bool ChartEditor::set_key_count(
    const std::uint16_t key_count,
    std::string* error
) {
    if (key_count == 0U || key_count > 18U) {
        assign_error(error, "key count must be between 1 and 18");
        return false;
    }
    for (const auto& [id, note] : impl_->notes) {
        (void)id;
        if (note.lane >= key_count) {
            assign_error(
                error,
                "new key count would place an existing note outside its lanes"
            );
            return false;
        }
    }
    impl_->commit(
        KeyCountChange{impl_->key_count, key_count},
        "Change key count"
    );
    return true;
}

bool ChartEditor::set_scroll_speed(
    const double scroll_speed,
    std::string* error
) {
    if (!finite(scroll_speed) || scroll_speed <= 0.0
        || scroll_speed > 100.0) {
        assign_error(error, "scroll speed must be finite and between 0 and 100");
        return false;
    }
    impl_->commit(
        ScrollChange{impl_->scroll_speed, scroll_speed},
        "Change scroll speed"
    );
    return true;
}

bool ChartEditor::set_scripts(
    std::vector<std::string> scripts,
    std::string* error
) {
    std::string validation_error;
    if (!validate_scripts_value(scripts, impl_->limits, validation_error)) {
        assign_error(error, std::move(validation_error));
        return false;
    }
    impl_->commit(
        ScriptsChange{impl_->scripts, std::move(scripts)},
        "Edit chart scripts"
    );
    return true;
}

std::optional<std::uint32_t> ChartEditor::intern_note_payload(
    std::string payload_json,
    std::string* error
) {
    if (payload_json.size() > maximum_chart_note_payload_bytes) {
        assign_error(error, "note payload exceeds the 4096-byte limit");
        return std::nullopt;
    }
    if (!payload_json.empty()) {
        try {
            payload_json = Json::parse(payload_json).dump();
        } catch (const std::exception& exception) {
            assign_error(
                error,
                "note payload is not valid JSON: " + std::string(exception.what())
            );
            return std::nullopt;
        }
    }
    const auto found = std::find(
        impl_->note_payloads.begin(),
        impl_->note_payloads.end(),
        payload_json
    );
    if (found != impl_->note_payloads.end()) {
        return static_cast<std::uint32_t>(
            std::distance(impl_->note_payloads.begin(), found)
        );
    }
    if (impl_->note_payloads.size()
        >= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        assign_error(error, "note payload dictionary id space is exhausted");
        return std::nullopt;
    }
    impl_->note_payloads.push_back(std::move(payload_json));
    // Interning alone is not a semantic edit. The add/update command that
    // references this id is the history and dirty-state boundary.
    return static_cast<std::uint32_t>(impl_->note_payloads.size() - 1U);
}

bool ChartEditor::can_undo() const noexcept {
    return !impl_->undo_stack.empty();
}

bool ChartEditor::can_redo() const noexcept {
    return !impl_->redo_stack.empty();
}

bool ChartEditor::undo(std::string* label) {
    if (impl_->undo_stack.empty()) {
        return false;
    }
    auto entry = std::move(impl_->undo_stack.back());
    impl_->undo_stack.pop_back();
    impl_->apply(entry.change, false);
    impl_->current_state_id = entry.before_state;
    ++impl_->revision;
    if (label != nullptr) {
        *label = entry.label;
    }
    impl_->redo_stack.push_back(std::move(entry));
    return true;
}

bool ChartEditor::redo(std::string* label) {
    if (impl_->redo_stack.empty()) {
        return false;
    }
    auto entry = std::move(impl_->redo_stack.back());
    impl_->redo_stack.pop_back();
    impl_->apply(entry.change, true);
    impl_->current_state_id = entry.after_state;
    ++impl_->revision;
    if (label != nullptr) {
        *label = entry.label;
    }
    impl_->undo_stack.push_back(std::move(entry));
    return true;
}

void ChartEditor::clear_history() noexcept {
    impl_->undo_stack.clear();
    impl_->redo_stack.clear();
}

bool ChartEditor::dirty() const noexcept {
    return impl_->current_state_id != impl_->saved_state_id;
}

std::uint64_t ChartEditor::revision() const noexcept {
    return impl_->revision;
}

void ChartEditor::mark_saved() noexcept {
    impl_->saved_state_id = impl_->current_state_id;
}

Chart ChartEditor::to_chart() const {
    Chart chart;
    chart.title = impl_->metadata.title;
    chart.artist = impl_->metadata.artist;
    chart.charter = impl_->metadata.charter;
    chart.difficulty = impl_->metadata.difficulty;
    chart.stage_id = impl_->metadata.stage_id;
    chart.player_character = impl_->metadata.player_character;
    chart.opponent_character = impl_->metadata.opponent_character;
    chart.girlfriend_character = impl_->metadata.girlfriend_character;
    chart.note_style = impl_->metadata.note_style;
    chart.audio = impl_->metadata.audio;
    chart.source_format = ChartFormat::native;
    chart.key_count = impl_->key_count;
    chart.chart_scroll_speed = impl_->scroll_speed;
    chart.note_payloads = {""};
    chart.notes.reserve(impl_->notes.size());
    chart.events.reserve(impl_->events.size());
    chart.tempos.reserve(impl_->tempos.size());
    std::unordered_map<std::uint32_t, std::uint32_t> payload_remap;
    payload_remap.emplace(0U, 0U);
    payload_remap.reserve(impl_->note_payloads.size());
    for (const auto& [id, source_note] : impl_->notes) {
        (void)id;
        auto note = source_note;
        if (note.payload_id != 0U) {
            const auto existing = payload_remap.find(note.payload_id);
            if (existing != payload_remap.end()) {
                note.payload_id = existing->second;
            } else {
                const auto& payload = impl_->note_payloads.at(note.payload_id);
                if (payload.empty()) {
                    note.payload_id = 0U;
                    payload_remap.emplace(source_note.payload_id, 0U);
                } else {
                    const auto remapped = static_cast<std::uint32_t>(
                        chart.note_payloads.size()
                    );
                    chart.note_payloads.push_back(payload);
                    payload_remap.emplace(source_note.payload_id, remapped);
                    note.payload_id = remapped;
                }
            }
        }
        chart.notes.push_back(std::move(note));
    }
    for (const auto& [id, event] : impl_->events) {
        (void)id;
        chart.events.push_back(event);
    }
    for (const auto& [id, tempo] : impl_->tempos) {
        (void)id;
        chart.tempos.push_back(tempo);
    }
    chart.normalize();
    return chart;
}

std::vector<ValidationIssue> ChartEditor::validate() const {
    return validate_chart(to_chart());
}

EditorIoResult ChartEditor::save_project(
    const EditorStorage& storage,
    const std::filesystem::path& relative_path
) {
    // Project/autosave files intentionally accept an editor's intermediate
    // state. Every command already validates its own value, and avoiding a
    // full Chart copy here is essential for multi-million-note autosaves.
    const auto result = storage.write_atomic(
        relative_path,
        [this](std::ostream& output, std::string& error) {
            output << std::setprecision(17);
            output << "{\"format\":\"pulseforge-editor-chart\",\"version\":1";
            output << ",\"metadata\":{";
            output << "\"title\":" << quoted(impl_->metadata.title);
            output << ",\"artist\":" << quoted(impl_->metadata.artist);
            output << ",\"charter\":" << quoted(impl_->metadata.charter);
            output << ",\"difficulty\":" << quoted(impl_->metadata.difficulty);
            output << ",\"stage\":" << quoted(impl_->metadata.stage_id);
            output << ",\"player\":"
                   << quoted(impl_->metadata.player_character);
            output << ",\"opponent\":"
                   << quoted(impl_->metadata.opponent_character);
            output << ",\"girlfriend\":"
                   << quoted(impl_->metadata.girlfriend_character);
            output << ",\"noteStyle\":" << quoted(impl_->metadata.note_style);
            output << ",\"instrumental\":"
                   << quoted(path_to_utf8(impl_->metadata.audio.instrumental));
            output << ",\"vocals\":[";
            for (std::size_t index = 0U;
                 index < impl_->metadata.audio.vocals.size();
                 ++index) {
                if (index != 0U) {
                    output << ',';
                }
                output << quoted(path_to_utf8(impl_->metadata.audio.vocals[index]));
            }
            output << "]}";
            output << ",\"keyCount\":" << impl_->key_count;
            output << ",\"scrollSpeed\":" << impl_->scroll_speed;
            output << ",\"scripts\":";
            write_json_string_array(output, impl_->scripts);
            output << ",\"notePayloads\":";
            write_json_string_array(output, impl_->note_payloads);

            const auto tempos = sorted_entity_pointers(impl_->tempos);
            output << ",\"tempos\":[";
            for (std::size_t index = 0U; index < tempos.size(); ++index) {
                if (index != 0U) {
                    output << ',';
                }
                const auto& [id, tempo] = *tempos[index];
                output << "{\"id\":" << id << ",\"time\":"
                       << tempo.time_ms << ",\"bpm\":" << tempo.bpm
                       << ",\"numerator\":" << tempo.numerator
                       << ",\"denominator\":" << tempo.denominator << '}';
            }
            output << ']';

            const auto notes = sorted_entity_pointers(impl_->notes);
            output << ",\"notes\":[";
            for (std::size_t index = 0U; index < notes.size(); ++index) {
                if (index != 0U) {
                    output << ',';
                }
                const auto& [id, note] = *notes[index];
                output << "{\"id\":" << id << ",\"time\":"
                       << note.time_ms << ",\"duration\":" << note.duration_ms
                       << ",\"lane\":" << note.lane << ",\"owner\":"
                       << quoted(
                              note.owner == NoteOwner::player
                                  ? "player"
                                  : note.owner == NoteOwner::secondary_opponent
                                      ? "secondary_opponent"
                                      : "opponent"
                          )
                       << ",\"kind\":" << quoted(note.kind)
                       << ",\"payloadId\":" << note.payload_id << '}';
            }
            output << ']';

            const auto events = sorted_entity_pointers(impl_->events);
            output << ",\"events\":[";
            for (std::size_t index = 0U; index < events.size(); ++index) {
                if (index != 0U) {
                    output << ',';
                }
                const auto& [id, event] = *events[index];
                output << "{\"id\":" << id << ",\"time\":"
                       << event.time_ms << ",\"name\":" << quoted(event.name)
                       << ",\"value1\":" << quoted(event.value1)
                       << ",\"value2\":" << quoted(event.value2)
                       << ",\"payload\":" << quoted(event.payload_json)
                       << '}';
            }
            output << "]}";
            if (!output) {
                error = "failed while streaming editor project JSON";
                return false;
            }
            return true;
        }
    );
    if (result) {
        mark_saved();
    }
    return result;
}

EditorIoResult ChartEditor::load_project(
    const EditorStorage& storage,
    const std::filesystem::path& relative_path
) {
    std::string text;
    const auto read_result = storage.read_text(relative_path, text);
    if (!read_result) {
        return read_result;
    }

    try {
        const auto root = Json::parse(text);
        if (!root.is_object()
            || root.value("format", std::string{})
                != "pulseforge-editor-chart"
            || root.value("version", 0) != 1) {
            return validation_failure(
                read_result.path,
                "file is not a supported PulseForge editor project"
            );
        }
        const auto& metadata_json = root.at("metadata");
        if (!metadata_json.is_object()) {
            return validation_failure(
                read_result.path,
                "editor project metadata must be an object"
            );
        }

        ChartEditorMetadata metadata;
        metadata.title = metadata_json.at("title").get<std::string>();
        metadata.artist = metadata_json.at("artist").get<std::string>();
        metadata.charter = metadata_json.at("charter").get<std::string>();
        metadata.difficulty = metadata_json.at("difficulty").get<std::string>();
        metadata.stage_id = metadata_json.at("stage").get<std::string>();
        metadata.player_character = metadata_json.at("player").get<std::string>();
        metadata.opponent_character =
            metadata_json.at("opponent").get<std::string>();
        metadata.girlfriend_character =
            metadata_json.at("girlfriend").get<std::string>();
        metadata.note_style = metadata_json.at("noteStyle").get<std::string>();
        const auto instrumental =
            metadata_json.at("instrumental").get<std::string>();
        if (!instrumental.empty()) {
            metadata.audio.instrumental = path_from_utf8(instrumental);
        }
        const auto& vocals = metadata_json.at("vocals");
        if (!vocals.is_array() || vocals.size() > maximum_vocal_stems) {
            return validation_failure(
                read_result.path,
                "editor project vocals must be a bounded array"
            );
        }
        for (const auto& vocal : vocals) {
            metadata.audio.vocals.push_back(
                path_from_utf8(vocal.get<std::string>())
            );
        }

        std::string validation_error;
        if (!validate_metadata_value(metadata, validation_error)) {
            return validation_failure(read_result.path, validation_error);
        }

        const auto key_count_value = root.at("keyCount").get<std::uint64_t>();
        if (key_count_value == 0U || key_count_value > 18U) {
            return validation_failure(
                read_result.path,
                "editor project key count must be between 1 and 18"
            );
        }
        const auto key_count = static_cast<std::uint16_t>(key_count_value);
        const auto scroll_speed = root.at("scrollSpeed").get<double>();
        if (!finite(scroll_speed) || scroll_speed <= 0.0
            || scroll_speed > 100.0) {
            return validation_failure(
                read_result.path,
                "editor project scroll speed is outside the supported range"
            );
        }

        const auto& script_json = root.at("scripts");
        if (!script_json.is_array()
            || script_json.size() > impl_->limits.maximum_scripts) {
            return validation_failure(
                read_result.path,
                "editor project scripts must be a bounded array"
            );
        }
        const auto scripts = script_json.get<std::vector<std::string>>();
        if (!validate_scripts_value(scripts, impl_->limits, validation_error)) {
            return validation_failure(read_result.path, validation_error);
        }
        const auto& payload_json = root.at("notePayloads");
        if (!payload_json.is_array()
            || payload_json.size() > maximum_chart_notes + 1U) {
            return validation_failure(
                read_result.path,
                "editor project payload dictionary is not a bounded array"
            );
        }
        const auto payloads = payload_json.get<std::vector<std::string>>();
        if (payloads.empty() || !payloads.front().empty()) {
            return validation_failure(
                read_result.path,
                "editor project payload dictionary must start with an empty entry"
            );
        }
        for (const auto& payload : payloads) {
            if (payload.size() > maximum_chart_note_payload_bytes) {
                return validation_failure(
                    read_result.path,
                    "editor project note payload exceeds its safety limit"
                );
            }
        }

        const auto& tempo_json = root.at("tempos");
        const auto& note_json = root.at("notes");
        const auto& event_json = root.at("events");
        if (!tempo_json.is_array() || !note_json.is_array()
            || !event_json.is_array()
            || tempo_json.empty()
            || tempo_json.size() > maximum_chart_tempo_changes
            || note_json.size() > maximum_chart_notes
            || event_json.size() > maximum_chart_events) {
            return validation_failure(
                read_result.path,
                "editor project arrays exceed their configured limits"
            );
        }

        std::unordered_map<EditorEntityId, TempoChange> tempos;
        std::unordered_map<EditorEntityId, Note> notes;
        std::unordered_map<EditorEntityId, ChartEvent> events;
        tempos.reserve(tempo_json.size());
        notes.reserve(note_json.size());
        events.reserve(event_json.size());
        std::unordered_set<EditorEntityId> ids;
        ids.reserve(tempo_json.size() + note_json.size() + event_json.size());
        EditorEntityId greatest_id = 0U;

        std::vector<double> tempo_times;
        tempo_times.reserve(tempo_json.size());

        for (const auto& item : tempo_json) {
            const auto id = item.at("id").get<EditorEntityId>();
            TempoChange tempo{
                item.at("time").get<double>(),
                item.at("bpm").get<double>(),
                item.at("numerator").get<std::uint16_t>(),
                item.at("denominator").get<std::uint16_t>(),
            };
            const bool unique_id = id != 0U && ids.insert(id).second;
            const bool valid_value =
                validate_tempo_value(tempo, validation_error);
            if (!unique_id || !valid_value) {
                return validation_failure(
                    read_result.path,
                    !unique_id
                        ? "editor project contains an invalid or duplicate entity id"
                        : validation_error
                );
            }
            greatest_id = std::max(greatest_id, id);
            tempos.emplace(id, tempo);
            tempo_times.push_back(tempo.time_ms);
        }
        for (const auto& item : note_json) {
            const auto id = item.at("id").get<EditorEntityId>();
            const auto owner = item.at("owner").get<std::string>();
            if (owner != "player" && owner != "opponent"
                && owner != "secondary_opponent") {
                return validation_failure(
                    read_result.path,
                    "editor project note has an invalid owner"
                );
            }
            Note note{
                item.at("time").get<double>(),
                item.at("duration").get<double>(),
                item.at("lane").get<std::uint16_t>(),
                owner == "player"
                    ? NoteOwner::player
                    : owner == "secondary_opponent"
                        ? NoteOwner::secondary_opponent
                        : NoteOwner::opponent,
                item.at("kind").get<std::string>(),
                item.at("payloadId").get<std::uint32_t>(),
            };
            const bool unique_id = id != 0U && ids.insert(id).second;
            const bool valid_value = validate_note_value(
                    note,
                    key_count,
                    payloads,
                    validation_error
                );
            if (!unique_id || !valid_value) {
                return validation_failure(
                    read_result.path,
                    !unique_id
                        ? "editor project contains an invalid or duplicate entity id"
                        : validation_error
                );
            }
            greatest_id = std::max(greatest_id, id);
            notes.emplace(id, std::move(note));
        }
        for (const auto& item : event_json) {
            const auto id = item.at("id").get<EditorEntityId>();
            ChartEvent event{
                item.at("time").get<double>(),
                item.at("name").get<std::string>(),
                item.at("value1").get<std::string>(),
                item.at("value2").get<std::string>(),
                item.at("payload").get<std::string>(),
            };
            const bool unique_id = id != 0U && ids.insert(id).second;
            const bool valid_value = validate_event_value(event, validation_error);
            if (!unique_id || !valid_value) {
                return validation_failure(
                    read_result.path,
                    !unique_id
                        ? "editor project contains an invalid or duplicate entity id"
                        : validation_error
                );
            }
            greatest_id = std::max(greatest_id, id);
            events.emplace(id, std::move(event));
        }

        std::sort(tempo_times.begin(), tempo_times.end());
        for (std::size_t index = 1U; index < tempo_times.size(); ++index) {
            if (std::abs(tempo_times[index] - tempo_times[index - 1U])
                < editor_time_epsilon) {
                return validation_failure(
                    read_result.path,
                    "editor project contains duplicate BPM-change times"
                );
            }
        }
        if (greatest_id == std::numeric_limits<EditorEntityId>::max()) {
            return validation_failure(
                read_result.path,
                "editor project entity id space is exhausted"
            );
        }

        impl_->metadata = std::move(metadata);
        impl_->key_count = key_count;
        impl_->scroll_speed = scroll_speed;
        impl_->scripts = scripts;
        impl_->note_payloads = payloads;
        impl_->tempos = std::move(tempos);
        impl_->notes = std::move(notes);
        impl_->events = std::move(events);
        impl_->next_entity_id = greatest_id + 1U;
        impl_->undo_stack.clear();
        impl_->redo_stack.clear();
        impl_->current_state_id = impl_->next_state_id++;
        impl_->saved_state_id = impl_->current_state_id;
        impl_->last_autosaved_state_id = impl_->current_state_id;
        impl_->revision = 0U;
        impl_->has_autosave_time = false;
        return EditorIoResult{EditorIoStatus::ok, read_result.path, {}};
    } catch (const std::exception& exception) {
        return validation_failure(
            read_result.path,
            "invalid editor project JSON: " + std::string(exception.what())
        );
    }
}

EditorIoResult ChartEditor::save_psych_json(
    const EditorStorage& storage,
    const std::filesystem::path& relative_path
) const {
    return storage.write_atomic(
        relative_path,
        [this](std::ostream& output, std::string& error) {
            return export_psych_json(output, error);
        }
    );
}

bool ChartEditor::export_psych_json(
    std::ostream& output,
    std::string& error
) const {
    if (impl_->tempos.empty()) {
        error = "Psych export requires at least one BPM change";
        return false;
    }
    for (const auto& [id, tempo] : impl_->tempos) {
        (void)id;
        if (tempo.time_ms < 0.0) {
            error = "Psych section JSON cannot losslessly encode negative BPM changes";
            return false;
        }
        if (tempo.numerator != 4U || tempo.denominator != 4U) {
            error = "Psych section JSON cannot losslessly encode non-4/4 signatures";
            return false;
        }
    }

    const auto sorted_tempos = sorted_entity_pointers(impl_->tempos);
    const auto sorted_notes = sorted_entity_pointers(impl_->notes);
    const auto sorted_events = sorted_entity_pointers(impl_->events);

    std::vector<TempoChange> tempos;
    tempos.reserve(sorted_tempos.size() + 1U);
    for (const auto* entry : sorted_tempos) {
        tempos.push_back(entry->second);
    }
    if (tempos.front().time_ms > editor_time_epsilon) {
        auto initial = tempos.front();
        initial.time_ms = 0.0;
        tempos.insert(tempos.begin(), initial);
    }

    double content_end = 0.0;
    for (const auto* entry : sorted_notes) {
        content_end = std::max(content_end, entry->second.end_time_ms());
    }
    for (const auto* entry : sorted_events) {
        content_end = std::max(content_end, entry->second.time_ms);
    }
    content_end = std::max(content_end, tempos.back().time_ms);

    output << std::setprecision(17);
    output << "{\"song\":{";
    output << "\"song\":" << quoted(impl_->metadata.title);
    output << ",\"artist\":" << quoted(impl_->metadata.artist);
    output << ",\"charter\":" << quoted(impl_->metadata.charter);
    output << ",\"bpm\":" << tempos.front().bpm;
    output << ",\"speed\":" << impl_->scroll_speed;
    output << ",\"needsVoices\":"
           << (impl_->metadata.audio.vocals.empty() ? "false" : "true");
    output << ",\"player1\":"
           << quoted(impl_->metadata.player_character);
    output << ",\"player2\":"
           << quoted(impl_->metadata.opponent_character);
    output << ",\"gfVersion\":"
           << quoted(impl_->metadata.girlfriend_character);
    output << ",\"stage\":" << quoted(impl_->metadata.stage_id);
    output << ",\"arrowSkin\":" << quoted(impl_->metadata.note_style);
    output << ",\"keyCount\":" << impl_->key_count;
    output << ",\"validScore\":true";
    // Unknown members are ignored by Psych but retain PulseForge-only editor
    // information if the file is later imported by this engine.
    output << ",\"pulseforgeScripts\":";
    write_json_string_array(output, impl_->scripts);
    output << ",\"pulseforgeNotePayloads\":";
    write_json_string_array(output, impl_->note_payloads);
    output << ",\"notes\":[";

    std::size_t note_index = 0U;
    std::size_t tempo_index = 1U;
    double section_start = 0.0;
    double current_bpm = tempos.front().bpm;
    bool first_section = true;
    bool first_output_section = true;
    constexpr std::size_t maximum_sections = 1'000'000U;
    std::size_t section_count = 0U;

    while (first_section || note_index < sorted_notes.size()
           || tempo_index < tempos.size()
           || section_start < content_end - editor_time_epsilon) {
        first_section = false;
        if (++section_count > maximum_sections) {
            error = "Psych export would require more than one million sections";
            return false;
        }

        bool change_bpm = false;
        while (tempo_index < tempos.size()
               && std::abs(tempos[tempo_index].time_ms - section_start)
                   < editor_time_epsilon) {
            current_bpm = tempos[tempo_index].bpm;
            change_bpm = section_start > editor_time_epsilon;
            ++tempo_index;
        }

        const double nominal_end = section_start + 240'000.0 / current_bpm;
        double section_end = nominal_end;
        if (tempo_index < tempos.size()
            && tempos[tempo_index].time_ms > section_start + editor_time_epsilon
            && tempos[tempo_index].time_ms
                < nominal_end - editor_time_epsilon) {
            section_end = tempos[tempo_index].time_ms;
        }
        if (!finite(section_end)
            || section_end <= section_start + editor_time_epsilon) {
            error = "Psych section generation made no forward progress";
            return false;
        }
        const double length_steps =
            (section_end - section_start) * current_bpm / 15'000.0;

        if (!first_output_section) {
            output << ',';
        }
        first_output_section = false;
        output << "{\"sectionNotes\":[";
        bool first_note = true;
        while (note_index < sorted_notes.size()) {
            const auto& note = sorted_notes[note_index]->second;
            const bool belongs = note.time_ms < section_end - editor_time_epsilon
                || (section_start == 0.0 && note.time_ms < 0.0);
            if (!belongs) {
                break;
            }
            if (!first_note) {
                output << ',';
            }
            first_note = false;
            const std::uint32_t raw_lane = note.owner == NoteOwner::player
                ? static_cast<std::uint32_t>(note.lane)
                : static_cast<std::uint32_t>(impl_->key_count)
                    + static_cast<std::uint32_t>(note.lane);
            output << '[' << note.time_ms << ',' << raw_lane << ','
                   << note.duration_ms << ',';
            if (note.kind.empty() || note.kind == "normal") {
                output << "null";
            } else {
                output << quoted(note.kind);
            }
            if (note.payload_id != 0U) {
                output << ',' << note.payload_id;
            }
            output << ']';
            ++note_index;
        }
        output << "],\"lengthInSteps\":" << length_steps
               << ",\"mustHitSection\":true"
               << ",\"gfSection\":false"
               << ",\"altAnim\":false"
               << ",\"changeBPM\":" << (change_bpm ? "true" : "false")
               << ",\"bpm\":" << current_bpm << '}';
        section_start = section_end;
    }
    output << ']';

    output << ",\"events\":[";
    std::size_t event_index = 0U;
    bool first_group = true;
    while (event_index < sorted_events.size()) {
        const double group_time = sorted_events[event_index]->second.time_ms;
        if (!first_group) {
            output << ',';
        }
        first_group = false;
        output << '[' << group_time << ",[";
        bool first_event = true;
        while (event_index < sorted_events.size()
               && std::abs(
                      sorted_events[event_index]->second.time_ms - group_time
                  ) < editor_time_epsilon) {
            if (!first_event) {
                output << ',';
            }
            first_event = false;
            const auto& event = sorted_events[event_index]->second;
            output << '[' << quoted(event.name) << ',' << quoted(event.value1)
                   << ',' << quoted(event.value2) << ']';
            ++event_index;
        }
        output << "]]";
    }
    output << "]}}";
    if (!output) {
        error = "failed while streaming Psych chart JSON";
        return false;
    }
    return true;
}

void ChartEditor::configure_autosave(ChartEditorAutosaveSettings settings) {
    if (settings.interval <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("autosave interval must be greater than zero");
    }
    if (settings.relative_path.empty()) {
        throw std::invalid_argument("autosave path cannot be empty");
    }
    impl_->autosave = std::move(settings);
    impl_->has_autosave_time = false;
}

const ChartEditorAutosaveSettings& ChartEditor::autosave_settings()
    const noexcept {
    return impl_->autosave;
}

EditorAutosaveStatus ChartEditor::autosave_if_due(
    const EditorStorage& storage,
    const std::chrono::steady_clock::time_point now,
    std::string* error
) {
    if (!impl_->autosave.enabled) {
        return EditorAutosaveStatus::disabled;
    }
    if (impl_->current_state_id == impl_->last_autosaved_state_id) {
        return EditorAutosaveStatus::not_dirty;
    }
    if (impl_->has_autosave_time
        && now >= impl_->last_autosave_time
        && now - impl_->last_autosave_time < impl_->autosave.interval) {
        return EditorAutosaveStatus::not_due;
    }

    const auto status = force_autosave(storage, error);
    if (status == EditorAutosaveStatus::saved) {
        impl_->last_autosave_time = now;
        impl_->has_autosave_time = true;
    }
    return status;
}

EditorAutosaveStatus ChartEditor::force_autosave(
    const EditorStorage& storage,
    std::string* error
) {
    if (!impl_->autosave.enabled) {
        return EditorAutosaveStatus::disabled;
    }
    if (impl_->current_state_id == impl_->last_autosaved_state_id) {
        return EditorAutosaveStatus::not_dirty;
    }
    const auto saved_state = impl_->saved_state_id;
    const auto result = save_project(storage, impl_->autosave.relative_path);
    impl_->saved_state_id = saved_state;
    if (!result) {
        assign_error(error, result.message);
        return EditorAutosaveStatus::failed;
    }
    impl_->last_autosaved_state_id = impl_->current_state_id;
    return EditorAutosaveStatus::saved;
}

}  // namespace pulseforge
