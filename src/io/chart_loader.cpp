#include "pulseforge/chart_loader.hpp"
#include "pulseforge/psych_stock_provider.hpp"

#include "fast_psych_loader.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace pulseforge {
namespace {

using Json = nlohmann::json;

constexpr std::uintmax_t maximum_metadata_json_bytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::uintmax_t maximum_events_json_bytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::uintmax_t fast_psych_minimum_bytes = 1ULL * 1024ULL * 1024ULL;
constexpr std::size_t file_read_chunk_bytes = 64U * 1024U;
constexpr std::size_t maximum_audio_directory_entries = 4'096U;
constexpr std::size_t maximum_discovered_vocal_stems = 8U;

[[nodiscard]] char ascii_lower(const char value) noexcept {
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value - 'A' + 'a')
        : value;
}

[[nodiscard]] std::string lower_ascii(const std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), ascii_lower);
    return result;
}

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {
        reinterpret_cast<const char*>(value.data()),
        value.size(),
    };
}

// Audio file naming in FNF forks is mostly ASCII but separators vary between
// engines and exported packs. Canonicalizing them lets `my_song`, `my-song`
// and `My Song` identify the same stem without applying fuzzy matching to an
// unrelated file.
[[nodiscard]] std::string canonical_audio_id(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    bool separator_pending = false;
    for (const char raw : value) {
        const auto byte = static_cast<unsigned char>(raw);
        if ((byte >= 'a' && byte <= 'z')
            || (byte >= '0' && byte <= '9')) {
            if (separator_pending && !result.empty()) {
                result.push_back('-');
            }
            result.push_back(raw);
            separator_pending = false;
        } else if (byte >= 'A' && byte <= 'Z') {
            if (separator_pending && !result.empty()) {
                result.push_back('-');
            }
            result.push_back(ascii_lower(raw));
            separator_pending = false;
        } else if (byte >= 0x80U) {
            if (separator_pending && !result.empty()) {
                result.push_back('-');
            }
            result.push_back(raw);
            separator_pending = false;
        } else {
            separator_pending = !result.empty();
        }
    }
    return result;
}

[[nodiscard]] bool supported_audio_extension(
    const std::filesystem::path& path
) {
    const auto extension = lower_ascii(path_utf8(path.extension()));
    return extension == ".ogg" || extension == ".wav"
        || extension == ".mp3" || extension == ".flac";
}

[[nodiscard]] bool path_equal_ascii_insensitive(
    const std::filesystem::path& left,
    const std::filesystem::path& right
) {
    return lower_ascii(path_utf8(left.lexically_normal()))
        == lower_ascii(path_utf8(right.lexically_normal()));
}

void append_unique_directory(
    std::vector<std::filesystem::path>& directories,
    const std::filesystem::path& directory
) {
    if (directory.empty()) {
        return;
    }
    const auto duplicate = std::find_if(
        directories.begin(),
        directories.end(),
        [&](const auto& existing) {
            return path_equal_ascii_insensitive(existing, directory);
        }
    );
    if (duplicate == directories.end()) {
        directories.push_back(directory.lexically_normal());
    }
}

[[nodiscard]] std::vector<std::filesystem::path> audio_files_in(
    const std::filesystem::path& directory
) {
    std::vector<std::filesystem::path> result;
    std::error_code error;
    std::filesystem::directory_iterator iterator(
        directory,
        std::filesystem::directory_options::skip_permission_denied,
        error
    );
    if (error) {
        return result;
    }
    const std::filesystem::directory_iterator end;
    std::size_t inspected = 0U;
    for (; iterator != end && inspected < maximum_audio_directory_entries;
         iterator.increment(error), ++inspected) {
        if (error) {
            break;
        }
        std::error_code type_error;
        if (!iterator->is_regular_file(type_error) || type_error
            || !supported_audio_extension(iterator->path())) {
            continue;
        }
        result.push_back(iterator->path());
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        const auto left_text = path_utf8(left.filename());
        const auto right_text = path_utf8(right.filename());
        const auto left_key = lower_ascii(left_text);
        const auto right_key = lower_ascii(right_text);
        return left_key != right_key ? left_key < right_key
                                     : left_text < right_text;
    });
    return result;
}

struct LimitedReadResult {
    std::optional<std::string> text;
    std::string error;
};

[[nodiscard]] LimitedReadResult read_limited_file(
    const std::filesystem::path& path,
    const std::uintmax_t maximum_bytes,
    const std::string_view open_error_prefix,
    const std::string_view read_error_prefix,
    const std::string_view too_large_error
) {
    std::error_code size_error;
    const auto reported_size = std::filesystem::file_size(path, size_error);
    if (!size_error && reported_size > maximum_bytes) {
        return {
            std::nullopt,
            std::string(too_large_error) + ": " + path.string(),
        };
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {
            std::nullopt,
            std::string(open_error_prefix) + path.string(),
        };
    }

    std::string text;
    if (!size_error) {
        text.reserve(static_cast<std::size_t>(reported_size));
    }
    std::array<char, file_read_chunk_bytes> chunk{};
    while (true) {
        // Read at most one byte beyond the limit. This second check is
        // intentional: file_size() is advisory and a file can grow between the
        // metadata query and the actual read.
        const auto remaining =
            maximum_bytes + 1U - static_cast<std::uintmax_t>(text.size());
        const auto request = static_cast<std::streamsize>(
            std::min<std::uintmax_t>(remaining, chunk.size())
        );
        input.read(chunk.data(), request);
        const auto count = input.gcount();
        if (count > 0) {
            text.append(chunk.data(), static_cast<std::size_t>(count));
            if (text.size() > maximum_bytes) {
                return {
                    std::nullopt,
                    std::string(too_large_error) + ": " + path.string(),
                };
            }
        }

        if (input.bad()) {
            return {
                std::nullopt,
                std::string(read_error_prefix) + path.string(),
            };
        }
        if (input.eof()) {
            break;
        }
        if (input.fail() || count == 0) {
            return {
                std::nullopt,
                std::string(read_error_prefix) + path.string(),
            };
        }
    }
    return {std::move(text), {}};
}

[[nodiscard]] bool is_json_integer(const Json& value) noexcept {
    return value.is_number_integer() || value.is_number_unsigned();
}

[[noreturn]] void reject_strict_import(
    const std::string_view path,
    const std::string_view expectation
) {
    throw std::runtime_error(
        "strict import rejected " + std::string(path) + ": "
        + std::string(expectation)
    );
}

template <typename Predicate>
void require_strict_field_type(
    const Json& object,
    const std::string_view key,
    const std::string_view object_path,
    const std::string_view expectation,
    Predicate&& predicate
) {
    const auto iterator = object.find(key);
    if (iterator == object.end()) {
        return;
    }
    if (!std::forward<Predicate>(predicate)(*iterator)) {
        reject_strict_import(
            std::string(object_path) + "." + std::string(key),
            expectation
        );
    }
}

void require_strict_string_field(
    const Json& object,
    const std::string_view key,
    const std::string_view object_path
) {
    require_strict_field_type(
        object,
        key,
        object_path,
        "expected a string; permissive scalar coercion is disabled",
        [](const Json& value) { return value.is_string(); }
    );
}

void require_strict_number_field(
    const Json& object,
    const std::string_view key,
    const std::string_view object_path
) {
    require_strict_field_type(
        object,
        key,
        object_path,
        "expected a number; permissive defaulting is disabled",
        [](const Json& value) { return value.is_number(); }
    );
}

void require_strict_integer_field(
    const Json& object,
    const std::string_view key,
    const std::string_view object_path
) {
    require_strict_field_type(
        object,
        key,
        object_path,
        "expected an integer; permissive defaulting is disabled",
        [](const Json& value) { return is_json_integer(value); }
    );
}

void require_strict_boolean_field(
    const Json& object,
    const std::string_view key,
    const std::string_view object_path
) {
    require_strict_field_type(
        object,
        key,
        object_path,
        "expected a boolean; permissive defaulting is disabled",
        [](const Json& value) { return value.is_boolean(); }
    );
}

template <std::size_t Size>
void require_strict_one_of(
    const Json& object,
    const std::array<std::string_view, Size>& keys,
    const std::string_view object_path,
    const std::string_view expectation
) {
    const bool present = std::any_of(
        keys.begin(),
        keys.end(),
        [&object](const std::string_view key) {
            return object.find(key) != object.end();
        }
    );
    if (!present) {
        reject_strict_import(object_path, expectation);
    }
}

[[nodiscard]] double number_or(
    const Json& object,
    const std::string_view key,
    const double fallback
) {
    const auto iterator = object.find(key);
    if (iterator == object.end() || !iterator->is_number()) {
        return fallback;
    }
    return iterator->get<double>();
}

[[nodiscard]] std::int64_t integer_or(
    const Json& object,
    const std::string_view key,
    const std::int64_t fallback
) {
    const auto iterator = object.find(key);
    if (iterator == object.end()) {
        return fallback;
    }
    if (iterator->is_number_unsigned()) {
        const auto value = iterator->get<std::uint64_t>();
        if (value > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())) {
            return std::numeric_limits<std::int64_t>::max();
        }
        return static_cast<std::int64_t>(value);
    }
    if (iterator->is_number_integer()) {
        return iterator->get<std::int64_t>();
    }
    return fallback;
}

[[nodiscard]] std::int64_t integer_value_or(
    const Json& value,
    const std::int64_t fallback
) {
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max())) {
            return std::numeric_limits<std::int64_t>::max();
        }
        return static_cast<std::int64_t>(number);
    }
    if (value.is_number_integer()) {
        return value.get<std::int64_t>();
    }
    return fallback;
}

[[nodiscard]] std::uint16_t lane_or_invalid(
    const Json& object,
    const std::string_view key,
    const std::int64_t fallback = 0
) {
    const auto lane = integer_or(object, key, fallback);
    if (lane < 0
        || lane > static_cast<std::int64_t>(
                      std::numeric_limits<std::uint16_t>::max())) {
        return std::numeric_limits<std::uint16_t>::max();
    }
    return static_cast<std::uint16_t>(lane);
}

[[nodiscard]] std::uint16_t unsigned16_or_invalid(
    const Json& object,
    const std::string_view key,
    const std::int64_t fallback
) {
    const auto value = integer_or(object, key, fallback);
    if (value < 0
        || value > static_cast<std::int64_t>(
                       std::numeric_limits<std::uint16_t>::max())) {
        return 0;
    }
    return static_cast<std::uint16_t>(value);
}

[[nodiscard]] bool boolean_or(
    const Json& object,
    const std::string_view key,
    const bool fallback
) {
    const auto iterator = object.find(key);
    if (iterator == object.end() || !iterator->is_boolean()) {
        return fallback;
    }
    return iterator->get<bool>();
}

[[nodiscard]] std::string string_or(
    const Json& object,
    const std::string_view key,
    std::string fallback = {}
) {
    const auto iterator = object.find(key);
    if (iterator == object.end()) {
        return fallback;
    }
    if (iterator->is_string()) {
        return iterator->get<std::string>();
    }
    if (iterator->is_number() || iterator->is_boolean()) {
        return iterator->dump();
    }
    return fallback;
}

[[nodiscard]] std::string scalar_string(const Json& value) {
    if (value.is_null()) {
        return {};
    }
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return value.dump();
}

[[nodiscard]] std::string bounded_json_payload(
    const Json& value,
    const std::size_t maximum_bytes,
    const std::string_view label
) {
    auto payload = value.dump();
    if (payload.size() > maximum_bytes) {
        throw std::runtime_error(
            std::string(label) + " exceeds the JSON payload safety limit"
        );
    }
    return payload;
}

[[nodiscard]] bool is_denpa_schema(const Json& root) {
    const auto wrapper = root.find("song");
    if (wrapper == root.end() || !wrapper->is_object()) {
        return false;
    }
    return wrapper->contains("header") && wrapper->contains("options");
}

void validate_strict_audio(
    const Json& audio,
    const std::string_view path
) {
    require_strict_string_field(audio, "instrumental", path);
    const auto vocals = audio.find("vocals");
    if (vocals == audio.end()) {
        return;
    }
    if (vocals->is_string()) {
        return;
    }
    if (!vocals->is_array()) {
        reject_strict_import(
            std::string(path) + ".vocals",
            "expected a string or an array of strings"
        );
    }
    for (std::size_t index = 0; index < vocals->size(); ++index) {
        if (!(*vocals)[index].is_string()) {
            reject_strict_import(
                std::string(path) + ".vocals["
                    + std::to_string(index) + "]",
                "expected a string; permissive item skipping is disabled"
            );
        }
    }
}

void validate_strict_native(const Json& root) {
    require_strict_string_field(root, "format", "$");

    const Json* song = &root;
    if (const auto wrapper = root.find("song"); wrapper != root.end()) {
        if (!wrapper->is_object()) {
            reject_strict_import(
                "$.song",
                "expected an object; permissive wrapper fallback is disabled"
            );
        }
        song = &*wrapper;
    }

    for (const auto* field : {"title", "name", "artist", "charter", "difficulty"}) {
        require_strict_string_field(*song, field, "$.song");
    }
    require_strict_integer_field(*song, "keyCount", "$.song");
    require_strict_number_field(*song, "scrollSpeed", "$.song");
    if (song != &root) {
        require_strict_number_field(root, "scrollSpeed", "$");
    }

    if (const auto audio = root.find("audio"); audio != root.end()) {
        if (!audio->is_object()) {
            reject_strict_import(
                "$.audio",
                "expected an object; permissive field skipping is disabled"
            );
        }
        validate_strict_audio(*audio, "$.audio");
    }

    if (const auto tempos = root.find("tempos"); tempos != root.end()) {
        if (!tempos->is_array()) {
            reject_strict_import(
                "$.tempos",
                "expected an array; permissive field skipping is disabled"
            );
        }
        for (std::size_t index = 0; index < tempos->size(); ++index) {
            const auto& item = (*tempos)[index];
            const auto path = "$.tempos[" + std::to_string(index) + "]";
            if (!item.is_object()) {
                reject_strict_import(
                    path,
                    "expected an object; permissive item skipping is disabled"
                );
            }
            require_strict_number_field(item, "time", path);
            require_strict_number_field(item, "timeMs", path);
            require_strict_number_field(item, "bpm", path);
            require_strict_integer_field(item, "numerator", path);
            require_strict_integer_field(item, "denominator", path);
            require_strict_one_of(
                item,
                std::array<std::string_view, 2>{"time", "timeMs"},
                path,
                "required time/timeMs field is missing"
            );
            if (!item.contains("bpm")) {
                reject_strict_import(path, "required bpm field is missing");
            }
        }
    } else {
        reject_strict_import("$", "required tempos array is missing");
    }

    if (const auto notes = root.find("notes"); notes != root.end()) {
        if (!notes->is_array()) {
            reject_strict_import(
                "$.notes",
                "expected an array; permissive field skipping is disabled"
            );
        }
        for (std::size_t index = 0; index < notes->size(); ++index) {
            const auto& item = (*notes)[index];
            const auto path = "$.notes[" + std::to_string(index) + "]";
            if (!item.is_object()) {
                reject_strict_import(
                    path,
                    "expected an object; permissive item skipping is disabled"
                );
            }
            require_strict_number_field(item, "time", path);
            require_strict_number_field(item, "timeMs", path);
            require_strict_number_field(item, "duration", path);
            require_strict_number_field(item, "durationMs", path);
            require_strict_integer_field(item, "lane", path);
            require_strict_string_field(item, "kind", path);
            require_strict_string_field(item, "type", path);
            require_strict_one_of(
                item,
                std::array<std::string_view, 2>{"time", "timeMs"},
                path,
                "required time/timeMs field is missing"
            );
            if (!item.contains("lane")) {
                reject_strict_import(path, "required lane field is missing");
            }

            if (const auto owner = item.find("owner"); owner != item.end()) {
                if (!owner->is_string()) {
                    reject_strict_import(
                        path + ".owner",
                        "expected a supported owner string"
                    );
                }
                const auto value = owner->get<std::string_view>();
                if (value != "player"
                    && value != "opponent"
                    && value != "enemy") {
                    reject_strict_import(
                        path + ".owner",
                        "expected player, opponent, or enemy; permissive "
                        "fallback to player is disabled"
                    );
                }
            }
        }
    }

    if (const auto events = root.find("events"); events != root.end()) {
        if (!events->is_array()) {
            reject_strict_import(
                "$.events",
                "expected an array; permissive field skipping is disabled"
            );
        }
        for (std::size_t index = 0; index < events->size(); ++index) {
            const auto& item = (*events)[index];
            const auto path = "$.events[" + std::to_string(index) + "]";
            if (!item.is_object()) {
                reject_strict_import(
                    path,
                    "expected an object; permissive item skipping is disabled"
                );
            }
            require_strict_number_field(item, "time", path);
            require_strict_number_field(item, "timeMs", path);
            require_strict_string_field(item, "name", path);
            require_strict_string_field(item, "value1", path);
            require_strict_string_field(item, "value2", path);
            require_strict_one_of(
                item,
                std::array<std::string_view, 2>{"time", "timeMs"},
                path,
                "required time/timeMs field is missing"
            );
            if (!item.contains("name")) {
                reject_strict_import(path, "required event name is missing");
            }
        }
    }
}

void validate_strict_psych_event_groups(
    const Json& events,
    const std::string_view path
) {
    if (!events.is_array()) {
        reject_strict_import(
            path,
            "expected an array; permissive event skipping is disabled"
        );
    }
    for (std::size_t group_index = 0; group_index < events.size();
         ++group_index) {
        const auto& group = events[group_index];
        const auto group_path = std::string(path) + "["
            + std::to_string(group_index) + "]";
        if (!group.is_array()
            || group.size() < 2
            || !group[0].is_number()
            || !group[1].is_array()) {
            reject_strict_import(
                group_path,
                "expected [number, event-array]; permissive group skipping "
                "is disabled"
            );
        }
        for (std::size_t entry_index = 0; entry_index < group[1].size();
             ++entry_index) {
            const auto& entry = group[1][entry_index];
            const auto entry_path = group_path + "[1]["
                + std::to_string(entry_index) + "]";
            if (!entry.is_array() || entry.empty() || !entry[0].is_string()) {
                reject_strict_import(
                    entry_path,
                    "expected a non-empty event array with a string name"
                );
            }
            for (std::size_t value_index = 1;
                 value_index < std::min<std::size_t>(entry.size(), 3);
                 ++value_index) {
                if (!entry[value_index].is_string()
                    && !entry[value_index].is_null()) {
                    reject_strict_import(
                        entry_path + "[" + std::to_string(value_index) + "]",
                        "expected a string or null; permissive scalar "
                        "coercion is disabled"
                    );
                }
            }
        }
    }
}

void validate_strict_psych(const Json& root) {
    const Json& song = root.contains("song") && root["song"].is_object()
        ? root["song"]
        : root;
    const std::string_view song_path =
        &song == &root ? "$" : "$.song";
    const bool denpa_schema = is_denpa_schema(root);

    if (denpa_schema) {
        const auto header = song.find("header");
        if (header == song.end() || !header->is_object()) {
            reject_strict_import(
                "$.song.header",
                "required DenpaEx header object is missing or malformed"
            );
        }
        const auto gameplay_options = song.find("options");
        if (gameplay_options == song.end() || !gameplay_options->is_object()) {
            reject_strict_import(
                "$.song.options",
                "required DenpaEx options object is missing or malformed"
            );
        }
        const auto assets = song.find("assets");
        if (assets == song.end() || !assets->is_object()) {
            reject_strict_import(
                "$.song.assets",
                "required DenpaEx assets object is missing or malformed"
            );
        }

        require_strict_string_field(*header, "song", "$.song.header");
        require_strict_number_field(*header, "bpm", "$.song.header");
        require_strict_boolean_field(
            *header,
            "needsVoices",
            "$.song.header"
        );
        if (!header->contains("song")
            || !header->contains("bpm")
            || !header->contains("needsVoices")) {
            reject_strict_import(
                "$.song.header",
                "required song/bpm/needsVoices field is missing"
            );
        }

        require_strict_number_field(
            *gameplay_options,
            "speed",
            "$.song.options"
        );
        if (const auto mania = gameplay_options->find("mania");
            mania != gameplay_options->end() && !mania->is_null()) {
            require_strict_integer_field(
                *gameplay_options,
                "mania",
                "$.song.options"
            );
        }
        if (!gameplay_options->contains("speed")) {
            reject_strict_import(
                "$.song.options",
                "required speed field is missing"
            );
        }
        // PULSEFORGE_P1_4_0_DENPA_PLAYER4_STRICT_V1
        require_strict_string_field(*assets, "player4", "$.song.assets");
        require_strict_boolean_field(*assets, "enablePlayer4", "$.song.assets");
        if (!song.contains("notes")) {
            reject_strict_import(
                "$.song",
                "required notes array is missing"
            );
        }
    } else {
        for (const auto* field : {"song", "name", "artist", "charter"}) {
            require_strict_string_field(song, field, song_path);
        }
        require_strict_integer_field(song, "keyCount", song_path);
        require_strict_integer_field(song, "mania", song_path);
        require_strict_number_field(song, "speed", song_path);
        require_strict_number_field(song, "bpm", song_path);
        if (!song.contains("bpm")) {
            reject_strict_import(song_path, "required bpm field is missing");
        }
    }

    if (const auto sections = song.find("notes"); sections != song.end()) {
        if (!sections->is_array()) {
            reject_strict_import(
                std::string(song_path) + ".notes",
                "expected an array; permissive section skipping is disabled"
            );
        }
        for (std::size_t section_index = 0;
             section_index < sections->size();
             ++section_index) {
            const auto& section = (*sections)[section_index];
            const auto section_path = std::string(song_path) + ".notes["
                + std::to_string(section_index) + "]";
            if (!section.is_object()) {
                reject_strict_import(
                    section_path,
                    "expected an object; permissive section skipping is "
                    "disabled"
                );
            }
            require_strict_boolean_field(
                section,
                "changeBPM",
                section_path
            );
            require_strict_boolean_field(
                section,
                "mustHitSection",
                section_path
            );
            require_strict_number_field(section, "bpm", section_path);
            const auto length_in_steps = section.find("lengthInSteps");
            if (!denpa_schema
                || length_in_steps == section.end()
                || !length_in_steps->is_null()) {
                require_strict_number_field(
                    section,
                    "lengthInSteps",
                    section_path
                );
            }
            if (!section.contains("mustHitSection")) {
                reject_strict_import(
                    section_path,
                    "required mustHitSection field is missing"
                );
            }

            const auto raw_notes = section.find("sectionNotes");
            if (raw_notes == section.end()) {
                reject_strict_import(
                    section_path,
                    "required sectionNotes array is missing"
                );
            }
            if (!raw_notes->is_array()) {
                reject_strict_import(
                    section_path + ".sectionNotes",
                    "expected an array; permissive note skipping is disabled"
                );
            }
            for (std::size_t note_index = 0;
                 note_index < raw_notes->size();
                 ++note_index) {
                const auto& raw_note = (*raw_notes)[note_index];
                const auto note_path = section_path + ".sectionNotes["
                    + std::to_string(note_index) + "]";
                if (!raw_note.is_array()
                    || raw_note.size() < 2
                    || !raw_note[0].is_number()
                    || !is_json_integer(raw_note[1])) {
                    reject_strict_import(
                        note_path,
                        "expected at least [number, integer]; permissive note "
                        "skipping is disabled"
                    );
                }

                const auto raw_lane = integer_value_or(raw_note[1], -1);
                if (raw_lane < 0) {
                    if (raw_note.size() < 3 || !raw_note[2].is_string()) {
                        reject_strict_import(
                            note_path,
                            "negative-lane event requires a string event name"
                        );
                    }
                    for (std::size_t value_index = 3;
                         value_index < std::min<std::size_t>(
                             raw_note.size(),
                             5
                         );
                         ++value_index) {
                        if (!raw_note[value_index].is_string()
                            && !raw_note[value_index].is_null()) {
                            reject_strict_import(
                                note_path + "["
                                    + std::to_string(value_index) + "]",
                                "expected a string or null; permissive scalar "
                                "coercion is disabled"
                            );
                        }
                    }
                    continue;
                }

                if (raw_note.size() > 2 && !raw_note[2].is_number()) {
                    reject_strict_import(
                        note_path + "[2]",
                        "expected a numeric sustain length; permissive "
                        "defaulting is disabled"
                    );
                }
                if (raw_note.size() > 2 && raw_note[2].is_number()) {
                    const double sustain_length = raw_note[2].get<double>();
                    if (!std::isfinite(sustain_length)
                        || sustain_length < 0.0) {
                        reject_strict_import(
                            note_path + "[2]",
                            "expected a finite non-negative sustain length; "
                            "permissive legacy normalization is disabled"
                        );
                    }
                }
                if (raw_note.size() > 3
                    && !raw_note[3].is_null()
                    && !raw_note[3].is_string()
                    && !raw_note[3].is_number()
                    && !(denpa_schema && raw_note[3].is_boolean())) {
                    reject_strict_import(
                        note_path + "[3]",
                        "expected a scalar or null note type"
                    );
                }
            }
        }
    }

    if (const auto events = song.find("events"); events != song.end()) {
        validate_strict_psych_event_groups(
            *events,
            std::string(song_path) + ".events"
        );
    }
    if (&song != &root) {
        if (const auto events = root.find("events"); events != root.end()) {
            validate_strict_psych_event_groups(*events, "$.events");
        }
    }
}

void validate_strict_vslice_metadata(
    const Json& metadata,
    const std::string_view path
) {
    for (const auto* field : {"songName", "title", "artist", "charter"}) {
        require_strict_string_field(metadata, field, path);
    }

    const auto changes = metadata.find("timeChanges");
    if (changes == metadata.end()) {
        return;
    }
    if (!changes->is_array()) {
        reject_strict_import(
            std::string(path) + ".timeChanges",
            "expected an array; permissive field skipping is disabled"
        );
    }
    for (std::size_t index = 0; index < changes->size(); ++index) {
        const auto& item = (*changes)[index];
        const auto item_path = std::string(path) + ".timeChanges["
            + std::to_string(index) + "]";
        if (!item.is_object()) {
            reject_strict_import(
                item_path,
                "expected an object; permissive item skipping is disabled"
            );
        }
        require_strict_number_field(item, "b", item_path);
        require_strict_number_field(item, "t", item_path);
        require_strict_number_field(item, "bpm", item_path);
        require_strict_integer_field(item, "n", item_path);
        require_strict_integer_field(item, "d", item_path);
        if (!item.contains("t") || !item.contains("bpm")) {
            reject_strict_import(
                item_path,
                "required t and bpm fields are missing"
            );
        }
    }
}

void validate_strict_vslice_notes(
    const Json& notes,
    const std::string_view path
) {
    if (!notes.is_array()) {
        reject_strict_import(
            path,
            "expected an array; permissive note skipping is disabled"
        );
    }
    for (std::size_t index = 0; index < notes.size(); ++index) {
        const auto& item = notes[index];
        const auto item_path = std::string(path) + "["
            + std::to_string(index) + "]";
        if (!item.is_object()) {
            reject_strict_import(
                item_path,
                "expected an object; permissive item skipping is disabled"
            );
        }
        require_strict_number_field(item, "t", item_path);
        require_strict_number_field(item, "time", item_path);
        require_strict_number_field(item, "l", item_path);
        require_strict_number_field(item, "duration", item_path);
        require_strict_integer_field(item, "d", item_path);
        require_strict_integer_field(item, "lane", item_path);
        require_strict_string_field(item, "k", item_path);
        require_strict_string_field(item, "kind", item_path);
        require_strict_one_of(
            item,
            std::array<std::string_view, 2>{"t", "time"},
            item_path,
            "required t/time field is missing"
        );
        require_strict_one_of(
            item,
            std::array<std::string_view, 2>{"d", "lane"},
            item_path,
            "required d/lane field is missing"
        );
    }
}

void validate_strict_vslice(const Json& root) {
    validate_strict_vslice_metadata(root, "$");

    if (const auto speed = root.find("scrollSpeed"); speed != root.end()) {
        if (speed->is_object()) {
            for (const auto& [name, value] : speed->items()) {
                if (!value.is_number()) {
                    reject_strict_import(
                        "$.scrollSpeed." + name,
                        "expected a number; permissive difficulty fallback is "
                        "disabled"
                    );
                }
            }
        } else if (!speed->is_number()) {
            reject_strict_import(
                "$.scrollSpeed",
                "expected a number or difficulty object"
            );
        }
    }

    if (const auto notes = root.find("notes"); notes != root.end()) {
        if (notes->is_array()) {
            validate_strict_vslice_notes(*notes, "$.notes");
        } else if (notes->is_object()) {
            for (const auto& [difficulty, value] : notes->items()) {
                validate_strict_vslice_notes(
                    value,
                    "$.notes." + difficulty
                );
            }
        } else {
            reject_strict_import(
                "$.notes",
                "expected an array or difficulty object"
            );
        }
    }

    if (const auto events = root.find("events"); events != root.end()) {
        if (!events->is_array()) {
            reject_strict_import(
                "$.events",
                "expected an array; permissive field skipping is disabled"
            );
        }
        for (std::size_t index = 0; index < events->size(); ++index) {
            const auto& item = (*events)[index];
            const auto item_path = "$.events[" + std::to_string(index) + "]";
            if (!item.is_object()) {
                reject_strict_import(
                    item_path,
                    "expected an object; permissive item skipping is disabled"
                );
            }
            require_strict_number_field(item, "t", item_path);
            require_strict_number_field(item, "time", item_path);
            require_strict_string_field(item, "e", item_path);
            require_strict_string_field(item, "name", item_path);
            // Current V-Slice event values are arbitrary JSON. Structural
            // preservation and bounded-size validation happen while parsing.
        }
    }
}

void parse_audio(
    Chart& chart,
    const Json& audio,
    const std::filesystem::path& source_path
) {
    const auto base = source_path.empty()
        ? std::filesystem::path{}
        : source_path.parent_path();
    const auto instrumental = string_or(audio, "instrumental");
    if (!instrumental.empty()) {
        auto path = std::filesystem::path(instrumental);
        chart.audio.instrumental = path.is_relative() ? base / path : std::move(path);
    }

    const auto vocals = audio.find("vocals");
    if (vocals == audio.end()) {
        return;
    }
    if (vocals->is_string()) {
        auto path = std::filesystem::path(vocals->get<std::string>());
        chart.audio.vocals.push_back(path.is_relative() ? base / path : std::move(path));
    } else if (vocals->is_array()) {
        for (const auto& item : *vocals) {
            if (!item.is_string()) {
                continue;
            }
            auto path = std::filesystem::path(item.get<std::string>());
            chart.audio.vocals.push_back(path.is_relative() ? base / path : std::move(path));
        }
    }
}

void resolve_conventional_audio(
    Chart& chart,
    const std::filesystem::path& source_path,
    std::string_view requested_song_id,
    bool discover_vocals
);

[[nodiscard]] Chart parse_native(
    const Json& root,
    const std::filesystem::path& source_path,
    const ChartLoadOptions& options
) {
    Chart chart;
    chart.source_format = ChartFormat::native;
    const Json& song = root.contains("song") && root["song"].is_object()
        ? root["song"]
        : root;
    chart.title = string_or(song, "title", string_or(song, "name", chart.title));
    chart.artist = string_or(song, "artist", chart.artist);
    chart.charter = string_or(song, "charter", chart.charter);
    chart.difficulty = string_or(song, "difficulty", options.difficulty);
    chart.stage_id = string_or(song, "stage");
    chart.player_character = string_or(song, "player1");
    chart.opponent_character = string_or(song, "player2");
    chart.girlfriend_character = string_or(
        song,
        "gfVersion",
        string_or(song, "girlfriend")
    );
    chart.note_style = string_or(
        song,
        "noteStyle",
        string_or(song, "arrowSkin")
    );
    const auto native_key_count = integer_or(song, "keyCount", 4);
    chart.key_count =
        native_key_count >= 1
                && native_key_count <= maximum_supported_key_count
            ? static_cast<std::uint16_t>(native_key_count)
            : std::numeric_limits<std::uint16_t>::max();
    chart.chart_scroll_speed = number_or(
        song,
        "scrollSpeed",
        number_or(root, "scrollSpeed", 1.0)
    );

    bool discover_native_vocals = true;
    if (const auto audio = root.find("audio");
        audio != root.end() && audio->is_object()) {
        discover_native_vocals = !audio->contains("vocals");
        parse_audio(chart, *audio, source_path);
    }

    if (const auto tempos = root.find("tempos");
        tempos != root.end() && tempos->is_array()) {
        for (const auto& item : *tempos) {
            if (!item.is_object()) {
                continue;
            }
            chart.tempos.push_back({
                number_or(item, "time", number_or(item, "timeMs", 0.0)),
                number_or(item, "bpm", 120.0),
                unsigned16_or_invalid(item, "numerator", 4),
                unsigned16_or_invalid(item, "denominator", 4),
            });
        }
    }

    if (const auto notes = root.find("notes"); notes != root.end() && notes->is_array()) {
        chart.notes.reserve(notes->size());
        for (const auto& item : *notes) {
            if (!item.is_object()) {
                continue;
            }
            const auto owner = string_or(item, "owner", "player");
            chart.notes.push_back({
                number_or(item, "time", number_or(item, "timeMs", 0.0)),
                number_or(item, "duration", number_or(item, "durationMs", 0.0)),
                lane_or_invalid(item, "lane"),
                owner == "opponent" || owner == "enemy"
                    ? NoteOwner::opponent
                    : NoteOwner::player,
                string_or(item, "kind", string_or(item, "type", "normal")),
            });
        }

        // Native lanes are unambiguous (unlike Psych's combined two-side
        // domain), so a valid note may safely widen stale or absent metadata.
        // A lane at/above the engine limit remains untouched and is rejected
        // by validation instead of being truncated or wrapped.
        if (chart.key_count <= maximum_supported_key_count) {
            for (const auto& note : chart.notes) {
                if (note.lane < maximum_supported_key_count) {
                    chart.key_count = std::max<std::uint16_t>(
                        chart.key_count,
                        static_cast<std::uint16_t>(note.lane + 1U)
                    );
                }
            }
        }
    }

    if (const auto events = root.find("events");
        events != root.end() && events->is_array()) {
        chart.events.reserve(events->size());
        for (const auto& item : *events) {
            if (!item.is_object()) {
                continue;
            }
            chart.events.push_back({
                number_or(item, "time", number_or(item, "timeMs", 0.0)),
                string_or(item, "name"),
                string_or(item, "value1"),
                string_or(item, "value2"),
            });
        }
    }

    resolve_conventional_audio(
        chart,
        source_path,
        chart.title,
        discover_native_vocals
    );
    return chart;
}

void parse_psych_events(Chart& chart, const Json& events) {
    if (!events.is_array()) {
        return;
    }
    for (const auto& event_group : events) {
        if (!event_group.is_array() || event_group.size() < 2 || !event_group[0].is_number()) {
            continue;
        }
        const double time = event_group[0].get<double>();
        const auto& entries = event_group[1];
        if (!entries.is_array()) {
            continue;
        }
        for (const auto& entry : entries) {
            if (!entry.is_array() || entry.empty()) {
                continue;
            }
            chart.events.push_back({
                time,
                scalar_string(entry[0]),
                entry.size() > 1 ? scalar_string(entry[1]) : std::string{},
                entry.size() > 2 ? scalar_string(entry[2]) : std::string{},
            });
        }
    }
}

void validate_strict_psych_section_events(
    const Json& sections,
    const std::string_view path
) {
    if (!sections.is_array()) {
        reject_strict_import(
            path,
            "expected an array; permissive section skipping is disabled"
        );
    }
    for (std::size_t section_index = 0; section_index < sections.size();
         ++section_index) {
        const auto& section = sections[section_index];
        const auto section_path = std::string(path) + "["
            + std::to_string(section_index) + "]";
        if (!section.is_object()) {
            reject_strict_import(
                section_path,
                "expected an object; permissive section skipping is disabled"
            );
        }
        const auto notes = section.find("sectionNotes");
        if (notes == section.end() || !notes->is_array()) {
            reject_strict_import(
                section_path + ".sectionNotes",
                "expected an array; permissive event skipping is disabled"
            );
        }
        for (std::size_t note_index = 0; note_index < notes->size();
             ++note_index) {
            const auto& raw_note = (*notes)[note_index];
            const auto note_path = section_path + ".sectionNotes["
                + std::to_string(note_index) + "]";
            if (!raw_note.is_array()
                || raw_note.size() < 2U
                || !raw_note[0].is_number()
                || !is_json_integer(raw_note[1])) {
                reject_strict_import(
                    note_path,
                    "expected at least [number, integer]; permissive event "
                    "skipping is disabled"
                );
            }

            // Old Psych event sidecars are full chart-shaped documents. Valid
            // non-event notes can therefore coexist with the negative-lane
            // tuples and are intentionally ignored here.
            if (integer_value_or(raw_note[1], 0) >= 0) {
                continue;
            }
            if (raw_note.size() < 3U || !raw_note[2].is_string()) {
                reject_strict_import(
                    note_path,
                    "negative-lane event requires a string event name"
                );
            }
            for (std::size_t value_index = 3U;
                 value_index < std::min<std::size_t>(raw_note.size(), 5U);
                 ++value_index) {
                if (!raw_note[value_index].is_string()
                    && !raw_note[value_index].is_null()) {
                    reject_strict_import(
                        note_path + "[" + std::to_string(value_index) + "]",
                        "expected a string or null; permissive scalar "
                        "coercion is disabled"
                    );
                }
            }
        }
    }
}

void parse_psych_section_events(Chart& chart, const Json& sections) {
    if (!sections.is_array()) {
        return;
    }
    for (const auto& section : sections) {
        if (!section.is_object()) {
            continue;
        }
        const auto notes = section.find("sectionNotes");
        if (notes == section.end() || !notes->is_array()) {
            continue;
        }
        for (const auto& raw_note : *notes) {
            if (!raw_note.is_array()
                || raw_note.size() < 3U
                || !raw_note[0].is_number()
                || !is_json_integer(raw_note[1])
                || integer_value_or(raw_note[1], 0) >= 0) {
                continue;
            }
            chart.events.push_back({
                raw_note[0].get<double>(),
                scalar_string(raw_note[2]),
                raw_note.size() > 3U
                    ? scalar_string(raw_note[3])
                    : std::string{},
                raw_note.size() > 4U
                    ? scalar_string(raw_note[4])
                    : std::string{},
            });
        }
    }
}

[[nodiscard]] std::optional<std::uint64_t> largest_psych_raw_lane(
    const Json& song
) {
    std::optional<std::uint64_t> result;
    const auto sections = song.find("notes");
    if (sections == song.end() || !sections->is_array()) {
        return result;
    }
    for (const auto& section : *sections) {
        if (!section.is_object()) {
            continue;
        }
        const auto notes = section.find("sectionNotes");
        if (notes == section.end() || !notes->is_array()) {
            continue;
        }
        for (const auto& raw_note : *notes) {
            if (!raw_note.is_array() || raw_note.size() < 2U
                || !raw_note[1].is_number_integer()) {
                continue;
            }
            const auto raw_lane = integer_value_or(raw_note[1], -1);
            if (raw_lane < 0) {
                continue;
            }
            const auto lane = static_cast<std::uint64_t>(raw_lane);
            result = result.has_value() ? std::max(*result, lane) : lane;
        }
    }
    return result;
}

[[nodiscard]] std::int64_t psych_key_count(
    const Json& song,
    const Json& gameplay_options,
    const bool has_scoped_options
) {
    const auto integer_field = [](const Json& object, const std::string_view key)
        -> std::optional<std::int64_t> {
        const auto iterator = object.find(key);
        if (iterator == object.end() || !is_json_integer(*iterator)) {
            return std::nullopt;
        }
        return integer_value_or(*iterator, 0);
    };
    std::optional<std::int64_t> declared;
    const auto absorb_lane_count = [&](const std::optional<std::int64_t> value) {
        if (!value.has_value() || *value <= 0) {
            return true;
        }
        if (*value > maximum_supported_key_count) {
            return false;
        }
        declared = declared.has_value()
            ? std::max(*declared, *value)
            : *value;
        return true;
    };
    if (!absorb_lane_count(integer_field(song, "keyCount"))
        || !absorb_lane_count(integer_field(song, "mania"))) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (has_scoped_options) {
        const auto mania_index = integer_field(gameplay_options, "mania");
        if (mania_index.has_value()) {
            if (*mania_index < 0
                || *mania_index >= maximum_supported_key_count) {
                return std::numeric_limits<std::int64_t>::max();
            }
            const auto option_key_count = *mania_index + 1;
            declared = declared.has_value()
                ? std::max(*declared, option_key_count)
                : option_key_count;
        }
    }
    if (!declared.has_value()) {
        declared = 4;
    }

    const auto largest = largest_psych_raw_lane(song);
    if (!largest.has_value()) {
        return *declared;
    }
    // Psych stores both strumlines in [0, 2K). The smallest K capable of
    // representing the observed maximum is floor(max / 2) + 1. Metadata wins
    // when it declares a wider mode; real notes widen stale/default metadata.
    // With no metadata, a sparse chart whose maximum is below 8 is inherently
    // ambiguous (4K opposite-side versus a wider primary side), so the
    // ecosystem-compatible 4K default remains the lower bound. Third Strum is
    // an ownership override, not evidence for a separate lane domain.
    const auto required = *largest / 2U + 1U;
    if (required > maximum_supported_key_count) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return std::max<std::int64_t>(
        *declared,
        static_cast<std::int64_t>(required)
    );
}

void parse_adjacent_psych_events(
    Chart& chart,
    const std::filesystem::path& source_path,
    const bool strict
) {
    if (source_path.empty()) {
        return;
    }

    std::optional<std::filesystem::path> events_path;
    for (const auto* filename : {"events.json", "event.json"}) {
        const auto candidate = source_path.parent_path() / filename;
        std::error_code filesystem_error;
        if (!std::filesystem::is_regular_file(candidate, filesystem_error)
            || filesystem_error) {
            continue;
        }

        filesystem_error.clear();
        if (std::filesystem::equivalent(
                source_path,
                candidate,
                filesystem_error
            ) && !filesystem_error) {
            continue;
        }
        events_path = candidate;
        break;
    }
    if (!events_path.has_value()) {
        return;
    }

    auto read = read_limited_file(
        *events_path,
        maximum_events_json_bytes,
        "cannot open adjacent events JSON: ",
        "cannot read adjacent events JSON: ",
        "adjacent events JSON exceeds the 16 MiB safety limit"
    );
    if (!read.text.has_value()) {
        throw std::runtime_error(read.error);
    }

    const auto root = Json::parse(*read.text, nullptr, true, true);
    if (!root.is_object()) {
        throw std::runtime_error(
            "adjacent events JSON root must be an object"
        );
    }

    const Json& song = root.contains("song") && root["song"].is_object()
        ? root["song"]
        : root;
    const auto sidecar_name = path_utf8(events_path->filename());
    bool recognized = false;

    // Psych 0.5-era sidecars were chart-shaped and encoded events as notes
    // whose lane was negative. Preserve the same event ordering as a normal
    // Psych chart: section events, song events, then root events.
    if (const auto sections = song.find("notes"); sections != song.end()) {
        recognized = true;
        if (strict) {
            validate_strict_psych_section_events(
                *sections,
                sidecar_name + (&song == &root ? ".notes" : ".song.notes")
            );
        }
        if (!sections->is_array()) {
            throw std::runtime_error(
                "adjacent events JSON notes field must be an array"
            );
        }
        parse_psych_section_events(chart, *sections);
    }

    const auto parse_event_array = [&](const Json& container,
                                       const std::string& path) {
        const auto events = container.find("events");
        if (events == container.end()) {
            return false;
        }
        if (!strict && events->is_null()) {
            return false;
        }
        if (strict) {
            validate_strict_psych_event_groups(*events, path);
        }
        if (!events->is_array()) {
            throw std::runtime_error(
                "adjacent events JSON events field must be an array"
            );
        }
        parse_psych_events(chart, *events);
        return true;
    };
    recognized = parse_event_array(
        song,
        sidecar_name + (&song == &root
            ? ".events"
            : ".song.events")
    ) || recognized;
    if (&song != &root) {
        recognized = parse_event_array(
            root,
            sidecar_name + ".events"
        ) || recognized;
    }

    if (!recognized) {
        throw std::runtime_error(
            "adjacent events JSON contains neither an events array nor "
            "legacy sectionNotes"
        );
    }
}

void resolve_conventional_audio(
    Chart& chart,
    const std::filesystem::path& source_path,
    const std::string_view requested_song_id,
    const bool discover_vocals
) {
    if (source_path.empty()) {
        return;
    }

    const auto requested_song_path = requested_song_id.empty()
        ? source_path.parent_path().filename()
        : std::filesystem::path(requested_song_id);
    const auto song_id =
        requested_song_path.is_absolute()
            || requested_song_path.has_root_name()
            || requested_song_path.has_root_directory()
            || requested_song_path.has_parent_path()
            || requested_song_path == "."
            || requested_song_path == ".."
        ? source_path.parent_path().filename()
        : requested_song_path;
    const auto chart_directory = source_path.parent_path();
    const auto chart_folder_id = chart_directory.filename();
    std::vector<std::filesystem::path> candidate_directories;
    candidate_directories.reserve(32U);
    if (!chart.audio.instrumental.empty()) {
        append_unique_directory(
            candidate_directories,
            chart.audio.instrumental.parent_path()
        );
    }
    append_unique_directory(candidate_directories, chart_directory);

    // Psych, Slice, JS Engine, DenpaEx and V-Slice all converge on some form
    // of `<root>/songs/<song>/Inst.*`, but `root` can be a mod, assets folder,
    // packaged build, or one of several nested engine directories. Walking a
    // bounded ancestry is deterministic and avoids an expensive recursive
    // search across the whole installation.
    auto ancestor = chart_directory;
    for (std::size_t depth = 0U; depth < 10U && !ancestor.empty(); ++depth) {
        append_unique_directory(
            candidate_directories,
            ancestor / "songs" / song_id
        );
        append_unique_directory(
            candidate_directories,
            ancestor / "songs" / chart_folder_id
        );
        append_unique_directory(
            candidate_directories,
            ancestor / "assets" / "songs" / song_id
        );
        append_unique_directory(
            candidate_directories,
            ancestor / "assets" / "songs" / chart_folder_id
        );
        const auto parent = ancestor.parent_path();
        if (parent == ancestor) {
            break;
        }
        ancestor = parent;
    }

    const auto requested_id = canonical_audio_id(path_utf8(song_id));
    const auto folder_id = canonical_audio_id(path_utf8(chart_folder_id));
    const auto title_id = canonical_audio_id(chart.title);
    const auto difficulty_id = canonical_audio_id(chart.difficulty);

    // PULSEFORGE_P1_2_0_STOCK_AUDIO_FALLBACK_V1
    // Some Psych/FNF mods intentionally omit base-game Inst/Voices and rely on
    // the engine's stock asset library. PulseForge's own assets directory is
    // intentionally small, so use the same single complete sibling provider
    // as the runtime content resolver. Custom song IDs never cross mod packs.
    std::optional<std::size_t> stock_provider_directory;
    std::string stock_song_id;
    for (const auto& id : {requested_id, folder_id, title_id}) {
        if (!id.empty() && psych_stock::is_stock_song_id(id)) {
            stock_song_id = id;
            break;
        }
    }
    if (!stock_song_id.empty()) {
        const auto provider = psych_stock::discover_stock_provider_cached(
            chart_directory
        );
        if (provider.found()) {
            const auto candidate = provider.assets_root / "songs" / stock_song_id;
            const auto before = candidate_directories.size();
            append_unique_directory(candidate_directories, candidate);
            if (candidate_directories.size() > before) {
                stock_provider_directory = before;
            } else {
                const auto normalized = candidate.lexically_normal();
                const auto found = std::find_if(
                    candidate_directories.begin(),
                    candidate_directories.end(),
                    [&](const auto& current) {
                        return path_equal_ascii_insensitive(current, normalized);
                    }
                );
                if (found != candidate_directories.end()) {
                    stock_provider_directory = static_cast<std::size_t>(
                        std::distance(candidate_directories.begin(), found)
                    );
                }
            }
        }
    }
    const auto matches_named_instrumental = [&](const std::string& stem) {
        if (stem == "inst" || stem == "instrumental" || stem == "music") {
            return true;
        }
        for (const auto& id : {requested_id, folder_id, title_id}) {
            if (id.empty()) {
                continue;
            }
            if (stem == id
                || (stem.size() > id.size()
                    && stem.starts_with(id)
                    && stem[id.size()] == '-')) {
                return true;
            }
        }
        return false;
    };
    const auto matches_vocal = [](const std::string& stem) {
        return stem == "voices" || stem == "secvoices"
            || stem == "vocals" || stem.starts_with("voices-")
            || stem.starts_with("secvoices-")
            || stem.starts_with("vocals-");
    };

    std::optional<std::size_t> selected_directory;
    if (chart.audio.instrumental.empty()) {
        for (std::size_t directory_index = 0U;
             directory_index < candidate_directories.size();
             ++directory_index) {
            const auto files = audio_files_in(
                candidate_directories[directory_index]
            );
            const auto difficulty_inst = std::find_if(
                files.begin(),
                files.end(),
                [&](const auto& path) {
                    const auto stem = canonical_audio_id(
                        path_utf8(path.stem())
                    );
                    return !difficulty_id.empty()
                        && (stem == "inst-" + difficulty_id
                            || stem == "instrumental-" + difficulty_id);
                }
            );
            const auto exact_inst = std::find_if(
                files.begin(),
                files.end(),
                [](const auto& path) {
                    return canonical_audio_id(path_utf8(path.stem())) == "inst";
                }
            );
            auto named = difficulty_inst != files.end()
                ? difficulty_inst
                : exact_inst != files.end()
                    ? exact_inst
                    : std::find_if(
                        files.begin(),
                        files.end(),
                        [&](const auto& path) {
                            return matches_named_instrumental(
                                canonical_audio_id(path_utf8(path.stem()))
                            );
                        }
                    );
            if (named == files.end()) {
                // A pack containing exactly one alternate Inst is unambiguous.
                // With two or more variants we deliberately refuse to guess.
                auto only_variant = files.end();
                for (auto candidate = files.begin(); candidate != files.end();
                     ++candidate) {
                    const auto stem = canonical_audio_id(
                        path_utf8(candidate->stem())
                    );
                    if (!stem.starts_with("inst-")
                        && !stem.starts_with("instrumental-")) {
                        continue;
                    }
                    if (only_variant != files.end()) {
                        only_variant = files.end();
                        break;
                    }
                    only_variant = candidate;
                }
                named = only_variant;
            }
            if (named != files.end()) {
                chart.audio.instrumental = *named;
                selected_directory = directory_index;
                break;
            }
        }
    } else {
        selected_directory = 0U;
    }

    if (!discover_vocals || chart.audio.vocals.size()
        >= maximum_discovered_vocal_stems) {
        return;
    }

    // Vocal stems must come from the selected song directory. If no
    // instrumental exists, a vocal-only export is still valid, but only the
    // first matching song directory is used so stems can never bleed between
    // two songs with similar names.
    std::vector<std::size_t> vocal_directory_order;
    if (selected_directory.has_value()) {
        vocal_directory_order.push_back(*selected_directory);
        // A base-song mod may override only Inst while intentionally inheriting
        // stock Voices. Try the stock provider only if the selected local song
        // directory contains no compatible vocal stems; the loop returns on
        // the first directory that contributes vocals.
        if (stock_provider_directory.has_value()
            && *stock_provider_directory != *selected_directory) {
            vocal_directory_order.push_back(*stock_provider_directory);
        }
    } else {
        vocal_directory_order.resize(candidate_directories.size());
        for (std::size_t index = 0U; index < vocal_directory_order.size();
             ++index) {
            vocal_directory_order[index] = index;
        }
    }
    for (const auto directory_index : vocal_directory_order) {
        const auto files = audio_files_in(
            candidate_directories[directory_index]
        );
        std::vector<std::string> instrumental_variant_suffixes;
        instrumental_variant_suffixes.reserve(8U);
        for (const auto& file : files) {
            const auto stem = canonical_audio_id(path_utf8(file.stem()));
            std::string suffix;
            if (stem.starts_with("inst-") && stem.size() > 5U) {
                suffix = stem.substr(5U);
            } else if (stem.starts_with("instrumental-")
                && stem.size() > 13U) {
                suffix = stem.substr(13U);
            }
            if (!suffix.empty()
                && std::find(
                    instrumental_variant_suffixes.begin(),
                    instrumental_variant_suffixes.end(),
                    suffix
                ) == instrumental_variant_suffixes.end()) {
                instrumental_variant_suffixes.push_back(std::move(suffix));
            }
        }
        const auto has_suffix = [](const std::string_view stem,
                                   const std::string_view suffix) {
            return !suffix.empty() && stem.size() > suffix.size()
                && stem.ends_with(suffix)
                && stem[stem.size() - suffix.size() - 1U] == '-';
        };
        const bool has_difficulty_vocals = !difficulty_id.empty()
            && std::any_of(files.begin(), files.end(), [&](const auto& file) {
                const auto stem = canonical_audio_id(path_utf8(file.stem()));
                return matches_vocal(stem)
                    && has_suffix(stem, difficulty_id);
            });
        const auto matches_selected_vocal = [&](const std::string& stem) {
            if (!matches_vocal(stem)) {
                return false;
            }
            if (has_difficulty_vocals) {
                return has_suffix(stem, difficulty_id);
            }
            // When a difficulty-specific instrumental exists, similarly
            // suffixed vocal stems belong exclusively to that mix. Excluding
            // them from the base fallback prevents normal+erect voices from
            // being summed together.
            return std::none_of(
                instrumental_variant_suffixes.begin(),
                instrumental_variant_suffixes.end(),
                [&](const auto& suffix) { return has_suffix(stem, suffix); }
            );
        };
        bool found_in_directory = false;
        for (const auto& file : files) {
            if (!matches_selected_vocal(
                    canonical_audio_id(path_utf8(file.stem()))
                )) {
                continue;
            }
            const auto duplicate = std::find_if(
                chart.audio.vocals.begin(),
                chart.audio.vocals.end(),
                [&](const auto& existing) {
                    return path_equal_ascii_insensitive(existing, file);
                }
            );
            if (duplicate == chart.audio.vocals.end()) {
                chart.audio.vocals.push_back(file);
            }
            found_in_directory = true;
            if (chart.audio.vocals.size()
                >= maximum_discovered_vocal_stems) {
                return;
            }
        }
        if (found_in_directory) {
            return;
        }
    }
}

[[nodiscard]] Chart parse_psych(
    const Json& root,
    const std::filesystem::path& source_path,
    const ChartLoadOptions& options
) {
    Chart chart;
    const auto& song = root.contains("song") && root["song"].is_object()
        ? root["song"]
        : root;
    const bool denpa_schema = is_denpa_schema(root);
    chart.source_format = denpa_schema
        ? ChartFormat::denpa
        : ChartFormat::psych;

    const Json* header = &song;
    const Json* gameplay_options = &song;
    bool has_scoped_options = false;
    if (denpa_schema) {
        const auto header_field = song.find("header");
        const auto options_field = song.find("options");
        if (header_field == song.end() || !header_field->is_object()
            || options_field == song.end() || !options_field->is_object()) {
            throw std::runtime_error(
                "DenpaEx chart requires object-valued header and options"
            );
        }
        header = &*header_field;
        gameplay_options = &*options_field;
        has_scoped_options = true;
    } else if (const auto options_field = song.find("options");
               options_field != song.end() && options_field->is_object()) {
        // JS/Psych-family exports can carry indexed mania/speed settings in a
        // scoped options object without also using Denpa's header schema.
        gameplay_options = &*options_field;
        has_scoped_options = true;
    }

    chart.title = string_or(
        *header,
        "song",
        string_or(song, "name", "Untitled")
    );
    chart.artist = string_or(*header, "artist", string_or(song, "artist", "Unknown"));
    chart.charter = string_or(
        *header,
        "charter",
        string_or(song, "charter", "Unknown")
    );
    chart.difficulty = options.difficulty;
    chart.stage_id = string_or(song, "stage");
    chart.player_character = string_or(song, "player1");
    chart.opponent_character = string_or(song, "player2");
    chart.girlfriend_character = string_or(song, "gfVersion");
    chart.note_style = string_or(song, "arrowSkin");
    if (denpa_schema) {
        // PULSEFORGE_P1_4_0_DENPA_PLAYER4_METADATA_V1
        // DenpaEx scopes character metadata under song.assets.  Preserve the
        // second opponent instead of silently falling back to player2.
        if (const auto assets = song.find("assets");
            assets != song.end() && assets->is_object()) {
            chart.player_character = string_or(
                *assets, "player1", chart.player_character
            );
            chart.opponent_character = string_or(
                *assets, "player2", chart.opponent_character
            );
            chart.girlfriend_character = string_or(
                *assets, "gfVersion", chart.girlfriend_character
            );
            chart.secondary_opponent_character = string_or(*assets, "player4");
            chart.secondary_opponent_enabled = boolean_or(
                *assets, "enablePlayer4", false
            );
            chart.note_style = string_or(
                *assets, "arrowSkin", chart.note_style
            );
            chart.stage_id = string_or(*assets, "stage", chart.stage_id);
        }
    }
    const auto key_count = psych_key_count(
        song,
        *gameplay_options,
        has_scoped_options
    );
    chart.key_count =
        key_count >= 1 && key_count <= maximum_supported_key_count
            ? static_cast<std::uint16_t>(key_count)
            : std::numeric_limits<std::uint16_t>::max();
    chart.chart_scroll_speed = has_scoped_options
        ? number_or(
            *gameplay_options,
            "speed",
            number_or(song, "speed", 1.0)
        )
        : number_or(song, "speed", 1.0);
    const bool discover_vocals = boolean_or(
        *header,
        "needsVoices",
        !denpa_schema
    );

    double current_bpm = number_or(*header, "bpm", 120.0);
    if (!std::isfinite(current_bpm)
        || current_bpm <= 0.0) {
        throw std::runtime_error(
            "Psych song BPM is outside the supported range"
        );
    }
    double section_start_ms = 0.0;
    chart.tempos.push_back({0.0, current_bpm, 4, 4});

    const auto sections = song.find("notes");
    if (sections != song.end() && sections->is_array()) {
        for (const auto& section : *sections) {
            if (!section.is_object()) {
                continue;
            }
            if (boolean_or(section, "changeBPM", false)) {
                const double changed_bpm = number_or(section, "bpm", current_bpm);
                if (!std::isfinite(changed_bpm)
                    || changed_bpm <= 0.0) {
                    throw std::runtime_error(
                        "Psych section BPM is outside the supported range"
                    );
                }
                if (std::abs(changed_bpm - current_bpm) > 0.0001) {
                    current_bpm = changed_bpm;
                    chart.tempos.push_back({section_start_ms, current_bpm, 4, 4});
                }
            }

            const bool must_hit_section = boolean_or(section, "mustHitSection", false);
            const auto section_notes = section.find("sectionNotes");
            if (section_notes != section.end() && section_notes->is_array()) {
                for (const auto& raw_note : *section_notes) {
                    if (!raw_note.is_array()
                        || raw_note.size() < 2
                        || !raw_note[0].is_number()
                        || !raw_note[1].is_number_integer()) {
                        continue;
                    }
                    const auto raw_lane = integer_value_or(raw_note[1], -1);
                    if (raw_lane < 0) {
                        if (raw_note.size() >= 3) {
                            chart.events.push_back({
                                raw_note[0].get<double>(),
                                scalar_string(raw_note[2]),
                                raw_note.size() > 3
                                    ? scalar_string(raw_note[3])
                                    : std::string{},
                                raw_note.size() > 4
                                    ? scalar_string(raw_note[4])
                                    : std::string{},
                            });
                        }
                        continue;
                    }
                    const auto lane_domain =
                        static_cast<std::int64_t>(chart.key_count) * 2;
                    if (raw_lane >= lane_domain) {
                        throw std::runtime_error(
                            "Psych note lane is outside the two strumlines"
                        );
                    }
                    const bool other_side = raw_lane >= chart.key_count;
                    bool player = other_side
                        ? !must_hit_section
                        : must_hit_section;
                    const double raw_duration =
                        raw_note.size() > 2 && raw_note[2].is_number()
                        ? raw_note[2].get<double>()
                        : 0.0;
                    // Several legacy Psych converters encode a tap note as
                    // end-time (zero) minus strum-time, producing -strumTime.
                    // Psych/Haxe treats that as a tap because it emits no
                    // sustain segments. Preserve the note in permissive mode
                    // while retaining NaN/overflow for structural validation.
                    const double duration = raw_duration < 0.0
                        ? 0.0
                        : raw_duration;
                    std::string kind = "normal";
                    if (raw_note.size() > 3 && !raw_note[3].is_null()) {
                        kind = scalar_string(raw_note[3]);
                        if (kind == "0" || kind.empty()) {
                            kind = "normal";
                        }
                    }
                    // PULSEFORGE_P1_4_0D_THIRD_STRUM_IMPORT_PARITY_V1
                    // The literal Third Strum note type is the canonical
                    // secondary-opponent identity across Psych-family and
                    // DenpaEx imports. This matches PFC1 runtime projection and
                    // allows PulseForge's Psych editor export to round-trip.
                    const auto owner = kind == "Third Strum"
                        ? NoteOwner::secondary_opponent
                        : player ? NoteOwner::player : NoteOwner::opponent;
                    // PULSEFORGE_P1_4_0_DENPA_THIRD_STRUM_IMPORT_V1
                    chart.notes.push_back({
                        raw_note[0].get<double>(),
                        duration,
                        static_cast<std::uint16_t>(
                            raw_lane % static_cast<std::int64_t>(chart.key_count)
                        ),
                        owner,
                        std::move(kind),
                    });
                }
            }

            const double length_steps = number_or(section, "lengthInSteps", 16.0);
            section_start_ms += length_steps * (60'000.0 / current_bpm) / 4.0;
        }
    }

    if (const auto events = song.find("events"); events != song.end()) {
        parse_psych_events(chart, *events);
    }
    if (&song != &root) {
        if (const auto events = root.find("events"); events != root.end()) {
            parse_psych_events(chart, *events);
        }
    }
    parse_adjacent_psych_events(chart, source_path, options.strict);

    resolve_conventional_audio(
        chart,
        source_path,
        string_or(
            *header,
            "song",
            source_path.parent_path().filename().string()
        ),
        discover_vocals
    );

    return chart;
}

void parse_vslice_time_changes(Chart& chart, const Json& metadata) {
    const auto iterator = metadata.find("timeChanges");
    if (iterator == metadata.end() || !iterator->is_array()) {
        return;
    }

    double previous_time = 0.0;
    double previous_beat = 0.0;
    double previous_bpm = 120.0;
    for (const auto& item : *iterator) {
        if (!item.is_object()) {
            continue;
        }
        const double beat = number_or(item, "b", previous_beat);
        const double derived_time = previous_time
            + (beat - previous_beat) * 60'000.0 / previous_bpm;
        const double time = number_or(item, "t", derived_time);
        const double bpm = number_or(item, "bpm", previous_bpm);
        chart.tempos.push_back({
            time,
            bpm,
            unsigned16_or_invalid(item, "n", 4),
            unsigned16_or_invalid(item, "d", 4),
        });
        previous_time = time;
        previous_beat = beat;
        previous_bpm = bpm;
    }
}

void apply_vslice_metadata(Chart& chart, const Json& metadata) {
    chart.title = string_or(
        metadata,
        "songName",
        string_or(metadata, "title", chart.title)
    );
    chart.artist = string_or(metadata, "artist", chart.artist);
    chart.charter = string_or(metadata, "charter", chart.charter);
    if (const auto play_data = metadata.find("playData");
        play_data != metadata.end() && play_data->is_object()) {
        chart.stage_id = string_or(*play_data, "stage", chart.stage_id);
        chart.note_style = string_or(
            *play_data,
            "noteStyle",
            chart.note_style
        );
        if (const auto characters = play_data->find("characters");
            characters != play_data->end() && characters->is_object()) {
            chart.player_character = string_or(
                *characters,
                "player",
                chart.player_character
            );
            chart.opponent_character = string_or(
                *characters,
                "opponent",
                chart.opponent_character
            );
            chart.girlfriend_character = string_or(
                *characters,
                "girlfriend",
                chart.girlfriend_character
            );
        }
    }
    parse_vslice_time_changes(chart, metadata);
}

[[nodiscard]] Chart parse_vslice(
    const Json& root,
    const std::filesystem::path& source_path,
    const ChartLoadOptions& options
) {
    Chart chart;
    chart.source_format = ChartFormat::vslice;
    chart.difficulty = options.difficulty;
    chart.key_count = 4;

    if (root.contains("songName") || root.contains("timeChanges")) {
        apply_vslice_metadata(chart, root);
    }
    if (options.metadata_path.has_value()) {
        auto metadata_read = read_limited_file(
            *options.metadata_path,
            maximum_metadata_json_bytes,
            "cannot open V-Slice metadata file: ",
            "cannot read V-Slice metadata file: ",
            "V-Slice metadata JSON exceeds the 16 MiB safety limit"
        );
        if (!metadata_read.text.has_value()) {
            throw std::runtime_error(std::move(metadata_read.error));
        }
        const auto metadata = Json::parse(
            *metadata_read.text,
            nullptr,
            true,
            true
        );
        if (!metadata.is_object()) {
            throw std::runtime_error(
                "V-Slice metadata root must be a JSON object"
            );
        }
        if (options.strict) {
            validate_strict_vslice_metadata(metadata, "$metadata");
        }
        apply_vslice_metadata(chart, metadata);
    }

    const auto scroll_speed = root.find("scrollSpeed");
    if (scroll_speed != root.end()) {
        if (scroll_speed->is_number()) {
            chart.chart_scroll_speed = scroll_speed->get<double>();
        } else if (scroll_speed->is_object()) {
            chart.chart_scroll_speed = number_or(
                *scroll_speed,
                options.difficulty,
                number_or(*scroll_speed, "default", 1.0)
            );
        }
    }

    const auto notes = root.find("notes");
    if (notes != root.end()) {
        const Json* selected = nullptr;
        if (notes->is_array()) {
            selected = &*notes;
        } else if (notes->is_object()) {
            const auto difficulty = notes->find(options.difficulty);
            if (difficulty != notes->end() && difficulty->is_array()) {
                selected = &*difficulty;
            } else {
                if (options.difficulty_explicit) {
                    throw std::runtime_error(
                        "V-Slice difficulty not found: " + options.difficulty
                    );
                }
                const auto normal = notes->find("normal");
                if (normal != notes->end() && normal->is_array()) {
                    selected = &*normal;
                    chart.difficulty = "normal";
                }
            }
            if (selected == nullptr && !options.difficulty_explicit) {
                const Json* only_difficulty = nullptr;
                std::string only_name;
                for (const auto& [name, value] : notes->items()) {
                    if (value.is_array()) {
                        if (only_difficulty != nullptr) {
                            only_difficulty = nullptr;
                            only_name.clear();
                            break;
                        }
                        only_difficulty = &value;
                        only_name = name;
                    }
                }
                if (only_difficulty != nullptr) {
                    selected = only_difficulty;
                    chart.difficulty = std::move(only_name);
                }
            }
            if (selected == nullptr) {
                throw std::runtime_error(
                    "V-Slice difficulty not found: " + options.difficulty
                );
            }
        }

        if (selected != nullptr) {
            chart.notes.reserve(selected->size());
            std::unordered_map<std::string, std::uint32_t> payload_ids;
            for (const auto& item : *selected) {
                if (!item.is_object()) {
                    continue;
                }
                const auto raw_direction = integer_or(
                    item,
                    "d",
                    integer_or(item, "lane", 0)
                );
                if (raw_direction < 0) {
                    throw std::runtime_error(
                        "V-Slice note direction cannot be negative"
                    );
                }
                const auto lane_domain =
                    static_cast<std::int64_t>(chart.key_count) * 2;
                if (raw_direction >= lane_domain) {
                    throw std::runtime_error(
                        "V-Slice note direction is outside the two strumlines"
                    );
                }
                // V-Slice encodes the strumline in floor(direction / keyCount):
                // 0 is the player and 1 is the opponent. This is intentionally
                // the inverse of the legacy Psych section-side convention.
                const bool player = raw_direction < chart.key_count;
                std::uint32_t payload_id = 0;
                if (const auto payload = item.find("p");
                    payload != item.end()) {
                    auto canonical = bounded_json_payload(
                        *payload,
                        maximum_chart_note_payload_bytes,
                        "V-Slice note payload"
                    );
                    const auto next_id = static_cast<std::uint32_t>(
                        chart.note_payloads.size()
                    );
                    const auto [iterator, inserted] = payload_ids.emplace(
                        canonical,
                        next_id
                    );
                    if (inserted) {
                        chart.note_payloads.push_back(std::move(canonical));
                    }
                    payload_id = iterator->second;
                }
                chart.notes.push_back({
                    number_or(item, "t", number_or(item, "time", 0.0)),
                    number_or(item, "l", number_or(item, "duration", 0.0)),
                    static_cast<std::uint16_t>(
                        raw_direction % static_cast<std::int64_t>(chart.key_count)
                    ),
                    player ? NoteOwner::player : NoteOwner::opponent,
                    string_or(item, "k", string_or(item, "kind", "normal")),
                    payload_id,
                });
            }
        }
    }

    if (const auto events = root.find("events");
        events != root.end() && events->is_array()) {
        for (const auto& item : *events) {
            if (!item.is_object()) {
                continue;
            }
            std::string value1;
            std::string value2;
            std::string payload_json;
            if (const auto values = item.find("v"); values != item.end()) {
                payload_json = bounded_json_payload(
                    *values,
                    maximum_chart_event_value_bytes,
                    "V-Slice event payload"
                );
                if (values->is_array()) {
                    if (!values->empty()) {
                        value1 = scalar_string((*values)[0]);
                    }
                    if (values->size() > 1) {
                        value2 = scalar_string((*values)[1]);
                    }
                } else {
                    value1 = scalar_string(*values);
                }
            }
            chart.events.push_back({
                number_or(item, "t", number_or(item, "time", 0.0)),
                string_or(item, "e", string_or(item, "name")),
                std::move(value1),
                std::move(value2),
                std::move(payload_json),
            });
        }
    }

    if (chart.tempos.empty()) {
        chart.tempos.push_back({0.0, 120.0, 4, 4});
    }

    resolve_conventional_audio(
        chart,
        source_path,
        source_path.parent_path().filename().string(),
        true
    );
    return chart;
}

[[nodiscard]] ChartFormat detect_format_root(const Json& root) {
    if (is_denpa_schema(root)) {
        return ChartFormat::denpa;
    }
    const Json* psych_song = &root;
    if (root.contains("song") && root["song"].is_object()) {
        psych_song = &root["song"];
    }
    bool has_psych_sections = false;
    if (const auto notes = psych_song->find("notes");
        notes != psych_song->end() && notes->is_array()) {
        has_psych_sections = std::any_of(
            notes->begin(),
            notes->end(),
            [](const Json& item) {
                return item.is_object() && item.contains("sectionNotes");
            }
        );
    }
    if (has_psych_sections
        || (psych_song != &root
            && psych_song->contains("notes")
            && (*psych_song)["notes"].is_array())) {
        return ChartFormat::psych;
    }
    const auto format = string_or(root, "format");
    if (format.find("pulseforge") != std::string::npos
        || root.contains("tempos")
        || (root.contains("song")
            && root["song"].is_object()
            && root["song"].contains("title"))) {
        return ChartFormat::native;
    }
    if (root.contains("timeChanges")
        || (root.contains("notes") && root["notes"].is_object())
        || root.contains("scrollSpeed")) {
        return ChartFormat::vslice;
    }
    return ChartFormat::native;
}

[[nodiscard]] ChartLoadResult finalize_chart(
    Chart chart,
    const ChartLoadOptions& options
) {
    const auto raw_issues = validate_chart(chart);
    const auto unrecoverable_raw_issue = std::find_if(
        raw_issues.begin(),
        raw_issues.end(),
        [](const ValidationIssue& issue) {
            if (issue.severity != ValidationSeverity::error) {
                return false;
            }
            return issue.message.find("not sorted by time")
                    == std::string::npos
                && issue.message
                        != "the chart must contain at least one tempo change";
        }
    );
    if (unrecoverable_raw_issue != raw_issues.end()) {
        return {
            std::nullopt,
            "chart validation failed before normalization\n- "
                + unrecoverable_raw_issue->message
                + " (item "
                + std::to_string(unrecoverable_raw_issue->item_index)
                + ')',
            };
    }

    // normalize() only sorts, deduplicates tempo timestamps, supplies the
    // documented initial tempo, and canonicalizes an empty note kind. All
    // other mutations were rejected above, so permissive imports avoid a
    // second O(N) scan. Strict imports retain a final scan because warnings
    // that occur after hundreds of repairable ordering diagnostics must not
    // be hidden by the bounded diagnostic collector.
    chart.normalize();
    // Supplying time zero can add one tempo after the raw-size validation.
    // This is the only normalization mutation that can increase a bounded
    // container, so retain the O(1) postcondition without rescanning notes.
    if (chart.tempos.size() > maximum_chart_tempo_changes) {
        return {
            std::nullopt,
            "chart validation failed after normalization\n- "
            "chart has too many tempo changes",
        };
    }
    if (options.strict) {
        const auto normalized_issues = validate_chart(chart);
        if (!normalized_issues.empty()) {
            std::ostringstream message;
            message << "chart validation failed";
            for (const auto& issue : normalized_issues) {
                message << "\n- " << issue.message << " (item "
                        << issue.item_index << ')';
            }
            return {std::nullopt, message.str()};
        }
    }
    return {std::move(chart), {}};
}

}  // namespace

ChartLoadResult ChartLoader::load(
    const std::filesystem::path& path,
    const ChartLoadOptions& options
) {
    try {
        std::error_code size_error;
        const auto reported_size = std::filesystem::file_size(
            path,
            size_error
        );
        if (!size_error && reported_size > maximum_chart_json_bytes) {
            return {
                std::nullopt,
                "chart JSON exceeds the 512 MiB safety limit: "
                    + path.string(),
            };
        }

        // Large Psych-family files use a bounded on-demand traversal. Strict
        // imports intentionally retain the DOM parser's exhaustive diagnostics.
        if (!options.strict
            && !size_error
            && reported_size >= fast_psych_minimum_bytes) {
            auto fast = detail::load_fast_psych_chart(
                path,
                options.difficulty
            );
            if (fast.recognized) {
                if (!fast.error.empty()) {
                    return {
                        std::nullopt,
                        "invalid chart JSON: " + fast.error,
                    };
                }
                if (!fast.chart.has_value()) {
                    return {
                        std::nullopt,
                        "invalid chart JSON: Psych chart could not be decoded",
                    };
                }

                auto chart = std::move(*fast.chart);
                parse_adjacent_psych_events(chart, path, false);
                resolve_conventional_audio(
                    chart,
                    path,
                    fast.requested_song_id,
                    fast.discover_vocals
                );
                return finalize_chart(std::move(chart), options);
            }
        }

        auto read = read_limited_file(
            path,
            maximum_chart_json_bytes,
            "cannot open chart: ",
            "cannot read chart: ",
            "chart JSON exceeds the 512 MiB safety limit"
        );
        if (!read.text.has_value()) {
            return {std::nullopt, std::move(read.error)};
        }
        return parse(*read.text, path, options);
    } catch (const std::exception& exception) {
        return {
            std::nullopt,
            std::string("cannot read chart: ") + exception.what(),
        };
    }
}

ChartLoadResult ChartLoader::parse(
    const std::string_view json_text,
    const std::filesystem::path& source_path,
    const ChartLoadOptions& options
) {
    if (json_text.size() > maximum_chart_json_bytes) {
        return {std::nullopt, "chart JSON exceeds the 512 MiB safety limit"};
    }
    try {
        const auto root = Json::parse(json_text, nullptr, true, true);
        if (!root.is_object()) {
            return {std::nullopt, "chart root must be a JSON object"};
        }

        if (root.contains("song") && root["song"].is_object()) {
            const auto& song = root["song"];
            if (song.contains("notes") && !song["notes"].is_array()) {
                return {
                    std::nullopt,
                    "Psych song.notes must be a JSON array",
                };
            }
        }

        const auto format = detect_format_root(root);
        const bool recognizable_native =
            root.contains("tempos")
            || (root.contains("notes") && root["notes"].is_array())
            || (root.contains("song")
                && root["song"].is_object()
                && (root["song"].contains("title")
                    || root["song"].contains("keyCount")))
            || string_or(root, "format").find("pulseforge") != std::string::npos;
        const bool recognizable_vslice =
            root.contains("timeChanges")
            || root.contains("scrollSpeed")
            || (root.contains("notes") && root["notes"].is_object());
        if (format == ChartFormat::native
            && !recognizable_native
            && !recognizable_vslice) {
            return {
                std::nullopt,
                "unrecognized chart schema",
            };
        }
        if (options.strict) {
            switch (format) {
            case ChartFormat::native:
            case ChartFormat::midi:
            case ChartFormat::pfm:
                // MIDI/PFM bypass ChartLoader. Grouping them here keeps this
                // JSON-only switch exhaustive and provides a defensive native
                // validation fallback if a caller ever forwards such a value.
                validate_strict_native(root);
                break;
            case ChartFormat::psych:
                validate_strict_psych(root);
                break;
            case ChartFormat::denpa:
                validate_strict_psych(root);
                break;
            case ChartFormat::vslice:
                validate_strict_vslice(root);
                break;
            }
        }
        Chart chart;
        switch (format) {
        case ChartFormat::native:
        case ChartFormat::midi:
        case ChartFormat::pfm:
            chart = parse_native(root, source_path, options);
            break;
        case ChartFormat::psych:
            chart = parse_psych(root, source_path, options);
            break;
        case ChartFormat::denpa:
            chart = parse_psych(root, source_path, options);
            break;
        case ChartFormat::vslice:
            chart = parse_vslice(root, source_path, options);
            break;
        }

        return finalize_chart(std::move(chart), options);
    } catch (const std::exception& exception) {
        return {
            std::nullopt,
            std::string("invalid chart JSON: ") + exception.what(),
        };
    }
}

ChartFormat ChartLoader::detect_format(const std::string_view json_text) {
    if (json_text.size() > maximum_chart_json_bytes) {
        return ChartFormat::native;
    }
    try {
        const auto root = Json::parse(json_text, nullptr, true, true);
        if (!root.is_object()) {
            return ChartFormat::native;
        }
        return detect_format_root(root);
    } catch (...) {
        return ChartFormat::native;
    }
}

void ChartLoader::resolve_conventional_audio(
    Chart& chart,
    const std::filesystem::path& source_path,
    const std::string_view requested_song_id,
    const bool discover_vocals
) {
    ::pulseforge::resolve_conventional_audio(
        chart,
        source_path,
        requested_song_id,
        discover_vocals
    );
}

}  // namespace pulseforge
