#include "pulseforge/replay.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <type_traits>

namespace pulseforge {
namespace {

using Json = nlohmann::json;

constexpr std::uintmax_t maximum_replay_json_bytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t maximum_replay_inputs = 500'000;
constexpr std::size_t maximum_replay_text_bytes = 4'096;
constexpr double minimum_replay_time_ms = -60'000.0;
constexpr double maximum_replay_time_ms =
    12.0 * 60.0 * 60.0 * 1'000.0 + 10'000.0;

class Fingerprint {
public:
    void bytes(const void* data, const std::size_t size) noexcept {
        const auto* current = static_cast<const unsigned char*>(data);
        for (std::size_t index = 0; index < size; ++index) {
            value_ ^= current[index];
            value_ *= 1'099'511'628'211ULL;
        }
    }

    template <typename Type>
    void value(const Type item) noexcept
        requires(std::is_integral_v<Type>)
    {
        using Unsigned = std::make_unsigned_t<Type>;
        const auto unsigned_item = static_cast<Unsigned>(item);
        for (std::size_t byte_index = 0;
             byte_index < sizeof(Unsigned);
             ++byte_index) {
            const auto byte = static_cast<unsigned char>(
                unsigned_item >> (byte_index * 8U)
            );
            bytes(&byte, 1);
        }
    }

    void text(const std::string_view text) noexcept {
        const auto size = static_cast<std::uint64_t>(text.size());
        value(size);
        bytes(text.data(), text.size());
    }

    [[nodiscard]] std::string finish() const {
        std::ostringstream stream;
        stream << std::hex << std::setfill('0') << std::setw(16) << value_;
        return stream.str();
    }

private:
    std::uint64_t value_{1'469'598'103'934'665'603ULL};
};

[[nodiscard]] Json gameplay_to_json(const GameplaySettings& settings) {
    return {
        {"windows", {
            {"marvelous", settings.windows.marvelous_ms},
            {"sick", settings.windows.sick_ms},
            {"good", settings.windows.good_ms},
            {"bad", settings.windows.bad_ms},
            {"miss", settings.windows.miss_ms},
        }},
        {"inputOffsetMs", settings.input_offset_ms},
        {"visualOffsetMs", settings.visual_offset_ms},
        {"releaseGraceMs", settings.release_grace_ms},
        {"stackedNoteToleranceMs", settings.stacked_note_tolerance_ms},
        {"healthGain", settings.health_gain},
        {"healthLoss", settings.health_loss},
        {"ghostTapHealthLoss", settings.ghost_tap_health_loss},
        {"scrollSpeed", settings.scroll_speed},
        {"randomSeed", settings.random_seed},
        {"ghostTapping", settings.ghost_tapping},
        {"autoplay", settings.autoplay},
        {"practice", settings.practice},
        {"noFail", settings.no_fail},
        {"mirror", settings.mirror},
        {"randomizeLanes", settings.randomize_lanes},
        {"downscroll", settings.downscroll},
        {"middleScroll", settings.middle_scroll},
        {"hideOpponentNotes", settings.hide_opponent_notes},
    };
}

template <typename Type>
[[nodiscard]] Type json_value(
    const Json& object,
    const char* key,
    const Type fallback
) {
    const auto iterator = object.find(key);
    if (iterator == object.end()) {
        return fallback;
    }
    try {
        return iterator->get<Type>();
    } catch (...) {
        return fallback;
    }
}

[[nodiscard]] GameplaySettings gameplay_from_json(const Json& object) {
    GameplaySettings settings;
    if (const auto windows = object.find("windows");
        windows != object.end() && windows->is_object()) {
        settings.windows.marvelous_ms =
            json_value(*windows, "marvelous", settings.windows.marvelous_ms);
        settings.windows.sick_ms =
            json_value(*windows, "sick", settings.windows.sick_ms);
        settings.windows.good_ms =
            json_value(*windows, "good", settings.windows.good_ms);
        settings.windows.bad_ms =
            json_value(*windows, "bad", settings.windows.bad_ms);
        settings.windows.miss_ms =
            json_value(*windows, "miss", settings.windows.miss_ms);
    }
    settings.input_offset_ms =
        json_value(object, "inputOffsetMs", settings.input_offset_ms);
    settings.visual_offset_ms =
        json_value(object, "visualOffsetMs", settings.visual_offset_ms);
    settings.release_grace_ms =
        json_value(object, "releaseGraceMs", settings.release_grace_ms);
    settings.stacked_note_tolerance_ms = json_value(
        object,
        "stackedNoteToleranceMs",
        settings.stacked_note_tolerance_ms
    );
    settings.health_gain = json_value(object, "healthGain", settings.health_gain);
    settings.health_loss = json_value(object, "healthLoss", settings.health_loss);
    settings.ghost_tap_health_loss = json_value(
        object,
        "ghostTapHealthLoss",
        settings.ghost_tap_health_loss
    );
    settings.scroll_speed = json_value(object, "scrollSpeed", settings.scroll_speed);
    settings.random_seed = json_value(object, "randomSeed", settings.random_seed);
    settings.ghost_tapping =
        json_value(object, "ghostTapping", settings.ghost_tapping);
    settings.autoplay = json_value(object, "autoplay", settings.autoplay);
    settings.practice = json_value(object, "practice", settings.practice);
    settings.no_fail = json_value(object, "noFail", settings.no_fail);
    settings.mirror = json_value(object, "mirror", settings.mirror);
    settings.randomize_lanes =
        json_value(object, "randomizeLanes", settings.randomize_lanes);
    settings.downscroll = json_value(object, "downscroll", settings.downscroll);
    settings.middle_scroll =
        json_value(object, "middleScroll", settings.middle_scroll);
    settings.hide_opponent_notes =
        json_value(object, "hideOpponentNotes", settings.hide_opponent_notes);
    sanitize_gameplay_settings(settings);
    return settings;
}

[[nodiscard]] bool valid_gameplay_json_types(const Json& object) {
    const auto valid_optional = [&object](
        const char* const key,
        const auto& predicate
    ) {
        const auto iterator = object.find(key);
        return iterator == object.end() || predicate(*iterator);
    };
    if (!valid_optional("windows", [](const Json& value) {
            return value.is_object();
        })) {
        return false;
    }
    if (const auto windows = object.find("windows");
        windows != object.end()) {
        for (const auto* key : {"marvelous", "sick", "good", "bad", "miss"}) {
            const auto value = windows->find(key);
            if (value != windows->end() && !value->is_number()) {
                return false;
            }
        }
    }
    for (const auto* key : {
             "inputOffsetMs",
             "visualOffsetMs",
             "releaseGraceMs",
             "stackedNoteToleranceMs",
             "healthGain",
             "healthLoss",
             "ghostTapHealthLoss",
             "scrollSpeed",
         }) {
        if (!valid_optional(key, [](const Json& value) {
                return value.is_number();
            })) {
            return false;
        }
    }
    if (!valid_optional("randomSeed", [](const Json& value) {
            return value.is_number_unsigned();
        })) {
        return false;
    }
    for (const auto* key : {
             "ghostTapping",
             "autoplay",
             "practice",
             "noFail",
             "mirror",
             "randomizeLanes",
             "downscroll",
             "middleScroll",
             "hideOpponentNotes",
         }) {
        if (!valid_optional(key, [](const Json& value) {
                return value.is_boolean();
            })) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::string chart_fingerprint(const Chart& chart) {
    Fingerprint fingerprint;
    fingerprint.text(chart.title);
    fingerprint.text(chart.difficulty);
    fingerprint.text(chart.stage_id);
    fingerprint.text(chart.player_character);
    fingerprint.text(chart.opponent_character);
    fingerprint.text(chart.girlfriend_character);
    fingerprint.text(chart.note_style);
    fingerprint.value(chart.key_count);
    fingerprint.value(std::bit_cast<std::uint64_t>(chart.chart_scroll_speed));
    fingerprint.value(static_cast<std::uint64_t>(chart.tempos.size()));
    for (const auto& tempo : chart.tempos) {
        fingerprint.value(std::bit_cast<std::uint64_t>(tempo.time_ms));
        fingerprint.value(std::bit_cast<std::uint64_t>(tempo.bpm));
        fingerprint.value(tempo.numerator);
        fingerprint.value(tempo.denominator);
    }
    fingerprint.value(static_cast<std::uint64_t>(chart.notes.size()));
    for (const auto& note : chart.notes) {
        fingerprint.value(std::bit_cast<std::uint64_t>(note.time_ms));
        fingerprint.value(std::bit_cast<std::uint64_t>(note.duration_ms));
        fingerprint.value(note.lane);
        fingerprint.value(static_cast<std::uint8_t>(note.owner));
        fingerprint.text(note.kind);
        fingerprint.text(
            note.payload_id < chart.note_payloads.size()
                ? std::string_view(chart.note_payloads[note.payload_id])
                : std::string_view{}
        );
    }
    fingerprint.value(static_cast<std::uint64_t>(chart.events.size()));
    for (const auto& event : chart.events) {
        fingerprint.value(std::bit_cast<std::uint64_t>(event.time_ms));
        fingerprint.text(event.name);
        fingerprint.text(event.value1);
        fingerprint.text(event.value2);
        fingerprint.text(event.payload_json);
    }
    return fingerprint.finish();
}

Replay make_replay(
    const Chart& chart,
    const GameplaySession& session,
    std::string engine_version
) {
    Replay replay;
    replay.engine_version = std::move(engine_version);
    replay.chart_hash = chart_fingerprint(chart);
    replay.difficulty = chart.difficulty;
    replay.random_seed = session.settings().random_seed;
    replay.settings = session.settings();
    replay.inputs.assign(
        session.recorded_inputs().begin(),
        session.recorded_inputs().end()
    );
    return replay;
}

bool save_replay(
    const std::filesystem::path& path,
    const Replay& replay,
    std::string* error
) {
    try {
        if (replay.inputs.size() > maximum_replay_inputs
            || replay.engine_version.size() > maximum_replay_text_bytes
            || replay.chart_hash.size() > maximum_replay_text_bytes
            || replay.difficulty.size() > maximum_replay_text_bytes) {
            if (error != nullptr) {
                *error = "replay exceeds its input or text safety limit";
            }
            return false;
        }
        Json inputs = Json::array();
        for (const auto& input : replay.inputs) {
            if (!std::isfinite(input.time_ms)
                || input.time_ms < minimum_replay_time_ms
                || input.time_ms > maximum_replay_time_ms) {
                if (error != nullptr) {
                    *error = "replay contains an invalid input timestamp";
                }
                return false;
            }
            inputs.push_back({
                {"timeMs", input.time_ms},
                {"lane", input.lane},
                {"pressed", input.pressed},
            });
        }
        auto safe_settings = replay.settings;
        sanitize_gameplay_settings(safe_settings);
        const Json root{
            {"format", "pulseforge-replay"},
            {"formatVersion", replay.format_version},
            {"engineVersion", replay.engine_version},
            {"chartHash", replay.chart_hash},
            {"difficulty", replay.difficulty},
            {"randomSeed", replay.random_seed},
            {"settings", gameplay_to_json(safe_settings)},
            {"inputs", std::move(inputs)},
        };

        const auto serialized = root.dump();
        if (serialized.size() >= maximum_replay_json_bytes) {
            if (error != nullptr) {
                *error = "serialized replay exceeds the 64 MiB safety limit";
            }
            return false;
        }
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            if (error != nullptr) {
                *error = "cannot create replay: " + path.string();
            }
            return false;
        }
        output << serialized << '\n';
        if (!output) {
            if (error != nullptr) {
                *error = "failed while writing replay: " + path.string();
            }
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = exception.what();
        }
        return false;
    }
}

ReplayLoadResult load_replay(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return {std::nullopt, "cannot open replay: " + path.string()};
    }
    const auto end = input.tellg();
    if (end < std::streampos{0}
        || static_cast<std::uintmax_t>(end) > maximum_replay_json_bytes) {
        return {std::nullopt, "replay JSON exceeds the 64 MiB safety limit"};
    }
    std::string json_text(static_cast<std::size_t>(end), '\0');
    input.seekg(0, std::ios::beg);
    if (!json_text.empty()) {
        input.read(json_text.data(), static_cast<std::streamsize>(json_text.size()));
    }
    if (!input) {
        return {std::nullopt, "failed while reading replay"};
    }

    try {
        const auto root = Json::parse(json_text, nullptr, true, true);
        if (!root.is_object()
            || json_value<std::string>(root, "format", {}) != "pulseforge-replay") {
            return {std::nullopt, "not a PulseForge replay"};
        }
        Replay replay;
        const auto version = root.find("formatVersion");
        if (version == root.end()
            || !version->is_number_unsigned()
            || version->get<std::uint64_t>()
                > std::numeric_limits<std::uint32_t>::max()) {
            return {std::nullopt, "invalid replay format version"};
        }
        replay.format_version =
            static_cast<std::uint32_t>(version->get<std::uint64_t>());
        if (replay.format_version != 1) {
            return {
                std::nullopt,
                "unsupported replay format version: "
                    + std::to_string(replay.format_version),
            };
        }
        for (const auto* key : {"engineVersion", "chartHash", "difficulty"}) {
            const auto value = root.find(key);
            if (value != root.end() && !value->is_string()) {
                return {std::nullopt, "replay metadata fields must be strings"};
            }
        }
        replay.engine_version = json_value<std::string>(root, "engineVersion", {});
        replay.chart_hash = json_value<std::string>(root, "chartHash", {});
        replay.difficulty = json_value<std::string>(root, "difficulty", "normal");
        if (replay.engine_version.size() > maximum_replay_text_bytes
            || replay.chart_hash.size() > maximum_replay_text_bytes
            || replay.difficulty.size() > maximum_replay_text_bytes) {
            return {std::nullopt, "replay metadata string exceeds its safety limit"};
        }
        if (const auto random_seed = root.find("randomSeed");
            random_seed != root.end()) {
            if (!random_seed->is_number_unsigned()) {
                return {std::nullopt, "replay randomSeed must be unsigned"};
            }
            replay.random_seed = random_seed->get<std::uint64_t>();
        }
        if (const auto settings = root.find("settings");
            settings != root.end()) {
            if (!settings->is_object() || !valid_gameplay_json_types(*settings)) {
                return {std::nullopt, "replay settings contain an invalid type"};
            }
            replay.settings = gameplay_from_json(*settings);
        }
        replay.settings.random_seed = replay.random_seed;

        if (const auto inputs = root.find("inputs");
            inputs != root.end() && inputs->is_array()) {
            if (inputs->size() > maximum_replay_inputs) {
                return {std::nullopt, "replay contains too many input events"};
            }
            replay.inputs.reserve(inputs->size());
            for (const auto& item : *inputs) {
                if (!item.is_object()) {
                    return {std::nullopt, "replay input must be an object"};
                }
                const auto time = item.find("timeMs");
                const auto lane = item.find("lane");
                const auto pressed = item.find("pressed");
                if (time == item.end()
                    || !time->is_number()
                    || !std::isfinite(time->get<double>())
                    || time->get<double>() < minimum_replay_time_ms
                    || time->get<double>() > maximum_replay_time_ms) {
                    return {
                        std::nullopt,
                        "replay input time is outside the supported range",
                    };
                }
                if (lane == item.end()
                    || !lane->is_number_unsigned()
                    || lane->get<std::uint64_t>()
                        > std::numeric_limits<std::uint16_t>::max()) {
                    return {std::nullopt, "replay input lane is invalid"};
                }
                if (pressed == item.end() || !pressed->is_boolean()) {
                    return {std::nullopt, "replay input pressed must be boolean"};
                }
                replay.inputs.push_back({
                    time->get<double>(),
                    static_cast<std::uint16_t>(lane->get<std::uint64_t>()),
                    pressed->get<bool>(),
                });
            }
        } else {
            return {std::nullopt, "replay inputs must be an array"};
        }
        std::stable_sort(
            replay.inputs.begin(),
            replay.inputs.end(),
            [](const auto& left, const auto& right) {
                return left.time_ms < right.time_ms;
            }
        );
        return {std::move(replay), {}};
    } catch (const std::exception& exception) {
        return {
            std::nullopt,
            std::string("invalid replay JSON: ") + exception.what(),
        };
    }
}

ScoreSummary simulate_replay(
    const Chart& chart,
    const Replay& replay,
    const double simulation_step_ms
) {
    auto settings = replay.settings;
    settings.random_seed = replay.random_seed;
    sanitize_gameplay_settings(settings);
    GameplaySession session(chart, settings);
    std::vector<InputRecord> inputs;
    const auto bounded_input_count = std::min(
        replay.inputs.size(),
        maximum_replay_inputs
    );
    inputs.reserve(bounded_input_count);
    for (std::size_t index = 0; index < bounded_input_count; ++index) {
        const auto& input = replay.inputs[index];
        if (std::isfinite(input.time_ms)
            && input.time_ms >= minimum_replay_time_ms
            && input.time_ms <= maximum_replay_time_ms) {
            inputs.push_back(input);
        }
    }
    std::stable_sort(
        inputs.begin(),
        inputs.end(),
        [](const auto& left, const auto& right) {
            return left.time_ms < right.time_ms;
        }
    );

    double current_time = std::min(0.0, chart.notes.empty() ? 0.0 : chart.notes.front().time_ms);
    std::size_t input_index = 0;
    const double step = std::isfinite(simulation_step_ms)
        ? std::clamp(simulation_step_ms, 0.05, 100.0)
        : 1.0;
    constexpr std::size_t maximum_simulation_steps = 100'000;
    std::size_t simulation_steps = 0;

    while (input_index < inputs.size()
           && !session.complete()
           && !session.summary().failed) {
        const double event_time = inputs[input_index].time_ms;
        while (current_time + step < event_time
               && !session.complete()
               && !session.summary().failed
               && simulation_steps < maximum_simulation_steps) {
            current_time += step;
            session.update(current_time);
            ++simulation_steps;
        }
        if (session.complete() || session.summary().failed) {
            break;
        }
        current_time = event_time;
        while (input_index < inputs.size()
               && inputs[input_index].time_ms == event_time) {
            const auto& event = inputs[input_index];
            if (event.pressed) {
                session.press(event.lane, event.time_ms);
            } else {
                session.release(event.lane, event.time_ms);
            }
            ++input_index;
        }
        // Match the desktop loop: timestamped inputs are dispatched before
        // the gameplay scheduler advances to that frame time. press/release
        // perform their own bounded catch-up for misses and sustain ticks.
        if (!session.summary().failed) {
            session.update(current_time);
        }
    }

    const double end_time = chart.duration_ms() + settings.windows.miss_ms;
    while (current_time < end_time
           && !session.complete()
           && !session.summary().failed
           && simulation_steps < maximum_simulation_steps) {
        current_time = std::min(current_time + step, end_time);
        session.update(current_time);
        ++simulation_steps;
    }
    if (!session.complete() && !session.summary().failed) {
        session.finish_song(end_time);
    }
    return session.summary();
}

}  // namespace pulseforge
