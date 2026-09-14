# PulseForge Split Chart Merger

`merge_split_charts.py` combines notes from split Psych/FNF JSON charts without loading the complete charts into memory. It was added for very large charts such as multi-part exports where individual JSON files can be hundreds of MiB or larger.

## Usage

```bash
python3 tools/chart_merge/merge_split_charts.py \
  "FLP 6 part 1 chart.json" \
  "FLP 6 part 2 chart.json" \
  "FLP 6 part 3 chart.json" \
  --name Sonorous
```

When `--output` is omitted the result is written to:

```text
mods/pulseforge-created/charts/<name>.json
```

That is the same PulseForge-created content mod used by the in-engine editors.

A directory can be passed instead of enumerating files. JSON filenames are sorted naturally, so `part 2` comes before `part 10`.

## Strategies

- `--strategy kway` is the default. It keeps approximately one note per input stream in memory and merges by `strumTime`. Each source section must already be time-sorted. This is the preferred path for millions of notes.
- `--strategy external-sort` accepts arbitrary note order. It creates bounded temporary sorted runs and merges them. Control the run RAM budget with `--sort-memory-mib`.
- `--strategy concat` preserves natural part order exactly and performs no chronological sort.

The merger preserves the template chart metadata and replaces each `sectionNotes` array with the combined note stream. It intentionally does not deduplicate notes: every note from every input is retained. All inputs must have the same number of sections.

## Safety and scale

The tool indexes only byte ranges of `sectionNotes`, streams JSON note values, writes through a temporary `.partial` file, flushes it, and atomically replaces the destination on success. The default k-way path therefore scales with the number of split files rather than the total number of notes.

Use `--report path.json` to store a small provenance report containing input files, note count, section count, strategy, output path and template path.
