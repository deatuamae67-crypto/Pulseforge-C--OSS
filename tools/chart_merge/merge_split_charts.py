#!/usr/bin/env python3
"""PulseForge bounded-memory merger for split Psych/FNF JSON charts.

The merger indexes byte ranges for each ``sectionNotes`` array and streams
individual note JSON values. It never materializes a complete chart or a
complete note set in RAM.

Strategies:
- kway (default): O(number of parts) RAM; every part must already be ordered by
  strumTime inside each section. This is the fast path for very large charts.
- external-sort: accepts arbitrary note order using bounded temporary runs.
- concat: preserves natural part order without sorting.
"""

from __future__ import annotations

import argparse
import contextlib
import heapq
import json
import math
import os
import re
import shutil
import struct
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Iterable, Iterator, Sequence

KEY = b'"sectionNotes"'
COPY_CHUNK = 8 * 1024 * 1024
READ_CHUNK = 1024 * 1024
_RUN_HEADER = struct.Struct("<dQI")


class MergeError(RuntimeError):
    pass


@dataclass(frozen=True)
class ArrayRange:
    content_start: int
    content_end: int


@dataclass(frozen=True)
class IndexedChart:
    path: Path
    sections: tuple[ArrayRange, ...]
    size: int


def _natural_key(path: Path) -> tuple[object, ...]:
    pieces = re.split(r"(\d+)", path.name.casefold())
    return tuple(int(piece) if piece.isdigit() else piece for piece in pieces)


def _skip_ws(stream: BinaryIO, position: int, size: int) -> int:
    stream.seek(position)
    while position < size:
        chunk = stream.read(min(4096, size - position))
        if not chunk:
            return position
        for offset, value in enumerate(chunk):
            if value not in b" \t\r\n":
                return position + offset
        position += len(chunk)
    return position


def _find_matching_array_end(stream: BinaryIO, open_pos: int, size: int) -> int:
    stream.seek(open_pos)
    if stream.read(1) != b"[":
        raise MergeError(f"internal parser error at byte {open_pos}: expected '['")
    depth = 1
    in_string = False
    escaped = False
    absolute = open_pos + 1
    while absolute < size:
        chunk = stream.read(min(READ_CHUNK, size - absolute))
        if not chunk:
            break
        for offset, value in enumerate(chunk):
            if in_string:
                if escaped:
                    escaped = False
                elif value == 0x5C:
                    escaped = True
                elif value == 0x22:
                    in_string = False
                continue
            if value == 0x22:
                in_string = True
            elif value == 0x5B:
                depth += 1
            elif value == 0x5D:
                depth -= 1
                if depth == 0:
                    return absolute + offset
        absolute += len(chunk)
    raise MergeError(f"unterminated sectionNotes array beginning at byte {open_pos}")


def index_chart(path: Path) -> IndexedChart:
    try:
        size = path.stat().st_size
    except OSError as exc:
        raise MergeError(f"cannot stat {path}: {exc}") from exc
    if size == 0:
        raise MergeError(f"empty chart: {path}")

    sections: list[ArrayRange] = []
    with path.open("rb", buffering=READ_CHUNK) as stream:
        position = 0
        overlap = len(KEY) - 1
        carry = b""
        while position < size:
            stream.seek(position)
            chunk = stream.read(min(READ_CHUNK, size - position))
            if not chunk:
                break
            data = carry + chunk
            base = position - len(carry)
            search_at = 0
            found_jump = False
            while True:
                local = data.find(KEY, search_at)
                if local < 0:
                    break
                key_pos = base + local
                colon_pos = _skip_ws(stream, key_pos + len(KEY), size)
                stream.seek(colon_pos)
                if stream.read(1) != b":":
                    search_at = local + 1
                    continue
                open_pos = _skip_ws(stream, colon_pos + 1, size)
                stream.seek(open_pos)
                if stream.read(1) != b"[":
                    raise MergeError(
                        f"{path}: sectionNotes at byte {key_pos} is not an array"
                    )
                close_pos = _find_matching_array_end(stream, open_pos, size)
                sections.append(ArrayRange(open_pos + 1, close_pos))
                position = close_pos + 1
                carry = b""
                found_jump = True
                break
            if found_jump:
                continue
            keep = min(overlap, len(data))
            carry = data[-keep:] if keep else b""
            position += len(chunk)

    if not sections:
        raise MergeError(f"{path}: no sectionNotes arrays found")
    return IndexedChart(path=path, sections=tuple(sections), size=size)


class ArrayElements(Iterator[bytes]):
    """Stream top-level JSON values from one indexed array-content range."""

    def __init__(self, stream: BinaryIO, array_range: ArrayRange):
        self.stream = stream
        self.end = array_range.content_end
        self.position = array_range.content_start
        self.done = False

    def __iter__(self) -> "ArrayElements":
        return self

    def _read_byte(self) -> int | None:
        if self.position >= self.end:
            return None
        self.stream.seek(self.position)
        raw = self.stream.read(1)
        if not raw:
            return None
        self.position += 1
        return raw[0]

    def __next__(self) -> bytes:
        if self.done:
            raise StopIteration
        while True:
            value = self._read_byte()
            if value is None:
                self.done = True
                raise StopIteration
            if value not in b" \t\r\n,":
                break

        out = bytearray([value])
        in_string = value == 0x22
        escaped = False
        braces = 1 if value == 0x7B else 0
        brackets = 1 if value == 0x5B else 0
        primitive = braces == 0 and brackets == 0 and not in_string

        while True:
            value = self._read_byte()
            if value is None:
                self.done = True
                raw = bytes(out).strip()
                if raw:
                    return raw
                raise StopIteration
            if in_string:
                out.append(value)
                if escaped:
                    escaped = False
                elif value == 0x5C:
                    escaped = True
                elif value == 0x22:
                    in_string = False
                    if braces == 0 and brackets == 0:
                        return bytes(out).strip()
                continue
            if value == 0x22:
                in_string = True
                out.append(value)
            elif value == 0x7B:
                braces += 1
                out.append(value)
            elif value == 0x7D:
                braces -= 1
                if braces < 0:
                    raise MergeError("unbalanced object inside sectionNotes")
                out.append(value)
                if braces == 0 and brackets == 0:
                    return bytes(out).strip()
            elif value == 0x5B:
                brackets += 1
                out.append(value)
            elif value == 0x5D:
                brackets -= 1
                if brackets < 0:
                    raise MergeError("unbalanced array inside sectionNotes")
                out.append(value)
                if braces == 0 and brackets == 0:
                    return bytes(out).strip()
            elif primitive and braces == 0 and brackets == 0 and value in b", \t\r\n":
                return bytes(out).strip()
            else:
                out.append(value)


def _note_time(raw: bytes) -> float:
    view = raw.lstrip()
    if not view.startswith(b"["):
        raise MergeError("sectionNotes element is not an array")
    index = 1
    while index < len(view) and view[index] in b" \t\r\n":
        index += 1
    start = index
    while index < len(view) and view[index] not in b",] \t\r\n":
        index += 1
    token = view[start:index]
    if not token:
        raise MergeError("note is missing strumTime")
    try:
        value = float(token.decode("ascii"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise MergeError(f"invalid note strumTime: {token[:64]!r}") from exc
    if not math.isfinite(value):
        raise MergeError("non-finite note strumTime")
    return value


def _write_joined(output: BinaryIO, values: Iterable[bytes]) -> int:
    first = True
    count = 0
    for raw in values:
        if not raw:
            continue
        if not first:
            output.write(b",")
        output.write(raw)
        first = False
        count += 1
    return count


def _kway_values(iterators: Sequence[Iterator[bytes]]) -> Iterator[bytes]:
    heap: list[tuple[float, int, int, bytes, Iterator[bytes]]] = []
    serial = 0
    previous_by_source: dict[int, float] = {}
    for source_index, iterator in enumerate(iterators):
        try:
            raw = next(iterator)
        except StopIteration:
            continue
        key = _note_time(raw)
        previous_by_source[source_index] = key
        heapq.heappush(heap, (key, source_index, serial, raw, iterator))
        serial += 1

    while heap:
        _, source_index, _, raw, iterator = heapq.heappop(heap)
        yield raw
        try:
            next_raw = next(iterator)
        except StopIteration:
            continue
        next_key = _note_time(next_raw)
        if next_key < previous_by_source[source_index]:
            raise MergeError(
                "source section is not time-sorted; rerun with --strategy external-sort"
            )
        previous_by_source[source_index] = next_key
        heapq.heappush(
            heap, (next_key, source_index, serial, next_raw, iterator)
        )
        serial += 1


def _write_run(path: Path, records: list[tuple[float, int, bytes]]) -> None:
    records.sort(key=lambda item: (item[0], item[1]))
    with path.open("wb") as stream:
        for key, serial, raw in records:
            if len(raw) > 0xFFFFFFFF:
                raise MergeError("single note JSON value exceeds 4 GiB")
            stream.write(_RUN_HEADER.pack(key, serial, len(raw)))
            stream.write(raw)


def _read_run_record(stream: BinaryIO) -> tuple[float, int, bytes] | None:
    header = stream.read(_RUN_HEADER.size)
    if not header:
        return None
    if len(header) != _RUN_HEADER.size:
        raise MergeError("truncated external-sort run")
    key, serial, length = _RUN_HEADER.unpack(header)
    raw = stream.read(length)
    if len(raw) != length:
        raise MergeError("truncated external-sort note payload")
    return key, serial, raw


def _external_sorted_values(
    iterators: Sequence[Iterator[bytes]], temp_root: Path, memory_limit: int
) -> Iterator[bytes]:
    runs: list[Path] = []
    records: list[tuple[float, int, bytes]] = []
    used = 0
    serial = 0

    def flush() -> None:
        nonlocal records, used
        if not records:
            return
        run = temp_root / f"run-{len(runs):06d}.bin"
        _write_run(run, records)
        runs.append(run)
        records = []
        used = 0

    for iterator in iterators:
        for raw in iterator:
            records.append((_note_time(raw), serial, raw))
            used += len(raw) + 40
            serial += 1
            if used >= memory_limit:
                flush()
    flush()

    if not runs:
        return
    if len(runs) == 1:
        with runs[0].open("rb") as stream:
            while (record := _read_run_record(stream)) is not None:
                yield record[2]
        return

    with contextlib.ExitStack() as stack:
        streams = [stack.enter_context(path.open("rb")) for path in runs]
        heap: list[tuple[float, int, int, bytes]] = []
        for run_index, stream in enumerate(streams):
            record = _read_run_record(stream)
            if record is not None:
                key, serial_value, raw = record
                heapq.heappush(heap, (key, serial_value, run_index, raw))
        while heap:
            _, _, run_index, raw = heapq.heappop(heap)
            yield raw
            record = _read_run_record(streams[run_index])
            if record is not None:
                key, serial_value, next_raw = record
                heapq.heappush(heap, (key, serial_value, run_index, next_raw))


def _copy_range(source: BinaryIO, output: BinaryIO, start: int, end: int) -> None:
    if end < start:
        raise MergeError("invalid template copy range")
    source.seek(start)
    remaining = end - start
    while remaining:
        block = source.read(min(COPY_CHUNK, remaining))
        if not block:
            raise MergeError("template ended while copying output")
        output.write(block)
        remaining -= len(block)


def merge_charts(
    inputs: Sequence[Path],
    output: Path,
    strategy: str,
    sort_memory_mib: int,
    temp_dir: Path | None,
) -> tuple[int, int, Path]:
    if len(inputs) < 2:
        raise MergeError("at least two input charts are required")
    resolved_output = output.resolve()
    if any(path.resolve() == resolved_output for path in inputs):
        raise MergeError("output path must not overwrite an input chart")

    indexed = [index_chart(path) for path in inputs]
    template = max(indexed, key=lambda item: len(item.sections))
    section_count = len(template.sections)
    for item in indexed:
        if len(item.sections) != section_count:
            raise MergeError(
                f"section-count mismatch: {item.path} has {len(item.sections)}, "
                f"template {template.path} has {section_count}"
            )

    output.parent.mkdir(parents=True, exist_ok=True)
    temp_parent = temp_dir if temp_dir is not None else output.parent
    temp_parent.mkdir(parents=True, exist_ok=True)
    temp_output = output.with_name(output.name + ".partial")
    temp_root = Path(
        tempfile.mkdtemp(prefix="pulseforge-chart-merge-", dir=str(temp_parent))
    )
    total_notes = 0

    try:
        with contextlib.ExitStack() as stack:
            template_stream = stack.enter_context(template.path.open("rb", buffering=READ_CHUNK))
            input_streams = [
                stack.enter_context(item.path.open("rb", buffering=READ_CHUNK))
                for item in indexed
            ]
            out = stack.enter_context(temp_output.open("wb", buffering=COPY_CHUNK))
            template_cursor = 0
            memory_limit = max(1, sort_memory_mib) * 1024 * 1024

            for section_index, template_range in enumerate(template.sections):
                _copy_range(
                    template_stream,
                    out,
                    template_cursor,
                    template_range.content_start,
                )
                iterators = [
                    ArrayElements(stream, item.sections[section_index])
                    for stream, item in zip(input_streams, indexed)
                ]
                if strategy == "concat":
                    values: Iterable[bytes] = (
                        raw for iterator in iterators for raw in iterator
                    )
                elif strategy == "kway":
                    values = _kway_values(iterators)
                elif strategy == "external-sort":
                    section_temp = temp_root / f"section-{section_index:06d}"
                    section_temp.mkdir()
                    values = _external_sorted_values(iterators, section_temp, memory_limit)
                else:
                    raise MergeError(f"unknown strategy: {strategy}")
                total_notes += _write_joined(out, values)
                template_cursor = template_range.content_end
                if strategy == "external-sort":
                    shutil.rmtree(temp_root / f"section-{section_index:06d}")

            _copy_range(template_stream, out, template_cursor, template.size)
            out.flush()
            os.fsync(out.fileno())
        os.replace(temp_output, output)
    except BaseException:
        with contextlib.suppress(OSError):
            temp_output.unlink()
        raise
    finally:
        shutil.rmtree(temp_root, ignore_errors=True)

    return section_count, total_notes, template.path


def _discover_inputs(arguments: Sequence[str]) -> list[Path]:
    paths: list[Path] = []
    for raw in arguments:
        path = Path(raw)
        if path.is_dir():
            paths.extend(
                child
                for child in path.iterdir()
                if child.is_file() and child.suffix.casefold() == ".json"
            )
        else:
            paths.append(path)
    normalized: list[Path] = []
    seen: set[str] = set()
    for path in sorted(paths, key=_natural_key):
        if path.suffix.casefold() != ".json":
            raise MergeError(f"input is not JSON: {path}")
        if not path.is_file():
            raise MergeError(f"input chart does not exist: {path}")
        key = os.path.normcase(str(path.resolve()))
        if key in seen:
            continue
        seen.add(key)
        normalized.append(path)
    return normalized


def _safe_name(value: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9._ -]+", "-", value).strip(" .-")
    return cleaned or "merged-chart"


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Merge split Psych/FNF charts with bounded memory"
    )
    parser.add_argument("inputs", nargs="+", help="split chart JSON files or directories")
    parser.add_argument("-o", "--output", type=Path, help="explicit output JSON path")
    parser.add_argument(
        "--mod-root",
        type=Path,
        default=Path("mods/pulseforge-created"),
        help="PulseForge Created Content mod root used when --output is omitted",
    )
    parser.add_argument("--name", default="merged-chart", help="output chart name")
    parser.add_argument(
        "--strategy",
        choices=("kway", "external-sort", "concat"),
        default="kway",
        help="bounded-memory merge strategy (default: kway)",
    )
    parser.add_argument(
        "--sort-memory-mib",
        type=int,
        default=128,
        help="RAM budget for each external-sort run (default: 128 MiB)",
    )
    parser.add_argument("--temp-dir", type=Path, help="temporary-run directory")
    parser.add_argument("--report", type=Path, help="write JSON provenance report")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    try:
        inputs = _discover_inputs(args.inputs)
        if len(inputs) < 2:
            raise MergeError("need at least two distinct JSON charts after discovery")
        output = args.output
        if output is None:
            output = args.mod_root / "charts" / f"{_safe_name(args.name)}.json"
        section_count, note_count, template = merge_charts(
            inputs, output, args.strategy, args.sort_memory_mib, args.temp_dir
        )
        report = {
            "schema": "pulseforge-split-chart-merge-v1",
            "output": str(output),
            "template": str(template),
            "inputCount": len(inputs),
            "sectionCount": section_count,
            "noteCount": note_count,
            "strategy": args.strategy,
            "inputs": [str(path) for path in inputs],
        }
        encoded = json.dumps(report, indent=2, ensure_ascii=False) + "\n"
        if args.report is not None:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(encoded, encoding="utf-8")
        print(encoded, end="")
        return 0
    except MergeError as exc:
        print(f"PulseForge chart merge failed: {exc}", file=sys.stderr)
        return 2
    except OSError as exc:
        print(f"PulseForge chart merge I/O failure: {exc}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(main())
