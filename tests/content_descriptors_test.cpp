#include "pulseforge/content_descriptors.hpp"

#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Descriptor>
[[nodiscard]] bool has_diagnostic(
    const pulseforge::DescriptorParseResult<Descriptor>& result,
    const pulseforge::DescriptorDiagnosticCode code,
    const pulseforge::DescriptorDiagnosticSeverity severity
) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.code == code && diagnostic.severity == severity) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] pulseforge::DescriptorParseOptions strict_options() {
    pulseforge::DescriptorParseOptions options;
    options.mode = pulseforge::DescriptorParseMode::strict;
    return options;
}

void test_vslice_song() {
    constexpr std::string_view metadata = R"json(
    {
      "version": "2.2.4",
      "songName": "Universal Test",
      "artist": "Pulse Artist",
      "charter": "Pulse Charter",
      "timeChanges": [{"t": 0, "bpm": 120}],
      "offsets": {
        "instrumental": -12.5,
        "altInstrumentals": {"pico": 4.0},
        "vocals": {"bf": 8.0, "dad": -3.0},
        "altVocals": {"erect": {"bf": 2.0}},
        "futureOffsetMode": "sample-accurate"
      },
      "playData": {
        "difficulties": ["easy", "normal", "hard"],
        "songVariations": ["erect", "pico"],
        "stage": "mainStage",
        "noteStyle": "funkin",
        "characters": {
          "player": "bf",
          "girlfriend": "gf",
          "opponent": "dad",
          "instrumental": "",
          "altInstrumentals": ["pico"],
          "playerVocals": ["bf"],
          "opponentVocals": ["dad"],
          "futureCharacterField": true
        },
        "previewStart": 0.1,
        "previewEnd": 0.5,
        "ratings": {"easy": 1, "normal": 3, "hard": 5},
        "album": "volume1"
      }
    })json";

    const auto result = pulseforge::ContentDescriptorParser::parse_vslice_song(
        metadata,
        "universal-test",
        strict_options()
    );
    require(
        static_cast<bool>(result),
        "strict current V-Slice song metadata parses"
    );
    require(result.diagnostics.empty(), "valid V-Slice song has no diagnostics");
    require(result.value->name == "Universal Test", "song name is retained");
    require(result.value->artist == "Pulse Artist", "artist is retained");
    require(result.value->difficulties.size() == 3U, "difficulties parse");
    require(result.value->variations.size() == 2U, "variations parse");
    require(result.value->stage == "mainStage", "stage id parses");
    require(result.value->note_style == "funkin", "note style parses");
    require(result.value->characters.player == "bf", "player id parses");
    require(
        result.value->characters.alternate_instrumentals.size() == 1U,
        "alternate instrumental ids parse"
    );
    require(
        result.value->preview_start == 0.1
            && result.value->preview_end == 0.5,
        "preview window parses"
    );
    require(
        std::abs(result.value->offsets.instrumental + 12.5) < 0.0001,
        "instrumental offset parses"
    );
    require(result.value->offsets.vocals.size() == 2U, "vocal offsets parse");
    require(
        result.value->extensions_json.find("ratings") != std::string::npos
            && result.value->extensions_json.find("timeChanges")
                != std::string::npos,
        "unmodelled current and future metadata is preserved"
    );
}

void test_vslice_level() {
    constexpr std::string_view metadata = R"json({
      "version":"1.0.2",
      "name":"Hidden Collection",
      "visible":false,
      "songs":["song-a","song-b"],
      "background":"#112233",
      "titleAsset":"story/title",
      "props":[]
    })json";
    const auto result = pulseforge::ContentDescriptorParser::parse_vslice_level(
        metadata,
        "hidden-collection",
        strict_options()
    );
    require(static_cast<bool>(result), "strict current V-Slice level parses");
    require(!result.value->visible, "level visibility parses");
    require(result.value->songs.size() == 2U, "level songs parse");
    require(result.value->background.has_value(), "level background parses");
    require(
        result.value->background->color == "#112233",
        "level background color parses"
    );
    require(
        result.value->extensions_json.find("titleAsset") != std::string::npos
            && result.value->extensions_json.find("props") != std::string::npos,
        "unmodelled level data remains available"
    );
}

void test_psych_week() {
    constexpr std::string_view metadata = R"json({
      "songs":[
        ["Bopeebo","dad",[146,113,253]],
        ["Fresh","dad",[10,20,30]]
      ],
      "weekCharacters":["dad","bf","gf"],
      "weekBackground":"stage",
      "weekBefore":"tutorial",
      "storyName":"Daddy Dearest",
      "weekName":"Week 1",
      "startUnlocked":false,
      "hiddenUntilUnlocked":true,
      "hideStoryMode":false,
      "hideFreeplay":true,
      "difficulties":"Easy, Normal, Hard",
      "futureWeekField":{"ranked":true}
    })json";
    const auto result = pulseforge::ContentDescriptorParser::parse_psych_week(
        metadata,
        "week1",
        strict_options()
    );
    require(static_cast<bool>(result), "strict Psych week schema parses");
    require(result.value->songs.size() == 2U, "Psych week songs parse");
    require(result.value->songs[1].color.green == 20, "song RGB parses");
    require(result.value->characters[0] == "dad", "week characters parse");
    require(!result.value->start_unlocked, "week start lock parses");
    require(result.value->hidden_until_unlocked, "hidden lock parses");
    require(result.value->hide_freeplay, "freeplay visibility parses");
    require(result.value->difficulties.size() == 3U, "difficulty CSV parses");
    require(result.value->difficulties[1] == "Normal", "CSV is trimmed");
    require(
        result.value->extensions_json.find("futureWeekField")
            != std::string::npos,
        "week extensions are preserved"
    );
}

void test_psych_stage() {
    constexpr std::string_view metadata = R"json({
      "directory":"shared",
      "defaultZoom":0.85,
      "isPixelStage":false,
      "stageUI":"normal",
      "boyfriend":[770,100],
      "girlfriend":[400,130],
      "opponent":[100,100],
      "p4":[-260,140],
      "hide_girlfriend":false,
      "camera_boyfriend":[0,-10],
      "camera_girlfriend":[0,0],
      "camera_opponent":[20,30],
      "camera_p4":[-40,25],
      "camera_speed":1.25,
      "preload":{"images":["stages/bg"]},
      "objects":[{
        "type":"animatedSprite",
        "name":"background",
        "image":"stages/bg",
        "x":-200,
        "y":-100,
        "width":640,
        "height":360,
        "scale":[1.2,1.2],
        "scroll":[0.9,0.9],
        "alpha":0.8,
        "angle":2,
        "color":"AABBCC",
        "antialiasing":true,
        "flipX":false,
        "flipY":true,
        "foreground":true,
        "screenSpace":true,
        "blend":"add",
        "filters":3,
        "firstAnimation":"idle",
        "animations":[{
          "anim":"idle",
          "name":"BG idle",
          "fps":24,
          "loop":true,
          "indices":[0,1,2],
          "offsets":[4,-2],
          "frameTags":"future"
        }],
        "shader":"crt"
      }],
      "_editorMeta":{"gf":"gf"}
    })json";
    const auto result = pulseforge::ContentDescriptorParser::parse_psych_stage(
        metadata,
        "stage",
        strict_options()
    );
    require(static_cast<bool>(result), "strict Psych stage schema parses");
    require(std::abs(result.value->default_zoom - 0.85) < 0.0001, "zoom parses");
    // PULSEFORGE_P1_4_0_DENPA_STAGE_P4_TEST_V1
    require(
        result.value->p4.x == -260.0 && result.value->p4.y == 140.0,
        "Denpa player4 stage anchor parses"
    );
    require(
        result.value->camera_p4.x == -40.0
            && result.value->camera_p4.y == 25.0,
        "Denpa player4 camera offset parses"
    );
    require(result.value->objects.size() == 1U, "stage objects parse");
    const auto& object = result.value->objects.front();
    require(object.type == "animatedSprite", "stage object type parses");
    require(object.width == 640.0 && object.height == 360.0,
            "stage object geometry parses");
    require(object.foreground && object.screen_space && object.blend == "add",
            "stage object layer/camera/blend parses");
    require(object.layer == pulseforge::StageObjectLayer::foreground,
            "legacy foreground maps to the explicit stage layer");
    require(object.animations.size() == 1U, "stage animations parse");
    require(object.animations[0].indices.size() == 3U, "indices parse");
    require(
        object.extensions_json.find("shader") != std::string::npos,
        "unknown stage object fields are canonicalized"
    );
    require(
        object.animations[0].extensions_json.find("frameTags")
            != std::string::npos,
        "unknown animation fields are canonicalized"
    );
    require(
        result.value->preload_json.find("images") != std::string::npos,
        "dynamic preload data is retained"
    );
    require(
        result.value->extensions_json.find("_editorMeta")
            != std::string::npos,
        "stage editor extensions are retained"
    );
}

void test_psych_character() {
    constexpr std::string_view metadata = R"json({
      "animations":[{
        "anim":"idle",
        "name":"BF idle dance",
        "fps":24,
        "loop":false,
        "indices":[],
        "offsets":[-5,0],
        "atlasLayer":"body"
      }],
      "image":"characters/BOYFRIEND",
      "scale":1,
      "sing_duration":4,
      "healthicon":"bf",
      "position":[0,350],
      "camera_position":[0,0],
      "flip_x":true,
      "no_antialiasing":false,
      "healthbar_colors":[49,176,209],
      "vocals_file":"Voices-bf",
      "_editor_isPlayer":true,
      "futureRenderer":"multisparrow"
    })json";
    const auto result =
        pulseforge::ContentDescriptorParser::parse_psych_character(
            metadata,
            "bf",
            strict_options()
        );
    require(static_cast<bool>(result), "strict Psych character schema parses");
    require(result.value->image == "characters/BOYFRIEND", "image parses");
    require(result.value->position.y == 350.0, "position parses");
    require(result.value->healthbar_color.green == 176, "health color parses");
    require(result.value->editor_is_player == true, "editor flag parses");
    require(result.value->animations.size() == 1U, "animations parse");
    require(
        result.value->animations[0].offsets.x == -5.0,
        "animation offsets parse"
    );
    require(
        result.value->extensions_json.find("futureRenderer")
            != std::string::npos,
        "character root extensions are retained"
    );
}

void test_permissive_and_strict_modes() {
    constexpr std::string_view legacy = R"json({
      // TJSON-style comments are accepted only in permissive mode.
      "songs":[["Legacy","dad",[300,-2,128]]],
      "weekCharacters":["dad","bf","gf"],
      "weekBackground":"stage",
      "weekBefore":"",
      "storyName":"Legacy",
      "weekName":"Legacy Week",
      "startUnlocked":1,
      "hiddenUntilUnlocked":0,
      "hideStory":1,
      "hideFreeplay":0,
      "difficulties":["Normal","Insane"]
    })json";

    const auto permissive =
        pulseforge::ContentDescriptorParser::parse_psych_week(
            legacy,
            "legacy"
        );
    require(
        static_cast<bool>(permissive),
        "permissive mode accepts bounded legacy shapes"
    );
    require(!permissive.diagnostics.empty(), "coercions emit diagnostics");
    require(permissive.value->hide_story, "legacy hideStory alias maps");
    require(
        permissive.value->songs[0].color.red == 255
            && permissive.value->songs[0].color.green == 0,
        "permissive colors are bounded"
    );
    require(
        permissive.value->difficulties.size() == 2U,
        "permissive array difficulties map"
    );

    const auto strict = pulseforge::ContentDescriptorParser::parse_psych_week(
        legacy,
        "legacy",
        strict_options()
    );
    require(!strict, "strict mode rejects comments and legacy coercions");
    require(
        has_diagnostic(
            strict,
            pulseforge::DescriptorDiagnosticCode::invalid_json,
            pulseforge::DescriptorDiagnosticSeverity::error
        ),
        "strict comment rejection is diagnosed"
    );

    constexpr std::string_view legacy_without_comments = R"json({
      "songs":[["Legacy","dad",[146,113,253]]],
      "weekCharacters":["dad","bf","gf"],
      "weekBackground":"stage",
      "weekBefore":"",
      "storyName":"Legacy",
      "weekName":"Legacy Week",
      "startUnlocked":1,
      "hiddenUntilUnlocked":0,
      "hideStory":1,
      "hideFreeplay":0,
      "difficulties":["Normal","Insane"]
    })json";
    const auto strict_types =
        pulseforge::ContentDescriptorParser::parse_psych_week(
            legacy_without_comments,
            "legacy",
            strict_options()
        );
    require(!strict_types, "strict mode rejects legacy field types and aliases");
    require(
        has_diagnostic(
            strict_types,
            pulseforge::DescriptorDiagnosticCode::wrong_type,
            pulseforge::DescriptorDiagnosticSeverity::error
        ),
        "strict type rejection is diagnosed"
    );
}

void test_limits_and_diagnostics() {
    constexpr std::string_view level = R"json({
      "version":"1.0.2",
      "name":"Level",
      "songs":["a"],
      "future":"a deliberately large preserved extension"
    })json";

    pulseforge::DescriptorParseOptions input_limit;
    input_limit.limits.max_input_bytes = 8U;
    const auto oversized =
        pulseforge::ContentDescriptorParser::parse_vslice_level(
            level,
            "level",
            input_limit
        );
    require(!oversized, "input byte cap rejects metadata before DOM parsing");
    require(
        has_diagnostic(
            oversized,
            pulseforge::DescriptorDiagnosticCode::input_too_large,
            pulseforge::DescriptorDiagnosticSeverity::error
        ),
        "input limit has a typed diagnostic"
    );

    pulseforge::DescriptorParseOptions array_limit;
    array_limit.limits.max_array_items = 2U;
    constexpr std::string_view large_array = R"json({
      "version":"1.0.2","name":"Level","songs":["a","b","c"]
    })json";
    const auto excessive_array =
        pulseforge::ContentDescriptorParser::parse_vslice_level(
            large_array,
            "level",
            array_limit
        );
    require(!excessive_array, "array item cap applies to every JSON array");
    require(
        has_diagnostic(
            excessive_array,
            pulseforge::DescriptorDiagnosticCode::array_too_large,
            pulseforge::DescriptorDiagnosticSeverity::error
        ),
        "array limit has a typed diagnostic"
    );

    pulseforge::DescriptorParseOptions string_limit;
    string_limit.limits.max_string_bytes = 4U;
    const auto excessive_string =
        pulseforge::ContentDescriptorParser::parse_vslice_level(
            level,
            "id",
            string_limit
        );
    require(!excessive_string, "string cap applies to all parsed strings");
    require(
        has_diagnostic(
            excessive_string,
            pulseforge::DescriptorDiagnosticCode::string_too_long,
            pulseforge::DescriptorDiagnosticSeverity::error
        ),
        "string limit has a typed diagnostic"
    );

    pulseforge::DescriptorParseOptions depth_limit;
    depth_limit.limits.max_depth = 2U;
    const auto excessive_depth =
        pulseforge::ContentDescriptorParser::parse_vslice_level(
            R"json({"a":{"b":{"c":1}}})json",
            "level",
            depth_limit
        );
    require(!excessive_depth, "depth is rejected before DOM construction");
    require(
        has_diagnostic(
            excessive_depth,
            pulseforge::DescriptorDiagnosticCode::nesting_too_deep,
            pulseforge::DescriptorDiagnosticSeverity::error
        ),
        "depth limit has a typed diagnostic"
    );

    pulseforge::DescriptorParseOptions preserve_permissive;
    preserve_permissive.limits.max_preserved_json_bytes = 8U;
    const auto dropped_extension =
        pulseforge::ContentDescriptorParser::parse_vslice_level(
            level,
            "level",
            preserve_permissive
        );
    require(
        static_cast<bool>(dropped_extension),
        "permissive mode can omit oversized extensions"
    );
    require(dropped_extension.value->extensions_json.empty(), "extension omitted");
    require(
        has_diagnostic(
            dropped_extension,
            pulseforge::DescriptorDiagnosticCode::preserved_json_too_large,
            pulseforge::DescriptorDiagnosticSeverity::warning
        ),
        "dropped extension emits a warning"
    );

    auto preserve_strict = preserve_permissive;
    preserve_strict.mode = pulseforge::DescriptorParseMode::strict;
    const auto rejected_extension =
        pulseforge::ContentDescriptorParser::parse_vslice_level(
            level,
            "level",
            preserve_strict
        );
    require(!rejected_extension, "strict mode rejects lost extension data");

    pulseforge::DescriptorParseOptions diagnostic_limit = strict_options();
    diagnostic_limit.limits.max_diagnostics = 1U;
    const auto many_errors =
        pulseforge::ContentDescriptorParser::parse_psych_character(
            "{}",
            "missing",
            diagnostic_limit
        );
    require(!many_errors, "strict missing fields fail");
    require(many_errors.diagnostics.size() == 1U, "diagnostics are bounded");
    require(many_errors.diagnostics_truncated, "diagnostic truncation is reported");

    pulseforge::DescriptorParseOptions invalid_limits;
    invalid_limits.limits.max_array_items = 0U;
    const auto invalid = pulseforge::ContentDescriptorParser::parse_vslice_level(
        "{}",
        "level",
        invalid_limits
    );
    require(!invalid, "zero parser limits are rejected");
    require(
        has_diagnostic(
            invalid,
            pulseforge::DescriptorDiagnosticCode::invalid_limits,
            pulseforge::DescriptorDiagnosticSeverity::error
        ),
        "invalid limits have a typed diagnostic"
    );

    const auto wrong_root = pulseforge::ContentDescriptorParser::parse_vslice_level(
        "[]",
        "level"
    );
    require(!wrong_root, "non-object descriptor roots are rejected");
    require(
        has_diagnostic(
            wrong_root,
            pulseforge::DescriptorDiagnosticCode::root_not_object,
            pulseforge::DescriptorDiagnosticSeverity::error
        ),
        "root mismatch has a typed diagnostic"
    );
}

}  // namespace

int main() {
    try {
        test_vslice_song();
        test_vslice_level();
        test_psych_week();
        test_psych_stage();
        test_psych_character();
        test_permissive_and_strict_modes();
        test_limits_and_diagnostics();
        std::cout << "Content descriptor tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "Content descriptor tests failed: "
                  << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
