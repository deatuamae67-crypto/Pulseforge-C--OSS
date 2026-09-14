#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools/chart_merge/merge_split_charts.py"

spec = importlib.util.spec_from_file_location("pulseforge_split_chart_merger", MODULE_PATH)
assert spec is not None and spec.loader is not None
module = importlib.util.module_from_spec(spec)
# Python 3.12 dataclasses resolve postponed annotations through sys.modules.
# Register the dynamically loaded module before executing it, exactly as the
# regular import machinery would.
sys.modules[spec.name] = module
spec.loader.exec_module(module)


def chart(path: Path, notes: list[list[object]], second: list[list[object]] | None = None) -> None:
    payload = {
        "song": {
            "song": "Synthetic Split",
            "bpm": 120,
            "notes": [
                {"lengthInSteps": 16, "mustHitSection": False, "sectionNotes": notes},
                {"lengthInSteps": 16, "mustHitSection": True, "sectionNotes": second or []},
            ],
        }
    }
    path.write_text(json.dumps(payload, separators=(",", ":")), encoding="utf-8")


def times(path: Path) -> list[list[float]]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    return [
        [float(note[0]) for note in section["sectionNotes"]]
        for section in payload["song"]["notes"]
    ]


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="pulseforge-chart-merge-test-") as raw:
        root = Path(raw)
        part1 = root / "FLP 6 part 1 chart.json"
        part2 = root / "FLP 6 part 2 chart.json"
        part10 = root / "FLP 6 part 10 chart.json"
        chart(part1, [[100, 0, 0], [400, 1, 0]], [[50, 2, 0]])
        chart(part2, [[200, 2, 0]], [[150, 3, 0]])
        chart(part10, [[300, 3, 0]], [[250, 0, 0]])

        discovered = module._discover_inputs([str(root)])
        assert [item.name for item in discovered] == [
            part1.name,
            part2.name,
            part10.name,
        ]

        merged = root / "merged.json"
        sections, notes, template = module.merge_charts(
            discovered, merged, "kway", 4, None
        )
        assert sections == 2
        assert notes == 7
        assert template in discovered
        assert times(merged) == [[100.0, 200.0, 300.0, 400.0], [50.0, 150.0, 250.0]]

        unsorted = root / "unsorted.json"
        chart(unsorted, [[500, 0, 0], [25, 1, 0]])
        other = root / "other.json"
        chart(other, [[75, 2, 0]])
        failed = False
        try:
            module.merge_charts([unsorted, other], root / "must-fail.json", "kway", 4, None)
        except module.MergeError:
            failed = True
        assert failed

        external = root / "external.json"
        module.merge_charts([unsorted, other], external, "external-sort", 1, None)
        assert times(external)[0] == [25.0, 75.0, 500.0]

        default_root = root / "mods/pulseforge-created"
        exit_code = module.main([
            str(part1),
            str(part2),
            "--mod-root",
            str(default_root),
            "--name",
            "Sonorous merged",
        ])
        assert exit_code == 0
        assert (default_root / "charts/Sonorous merged.json").is_file()

    print("Split chart merger self-test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
