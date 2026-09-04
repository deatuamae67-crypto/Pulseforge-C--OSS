#include "pulseforge/editor_models.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace pulseforge {
namespace {

using Json = nlohmann::json;

constexpr std::size_t maximum_descriptor_string_bytes = 16U * 1024U;
constexpr std::size_t maximum_descriptor_extensions_bytes = 256U * 1024U;

[[nodiscard]] bool finite_and_bounded(const double value) noexcept {
    return std::isfinite(value) && std::abs(value) <= 10'000'000.0;
}

[[nodiscard]] bool safe_id(const std::string_view value) {
    if (value.empty() || value.size() > 256U) {
        return false;
    }
    for (const auto raw : value) {
        const auto character = static_cast<unsigned char>(raw);
        if (character < 0x20U || raw == '/' || raw == '\\' || raw == ':'
            || raw == '*'
            || raw == '?' || raw == '"' || raw == '<' || raw == '>'
            || raw == '|') {
            return false;
        }
    }
    return value != "." && value != "..";
}

[[nodiscard]] bool safe_logical_asset(const std::string_view value) {
    if (value.empty() || value.size() > maximum_descriptor_string_bytes) {
        return false;
    }
    std::size_t start = 0U;
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
        if (index == start) {
            return false;
        }
        const auto component = value.substr(start, index - start);
        if (component == "." || component == "..") {
            return false;
        }
        start = index + 1U;
    }
    return true;
}

[[nodiscard]] bool extension_is_object(
    const std::string& text,
    const bool allow_array,
    std::string& error
) {
    if (text.empty()) {
        return true;
    }
    if (text.size() > maximum_descriptor_extensions_bytes) {
        error = "preserved extension JSON exceeds the 256 KiB limit";
        return false;
    }
    try {
        const auto value = Json::parse(text);
        if (!value.is_object() && !(allow_array && value.is_array())) {
            error = allow_array
                ? "preserved extension JSON must be an object or array"
                : "preserved extension JSON must be an object";
            return false;
        }
    } catch (const std::exception& exception) {
        error = "preserved extension JSON is invalid: "
            + std::string(exception.what());
        return false;
    }
    return true;
}

[[nodiscard]] Json extension_object(const std::string& text) {
    if (text.empty()) {
        return Json::object();
    }
    const auto value = Json::parse(text);
    return value.is_object() ? value : Json::object();
}

[[nodiscard]] Json serialize_animation(const AnimationDescriptor& animation) {
    auto value = extension_object(animation.extensions_json);
    value["anim"] = animation.id;
    value["name"] = animation.name;
    value["fps"] = animation.fps;
    value["loop"] = animation.loop;
    value["indices"] = animation.indices;
    value["offsets"] = Json::array({animation.offsets.x, animation.offsets.y});
    return value;
}

[[nodiscard]] Json serialize_character(const CharacterDescriptor& descriptor) {
    auto root = extension_object(descriptor.extensions_json);
    root["image"] = descriptor.image;
    root["scale"] = descriptor.scale;
    root["sing_duration"] = descriptor.sing_duration;
    root["healthicon"] = descriptor.health_icon;
    root["position"] = Json::array({
        descriptor.position.x,
        descriptor.position.y,
    });
    root["camera_position"] = Json::array({
        descriptor.camera_position.x,
        descriptor.camera_position.y,
    });
    root["flip_x"] = descriptor.flip_x;
    root["no_antialiasing"] = descriptor.no_antialiasing;
    root["healthbar_colors"] = Json::array({
        descriptor.healthbar_color.red,
        descriptor.healthbar_color.green,
        descriptor.healthbar_color.blue,
    });
    root["vocals_file"] = descriptor.vocals_file;
    if (descriptor.editor_is_player.has_value()) {
        root["_editor_isPlayer"] = *descriptor.editor_is_player;
    } else {
        root.erase("_editor_isPlayer");
    }
    root["animations"] = Json::array();
    for (const auto& animation : descriptor.animations) {
        root["animations"].push_back(serialize_animation(animation));
    }
    return root;
}

[[nodiscard]] Json serialize_week(const WeekDescriptor& descriptor) {
    auto root = extension_object(descriptor.extensions_json);
    root["songs"] = Json::array();
    for (const auto& song : descriptor.songs) {
        Json value = Json::array({
            song.name,
            song.character,
            Json::array({song.color.red, song.color.green, song.color.blue}),
        });
        if (!song.extensions_json.empty()) {
            const auto extras = Json::parse(song.extensions_json);
            if (extras.is_array()) {
                for (const auto& extra : extras) {
                    value.push_back(extra);
                }
            } else {
                value.push_back(extras);
            }
        }
        root["songs"].push_back(std::move(value));
    }
    root["weekCharacters"] = Json::array({
        descriptor.characters[0],
        descriptor.characters[1],
        descriptor.characters[2],
    });
    root["weekBackground"] = descriptor.background;
    root["weekBefore"] = descriptor.previous_week;
    root["storyName"] = descriptor.story_name;
    root["weekName"] = descriptor.display_name;
    root["startUnlocked"] = descriptor.start_unlocked;
    root["hiddenUntilUnlocked"] = descriptor.hidden_until_unlocked;
    root["hideStoryMode"] = descriptor.hide_story;
    root["hideFreeplay"] = descriptor.hide_freeplay;
    std::string difficulties;
    for (std::size_t index = 0U; index < descriptor.difficulties.size(); ++index) {
        if (index != 0U) {
            difficulties += ", ";
        }
        difficulties += descriptor.difficulties[index];
    }
    root["difficulties"] = std::move(difficulties);
    return root;
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

[[nodiscard]] std::string descriptor_parse_error(
    const std::vector<DescriptorDiagnostic>& diagnostics
) {
    if (diagnostics.empty()) {
        return "descriptor parser rejected the JSON";
    }
    return diagnostics.front().path + ": " + diagnostics.front().message;
}

template <typename Document>
class SnapshotHistory final {
public:
    explicit SnapshotHistory(Document initial, const std::size_t maximum)
        : current_(std::move(initial)), maximum_(std::max<std::size_t>(maximum, 1U)) {}

    [[nodiscard]] const Document& current() const noexcept {
        return current_;
    }

    void replace(Document after, std::string label) {
        redo_.clear();
        const auto after_state = next_state_++;
        undo_.push_back(Entry{
            current_,
            std::move(after),
            std::move(label),
            current_state_,
            after_state,
        });
        current_ = undo_.back().after;
        current_state_ = after_state;
        if (undo_.size() > maximum_) {
            undo_.erase(undo_.begin());
        }
    }

    [[nodiscard]] bool undo(std::string* label) {
        if (undo_.empty()) {
            return false;
        }
        auto entry = std::move(undo_.back());
        undo_.pop_back();
        current_ = entry.before;
        current_state_ = entry.before_state;
        if (label != nullptr) {
            *label = entry.label;
        }
        redo_.push_back(std::move(entry));
        return true;
    }

    [[nodiscard]] bool redo(std::string* label) {
        if (redo_.empty()) {
            return false;
        }
        auto entry = std::move(redo_.back());
        redo_.pop_back();
        current_ = entry.after;
        current_state_ = entry.after_state;
        if (label != nullptr) {
            *label = entry.label;
        }
        undo_.push_back(std::move(entry));
        return true;
    }

    void load(Document document) {
        current_ = std::move(document);
        undo_.clear();
        redo_.clear();
        current_state_ = next_state_++;
        saved_state_ = current_state_;
    }

    [[nodiscard]] bool dirty() const noexcept {
        return current_state_ != saved_state_;
    }

    void mark_saved() noexcept {
        saved_state_ = current_state_;
    }

private:
    struct Entry {
        Document before;
        Document after;
        std::string label;
        std::uint64_t before_state{};
        std::uint64_t after_state{};
    };

    Document current_;
    std::size_t maximum_{};
    std::vector<Entry> undo_;
    std::vector<Entry> redo_;
    std::uint64_t current_state_{1U};
    std::uint64_t saved_state_{1U};
    std::uint64_t next_state_{2U};
};

}  // namespace

std::vector<std::string> validate_character_editor_document(
    const CharacterDescriptor& descriptor
) {
    std::vector<std::string> errors;
    if (!safe_id(descriptor.id)) {
        errors.emplace_back("character id is empty, unsafe, or too long");
    }
    if (!safe_logical_asset(descriptor.image)) {
        errors.emplace_back("character image is not a safe logical asset path");
    }
    if (!std::isfinite(descriptor.scale) || descriptor.scale <= 0.0
        || descriptor.scale > 100.0) {
        errors.emplace_back("character scale must be finite and between 0 and 100");
    }
    if (!std::isfinite(descriptor.sing_duration)
        || descriptor.sing_duration < 0.0
        || descriptor.sing_duration > 64.0) {
        errors.emplace_back("character sing duration must be between 0 and 64");
    }
    if (!finite_and_bounded(descriptor.position.x)
        || !finite_and_bounded(descriptor.position.y)
        || !finite_and_bounded(descriptor.camera_position.x)
        || !finite_and_bounded(descriptor.camera_position.y)) {
        errors.emplace_back("character positions must be finite and bounded");
    }
    const auto& color = descriptor.healthbar_color;
    if (color.red < 0 || color.red > 255 || color.green < 0
        || color.green > 255 || color.blue < 0 || color.blue > 255) {
        errors.emplace_back("character healthbar color must use RGB 0..255");
    }
    if (descriptor.health_icon.size() > maximum_descriptor_string_bytes
        || descriptor.vocals_file.size() > maximum_descriptor_string_bytes) {
        errors.emplace_back("character icon or vocals field is too long");
    }
    if (!descriptor.vocals_file.empty()
        && !safe_logical_asset(descriptor.vocals_file)) {
        errors.emplace_back("character vocals file is not a safe logical path");
    }
    if (descriptor.animations.size() > 4'096U) {
        errors.emplace_back("character has more than 4096 animations");
    }
    std::unordered_set<std::string> animation_ids;
    animation_ids.reserve(descriptor.animations.size());
    for (const auto& animation : descriptor.animations) {
        if (animation.id.empty()
            || animation.id.size() > maximum_descriptor_string_bytes
            || animation.name.empty()
            || animation.name.size() > maximum_descriptor_string_bytes) {
            errors.emplace_back("animation id/name is empty or too long");
            break;
        }
        if (!animation_ids.insert(animation.id).second) {
            errors.emplace_back("animation ids must be unique");
            break;
        }
        if (animation.fps <= 0 || animation.fps > 1'000) {
            errors.emplace_back("animation FPS must be between 1 and 1000");
            break;
        }
        if (!finite_and_bounded(animation.offsets.x)
            || !finite_and_bounded(animation.offsets.y)
            || animation.indices.size() > 1'000'000U) {
            errors.emplace_back("animation offsets or frame indices exceed limits");
            break;
        }
        std::string extension_error;
        if (!extension_is_object(
                animation.extensions_json,
                false,
                extension_error
            )) {
            errors.push_back(std::move(extension_error));
            break;
        }
    }
    std::string extension_error;
    if (!extension_is_object(
            descriptor.extensions_json,
            false,
            extension_error
        )) {
        errors.push_back(std::move(extension_error));
    }
    return errors;
}

std::vector<std::string> validate_week_editor_document(
    const WeekDescriptor& descriptor
) {
    std::vector<std::string> errors;
    if (!safe_id(descriptor.id)) {
        errors.emplace_back("week id is empty, unsafe, or too long");
    }
    if (descriptor.songs.size() > 4'096U) {
        errors.emplace_back("week contains more than 4096 songs");
    }
    for (const auto& song : descriptor.songs) {
        if (song.name.empty() || song.character.empty()
            || song.name.size() > maximum_descriptor_string_bytes
            || song.character.size() > maximum_descriptor_string_bytes) {
            errors.emplace_back("week song name/character is empty or too long");
            break;
        }
        if (song.color.red < 0 || song.color.red > 255
            || song.color.green < 0 || song.color.green > 255
            || song.color.blue < 0 || song.color.blue > 255) {
            errors.emplace_back("week song color must use RGB 0..255");
            break;
        }
        std::string extension_error;
        if (!extension_is_object(song.extensions_json, true, extension_error)) {
            errors.push_back(std::move(extension_error));
            break;
        }
    }
    for (const auto& character : descriptor.characters) {
        if (character.empty()
            || character.size() > maximum_descriptor_string_bytes) {
            errors.emplace_back("week menu character is empty or too long");
            break;
        }
    }
    const std::string* strings[] = {
        &descriptor.background,
        &descriptor.previous_week,
        &descriptor.story_name,
        &descriptor.display_name,
    };
    for (const auto* value : strings) {
        if (value->size() > maximum_descriptor_string_bytes) {
            errors.emplace_back("week metadata string is too long");
            break;
        }
    }
    if (descriptor.difficulties.size() > 128U) {
        errors.emplace_back("week contains more than 128 difficulties");
    }
    std::unordered_set<std::string> difficulty_names;
    for (const auto& difficulty : descriptor.difficulties) {
        if (difficulty.empty() || difficulty.size() > 256U
            || !difficulty_names.insert(difficulty).second) {
            errors.emplace_back("week difficulties must be non-empty and unique");
            break;
        }
    }
    std::string extension_error;
    if (!extension_is_object(
            descriptor.extensions_json,
            false,
            extension_error
        )) {
        errors.push_back(std::move(extension_error));
    }
    return errors;
}

struct CharacterEditor::Impl {
    Impl(CharacterDescriptor descriptor, const std::size_t maximum)
        : history(std::move(descriptor), maximum) {}

    SnapshotHistory<CharacterDescriptor> history;
};

CharacterEditor::CharacterEditor(
    CharacterDescriptor descriptor,
    const std::size_t maximum_history_entries
) : impl_(std::make_unique<Impl>(
        std::move(descriptor),
        maximum_history_entries
    )) {}

CharacterEditor::~CharacterEditor() = default;
CharacterEditor::CharacterEditor(CharacterEditor&&) noexcept = default;
CharacterEditor& CharacterEditor::operator=(CharacterEditor&&) noexcept = default;

const CharacterDescriptor& CharacterEditor::document() const noexcept {
    return impl_->history.current();
}

bool CharacterEditor::replace(
    CharacterDescriptor descriptor,
    std::string label,
    std::string* error
) {
    const auto errors = validate_character_editor_document(descriptor);
    if (!errors.empty()) {
        if (error != nullptr) {
            *error = errors.front();
        }
        return false;
    }
    if (label.empty()) {
        label = "Edit character";
    }
    impl_->history.replace(std::move(descriptor), std::move(label));
    return true;
}

bool CharacterEditor::undo(std::string* label) {
    return impl_->history.undo(label);
}

bool CharacterEditor::redo(std::string* label) {
    return impl_->history.redo(label);
}

bool CharacterEditor::dirty() const noexcept {
    return impl_->history.dirty();
}

void CharacterEditor::mark_saved() noexcept {
    impl_->history.mark_saved();
}

EditorIoResult CharacterEditor::load_psych_json(
    const EditorStorage& storage,
    const std::filesystem::path& relative_path,
    const std::string_view id
) {
    std::string text;
    const auto read = storage.read_text(relative_path, text);
    if (!read) {
        return read;
    }
    const auto parsed = ContentDescriptorParser::parse_psych_character(
        text,
        id
    );
    if (!parsed) {
        return validation_failure(
            read.path,
            descriptor_parse_error(parsed.diagnostics)
        );
    }
    const auto errors = validate_character_editor_document(*parsed.value);
    if (!errors.empty()) {
        return validation_failure(read.path, errors.front());
    }
    impl_->history.load(*parsed.value);
    return EditorIoResult{EditorIoStatus::ok, read.path, {}};
}

EditorIoResult CharacterEditor::save_psych_json(
    const EditorStorage& storage,
    const std::filesystem::path& relative_path
) {
    const auto errors = validate_character_editor_document(document());
    if (!errors.empty()) {
        return validation_failure(relative_path, errors.front());
    }
    try {
        const auto text = serialize_character(document()).dump(2);
        const auto result = storage.write_atomic(relative_path, text);
        if (result) {
            mark_saved();
        }
        return result;
    } catch (const std::exception& exception) {
        return validation_failure(
            relative_path,
            "cannot serialize character: " + std::string(exception.what())
        );
    }
}

struct WeekEditor::Impl {
    Impl(WeekDescriptor descriptor, const std::size_t maximum)
        : history(std::move(descriptor), maximum) {}

    SnapshotHistory<WeekDescriptor> history;
};

WeekEditor::WeekEditor(
    WeekDescriptor descriptor,
    const std::size_t maximum_history_entries
) : impl_(std::make_unique<Impl>(
        std::move(descriptor),
        maximum_history_entries
    )) {}

WeekEditor::~WeekEditor() = default;
WeekEditor::WeekEditor(WeekEditor&&) noexcept = default;
WeekEditor& WeekEditor::operator=(WeekEditor&&) noexcept = default;

const WeekDescriptor& WeekEditor::document() const noexcept {
    return impl_->history.current();
}

bool WeekEditor::replace(
    WeekDescriptor descriptor,
    std::string label,
    std::string* error
) {
    const auto errors = validate_week_editor_document(descriptor);
    if (!errors.empty()) {
        if (error != nullptr) {
            *error = errors.front();
        }
        return false;
    }
    if (label.empty()) {
        label = "Edit week";
    }
    impl_->history.replace(std::move(descriptor), std::move(label));
    return true;
}

bool WeekEditor::undo(std::string* label) {
    return impl_->history.undo(label);
}

bool WeekEditor::redo(std::string* label) {
    return impl_->history.redo(label);
}

bool WeekEditor::dirty() const noexcept {
    return impl_->history.dirty();
}

void WeekEditor::mark_saved() noexcept {
    impl_->history.mark_saved();
}

EditorIoResult WeekEditor::load_psych_json(
    const EditorStorage& storage,
    const std::filesystem::path& relative_path,
    const std::string_view id
) {
    std::string text;
    const auto read = storage.read_text(relative_path, text);
    if (!read) {
        return read;
    }
    const auto parsed = ContentDescriptorParser::parse_psych_week(text, id);
    if (!parsed) {
        return validation_failure(
            read.path,
            descriptor_parse_error(parsed.diagnostics)
        );
    }
    const auto errors = validate_week_editor_document(*parsed.value);
    if (!errors.empty()) {
        return validation_failure(read.path, errors.front());
    }
    impl_->history.load(*parsed.value);
    return EditorIoResult{EditorIoStatus::ok, read.path, {}};
}

EditorIoResult WeekEditor::save_psych_json(
    const EditorStorage& storage,
    const std::filesystem::path& relative_path
) {
    const auto errors = validate_week_editor_document(document());
    if (!errors.empty()) {
        return validation_failure(relative_path, errors.front());
    }
    try {
        const auto text = serialize_week(document()).dump(2);
        const auto result = storage.write_atomic(relative_path, text);
        if (result) {
            mark_saved();
        }
        return result;
    } catch (const std::exception& exception) {
        return validation_failure(
            relative_path,
            "cannot serialize week: " + std::string(exception.what())
        );
    }
}

}  // namespace pulseforge
