#include "pulseforge/note_types.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void test_builtin_semantics() {
    pulseforge::NoteTypeRegistry registry;
    const auto ids = pulseforge::builtin_note_type_ids();
    require(ids.size() == 8U, "all editor-compatible built-ins are exposed");
    require(ids.front() == "normal", "normal is the canonical fallback");
    require(ids.back() == "GF Cross Fade", "JS cross-fade built-in is exposed");

    const auto alt = registry.resolve("Alt Animation");
    require(alt.behavior().animation.suffix == "-alt", "alt animation adds suffix");
    const auto gf = registry.resolve("GF Sing");
    require(
        gf.behavior().animation.target
            == pulseforge::NoteAnimationTarget::girlfriend,
        "GF Sing targets the girlfriend"
    );
    const auto hurt = registry.resolve("Hurt Note");
    require(hurt.behavior().health.hit_causes_miss, "hurt hit causes a miss");
    require(hurt.behavior().health.miss == 0.3, "hurt head uses classic miss health");
    require(
        hurt.behavior().sustain.miss_health == 0.1,
        "hurt sustain has the classic smaller miss-health override"
    );
    // PULSEFORGE_P1_5_0D_BUILTIN_HURT_CORPUS_TEST_V1
    require(
        hurt.behavior().visual.texture_id == "HURTNOTE_assets",
        "hurt note keeps the canonical HURTNOTE_assets texture id"
    );
    require(
        hurt.behavior().feedback.splash_id == "HURTnoteSplashes",
        "hurt note keeps the canonical splash atlas id"
    );
    require(
        hurt.behavior().sustain.hit_causes_miss == std::optional<bool>{true},
        "hurt sustain explicitly preserves hazard-on-hit semantics"
    );
    const auto hey = registry.resolve("Hey!");
    require(
        hey.behavior().animation.cue == pulseforge::NoteAnimationCue::hey,
        "Hey emits the special cue"
    );
    const auto silent = registry.resolve("No Animation");
    require(
        silent.behavior().animation.cue == pulseforge::NoteAnimationCue::none,
        "No Animation suppresses character animation"
    );
    const auto cross_fade = registry.resolve("Cross Fade");
    require(
        cross_fade.behavior().cross_fade
            == pulseforge::NoteCrossFadeTarget::performer,
        "Cross Fade targets the performer"
    );
    const auto gf_cross_fade = registry.resolve("GF Cross Fade");
    require(
        gf_cross_fade.behavior().cross_fade
            == pulseforge::NoteCrossFadeTarget::girlfriend,
        "GF Cross Fade targets the girlfriend"
    );
}

void test_historical_hurt_aliases() {
    pulseforge::NoteTypeRegistry registry;
    constexpr std::string_view aliases[]{
        "hurt",
        "HURT",
        "Hurt Note",
        "  hurt   note  ",
        "hurt-note",
        "HURT_NOTE",
        "mine",
        " MiNe ",
    };
    for (const auto alias : aliases) {
        require(
            pulseforge::builtin_note_type_causes_miss(alias),
            "allocation-free hurt classifier accepts compatibility alias"
        );
        const auto resolved = registry.resolve(alias);
        require(!resolved.used_fallback, "hurt alias resolves to built-in semantics");
        require(
            resolved.behavior().health.hit_causes_miss,
            "hurt alias keeps hit-causes-miss behavior"
        );
        require(resolved.round_trip_id() == alias, "hurt alias spelling round-trips");
    }
    require(
        !pulseforge::builtin_note_type_causes_miss("hurt notes"),
        "near-miss names do not become hurt notes"
    );
    require(
        !pulseforge::builtin_note_type_causes_miss("landmine"),
        "substring mine does not become a hurt note"
    );
}

void test_unknown_round_trip_and_fallback() {
    pulseforge::NoteTypeRegistry registry;
    const std::string unknown{"Mod Author's Exact TYPE_42"};
    const auto resolved = registry.resolve(unknown);
    require(resolved.used_fallback, "unknown type reports normal fallback");
    require(resolved.round_trip_id() == unknown, "unknown spelling round-trips exactly");
    require(resolved.behavior().id == "normal", "unknown type has normal behavior");
    require(!resolved.behavior().health.hit_causes_miss, "normal fallback is safe");
}

void test_whitelisted_parser() {
    constexpr std::string_view source = R"txt(
# Both H-Slice separators are accepted.
health.hit = 0.04
missHealth: 0.25
hitCausesMiss = true
animation.target = gf
animation.suffix: "-glitch"
animation.cue = hey
texture = HURTNOTE_assets
rgb.r = 12
color.g: 34
rgbShader.b = 56
alpha = 0.75
scale: 1.25
splash.enabled = false
splash.id = sparks_red
hitsound.enabled = true
hitsound.id: clang_01
sustain.enabled = true
sustain.inheritsType = false
sustain.missHealth = 0.08
sustain.hitCausesMiss: false
scroll.multiplier = 1.5
crossFade = performer
extraData.comboTier = 7
extraData.isHazard: true
extraData.caption = "bounded text"
extraData.optional = null
)txt";
    const auto parsed = pulseforge::parse_note_type_text("Glitch Mine", source);
    require(parsed.ok(), "complete whitelisted definition parses");
    require(parsed.diagnostics.empty(), "valid definition has no diagnostics");
    const auto& definition = *parsed.definition;
    require(definition.id == "Glitch Mine", "custom ID is retained");
    require(definition.health.hit == 0.04, "hit health parsed");
    require(definition.health.miss == 0.25, "legacy missHealth alias parsed");
    require(definition.health.hit_causes_miss, "legacy hurt property parsed");
    require(
        definition.animation.target == pulseforge::NoteAnimationTarget::girlfriend,
        "GF target parsed"
    );
    require(definition.animation.suffix == "-glitch", "suffix parsed");
    require(
        definition.animation.cue == pulseforge::NoteAnimationCue::hey,
        "animation cue parsed"
    );
    require(definition.visual.texture_id == "HURTNOTE_assets", "texture logical ID parsed");
    require(
        definition.visual.rgb
            == std::optional<std::array<std::uint8_t, 3U>>({12U, 34U, 56U}),
        "RGB aliases produce one typed color"
    );
    require(definition.visual.alpha == 0.75F, "alpha parsed");
    require(definition.visual.scale == 1.25F, "scale parsed");
    require(!definition.feedback.splash_enabled, "splash toggle parsed");
    require(definition.feedback.hitsound_enabled, "hitsound toggle parsed");
    require(!definition.sustain.inherits_type, "sustain inheritance parsed");
    require(definition.sustain.miss_health == 0.08, "sustain health override parsed");
    require(definition.sustain.hit_causes_miss == false, "sustain miss override parsed");
    require(definition.scroll_multiplier == 1.5F, "scroll multiplier parsed");
    require(
        definition.cross_fade == pulseforge::NoteCrossFadeTarget::performer,
        "cross-fade policy parsed"
    );
    require(
        std::get<std::int64_t>(definition.extra_data.at("comboTier")) == 7,
        "integer extraData stays typed"
    );
    require(
        std::get<bool>(definition.extra_data.at("isHazard")),
        "boolean extraData stays typed"
    );
    require(
        std::get<std::string>(definition.extra_data.at("caption")) == "bounded text",
        "string extraData stays typed"
    );
    require(
        std::holds_alternative<std::monostate>(
            definition.extra_data.at("optional")
        ),
        "null extraData stays typed"
    );
}

void test_parser_rejects_reflection_and_paths() {
    constexpr std::string_view dangerous[]{
        "parent.parent.health = 0\n",
        "texture = ../images/secret\n",
        "hitsound.id = C:\\Windows\\sound\n",
        "animation.target = runtime.player.object\n",
        "shader.uniform[0] = 999\n",
        "extraData.bad/key = true\n",
        "alpha = nan\n",
        "scale = 100000\n",
        "hitHealth = 0.1\nhealth.hit = 0.2\n",
    };
    for (const auto source : dangerous) {
        const auto parsed = pulseforge::parse_note_type_text("Unsafe", source);
        require(!parsed, "unsafe or ambiguous definition is rejected atomically");
        require(!parsed.diagnostics.empty(), "rejection includes an in-engine diagnostic");
    }
}

void test_parser_budgets() {
    pulseforge::NoteTypeParseLimits limits;
    limits.maximum_source_bytes = 12U;
    auto parsed = pulseforge::parse_note_type_text(
        "Budgeted", "health.hit = 0.2\n", limits
    );
    require(!parsed, "source byte limit is enforced before parsing");

    limits = {};
    limits.maximum_extra_entries = 1U;
    parsed = pulseforge::parse_note_type_text(
        "Budgeted", "extraData.a=1\nextraData.b=2\n", limits
    );
    require(!parsed, "extraData entry budget is enforced");

    limits = {};
    limits.maximum_diagnostics = 1U;
    parsed = pulseforge::parse_note_type_text(
        "Budgeted", "bad.one=1\nbad.two=2\n", limits
    );
    require(!parsed, "unknown properties fail atomically");
    require(parsed.diagnostics.size() == 1U, "diagnostic storage is bounded");

    limits = {};
    limits.maximum_extra_entries = 33U;
    limits.maximum_extra_total_bytes = 16U * 1'024U;
    std::string expanded;
    for (std::size_t index = 0U; index < 33U; ++index) {
        expanded += "extraData.k" + std::to_string(index) + "="
            + std::to_string(index) + "\n";
    }
    parsed = pulseforge::parse_note_type_text("Expanded", expanded, limits);
    require(
        parsed && parsed.definition->extra_data.size() == 33U,
        "caller-provided extraData budgets remain authoritative"
    );

    pulseforge::NoteTypeRegistry registry;
    std::vector<pulseforge::NoteTypeParseDiagnostic> diagnostics;
    std::string error;
    require(
        registry.register_text(
            "Expanded",
            expanded,
            pulseforge::NoteTypeReplacePolicy::reject_existing,
            &diagnostics,
            &error,
            limits
        ),
        "registry preserves caller-provided extraData budgets"
    );
    require(
        registry.resolve("Expanded").behavior().extra_data.size() == 33U,
        "registered definition retains every validated extraData entry"
    );
}

void test_registry_precedence() {
    pulseforge::NoteTypeRegistry registry;
    std::string error;
    std::vector<pulseforge::NoteTypeParseDiagnostic> diagnostics;
    require(
        registry.register_text(
            "Laser", "texture = laser_note\nhealth.hit = 0.1\n",
            pulseforge::NoteTypeReplacePolicy::reject_existing,
            &diagnostics, &error
        ),
        "validated text registers"
    );
    require(registry.resolve("Laser").behavior().visual.texture_id == "laser_note",
            "registered semantics resolve");
    require(
        !registry.register_text("Laser", "texture = replacement\n"),
        "custom type is not silently replaced"
    );
    require(
        registry.register_text(
            "Laser", "texture = replacement\n",
            pulseforge::NoteTypeReplacePolicy::replace_custom
        ),
        "explicit custom precedence can replace custom data"
    );
    require(registry.resolve("Laser").behavior().visual.texture_id == "replacement",
            "explicit replacement becomes active");
    require(
        !registry.register_text(
            "Hurt Note", "hitCausesMiss = false\n",
            pulseforge::NoteTypeReplacePolicy::replace_custom
        ),
        "built-in semantics cannot be replaced by mod data"
    );
    const auto ids = registry.ids();
    require(ids.back() == "Laser", "custom IDs follow canonical built-in order");
}

}  // namespace

int main() {
    try {
        test_builtin_semantics();
        test_historical_hurt_aliases();
        test_unknown_round_trip_and_fallback();
        test_whitelisted_parser();
        test_parser_rejects_reflection_and_paths();
        test_parser_budgets();
        test_registry_precedence();
        std::cout << "Note type tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        std::cerr << "Note type tests failed: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
