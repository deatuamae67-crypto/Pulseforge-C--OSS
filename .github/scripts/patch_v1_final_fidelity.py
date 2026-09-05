from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"expected one anchor in {path}, found {count}: {old[:120]!r}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


replace_once(
    "src/app/application.cpp",
    '''    [[nodiscard]] static double script_ease_value(
        const double raw,
        const std::string_view easing
    ) noexcept {
        const double t = std::clamp(raw, 0.0, 1.0);
        if (easing == "linear") return t;
        if (easing == "quadIn") return t * t;
        if (easing == "quadOut") return 1.0 - (1.0 - t) * (1.0 - t);
        if (easing == "quadInOut") return t < 0.5
            ? 2.0 * t * t
            : 1.0 - std::pow(-2.0 * t + 2.0, 2.0) / 2.0;
        if (easing == "cubeIn") return t * t * t;
        if (easing == "cubeOut") return 1.0 - std::pow(1.0 - t, 3.0);
        if (easing == "sineInOut") return -(std::cos(3.14159265358979323846 * t) - 1.0) * 0.5;
        if (easing == "circIn" || easing == "CircIn") {
            return 1.0 - std::sqrt(std::max(0.0, 1.0 - t * t));
        }
        if (easing == "circOut" || easing == "CircOut") {
            const double shifted = t - 1.0;
            return std::sqrt(std::max(0.0, 1.0 - shifted * shifted));
        }
        if (easing == "circInOut" || easing == "CircInOut") {
            if (t < 0.5) {
                const double doubled = 2.0 * t;
                return (1.0 - std::sqrt(
                    std::max(0.0, 1.0 - doubled * doubled)
                )) * 0.5;
            }
            const double shifted = -2.0 * t + 2.0;
            return (std::sqrt(
                std::max(0.0, 1.0 - shifted * shifted)
            ) + 1.0) * 0.5;
        }
        return t;
    }''',
    '''    [[nodiscard]] static double script_ease_value(
        const double raw,
        const std::string_view easing
    ) noexcept {
        // PULSEFORGE_1_0_0_PSYCH_EASING_FIDELITY_V1
        // Psych/FlxEase names in the historical corpus vary in case
        // (quadOut/quadout/quadinOut) and include quart/quint families. Keep
        // lookup allocation-free because this runs once per live tween/frame.
        const auto ease_is = [easing](const std::string_view expected) noexcept {
            if (easing.size() != expected.size()) return false;
            for (std::size_t index = 0U; index < easing.size(); ++index) {
                const auto fold = [](const unsigned char value) noexcept {
                    return value >= 'A' && value <= 'Z'
                        ? static_cast<unsigned char>(value - 'A' + 'a')
                        : value;
                };
                if (fold(static_cast<unsigned char>(easing[index]))
                    != fold(static_cast<unsigned char>(expected[index]))) {
                    return false;
                }
            }
            return true;
        };
        const double t = std::clamp(raw, 0.0, 1.0);
        constexpr double pi = 3.14159265358979323846;
        if (ease_is("linear")) return t;
        if (ease_is("quadIn")) return t * t;
        if (ease_is("quadOut")) return 1.0 - std::pow(1.0 - t, 2.0);
        if (ease_is("quadInOut")) return t < 0.5
            ? 2.0 * t * t
            : 1.0 - std::pow(-2.0 * t + 2.0, 2.0) / 2.0;
        if (ease_is("cubeIn")) return t * t * t;
        if (ease_is("cubeOut")) return 1.0 - std::pow(1.0 - t, 3.0);
        if (ease_is("cubeInOut")) return t < 0.5
            ? 4.0 * t * t * t
            : 1.0 - std::pow(-2.0 * t + 2.0, 3.0) / 2.0;
        if (ease_is("quartIn")) return std::pow(t, 4.0);
        if (ease_is("quartOut")) return 1.0 - std::pow(1.0 - t, 4.0);
        if (ease_is("quartInOut")) return t < 0.5
            ? 8.0 * std::pow(t, 4.0)
            : 1.0 - std::pow(-2.0 * t + 2.0, 4.0) / 2.0;
        if (ease_is("quintIn")) return std::pow(t, 5.0);
        if (ease_is("quintOut")) return 1.0 - std::pow(1.0 - t, 5.0);
        if (ease_is("quintInOut")) return t < 0.5
            ? 16.0 * std::pow(t, 5.0)
            : 1.0 - std::pow(-2.0 * t + 2.0, 5.0) / 2.0;
        if (ease_is("sineIn")) return 1.0 - std::cos((t * pi) * 0.5);
        if (ease_is("sineOut")) return std::sin((t * pi) * 0.5);
        if (ease_is("sineInOut")) return -(std::cos(pi * t) - 1.0) * 0.5;
        if (ease_is("circIn")) {
            return 1.0 - std::sqrt(std::max(0.0, 1.0 - t * t));
        }
        if (ease_is("circOut")) {
            const double shifted = t - 1.0;
            return std::sqrt(std::max(0.0, 1.0 - shifted * shifted));
        }
        if (ease_is("circInOut")) {
            if (t < 0.5) {
                const double doubled = 2.0 * t;
                return (1.0 - std::sqrt(
                    std::max(0.0, 1.0 - doubled * doubled)
                )) * 0.5;
            }
            const double shifted = -2.0 * t + 2.0;
            return (std::sqrt(
                std::max(0.0, 1.0 - shifted * shifted)
            ) + 1.0) * 0.5;
        }
        return t;
    }'''
)

# Public OSS release ships the engine. The historical corpus remains an
# external compatibility/reference corpus unless individual redistribution
# rights are established; avoid claiming that ~15 GB of third-party assets are
# release binaries.
replace_once(
    "docs/MOD_CORPUS_1.0.0.md",
    '''The public Git repository stores a deterministic inventory rather than committing roughly 15.0 GB of static content into Git history. The GitHub 1.0.0 release distributes the runnable content as separately checksummed assets sourced from the public read-only Drive corpus.''',
    '''The public Git repository stores a deterministic inventory rather than committing roughly 15.0 GB of static content into Git history. The historical Drive tree is used as an external compatibility/reference corpus. The public GitHub 1.0.0 release ships the OSS engine and its platform packages; third-party mod payloads are not automatically redistributed by the OSS release unless their redistribution rights are separately established.'''
)
replace_once(
    "mods/README.md",
    '''The definitive release contains the complete 29-mod runnable engine corpus, but the ~15.0 GB payload is intentionally not committed into Git history. See `../docs/MOD_CORPUS_1.0.0.json` for the exact inventory and the GitHub `v1.0.0` release for the checksummed downloadable content archives.''',
    '''PulseForge 1.0.0 is validated against a 29-mod historical compatibility corpus, but the ~15.0 GB third-party payload is intentionally not committed into Git history or automatically republished as OSS release assets. See `../docs/MOD_CORPUS_1.0.0.json` for the exact external-corpus inventory and provenance. Public `v1.0.0` release assets contain the engine/platform packages; mod payload redistribution is separate and license-dependent.'''
)
replace_once(
    "docs/RELEASE_NOTES_1.0.0.md",
    '''- Complete 29-mod runnable engine corpus distributed as checksummed GitHub release content assets; the Drive-root audit adds `taimuresu spam amplified` and `charts feitos no flp` beyond the historical 27-entry `modsList.txt`.''',
    '''- Compatibility validated against a complete 29-mod historical corpus; the Drive-root audit adds `taimuresu spam amplified` and `charts feitos no flp` beyond the historical 27-entry `modsList.txt`, while third-party payload redistribution remains separate from the OSS engine release.'''
)
replace_once(
    "docs/RELEASE_NOTES_1.0.0.md",
    '''- Overkill/Timeless compatibility fixed end-to-end: glitch helpers no longer abort stage setup; `setScrollFactor`/`setLuaSpriteScrollFactor` follow Psych semantics; decimal beat/step plus resolved `mustHitSection` globals drive the modchart; and the script's animation probe runs through bounded literal `string.find` plus an allow-listed `getAnimationName` bridge rather than unrestricted patterns/reflection.''',
    '''- Overkill/Timeless compatibility fixed end-to-end: glitch helpers no longer abort stage setup; `setScrollFactor`/`setLuaSpriteScrollFactor` follow Psych semantics; decimal beat/step plus resolved `mustHitSection` globals drive the modchart; the animation probe runs through bounded literal `string.find` plus an allow-listed `getAnimationName` bridge; and Psych easing names are matched case-insensitively with quad/cube/quart/quint/sine/circ families so the original fades and strum tweens do not silently fall back to linear.'''
)
replace_once(
    "docs/RELEASE_NOTES_1.0.0.md",
    '''The core platform packages and all mod-content packages are built/packaged from the final 1.0.0 source/provenance chain. Android CI output remains test-signed and macOS CI output remains ad-hoc signed unless separate production identities are supplied.''',
    '''The public release builds the core platform packages from the final 1.0.0 source/provenance chain. The external 29-mod corpus is represented by a deterministic compatibility manifest, not automatically bundled into the OSS binaries. Android CI output remains test-signed and macOS CI output remains ad-hoc signed unless separate production identities are supplied.'''
)
replace_once(
    "CHANGELOG.md",
    '''- Distributes the complete 29-mod runnable engine corpus as separately verifiable release assets instead of bloating Git history with approximately 15.0 GB of static content; a Drive-root audit recovered two runnable mods omitted from the historical `modsList.txt`.''',
    '''- Validates against a complete 29-mod historical compatibility corpus without bloating Git history with approximately 15.0 GB of static third-party content; a Drive-root audit recovered two runnable mods omitted from the historical `modsList.txt`. Public OSS release assets contain the engine/platform packages, while third-party mod redistribution remains separate and license-dependent.'''
)
replace_once(
    "CHANGELOG.md",
    '''- Adds Overkill/Timeless Lua compatibility: `addGlitchEffect` receives a bounded renderer-native fallback; `setScrollFactor(tag)` and legacy `setLuaSpriteScrollFactor` follow Psych semantics; fractional `curDecBeat`/`curDecStep` and resolved `mustHitSection` are available; a bounded literal-only `string.find` and allow-listed character `getAnimationName` bridge prevent the original scripts from aborting without exposing general Lua patterns or native reflection.''',
    '''- Adds Overkill/Timeless Lua compatibility: `addGlitchEffect` receives a bounded renderer-native fallback; `setScrollFactor(tag)` and legacy `setLuaSpriteScrollFactor` follow Psych semantics; fractional `curDecBeat`/`curDecStep` and resolved `mustHitSection` are available; a bounded literal-only `string.find` and allow-listed character `getAnimationName` bridge prevent the original scripts from aborting without exposing general Lua patterns or native reflection; Psych easing names are case-insensitive and include quad/cube/quart/quint/sine/circ families for modchart fidelity.'''
)

print("final 1.0.0 fidelity/distribution patch applied")
