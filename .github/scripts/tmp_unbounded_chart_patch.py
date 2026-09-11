from pathlib import Path


def replace(path: str, old: str, new: str, expected: int | None = None) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    found = text.count(old)
    if expected is not None and found != expected:
        raise SystemExit(
            f"{path}: expected {expected} occurrence(s), found {found}: {old!r}"
        )
    if found == 0:
        raise SystemExit(f"{path}: replacement anchor not found: {old!r}")
    p.write_text(text.replace(old, new), encoding="utf-8")


def replace_if_present(path: str, old: str, new: str) -> int:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    found = text.count(old)
    if found:
        p.write_text(text.replace(old, new), encoding="utf-8")
    return found


# Remove the historical 5,000,000-note and 512 MiB values as chart-validity
# ceilings. Keep the exact same numbers as materialized-loader routing budgets,
# because the fast DOM/vector path is not how billion/trillion-note charts are
# supposed to be represented. PFC1/PatternRun remains the scalable path.
replace(
    "include/pulseforge/chart.hpp",
    "#include <filesystem>\n#include <string>",
    "#include <filesystem>\n#include <limits>\n#include <string>",
    1,
)
replace(
    "include/pulseforge/chart.hpp",
    """// Shared loader/model limits. Parsers enforce these while consuming input so
// malformed or adversarial files cannot build an oversized intermediate model
// before Chart validation gets a chance to run.
inline constexpr std::size_t maximum_chart_tempo_changes = 100'000;
// These values bound only the fully materialized Chart representation. They are
// NOT engine-wide chart-size limits: application/launcher route charts beyond
// this fast-path budget to the bounded PFC1 streaming architecture instead.
// PatternRun is the constant-storage representation for huge repetitive runs.
inline constexpr std::size_t maximum_chart_notes = 5'000'000;
inline constexpr std::size_t maximum_chart_events = 250'000;
inline constexpr std::uint64_t maximum_chart_json_bytes =
    512ULL * 1024ULL * 1024ULL;
""",
    """// Shared materialized-model limits that are unrelated to note cardinality.
// Tempo/event metadata is still materialized today, so those independent bounds
// remain until those timelines are streamed too.
inline constexpr std::size_t maximum_chart_tempo_changes = 100'000;
inline constexpr std::size_t maximum_chart_events = 250'000;

// There is no product-policy ceiling on total chart notes or JSON source bytes.
// The scalable PFC1 path uses 64-bit counters and PatternRun can represent
// millions, billions or trillions of logical notes without expanding them.
inline constexpr std::size_t maximum_chart_notes =
    std::numeric_limits<std::size_t>::max();
inline constexpr std::uint64_t maximum_chart_json_bytes =
    std::numeric_limits<std::uint64_t>::max();

// These are routing budgets, NOT chart validity limits. Crossing either budget
// selects the bounded PFC1 streaming architecture instead of materializing the
// whole source/model in RAM. This distinction is critical on Android.
inline constexpr std::size_t materialized_chart_note_budget = 5'000'000;
inline constexpr std::uint64_t materialized_chart_json_budget =
    512ULL * 1024ULL * 1024ULL;
""",
    1,
)

# The materialized parsers retain their old budgets solely so they can decline
# and let application/launcher fall through to streaming. No PFC1 total-note or
# total-source limit is introduced here.
replacements = 0
for path in ("src/io/chart_loader.cpp", "src/io/fast_psych_loader.cpp"):
    replacements += replace_if_present(
        path, "maximum_chart_json_bytes", "materialized_chart_json_budget"
    )
    replacements += replace_if_present(
        path, "maximum_chart_notes", "materialized_chart_note_budget"
    )
if replacements == 0:
    raise SystemExit("materialized loader routing anchors were not found")

replace(
    "src/app/launcher.cpp",
    "source_bytes > maximum_chart_json_bytes",
    "source_bytes > materialized_chart_json_budget",
    1,
)

# The 2,000,000 generated-gameplay-event estimate was another chart-wide policy
# ceiling that could reject a dense but otherwise valid materialized chart. The
# actual streaming scheduler remains bounded per frame/window/update.
replace(
    "src/core/chart.cpp",
    "constexpr std::size_t maximum_generated_gameplay_events = 2'000'000;",
    "constexpr std::size_t maximum_generated_gameplay_events =\n    std::numeric_limits<std::size_t>::max();",
    1,
)

# The bridge inherits the now allocator-limited note total by default. No change
# is required to PFC1's total logical/explicit note limits: they are already
# uint64_t max. Keep chunk/window/query budgets finite because they are working
# sets, not chart-size ceilings.

# Document what was actually removed and what intentionally remains bounded.
p = Path("CHANGELOG.md")
text = p.read_text(encoding="utf-8")
marker = "All notable public PulseForge changes are documented in this file.\n\n"
if marker not in text:
    raise SystemExit("CHANGELOG.md insertion point not found")
entry = """## [Unreleased]

### Extreme-scale charts

- Removes the historical 5,000,000-note and 512 MiB values as chart-validity ceilings; total note/source capacity now follows the host/PFC1 representation instead of a product-policy constant.
- Retains those former values only as materialized-loader routing budgets. Crossing them selects bounded PFC1 streaming rather than rejecting the chart, preserving Android memory behavior while allowing extreme-scale sources.
- Removes the historical 2,000,000 generated-gameplay-event validation ceiling so dense materialized charts are not rejected solely by an estimated event count.
- Keeps finite per-chunk, per-window, per-query and per-frame streaming working sets, plus arithmetic/format validation. These bound RAM/CPU work at one time and do not cap the total logical note count.
- Existing PFC1 `PatternRun` coverage continues to validate a 1,000,000,000,000-note chart in constant-size storage.

"""
if "### Extreme-scale charts" not in text:
    p.write_text(text.replace(marker, marker + entry, 1), encoding="utf-8")

# Sanity checks make the intended contract explicit and stop the temporary
# workflow before committing if a future source change makes the patch partial.
chart_h = Path("include/pulseforge/chart.hpp").read_text(encoding="utf-8")
for needle in (
    "maximum_chart_notes =\n    std::numeric_limits<std::size_t>::max()",
    "maximum_chart_json_bytes =\n    std::numeric_limits<std::uint64_t>::max()",
    "materialized_chart_note_budget = 5'000'000",
    "materialized_chart_json_budget =",
):
    if needle not in chart_h:
        raise SystemExit(f"missing extreme-scale chart invariant: {needle}")

fast = Path("src/io/fast_psych_loader.cpp").read_text(encoding="utf-8")
if "maximum_chart_notes" in fast or "maximum_chart_json_bytes" in fast:
    raise SystemExit("fast Psych loader still treats a chart-wide maximum as a materialized budget")

launcher = Path("src/app/launcher.cpp").read_text(encoding="utf-8")
if "source_bytes > maximum_chart_json_bytes" in launcher:
    raise SystemExit("launcher still treats maximum source size as the streaming route threshold")

print("extreme-scale chart patch applied")
