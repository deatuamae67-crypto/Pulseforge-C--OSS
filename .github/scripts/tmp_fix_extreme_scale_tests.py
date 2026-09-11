from pathlib import Path


def replace(path: str, old: str, new: str, expected: int = 1) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    found = text.count(old)
    if found != expected:
        raise SystemExit(f"{path}: expected {expected} occurrence(s), found {found}")
    p.write_text(text.replace(old, new), encoding="utf-8")

replace(
    "tests/test_main.cpp",
    """void test_generated_workload_limit() {
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
""",
    """void test_generated_workload_scale() {
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
        std::none_of(
            issues.begin(),
            issues.end(),
            [](const pulseforge::ValidationIssue& issue) {
                return issue.severity == pulseforge::ValidationSeverity::error;
            }
        ),
        "generated gameplay workload is not rejected by a chart-wide policy ceiling"
    );
}
""",
)
replace(
    "tests/test_main.cpp",
    '{"generated workload limit", test_generated_workload_limit},',
    '{"generated workload scale", test_generated_workload_scale},',
)

# maximum_chart_notes is now SIZE_MAX. The old +1 dictionary expression wrapped
# to zero and made every saved editor project fail to reopen. The project file is
# already bounded by EditorStorage, and each payload has its own byte validation,
# so there is no need to derive a second dictionary cap from the note total.
replace(
    "src/editor/chart_editor.cpp",
    """        if (!payload_json.is_array()
            || payload_json.size() > maximum_chart_notes + 1U) {
            return validation_failure(
                read_result.path,
                "editor project payload dictionary is not a bounded array"
            );
        }
""",
    """        if (!payload_json.is_array()) {
            return validation_failure(
                read_result.path,
                "editor project payload dictionary is not an array"
            );
        }
""",
)

print("extreme-scale regression fixes applied")
