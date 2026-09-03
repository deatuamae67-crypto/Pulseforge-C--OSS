#!/usr/bin/env python3
"""PulseForge AutoChart optional neural analysis backend.

This helper is intentionally isolated from the engine process.  A model/runtime
failure cannot crash gameplay or the native DSP path.  It writes one bounded JSON
file consumed by pulseforge-autochart and keeps large model/stem caches outside
of the chart model.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib
import importlib.metadata
import json
import math
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import traceback
from typing import Any, Iterable

SCHEMA = "pulseforge-autochart-ml-v3"
PIPELINE_REVISION = "fnf-vocal-focus-v4-compact-cache"
HEALTH_SCHEMA = "pulseforge-autochart-ml-health-v1"
PHONEME_MODEL = "facebook/wav2vec2-xlsr-53-phon-cv-ft"
PHONEME_MODEL_REVISION = "1c2ce0df451c63934f4bad5b65e8e87acf1bf15f"
MAX_PITCH_EVENTS = 1_000_000
MAX_VOCAL_EVENTS = 1_000_000
BASIC_PITCH_CHUNK_SECONDS = 90.0
BASIC_PITCH_OVERLAP_SECONDS = 1.5
PHONEME_CHUNK_SECONDS = 12.0
PHONEME_OVERLAP_SECONDS = 0.5


def log(message: str) -> None:
    print(f"[AutoChart ML] {message}", flush=True)


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            block = stream.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def stable_key(
    input_path: Path,
    mode: str,
    source_sep: bool,
    beats: bool,
    drums: bool,
    pitch: bool,
    vocals: bool,
    compact_models: bool,
) -> str:
    digest = hashlib.sha256()
    digest.update(file_sha256(input_path).encode("ascii"))
    digest.update(b"\0")
    digest.update(SCHEMA.encode("ascii"))
    digest.update(b"\0")
    digest.update(PIPELINE_REVISION.encode("ascii"))
    digest.update(b"\0")
    digest.update(mode.encode("ascii"))
    digest.update(bytes([source_sep, beats, drums, pitch, vocals, compact_models]))
    if vocals:
        digest.update(PHONEME_MODEL.encode("utf-8"))
        digest.update(PHONEME_MODEL_REVISION.encode("ascii"))
    return digest.hexdigest()


def run_checked(arguments: list[str]) -> None:
    log("exec: " + " ".join(arguments[:6]) + (" ..." if len(arguments) > 6 else ""))

    process = subprocess.Popen(arguments)
    try:
        return_code = process.wait()
    except BaseException:
        # If AutoChart is interrupted while Python is waiting for Demucs/FFmpeg,
        # do not intentionally leave that immediate child running.
        try:
            process.terminate()
            process.wait(timeout=5.0)
        except Exception:
            try:
                process.kill()
                process.wait(timeout=2.0)
            except Exception:
                pass
        raise

    if return_code != 0:
        raise RuntimeError(f"command failed with exit code {return_code}: {arguments[0]}")


def make_analysis_wav(ffmpeg: Path, source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    run_checked([
        str(ffmpeg), "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
        "-i", str(source), "-map", "0:a:0", "-vn", "-ar", "44100", "-ac", "2",
        "-c:a", "pcm_f32le", str(destination),
    ])


def pick_device(requested: str) -> str:
    if requested not in {"auto", "cpu", "cuda"}:
        raise RuntimeError("ML device must be auto, cpu or cuda")
    try:
        import torch
        if requested == "cuda":
            if not torch.cuda.is_available():
                raise RuntimeError("CUDA was requested but PyTorch cannot access a CUDA device")
            return "cuda"
        if requested == "auto" and torch.cuda.is_available():
            return "cuda"
    except Exception:
        if requested == "cuda":
            raise
    return "cpu"


def separate_stems(
    audio_wav: Path,
    output_root: Path,
    mode: str,
    device: str,
    compact_models: bool = True,
) -> dict[str, str]:
    # PULSEFORGE_P1_5_0E_COMPACT_NEURAL_MODEL_POLICY_V1
    # htdemucs_ft is an ensemble-style additional model family. By default,
    # maximum quality spends CPU on finer native DSP instead of downloading
    # another large weight set; users can still opt out of compact_models.
    model = "htdemucs" if compact_models or mode != "maximum" else "htdemucs_ft"
    output_root.mkdir(parents=True, exist_ok=True)
    run_checked([
        sys.executable, "-m", "demucs.separate",
        "--name", model,
        "--device", device,
        "--out", str(output_root),
        "--filename", "{stem}.wav",
        str(audio_wav),
    ])
    stems: dict[str, str] = {}
    for stem in ("drums", "bass", "vocals", "other"):
        matches = list(output_root.rglob(f"{stem}.wav"))
        if matches:
            stems[stem] = str(matches[0].resolve())
    if len(stems) < 3:
        raise RuntimeError("Demucs completed but did not produce the expected stems")
    return stems


def neural_beats(audio_wav: Path, device: str) -> tuple[list[float], list[float]]:
    from beat_this.inference import File2Beats

    tracker = File2Beats(checkpoint_path="final0", device=device, dbn=False)
    beats, downbeats = tracker(str(audio_wav))
    beat_values = [float(value) for value in beats if math.isfinite(float(value)) and float(value) >= 0.0]
    downbeat_values = [
        float(value) for value in downbeats if math.isfinite(float(value)) and float(value) >= 0.0
    ]
    beat_values.sort()
    downbeat_values.sort()
    return beat_values, downbeat_values


def _basic_pitch_predict(path: Path) -> list[tuple[float, float, int, float]]:
    from basic_pitch.inference import predict

    _, _, events = predict(
        str(path),
        onset_threshold=0.56,
        frame_threshold=0.32,
        minimum_note_length=45.0,
        multiple_pitch_bends=False,
        melodia_trick=True,
    )
    result: list[tuple[float, float, int, float]] = []
    for start, end, midi, amplitude, _pitch_bends in events:
        start_f = float(start)
        end_f = float(end)
        amplitude_f = float(amplitude)
        midi_i = int(midi)
        if not (math.isfinite(start_f) and math.isfinite(end_f) and math.isfinite(amplitude_f)):
            continue
        if start_f < 0.0 or end_f <= start_f or midi_i < 0 or midi_i > 127:
            continue
        result.append((start_f, end_f, midi_i, max(0.0, min(1.0, amplitude_f))))
    return result


def _dedupe_pitch_events(
    events: list[tuple[float, float, int, float]], tolerance: float = 0.045
) -> list[tuple[float, float, int, float]]:
    events.sort(key=lambda item: (item[0], item[2], item[1]))
    output: list[tuple[float, float, int, float]] = []
    for event in events:
        if output and event[2] == output[-1][2] and abs(event[0] - output[-1][0]) <= tolerance:
            previous = output[-1]
            output[-1] = (
                min(previous[0], event[0]),
                max(previous[1], event[1]),
                event[2],
                max(previous[3], event[3]),
            )
        else:
            output.append(event)
        if len(output) >= MAX_PITCH_EVENTS:
            break
    return output



def _collapse_vocal_harmonics(
    events: list[tuple[float, float, int, float]],
    onset_tolerance: float = 0.035,
) -> list[tuple[float, float, int, float]]:
    """Reduce a polyphonic Basic Pitch chord at one vocal articulation to one event.

    FNF vocals are frequently synthesized and strongly harmonic. Basic Pitch can
    report the fundamental and several overtones at the same syllable onset.
    For charting we want the articulation, not one note per harmonic partial.
    """
    if not events:
        return []
    ordered = sorted(events, key=lambda item: (item[0], -item[3], item[2], item[1]))
    collapsed: list[tuple[float, float, int, float]] = []
    group: list[tuple[float, float, int, float]] = []

    def flush() -> None:
        if not group:
            return
        # Prefer the most confident event. In near ties prefer the lower MIDI
        # value because it is more likely to be the fundamental than an overtone.
        best = max(group, key=lambda item: (item[3], -item[2], item[1] - item[0]))
        start = min(item[0] for item in group)
        end = max(item[1] for item in group)
        confidence = max(item[3] for item in group)
        collapsed.append((start, end, best[2], confidence))

    for event in ordered:
        if group and event[0] - group[0][0] > onset_tolerance:
            flush()
            group = []
        group.append(event)
    flush()
    return collapsed[:MAX_PITCH_EVENTS]


def transcribe_pitch_stem(stem_name: str, path: Path, scratch: Path) -> list[dict[str, Any]]:
    try:
        import soundfile as sf
    except Exception:
        sf = None

    raw_events: list[tuple[float, float, int, float]] = []
    if sf is None:
        raw_events = _basic_pitch_predict(path)
    else:
        info = sf.info(str(path))
        duration = float(info.frames) / float(info.samplerate) if info.samplerate else 0.0
        if duration <= BASIC_PITCH_CHUNK_SECONDS + 5.0:
            raw_events = _basic_pitch_predict(path)
        else:
            chunk_dir = scratch / f"pitch-{stem_name}"
            chunk_dir.mkdir(parents=True, exist_ok=True)
            stride = BASIC_PITCH_CHUNK_SECONDS - BASIC_PITCH_OVERLAP_SECONDS
            start_seconds = 0.0
            chunk_index = 0
            while start_seconds < duration:
                start_frame = int(round(start_seconds * info.samplerate))
                frame_count = int(round(BASIC_PITCH_CHUNK_SECONDS * info.samplerate))
                data, sample_rate = sf.read(
                    str(path),
                    start=start_frame,
                    frames=frame_count,
                    dtype="float32",
                    always_2d=True,
                )
                if len(data) == 0:
                    break
                chunk_path = chunk_dir / f"chunk-{chunk_index:05d}.wav"
                sf.write(str(chunk_path), data, sample_rate, subtype="FLOAT")
                for event in _basic_pitch_predict(chunk_path):
                    raw_events.append((
                        event[0] + start_seconds,
                        event[1] + start_seconds,
                        event[2],
                        event[3],
                    ))
                chunk_index += 1
                start_seconds += stride
                if len(raw_events) >= MAX_PITCH_EVENTS:
                    break
    raw_events = _dedupe_pitch_events(raw_events)
    if stem_name == "vocals":
        raw_events = _collapse_vocal_harmonics(raw_events)
    return [
        {
            "stem": stem_name,
            "startSeconds": start,
            "endSeconds": end,
            "midi": midi,
            "confidence": confidence,
        }
        for start, end, midi, confidence in raw_events
    ]


def transcribe_stems(stems: dict[str, str], scratch: Path, mode: str) -> list[dict[str, Any]]:
    # FNF chart notes represent vocal articulations. Instrumental stems are
    # intentionally excluded from Basic Pitch candidate generation.
    del mode
    vocal_path = stems.get("vocals", "")
    if not vocal_path:
        raise RuntimeError(
            "vocal-focused pitch transcription requires the separated vocals stem"
        )
    log("transcribing vocals stem with Basic Pitch (instrumental stems excluded)")
    events = transcribe_pitch_stem("vocals", Path(vocal_path), scratch)
    events.sort(key=lambda event: (float(event["startSeconds"]), int(event["midi"])))
    return events[:MAX_PITCH_EVENTS]



def _clean_phoneme_token(value: str) -> str:
    value = value.replace("\t", " ").replace("\r", " ").replace("\n", " ").strip()
    return value[:96]


def _phoneme_is_vowel(token: str) -> bool:
    # IPA/eSpeak nuclei. The recognizer is multilingual, so use phonetic
    # symbols rather than language-specific orthography.
    vowel_symbols = set(
        "aeiouyAEIOUYɑɒæɐɜɞəɘɚɛɝɪɨɔɵɶœɤɯʊʌøɐɶɒ"
    )
    return any(character in vowel_symbols for character in token)


def _dedupe_phoneme_events(events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    events.sort(key=lambda event: (float(event["startSeconds"]), str(event["token"])))
    output: list[dict[str, Any]] = []
    for event in events:
        if output:
            previous = output[-1]
            same_token = str(previous["token"]) == str(event["token"])
            close = abs(float(previous["startSeconds"]) - float(event["startSeconds"])) <= 0.075
            if same_token and close:
                previous["startSeconds"] = min(
                    float(previous["startSeconds"]), float(event["startSeconds"])
                )
                previous["endSeconds"] = max(
                    float(previous["endSeconds"]), float(event["endSeconds"])
                )
                previous["confidence"] = max(
                    float(previous["confidence"]), float(event["confidence"])
                )
                continue
        output.append(event)
        if len(output) >= MAX_VOCAL_EVENTS:
            break
    return output


def _derive_syllable_events(phonemes: list[dict[str, Any]]) -> list[dict[str, Any]]:
    syllables: list[dict[str, Any]] = []
    cluster: list[dict[str, Any]] = []
    for phoneme in phonemes:
        start = float(phoneme["startSeconds"])
        if cluster and start - float(cluster[-1]["endSeconds"]) > 0.22:
            cluster.clear()
        cluster.append(phoneme)
        if not bool(phoneme.get("vowel")):
            if len(cluster) > 8:
                cluster = cluster[-8:]
            continue

        onset = float(cluster[0]["startSeconds"])
        nucleus_end = float(phoneme["endSeconds"])
        confidence = sum(float(item["confidence"]) for item in cluster) / float(len(cluster))
        token = _clean_phoneme_token("".join(str(item["token"]) for item in cluster))
        candidate = {
            "role": "syllable",
            "startSeconds": onset,
            "endSeconds": max(nucleus_end, onset + 0.025),
            "confidence": max(0.0, min(1.0, confidence)),
            "token": token,
        }
        if syllables and onset - float(syllables[-1]["startSeconds"]) < 0.085:
            # Adjacent vowel tokens are usually a diphthong/nucleus from one
            # articulation rather than two chartable syllables.
            syllables[-1]["endSeconds"] = max(
                float(syllables[-1]["endSeconds"]), candidate["endSeconds"]
            )
            syllables[-1]["confidence"] = max(
                float(syllables[-1]["confidence"]), candidate["confidence"]
            )
            merged = _clean_phoneme_token(
                str(syllables[-1]["token"]) + str(candidate["token"])
            )
            syllables[-1]["token"] = merged
        else:
            syllables.append(candidate)
        cluster.clear()
        if len(syllables) >= MAX_VOCAL_EVENTS // 2:
            break
    return syllables


def transcribe_vocal_phonemes(vocal_path: Path, device: str) -> list[dict[str, Any]]:
    """Return bounded phoneme spans plus derived syllable/articulation onsets.

    The model is speech-trained, not singing-specific. PulseForge therefore
    treats these events as corroborating timing evidence and never as the sole
    source of a note. Basic Pitch and vocal-stem DSP remain independent inputs.
    """
    import librosa
    import soundfile as sf
    import torch
    from transformers import AutoModelForCTC, AutoProcessor

    log(f"loading multilingual phoneme model {PHONEME_MODEL}")
    processor = AutoProcessor.from_pretrained(
        PHONEME_MODEL, revision=PHONEME_MODEL_REVISION
    )
    model = AutoModelForCTC.from_pretrained(
        PHONEME_MODEL, revision=PHONEME_MODEL_REVISION
    )
    model.eval()
    model.to(device)
    tokenizer = processor.tokenizer
    special_ids = set(getattr(tokenizer, "all_special_ids", []))
    blank_id = getattr(tokenizer, "pad_token_id", None)

    info = sf.info(str(vocal_path))
    if not info.samplerate or info.frames <= 0:
        return []
    source_rate = int(info.samplerate)
    duration = float(info.frames) / float(source_rate)
    stride = max(1.0, PHONEME_CHUNK_SECONDS - PHONEME_OVERLAP_SECONDS)
    phonemes: list[dict[str, Any]] = []
    start_seconds = 0.0
    with sf.SoundFile(str(vocal_path), "r") as stream:
        while start_seconds < duration and len(phonemes) < MAX_VOCAL_EVENTS:
            start_frame = int(round(start_seconds * source_rate))
            stream.seek(min(start_frame, len(stream)))
            frame_count = int(round(PHONEME_CHUNK_SECONDS * source_rate))
            data = stream.read(frame_count, dtype="float32", always_2d=True)
            if len(data) == 0:
                break
            mono = data.mean(axis=1)
            if source_rate != 16_000:
                mono = librosa.resample(
                    mono,
                    orig_sr=source_rate,
                    target_sr=16_000,
                    res_type="soxr_hq",
                )
            if len(mono) < 320:
                break
            inputs = processor(mono, sampling_rate=16_000, return_tensors="pt")
            model_inputs = {
                key: value.to(device)
                for key, value in inputs.items()
                if hasattr(value, "to")
            }
            with torch.inference_mode():
                logits = model(**model_inputs).logits[0]
                probabilities = torch.softmax(logits, dim=-1)
                confidences, token_ids = probabilities.max(dim=-1)
            ids = token_ids.detach().cpu().tolist()
            conf = confidences.detach().cpu().tolist()
            frame_count_out = len(ids)
            if frame_count_out == 0:
                start_seconds += stride
                continue
            actual_duration = float(len(mono)) / 16_000.0
            frame_seconds = actual_duration / float(frame_count_out)
            run_start = 0
            while run_start < frame_count_out:
                token_id = int(ids[run_start])
                run_end = run_start + 1
                while run_end < frame_count_out and int(ids[run_end]) == token_id:
                    run_end += 1
                if token_id not in special_ids and token_id != blank_id:
                    token = _clean_phoneme_token(str(tokenizer.convert_ids_to_tokens(token_id)))
                    if token and token not in {"|", "<pad>", "<s>", "</s>", "<unk>"}:
                        mean_conf = sum(float(v) for v in conf[run_start:run_end]) / float(run_end - run_start)
                        if mean_conf >= 0.20:
                            event_start = start_seconds + float(run_start) * frame_seconds
                            event_end = start_seconds + float(run_end) * frame_seconds
                            phonemes.append({
                                "role": "phoneme",
                                "startSeconds": event_start,
                                "endSeconds": max(event_end, event_start + 0.01),
                                "confidence": max(0.0, min(1.0, mean_conf)),
                                "token": token,
                                "vowel": _phoneme_is_vowel(token),
                            })
                run_start = run_end
            start_seconds += stride

    phonemes = _dedupe_phoneme_events(phonemes)
    syllables = _derive_syllable_events(phonemes)
    # Strip the internal vowel marker before serialization.
    for event in phonemes:
        event.pop("vowel", None)
    combined = phonemes + syllables
    combined.sort(key=lambda event: (float(event["startSeconds"]), str(event["role"])))
    return combined[:MAX_VOCAL_EVENTS]


def write_vocal_events_tsv(path: Path, events: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write("start_ms\tend_ms\tconfidence\trole\ttoken\n")
        for event in events[:MAX_VOCAL_EVENTS]:
            token = _clean_phoneme_token(str(event.get("token", "")))
            stream.write(
                f"{float(event['startSeconds']) * 1000.0:.6f}\t"
                f"{float(event['endSeconds']) * 1000.0:.6f}\t"
                f"{float(event['confidence']):.6f}\t"
                f"{str(event['role'])}\t{token}\n"
            )
    os.replace(temporary, path)

def transcribe_drums(audio_path: Path, device: str) -> list[dict[str, Any]]:
    from adtof_pytorch import transcribe_to_midi
    from adtof_pytorch.post_processing import PeakPicker, FRAME_RNN_THRESHOLDS, LABELS_5

    # The pinned ADTOF PyTorch port can return frame activations directly.
    activations = transcribe_to_midi(
        str(audio_path),
        str(audio_path.with_suffix(".adtof-unused.mid")),
        return_activations=True,
        device=device,
    )
    picker = PeakPicker(thresholds=FRAME_RNN_THRESHOLDS, fps=100)
    peaks = picker.pick(activations, labels=LABELS_5)[0]
    roles = {35: "kick", 38: "snare", 47: "tom", 42: "hihat", 49: "cymbal"}
    events: list[dict[str, Any]] = []
    time_frames = int(activations.shape[1])
    for class_index, midi in enumerate(LABELS_5):
        for time_s in peaks.get(int(midi), []):
            frame = min(time_frames - 1, max(0, int(round(float(time_s) * 100.0))))
            confidence = float(activations[0, frame, class_index])
            events.append({
                "timeSeconds": float(time_s),
                "midi": int(midi),
                "role": roles.get(int(midi), "drum"),
                "confidence": max(0.0, min(1.0, confidence)),
            })
            if len(events) >= MAX_PITCH_EVENTS:
                break
        if len(events) >= MAX_PITCH_EVENTS:
            break
    events.sort(key=lambda event: (float(event["timeSeconds"]), int(event["midi"])))
    return events


def write_drum_events_tsv(path: Path, events: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write("time_ms\tmidi\tconfidence\trole\n")
        for event in events[:MAX_PITCH_EVENTS]:
            stream.write(
                f"{float(event['timeSeconds']) * 1000.0:.6f}\t"
                f"{int(event['midi'])}\t{float(event['confidence']):.6f}\t"
                f"{str(event['role'])}\n"
            )
    os.replace(temporary, path)


def write_pitch_events_tsv(path: Path, events: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write("start_ms\tend_ms\tmidi\tconfidence\tstem\n")
        for event in events[:MAX_PITCH_EVENTS]:
            stream.write(
                f"{float(event['startSeconds']) * 1000.0:.6f}\t"
                f"{float(event['endSeconds']) * 1000.0:.6f}\t"
                f"{int(event['midi'])}\t{float(event['confidence']):.6f}\t"
                f"{str(event['stem'])}\n"
            )
    os.replace(temporary, path)


def write_json_atomic(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(payload, indent=2, ensure_ascii=False), encoding="utf-8")
    os.replace(temporary, path)


def load_cached(path: Path) -> dict[str, Any] | None:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None
    if payload.get("schema") != SCHEMA or not payload.get("ok"):
        return None
    # A stale cache with deleted stem files must not be reported as usable.
    for stem_path in payload.get("stems", {}).values():
        if stem_path and not Path(stem_path).is_file():
            return None
    drum_path = payload.get("drumEventsPath", "")
    if payload.get("stages", {}).get("drumTranscription") and (not drum_path or not Path(drum_path).is_file()):
        return None
    pitch_path = payload.get("pitchEventsPath", "")
    if payload.get("stages", {}).get("pitchTranscription") and (not pitch_path or not Path(pitch_path).is_file()):
        return None
    vocal_path = payload.get("vocalEventsPath", "")
    if payload.get("stages", {}).get("vocalRefinement") and (not vocal_path or not Path(vocal_path).is_file()):
        return None
    return payload



def _package_version(distribution: str) -> str:
    try:
        return importlib.metadata.version(distribution)
    except Exception:
        return "unknown"


def _health_stage(
    name: str,
    import_name: str | None = None,
    distribution: str | None = None,
) -> dict[str, Any]:
    started = time.perf_counter()
    stage: dict[str, Any] = {
        "name": name,
        "available": False,
        "tested": False,
        "latencyMs": 0.0,
        "detail": "",
    }
    try:
        if import_name:
            importlib.import_module(import_name)
        stage["available"] = True
        if distribution:
            stage["detail"] = f"version {_package_version(distribution)}"
        else:
            stage["detail"] = "available"
    except Exception as exc:
        stage["detail"] = f"unavailable: {exc}"
    stage["latencyMs"] = (time.perf_counter() - started) * 1000.0
    return stage


def _make_health_fixture(path: Path) -> None:
    import numpy as np
    import soundfile as sf

    sample_rate = 44_100
    seconds = 4.0
    count = int(sample_rate * seconds)
    time_axis = np.arange(count, dtype=np.float32) / float(sample_rate)
    # A bounded deterministic fixture: harmonic tone + percussive pulses. It is
    # not intended to benchmark model quality, only to prove that weights,
    # tensor runtimes, decoders and inference paths execute end-to-end.
    signal = 0.08 * np.sin(2.0 * np.pi * 220.0 * time_axis)
    signal += 0.04 * np.sin(2.0 * np.pi * 330.0 * time_axis)
    for pulse_time in (0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5):
        center = int(round(pulse_time * sample_rate))
        length = min(int(sample_rate * 0.08), count - center)
        if length <= 0:
            continue
        envelope = np.exp(-np.arange(length, dtype=np.float32) / (sample_rate * 0.018))
        signal[center:center + length] += 0.45 * envelope
    stereo = np.stack([signal, signal], axis=1)
    sf.write(str(path), stereo, sample_rate, subtype="FLOAT")


def run_health_check(
    output: Path,
    workspace: Path,
    ffmpeg: Path,
    cache_root: Path,
    requested_device: str,
    deep: bool,
) -> int:
    workspace.mkdir(parents=True, exist_ok=True)
    cache_root.mkdir(parents=True, exist_ok=True)
    model_cache = cache_root / "models"
    model_cache.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault("TORCH_HOME", str(model_cache / "torch"))
    os.environ.setdefault("XDG_CACHE_HOME", str(model_cache / "xdg"))
    os.environ.setdefault("HF_HOME", str(model_cache / "huggingface"))

    stages: list[dict[str, Any]] = []
    stages.append({
        "name": "python",
        "available": True,
        "tested": True,
        "latencyMs": 0.0,
        "detail": sys.version.splitlines()[0],
    })

    ffmpeg_stage = {
        "name": "ffmpeg",
        "available": False,
        "tested": True,
        "latencyMs": 0.0,
        "detail": "",
    }
    started = time.perf_counter()
    try:
        process = subprocess.run(
            [str(ffmpeg), "-version"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=15,
        )
        ffmpeg_stage["available"] = process.returncode == 0
        first_line = process.stdout.splitlines()[0] if process.stdout else ""
        ffmpeg_stage["detail"] = first_line[:240] or f"exit {process.returncode}"
    except Exception as exc:
        ffmpeg_stage["detail"] = f"unavailable: {exc}"
    ffmpeg_stage["latencyMs"] = (time.perf_counter() - started) * 1000.0
    stages.append(ffmpeg_stage)

    package_specs = [
        ("torch", "torch", "torch"),
        ("soundfile", "soundfile", "soundfile"),
        ("demucs", "demucs", "demucs"),
        ("beatThis", "beat_this", "beat-this"),
        ("adtof", "adtof_pytorch", "adtof-pytorch"),
        ("basicPitch", "basic_pitch", "basic-pitch"),
        ("transformers", "transformers", "transformers"),
    ]
    for name, module_name, distribution in package_specs:
        stages.append(_health_stage(name, module_name, distribution))

    device = "cpu"
    try:
        device = pick_device(requested_device)
    except Exception as exc:
        stages.append({
            "name": "device",
            "available": False,
            "tested": True,
            "latencyMs": 0.0,
            "detail": str(exc),
        })
    else:
        stages.append({
            "name": "device",
            "available": True,
            "tested": True,
            "latencyMs": 0.0,
            "detail": device,
        })

    phoneme_stage = {
        "name": "phonemeModel",
        "available": any(stage["name"] == "transformers" and stage["available"] for stage in stages),
        "tested": False,
        "latencyMs": 0.0,
        "detail": PHONEME_MODEL,
    }
    stages.append(phoneme_stage)

    if deep and ffmpeg_stage["available"]:
        fixture = workspace / "health-fixture.wav"
        try:
            _make_health_fixture(fixture)
        except Exception as exc:
            stages.append({
                "name": "fixture",
                "available": False,
                "tested": True,
                "latencyMs": 0.0,
                "detail": f"could not create fixture: {exc}",
            })
        else:
            def deep_stage(name: str, callback: Any) -> None:
                target = next((stage for stage in stages if stage["name"] == name), None)
                if target is None or not target["available"]:
                    return
                started_local = time.perf_counter()
                try:
                    detail = callback()
                    target["tested"] = True
                    if detail:
                        target["detail"] = str(detail)[:300]
                except Exception as exc:
                    target["available"] = False
                    target["tested"] = True
                    target["detail"] = f"deep test failed: {exc}"
                target["latencyMs"] += (time.perf_counter() - started_local) * 1000.0

            deep_stage(
                "basicPitch",
                lambda: f"inference ok; {len(_basic_pitch_predict(fixture))} events",
            )
            deep_stage(
                "beatThis",
                lambda: f"inference ok; {len(neural_beats(fixture, device)[0])} beats",
            )
            deep_stage(
                "adtof",
                lambda: f"inference ok; {len(transcribe_drums(fixture, device))} drum events",
            )

            def test_phonemes() -> str:
                events = transcribe_vocal_phonemes(fixture, device)
                return f"inference ok; {len(events)} phoneme/syllable events"

            deep_stage("phonemeModel", test_phonemes)

            def test_demucs() -> str:
                stems = separate_stems(
                    fixture,
                    workspace / "health-demucs",
                    "accurate",
                    device,
                )
                return f"inference ok; {len(stems)} stems"

            deep_stage("demucs", test_demucs)

    core_ok = bool(ffmpeg_stage["available"])
    torch_ok = any(stage["name"] == "torch" and stage["available"] for stage in stages)
    model_ok = any(
        stage["name"] in {"demucs", "beatThis", "adtof", "basicPitch", "phonemeModel"}
        and stage["available"]
        for stage in stages
    )
    payload = {
        "schema": HEALTH_SCHEMA,
        "ok": bool(core_ok and torch_ok and model_ok),
        "deep": bool(deep),
        "pythonVersion": sys.version.splitlines()[0],
        "device": device,
        "phonemeModel": PHONEME_MODEL,
        "phonemeModelRevision": PHONEME_MODEL_REVISION,
        "stages": stages,
    }
    if not payload["ok"]:
        payload["error"] = "AutoChart ML environment is incomplete"
    write_json_atomic(output, payload)
    return 0 if payload["ok"] else 2


INCOMPLETE_CACHE_PREFIX = ".incomplete-"


def _process_is_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    if pid == os.getpid():
        return True

    if os.name == "nt":
        try:
            import ctypes

            synchronize = 0x00100000
            wait_timeout = 0x00000102
            kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel32.OpenProcess.argtypes = [
                ctypes.c_uint32,
                ctypes.c_int,
                ctypes.c_uint32,
            ]
            kernel32.OpenProcess.restype = ctypes.c_void_p
            kernel32.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
            kernel32.WaitForSingleObject.restype = ctypes.c_uint32
            kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
            kernel32.CloseHandle.restype = ctypes.c_int

            handle = kernel32.OpenProcess(synchronize, 0, pid)
            if not handle:
                # ERROR_INVALID_PARAMETER means that PID does not exist.
                # Other failures (for example access denied) are treated as
                # "alive" so cleanup never deletes a possibly active run.
                return ctypes.get_last_error() != 87
            try:
                return kernel32.WaitForSingleObject(handle, 0) == wait_timeout
            finally:
                kernel32.CloseHandle(handle)
        except Exception:
            return True

    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    except Exception:
        return True


def _incomplete_cache_pid(path: Path) -> int | None:
    name = path.name
    if not name.startswith(INCOMPLETE_CACHE_PREFIX):
        return None
    # .incomplete-<64 hex key>-<pid>-<nonce>
    remainder = name[len(INCOMPLETE_CACHE_PREFIX):]
    pieces = remainder.split("-", 2)
    if len(pieces) != 3:
        return None
    try:
        return int(pieces[1])
    except ValueError:
        return None


def cleanup_abandoned_incomplete_caches(cache_root: Path) -> None:
    """Delete only transactional run directories whose owner is no longer alive."""
    if not cache_root.is_dir():
        return

    for candidate in cache_root.glob(f"{INCOMPLETE_CACHE_PREFIX}*"):
        if not candidate.is_dir():
            continue
        pid = _incomplete_cache_pid(candidate)
        if pid is None or pid == os.getpid() or _process_is_alive(pid):
            continue
        try:
            shutil.rmtree(candidate)
            log(f"removed abandoned incomplete ML cache {candidate.name}")
        except OSError as exc:
            log(f"could not remove abandoned ML cache {candidate.name}: {exc}")


def _rewrite_cached_payload_paths(
    payload: dict[str, Any],
    old_root: Path,
    new_root: Path,
) -> None:
    old_root = old_root.resolve()
    new_root = new_root.resolve()

    def relocate(value: Any) -> Any:
        if not isinstance(value, str) or not value:
            return value
        try:
            relative = Path(value).resolve().relative_to(old_root)
        except (OSError, ValueError):
            return value
        return str((new_root / relative).resolve())

    stems = payload.get("stems")
    if isinstance(stems, dict):
        for name in tuple(stems):
            stems[name] = relocate(stems[name])

    for key in ("drumEventsPath", "pitchEventsPath", "vocalEventsPath"):
        if key in payload:
            payload[key] = relocate(payload[key])


def _compact_completed_run_artifacts(
    work_cache_dir: Path,
    payload: dict[str, Any],
    ffmpeg: Path,
) -> None:
    """Keep all evidence consumed by C++, discard only redundant PCM/stems."""
    # PULSEFORGE_P1_5_0E_COMPACT_COMPLETED_ML_CACHE_V2
    # The native fusion pass consumes the separated vocal stem on every cache
    # hit.  Preserve it losslessly while removing the decoded full mix and the
    # three Demucs stems that are not exposed in the payload.  FLAC typically
    # shrinks this persistent evidence substantially without moving onset times
    # or changing samples after decode.
    stems = payload.get("stems")
    vocal_text = stems.get("vocals", "") if isinstance(stems, dict) else ""
    vocal_source = Path(vocal_text) if vocal_text else None
    compact_vocal = work_cache_dir / "vocals-analysis.flac"
    preserved_vocal: Path | None = None

    if vocal_source is not None and vocal_source.is_file():
        try:
            run_checked([
                str(ffmpeg), "-nostdin", "-hide_banner", "-loglevel", "error",
                "-y", "-i", str(vocal_source), "-map", "0:a:0", "-vn",
                "-c:a", "flac", "-compression_level", "8", str(compact_vocal),
            ])
            if compact_vocal.is_file() and compact_vocal.stat().st_size > 0:
                preserved_vocal = compact_vocal
        except Exception as exc:
            log(f"lossless vocal-cache compaction unavailable: {exc}")

        if preserved_vocal is None:
            # Compression failure must never trade accuracy for disk space.
            # Move/copy the original vocal evidence outside the Demucs tree so
            # the redundant stem directory can still be pruned safely.
            fallback_vocal = work_cache_dir / (
                "vocals-analysis" + (vocal_source.suffix or ".wav")
            )
            try:
                if vocal_source.resolve() != fallback_vocal.resolve():
                    shutil.copy2(vocal_source, fallback_vocal)
                preserved_vocal = fallback_vocal
            except OSError as exc:
                log(f"could not preserve vocal stem during cache compaction: {exc}")
                preserved_vocal = vocal_source

    if isinstance(stems, dict):
        payload["stems"] = (
            {"vocals": str(preserved_vocal.resolve())}
            if preserved_vocal is not None and preserved_vocal.is_file()
            else dict(stems)
        )

    try:
        (work_cache_dir / "analysis.wav").unlink(missing_ok=True)
    except OSError:
        pass

    demucs_root = work_cache_dir / "demucs"
    if preserved_vocal is not None:
        try:
            preserved_inside_demucs = preserved_vocal.resolve().is_relative_to(
                demucs_root.resolve()
            )
        except (OSError, ValueError):
            preserved_inside_demucs = False
    else:
        preserved_inside_demucs = False
    if not preserved_inside_demucs:
        shutil.rmtree(demucs_root, ignore_errors=True)


def _directory_size(path: Path) -> int:
    total = 0
    try:
        for item in path.rglob("*"):
            try:
                if item.is_file():
                    total += item.stat().st_size
            except OSError:
                continue
    except OSError:
        return 0
    return total


def prune_completed_run_caches(
    cache_root: Path,
    maximum_bytes: int,
    keep: Path | None = None,
) -> None:
    """LRU-prune per-song evidence while never touching shared model weights."""
    if maximum_bytes <= 0 or not cache_root.is_dir():
        return
    entries: list[tuple[float, int, Path]] = []
    total = 0
    try:
        candidates = list(cache_root.iterdir())
    except OSError:
        return
    for candidate in candidates:
        if not candidate.is_dir() or candidate.name == "models" \
                or candidate.name.startswith(INCOMPLETE_CACHE_PREFIX):
            continue
        result = candidate / "result.json"
        if not result.is_file():
            continue
        size = _directory_size(candidate)
        try:
            modified = result.stat().st_mtime
        except OSError:
            modified = 0.0
        entries.append((modified, size, candidate))
        total += size
    entries.sort(key=lambda entry: entry[0])
    keep_resolved = keep.resolve() if keep is not None else None
    for _modified, size, candidate in entries:
        if total <= maximum_bytes:
            break
        try:
            if keep_resolved is not None and candidate.resolve() == keep_resolved:
                continue
        except OSError:
            pass
        shutil.rmtree(candidate, ignore_errors=True)
        total = max(0, total - size)


def _promote_completed_cache(
    work_cache_dir: Path,
    final_cache_dir: Path,
    payload: dict[str, Any],
) -> None:
    """Atomically-ish promote a completed run; partial work never becomes cache."""
    # Another concurrent run may have completed the same key first.
    existing = load_cached(final_cache_dir / "result.json")
    if existing is not None:
        shutil.rmtree(work_cache_dir, ignore_errors=True)
        return

    if final_cache_dir.exists():
        # Legacy/interrupted cache directories had no transaction marker.
        # A directory without a valid result is not a completed cache.
        shutil.rmtree(final_cache_dir)

    _rewrite_cached_payload_paths(payload, work_cache_dir, final_cache_dir)
    os.replace(work_cache_dir, final_cache_dir)


def _install_interrupt_handlers() -> dict[int, Any]:
    previous: dict[int, Any] = {}

    def interrupted(signum: int, _frame: Any) -> None:
        raise KeyboardInterrupt(f"AutoChart interrupted by signal {signum}")

    for name in ("SIGINT", "SIGTERM", "SIGBREAK"):
        value = getattr(signal, name, None)
        if value is None:
            continue
        try:
            previous[value] = signal.getsignal(value)
            signal.signal(value, interrupted)
        except (OSError, RuntimeError, ValueError):
            pass
    return previous


def _restore_interrupt_handlers(previous: dict[int, Any]) -> None:
    for signum, handler in previous.items():
        try:
            signal.signal(signum, handler)
        except (OSError, RuntimeError, ValueError):
            pass



def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input")
    parser.add_argument("--output", required=True)
    parser.add_argument("--workspace", default="")
    parser.add_argument("--ffmpeg", required=True)
    parser.add_argument("--cache-root", required=True)
    parser.add_argument("--mode", choices=["fast", "accurate", "maximum"], default="accurate")
    parser.add_argument("--device", choices=["auto", "cpu", "cuda"], default="auto")
    parser.add_argument("--source-separation", choices=["0", "1"], default="1")
    parser.add_argument("--beat-tracking", choices=["0", "1"], default="1")
    parser.add_argument("--drum-transcription", choices=["0", "1"], default="1")
    parser.add_argument("--pitch-transcription", choices=["0", "1"], default="1")
    parser.add_argument("--vocal-refinement", choices=["0", "1"], default="1")
    parser.add_argument("--health", action="store_true")
    parser.add_argument("--deep-health", action="store_true")
    parser.add_argument("--cache", choices=["0", "1"], default="1")
    parser.add_argument("--compact-models", choices=["0", "1"], default="1")
    parser.add_argument("--run-cache-budget-mb", type=int, default=1024)
    args = parser.parse_args()

    output = Path(args.output).resolve()
    workspace_owned = not bool(args.workspace)
    workspace = (
        Path(args.workspace).resolve()
        if args.workspace
        else Path(tempfile.mkdtemp(prefix="pulseforge-autochart-ml-"))
    )
    ffmpeg = Path(args.ffmpeg)
    cache_root = Path(args.cache_root).resolve()
    workspace.mkdir(parents=True, exist_ok=True)
    cache_root.mkdir(parents=True, exist_ok=True)

    # This is safe even after a hard kill: transactional directories include
    # their owner PID, and only directories belonging to dead processes vanish.
    cleanup_abandoned_incomplete_caches(cache_root)

    if args.health or args.deep_health:
        try:
            return run_health_check(
                output, workspace, ffmpeg, cache_root, args.device, bool(args.deep_health)
            )
        finally:
            if workspace_owned:
                shutil.rmtree(workspace, ignore_errors=True)

    if not args.input:
        parser.error("--input is required unless --health/--deep-health is used")
    source = Path(args.input).resolve()

    source_sep_enabled = args.source_separation == "1"
    beat_enabled = args.beat_tracking == "1"
    drum_enabled = args.drum_transcription == "1"
    pitch_enabled = args.pitch_transcription == "1"
    vocal_enabled = args.vocal_refinement == "1"
    cache_enabled = args.cache == "1"
    compact_models = args.compact_models == "1"
    run_cache_budget_bytes = max(64, min(args.run_cache_budget_mb, 16 * 1024)) * 1024 * 1024

    key = stable_key(
        source, args.mode, source_sep_enabled, beat_enabled, drum_enabled,
        pitch_enabled, vocal_enabled, compact_models
    )

    final_cache_dir = cache_root / key
    cached_result_path = final_cache_dir / "result.json"

    if cache_enabled:
        cached = load_cached(cached_result_path)
        if cached is not None:
            cached["cacheHit"] = True
            write_json_atomic(output, cached)
            log(f"cache hit {key[:16]}")
            if workspace_owned:
                shutil.rmtree(workspace, ignore_errors=True)
            return 0

    # Never write multi-GB analysis/stems directly into the completed cache.
    # On success this directory is renamed to <key>; on failure/cancel it is
    # removed. A hard kill leaves only an explicitly marked .incomplete dir,
    # which the next AutoChart run safely sweeps if its owner PID is dead.
    if cache_enabled:
        work_cache_dir = cache_root / (
            f"{INCOMPLETE_CACHE_PREFIX}{key}-{os.getpid()}-{time.time_ns()}"
        )
    else:
        work_cache_dir = workspace / "ml-artifacts"

    shutil.rmtree(work_cache_dir, ignore_errors=True)
    work_cache_dir.mkdir(parents=True, exist_ok=True)
    cache_dir = work_cache_dir

    model_cache = cache_root / "models"
    model_cache.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault("TORCH_HOME", str(model_cache / "torch"))
    os.environ.setdefault("XDG_CACHE_HOME", str(model_cache / "xdg"))
    os.environ.setdefault("HF_HOME", str(model_cache / "huggingface"))

    diagnostics: list[str] = []
    payload: dict[str, Any] = {
        "schema": SCHEMA,
        "pipelineRevision": PIPELINE_REVISION,
        "ok": False,
        "cacheHit": False,
        "device": "cpu",
        "stages": {
            "sourceSeparation": False,
            "beatTracking": False,
            "drumTranscription": False,
            "pitchTranscription": False,
            "vocalRefinement": False,
        },
        "stems": {},
        "beatsSeconds": [],
        "downbeatsSeconds": [],
        "drumEventsPath": "",
        "drumEventCount": 0,
        "pitchEventsPath": "",
        "pitchEventCount": 0,
        "vocalEventsPath": "",
        "phonemeEventCount": 0,
        "syllableEventCount": 0,
        "diagnostics": diagnostics,
    }

    previous_handlers = _install_interrupt_handlers()
    promoted = False

    try:
        device = pick_device(args.device)
        payload["device"] = device
        analysis_wav = cache_dir / "analysis.wav"
        if not analysis_wav.is_file():
            log("creating canonical analysis WAV")
            make_analysis_wav(ffmpeg, source, analysis_wav)

        stems: dict[str, str] = {}
        if source_sep_enabled:
            try:
                log("running Demucs source separation")
                stems = separate_stems(
                    analysis_wav, cache_dir / "demucs", args.mode, device, compact_models
                )
                # FNF vocal-first policy: only the vocal stem may become note evidence.
                payload["stems"] = (
                    {"vocals": stems["vocals"]} if stems.get("vocals") else {}
                )
                payload["stages"]["sourceSeparation"] = bool(stems.get("vocals"))
            except Exception as exc:
                diagnostics.append(f"source separation unavailable: {exc}")
                log(diagnostics[-1])

        if beat_enabled:
            try:
                log("running Beat This! neural beat/downbeat tracker")
                beats, downbeats = neural_beats(analysis_wav, device)
                payload["beatsSeconds"] = beats
                payload["downbeatsSeconds"] = downbeats
                payload["stages"]["beatTracking"] = len(beats) >= 4
            except Exception as exc:
                diagnostics.append(f"neural beat tracking unavailable: {exc}")
                log(diagnostics[-1])

        if drum_enabled:
            try:
                drum_source = Path(stems["drums"]) if stems.get("drums") else analysis_wav
                log("running ADTOF neural drum transcription")
                drum_events = transcribe_drums(drum_source, device)
                drum_path = cache_dir / "drum-events.tsv"
                write_drum_events_tsv(drum_path, drum_events)
                payload["drumEventsPath"] = str(drum_path.resolve())
                payload["drumEventCount"] = len(drum_events)
                payload["stages"]["drumTranscription"] = bool(drum_events)
            except Exception as exc:
                diagnostics.append(f"drum transcription unavailable: {exc}")
                log(diagnostics[-1])

        if pitch_enabled:
            try:
                if not stems.get("vocals"):
                    raise RuntimeError(
                        "pitch transcription refused full-mix fallback: "
                        "a separated vocals stem is required"
                    )
                pitch_events = transcribe_stems(stems, workspace, args.mode)
                pitch_path = cache_dir / "pitch-events.tsv"
                write_pitch_events_tsv(pitch_path, pitch_events)
                payload["pitchEventsPath"] = str(pitch_path.resolve())
                payload["pitchEventCount"] = len(pitch_events)
                payload["stages"]["pitchTranscription"] = bool(pitch_events)
            except Exception as exc:
                diagnostics.append(f"pitch transcription unavailable: {exc}")
                log(diagnostics[-1])

        if vocal_enabled:
            try:
                vocal_source_text = stems.get("vocals", "") if stems else ""
                if not vocal_source_text:
                    raise RuntimeError("vocal refinement requires a separated vocal stem")
                log("running multilingual phoneme/syllable refinement on vocal stem")
                vocal_events = transcribe_vocal_phonemes(Path(vocal_source_text), device)
                vocal_path = cache_dir / "vocal-events.tsv"
                write_vocal_events_tsv(vocal_path, vocal_events)
                payload["vocalEventsPath"] = str(vocal_path.resolve())
                payload["phonemeEventCount"] = sum(
                    1 for event in vocal_events if event.get("role") == "phoneme"
                )
                payload["syllableEventCount"] = sum(
                    1 for event in vocal_events if event.get("role") == "syllable"
                )
                payload["stages"]["vocalRefinement"] = payload["syllableEventCount"] > 0
            except Exception as exc:
                diagnostics.append(f"vocal refinement unavailable: {exc}")
                log(diagnostics[-1])

        vocal_evidence_ok = bool(
            payload["stages"]["sourceSeparation"]
            and (
                payload["stages"]["pitchTranscription"]
                or payload["stages"]["vocalRefinement"]
            )
        )
        payload["ok"] = vocal_evidence_ok
        if not payload["ok"]:
            raise RuntimeError(
                "vocal-focused AutoChart could not obtain reliable vocal evidence; "
                "Demucs vocals plus Basic Pitch or vocal refinement are required"
            )

        if cache_enabled:
            _compact_completed_run_artifacts(work_cache_dir, payload, ffmpeg)
            # If another process completed the same key while this run was busy,
            # prefer that already-valid cache and discard this duplicate work.
            concurrent = load_cached(cached_result_path)
            if concurrent is not None:
                shutil.rmtree(work_cache_dir, ignore_errors=True)
                concurrent["cacheHit"] = True
                write_json_atomic(output, concurrent)
                return 0

            _promote_completed_cache(work_cache_dir, final_cache_dir, payload)
            promoted = True
            write_json_atomic(cached_result_path, payload)
            prune_completed_run_caches(
                cache_root, run_cache_budget_bytes, keep=final_cache_dir
            )

        write_json_atomic(output, payload)
        return 0

    except KeyboardInterrupt as exc:
        payload["error"] = "AutoChart cancelled"
        payload["ok"] = False
        diagnostics.append(str(exc))
        try:
            write_json_atomic(output, payload)
        except Exception:
            pass
        log("AutoChart cancelled; removing incomplete ML artifacts")
        return 130

    except Exception as exc:
        diagnostics.append(str(exc))
        diagnostics.append(traceback.format_exc(limit=5))
        payload["error"] = str(exc)
        payload["ok"] = False
        write_json_atomic(output, payload)
        return 2

    finally:
        _restore_interrupt_handlers(previous_handlers)

        if cache_enabled and not promoted and work_cache_dir.exists():
            shutil.rmtree(work_cache_dir, ignore_errors=True)

        # Only delete a workspace that this Python process created itself.
        # The C++ caller owns --workspace and removes it after consuming result files.
        if workspace_owned:
            shutil.rmtree(workspace, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
