#!/usr/bin/env python3
"""Conservatively complete a PulseForge Complete mod from verified sibling mods.

The recovery step never guesses by filename alone. It only borrows runtime
components when the incomplete target can be tied to a sibling mod by either:

* an exact gameplay-chart fingerprint; or
* an exact audio content identity (SHA-256 / canonical Git LFS OID).

Existing target files are authoritative and are never overwritten. Recovered
files are placed in the conventional layout understood by the engine and every
copy is written to a provenance report. The normal Complete payload auditor is
still responsible for accepting or rejecting the resulting runtime closure.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import tempfile
from dataclasses import dataclass
from pathlib import Path

AUDIO_EXTS = (".ogg", ".wav", ".mp3", ".flac")
VOICE_STEMS = (
    "Voices",
    "Voices-Player",
    "Voices-Opponent",
    "Voices-BF",
    "Voices-Dad",
    "Voices-Player1",
    "Voices-Player2",
)
LFS_RE = re.compile(
    r"\Aversion https://git-lfs\.github\.com/spec/v1\n"
    r"oid sha256:([0-9a-f]{64})\n"
    r"size ([0-9]+)\n?\Z"
)


@dataclass(frozen=True)
class ChartRef:
    mod: Path
    path: Path
    song: str
    fingerprint: str
    root_kind: str


@dataclass(frozen=True)
class AudioRef:
    mod: Path
    path: Path
    song: str
    stem: str
    identity: str
    root_kind: str


def norm_id(value: object) -> str:
    return re.sub(r"[^\w\x80-\uffff]+", "-", str(value or "").strip().lower()).strip("-")


def read_json(path: Path) -> object | None:
    try:
        if path.stat().st_size > 16 * 1024 * 1024:
            return None
        return json.loads(path.read_text(encoding="utf-8", errors="strict"))
    except Exception:
        return None


def song_object(raw: object) -> dict | None:
    if not isinstance(raw, dict):
        return None
    value = raw.get("song", raw)
    return value if isinstance(value, dict) else None


def root_kind(path: Path, marker: str) -> str:
    parts = [part.lower() for part in path.parts]
    try:
        index = max(i for i, part in enumerate(parts[:-1]) if part == marker)
    except ValueError:
        return ""
    prefix = parts[:index]
    if prefix[-2:] == ["assets", "preload"]:
        return "assets/preload"
    if prefix[-2:] == ["assets", "shared"]:
        return "assets/shared"
    if prefix[-1:] == ["assets"]:
        return "assets"
    return ""


def folder_song(path: Path, marker: str) -> str:
    parts = [part.lower() for part in path.parts]
    try:
        index = max(i for i, part in enumerate(parts[:-1]) if part == marker)
    except ValueError:
        return ""
    if index + 1 >= len(path.parts) - 1:
        return ""
    return norm_id(path.parts[index + 1])


def chart_fingerprint(raw: object) -> str | None:
    """Fingerprint the gameplay/timing identity, not presentation names."""
    song = song_object(raw)
    if song is None:
        return None
    payload = {
        "bpm": song.get("bpm"),
        "speed": song.get("speed"),
        "needsVoices": song.get("needsVoices"),
        "notes": song.get("notes", []),
        "events": song.get("events", []),
    }
    encoded = json.dumps(
        payload, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def content_identity(path: Path) -> str | None:
    try:
        size = path.stat().st_size
    except OSError:
        return None
    if size <= 1024:
        try:
            text = path.read_text(encoding="utf-8", errors="strict")
        except (OSError, UnicodeError):
            text = ""
        match = LFS_RE.fullmatch(text)
        if match:
            return "sha256:" + match.group(1)
    digest = hashlib.sha256()
    try:
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError:
        return None
    return "sha256:" + digest.hexdigest()


def discover_charts(mod: Path) -> list[ChartRef]:
    result: list[ChartRef] = []
    for path in mod.rglob("*.json"):
        rel = path.relative_to(mod)
        low = [part.lower() for part in rel.parts]
        if "data" not in low[:-1] or path.stem.lower() == "events":
            continue
        raw = read_json(path)
        fingerprint = chart_fingerprint(raw)
        if fingerprint is None:
            continue
        song = folder_song(rel, "data")
        if not song:
            song_data = song_object(raw)
            song = norm_id(song_data.get("song")) if song_data else ""
        if song:
            result.append(ChartRef(mod, path, song, fingerprint, root_kind(rel, "data")))
    return result


def audio_stem(path: Path) -> str | None:
    stem = path.stem.lower()
    if stem == "inst":
        return "Inst"
    for candidate in VOICE_STEMS:
        if stem == candidate.lower():
            return candidate
    return None


def discover_audio(mod: Path) -> list[AudioRef]:
    result: list[AudioRef] = []
    for path in mod.rglob("*"):
        if not path.is_file() or path.suffix.lower() not in AUDIO_EXTS:
            continue
        stem = audio_stem(path)
        if stem is None:
            continue
        rel = path.relative_to(mod)
        song = folder_song(rel, "songs")
        identity = content_identity(path)
        if song and identity:
            result.append(AudioRef(mod, path, song, stem, identity, root_kind(rel, "songs")))
    return result


def sibling_mods(project: Path, target: Path) -> list[Path]:
    root = project / "mods"
    if not root.is_dir():
        return []
    target = target.resolve()
    return sorted(
        [item for item in root.iterdir() if item.is_dir() and item.resolve() != target],
        key=lambda item: item.name.casefold(),
    )


def conventional_base(mod: Path, kind: str, category: str, song: str) -> Path:
    return mod / kind / category / song if kind else mod / category / song


def copy_file(
    source: Path,
    destination: Path,
    target: Path,
    records: list[dict],
    reason: str,
    donor: Path,
    evidence: dict,
) -> bool:
    if destination.exists():
        return False
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    records.append(
        {
            "source_mod": donor.name,
            "source": source.relative_to(donor).as_posix(),
            "destination": destination.relative_to(target).as_posix(),
            "reason": reason,
            "evidence": evidence,
        }
    )
    return True


def group_audio_signature(items: list[AudioRef]) -> tuple[tuple[str, str], ...]:
    return tuple(sorted((item.stem.lower(), item.identity) for item in items))


def choose_chart_donor(
    candidates: list[ChartRef], donor_audio: list[AudioRef]
) -> tuple[tuple[Path, str, str], list[ChartRef]] | None:
    groups: dict[tuple[Path, str, str], list[ChartRef]] = {}
    for item in candidates:
        groups.setdefault((item.mod, item.song, item.root_kind), []).append(item)
    viable = []
    for key, charts in groups.items():
        audio = [a for a in donor_audio if a.mod == key[0] and a.song == key[1]]
        if audio:
            viable.append((key, charts, group_audio_signature(audio)))
    if not viable:
        return None
    signatures = {entry[2] for entry in viable}
    if len(signatures) != 1:
        return None
    key, charts, _ = sorted(viable, key=lambda entry: (entry[0][0].name.casefold(), entry[0][1]))[0]
    return key, charts


def choose_audio_donor(
    candidates: list[AudioRef], donor_charts: list[ChartRef]
) -> tuple[tuple[Path, str, str], list[ChartRef]] | None:
    groups: dict[tuple[Path, str, str], list[AudioRef]] = {}
    for item in candidates:
        groups.setdefault((item.mod, item.song, item.root_kind), []).append(item)
    viable = []
    for key in groups:
        charts = [c for c in donor_charts if c.mod == key[0] and c.song == key[1]]
        if not charts:
            continue
        signature = tuple(sorted(c.fingerprint for c in charts))
        viable.append((key, charts, signature))
    if not viable:
        return None
    signatures = {entry[2] for entry in viable}
    if len(signatures) != 1:
        return None
    key, charts, _ = sorted(viable, key=lambda entry: (entry[0][0].name.casefold(), entry[0][1]))[0]
    return key, charts


def copy_song_audio(
    donor: Path,
    donor_song: str,
    target: Path,
    target_song: str,
    target_kind: str,
    donor_audio: list[AudioRef],
    records: list[dict],
    reason: str,
    evidence: dict,
) -> None:
    destination_root = conventional_base(target, target_kind, "songs", target_song)
    for item in sorted(
        [a for a in donor_audio if a.mod == donor and a.song == donor_song],
        key=lambda a: (a.stem.casefold(), a.path.name.casefold()),
    ):
        copy_file(item.path, destination_root / item.path.name, target, records, reason, donor, evidence)


def find_suffix(donor: Path, suffixes: list[str]) -> Path | None:
    wanted = [value.replace("\\", "/").lower() for value in suffixes]
    for path in donor.rglob("*"):
        if not path.is_file():
            continue
        rel = path.relative_to(donor).as_posix().lower()
        if any(rel.endswith(value) for value in wanted):
            return path
    return None


def copy_runtime_refs(
    donor: Path,
    chart: ChartRef,
    target: Path,
    records: list[dict],
    evidence: dict,
) -> None:
    """Copy direct character/stage/custom-script dependencies from the donor.

    Nested Lua asset references remain the responsibility of the existing
    functional-closure auditor; this step deliberately does not guess them.
    """
    raw = read_json(chart.path)
    song = song_object(raw)
    if song is None:
        return

    characters = []
    for key in ("player1", "player2", "gfVersion"):
        value = song.get(key)
        if isinstance(value, str) and value.strip():
            characters.append(value.strip())
    for event in song.get("events", []) if isinstance(song.get("events"), list) else []:
        if not (isinstance(event, list) and len(event) > 1 and isinstance(event[1], list)):
            continue
        for row in event[1]:
            if (
                isinstance(row, list)
                and len(row) > 2
                and isinstance(row[0], str)
                and norm_id(row[0]) == "change-character"
                and isinstance(row[2], str)
                and row[2].strip()
            ):
                characters.append(row[2].strip())

    char_roots = (
        "characters", "data/characters", "assets/characters", "assets/data/characters",
        "assets/preload/characters", "assets/shared/characters",
    )
    image_roots = (
        "images", "assets/images", "shared/images", "assets/shared/images",
        "preload/images", "assets/preload/images",
    )
    for character in sorted(set(characters), key=str.casefold):
        cid = norm_id(character)
        source = find_suffix(donor, [f"{root}/{cid}.json" for root in char_roots])
        if source is None:
            continue
        destination = target / source.relative_to(donor)
        copy_file(source, destination, target, records, "copy chart-referenced character definition", donor, evidence)
        character_json = read_json(source)
        image = character_json.get("image") if isinstance(character_json, dict) else None
        if not isinstance(image, str) or not image.strip():
            continue
        image_ref = image.replace("\\", "/").strip().strip("/")
        suffixes = []
        for ext in (".png", ".jpg", ".jpeg", ".webp"):
            suffixes.extend(f"{root}/{image_ref}{ext}" for root in image_roots)
        image_source = find_suffix(donor, suffixes)
        if image_source is None:
            continue
        copy_file(image_source, target / image_source.relative_to(donor), target, records, "copy character image", donor, evidence)
        stem = image_source.with_suffix("")
        for ext in (".xml", ".txt", ".json"):
            atlas = stem.with_suffix(ext)
            if atlas.is_file():
                copy_file(atlas, target / atlas.relative_to(donor), target, records, "copy character atlas metadata", donor, evidence)

    stage = song.get("stage")
    if isinstance(stage, str) and stage.strip():
        sid = norm_id(stage)
        stage_roots = (
            "stages", "data/stages", "assets/stages", "assets/data/stages",
            "assets/preload/stages", "assets/shared/stages",
        )
        for ext in (".json", ".lua"):
            source = find_suffix(donor, [f"{root}/{sid}{ext}" for root in stage_roots])
            if source is not None:
                copy_file(source, target / source.relative_to(donor), target, records, "copy chart-referenced stage", donor, evidence)

    note_types: set[str] = set()
    for section in song.get("notes", []) if isinstance(song.get("notes"), list) else []:
        if not isinstance(section, dict):
            continue
        for note in section.get("sectionNotes", []) if isinstance(section.get("sectionNotes"), list) else []:
            if isinstance(note, list) and len(note) > 3 and isinstance(note[3], str) and note[3].strip():
                note_types.add(note[3].strip())
    event_types: set[str] = set()
    for event in song.get("events", []) if isinstance(song.get("events"), list) else []:
        if not (isinstance(event, list) and len(event) > 1 and isinstance(event[1], list)):
            continue
        for row in event[1]:
            if isinstance(row, list) and row and isinstance(row[0], str) and row[0].strip():
                event_types.add(row[0].strip())

    for folder, names in (("custom_notetypes", note_types), ("custom_events", event_types)):
        roots = (folder, f"data/{folder}", f"assets/{folder}", f"assets/data/{folder}", f"assets/preload/{folder}", f"assets/shared/{folder}")
        for name in sorted(names, key=str.casefold):
            nid = norm_id(name)
            source = find_suffix(donor, [f"{root}/{nid}.lua" for root in roots])
            if source is not None:
                copy_file(source, target / source.relative_to(donor), target, records, f"copy chart-referenced {folder} script", donor, evidence)


def copy_chart_data(
    donor: Path,
    donor_song: str,
    target: Path,
    target_song: str,
    target_kind: str,
    donor_charts: list[ChartRef],
    records: list[dict],
    evidence: dict,
) -> None:
    destination_root = conventional_base(target, target_kind, "data", target_song)
    charts = [c for c in donor_charts if c.mod == donor and c.song == donor_song]
    for chart in sorted(charts, key=lambda c: c.path.name.casefold()):
        copy_file(chart.path, destination_root / chart.path.name, target, records, "complete missing chart data from identical song audio", donor, evidence)
        events = chart.path.parent / "events.json"
        if events.is_file():
            copy_file(events, destination_root / "events.json", target, records, "copy matching standalone chart events", donor, evidence)
        copy_runtime_refs(donor, chart, target, records, evidence)


def recover(project: Path, target: Path) -> dict:
    project = project.resolve()
    target = target.resolve()
    donors = sibling_mods(project, target)
    target_charts = discover_charts(target)
    target_audio = discover_audio(target)
    donor_charts = [chart for donor in donors for chart in discover_charts(donor)]
    donor_audio = [audio for donor in donors for audio in discover_audio(donor)]

    by_chart: dict[str, list[ChartRef]] = {}
    for chart in donor_charts:
        by_chart.setdefault(chart.fingerprint, []).append(chart)
    by_audio: dict[tuple[str, str], list[AudioRef]] = {}
    for audio in donor_audio:
        by_audio.setdefault((audio.stem.lower(), audio.identity), []).append(audio)

    records: list[dict] = []
    ambiguous: list[dict] = []

    # Chart-only/incomplete chart case -> recover audio and direct dependencies.
    for chart in target_charts:
        local_audio = [audio for audio in target_audio if audio.song == chart.song]
        raw = read_json(chart.path)
        song = song_object(raw) or {}
        have_inst = any(audio.stem == "Inst" for audio in local_audio)
        need_voice = song.get("needsVoices") is True
        have_voice = any(audio.stem != "Inst" for audio in local_audio)
        if have_inst and (have_voice or not need_voice):
            continue
        chosen = choose_chart_donor(by_chart.get(chart.fingerprint, []), donor_audio)
        if chosen is None:
            if by_chart.get(chart.fingerprint):
                ambiguous.append({"kind": "chart_to_dependencies", "chart": chart.path.relative_to(target).as_posix(), "fingerprint": chart.fingerprint})
            continue
        (donor, donor_song, _), donor_group = chosen
        evidence = {
            "method": "exact_gameplay_chart_fingerprint",
            "sha256": chart.fingerprint,
            "target_song": chart.song,
            "donor_song": donor_song,
        }
        copy_song_audio(donor, donor_song, target, chart.song, chart.root_kind, donor_audio, records, "complete missing song audio from identical chart", evidence)
        copy_runtime_refs(donor, donor_group[0], target, records, evidence)
        events = donor_group[0].path.parent / "events.json"
        if events.is_file():
            destination = conventional_base(target, chart.root_kind, "data", chart.song) / "events.json"
            copy_file(events, destination, target, records, "copy matching standalone chart events", donor, evidence)

    # Audio-only case -> recover charts, sibling stems and direct dependencies.
    target_audio = discover_audio(target)
    songs_with_charts = {chart.song for chart in discover_charts(target)}
    for audio in target_audio:
        if audio.song in songs_with_charts:
            continue
        matches = by_audio.get((audio.stem.lower(), audio.identity), [])
        chosen = choose_audio_donor(matches, donor_charts)
        if chosen is None:
            if matches:
                ambiguous.append({"kind": "audio_to_dependencies", "audio": audio.path.relative_to(target).as_posix(), "identity": audio.identity})
            continue
        (donor, donor_song, _), charts = chosen
        evidence = {
            "method": "exact_audio_content_identity",
            "identity": audio.identity,
            "stem": audio.stem,
            "target_song": audio.song,
            "donor_song": donor_song,
        }
        copy_chart_data(donor, donor_song, target, audio.song, audio.root_kind, donor_charts, records, evidence)
        copy_song_audio(donor, donor_song, target, audio.song, audio.root_kind, donor_audio, records, "complete sibling song stems from identical audio", evidence)
        songs_with_charts.add(audio.song)

    return {
        "format": "pulseforge-complete-cross-mod-recovery-v1",
        "target_mod": target.name,
        "donor_mods_scanned": len(donors),
        "recovered_files": len(records),
        "recoveries": records,
        "ambiguous_matches": ambiguous,
        "warnings": (["ambiguous donor matches were left untouched; no arbitrary cross-mod choice was made"] if ambiguous else []),
    }


def selftest() -> int:
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        mods = root / "mods"
        donor = mods / "donor"
        target_chart = mods / "target-chart"
        chart = {
            "song": {
                "song": "Example",
                "bpm": 150,
                "speed": 1.0,
                "needsVoices": True,
                "player1": "bf",
                "player2": "dad",
                "stage": "stage",
                "notes": [{"lengthInSteps": 16, "mustHitSection": True, "sectionNotes": [[100.0, 0, 0.0], [200.0, 4, 0.0]]}],
                "events": [],
            }
        }
        donor_data = donor / "data" / "example"
        donor_song = donor / "songs" / "example"
        donor_data.mkdir(parents=True)
        donor_song.mkdir(parents=True)
        donor_data.joinpath("example.json").write_text(json.dumps(chart), encoding="utf-8")
        donor_song.joinpath("Inst.ogg").write_bytes(b"same-inst")
        donor_song.joinpath("Voices.ogg").write_bytes(b"same-voices")

        target_data = target_chart / "data" / "renamed-example"
        target_data.mkdir(parents=True)
        renamed = json.loads(json.dumps(chart))
        renamed["song"]["song"] = "Renamed Example"
        target_data.joinpath("hard.json").write_text(json.dumps(renamed), encoding="utf-8")
        report = recover(root, target_chart)
        if report["recovered_files"] != 2:
            return 1
        if not (target_chart / "songs" / "renamed-example" / "Inst.ogg").is_file():
            return 1
        if not (target_chart / "songs" / "renamed-example" / "Voices.ogg").is_file():
            return 1

    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        mods = root / "mods"
        donor = mods / "donor"
        target_audio = mods / "target-audio"
        chart = {"song": {"song": "Example", "bpm": 120, "speed": 1, "needsVoices": True, "notes": [], "events": []}}
        donor_data = donor / "data" / "example"
        donor_song = donor / "songs" / "example"
        donor_data.mkdir(parents=True)
        donor_song.mkdir(parents=True)
        donor_data.joinpath("example.json").write_text(json.dumps(chart), encoding="utf-8")
        donor_song.joinpath("Inst.ogg").write_bytes(b"same-inst")
        donor_song.joinpath("Voices.ogg").write_bytes(b"same-voices")
        target_song = target_audio / "songs" / "local-name"
        target_song.mkdir(parents=True)
        target_song.joinpath("Inst.ogg").write_bytes(b"same-inst")
        report = recover(root, target_audio)
        if report["recovered_files"] < 2:
            return 1
        if not (target_audio / "data" / "local-name" / "example.json").is_file():
            return 1
        if not (target_audio / "songs" / "local-name" / "Voices.ogg").is_file():
            return 1
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", nargs="?", default=".")
    parser.add_argument("--target")
    parser.add_argument("--output")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        rc = selftest()
        print("PulseForge Complete cross-mod recovery self-test:", "PASS" if rc == 0 else "FAIL")
        return rc
    if not args.target:
        parser.error("--target is required")
    root = Path(args.root).resolve()
    target = Path(args.target)
    target = target if target.is_absolute() else root / target
    report = recover(root, target)
    if args.output:
        output = Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(
        f"PulseForge Complete cross-mod recovery: {report['recovered_files']} files recovered; "
        f"{len(report['ambiguous_matches'])} ambiguous matches"
    )
    for warning in report["warnings"]:
        print("WARNING:", warning)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
