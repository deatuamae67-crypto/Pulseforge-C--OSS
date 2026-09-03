#include "pulseforge/chart_loader.hpp"
#include "pulseforge/gameplay.hpp"
#include "pulseforge/note_render_lod.hpp"
#include "pulseforge/replay.hpp"
#include "pulseforge/settings.hpp"
#include "pulseforge/timing_map.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using pulseforge::Chart;
using pulseforge::GameplaySession;
using pulseforge::Note;
using pulseforge::NoteOwner;
using pulseforge::TempoChange;

void require(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void require_near(
    const double actual,
    const double expected,
    const double tolerance,
    const std::string& message
) {
    if (!std::isfinite(actual)
        || !std::isfinite(expected)
        || !std::isfinite(tolerance)
        || tolerance < 0.0
        || std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(
            message + ": expected " + std::to_string(expected)
            + ", got " + std::to_string(actual)
        );
    }
}

[[nodiscard]] Chart basic_chart() {
    Chart chart;
    chart.title = "Core test";
    chart.tempos = {{0.0, 120.0, 4, 4}};
    chart.notes = {
        {1'000.0, 0.0, 0, NoteOwner::player, "normal"},
        {1'500.0, 500.0, 1, NoteOwner::player, "normal"},
        {1'750.0, 0.0, 2, NoteOwner::opponent, "normal"},
    };
    chart.normalize();
    return chart;
}

void test_timing_map_round_trip() {
    pulseforge::TimingMap timing({
        {0.0, 120.0, 4, 4},
        {2'000.0, 240.0, 3, 4},
        {3'000.0, 90.0, 7, 8},
    });
    require_near(timing.beat_at(2'000.0), 4.0, 0.000001, "beat at BPM change");
    require_near(timing.beat_at(3'000.0), 8.0, 0.000001, "beat at second change");
    for (double time = -500.0; time < 8'000.0; time += 17.25) {
        require_near(
            timing.time_at_beat(timing.beat_at(time)),
            time,
            0.000001,
            "time/beat round trip"
        );
    }
}

void test_native_loader() {
    constexpr auto json = R"json(
    {
      "format": "pulseforge-chart",
      "song": {
        "title": "Native",
        "artist": "Tests",
        "keyCount": 4,
        "scrollSpeed": 1.25
      },
      "tempos": [
        {"time": 0, "bpm": 120},
        {"time": 2000, "bpm": 180, "numerator": 3, "denominator": 4}
      ],
      "notes": [
        {"time": 1000, "lane": 0, "owner": "player"},
        {"time": 500, "lane": 2, "owner": "opponent", "duration": 125}
      ],
      "events": [
        {"time": 750, "name": "Camera Flash", "value1": "white"}
      ]
    })json";
    const auto result = pulseforge::ChartLoader::parse(json);
    require(static_cast<bool>(result), result.error);
    require(result.chart->notes.size() == 2, "native notes parsed");
    require(result.chart->notes.front().time_ms == 500.0, "native notes normalized");
    require(result.chart->tempos.size() == 2, "native tempos parsed");
    require(result.chart->events.size() == 1, "native event parsed");

    constexpr auto stale_key_count = R"json(
    {
      "format":"pulseforge-chart",
      "song":{"title":"Native 6K","keyCount":4},
      "tempos":[{"time":0,"bpm":120}],
      "notes":[{"time":100,"lane":5,"owner":"player"}]
    })json";
    const auto widened = pulseforge::ChartLoader::parse(stale_key_count);
    require(static_cast<bool>(widened), widened.error);
    require(
        widened.chart->key_count == 6U
            && widened.chart->notes.front().lane == 5U,
        "native largest lane widens stale key-count metadata"
    );

    constexpr auto unsupported_lane = R"json(
    {
      "format":"pulseforge-chart",
      "song":{"title":"Native 19K","keyCount":4},
      "tempos":[{"time":0,"bpm":120}],
      "notes":[{"time":100,"lane":18,"owner":"player"}]
    })json";
    require(
        !pulseforge::ChartLoader::parse(unsupported_lane),
        "native lanes above the 18K runtime maximum are rejected"
    );
}

void test_psych_loader() {
    constexpr auto json = R"json(
    {
      "song": {
        "song": "Psych Test",
        "bpm": 120,
        "speed": 1.4,
        "notes": [
          {
            "mustHitSection": false,
            "sectionNotes": [
              [100, 0, 0, ""],
              [200, 4, 250, "Alt Animation"]
            ]
          },
          {
            "mustHitSection": true,
            "changeBPM": true,
            "bpm": 150,
            "sectionNotes": [[2100, 2, 0, "Hurt Note"]]
          }
        ],
        "events": [
          [500, [["Set GF Speed", "2", ""]]]
        ]
      }
    })json";
    const auto result = pulseforge::ChartLoader::parse(json);
    require(static_cast<bool>(result), result.error);
    require(result.chart->source_format == pulseforge::ChartFormat::psych, "Psych detected");
    require(result.chart->notes.size() == 3, "Psych notes parsed");
    require(
        result.chart->notes[0].owner == NoteOwner::opponent,
        "Psych section-side opponent mapping"
    );
    require(
        result.chart->notes[1].owner == NoteOwner::player,
        "Psych flipped-side player mapping"
    );
    require(result.chart->tempos.size() == 2, "Psych BPM changes parsed");
    require(result.chart->events.size() == 1, "Psych events parsed");

    // PULSEFORGE_P1_4_0D_THIRD_STRUM_IMPORT_PARITY_TEST_V1
    constexpr auto generic_third_strum = R"json(
    {"song":{"song":"Psych Third Strum","bpm":120,"keyCount":4,"notes":[
      {"mustHitSection":true,"sectionNotes":[[100,4,250,"Third Strum"]]}
    ]}})json";
    const auto generic_third = pulseforge::ChartLoader::parse(generic_third_strum);
    require(static_cast<bool>(generic_third), generic_third.error);
    require(
        generic_third.chart->notes.size() == 1U
            && generic_third.chart->notes.front().owner
                == NoteOwner::secondary_opponent
            && generic_third.chart->notes.front().kind == "Third Strum",
        "Psych Third Strum uses the same canonical secondary owner as DenpaEx"
    );

    constexpr auto negative_legacy_sustain = R"json(
    {"song":{"song":"Legacy taps","bpm":120,"notes":[
      {"mustHitSection":true,"sectionNotes":[
        [1000,0,-1000,""],[1100,1,25,""]
      ]}
    ]}})json";
    const auto repaired = pulseforge::ChartLoader::parse(
        negative_legacy_sustain
    );
    require(static_cast<bool>(repaired), repaired.error);
    require(
        repaired.chart->notes.size() == 2U
            && repaired.chart->notes[0U].duration_ms == 0.0
            && repaired.chart->notes[0U].end_time_ms() == 1'000.0
            && repaired.chart->notes[1U].duration_ms == 25.0,
        "permissive Psych import treats a negative legacy sustain as a tap"
    );
    pulseforge::ChartLoadOptions strict_legacy_sustain;
    strict_legacy_sustain.strict = true;
    const auto strict_negative = pulseforge::ChartLoader::parse(
        negative_legacy_sustain,
        {},
        strict_legacy_sustain
    );
    require(
        !strict_negative
            && strict_negative.error.find("non-negative sustain length")
                != std::string::npos,
        "strict Psych import rejects a negative sustain with a focused diagnostic"
    );

    constexpr auto inferred_six_key = R"json(
    {"song":{"song":"Inferred 6K","bpm":120,"notes":[
      {"mustHitSection":true,"sectionNotes":[[100,11,0,""]]}
    ]}})json";
    const auto six_key = pulseforge::ChartLoader::parse(inferred_six_key);
    require(static_cast<bool>(six_key), six_key.error);
    require(
        six_key.chart->key_count == 6U
            && six_key.chart->notes.front().lane == 5U,
        "Psych combined lane domain infers 6K without metadata"
    );

    constexpr auto stale_four_key = R"json(
    {"song":{"song":"Inferred 9K","bpm":120,"keyCount":4,"notes":[
      {"mustHitSection":true,"sectionNotes":[[100,17,0,""]]}
    ]}})json";
    const auto nine_key = pulseforge::ChartLoader::parse(stale_four_key);
    require(static_cast<bool>(nine_key), nine_key.error);
    require(
        nine_key.chart->key_count == 9U
            && nine_key.chart->notes.front().lane == 8U,
        "Psych largest lane widens stale 4K metadata to 9K"
    );

    constexpr auto unsupported_key_mode = R"json(
    {"song":{"song":"Unsupported 19K","bpm":120,"notes":[
      {"mustHitSection":true,"sectionNotes":[[100,36,0,""]]}
    ]}})json";
    require(
        !pulseforge::ChartLoader::parse(unsupported_key_mode),
        "Psych lanes above the two-strumline 18K domain are rejected"
    );

    constexpr auto js_options_sparse = R"json(
    {"song":{"song":"JS Options 6K","bpm":120,
      "options":{"mania":5,"speed":1.25},"notes":[
        {"mustHitSection":true,"sectionNotes":[[100,0,0,""]]}
      ]}})json";
    const auto options_six_key = pulseforge::ChartLoader::parse(
        js_options_sparse
    );
    require(static_cast<bool>(options_six_key), options_six_key.error);
    require(
        options_six_key.chart->key_count == 6U
            && options_six_key.chart->chart_scroll_speed == 1.25,
        "Psych song.options.mania identifies sparse JS-engine 6K charts"
    );

    constexpr auto stronger_song_metadata = R"json(
    {"song":{"song":"Strong 9K","bpm":120,"keyCount":9,
      "options":{"mania":5},"notes":[]}})json";
    const auto stronger = pulseforge::ChartLoader::parse(
        stronger_song_metadata
    );
    require(static_cast<bool>(stronger), stronger.error);
    require(
        stronger.chart->key_count == 9U,
        "explicit song key metadata remains the stronger key-mode floor"
    );
}

void test_psych_event_sidecars() {
    const auto unique = std::to_string(
        std::chrono::high_resolution_clock::now().time_since_epoch().count()
    );
    const auto temporary_root = std::filesystem::temp_directory_path()
        / ("pulseforge-psych-sidecars-" + unique);
    const auto chart_directory = temporary_root / "song";
    std::filesystem::create_directories(chart_directory);

    const auto write_fixture = [](
        const std::filesystem::path& path,
        const std::string_view contents,
        const std::string_view label
    ) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << contents;
        require(static_cast<bool>(output), std::string(label));
    };
    constexpr auto chart_json = R"json({
      "song":{
        "song":"Sidecar Test",
        "bpm":120,
        "notes":[{
          "mustHitSection":true,
          "sectionNotes":[[100,0,0,""]]
        }]
      }
    })json";
    const auto chart_path = chart_directory / "sidecar-test-hard.json";
    write_fixture(chart_path, chart_json, "write Psych sidecar chart fixture");

    const auto legacy_path = chart_directory / "event.json";
    write_fixture(
        legacy_path,
        R"json({"song":{"notes":[{"sectionNotes":[
          [200,-1,"Legacy Event","left","right"],
          [225,0,0,""]
        ]}]}})json",
        "write legacy event.json fixture"
    );
    const auto modern_path = chart_directory / "events.json";
    write_fixture(
        modern_path,
        R"json({"events":[
          [300,[["Modern Event","one","two"]]]
        ]})json",
        "write modern events.json fixture"
    );

    const auto preferred = pulseforge::ChartLoader::load(chart_path);
    require(static_cast<bool>(preferred), preferred.error);
    require(
        preferred.chart->events.size() == 1U
            && preferred.chart->events[0].name == "Modern Event",
        "events.json takes precedence over the legacy event.json sidecar"
    );

    std::error_code remove_error;
    std::filesystem::remove(modern_path, remove_error);
    require(!remove_error, "remove preferred events.json fixture");
    const auto fallback = pulseforge::ChartLoader::load(chart_path);
    require(static_cast<bool>(fallback), fallback.error);
    require(
        fallback.chart->events.size() == 1U
            && fallback.chart->events[0].time_ms == 200.0
            && fallback.chart->events[0].name == "Legacy Event"
            && fallback.chart->events[0].value1 == "left"
            && fallback.chart->events[0].value2 == "right",
        "event.json fallback imports old negative-lane sectionNotes"
    );
    require(
        fallback.chart->notes.size() == 1U,
        "ordinary notes inside a legacy event sidecar are ignored"
    );

    pulseforge::ChartLoadOptions strict;
    strict.strict = true;
    const auto strict_fallback = pulseforge::ChartLoader::load(
        chart_path,
        strict
    );
    require(
        static_cast<bool>(strict_fallback),
        strict_fallback.error
    );
    require(
        strict_fallback.chart->events.size() == 1U,
        "strict mode accepts a well-formed legacy Psych event sidecar"
    );

    write_fixture(
        legacy_path,
        R"json({"song":{"events":null,"notes":[{"sectionNotes":[
          [375,-1,"Legacy Null Events","left","right"]
        ]}]}})json",
        "write legacy sidecar with a null modern events field"
    );
    const auto null_events = pulseforge::ChartLoader::load(chart_path);
    require(static_cast<bool>(null_events), null_events.error);
    require(
        null_events.chart->events.size() == 1U
            && null_events.chart->events[0].time_ms == 375.0
            && null_events.chart->events[0].name == "Legacy Null Events",
        "permissive mode ignores events null after importing legacy sectionNotes"
    );
    const auto strict_null_events = pulseforge::ChartLoader::load(
        chart_path,
        strict
    );
    require(
        !strict_null_events
            && strict_null_events.error.find("strict import rejected")
                != std::string::npos,
        "strict mode keeps rejecting a null Psych events field"
    );

    write_fixture(
        legacy_path,
        R"json({"song":{"notes":[{"sectionNotes":[
          [400,-1,{"structured":"name"},"value",null]
        ]}]}})json",
        "write permissive legacy event fixture"
    );
    const auto permissive = pulseforge::ChartLoader::load(chart_path);
    require(static_cast<bool>(permissive), permissive.error);
    require(
        permissive.chart->events.size() == 1U
            && permissive.chart->events[0].name
                == R"json({"structured":"name"})json",
        "permissive legacy events retain Psych scalar coercion"
    );
    const auto rejected = pulseforge::ChartLoader::load(chart_path, strict);
    require(
        !rejected
            && rejected.error.find("strict import rejected")
                != std::string::npos,
        "strict mode rejects a legacy event name that permissive mode coerces"
    );

    const auto self_modern_directory = temporary_root / "self-modern";
    std::filesystem::create_directories(self_modern_directory);
    const auto self_modern_path = self_modern_directory / "events.json";
    write_fixture(
        self_modern_path,
        R"json({"song":{"song":"Self Modern","bpm":120,"notes":[],
          "events":[[500,[["Once","",""]]]]}})json",
        "write self-named modern chart fixture"
    );
    const auto self_modern = pulseforge::ChartLoader::load(self_modern_path);
    require(static_cast<bool>(self_modern), self_modern.error);
    require(
        self_modern.chart->events.size() == 1U,
        "a chart named events.json is not loaded again as its own sidecar"
    );

    const auto self_legacy_directory = temporary_root / "self-legacy";
    std::filesystem::create_directories(self_legacy_directory);
    const auto self_legacy_path = self_legacy_directory / "event.json";
    write_fixture(
        self_legacy_path,
        R"json({"song":{"song":"Self Legacy","bpm":120,"notes":[{
          "mustHitSection":false,
          "sectionNotes":[[600,-1,"Once Legacy","",""]]
        }]}})json",
        "write self-named legacy chart fixture"
    );
    const auto self_legacy = pulseforge::ChartLoader::load(self_legacy_path);
    require(static_cast<bool>(self_legacy), self_legacy.error);
    require(
        self_legacy.chart->events.size() == 1U,
        "a chart named event.json is not loaded again as its own sidecar"
    );

    const auto case_probe_directory = temporary_root / "case-probe";
    std::filesystem::create_directories(case_probe_directory);
    const auto mixed_case_chart_path = case_probe_directory / "Events.json";
    const auto lower_case_sidecar_path = case_probe_directory / "events.json";
    write_fixture(
        mixed_case_chart_path,
        R"json({"song":{"song":"Case-distinct chart","bpm":120,"notes":[]}})json",
        "write mixed-case Psych chart fixture"
    );
    write_fixture(
        lower_case_sidecar_path,
        R"json({"events":[[700,[["Case-distinct sidecar","",""]]]]})json",
        "write lower-case Psych event sidecar fixture"
    );
    std::error_code equivalent_error;
    const bool aliases_same_file = std::filesystem::equivalent(
        mixed_case_chart_path,
        lower_case_sidecar_path,
        equivalent_error
    );
    require(!equivalent_error, "probe case-sensitive filesystem semantics");
    if (!aliases_same_file) {
        const auto case_distinct = pulseforge::ChartLoader::load(
            mixed_case_chart_path
        );
        require(static_cast<bool>(case_distinct), case_distinct.error);
        require(
            case_distinct.chart->events.size() == 1U
                && case_distinct.chart->events[0].name
                    == "Case-distinct sidecar",
            "case-sensitive filesystems keep Events.json and events.json distinct"
        );
    }

    std::filesystem::remove_all(temporary_root, remove_error);
    require(!remove_error, "remove Psych sidecar fixtures");
}

void test_denpa_loader() {
    constexpr auto json = R"json(
    {
      "song": {
        "header": {
          "song": "Denpa Test",
          "bpm": 150,
          "needsVoices": true
        },
        "assets": {
          "player1": "bf",
          "player2": "dad",
          "player4": "monster",
          "enablePlayer4": true,
          "gfVersion": "gf"
        },
        "options": {
          "speed": 1.75,
          "mania": 5
        },
        "notes": [
          {
            "lengthInSteps": 16,
            "mustHitSection": false,
            "sectionNotes": [
              [100, 0, 0, ""],
              [200, 6, 250, "Hurt Note"],
              [250, 7, 0, "Third Strum"]
            ]
          },
          {
            "lengthInSteps": 16,
            "mustHitSection": true,
            "changeBPM": true,
            "bpm": 180,
            "sectionNotes": [[1800, 5, 0, true]]
          }
        ],
        "events": [
          [500, [["Change Mania", "3", "0"]]]
        ]
      }
    })json";

    pulseforge::ChartLoadOptions options;
    options.strict = true;
    const auto result = pulseforge::ChartLoader::parse(json, {}, options);
    require(static_cast<bool>(result), result.error);
    require(
        result.chart->source_format == pulseforge::ChartFormat::denpa,
        "DenpaEx detected"
    );
    require(result.chart->title == "Denpa Test", "DenpaEx title parsed");
    require(result.chart->key_count == 6, "DenpaEx mania is zero-based");
    require_near(
        result.chart->chart_scroll_speed,
        1.75,
        0.000001,
        "DenpaEx speed parsed"
    );
    require(result.chart->notes.size() == 4, "DenpaEx notes parsed");
    require(
        result.chart->notes[1].owner == NoteOwner::player,
        "DenpaEx opposite strumline mapped"
    );
    require(
        result.chart->notes[2].owner == NoteOwner::secondary_opponent,
        "DenpaEx Third Strum preserves a secondary-opponent owner"
    );
    require(
        result.chart->secondary_opponent_enabled
            && result.chart->secondary_opponent_character == "monster",
        "DenpaEx player4 metadata is preserved"
    );
    require(result.chart->tempos.size() == 2, "DenpaEx BPM changes parsed");
    require(result.chart->events.size() == 1, "DenpaEx events parsed");

    constexpr auto missing_bpm = R"json(
    {"song":{"header":{"song":"Bad","needsVoices":false},"assets":{},
      "options":{"speed":1,"mania":3},"notes":[]}})json";
    const auto rejected_bpm = pulseforge::ChartLoader::parse(
        missing_bpm,
        {},
        options
    );
    require(!rejected_bpm, "strict DenpaEx requires header BPM");

    constexpr auto malformed_mania = R"json(
    {"song":{"header":{"song":"Bad","bpm":120,"needsVoices":false},"assets":{},
      "options":{"speed":1,"mania":"3"},"notes":[]}})json";
    const auto rejected_mania = pulseforge::ChartLoader::parse(
        malformed_mania,
        {},
        options
    );
    require(!rejected_mania, "strict DenpaEx rejects coerced mania");

    for (const auto [mania, keys] : {
             std::pair{0, 1},
             std::pair{3, 4},
             std::pair{8, 9},
             std::pair{17, 18},
         }) {
        const auto mania_json = std::string{
            "{\"song\":{\"header\":{\"song\":\"Mania\",\"bpm\":120,"
            "\"needsVoices\":false},"
            "\"assets\":{},\"options\":{\"speed\":1,\"mania\":"
        } + std::to_string(mania) + "},\"notes\":[]}}";
        const auto mania_result = pulseforge::ChartLoader::parse(
            mania_json,
            {},
            options
        );
        require(static_cast<bool>(mania_result), mania_result.error);
        require(
            mania_result.chart->key_count == keys,
            "DenpaEx 1K/4K/9K mania mapping"
        );
    }

    constexpr auto null_mania = R"json(
    {"song":{"header":{"song":"Default Mania","bpm":120,
      "needsVoices":false},"assets":{},
      "options":{"speed":1,"mania":null},"notes":[{
        "lengthInSteps":null,"mustHitSection":false,"sectionNotes":[]
      },{
        "lengthInSteps":16,"mustHitSection":false,"changeBPM":true,
        "bpm":150,"sectionNotes":[]
      }]}})json";
    const auto null_mania_result = pulseforge::ChartLoader::parse(
        null_mania,
        {},
        options
    );
    require(static_cast<bool>(null_mania_result), null_mania_result.error);
    require(
        null_mania_result.chart->key_count == 4,
        "DenpaEx null mania uses the 4K default"
    );
    require(
        null_mania_result.chart->tempos.size() == 2,
        "DenpaEx null section length uses the parser fallback"
    );
    require_near(
        null_mania_result.chart->tempos[1].time_ms,
        2'000.0,
        0.000001,
        "DenpaEx null section length defaults to 16 steps"
    );

    constexpr auto ten_key_mania = R"json(
    {"song":{"header":{"song":"10K","bpm":120,"needsVoices":false},"assets":{},
      "options":{"speed":1,"mania":9},"notes":[]}})json";
    const auto accepted_ten_key_mania = pulseforge::ChartLoader::parse(
        ten_key_mania,
        {},
        options
    );
    require(
        static_cast<bool>(accepted_ten_key_mania),
        accepted_ten_key_mania.error
    );
    require(
        accepted_ten_key_mania.chart->key_count == 10U,
        "DenpaEx supports every runtime mania mode through 18K"
    );

    constexpr auto excessive_mania = R"json(
    {"song":{"header":{"song":"Bad","bpm":120,"needsVoices":false},"assets":{},
      "options":{"speed":1,"mania":18},"notes":[]}})json";
    require(
        !pulseforge::ChartLoader::parse(excessive_mania, {}, options),
        "DenpaEx rejects only mania modes above the supported 18K maximum"
    );

    constexpr auto missing_needs_voices = R"json(
    {"song":{"header":{"song":"Bad","bpm":120},"assets":{},
      "options":{"speed":1,"mania":3},"notes":[]}})json";
    const auto rejected_needs_voices = pulseforge::ChartLoader::parse(
        missing_needs_voices,
        {},
        options
    );
    require(
        !rejected_needs_voices,
        "strict DenpaEx requires header needsVoices"
    );

    const auto unique = std::to_string(
        std::chrono::high_resolution_clock::now().time_since_epoch().count()
    );
    const auto temporary_root =
        std::filesystem::temp_directory_path() / ("pulseforge-denpa-" + unique);
    const auto chart_directory = temporary_root
        / "mods" / "data" / "charts" / "denpa-test";
    const auto audio_directory = temporary_root
        / "mods" / "songs" / "denpa-test";
    std::filesystem::create_directories(chart_directory);
    std::filesystem::create_directories(audio_directory);
    const auto chart_path = chart_directory / "denpa-test.json";
    {
        std::ofstream output(chart_path, std::ios::binary | std::ios::trunc);
        output << json;
        require(static_cast<bool>(output), "write DenpaEx chart fixture");
    }
    {
        std::ofstream output(
            chart_directory / "events.json",
            std::ios::binary | std::ios::trunc
        );
        output << R"json({"song":{"events":[
          [750, [["Set GF Speed", "2", ""]]]
        ]}})json";
        require(static_cast<bool>(output), "write DenpaEx events fixture");
    }
    for (const auto* stem : {"Inst.ogg", "Voices.ogg", "SecVoices.ogg"}) {
        std::ofstream output(
            audio_directory / stem,
            std::ios::binary | std::ios::trunc
        );
        output.put('\0');
        require(static_cast<bool>(output), "write DenpaEx audio fixture");
    }
    const auto loaded = pulseforge::ChartLoader::load(chart_path, options);
    require(static_cast<bool>(loaded), loaded.error);
    require(loaded.chart->events.size() == 2, "adjacent DenpaEx events loaded");
    require(
        loaded.chart->audio.instrumental.filename() == "Inst.ogg",
        "DenpaEx instrumental resolved"
    );
    require(
        loaded.chart->audio.vocals.size() == 2,
        "DenpaEx Voices and SecVoices resolved"
    );
    auto no_voices_json = std::string{json};
    const auto needs_voices_position = no_voices_json.find(
        "\"needsVoices\": true"
    );
    require(
        needs_voices_position != std::string::npos,
        "DenpaEx fixture contains needsVoices"
    );
    no_voices_json.replace(
        needs_voices_position,
        std::string{"\"needsVoices\": true"}.size(),
        "\"needsVoices\": false"
    );
    const auto no_voices = pulseforge::ChartLoader::parse(
        no_voices_json,
        chart_path,
        options
    );
    require(static_cast<bool>(no_voices), no_voices.error);
    require(
        no_voices.chart->audio.vocals.empty(),
        "DenpaEx needsVoices=false suppresses discovered vocal stems"
    );
    std::filesystem::remove_all(temporary_root);
}

void test_large_denpa_file_matches_dom_loader() {
    std::string json = R"json({"song":{"notes":[{"lengthInSteps":16,"mustHitSection":false,"sectionNotes":[[300,0,0,""],[100,4,125,"Hurt Note"],[350,1,0,7   ],[355,2,500,"Third Strum"],[360,1.5,0,""],[200,-1,42   ,{"b":2,"a":1},[1, 2]]]},{"lengthInSteps":16,"mustHitSection":true,"changeBPM":true,"bpm":522,"sectionNotes":[]}],"padding":")json";
    json.append(1'100'000, 'x');
    json += R"json(","header":{"song":"Large Denpa","artist":"Drive fixture","charter":"Tests","bpm":1044,"needsVoices":false},"assets":{"player1":"bf","player2":"dad","player4":"monster","enablePlayer4":true,"gfVersion":"gf"},"options":{"speed":1.75,"mania":3}}})json";

    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    std::u8string filename = u8"pulseforge-large-denpa-λ-";
    for (const char character : suffix) {
        filename.push_back(static_cast<char8_t>(character));
    }
    filename += u8".json";
    const auto path = std::filesystem::temp_directory_path()
        / std::filesystem::path(filename);
    struct ScopedRemove final {
        std::filesystem::path path;
        ~ScopedRemove() {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } cleanup{path};

    const auto write_fixture = [&](const std::string_view fixture,
                                   const std::string_view description) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        require(
            static_cast<bool>(output),
            std::string(description) + " opens"
        );
        output.write(
            fixture.data(),
            static_cast<std::streamsize>(fixture.size())
        );
        require(
            static_cast<bool>(output),
            std::string(description) + " is written"
        );
    };

    write_fixture(json, "large Denpa fixture");

    const auto fast = pulseforge::ChartLoader::load(path);
    const auto dom = pulseforge::ChartLoader::parse(json, path);
    require(static_cast<bool>(fast), fast.error);
    require(static_cast<bool>(dom), dom.error);
    require(
        pulseforge::chart_fingerprint(*fast.chart)
            == pulseforge::chart_fingerprint(*dom.chart),
        "large on-demand Denpa load matches the DOM parser"
    );
    require(
        fast.chart->source_format == pulseforge::ChartFormat::denpa
            && fast.chart->notes.size() == 4
            && fast.chart->events.size() == 1
            && fast.chart->title == "Large Denpa"
            && fast.chart->key_count == 4,
        "large Denpa metadata after note data is preserved"
    );
    require(
        std::any_of(
            fast.chart->notes.begin(),
            fast.chart->notes.end(),
            [](const auto& note) { return note.kind == "7"; }
        ),
        "fast scalar_string canonicalizes a spaced numeric note type"
    );
    const auto fast_third = std::find_if(
        fast.chart->notes.begin(),
        fast.chart->notes.end(),
        [](const auto& note) { return note.kind == "Third Strum"; }
    );
    require(
        fast_third != fast.chart->notes.end()
            && fast_third->owner == NoteOwner::secondary_opponent
            && fast.chart->secondary_opponent_enabled
            && fast.chart->secondary_opponent_character == "monster",
        "large Denpa fast path preserves Third Strum/player4 identity"
    );
    require(
        fast.chart->events.front().name == "42"
            && fast.chart->events.front().value1 == R"json({"a":1,"b":2})json"
            && fast.chart->events.front().value2 == "[1,2]",
        "fast scalar_string canonicalizes numeric and structured event values"
    );
    require(
        fast.chart->tempos.size() == 2U
            && fast.chart->tempos[0U].bpm == 1'044.0
            && fast.chart->tempos[1U].bpm == 522.0,
        "large Denpa DOM/fast paths preserve positive BPM above 1000"
    );

    std::string multikey =
        R"json({"song":{"notes":[{"mustHitSection":true,"sectionNotes":[[100,17,-100,""]]}],"padding":")json";
    multikey.append(1'100'000, 'x');
    multikey +=
        R"json(","song":"Fast 9K","bpm":120,"keyCount":4}})json";
    write_fixture(multikey, "large multikey Psych fixture");
    const auto multikey_fast = pulseforge::ChartLoader::load(path);
    const auto multikey_dom = pulseforge::ChartLoader::parse(multikey, path);
    require(static_cast<bool>(multikey_fast), multikey_fast.error);
    require(static_cast<bool>(multikey_dom), multikey_dom.error);
    require(
        multikey_fast.chart->key_count == 9U
            && multikey_fast.chart->notes.front().lane == 8U
            && multikey_fast.chart->notes.front().duration_ms == 0.0
            && multikey_dom.chart->notes.front().duration_ms == 0.0
            && pulseforge::chart_fingerprint(*multikey_fast.chart)
                == pulseforge::chart_fingerprint(*multikey_dom.chart),
        "large Psych fast path and DOM agree on negative-sustain tap repair"
    );

    std::string large_third_strum =
        R"json({"song":{"notes":[{"mustHitSection":true,"sectionNotes":[[100,4,500,"Third Strum"]]}],"padding":")json";
    large_third_strum.append(1'100'000, 'x');
    large_third_strum +=
        R"json(","song":"Large Psych Third Strum","bpm":120,"keyCount":4}})json";
    write_fixture(large_third_strum, "large Psych Third Strum fixture");
    const auto third_fast = pulseforge::ChartLoader::load(path);
    const auto third_dom = pulseforge::ChartLoader::parse(large_third_strum, path);
    require(static_cast<bool>(third_fast), third_fast.error);
    require(static_cast<bool>(third_dom), third_dom.error);
    require(
        third_fast.chart->notes.size() == 1U
            && third_dom.chart->notes.size() == 1U
            && third_fast.chart->notes.front().owner
                == NoteOwner::secondary_opponent
            && third_dom.chart->notes.front().owner
                == NoteOwner::secondary_opponent
            && pulseforge::chart_fingerprint(*third_fast.chart)
                == pulseforge::chart_fingerprint(*third_dom.chart),
        "large Psych fast/DOM paths agree on Third Strum secondary-owner semantics"
    );

    std::string high_bpm_psych =
        R"json({"song":{"notes":[{"lengthInSteps":16,"mustHitSection":true,"sectionNotes":[]},{"lengthInSteps":16,"mustHitSection":false,"changeBPM":true,"bpm":522,"sectionNotes":[]}],"padding":")json";
    high_bpm_psych.append(1'100'000, 'x');
    high_bpm_psych +=
        R"json(","song":"High BPM Psych","bpm":1044}})json";
    write_fixture(high_bpm_psych, "high-BPM Psych fixture");
    const auto high_bpm_fast = pulseforge::ChartLoader::load(path);
    const auto high_bpm_dom = pulseforge::ChartLoader::parse(
        high_bpm_psych,
        path
    );
    require(static_cast<bool>(high_bpm_fast), high_bpm_fast.error);
    require(static_cast<bool>(high_bpm_dom), high_bpm_dom.error);
    require(
        high_bpm_fast.chart->tempos.size() == 2U
            && high_bpm_fast.chart->tempos[0U].bpm == 1'044.0
            && high_bpm_fast.chart->tempos[1U].bpm == 522.0
            && pulseforge::chart_fingerprint(*high_bpm_fast.chart)
                == pulseforge::chart_fingerprint(*high_bpm_dom.chart),
        "large Psych DOM/fast paths preserve 1044 to 522 BPM parity"
    );

    for (const std::string_view invalid_bpm : {"0", "-1", "1e309"}) {
        std::string invalid_bpm_psych =
            R"json({"song":{"notes":[],"padding":")json";
        invalid_bpm_psych.append(1'100'000, 'x');
        invalid_bpm_psych += "\",\"song\":\"Invalid BPM\",\"bpm\":";
        invalid_bpm_psych.append(invalid_bpm);
        invalid_bpm_psych += "}}";
        write_fixture(invalid_bpm_psych, "invalid-BPM Psych fixture");
        require(
            !pulseforge::ChartLoader::load(path)
                && !pulseforge::ChartLoader::parse(invalid_bpm_psych, path),
            "Psych DOM/fast paths reject non-positive or non-finite BPM"
        );
    }

    std::string overflowing_sustain =
        R"json({"song":{"notes":[{"mustHitSection":true,"sectionNotes":[[100,0,1e308,""]]}],"padding":")json";
    overflowing_sustain.append(1'100'000, 'x');
    overflowing_sustain +=
        R"json(","song":"Overflowing sustain","bpm":120}})json";
    write_fixture(overflowing_sustain, "overflowing-sustain Psych fixture");
    require(
        !pulseforge::ChartLoader::load(path)
            && !pulseforge::ChartLoader::parse(overflowing_sustain, path),
        "positive Psych sustain overflow is never legacy-clamped"
    );

    std::string sparse_options =
        R"json({"song":{"notes":[{"mustHitSection":true,"sectionNotes":[[100,0,0,""]]}],"padding":")json";
    sparse_options.append(1'100'000, 'x');
    sparse_options +=
        R"json(","song":"Fast Options 6K","bpm":120,"options":{"mania":5,"speed":1.25}}})json";
    write_fixture(sparse_options, "large sparse options.mania fixture");
    const auto sparse_options_fast = pulseforge::ChartLoader::load(path);
    const auto sparse_options_dom = pulseforge::ChartLoader::parse(
        sparse_options,
        path
    );
    require(static_cast<bool>(sparse_options_fast), sparse_options_fast.error);
    require(static_cast<bool>(sparse_options_dom), sparse_options_dom.error);
    require(
        sparse_options_fast.chart->key_count == 6U
            && sparse_options_dom.chart->key_count == 6U
            && pulseforge::chart_fingerprint(*sparse_options_fast.chart)
                == pulseforge::chart_fingerprint(*sparse_options_dom.chart),
        "sparse JS options.mania has fast-path/DOM 6K parity"
    );

    std::string malformed = R"json({"song":{"notes":[{"mustHitSection":false,"sectionNotes":[[100,0,0,""]]}],"padding":")json";
    malformed.append(1'100'000, 'x');
    malformed += R"json(","header":null,"options":{}}})json";
    write_fixture(malformed, "malformed Denpa fixture");
    require(
        !pulseforge::ChartLoader::load(path)
            && !pulseforge::ChartLoader::parse(malformed, path),
        "large Denpa object requirements match the DOM parser"
    );

    std::string ordered = R"json({"events":[[500,[["Root Event","",""]]]],"song":{"events":[[500,[["Song Event","",""]]]],"notes":[{"mustHitSection":false,"sectionNotes":[[500,-1,"Embedded Event","",""]]}],"padding":")json";
    ordered.append(1'100'000, 'x');
    ordered += R"json(","song":"Ordered Psych","bpm":120,"speed":1}})json";
    write_fixture(ordered, "ordered Psych fixture");
    const auto ordered_fast = pulseforge::ChartLoader::load(path);
    const auto ordered_dom = pulseforge::ChartLoader::parse(ordered, path);
    require(static_cast<bool>(ordered_fast), ordered_fast.error);
    require(static_cast<bool>(ordered_dom), ordered_dom.error);
    require(
        pulseforge::chart_fingerprint(*ordered_fast.chart)
            == pulseforge::chart_fingerprint(*ordered_dom.chart),
        "large Psych event ordering matches the DOM parser"
    );
    require(
        ordered_fast.chart->events.size() == 3
            && ordered_fast.chart->events[0].name == "Embedded Event"
            && ordered_fast.chart->events[1].name == "Song Event"
            && ordered_fast.chart->events[2].name == "Root Event",
        "same-time Psych events retain embedded, song, root order"
    );

    const std::string trailing = ordered + " trailing garbage";
    write_fixture(trailing, "trailing-garbage Psych fixture");
    require(
        !pulseforge::ChartLoader::load(path)
            && !pulseforge::ChartLoader::parse(trailing, path),
        "large Psych trailing garbage is rejected like the DOM parser"
    );

    std::string invalid_token = R"json({"song":{"ignored":NaN,"notes":[{"mustHitSection":false,"sectionNotes":[[100,0,0,""]]}],"padding":")json";
    invalid_token.append(1'100'000, 'x');
    invalid_token += R"json(","song":"Invalid Psych","bpm":120}})json";
    write_fixture(invalid_token, "invalid-token Psych fixture");
    require(
        !pulseforge::ChartLoader::load(path)
            && !pulseforge::ChartLoader::parse(invalid_token, path),
        "invalid tokens in ignored Psych fields are rejected like DOM"
    );

    std::string empty = R"json({"song":{"notes":[],"padding":")json";
    empty.append(1'100'000, 'x');
    empty += R"json(","song":"Empty Psych","bpm":120,"speed":1}})json";
    write_fixture(empty, "empty large Psych fixture");
    const auto empty_loaded = pulseforge::ChartLoader::load(path);
    const auto empty_dom = pulseforge::ChartLoader::parse(empty, path);
    require(static_cast<bool>(empty_loaded), empty_loaded.error);
    require(static_cast<bool>(empty_dom), empty_dom.error);
    require(
        empty_loaded.chart->source_format == pulseforge::ChartFormat::psych
            && empty_loaded.chart->notes.empty()
            && pulseforge::chart_fingerprint(*empty_loaded.chart)
                == pulseforge::chart_fingerprint(*empty_dom.chart),
        "large wrapped Psych notes:[] remains accepted with DOM parity"
    );

    std::string duplicate_fields = R"json({"song":{"notes":[{"sectionNotes":[[100,0,0,""]]}],"notes":[{"sectionNotes":[[200,1,0,""]]}],"padding":")json";
    duplicate_fields.append(1'100'000, 'x');
    duplicate_fields += R"json(","song":"Duplicate Psych","bpm":120}})json";
    write_fixture(duplicate_fields, "duplicate-field Psych fixture");
    const auto duplicate_loaded = pulseforge::ChartLoader::load(path);
    const auto duplicate_dom = pulseforge::ChartLoader::parse(
        duplicate_fields,
        path
    );
    require(static_cast<bool>(duplicate_loaded), duplicate_loaded.error);
    require(static_cast<bool>(duplicate_dom), duplicate_dom.error);
    require(
        duplicate_loaded.chart->notes.size() == 1
            && duplicate_loaded.chart->notes.front().time_ms == 200.0
            && pulseforge::chart_fingerprint(*duplicate_loaded.chart)
                == pulseforge::chart_fingerprint(*duplicate_dom.chart),
        "large Psych duplicate content fields fall back to DOM semantics"
    );

    std::string explicit_empty =
        R"json({"song":{"notes":[],"padding":")json";
    explicit_empty.append(1'100'000, 'x');
    explicit_empty +=
        R"json(","song":"","name":"Fallback","artist":"","charter":"","bpm":120}})json";
    write_fixture(explicit_empty, "empty-metadata Psych fixture");
    const auto empty_metadata_fast = pulseforge::ChartLoader::load(path);
    const auto empty_metadata_dom = pulseforge::ChartLoader::parse(
        explicit_empty,
        path
    );
    require(static_cast<bool>(empty_metadata_fast), empty_metadata_fast.error);
    require(static_cast<bool>(empty_metadata_dom), empty_metadata_dom.error);
    require(
        empty_metadata_fast.chart->title.empty()
            && empty_metadata_fast.chart->artist.empty()
            && empty_metadata_fast.chart->charter.empty()
            && pulseforge::chart_fingerprint(*empty_metadata_fast.chart)
                == pulseforge::chart_fingerprint(*empty_metadata_dom.chart),
        "explicitly empty Psych metadata has DOM parity"
    );

    std::string empty_denpa =
        R"json({"song":{"notes":[],"padding":")json";
    empty_denpa.append(1'100'000, 'x');
    empty_denpa +=
        R"json(","name":"Fallback","artist":"Fallback","charter":"Fallback","header":{"song":"","artist":"","charter":"","bpm":120,"needsVoices":false},"options":{"speed":1,"mania":3}}})json";
    write_fixture(empty_denpa, "empty-metadata Denpa fixture");
    const auto empty_denpa_fast = pulseforge::ChartLoader::load(path);
    const auto empty_denpa_dom = pulseforge::ChartLoader::parse(
        empty_denpa,
        path
    );
    require(static_cast<bool>(empty_denpa_fast), empty_denpa_fast.error);
    require(static_cast<bool>(empty_denpa_dom), empty_denpa_dom.error);
    require(
        empty_denpa_fast.chart->title.empty()
            && empty_denpa_fast.chart->artist.empty()
            && empty_denpa_fast.chart->charter.empty()
            && pulseforge::chart_fingerprint(*empty_denpa_fast.chart)
                == pulseforge::chart_fingerprint(*empty_denpa_dom.chart),
        "explicitly empty Denpa header metadata has DOM parity"
    );

    const std::string oversized_ignored_header(1'025, 'm');
    std::string limit_before_notes =
        "{\"song\":{\"header\":{\"song\":\""
        + oversized_ignored_header
        + "\"},\"notes\":[],\"padding\":\"";
    limit_before_notes.append(1'100'000, 'x');
    limit_before_notes += "\",\"song\":\"Order\",\"bpm\":120}}";
    std::string limit_after_notes =
        "{\"song\":{\"notes\":[],\"header\":{\"song\":\""
        + oversized_ignored_header
        + "\"},\"padding\":\"";
    limit_after_notes.append(1'100'000, 'x');
    limit_after_notes += "\",\"song\":\"Order\",\"bpm\":120}}";
    for (const auto* ordered_fixture : {
             &limit_before_notes,
             &limit_after_notes,
         }) {
        write_fixture(*ordered_fixture, "metadata-order Psych fixture");
        const auto ordered_limit_fast = pulseforge::ChartLoader::load(path);
        const auto ordered_limit_dom = pulseforge::ChartLoader::parse(
            *ordered_fixture,
            path
        );
        require(static_cast<bool>(ordered_limit_fast), ordered_limit_fast.error);
        require(static_cast<bool>(ordered_limit_dom), ordered_limit_dom.error);
        require(
            pulseforge::chart_fingerprint(*ordered_limit_fast.chart)
                == pulseforge::chart_fingerprint(*ordered_limit_dom.chart),
            "fast-only metadata limits are independent of key order"
        );
    }

    std::string bigint =
        R"json({"song":{"notes":[{"sectionNotes":[[0,-1,18446744073709551616],[100,18446744073709551616,0,""]]}],"ignored":18446744073709551616,"padding":")json";
    bigint.append(1'100'000, 'x');
    bigint += R"json(","song":"BIGINT","bpm":120}})json";
    write_fixture(bigint, "BIGINT Psych fixture");
    const auto bigint_fast = pulseforge::ChartLoader::load(path);
    const auto bigint_dom = pulseforge::ChartLoader::parse(bigint, path);
    require(static_cast<bool>(bigint_fast), bigint_fast.error);
    require(static_cast<bool>(bigint_dom), bigint_dom.error);
    require(
        bigint_fast.chart->notes.empty()
            && bigint_fast.chart->events.size() == 1
            && pulseforge::chart_fingerprint(*bigint_fast.chart)
                == pulseforge::chart_fingerprint(*bigint_dom.chart),
        "finite BIGINT coercion matches permissive DOM semantics"
    );

    std::string comments =
        R"json({"song":{"notes":[],/* Psych JSONC */"padding":")json";
    comments.append(1'100'000, 'x');
    comments +=
        R"json(","song":"Comments","bpm":120}}/* trailing comment */)json";
    write_fixture(comments, "commented Psych fixture");
    const auto comments_fast = pulseforge::ChartLoader::load(path);
    const auto comments_dom = pulseforge::ChartLoader::parse(comments, path);
    require(static_cast<bool>(comments_fast), comments_fast.error);
    require(static_cast<bool>(comments_dom), comments_dom.error);
    require(
        pulseforge::chart_fingerprint(*comments_fast.chart)
            == pulseforge::chart_fingerprint(*comments_dom.chart),
        "large commented JSON retains permissive DOM compatibility"
    );

    std::string signed_zero =
        R"json({"song":{"notes":[{"sectionNotes":[[-0,0,-0,""],[-0,-1,"Zero Event","",""]]}],"padding":")json";
    signed_zero.append(1'100'000, 'x');
    signed_zero += R"json(","song":"Signed zero","bpm":120}})json";
    write_fixture(signed_zero, "signed-zero Psych fixture");
    const auto signed_zero_fast = pulseforge::ChartLoader::load(path);
    const auto signed_zero_dom = pulseforge::ChartLoader::parse(
        signed_zero,
        path
    );
    require(static_cast<bool>(signed_zero_fast), signed_zero_fast.error);
    require(static_cast<bool>(signed_zero_dom), signed_zero_dom.error);
    require(
        pulseforge::chart_fingerprint(*signed_zero_fast.chart)
            == pulseforge::chart_fingerprint(*signed_zero_dom.chart),
        "signed zero is canonicalized before chart fingerprinting"
    );
}

void test_vslice_loader() {
    constexpr auto json = R"json(
    {
      "version": "2.2.4",
      "songName": "V-Slice Test",
      "artist": "Tests",
      "timeChanges": [
        {"t": 0, "b": 0, "bpm": 100, "n": 4, "d": 4},
        {"t": 2400, "b": 4, "bpm": 200, "n": 3, "d": 4}
      ],
      "scrollSpeed": {"normal": 1.3},
      "notes": {
        "normal": [
          {"t": 100, "d": 0, "l": 0, "k": "normal", "p": {"speed": 2, "tags": ["a"]}},
          {"t": 200, "d": 4, "l": 300, "k": "alt", "p": {"speed": 2, "tags": ["a"]}}
        ]
      },
      "events": [
        {"t": 300, "e": "FocusCamera", "v": {"char": 1}}
      ]
    })json";
    const auto result = pulseforge::ChartLoader::parse(json);
    require(static_cast<bool>(result), result.error);
    require(result.chart->source_format == pulseforge::ChartFormat::vslice, "V-Slice detected");
    require(result.chart->notes.size() == 2, "V-Slice notes parsed");
    require(result.chart->notes[0].owner == NoteOwner::player, "V-Slice player");
    require(result.chart->notes[1].owner == NoteOwner::opponent, "V-Slice opponent");
    require(
        result.chart->note_payloads.size() == 2
            && result.chart->notes[0].payload_id
                == result.chart->notes[1].payload_id,
        "V-Slice note payloads are preserved and interned"
    );
    require(
        result.chart->note_payloads[result.chart->notes[0].payload_id]
            == R"json({"speed":2,"tags":["a"]})json",
        "V-Slice note payload keeps canonical JSON"
    );
    require(
        result.chart->events.size() == 1
            && result.chart->events[0].payload_json
                == R"json({"char":1})json",
        "V-Slice arbitrary event payload is preserved"
    );
    require(result.chart->tempos.size() == 2, "V-Slice time changes");
}

void test_judgment_boundaries() {
    Chart chart;
    chart.tempos = {{0.0, 120.0, 4, 4}};
    chart.notes = {{1'000.0, 0.0, 0, NoteOwner::player, "normal"}};
    chart.normalize();

    const std::vector<std::pair<double, pulseforge::Rating>> cases{
        {22.5, pulseforge::Rating::marvelous},
        {45.0, pulseforge::Rating::sick},
        {90.0, pulseforge::Rating::good},
        {135.0, pulseforge::Rating::bad},
    };
    for (const auto& [offset, expected] : cases) {
        GameplaySession session(chart);
        session.update(1'000.0 + offset);
        session.press(0, 1'000.0 + offset);
        require(!session.frame_events().empty(), "judgment emits event");
        require(session.frame_events().back().rating == expected, "judgment boundary");
    }
}

void test_sustain_is_frame_rate_independent() {
    Chart chart;
    chart.tempos = {{0.0, 120.0, 4, 4}, {1'500.0, 180.0, 4, 4}};
    chart.notes = {{1'000.0, 1'000.0, 0, NoteOwner::player, "normal"}};
    chart.normalize();

    auto simulate = [&](const double frame_ms) {
        GameplaySession session(chart);
        session.update(1'000.0);
        session.press(0, 1'000.0);
        for (double time = 1'000.0 + frame_ms; time <= 2'100.0; time += frame_ms) {
            session.update(time);
        }
        session.update(2'100.0);
        session.release(0, 2'100.0);
        return session.summary();
    };

    const auto fast = simulate(1.0);
    const auto slow = simulate(33.333);
    require(fast.score == slow.score, "sustain score independent of frame rate");
    require(fast.hold_ticks == slow.hold_ticks, "sustain ticks independent of frame rate");
    require(fast.misses == slow.misses, "sustain misses independent of frame rate");
}

void test_replay_determinism() {
    auto chart = basic_chart();
    pulseforge::Replay replay;
    replay.chart_hash = pulseforge::chart_fingerprint(chart);
    replay.difficulty = chart.difficulty;
    replay.inputs = {
        {1'004.0, 0, true},
        {1'020.0, 0, false},
        {1'496.0, 1, true},
        {2'010.0, 1, false},
    };

    const auto fast = pulseforge::simulate_replay(chart, replay, 0.5);
    const auto slow = pulseforge::simulate_replay(chart, replay, 31.0);
    require(fast.score == slow.score, "replay score deterministic");
    require_near(
        fast.accuracy_percent(),
        slow.accuracy_percent(),
        0.000001,
        "replay accuracy deterministic"
    );
    require(fast.misses == slow.misses, "replay misses deterministic");
    require(fast.hold_ticks == slow.hold_ticks, "replay holds deterministic");
}

void test_replay_uses_live_input_order() {
    Chart chart;
    chart.tempos = {{0.0, 120.0, 4, 4}};
    chart.notes = {
        {1'000.0, 500.0, 0, NoteOwner::player, "normal"},
        {1'000.0, 0.0, 1, NoteOwner::player, "mine"},
    };
    chart.normalize();

    pulseforge::GameplaySettings settings;
    settings.autoplay = true;
    settings.no_fail = true;
    settings.input_offset_ms = 90.0;

    GameplaySession live(chart, settings);
    live.update(999.0);
    live.press(0, 1'000.0);
    live.press(1, 1'000.0);
    live.update(1'000.0);
    live.update(chart.duration_ms() + settings.windows.miss_ms);

    pulseforge::Replay replay;
    replay.settings = settings;
    replay.random_seed = settings.random_seed;
    replay.inputs = {
        {1'000.0, 0, true},
        {1'000.0, 1, true},
    };
    const auto simulated = pulseforge::simulate_replay(chart, replay, 17.0);
    const auto& expected = live.summary();
    require(
        simulated.score == expected.score
            && simulated.good == expected.good
            && simulated.marvelous == expected.marvelous
            && simulated.misses == expected.misses
            && simulated.hold_ticks == expected.hold_ticks
            && simulated.judged_notes == expected.judged_notes,
        "headless replay dispatches simultaneous input before the frame update"
    );

    Chart fatal_chart;
    fatal_chart.tempos = {{0.0, 120.0, 4, 4}};
    fatal_chart.notes = {
        {1'000.0, 1'000.0, 0, NoteOwner::player, "normal"},
        {1'250.0, 0.0, 1, NoteOwner::player, "mine"},
    };
    fatal_chart.normalize();

    pulseforge::GameplaySettings fatal_settings;
    fatal_settings.health_loss = 2.0;
    GameplaySession fatal_live(fatal_chart, fatal_settings);
    for (double time = 100.0; time < 1'000.0; time += 100.0) {
        fatal_live.update(time);
    }
    fatal_live.press(0, 1'000.0);
    fatal_live.update(1'000.0);
    fatal_live.update(1'100.0);
    fatal_live.update(1'200.0);
    fatal_live.press(1, 1'250.0);
    require(fatal_live.summary().failed, "mine input causes immediate failure");
    const auto terminal_live = fatal_live.summary();
    fatal_live.update(1'250.0);
    require(
        fatal_live.summary().score == terminal_live.score
            && fatal_live.summary().hold_ticks == terminal_live.hold_ticks,
        "a failed session cannot award later sustain work"
    );

    pulseforge::Replay fatal_replay;
    fatal_replay.settings = fatal_settings;
    fatal_replay.inputs = {
        {1'000.0, 0, true},
        {1'250.0, 1, true},
    };
    const auto fatal_simulated = pulseforge::simulate_replay(
        fatal_chart,
        fatal_replay,
        100.0
    );
    require(
        fatal_simulated.failed
            && fatal_simulated.score == terminal_live.score
            && fatal_simulated.hold_ticks == terminal_live.hold_ticks,
        "fatal replay input stops at the same terminal summary as live input"
    );
}

void test_invalid_chart_is_rejected() {
    constexpr auto invalid = R"json(
    {
      "format": "pulseforge-chart",
      "song": {"keyCount": 4},
      "tempos": [{"time": 0, "bpm": 120}],
      "notes": [{"time": 100, "lane": 99, "owner": "player"}]
    })json";
    const auto result = pulseforge::ChartLoader::parse(invalid);
    require(!result, "invalid lane rejected");

    Chart noisy;
    noisy.tempos = {{0.0, 120.0, 4, 4}};
    for (std::size_t index = 0; index < 300; ++index) {
        noisy.notes.push_back({
            -12'000.0 + static_cast<double>(index),
            0.0,
            0,
            NoteOwner::player,
            "normal",
        });
    }
    noisy.notes.push_back({0.0, 0.0, 99, NoteOwner::player, "normal"});
    const auto issues = pulseforge::validate_chart(noisy);
    require(issues.size() <= 256, "chart diagnostics have a hard memory bound");
    require(
        std::any_of(
            issues.begin(),
            issues.end(),
            [](const pulseforge::ValidationIssue& issue) {
                return issue.severity == pulseforge::ValidationSeverity::error;
            }
        ),
        "bounded warnings cannot hide a later chart error"
    );

    Chart descending;
    descending.tempos = {{0.0, 120.0, 4, 4}};
    for (std::size_t index = 0; index < 300; ++index) {
        descending.notes.push_back({
            1'000.0 - static_cast<double>(index),
            0.0,
            index == 299 ? std::uint16_t{99} : std::uint16_t{0},
            NoteOwner::player,
            "normal",
        });
    }
    const auto descending_issues = pulseforge::validate_chart(descending);
    require(
        std::any_of(
            descending_issues.begin(),
            descending_issues.end(),
            [](const pulseforge::ValidationIssue& issue) {
                return issue.message == "note lane is outside key_count";
            }
        ),
        "repairable ordering errors cannot hide a structural chart error"
    );

    std::string inserted_tempo_overflow = R"json({"format":"pulseforge-chart","song":{"keyCount":4},"tempos":[)json";
    for (std::size_t index = 0;
         index < pulseforge::maximum_chart_tempo_changes;
         ++index) {
        if (index != 0) {
            inserted_tempo_overflow.push_back(',');
        }
        inserted_tempo_overflow += "{\"time\":"
            + std::to_string(index + 1U)
            + ",\"bpm\":120}";
    }
    inserted_tempo_overflow += R"json(],"notes":[]})json";
    require(
        !pulseforge::ChartLoader::parse(inserted_tempo_overflow),
        "normalization cannot insert a tempo beyond the chart limit"
    );

    std::string strict_warning_after_ordering =
        R"json({"format":"pulseforge-chart","song":{"keyCount":4},"tempos":[{"time":0,"bpm":120}],"notes":[)json";
    for (std::size_t index = 0; index < 300; ++index) {
        if (index != 0) {
            strict_warning_after_ordering.push_back(',');
        }
        strict_warning_after_ordering += "{\"time\":"
            + std::to_string(1'000U - index)
            + ",\"lane\":0,\"owner\":\"player\"}";
    }
    strict_warning_after_ordering +=
        R"json(],"events":[{"time":1500,"name":""}]})json";
    pulseforge::ChartLoadOptions strict;
    strict.strict = true;
    require(
        !pulseforge::ChartLoader::parse(
            strict_warning_after_ordering,
            {},
            strict
        ),
        "strict validation catches a warning hidden behind bounded ordering diagnostics"
    );
}

void test_loader_edge_contracts() {
    constexpr auto overflow_lane = R"json(
    {
      "format": "pulseforge-chart",
      "song": {"keyCount": 4},
      "tempos": [{"time": 0, "bpm": 120}],
      "notes": [{"time": 100, "lane": 18446744073709551615}]
    })json";
    require(
        !pulseforge::ChartLoader::parse(overflow_lane),
        "unsigned lane overflow is rejected"
    );

    constexpr auto invalid_numbers = R"json(
    {
      "format": "pulseforge-chart",
      "tempos": [{"time": 0, "bpm": -10}],
      "notes": [{"time": 100, "duration": -500, "lane": 0}]
    })json";
    require(
        !pulseforge::ChartLoader::parse(invalid_numbers),
        "invalid values are not repaired before validation"
    );

    constexpr auto overflowing_end = R"json(
    {
      "format": "pulseforge-chart",
      "tempos": [{"time": 0, "bpm": 120}],
      "notes": [{"time": 1e308, "duration": 1e308, "lane": 0}]
    })json";
    require(
        !pulseforge::ChartLoader::parse(overflowing_end),
        "overflowing note end is rejected"
    );

    constexpr auto malformed_psych = R"json(
    {"song":{"song":"Broken","bpm":120,"notes":{}}}
    )json";
    require(
        !pulseforge::ChartLoader::parse(malformed_psych),
        "malformed Psych sections are rejected"
    );

    constexpr auto mania_zero = R"json(
    {"song":{"song":"Four keys","bpm":120,"mania":0,"notes":[]}}
    )json";
    const auto mania = pulseforge::ChartLoader::parse(mania_zero);
    require(static_cast<bool>(mania), mania.error);
    require(mania.chart->key_count == 4, "Psych mania zero maps to four keys");

    constexpr auto unwrapped_psych = R"json(
    {
      "song":"Unwrapped",
      "bpm":120,
      "notes":[
        {"mustHitSection":true,"sectionNotes":[[100,0,0,""]]}
      ],
      "events":[[50,[["Pulse","1",""]]]]
    }
    )json";
    const auto unwrapped = pulseforge::ChartLoader::parse(unwrapped_psych);
    require(static_cast<bool>(unwrapped), unwrapped.error);
    require(
        unwrapped.chart->source_format == pulseforge::ChartFormat::psych,
        "unwrapped Psych chart detected"
    );
    require(
        unwrapped.chart->events.size() == 1,
        "unwrapped Psych root events are not imported twice"
    );
}

void test_vslice_contracts() {
    constexpr auto chart_json = R"json(
    {
      "scrollSpeed":{"easy":1.0,"hard":1.2},
      "notes":{
        "easy":[{"t":100,"d":0}],
        "hard":[{"t":100,"d":4},{"t":200,"d":5}]
      }
    }
    )json";
    pulseforge::ChartLoadOptions options;
    options.difficulty = "impossible";
    options.difficulty_explicit = true;
    require(
        !pulseforge::ChartLoader::parse(chart_json, {}, options),
        "missing explicit V-Slice difficulty is rejected"
    );

    const auto metadata_path =
        std::filesystem::temp_directory_path()
        / "pulseforge-invalid-metadata.json";
    {
        std::ofstream output(metadata_path, std::ios::binary | std::ios::trunc);
        output << "[1,2,3]\n";
    }
    options = {};
    options.metadata_path = metadata_path;
    const auto metadata_result =
        pulseforge::ChartLoader::parse(chart_json, {}, options);
    std::error_code remove_error;
    std::filesystem::remove(metadata_path, remove_error);
    require(!metadata_result, "non-object V-Slice metadata is rejected");
}

void test_strict_import_contracts() {
    const auto require_strict_rejection = [](
        const std::string_view source,
        const std::string& label
    ) {
        const auto permissive = pulseforge::ChartLoader::parse(source);
        require(
            static_cast<bool>(permissive),
            label + " remains available in permissive migration mode"
        );
        pulseforge::ChartLoadOptions strict;
        strict.strict = true;
        const auto rejected = pulseforge::ChartLoader::parse(
            source,
            {},
            strict
        );
        require(!rejected, label + " is rejected in strict mode");
        require(
            rejected.error.find("strict import rejected") != std::string::npos,
            label + " reports a strict import diagnostic"
        );
    };

    require_strict_rejection(
        R"json({
          "format":"pulseforge-chart",
          "tempos":[{"time":0,"bpm":120}],
          "notes":[null,{"time":100,"lane":0}]
        })json",
        "native skipped item"
    );
    require_strict_rejection(
        R"json({
          "format":"pulseforge-chart",
          "tempos":[{"time":0,"bpm":120}],
          "notes":[{"time":"100","lane":0}]
        })json",
        "native numeric coercion"
    );
    require_strict_rejection(
        R"json({
          "format":"pulseforge-chart",
          "tempos":[{"time":0,"bpm":120}],
          "notes":[{"time":100,"lane":0,"owner":"spectator"}]
        })json",
        "native unknown owner"
    );
    require_strict_rejection(
        R"json({
          "format":"pulseforge-chart",
          "tempos":[{"time":0,"bpm":120}],
          "notes":[{"time":100}]
        })json",
        "native missing lane"
    );
    require_strict_rejection(
        R"json({"song":{"song":"strict","bpm":120,"notes":[null]}})json",
        "Psych skipped section"
    );
    require_strict_rejection(
        R"json({"song":{"song":"strict","notes":[]}})json",
        "Psych missing BPM"
    );
    require_strict_rejection(
        R"json({
          "song":{"song":"strict","bpm":120,
          "notes":[{"sectionNotes":[]}]}
        })json",
        "Psych missing section contract"
    );
    require_strict_rejection(
        R"json({
          "timeChanges":[{"t":0,"bpm":120}],
          "notes":[null]
        })json",
        "V-Slice skipped note"
    );
    require_strict_rejection(
        R"json({
          "timeChanges":[{"t":0,"bpm":120}],
          "notes":[{"t":100,"d":"4"}]
        })json",
        "V-Slice direction coercion"
    );
    require_strict_rejection(
        R"json({
          "timeChanges":[{"t":0,"bpm":120}],
          "notes":[{"l":0,"d":4}]
        })json",
        "V-Slice missing time"
    );

    pulseforge::ChartLoadOptions strict;
    strict.strict = true;
    const auto numeric_psych_type = pulseforge::ChartLoader::parse(
        R"json({
          "song":{
            "song":"strict",
            "bpm":120,
            "notes":[{
              "mustHitSection":true,
              "sectionNotes":[[100,0,0,7]]
            }]
          }
        })json",
        {},
        strict
    );
    require(
        static_cast<bool>(numeric_psych_type),
        "strict Psych keeps documented scalar numeric note types"
    );
    require(
        numeric_psych_type.chart->notes[0].kind == "7",
        "numeric Psych note type is canonicalized"
    );

    const auto structured_vslice_payload = pulseforge::ChartLoader::parse(
        R"json({
          "timeChanges":[{"t":0,"bpm":120}],
          "notes":[{"t":100,"d":0,"p":{"skin":"pixel"}}],
          "events":[{"t":50,"e":"FocusCamera","v":{"char":1}}]
        })json",
        {},
        strict
    );
    require(
        static_cast<bool>(structured_vslice_payload),
        "strict V-Slice accepts documented arbitrary JSON payloads"
    );
    require(
        structured_vslice_payload.chart->note_payloads.size() == 2
            && structured_vslice_payload.chart->events[0].payload_json
                == R"json({"char":1})json",
        "strict V-Slice preserves structured note and event payloads"
    );
}

void test_botplay_replay_and_score_saturation() {
    auto chart = basic_chart();
    pulseforge::GameplaySettings settings;
    settings.autoplay = true;
    settings.practice = true;
    GameplaySession original(chart, settings);
    original.update(chart.duration_ms());
    const auto replay = pulseforge::make_replay(chart, original, "test");
    const auto simulated = pulseforge::simulate_replay(chart, replay, 17.0);
    require(simulated.score == original.summary().score, "BOTPLAY replay score");
    require(
        simulated.marvelous == original.summary().marvelous,
        "BOTPLAY replay judgments"
    );
    require(
        simulated.hold_ticks == original.summary().hold_ticks,
        "BOTPLAY replay holds"
    );

    Chart one_note;
    one_note.tempos = {{0.0, 120.0, 4, 4}};
    one_note.notes = {{1'000.0, 0.0, 0, NoteOwner::player, "normal"}};
    one_note.normalize();
    GameplaySession saturated(one_note);
    saturated.add_score(std::numeric_limits<std::int64_t>::max());
    saturated.update(1'000.0);
    saturated.press(0, 1'000.0);
    require(
        saturated.summary().score == std::numeric_limits<std::int64_t>::max(),
        "hit score addition saturates"
    );
}

void test_release_order_and_completion() {
    Chart hold_chart;
    hold_chart.tempos = {{0.0, 120.0, 4, 4}};
    hold_chart.notes = {
        {1'000.0, 1'000.0, 0, NoteOwner::player, "normal"},
    };
    hold_chart.normalize();

    GameplaySession update_first(hold_chart);
    update_first.update(1'000.0);
    update_first.press(0, 1'000.0);
    update_first.update(1'800.0);
    update_first.release(0, 1'800.0);

    GameplaySession release_first(hold_chart);
    release_first.update(1'000.0);
    release_first.press(0, 1'000.0);
    release_first.release(0, 1'800.0);
    release_first.update(1'800.0);
    require(
        release_first.summary().score == update_first.summary().score,
        "release-before-update preserves sustain score"
    );
    require(
        release_first.summary().hold_ticks == update_first.summary().hold_ticks,
        "release-before-update preserves sustain ticks"
    );

    Chart opponent_chart;
    opponent_chart.tempos = {{0.0, 120.0, 4, 4}};
    opponent_chart.notes = {
        {3'000.0, 0.0, 0, NoteOwner::opponent, "normal"},
    };
    opponent_chart.events = {{3'500.0, "late", "", ""}};
    opponent_chart.normalize();
    GameplaySession opponent_only(opponent_chart);
    opponent_only.update(1'000.0);
    require(!opponent_only.complete(), "opponent/event content prevents early completion");
    opponent_only.update(opponent_chart.duration_ms());
    require(
        !opponent_only.complete(),
        "resolved chart content does not impersonate the media end"
    );
    opponent_only.finish_song(opponent_chart.duration_ms());
    require(opponent_only.complete(), "authoritative media end completes gameplay");
}

void test_input_offset_only_affects_player_input() {
    Chart chart;
    chart.tempos = {{0.0, 120.0, 4, 4}};
    chart.notes = {
        {1'000.0, 0.0, 0, NoteOwner::player, "normal"},
        {1'000.0, 0.0, 1, NoteOwner::opponent, "normal"},
    };
    chart.events = {{1'000.0, "clock-event", "", ""}};
    chart.normalize();

    pulseforge::GameplaySettings settings;
    settings.input_offset_ms = 1'000.0;
    GameplaySession session(chart, settings);

    session.update(0.0);
    session.press(0, 0.0);
    require(
        session.summary().marvelous == 1,
        "input offset calibrates physical player input"
    );

    session.begin_frame();
    session.update(999.0);
    const auto before_time = std::count_if(
        session.frame_events().begin(),
        session.frame_events().end(),
        [](const pulseforge::GameplayEvent& event) {
            return event.type == pulseforge::GameplayEventType::opponent_hit
                || event.type == pulseforge::GameplayEventType::chart_event;
        }
    );
    require(before_time == 0, "input offset does not advance chart schedulers");

    session.begin_frame();
    session.update(1'000.0);
    const auto at_time = std::count_if(
        session.frame_events().begin(),
        session.frame_events().end(),
        [](const pulseforge::GameplayEvent& event) {
            return event.type == pulseforge::GameplayEventType::opponent_hit
                || event.type == pulseforge::GameplayEventType::chart_event;
        }
    );
    require(at_time == 2, "chart schedulers stay on the audio clock");

    Chart cross_lane;
    cross_lane.tempos = {{0.0, 120.0, 4, 4}};
    cross_lane.notes = {
        {100.0, 0.0, 1, NoteOwner::player, "normal"},
        {1'000.0, 0.0, 0, NoteOwner::player, "normal"},
    };
    cross_lane.normalize();
    GameplaySession cross_lane_press(cross_lane, settings);
    cross_lane_press.press(0, 0.0);
    require(
        cross_lane_press.note_state(0) == pulseforge::NoteState::pending,
        "offset input does not miss another lane early"
    );

    Chart holds;
    holds.tempos = {{0.0, 120.0, 4, 4}};
    holds.notes = {
        {0.0, 1'000.0, 0, NoteOwner::player, "normal"},
        {0.0, 1'000.0, 1, NoteOwner::player, "normal"},
    };
    holds.normalize();
    GameplaySession cross_lane_release(holds, settings);
    cross_lane_release.press(0, -1'000.0);
    cross_lane_release.press(1, -1'000.0);
    cross_lane_release.release(0, 0.0);
    require(
        cross_lane_release.note_state(1) == pulseforge::NoteState::holding,
        "offset release does not advance another lane"
    );
}

void test_generated_workload_limit() {
    Chart chart;
    chart.tempos = {{0.0, 1'000.0, 4, 4}};
    for (std::uint16_t lane = 0; lane < 4; ++lane) {
        chart.notes.push_back({
            0.0,
            2.0 * 60.0 * 60.0 * 1'000.0,
            lane,
            NoteOwner::player,
            "normal",
        });
    }
    chart.normalize();
    const auto issues = pulseforge::validate_chart(chart);
    require(
        std::any_of(
            issues.begin(),
            issues.end(),
            [](const pulseforge::ValidationIssue& issue) {
                return issue.severity == pulseforge::ValidationSeverity::error;
            }
        ),
        "pathological generated event workload is rejected"
    );
}

void test_failure_is_immediate_and_exclusive() {
    Chart empty_chart;
    empty_chart.tempos = {{0.0, 120.0, 4, 4}};
    empty_chart.normalize();
    pulseforge::GameplaySettings settings;
    settings.ghost_tapping = false;
    settings.ghost_tap_health_loss = 1.0;

    GameplaySession live(empty_chart, settings);
    live.press(0, 100.0);
    live.press(0, 101.0);
    require(live.summary().failed, "health depletion latches failure immediately");
    require(live.summary().misses == 1, "post-failure input is rejected immediately");

    const std::vector<pulseforge::InputRecord> inputs{
        {100.0, 0, true},
        {101.0, 0, true},
    };
    pulseforge::Replay replay;
    replay.settings = settings;
    replay.random_seed = settings.random_seed;
    replay.inputs = inputs;
    const auto simulated = pulseforge::simulate_replay(
        empty_chart,
        replay,
        1'000.0 / 240.0
    );
    require(
        simulated.misses == live.summary().misses && simulated.failed,
        "failure is replay/FPS independent"
    );

    Chart lethal_chart;
    lethal_chart.tempos = {{0.0, 120.0, 4, 4}};
    for (std::size_t index = 0; index < 20; ++index) {
        lethal_chart.notes.push_back({
            0.0,
            0.0,
            static_cast<std::uint16_t>(index % 4U),
            NoteOwner::player,
            "normal",
        });
    }
    lethal_chart.normalize();
    GameplaySession lethal(lethal_chart);
    lethal.update(3'000.0);
    require(lethal.summary().failed, "terminal catch-up can fail");
    require(!lethal.complete(), "failure and completion are mutually exclusive");
    const auto completion_events = std::count_if(
        lethal.frame_events().begin(),
        lethal.frame_events().end(),
        [](const pulseforge::GameplayEvent& event) {
            return event.type == pulseforge::GameplayEventType::song_complete;
        }
    );
    require(completion_events == 0, "failed update emits no completion event");
}

void test_replay_loader_ranges() {
    const auto replay_path =
        std::filesystem::temp_directory_path()
        / "pulseforge-invalid-replay.json";
    {
        std::ofstream output(replay_path, std::ios::binary | std::ios::trunc);
        output
            << R"json({
              "format":"pulseforge-replay",
              "formatVersion":1,
              "chartHash":"test",
              "randomSeed":0,
              "settings":{},
              "inputs":[{"timeMs":100,"lane":65536,"pressed":true}]
            })json";
    }
    const auto loaded = pulseforge::load_replay(replay_path);
    std::error_code remove_error;
    std::filesystem::remove(replay_path, remove_error);
    require(!loaded, "replay lane overflow is rejected");

    auto chart = basic_chart();
    pulseforge::Replay programmatic;
    programmatic.inputs = {
        {std::numeric_limits<double>::quiet_NaN(), 0, true},
        {1'000.0, 0, true},
    };
    auto filtered = programmatic;
    filtered.inputs.erase(filtered.inputs.begin());
    const auto sanitized = pulseforge::simulate_replay(
        chart,
        programmatic,
        std::numeric_limits<double>::quiet_NaN()
    );
    const auto expected = pulseforge::simulate_replay(chart, filtered, 1.0);
    require(
        sanitized.score == expected.score
            && sanitized.misses == expected.misses
            && sanitized.hold_ticks == expected.hold_ticks,
        "programmatic replay NaNs are filtered before sorting and stepping"
    );
}

void test_settings_limits_and_sanitization() {
    const auto settings_path =
        std::filesystem::temp_directory_path()
        / "pulseforge-settings-test.json";
    {
        std::ofstream output(settings_path, std::ios::binary | std::ios::trunc);
        output << R"json({
          "audio":{
            "playbackRate":99,
            "audioOffsetMs":-999999,
            "outputLatencyCompensationMs":999999
          },
          "visual":{"scrollSpeedMode":"constant"},
          "performance":{"maxCosmeticBurstsPerFrame":999},
          "keyboard":[{"key":"D","lane":0}],
          "gamepad":[{"button":"paddle1","lane":8}]
        })json";
    }
    const auto loaded = pulseforge::load_settings(settings_path);
    require(static_cast<bool>(loaded), "bounded settings file loads");
    require_near(
        loaded.settings->audio.playback_rate,
        4.0,
        0.0,
        "playback rate uses the documented upper bound"
    );
    require_near(
        loaded.settings->audio.audio_offset_ms,
        -10'000.0,
        0.0,
        "audio offset uses the documented lower bound"
    );
    require_near(
        loaded.settings->audio.output_latency_compensation_ms,
        10'000.0,
        0.0,
        "latency compensation uses the documented upper bound"
    );
    require(
        loaded.settings->visual.scroll_speed_mode
            == pulseforge::ScrollSpeedMode::constant,
        "constant scroll-speed mode loads"
    );
    require(
        loaded.settings->performance.max_cosmetic_bursts_per_frame == 32,
        "cosmetic burst count uses the safe upper bound"
    );
    require(
        loaded.settings->gamepad.size() == 1
            && loaded.settings->gamepad.front().button == "paddle1"
            && loaded.settings->gamepad.front().lane == 8,
        "gamepad binding loads"
    );

    pulseforge::EngineSettings round_trip_settings;
    round_trip_settings.visual.scroll_speed_mode =
        pulseforge::ScrollSpeedMode::constant;
    round_trip_settings.visual.reduced_motion = true;
    // PULSEFORGE_P1_5_0E_MEDIA_THEME_LATENCY_SETTINGS_TEST_V1
    round_trip_settings.visual.theme = pulseforge::PresentationTheme::xbox_360;
    round_trip_settings.visual.audio_visualizer_image =
        pulseforge::AudioVisualizerImage::custom;
    round_trip_settings.visual.audio_visualizer_custom_image_path =
        "C:/PulseForge Media/visualizer.png";
    round_trip_settings.audio.menu_music_selection = "custom";
    round_trip_settings.audio.custom_menu_music_path =
        "C:/PulseForge Media/menu.ogg";
    round_trip_settings.audio.menu_music_muted = true;
    round_trip_settings.performance.max_cosmetic_bursts_per_frame = 17;
    round_trip_settings.performance.auto_pause_on_focus_loss = false;
    round_trip_settings.performance.pause_on_controller_disconnect = false;
    round_trip_settings.performance.ultra_low_latency = true;
    // PULSEFORGE_P1_5_0F_DISCORD_SETTINGS_ROUNDTRIP_TEST_V1
    round_trip_settings.discord.enabled = true;
    round_trip_settings.discord.application_id = 1234567890123456789ULL;
    round_trip_settings.discord.oauth_redirect_uri = "pulseforge:/authorize/callback";
    round_trip_settings.discord.privacy = pulseforge::DiscordPresencePrivacy::reduced;
    round_trip_settings.discord.show_chart_name = false;
    round_trip_settings.discord.show_difficulty_mania = true;
    round_trip_settings.discord.show_progress = true;
    round_trip_settings.discord.show_note_counter = false;
    round_trip_settings.discord.show_gameplay_stats = false;
    round_trip_settings.discord.show_botplay = false;
    round_trip_settings.discord.show_remaining_time = true;
    round_trip_settings.discord.show_mod_name = true;
    round_trip_settings.discord.advanced_customization = true;
    round_trip_settings.discord.activity_name_template = "{engine} / {activity}";
    round_trip_settings.discord.details_template = "{chart.name} • {difficulty}";
    round_trip_settings.discord.state_template = "{accuracy} • {misses} misses";
    round_trip_settings.discord.large_image_template = "{media.art}";
    round_trip_settings.discord.large_url_template = "{media.url}";
    round_trip_settings.discord.button1 = {
        true,
        "Open chart",
        "https://example.test/{chart.name}"
    };
    round_trip_settings.discord.button2 = {
        true,
        "Project",
        "https://example.test/pulseforge"
    };
    round_trip_settings.discord.retry_failed_updates = false;
    round_trip_settings.discord.publish_interval_ms = 4'000U;
    round_trip_settings.gamepad = {{"paddle1", 7}};
    std::string save_error;
    require(
        pulseforge::save_settings(
            settings_path,
            round_trip_settings,
            &save_error
        ),
        save_error
    );
    const auto round_trip = pulseforge::load_settings(settings_path);
    require(static_cast<bool>(round_trip), round_trip.error);
    require(
        round_trip.settings->visual.scroll_speed_mode
            == pulseforge::ScrollSpeedMode::constant
            && round_trip.settings->visual.reduced_motion
            && round_trip.settings->visual.theme
                == pulseforge::PresentationTheme::xbox_360
            && round_trip.settings->visual.audio_visualizer_image
                == pulseforge::AudioVisualizerImage::custom
            && round_trip.settings->visual.audio_visualizer_custom_image_path
                == "C:/PulseForge Media/visualizer.png"
            && round_trip.settings->audio.menu_music_selection == "custom"
            && round_trip.settings->audio.custom_menu_music_path
                == "C:/PulseForge Media/menu.ogg"
            && round_trip.settings->audio.menu_music_muted
            && round_trip.settings->performance.max_cosmetic_bursts_per_frame == 17
            && !round_trip.settings->performance.auto_pause_on_focus_loss
            && !round_trip.settings->performance.pause_on_controller_disconnect
            && round_trip.settings->performance.ultra_low_latency
            && round_trip.settings->discord.enabled
            && round_trip.settings->discord.application_id == 1234567890123456789ULL
            && round_trip.settings->discord.oauth_redirect_uri
                == "pulseforge:/authorize/callback"
            && round_trip.settings->discord.privacy
                == pulseforge::DiscordPresencePrivacy::reduced
            && !round_trip.settings->discord.show_chart_name
            && round_trip.settings->discord.show_difficulty_mania
            && round_trip.settings->discord.show_progress
            && !round_trip.settings->discord.show_note_counter
            && !round_trip.settings->discord.show_gameplay_stats
            && !round_trip.settings->discord.show_botplay
            && round_trip.settings->discord.show_remaining_time
            && round_trip.settings->discord.show_mod_name
            && round_trip.settings->discord.advanced_customization
            && round_trip.settings->discord.activity_name_template
                == "{engine} / {activity}"
            && round_trip.settings->discord.details_template
                == "{chart.name} • {difficulty}"
            && round_trip.settings->discord.state_template
                == "{accuracy} • {misses} misses"
            && round_trip.settings->discord.large_image_template == "{media.art}"
            && round_trip.settings->discord.large_url_template == "{media.url}"
            && round_trip.settings->discord.button1.enabled
            && round_trip.settings->discord.button1.label == "Open chart"
            && round_trip.settings->discord.button1.url
                == "https://example.test/{chart.name}"
            && round_trip.settings->discord.button2.enabled
            && round_trip.settings->discord.button2.label == "Project"
            && !round_trip.settings->discord.retry_failed_updates
            && round_trip.settings->discord.publish_interval_ms == 4'000U
            && round_trip.settings->gamepad.size() == 1
            && round_trip.settings->gamepad.front().button == "paddle1"
            && round_trip.settings->gamepad.front().lane == 7,
        "visual, performance, media, latency, Discord, and gamepad settings survive a save/load round trip"
    );

    pulseforge::EngineSettings performance_profile;
    performance_profile.visual.fps_cap = 360;
    performance_profile.audio.buffer_frames = 512U;
    pulseforge::set_maximum_performance_mode(performance_profile, true);
    require(
        performance_profile.performance.maximum_performance_mode
            && performance_profile.visual.fps_cap == 0
            && performance_profile.audio.buffer_frames == 64U,
        "maximum-performance profile uses uncapped FPS and the 64-frame audio path"
    );
    pulseforge::set_maximum_performance_mode(performance_profile, false);
    require(
        performance_profile.visual.fps_cap == 360
            && performance_profile.audio.buffer_frames == 512U,
        "maximum-performance profile restores the user's prior FPS/audio settings"
    );

    {
        std::ofstream output(settings_path, std::ios::binary | std::ios::trunc);
        output << R"json({"gamepad":"bad"})json";
    }
    const auto malformed_gamepad = pulseforge::load_settings(settings_path);
    require(!malformed_gamepad, "non-array gamepad settings are rejected");

    {
        std::ofstream output(settings_path, std::ios::binary | std::ios::trunc);
        output << R"json({"visual":{"scrollSpeedMode":"warp"}})json";
    }
    const auto malformed_scroll_mode = pulseforge::load_settings(settings_path);
    require(
        !malformed_scroll_mode,
        "unknown scroll-speed modes are rejected"
    );

    {
        std::ofstream output(settings_path, std::ios::binary | std::ios::trunc);
        output << R"json({"discord":{"privacy":"secret"}})json";
    }
    const auto malformed_discord_privacy = pulseforge::load_settings(settings_path);
    require(
        !malformed_discord_privacy,
        "unknown Discord Rich Presence privacy modes are rejected"
    );

    {
        std::ofstream output(settings_path, std::ios::binary | std::ios::trunc);
        output << R"json({"discord":{"publishIntervalMs":999999}})json";
    }
    const auto bounded_discord_interval = pulseforge::load_settings(settings_path);
    require(
        static_cast<bool>(bounded_discord_interval)
            && bounded_discord_interval.settings->discord.publish_interval_ms == 5'000U,
        "Discord Rich Presence publish interval is clamped to the bounded 2-5 second window"
    );

    {
        std::ofstream output(settings_path, std::ios::binary | std::ios::trunc);
        output << R"json({"discord":{"detailsTemplate":")json";
        output << std::string(513U, 'x');
        output << R"json("}})json";
    }
    const auto oversized_discord_template = pulseforge::load_settings(settings_path);
    require(
        !oversized_discord_template,
        "oversized Discord template strings are rejected before reaching the SDK"
    );

    {
        std::ofstream output(settings_path, std::ios::binary | std::ios::trunc);
        output << R"json({"discord":{"button1":{"enabled":true,"label":7,"url":"https://example.test"}}})json";
    }
    const auto malformed_discord_button = pulseforge::load_settings(settings_path);
    require(
        !malformed_discord_button,
        "Discord custom button template fields use a bounded string schema"
    );

    {
        std::ofstream output(settings_path, std::ios::binary | std::ios::trunc);
        output << R"json({"discord":{"oauthRedirectUri":"https://evil.example/callback"}})json";
    }
    const auto unsafe_discord_redirect = pulseforge::load_settings(settings_path);
    require(
        !unsafe_discord_redirect,
        "Discord OAuth redirects reject arbitrary remote callback URLs"
    );

    {
        std::ofstream output(settings_path, std::ios::binary | std::ios::trunc);
        output << R"json({"keyboard":[)json";
        for (std::size_t index = 0; index < 65; ++index) {
            if (index > 0) {
                output << ',';
            }
            output << R"json({"key":"D","lane":0})json";
        }
        output << "]}";
    }
    const auto excessive = pulseforge::load_settings(settings_path);
    std::error_code remove_error;
    std::filesystem::remove(settings_path, remove_error);
    require(!excessive, "excessive key binding count is rejected");
}

void test_dense_chart_scheduler() {
    Chart chart;
    chart.title = "100k benchmark";
    chart.tempos = {{0.0, 240.0, 4, 4}};
    constexpr std::size_t note_count = 100'000;
    chart.notes.reserve(note_count);
    for (std::size_t index = 0; index < note_count; ++index) {
        chart.notes.push_back({
            static_cast<double>(index) * 0.25,
            0.0,
            static_cast<std::uint16_t>(index % 4),
            NoteOwner::player,
            "normal",
        });
    }
    chart.normalize();
    pulseforge::GameplaySettings settings;
    settings.autoplay = true;
    settings.practice = true;
    GameplaySession session(chart, settings);

    const auto start = std::chrono::steady_clock::now();
    session.update(static_cast<double>(note_count) * 0.25 + 1.0);
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start
    ).count();
    require(
        session.summary().marvelous == note_count
            && session.summary().chart_total == note_count,
        "dense chart autoplay judges and counts every note even beyond callback capacity"
    );
    require(
        session.frame_events().size() == 65'536U
            && session.dropped_frame_events() >= note_count - 65'536U,
        "dense callback pressure is bounded and every coalesced event is accounted"
    );
    std::cout << "    dense scheduler: " << elapsed << " ms for "
              << note_count << " notes\n";
}

void test_dense_note_render_coverage() {
    constexpr double viewport_height = 720.0;
    constexpr double receptor_up = 110.0;
    constexpr double receptor_down = 610.0;
    constexpr double speed = 0.43;

    const auto up_edge = pulseforge::note_screen_span(
        (viewport_height - receptor_up) / speed,
        0.0,
        0.0,
        speed,
        false,
        receptor_up
    );
    require(up_edge.has_value(), "upscroll edge geometry is finite");
    require_near(
        up_edge->head_y,
        viewport_height,
        0.000001,
        "upscroll notes enter at the bottom edge"
    );
    require(
        pulseforge::note_intersects_viewport(*up_edge, viewport_height),
        "a clipped upscroll edge note is visible"
    );

    const auto down_edge = pulseforge::note_screen_span(
        receptor_down / speed,
        0.0,
        0.0,
        speed,
        true,
        receptor_down
    );
    require(down_edge.has_value(), "downscroll edge geometry is finite");
    require_near(
        down_edge->head_y,
        0.0,
        0.000001,
        "downscroll notes enter at the top edge"
    );
    require(
        pulseforge::note_intersects_viewport(*down_edge, viewport_height),
        "a clipped downscroll edge note is visible"
    );

    pulseforge::DenseNoteCoverage coverage(4U, viewport_height, 2.0);
    constexpr std::uint64_t dense_note_count = 1'000'000U;
    const pulseforge::NoteScreenSpan coincident{360.0, 360.0, false};
    for (std::uint64_t note = 0U; note < dense_note_count; ++note) {
        require(
            coverage.add(NoteOwner::player, 2U, coincident),
            "every coincident note enters the bounded LOD"
        );
    }
    require(
        coverage.add(
            NoteOwner::opponent,
            1U,
            {-100.0, 800.0, true}
        ),
        "an offscreen sustain head with a visible tail is retained"
    );
    // PULSEFORGE_P1_4_0_DENSE_THREE_OWNER_COVERAGE_TEST_V1
    require(
        coverage.add(
            NoteOwner::secondary_opponent,
            3U,
            {420.0, 420.0, false}
        ),
        "secondary-opponent heads enter an independent dense owner plane"
    );
    coverage.finalize();
    require(
        coverage.represented_note_count() == dense_note_count + 2U,
        "dense LOD accounts for every intersecting note without truncation"
    );
    const auto dense_row = static_cast<std::size_t>(360.0 / 2.0);
    require(
        coverage.cell(NoteOwner::player, 2U, dense_row).head_count
            == dense_note_count,
        "coincident head multiplicity is preserved"
    );
    const auto third_row = static_cast<std::size_t>(420.0 / 2.0);
    require(
        coverage.cell(NoteOwner::secondary_opponent, 3U, third_row).head_count
            == 1U
            && coverage.cell(NoteOwner::opponent, 3U, third_row).head_count == 0U
            && coverage.cell(NoteOwner::player, 3U, third_row).head_count == 0U,
        "dense coverage keeps player4 isolated from both historical owners"
    );
    for (std::size_t row = 0U; row < coverage.row_count(); ++row) {
        require(
            coverage.cell(NoteOwner::opponent, 1U, row).sustain_count == 1U,
            "sustain interval covers every intersecting viewport row"
        );
    }

    pulseforge::DenseNoteCoverage consumed_hold(
        4U,
        viewport_height,
        2.0
    );
    require(
        consumed_hold.add(
            NoteOwner::player,
            0U,
            {receptor_up, 600.0, true},
            false,
            false
        ),
        "a consumed sustain remains represented without its head"
    );
    consumed_hold.finalize();
    const auto receptor_row = static_cast<std::size_t>(receptor_up / 2.0);
    require(
        consumed_hold.cell(NoteOwner::player, 0U, receptor_row).head_count
            == 0U,
        "a consumed head is not drawn at or beyond the receptor"
    );
    require(
        consumed_hold.cell(NoteOwner::player, 0U, receptor_row).sustain_count
            == 1U,
        "the unconsumed sustain starts exactly at the receptor"
    );

    // The saturated renderer stores a 512 px margin around the viewport. A
    // cluster larger than the judgment window must remain representable on
    // both sides of the receptor instead of disappearing at time == now.
    constexpr double margin = 512.0;
    constexpr std::uint64_t beyond_window = 300'000U;
    pulseforge::DenseNoteCoverage crossing(
        4U,
        viewport_height + margin * 2.0,
        2.0
    );
    const auto past_up = pulseforge::note_screen_span(
        -100.0,
        0.0,
        0.0,
        speed,
        false,
        receptor_up + margin
    );
    const auto future_up = pulseforge::note_screen_span(
        100.0,
        0.0,
        0.0,
        speed,
        false,
        receptor_up + margin
    );
    const auto past_down = pulseforge::note_screen_span(
        -100.0,
        0.0,
        0.0,
        speed,
        true,
        receptor_down + margin
    );
    const auto future_down = pulseforge::note_screen_span(
        100.0,
        0.0,
        0.0,
        speed,
        true,
        receptor_down + margin
    );
    require(
        past_up.has_value() && future_up.has_value()
            && past_down.has_value() && future_down.has_value(),
        "dense crossing geometry is finite in both scroll directions"
    );
    require(
        past_up->head_y < receptor_up + margin
            && future_up->head_y > receptor_up + margin
            && past_down->head_y > receptor_down + margin
            && future_down->head_y < receptor_down + margin,
        "past and future density map to opposite receptor sides"
    );
    const auto pending_late_tap = pulseforge::visual_note_span(
        -100.0,
        0.0,
        0.0,
        speed,
        false,
        receptor_up + margin,
        false
    );
    const auto resolved_late_tap = pulseforge::visual_note_span(
        -100.0,
        0.0,
        0.0,
        speed,
        false,
        receptor_up + margin,
        true
    );
    require(
        pending_late_tap.has_value()
            && pending_late_tap->include_head
            && pending_late_tap->span.head_y < receptor_up + margin,
        "a pending late tap remains visible after crossing the receptor"
    );
    require(
        !resolved_late_tap.has_value(),
        "the same tap disappears immediately after judgment resolves it"
    );
    const auto past_up_fragment =
        pulseforge::clip_dense_note_row_to_receptor(
            past_up->head_y - 1.0,
            2.0,
            receptor_up + margin,
            false
        );
    const auto future_up_fragment =
        pulseforge::clip_dense_note_row_to_receptor(
            future_up->head_y - 1.0,
            2.0,
            receptor_up + margin,
            false
        );
    const auto past_down_fragment =
        pulseforge::clip_dense_note_row_to_receptor(
            past_down->head_y - 1.0,
            2.0,
            receptor_down + margin,
            true
        );
    const auto future_down_fragment =
        pulseforge::clip_dense_note_row_to_receptor(
            future_down->head_y - 1.0,
            2.0,
            receptor_down + margin,
            true
        );
    require(
        !past_up_fragment.has_value() && future_up_fragment.has_value()
            && !past_down_fragment.has_value()
            && future_down_fragment.has_value(),
        "dense LOD draws future rows and removes consumed rows in both scroll directions"
    );
    const auto crossing_up = pulseforge::clip_dense_note_row_to_receptor(
        receptor_up + margin - 1.0,
        2.0,
        receptor_up + margin,
        false
    );
    const auto crossing_down = pulseforge::clip_dense_note_row_to_receptor(
        receptor_down + margin - 1.0,
        2.0,
        receptor_down + margin,
        true
    );
    require(
        crossing_up.has_value() && crossing_down.has_value(),
        "a row straddling the receptor retains only its future-side fragment"
    );
    require_near(
        crossing_up->y,
        receptor_up + margin,
        0.000001,
        "upscroll straddling row begins at receptor"
    );
    require_near(
        crossing_up->height,
        1.0,
        0.000001,
        "upscroll straddling row is clipped"
    );
    require_near(
        crossing_down->y,
        receptor_down + margin - 1.0,
        0.000001,
        "downscroll straddling row keeps its future-side origin"
    );
    require_near(
        crossing_down->height,
        1.0,
        0.000001,
        "downscroll straddling row is clipped"
    );
    require(
        !pulseforge::clip_dense_note_row_to_receptor(
             0.0,
             std::numeric_limits<double>::infinity(),
             receptor_up,
             false
         ).has_value(),
        "invalid dense row geometry is rejected"
    );
    require(
        crossing.add_coincident(
            NoteOwner::player,
            0U,
            *past_up,
            beyond_window
        ) && crossing.add_coincident(
            NoteOwner::player,
            1U,
            *future_up,
            beyond_window
        ) && crossing.add_coincident(
            NoteOwner::opponent,
            2U,
            *past_down,
            beyond_window
        ) && crossing.add_coincident(
            NoteOwner::opponent,
            3U,
            *future_down,
            beyond_window
        ),
        "every >window-limit crossing cluster enters fixed-memory LOD"
    );
    require(
        crossing.represented_note_count() == beyond_window * 4U,
        "cross-receptor cache preserves multiplicity while draw-time lifetime clipping removes past rows"
    );
}

void test_media_end_controls_completion() {
    Chart chart;
    chart.tempos = {{0.0, 120.0, 4, 4}};
    chart.notes = {{1'000.0, 500.0, 0, NoteOwner::opponent, "normal"}};
    chart.events = {{2'500.0, "late-event", "", ""}};
    chart.normalize();

    GameplaySession scheduler_only(chart);
    scheduler_only.update(8'000.0);
    require(
        !scheduler_only.complete(),
        "even a clock far beyond the final object waits for AudioTransport end"
    );
    require(
        std::none_of(
            scheduler_only.frame_events().begin(),
            scheduler_only.frame_events().end(),
            [](const pulseforge::GameplayEvent& event) {
                return event.type == pulseforge::GameplayEventType::song_complete;
            }
        ),
        "ordinary scheduler updates never emit song_complete"
    );

    GameplaySession session(chart);
    session.finish_song(2'000.0);
    require(
        session.complete(),
        "authoritative media end completes even when malformed chart events extend past audio"
    );
    require(
        std::count_if(
            session.frame_events().begin(),
            session.frame_events().end(),
            [](const pulseforge::GameplayEvent& event) {
                return event.type == pulseforge::GameplayEventType::song_complete;
            }
        ) == 1,
        "media-end signal emits completion once"
    );

    session.reset();
    session.update(2'000.0);
    require(!session.complete(), "reset restores the media-end latch");
    session.finish_song(2'000.0);
    require(session.complete(), "media-end signal remains valid after reset");

    Chart exact;
    exact.tempos = {{0.0, 120.0, 4, 4}};
    exact.notes = {
        {1'000.0, 0.0, 0, NoteOwner::player, "normal"},
        {2'000.0, 0.0, 1, NoteOwner::player, "normal"},
    };
    exact.normalize();
    pulseforge::GameplaySettings no_fail;
    no_fail.no_fail = true;
    GameplaySession exact_session(exact, no_fail);
    exact_session.press(0U, 1'000.0);
    exact_session.finish_song(2'000.0);
    require(
        exact_session.complete() && exact_session.summary().misses == 1U,
        "a note exactly on the media end is terminally judged without a 180 ms deadlock"
    );
}

void test_coincident_notes_are_all_judged() {
    Chart chart;
    chart.tempos = {{0.0, 120.0, 4, 4}};
    constexpr std::size_t coincident_count = 100'000U;
    chart.notes.reserve(coincident_count);
    for (std::size_t index = 0U; index < coincident_count; ++index) {
        chart.notes.push_back({1'000.0, 0.0, 0, NoteOwner::player, "normal"});
    }
    chart.normalize();

    GameplaySession session(chart);
    session.press(0U, 1'000.0);
    require(
        session.summary().marvelous == coincident_count
            && session.summary().judged_notes
                == static_cast<double>(coincident_count),
        "one physical hit judges every logical note in a coincident dense stack"
    );
    require(
        std::none_of(
            chart.notes.begin(),
            chart.notes.end(),
            [&](const auto& note) {
                const auto index = static_cast<std::size_t>(&note - chart.notes.data());
                return session.note_state(index) == pulseforge::NoteState::ignored;
            }
        ),
        "dense stacked notes are never hidden from score semantics"
    );
}


void test_opponent_sustain_keeps_unconsumed_tail() {
    // PULSEFORGE_P1_3_0_OPPONENT_SUSTAIN_LIFETIME_TEST_V1
    Chart chart;
    chart.key_count = 4U;
    chart.tempos = {{0.0, 120.0, 4, 4}};
    chart.notes = {
        {1'000.0, 600.0, 1U, NoteOwner::opponent, "normal"},
    };
    chart.normalize();

    GameplaySession session(chart);
    session.begin_frame();
    session.update(1'000.0);
    require(
        session.note_state(0U) == pulseforge::NoteState::holding,
        "opponent sustain remains holding after its head is played"
    );
    require(
        std::count_if(
            session.frame_events().begin(),
            session.frame_events().end(),
            [](const auto& event) {
                return event.type
                    == pulseforge::GameplayEventType::opponent_hit;
            }
        ) == 1,
        "opponent sustain emits opponent_hit once at the head"
    );

    // Player input on the same lane must not own/drop the AI sustain.
    session.press(1U, 1'100.0);
    session.release(1U, 1'150.0);
    session.begin_frame();
    session.update(1'300.0);
    require(
        session.note_state(0U) == pulseforge::NoteState::holding,
        "opponent sustain survives matching player-lane input"
    );
    require(
        session.summary().score == 0
            && session.summary().hold_ticks == 0U
            && session.summary().hold_drops == 0U,
        "opponent sustain lifetime has no player scoring side effects"
    );

    session.begin_frame();
    session.update(1'599.0);
    require(
        session.note_state(0U) == pulseforge::NoteState::holding,
        "opponent sustain remains visible until immediately before its tail"
    );
    session.begin_frame();
    session.update(1'600.0);
    require(
        session.note_state(0U) == pulseforge::NoteState::completed,
        "opponent sustain completes only when its tail reaches the receptor"
    );
    require(
        std::none_of(
            session.frame_events().begin(),
            session.frame_events().end(),
            [](const auto& event) {
                return event.type
                    == pulseforge::GameplayEventType::hold_complete;
            }
        ),
        "opponent visual sustain does not emit a player hold_complete callback"
    );
}


void test_secondary_opponent_gameplay_semantics() {
    // PULSEFORGE_P1_4_0_SECONDARY_OPPONENT_GAMEPLAY_TEST_V1
    Chart chart;
    chart.key_count = 4U;
    chart.tempos = {{0.0, 120.0, 4, 4}};
    chart.secondary_opponent_enabled = true;
    chart.opponent_character = "dad";
    chart.secondary_opponent_character = "monster";
    chart.notes = {
        {1'000.0, 0.0, 0U, NoteOwner::opponent, "normal"},
        {1'100.0, 600.0, 2U, NoteOwner::secondary_opponent, "Third Strum"},
    };
    chart.normalize();

    GameplaySession session(chart);
    require(
        session.summary().chart_total == 0U,
        "secondary-opponent Chart Total starts at zero"
    );
    // PULSEFORGE_P1_5_0F_SECONDARY_OPPONENT_CHART_TOTAL_TEST_V1
    require(
        session.apply_event("Change Note Multiplier", "3", "opponent"),
        "P2 multiplier applies to both AI owners"
    );
    session.begin_frame();
    session.update(1'100.0);
    require(
        session.note_state(0U) == pulseforge::NoteState::completed
            && session.note_state(1U) == pulseforge::NoteState::holding,
        "primary and secondary opponents retain independent AI note lifetimes"
    );
    require(
        std::count_if(
            session.frame_events().begin(),
            session.frame_events().end(),
            [](const auto& event) {
                return event.type == pulseforge::GameplayEventType::opponent_hit;
            }
        ) == 2,
        "both AI owners emit the generic opponent_hit gameplay callback"
    );
    require(
        session.summary().chart_total == 6U,
        "primary and secondary opponent heads both contribute the P2 x3 logical count to Chart Total"
    );
    require(
        session.display_lane(1U) == session.display_lane(
            NoteOwner::secondary_opponent, 2U
        ),
        "secondary opponent uses the P2 dynamic-mania lane topology"
    );
    // PULSEFORGE_P1_4_0B_TEST_NODISCARD_EVENT_V1
    static_cast<void>(session.apply_event("Change P2 Mania", "2", "false"));
    require(
        session.display_lane(1U) < 2U,
        "Change P2 Mania remaps Third Strum lanes with the AI-side topology"
    );
    session.begin_frame();
    session.update(1'699.0);
    require(
        session.note_state(1U) == pulseforge::NoteState::holding,
        "secondary-opponent sustain stays alive until its real tail"
    );
    session.begin_frame();
    session.update(1'700.0);
    require(
        session.note_state(1U) == pulseforge::NoteState::completed,
        "secondary-opponent sustain completes at its real tail"
    );
    require(
        session.summary().score == 0
            && session.summary().hold_ticks == 0U
            && session.summary().hold_drops == 0U,
        "secondary-opponent notes do not contaminate player scoring semantics"
    );
}


void test_dynamic_lane_topology_events() {
    // PULSEFORGE_P1_3_0_DYNAMIC_LANE_TOPOLOGY_TEST_V1
    Chart chart;
    chart.key_count = 6U;
    chart.tempos = {{0.0, 120.0, 4, 4}};
    chart.notes = {
        {1'000.0, 500.0, 0U, NoteOwner::player, "normal"},
        {1'100.0, 0.0, 0U, NoteOwner::opponent, "normal"},
    };
    chart.normalize();

    pulseforge::GameplaySettings settings;
    settings.mirror = true;
    GameplaySession session(chart, settings);
    require(
        session.player_key_count() == 6U
            && session.opponent_key_count() == 6U,
        "dynamic mania starts from the chart source-lane domain"
    );

    require(
        session.apply_event("Change P1 Mania", "2", "true"),
        "Change P1 Mania is recognized"
    );
    require(
        session.player_key_count() == 2U
            && session.opponent_key_count() == 6U,
        "P1 mania changes only the player topology"
    );
    // PULSEFORGE_P1_4_0C_COMPLETE_DYNAMIC_LANE_PROJECTION_TEST_V1
    for (std::uint16_t source = 0U; source < chart.key_count; ++source) {
        require(
            session.display_lane(NoteOwner::player, source) < 2U,
            "every player source lane projects inside the shrunken receptor domain"
        );
    }
    require(
        session.display_lane(NoteOwner::player, 5U) == 0U,
        "high player source lanes are projected instead of retaining stale indices"
    );
    require(
        session.apply_event("Change P2 Mania", "3", "false"),
        "Change P2 Mania is recognized"
    );
    require(
        session.player_key_count() == 2U
            && session.opponent_key_count() == 3U,
        "P2 mania changes only the opponent topology"
    );
    require(
        session.apply_event("Change Mania", "4", "true"),
        "generic Change Mania is recognized"
    );
    require(
        session.player_key_count() == 4U
            && session.opponent_key_count() == 4U,
        "generic Change Mania updates both sides"
    );

    static_cast<void>(session.apply_event("Change P1 Mania", "19", "true"));
    static_cast<void>(session.apply_event("Change P2 Mania", "2.5", "true"));
    require(
        session.player_key_count() == 4U
            && session.opponent_key_count() == 4U,
        "invalid dynamic mania counts are bounded no-ops"
    );

    // Reduce to 2K while mirrored. Source lane 0 becomes display lane 1. A
    // sustain already held on the old topology must remain held by source-lane
    // identity instead of becoming an artificial hold drop.
    static_cast<void>(session.apply_event("Change P1 Mania", "6", "false"));
    session.update(1'000.0);
    session.press(5U, 1'000.0);  // mirrored 6K: source 0 -> display 5
    require(
        session.note_state(0U) == pulseforge::NoteState::holding,
        "mirrored source lane is hittable before a mania change"
    );
    static_cast<void>(session.apply_event("Change P1 Mania", "2", "true"));
    require(
        session.display_lane(0U) == 1U && session.lane_held(1U),
        "held source lane is reprojected onto the new player topology"
    );
    session.update(1'300.0);
    require(
        session.summary().hold_drops == 0U,
        "topology change does not synthesize a sustain drop"
    );
    session.release(1U, 1'500.0);
    require(
        session.summary().hold_drops == 0U,
        "release on the remapped lane completes the held sustain"
    );

    Chart same_time_chart;
    same_time_chart.key_count = 6U;
    same_time_chart.tempos = {{0.0, 120.0, 4, 4}};
    same_time_chart.notes = {
        {1'000.0, 0.0, 0U, NoteOwner::player, "normal"},
    };
    same_time_chart.events = {
        {1'000.0, "Change P1 Mania", "2", "true"},
    };
    same_time_chart.normalize();
    GameplaySession same_time(same_time_chart, settings);
    same_time.press(1U, 1'000.0);  // event first: mirrored 2K source 0 -> lane 1
    require(
        same_time.player_key_count() == 2U
            && same_time.note_state(0U) == pulseforge::NoteState::completed,
        "same-time mania event is applied before physical lane judgment"
    );

    // Two distinct 4K source lanes collapse onto receptor 1 in 2K. One
    // physical press must judge both coincident logical notes.
    Chart collapsed_chart;
    collapsed_chart.key_count = 4U;
    collapsed_chart.tempos = {{0.0, 120.0, 4, 4}};
    collapsed_chart.notes = {
        {1'000.0, 0.0, 2U, NoteOwner::player, "normal"},
        {1'000.0, 0.0, 3U, NoteOwner::player, "normal"},
    };
    collapsed_chart.normalize();
    pulseforge::GameplaySettings collapsed_settings;
    collapsed_settings.no_fail = true;
    GameplaySession collapsed(collapsed_chart, collapsed_settings);
    static_cast<void>(collapsed.apply_event("Change P1 Mania", "2", "false"));
    require(
        collapsed.display_lane(NoteOwner::player, 2U) == 1U
            && collapsed.display_lane(NoteOwner::player, 3U) == 1U,
        "4K source lanes 2 and 3 collapse deterministically onto 2K receptor 1"
    );
    collapsed.press(1U, 1'000.0);
    require(
        collapsed.note_state(0U) == pulseforge::NoteState::completed
            && collapsed.note_state(1U) == pulseforge::NoteState::completed
            && collapsed.summary().marvelous == 2U,
        "one collapsed receptor judges coincident notes from all represented source lanes"
    );
}


// PULSEFORGE_P1_5_0_NOTE_KIND_RUNTIME_BEHAVIOR_TEST_V1
void test_note_kind_runtime_behavior() {
    Chart chart;
    chart.tempos = {{0.0, 120.0, 4, 4}};
    chart.notes = {
        {1'000.0, 0.0, 0U, NoteOwner::player, "Custom Heal"},
        {1'500.0, 0.0, 1U, NoteOwner::player, "Custom Ignore"},
        {2'000.0, 0.0, 2U, NoteOwner::player, "Custom Hazard"},
        {2'500.0, 500.0, 3U, NoteOwner::player, "Custom Sustain"},
    };
    chart.normalize();

    pulseforge::GameplaySettings settings;
    settings.no_fail = true;
    GameplaySession session(chart, settings);

    pulseforge::NoteKindRuntimeBehavior heal;
    heal.hit_health = 0.20;
    heal.miss_health = 0.40;
    require(
        session.set_note_kind_behavior("Custom Heal", heal),
        "materialized session accepts a bounded kind behavior"
    );

    pulseforge::NoteKindRuntimeBehavior ignore;
    ignore.ignore_note = true;
    require(
        session.set_note_kind_behavior("Custom Ignore", ignore),
        "materialized session accepts ignoreNote semantics"
    );

    pulseforge::NoteKindRuntimeBehavior hazard;
    hazard.hit_causes_miss = true;
    hazard.miss_health = 0.30;
    require(
        session.set_note_kind_behavior("Custom Hazard", hazard),
        "materialized session accepts hitCausesMiss semantics"
    );

    pulseforge::NoteKindRuntimeBehavior sustain;
    sustain.hit_health = 0.10;
    sustain.miss_health = 0.20;
    sustain.sustain_miss_health = 0.25;
    require(
        session.set_note_kind_behavior("Custom Sustain", sustain),
        "materialized session accepts sustain-specific health semantics"
    );

    session.press(0U, 1'000.0);
    require_near(
        session.summary().health,
        1.20,
        0.000001,
        "custom hitHealth changes physical hit health"
    );

    session.update(1'900.0);
    require(
        session.note_state(1U) == pulseforge::NoteState::ignored
            && session.summary().misses == 0U,
        "ignoreNote retires a missed note without miss statistics"
    );

    session.press(2U, 2'000.0);
    require(
        session.summary().misses == 1U,
        "hitCausesMiss converts a successful press into a hazard miss"
    );
    require_near(
        session.summary().health,
        0.90,
        0.000001,
        "custom hazard uses its missHealth damage"
    );

    session.press(3U, 2'500.0);
    require_near(
        session.summary().health,
        1.00,
        0.000001,
        "custom sustain head uses custom hitHealth"
    );
    session.release(3U, 2'600.0);
    session.update(2'650.0);
    require(
        session.summary().hold_drops == 1U,
        "early release records a custom sustain hold drop"
    );
    require_near(
        session.summary().health,
        0.75,
        0.000001,
        "custom sustain drop uses sustain missHealth"
    );
}


// PULSEFORGE_P1_5_0C_DECLARATIVE_SUSTAIN_POLICY_TEST_V1
void test_declarative_sustain_policy_runtime() {
    Chart chart;
    chart.tempos = {{0.0, 120.0, 4, 4}};
    chart.notes = {
        {1'000.0, 500.0, 0U, NoteOwner::player, "Tap Sustain"},
        {2'000.0, 500.0, 1U, NoteOwner::player, "Head Only Sustain"},
    };
    chart.normalize();

    pulseforge::GameplaySettings settings;
    settings.no_fail = true;
    GameplaySession session(chart, settings);

    pulseforge::NoteKindRuntimeBehavior tap_sustain;
    tap_sustain.hit_health = 0.10;
    tap_sustain.sustain_enabled = false;
    tap_sustain.sustain_hit_causes_miss = true;
    tap_sustain.sustain_miss_health = 0.90;
    require(
        session.set_note_kind_behavior("Tap Sustain", tap_sustain),
        "materialized sustain.enabled behavior installs"
    );

    pulseforge::NoteKindRuntimeBehavior head_only;
    head_only.hit_health = 0.10;
    head_only.miss_health = 0.40;
    head_only.sustain_miss_health = 0.90;
    head_only.hit_causes_miss = false;
    head_only.sustain_hit_causes_miss = true;
    head_only.sustain_inherits_type = false;
    require(
        session.set_note_kind_behavior("Head Only Sustain", head_only),
        "materialized sustain.inheritsType behavior installs"
    );

    session.press(0U, 1'000.0);
    require(
        session.note_state(0U) == pulseforge::NoteState::completed,
        "sustain.enabled=false resolves the source sustain as a tap"
    );
    require_near(
        session.summary().health,
        1.10,
        0.000001,
        "disabled sustain keeps the custom head hitHealth"
    );
    session.release(0U, 1'050.0);
    session.update(1'200.0);
    require(
        session.summary().hold_drops == 0U,
        "disabled sustain never creates a hold to drop"
    );

    session.set_health(1.0);
    session.press(1U, 2'000.0);
    require(
        session.note_state(1U) == pulseforge::NoteState::holding,
        "sustain.inheritsType=false keeps the physical sustain lifetime"
    );
    require_near(
        session.summary().health,
        1.10,
        0.000001,
        "head-only sustain still applies custom head semantics"
    );
    session.release(1U, 2'050.0);
    require(
        session.summary().hold_drops == 1U,
        "head-only sustain can still be dropped physically"
    );
    require_near(
        session.summary().health,
        1.02,
        0.000001,
        "inheritsType=false returns tail/drop health to normal semantics"
    );
}

void test_idle_update_cost_is_independent_of_chart_size() {
    Chart empty_chart;
    empty_chart.tempos = {{0.0, 120.0, 4, 4}};
    empty_chart.normalize();

    Chart dense_chart;
    dense_chart.tempos = {{0.0, 120.0, 4, 4}};
    constexpr std::size_t note_count = 200'000;
    dense_chart.notes.reserve(note_count);
    for (std::size_t index = 0; index < note_count; ++index) {
        dense_chart.notes.push_back({
            1'000'000.0 + static_cast<double>(index) * 0.001,
            0.0,
            static_cast<std::uint16_t>(index % 4),
            NoteOwner::player,
            "normal",
        });
    }
    dense_chart.normalize();

    GameplaySession empty_session(empty_chart);
    GameplaySession dense_session(dense_chart);
    constexpr std::size_t update_count = 10'000;
    const auto measure = [](GameplaySession& session) {
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < update_count; ++index) {
            session.begin_frame();
            session.update((index & 1U) == 0U ? 0.0 : 0.001);
        }
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start
        ).count();
    };

    const double empty_elapsed = measure(empty_session);
    const double dense_elapsed = measure(dense_session);
    // The additive allowance absorbs scheduler/VM noise; a per-update chart
    // scan performs two billion note visits here and exceeds it by orders of
    // magnitude on both optimized and sanitizer builds.
    require(
        dense_elapsed <= empty_elapsed * 25.0 + 500.0,
        "idle update cost is independent of total chart size"
    );
    std::cout << "    idle updates: empty=" << empty_elapsed
              << " ms, 200k=" << dense_elapsed << " ms\n";
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests{
        {"timing map round trip", test_timing_map_round_trip},
        {"native loader", test_native_loader},
        {"Psych loader", test_psych_loader},
        {"Psych event sidecars", test_psych_event_sidecars},
        {"DenpaEx loader", test_denpa_loader},
        {"large Denpa on-demand loader", test_large_denpa_file_matches_dom_loader},
        {"V-Slice loader", test_vslice_loader},
        {"judgment boundaries", test_judgment_boundaries},
        {"sustain frame independence", test_sustain_is_frame_rate_independent},
        {"replay determinism", test_replay_determinism},
        {"replay live input order", test_replay_uses_live_input_order},
        {"invalid chart rejection", test_invalid_chart_is_rejected},
        {"loader edge contracts", test_loader_edge_contracts},
        {"V-Slice contracts", test_vslice_contracts},
        {"strict import contracts", test_strict_import_contracts},
        {"BOTPLAY replay and score saturation", test_botplay_replay_and_score_saturation},
        {"release order and completion", test_release_order_and_completion},
        {"input offset clock isolation", test_input_offset_only_affects_player_input},
        {"generated workload limit", test_generated_workload_limit},
        {"immediate exclusive failure", test_failure_is_immediate_and_exclusive},
        {"replay loader ranges", test_replay_loader_ranges},
        {"settings limits and sanitization", test_settings_limits_and_sanitization},
        {"dense chart scheduler", test_dense_chart_scheduler},
        {"dense note render coverage", test_dense_note_render_coverage},
        {"media-end completion authority", test_media_end_controls_completion},
        {"coincident note judgment", test_coincident_notes_are_all_judged},
        {"opponent sustain visual lifetime", test_opponent_sustain_keeps_unconsumed_tail},
        {"secondary opponent gameplay", test_secondary_opponent_gameplay_semantics},
        {"dynamic lane topology events", test_dynamic_lane_topology_events},
        {"note kind runtime behavior", test_note_kind_runtime_behavior},
        {"declarative sustain policy", test_declarative_sustain_policy_runtime},
        {"idle update cost", test_idle_update_cost_is_independent_of_chart_size},
    };

    std::size_t failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << exception.what() << '\n';
        }
    }

    std::cout << "\n" << (tests.size() - failures) << '/' << tests.size()
              << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
