#include "fast_psych_loader.hpp"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100 4244)
#endif
#include <simdjson.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace pulseforge::detail {
namespace {

using simdjson::ondemand::array;
using simdjson::ondemand::field;
using simdjson::ondemand::json_type;
using simdjson::ondemand::number;
using simdjson::ondemand::object;
using simdjson::ondemand::value;
using Json = nlohmann::json;

constexpr std::size_t maximum_metadata_text_bytes = 1'024;
constexpr std::size_t file_read_chunk_bytes = 64U * 1024U;
constexpr std::size_t maximum_json_nesting_depth = 128;
constexpr std::size_t maximum_scalar_json_source_bytes = 64U * 1024U;

class FastPathFallback final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct PsychMetadata {
    bool wrapped{};
    bool saw_notes_array{};
    bool saw_section_notes{};
    bool saw_events_field{};
    bool duplicate_notes_field{};
    bool duplicate_events_field{};
    bool duplicate_section_notes_field{};
    bool requires_dom_fallback{};
    bool saw_header_field{};
    bool saw_options_field{};
    bool saw_header_object{};
    bool saw_options_object{};
    bool header_needs_voices{};
    bool raw_entries_exceeded{};
    bool saw_raw_lane{};
    std::size_t raw_section_entries{};
    std::uint64_t max_raw_lane{};

    std::optional<std::string> song_title;
    std::optional<std::string> song_name;
    std::optional<std::string> song_artist;
    std::optional<std::string> song_charter;
    std::optional<std::string> song_stage;
    std::optional<std::string> song_player;
    std::optional<std::string> song_opponent;
    // PULSEFORGE_P1_4_0_FAST_DENPA_PLAYER4_METADATA_V1
    std::optional<std::string> song_secondary_opponent;
    std::optional<bool> song_secondary_opponent_enabled;
    std::optional<std::string> song_girlfriend;
    std::optional<std::string> song_note_style;
    std::optional<std::string> header_title;
    std::optional<std::string> header_artist;
    std::optional<std::string> header_charter;

    std::optional<double> song_bpm;
    std::optional<double> header_bpm;
    std::optional<double> song_speed;
    std::optional<double> option_speed;
    std::optional<std::int64_t> song_key_count;
    std::optional<std::int64_t> song_mania;
    std::optional<std::int64_t> option_mania;
};

struct ParseState {
    Chart chart;
    bool saw_section_notes{};
    std::size_t parsed_event_count{};
    double current_bpm{120.0};
    double section_start_ms{};
};

[[noreturn]] void throw_simdjson_error(const simdjson::error_code error) {
    throw simdjson::simdjson_error(error);
}

void validate_json_value(value json_value, std::size_t depth = 0);

[[nodiscard]] std::string bounded_scalar_string(
    value json_value,
    std::size_t maximum_bytes,
    std::string_view description
);

[[nodiscard]] bool read_string(
    value json_value,
    std::optional<std::string>& destination,
    const std::size_t maximum_bytes = maximum_metadata_text_bytes
) {
    json_type type{};
    const auto type_error = json_value.type().get(type);
    if (type_error != simdjson::SUCCESS) {
        throw_simdjson_error(type_error);
    }
    if (type == json_type::number || type == json_type::boolean) {
        destination = bounded_scalar_string(
            json_value,
            maximum_bytes,
            "chart metadata text"
        );
        return true;
    }
    if (type != json_type::string) {
        validate_json_value(json_value);
        destination.reset();
        return false;
    }
    std::string_view text;
    const auto error = json_value.get_string().get(text);
    if (error != simdjson::SUCCESS) {
        throw_simdjson_error(error);
    }
    if (text.size() > maximum_bytes) {
        throw FastPathFallback(
            "chart metadata text exceeds its safety limit"
        );
    }
    destination = std::string(text);
    return true;
}

[[nodiscard]] std::optional<double> read_number(value json_value) {
    double parsed{};
    const auto error = json_value.get_double().get(parsed);
    if (error != simdjson::SUCCESS) {
        validate_json_value(json_value);
        return std::nullopt;
    }
    return parsed;
}

[[nodiscard]] std::optional<std::int64_t> read_integer(value json_value) {
    std::int64_t signed_value{};
    if (json_value.get_int64().get(signed_value) == simdjson::SUCCESS) {
        return signed_value;
    }

    number parsed;
    const auto error = json_value.get_number().get(parsed);
    if (error == simdjson::BIGINT_ERROR) {
        double ignored{};
        const auto double_error = json_value.get_double().get(ignored);
        if (double_error != simdjson::SUCCESS) {
            throw_simdjson_error(double_error);
        }
        return std::nullopt;
    }
    if (error != simdjson::SUCCESS) {
        validate_json_value(json_value);
        return std::nullopt;
    }
    if (parsed.is_int64()) {
        return parsed.get_int64();
    }
    if (parsed.is_uint64()) {
        const auto unsigned_value = parsed.get_uint64();
        if (unsigned_value > static_cast<std::uint64_t>(
                                 std::numeric_limits<std::int64_t>::max())) {
            return std::numeric_limits<std::int64_t>::max();
        }
        return static_cast<std::int64_t>(unsigned_value);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<bool> read_boolean(value json_value) {
    bool parsed{};
    const auto error = json_value.get_bool().get(parsed);
    if (error != simdjson::SUCCESS) {
        validate_json_value(json_value);
        return std::nullopt;
    }
    return parsed;
}

[[nodiscard]] bool get_type(value json_value, json_type& type) {
    const auto error = json_value.type().get(type);
    if (error != simdjson::SUCCESS) {
        throw_simdjson_error(error);
    }
    return true;
}

[[nodiscard]] bool get_object(value json_value, object& result) {
    const auto error = json_value.get_object().get(result);
    if (error == simdjson::SUCCESS) {
        return true;
    }
    if (error == simdjson::INCORRECT_TYPE) {
        return false;
    }
    throw_simdjson_error(error);
}

[[nodiscard]] bool get_array(value json_value, array& result) {
    const auto error = json_value.get_array().get(result);
    if (error == simdjson::SUCCESS) {
        return true;
    }
    if (error == simdjson::INCORRECT_TYPE) {
        return false;
    }
    throw_simdjson_error(error);
}

void validate_json_value(value json_value, const std::size_t depth) {
    if (depth > maximum_json_nesting_depth) {
        throw FastPathFallback(
            "chart JSON nesting exceeds its safety limit"
        );
    }

    json_type type{};
    (void)get_type(json_value, type);
    switch (type) {
    case json_type::array: {
        array items;
        if (!get_array(json_value, items)) {
            throw std::runtime_error("invalid JSON array");
        }
        for (value item : items) {
            validate_json_value(item, depth + 1U);
        }
        return;
    }
    case json_type::object: {
        object fields;
        if (!get_object(json_value, fields)) {
            throw std::runtime_error("invalid JSON object");
        }
        for (field item : fields) {
            std::string_view key;
            const auto key_error = item.unescaped_key().get(key);
            if (key_error != simdjson::SUCCESS) {
                throw_simdjson_error(key_error);
            }
            (void)key;
            validate_json_value(item.value(), depth + 1U);
        }
        return;
    }
    case json_type::number: {
        double parsed{};
        const auto error = json_value.get_double().get(parsed);
        if (error != simdjson::SUCCESS) {
            throw_simdjson_error(error);
        }
        return;
    }
    case json_type::string: {
        std::string_view parsed;
        const auto error = json_value.get_string().get(parsed);
        if (error != simdjson::SUCCESS) {
            throw_simdjson_error(error);
        }
        return;
    }
    case json_type::boolean: {
        bool parsed{};
        const auto error = json_value.get_bool().get(parsed);
        if (error != simdjson::SUCCESS) {
            throw_simdjson_error(error);
        }
        return;
    }
    case json_type::null: {
        bool parsed{};
        const auto error = json_value.is_null().get(parsed);
        if (error != simdjson::SUCCESS || !parsed) {
            if (error != simdjson::SUCCESS) {
                throw_simdjson_error(error);
            }
            throw std::runtime_error("invalid JSON null value");
        }
        return;
    }
    case json_type::unknown:
        throw std::runtime_error("chart contains an invalid JSON value");
    }
    throw std::runtime_error("chart contains an unsupported JSON value");
}

[[nodiscard]] std::string bounded_scalar_string(
    value json_value,
    const std::size_t maximum_bytes,
    const std::string_view description
) {
    json_type type{};
    (void)get_type(json_value, type);
    if (type == json_type::unknown) {
        throw std::runtime_error("chart contains an invalid JSON scalar");
    }
    if (type == json_type::null) {
        validate_json_value(json_value);
        return {};
    }

    std::string_view text;
    if (type == json_type::string) {
        const auto error = json_value.get_string().get(text);
        if (error != simdjson::SUCCESS) {
            throw_simdjson_error(error);
        }
        if (text.size() > maximum_bytes) {
            throw std::runtime_error(
                std::string(description) + " exceeds its safety limit"
            );
        }
        return std::string(text);
    }

    if (type == json_type::boolean) {
        bool parsed{};
        const auto error = json_value.get_bool().get(parsed);
        if (error != simdjson::SUCCESS) {
            throw_simdjson_error(error);
        }
        return parsed ? "true" : "false";
    }

    if (type == json_type::number) {
        number parsed;
        const auto error = json_value.get_number().get(parsed);
        if (error == simdjson::BIGINT_ERROR) {
            text = json_value.raw_json_token();
            if (text.size() > maximum_scalar_json_source_bytes) {
                throw FastPathFallback(
                    std::string(description)
                        + " source exceeds its safety limit"
                );
            }
            const auto canonical = Json::parse(
                text,
                nullptr,
                true,
                true
            ).dump();
            if (canonical.size() > maximum_bytes) {
                throw std::runtime_error(
                    std::string(description) + " exceeds its safety limit"
                );
            }
            return canonical;
        }
        if (error != simdjson::SUCCESS) {
            throw_simdjson_error(error);
        }
        std::string canonical;
        if (parsed.is_int64()) {
            canonical = std::to_string(parsed.get_int64());
        } else if (parsed.is_uint64()) {
            canonical = std::to_string(parsed.get_uint64());
        } else {
            canonical = Json(parsed.as_double()).dump();
        }
        if (canonical.size() > maximum_bytes) {
            throw std::runtime_error(
                std::string(description) + " exceeds its safety limit"
            );
        }
        return canonical;
    }

    if (type == json_type::array || type == json_type::object) {
        const auto error = json_value.raw_json().get(text);
        if (error != simdjson::SUCCESS) {
            throw_simdjson_error(error);
        }
    } else {
        text = json_value.raw_json_token();
    }
    if (text.size() > maximum_scalar_json_source_bytes) {
        throw FastPathFallback(
            std::string(description) + " source exceeds its safety limit"
        );
    }
    const auto canonical = Json::parse(text, nullptr, true, true).dump();
    if (canonical.size() > maximum_bytes) {
        throw std::runtime_error(
            std::string(description) + " exceeds its safety limit"
        );
    }
    return canonical;
}

void parse_header_metadata(object header, PsychMetadata& metadata) {
    for (field item : header) {
        const std::string_view key = item.unescaped_key();
        value json_value = item.value();
        if (key == "song") {
            (void)read_string(json_value, metadata.header_title);
        } else if (key == "artist") {
            (void)read_string(json_value, metadata.header_artist);
        } else if (key == "charter") {
            (void)read_string(json_value, metadata.header_charter);
        } else if (key == "bpm") {
            metadata.header_bpm = read_number(json_value);
        } else if (key == "needsVoices") {
            metadata.header_needs_voices =
                read_boolean(json_value).value_or(false);
        } else {
            validate_json_value(json_value);
        }
    }
}

void parse_options_metadata(object options, PsychMetadata& metadata) {
    for (field item : options) {
        const std::string_view key = item.unescaped_key();
        value json_value = item.value();
        if (key == "speed") {
            metadata.option_speed = read_number(json_value);
        } else if (key == "mania") {
            metadata.option_mania = read_integer(json_value);
        } else {
            validate_json_value(json_value);
        }
    }
}

void parse_assets_metadata(object assets, PsychMetadata& metadata) {
    // PULSEFORGE_P1_4_0_FAST_DENPA_PLAYER4_METADATA_V1
    for (field item : assets) {
        const std::string_view key = item.unescaped_key();
        value json_value = item.value();
        if (key == "player1") {
            (void)read_string(json_value, metadata.song_player);
        } else if (key == "player2") {
            (void)read_string(json_value, metadata.song_opponent);
        } else if (key == "player4") {
            (void)read_string(json_value, metadata.song_secondary_opponent);
        } else if (key == "enablePlayer4") {
            metadata.song_secondary_opponent_enabled = read_boolean(json_value);
        } else if (key == "gfVersion" || key == "player3") {
            (void)read_string(json_value, metadata.song_girlfriend);
        } else if (key == "arrowSkin" || key == "noteStyle") {
            (void)read_string(json_value, metadata.song_note_style);
        } else if (key == "stage") {
            (void)read_string(json_value, metadata.song_stage);
        } else {
            validate_json_value(json_value);
        }
    }
}

void preflight_psych_sections(
    value json_value,
    PsychMetadata& metadata,
    bool& psych_candidate
) {
    array sections;
    if (!get_array(json_value, sections)) {
        return;
    }
    for (value section_value : sections) {
        object section;
        if (!get_object(section_value, section)) {
            validate_json_value(section_value);
            continue;
        }
        bool saw_section_notes_field{};
        for (field item : section) {
            std::string_view key;
            const auto key_error = item.unescaped_key().get(key);
            if (key_error != simdjson::SUCCESS) {
                throw_simdjson_error(key_error);
            }
            if (key != "sectionNotes") {
                validate_json_value(item.value());
                continue;
            }
            if (saw_section_notes_field) {
                metadata.duplicate_section_notes_field = true;
            }
            saw_section_notes_field = true;
            metadata.saw_section_notes = true;
            psych_candidate = true;
            array notes;
            if (!get_array(item.value(), notes)) {
                validate_json_value(item.value());
                continue;
            }
            for (value raw_note : notes) {
                constexpr auto aggregate_limit =
                    maximum_chart_notes + maximum_chart_events;
                if (metadata.raw_section_entries >= aggregate_limit) {
                    metadata.raw_entries_exceeded = true;
                } else {
                    ++metadata.raw_section_entries;
                }
                array fields;
                if (!get_array(raw_note, fields)) {
                    validate_json_value(raw_note);
                    continue;
                }
                std::size_t field_index{};
                for (value raw_field : fields) {
                    if (field_index == 1U) {
                        const auto lane = read_integer(raw_field);
                        if (lane.has_value() && *lane >= 0) {
                            metadata.saw_raw_lane = true;
                            metadata.max_raw_lane = std::max(
                                metadata.max_raw_lane,
                                static_cast<std::uint64_t>(*lane)
                            );
                        }
                    } else {
                        validate_json_value(raw_field);
                    }
                    ++field_index;
                }
            }
        }
    }
}

void parse_song_metadata_field(
    const std::string_view key,
    value json_value,
    PsychMetadata& metadata,
    const bool wrapped_context,
    bool& psych_candidate
) {
    if (key == "notes") {
        if (metadata.saw_notes_array) {
            metadata.duplicate_notes_field = true;
        }
        json_type type{};
        if (get_type(json_value, type) && type == json_type::array) {
            metadata.saw_notes_array = true;
            if (wrapped_context) {
                psych_candidate = true;
            }
            preflight_psych_sections(
                json_value,
                metadata,
                psych_candidate
            );
        } else {
            validate_json_value(json_value);
        }
    } else if (key == "header") {
        metadata.saw_header_field = true;
        metadata.saw_header_object = false;
        metadata.header_title.reset();
        metadata.header_artist.reset();
        metadata.header_charter.reset();
        metadata.header_bpm.reset();
        metadata.header_needs_voices = false;
        object header;
        if (get_object(json_value, header)) {
            metadata.saw_header_object = true;
            parse_header_metadata(header, metadata);
        } else {
            validate_json_value(json_value);
        }
    } else if (key == "options") {
        metadata.saw_options_field = true;
        metadata.saw_options_object = false;
        metadata.option_speed.reset();
        metadata.option_mania.reset();
        object options;
        if (get_object(json_value, options)) {
            metadata.saw_options_object = true;
            parse_options_metadata(options, metadata);
        } else {
            validate_json_value(json_value);
        }
    } else if (key == "assets") {
        object assets;
        if (get_object(json_value, assets)) {
            parse_assets_metadata(assets, metadata);
        } else {
            validate_json_value(json_value);
        }
    } else if (key == "song") {
        (void)read_string(json_value, metadata.song_title);
    } else if (key == "name") {
        (void)read_string(json_value, metadata.song_name);
    } else if (key == "artist") {
        (void)read_string(json_value, metadata.song_artist);
    } else if (key == "charter") {
        (void)read_string(json_value, metadata.song_charter);
    } else if (key == "stage") {
        (void)read_string(json_value, metadata.song_stage);
    } else if (key == "player1") {
        (void)read_string(json_value, metadata.song_player);
    } else if (key == "player2") {
        (void)read_string(json_value, metadata.song_opponent);
    } else if (key == "gfVersion") {
        (void)read_string(json_value, metadata.song_girlfriend);
    } else if (key == "arrowSkin") {
        (void)read_string(json_value, metadata.song_note_style);
    } else if (key == "bpm") {
        metadata.song_bpm = read_number(json_value);
    } else if (key == "speed") {
        metadata.song_speed = read_number(json_value);
    } else if (key == "keyCount") {
        metadata.song_key_count = read_integer(json_value);
    } else if (key == "mania") {
        metadata.song_mania = read_integer(json_value);
    } else if (key == "events") {
        if (metadata.saw_events_field) {
            metadata.duplicate_events_field = true;
        }
        metadata.saw_events_field = true;
        validate_json_value(json_value);
    } else {
        validate_json_value(json_value);
    }
}

void parse_song_metadata(
    object song,
    PsychMetadata& metadata,
    bool& psych_candidate
) {
    for (field item : song) {
        parse_song_metadata_field(
            item.unescaped_key(),
            item.value(),
            metadata,
            true,
            psych_candidate
        );
    }
}

[[nodiscard]] bool scan_metadata(
    simdjson::ondemand::document& document,
    PsychMetadata& metadata,
    bool& psych_candidate
) {
    object root;
    const auto root_error = document.get_object().get(root);
    if (root_error == simdjson::INCORRECT_TYPE) {
        return false;
    }
    if (root_error != simdjson::SUCCESS) {
        throw_simdjson_error(root_error);
    }

    PsychMetadata root_metadata;
    PsychMetadata wrapped_metadata;
    bool last_song_was_object{};
    bool saw_song_field{};
    bool duplicate_song_field{};

    for (field item : root) {
        const std::string_view key = item.unescaped_key();
        value json_value = item.value();
        if (key == "song") {
            if (saw_song_field) {
                duplicate_song_field = true;
            }
            saw_song_field = true;
            json_type type{};
            if (get_type(json_value, type) && type == json_type::object) {
                object wrapped_song;
                if (!get_object(json_value, wrapped_song)) {
                    return false;
                }
                wrapped_metadata = {};
                wrapped_metadata.wrapped = true;
                parse_song_metadata(
                    wrapped_song,
                    wrapped_metadata,
                    psych_candidate
                );
                last_song_was_object = true;
                continue;
            }
            last_song_was_object = false;
        }
        parse_song_metadata_field(
            key,
            json_value,
            root_metadata,
            false,
            psych_candidate
        );
    }

    const auto has_content_duplicates = [](const PsychMetadata& candidate) {
        return candidate.duplicate_notes_field
            || candidate.duplicate_events_field
            || candidate.duplicate_section_notes_field;
    };
    if (last_song_was_object) {
        wrapped_metadata.requires_dom_fallback = duplicate_song_field
            || has_content_duplicates(wrapped_metadata)
            || root_metadata.duplicate_events_field;
    } else {
        root_metadata.requires_dom_fallback = duplicate_song_field
            || has_content_duplicates(root_metadata);
    }
    metadata = last_song_was_object
        ? std::move(wrapped_metadata)
        : std::move(root_metadata);
    if (metadata.requires_dom_fallback) {
        return false;
    }
    if (metadata.raw_entries_exceeded) {
        throw std::runtime_error(
            "chart contains more note/event entries than the configured "
            "safety limits allow"
        );
    }
    return metadata.saw_notes_array;
}

void ensure_note_capacity(const Chart& chart) {
    if (chart.notes.size() >= maximum_chart_notes) {
        throw std::runtime_error(
            "chart has more than 5000000 notes"
        );
    }
}

void consume_event_capacity(ParseState& state) {
    if (state.parsed_event_count >= maximum_chart_events) {
        throw std::runtime_error(
            "chart has more than 250000 events"
        );
    }
    ++state.parsed_event_count;
}

void append_events(
    Chart& chart,
    std::vector<ChartEvent>& deferred_events
) {
    if (deferred_events.empty()) {
        return;
    }
    if (deferred_events.size()
        > maximum_chart_events - chart.events.size()) {
        throw std::runtime_error(
            "chart has more than 250000 events"
        );
    }
    if (chart.events.empty()) {
        chart.events = std::move(deferred_events);
        return;
    }
    chart.events.reserve(chart.events.size() + deferred_events.size());
    chart.events.insert(
        chart.events.end(),
        std::make_move_iterator(deferred_events.begin()),
        std::make_move_iterator(deferred_events.end())
    );
    deferred_events.clear();
}

void parse_raw_note(
    array raw_note,
    ParseState& state,
    const bool /*denpa_schema*/
) {
    double note_time{};
    double duration{};
    std::int64_t raw_lane{-1};
    bool has_time{};
    bool has_lane{};
    bool has_event_name{};
    std::string kind{"normal"};
    std::string event_name;
    std::string event_value1;
    std::string event_value2;

    std::size_t index = 0;
    for (value item : raw_note) {
        if (index == 0) {
            if (const auto parsed = read_number(item); parsed.has_value()) {
                note_time = *parsed;
                has_time = true;
            }
        } else if (index == 1) {
            if (const auto parsed = read_integer(item); parsed.has_value()) {
                raw_lane = *parsed;
                has_lane = true;
            }
        } else if (index == 2 && has_lane && raw_lane < 0) {
            event_name = bounded_scalar_string(
                item,
                maximum_chart_event_name_bytes,
                "event name"
            );
            has_event_name = true;
        } else if (index == 2) {
            const double raw_duration = read_number(item).value_or(0.0);
            duration = raw_duration < 0.0 ? 0.0 : raw_duration;
        } else if (index == 3 && has_lane && raw_lane < 0) {
            event_value1 = bounded_scalar_string(
                item,
                maximum_chart_event_value_bytes,
                "event value"
            );
        } else if (index == 3) {
            kind = bounded_scalar_string(
                item,
                maximum_chart_note_kind_bytes,
                "note kind"
            );
            if (kind.empty() || kind == "0") {
                kind = "normal";
            }
        } else if (index == 4 && has_lane && raw_lane < 0) {
            event_value2 = bounded_scalar_string(
                item,
                maximum_chart_event_value_bytes,
                "event value"
            );
        } else if (index >= 4) {
            validate_json_value(item);
        }
        ++index;
    }

    if (!has_time || !has_lane) {
        return;
    }
    if (raw_lane < 0) {
        if (has_event_name) {
            consume_event_capacity(state);
            state.chart.events.push_back({
                note_time,
                std::move(event_name),
                std::move(event_value1),
                std::move(event_value2),
            });
        }
        return;
    }

    const auto lane_domain =
        static_cast<std::int64_t>(state.chart.key_count) * 2;
    if (raw_lane >= lane_domain) {
        throw std::runtime_error(
            "Psych note lane is outside the two strumlines"
        );
    }

    ensure_note_capacity(state.chart);
    const bool other_side = raw_lane >= state.chart.key_count;
    const auto lane = static_cast<std::uint16_t>(
        raw_lane % static_cast<std::int64_t>(state.chart.key_count)
    );
    // PULSEFORGE_P1_4_0D_FAST_THIRD_STRUM_IMPORT_PARITY_V1
    // The literal Third Strum note type is canonical across Psych-family and
    // DenpaEx. Preserve its third owner before mustHitSection side flipping.
    const bool third_strum = kind == "Third Strum";
    state.chart.notes.push_back({
        note_time,
        duration,
        lane,
        third_strum
            ? NoteOwner::secondary_opponent
            : (other_side ? NoteOwner::player : NoteOwner::opponent),
        std::move(kind),
    });
}

void parse_section_notes(
    value json_value,
    ParseState& state,
    const bool denpa_schema
) {
    state.saw_section_notes = true;
    array notes;
    if (!get_array(json_value, notes)) {
        return;
    }
    for (value raw_note_value : notes) {
        array raw_note;
        if (get_array(raw_note_value, raw_note)) {
            parse_raw_note(raw_note, state, denpa_schema);
        } else {
            validate_json_value(raw_note_value);
        }
    }
}

void parse_sections(
    value json_value,
    ParseState& state,
    const bool denpa_schema
) {
    array sections;
    if (!get_array(json_value, sections)) {
        return;
    }

    for (value section_value : sections) {
        object section;
        if (!get_object(section_value, section)) {
            continue;
        }

        const auto note_begin = state.chart.notes.size();
        bool must_hit_section{};
        bool change_bpm{};
        double section_bpm = state.current_bpm;
        double length_steps = 16.0;

        for (field item : section) {
            const std::string_view key = item.unescaped_key();
            value field_value = item.value();
            if (key == "mustHitSection") {
                must_hit_section = read_boolean(field_value).value_or(false);
            } else if (key == "changeBPM") {
                change_bpm = read_boolean(field_value).value_or(false);
            } else if (key == "bpm") {
                section_bpm = read_number(field_value).value_or(
                    state.current_bpm
                );
            } else if (key == "lengthInSteps") {
                length_steps = read_number(field_value).value_or(16.0);
            } else if (key == "sectionNotes") {
                parse_section_notes(field_value, state, denpa_schema);
            }
        }

        if (must_hit_section) {
            for (std::size_t index = note_begin;
                 index < state.chart.notes.size();
                 ++index) {
                auto& note = state.chart.notes[index];
                if (note.owner == NoteOwner::secondary_opponent) {
                    // PULSEFORGE_P1_4_0D_FAST_THIRD_STRUM_SECTION_PARITY_V1
                    // Third Strum stays AI-owned regardless of section side.
                    continue;
                }
                note.owner = note.owner == NoteOwner::player
                    ? NoteOwner::opponent
                    : NoteOwner::player;
            }
        }

        if (change_bpm) {
            if (!std::isfinite(section_bpm)
                || section_bpm <= 0.0) {
                throw std::runtime_error(
                    "Psych section BPM is outside the supported range"
                );
            }
            if (std::abs(section_bpm - state.current_bpm) > 0.0001) {
                if (state.chart.tempos.size()
                    >= maximum_chart_tempo_changes) {
                    throw std::runtime_error(
                        "chart has more than 100000 tempo changes"
                    );
                }
                state.current_bpm = section_bpm;
                state.chart.tempos.push_back({
                    state.section_start_ms,
                    state.current_bpm,
                    4,
                    4,
                });
            }
        }
        state.section_start_ms +=
            length_steps * (60'000.0 / state.current_bpm) / 4.0;
    }
}

void parse_event_entry(
    array entry,
    const double time_ms,
    ParseState& state,
    std::vector<ChartEvent>& events
) {
    std::string name;
    std::string value1;
    std::string value2;
    std::size_t index = 0;
    for (value item : entry) {
        if (index == 0) {
            name = bounded_scalar_string(
                item,
                maximum_chart_event_name_bytes,
                "event name"
            );
        } else if (index == 1) {
            value1 = bounded_scalar_string(
                item,
                maximum_chart_event_value_bytes,
                "event value"
            );
        } else if (index == 2) {
            value2 = bounded_scalar_string(
                item,
                maximum_chart_event_value_bytes,
                "event value"
            );
        }
        ++index;
    }
    if (index == 0) {
        return;
    }
    consume_event_capacity(state);
    events.push_back({
        time_ms,
        std::move(name),
        std::move(value1),
        std::move(value2),
    });
}

void parse_event_groups(
    value json_value,
    ParseState& state,
    std::vector<ChartEvent>& events
) {
    array groups;
    if (!get_array(json_value, groups)) {
        return;
    }
    for (value group_value : groups) {
        array group;
        if (!get_array(group_value, group)) {
            continue;
        }
        double time_ms{};
        bool has_time{};
        std::size_t index = 0;
        for (value item : group) {
            if (index == 0) {
                if (const auto parsed = read_number(item); parsed.has_value()) {
                    time_ms = *parsed;
                    has_time = true;
                }
            } else if (index == 1 && has_time) {
                array entries;
                if (get_array(item, entries)) {
                    for (value entry_value : entries) {
                        array entry;
                        if (get_array(entry_value, entry)) {
                            parse_event_entry(
                                entry,
                                time_ms,
                                state,
                                events
                            );
                        }
                    }
                }
            }
            ++index;
        }
    }
}

void parse_song_content(
    object song,
    ParseState& state,
    const bool denpa_schema
) {
    std::vector<ChartEvent> song_events;
    for (field item : song) {
        const std::string_view key = item.unescaped_key();
        value json_value = item.value();
        if (key == "notes") {
            parse_sections(json_value, state, denpa_schema);
        } else if (key == "events") {
            parse_event_groups(json_value, state, song_events);
        }
    }
    append_events(state.chart, song_events);
}

void parse_content(
    simdjson::ondemand::document& document,
    ParseState& state,
    const PsychMetadata& metadata,
    const bool denpa_schema
) {
    object root = document.get_object();
    std::vector<ChartEvent> deferred_events;
    for (field item : root) {
        const std::string_view key = item.unescaped_key();
        value json_value = item.value();
        if (metadata.wrapped && key == "song") {
            object song;
            if (get_object(json_value, song)) {
                parse_song_content(song, state, denpa_schema);
            }
        } else if (key == "events") {
            parse_event_groups(json_value, state, deferred_events);
        } else if (!metadata.wrapped && key == "notes") {
            parse_sections(json_value, state, denpa_schema);
        }
    }
    append_events(state.chart, deferred_events);
}

[[nodiscard]] std::int64_t determine_key_count(
    const PsychMetadata& metadata,
    const bool /*denpa_schema*/
) {
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
    if (!absorb_lane_count(metadata.song_key_count)
        || !absorb_lane_count(metadata.song_mania)) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (metadata.saw_options_object && metadata.option_mania.has_value()) {
        const auto mania_index = *metadata.option_mania;
        if (mania_index < 0
            || mania_index >= maximum_supported_key_count) {
            return std::numeric_limits<std::int64_t>::max();
        }
        const auto option_key_count = mania_index + 1;
        declared = declared.has_value()
            ? std::max(*declared, option_key_count)
            : option_key_count;
    }
    if (!declared.has_value()) {
        declared = 4;
    }
    if (!metadata.saw_raw_lane) {
        return *declared;
    }
    const auto required = metadata.max_raw_lane / 2U + 1U;
    if (required > maximum_supported_key_count) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return std::max<std::int64_t>(
        *declared,
        static_cast<std::int64_t>(required)
    );
}

[[nodiscard]] Chart make_chart(
    const PsychMetadata& metadata,
    const bool denpa_schema,
    const std::string_view difficulty,
    const std::size_t raw_section_entries
) {
    Chart chart;
    chart.source_format = denpa_schema
        ? ChartFormat::denpa
        : ChartFormat::psych;
    chart.title = denpa_schema
        ? metadata.header_title.value_or(
              metadata.song_name.value_or("Untitled")
          )
        : metadata.song_title.value_or(
              metadata.song_name.value_or("Untitled")
          );
    chart.artist = denpa_schema
        ? metadata.header_artist.value_or(
              metadata.song_artist.value_or("Unknown")
          )
        : metadata.song_artist.value_or("Unknown");
    chart.charter = denpa_schema
        ? metadata.header_charter.value_or(
              metadata.song_charter.value_or("Unknown")
          )
        : metadata.song_charter.value_or("Unknown");
    chart.difficulty = std::string(difficulty);
    chart.stage_id = metadata.song_stage.value_or("");
    chart.player_character = metadata.song_player.value_or("");
    chart.opponent_character = metadata.song_opponent.value_or("");
    chart.secondary_opponent_character =
        metadata.song_secondary_opponent.value_or("");
    chart.secondary_opponent_enabled =
        metadata.song_secondary_opponent_enabled.value_or(false);
    chart.girlfriend_character = metadata.song_girlfriend.value_or("");
    chart.note_style = metadata.song_note_style.value_or("");

    const auto key_count = determine_key_count(metadata, denpa_schema);
    chart.key_count = key_count >= 1
            && key_count <= maximum_supported_key_count
        ? static_cast<std::uint16_t>(key_count)
        : std::numeric_limits<std::uint16_t>::max();
    chart.chart_scroll_speed = metadata.saw_options_object
        ? metadata.option_speed.value_or(
            metadata.song_speed.value_or(1.0)
        )
        : metadata.song_speed.value_or(1.0);

    const auto reserve_estimate = std::min(
        maximum_chart_notes,
        raw_section_entries
    );
    chart.notes.reserve(reserve_estimate);
    return chart;
}

[[nodiscard]] simdjson::padded_string read_bounded_chart_json(
    const std::filesystem::path& path
) {
    std::error_code size_error;
    const auto reported_size = std::filesystem::file_size(path, size_error);
    if (!size_error && reported_size > maximum_chart_json_bytes) {
        throw std::runtime_error(
            "chart JSON exceeds the 512 MiB safety limit"
        );
    }

    const auto initial_capacity = !size_error
        ? static_cast<std::size_t>(reported_size)
        : std::size_t{};
    simdjson::padded_string_builder builder(initial_capacity);
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("cannot open chart JSON");
    }

    std::array<char, file_read_chunk_bytes> buffer{};
    while (true) {
        input.read(
            buffer.data(),
            static_cast<std::streamsize>(buffer.size())
        );
        const auto bytes_read = input.gcount();
        if (bytes_read > 0) {
            const auto chunk_size = static_cast<std::size_t>(bytes_read);
            if (chunk_size > maximum_chart_json_bytes - builder.length()) {
                throw std::runtime_error(
                    "chart JSON exceeds the 512 MiB safety limit"
                );
            }
            if (!builder.append(buffer.data(), chunk_size)) {
                throw std::bad_alloc{};
            }
        }
        if (input.bad()) {
            throw std::runtime_error("cannot read chart JSON");
        }
        if (input.eof()) {
            break;
        }
        if (input.fail() || bytes_read == 0) {
            throw std::runtime_error("cannot read chart JSON");
        }
    }
    return builder.convert();
}

}  // namespace

FastPsychLoadResult load_fast_psych_chart(
    const std::filesystem::path& path,
    const std::string_view difficulty
) {
    FastPsychLoadResult result;
    PsychMetadata metadata;
    bool psych_candidate{};
    try {
        simdjson::padded_string json = read_bounded_chart_json(path);
        simdjson::ondemand::parser parser;
        simdjson::ondemand::document document;
        const auto iterate_error = parser.iterate(json).get(document);
        if (iterate_error == simdjson::MEMALLOC) {
            throw std::bad_alloc{};
        }
        if (iterate_error != simdjson::SUCCESS) {
            return result;
        }
        if (!scan_metadata(document, metadata, psych_candidate)) {
            return result;
        }
        if (!document.at_end()) {
            return {};
        }
        if (!metadata.wrapped && !metadata.saw_section_notes) {
            return result;
        }

        result.recognized = true;

        const bool denpa_schema = metadata.wrapped
            && metadata.saw_header_field
            && metadata.saw_options_field;
        if (denpa_schema
            && (!metadata.saw_header_object
                || !metadata.saw_options_object)) {
            result.error =
                "DenpaEx chart requires object-valued header and options";
            return result;
        }
        const double initial_bpm = denpa_schema
            ? metadata.header_bpm.value_or(120.0)
            : metadata.song_bpm.value_or(120.0);
        if (!std::isfinite(initial_bpm)
            || initial_bpm <= 0.0) {
            result.error = "Psych song BPM is outside the supported range";
            return result;
        }

        ParseState state;
        state.chart = make_chart(
            metadata,
            denpa_schema,
            difficulty,
            metadata.raw_section_entries
        );
        state.current_bpm = initial_bpm;
        state.chart.tempos.push_back({0.0, initial_bpm, 4, 4});

        document.rewind();
        parse_content(document, state, metadata, denpa_schema);
        if (!metadata.wrapped && !state.saw_section_notes) {
            return {};
        }

        result.recognized = true;
        result.denpa_schema = denpa_schema;
        result.discover_vocals = !denpa_schema
            || metadata.header_needs_voices;
        result.requested_song_id = denpa_schema
            ? metadata.header_title.value_or("")
            : metadata.song_title.value_or("");
        result.chart = std::move(state.chart);
        return result;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const FastPathFallback&) {
        return {};
    } catch (const simdjson::simdjson_error& exception) {
        if (exception.error() == simdjson::MEMALLOC) {
            throw std::bad_alloc{};
        }
        // The DOM parser deliberately accepts JSON comments and mirrors
        // nlohmann's BIGINT coercion. Rare syntax/conversion edge cases use
        // that compatibility path; ordinary JSON remains on the two-pass
        // on-demand loader.
        return {};
    } catch (const std::exception& exception) {
        result.recognized = psych_candidate
            || metadata.wrapped
            || metadata.saw_notes_array;
        result.error = exception.what();
        return result;
    }
}

}  // namespace pulseforge::detail
