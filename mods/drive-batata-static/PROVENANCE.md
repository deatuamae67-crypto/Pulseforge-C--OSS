# Drive `batata` static recovery

Imported on 2026-08-01 from the user-authorized Google Drive folder
[`batata`](https://drive.google.com/drive/folders/1ogisLRIMYvSUKicF38CW6ppQ3GrgSyGY).
Only static chart/audio data was copied. Executables, DLLs, plugins and the
nested compiled engine distributions were neither copied nor executed.

The audit compared 186 `assets/data` song folders against the eight local ZIP
snapshots and the existing Drive library. It retained the material not already
represented locally: `percipience`, five historical `ingratiating` charts, and
the missing `corrosion`/`ingratiating`/`percipience` stems. Duplicate chart
display names are disambiguated by the engine catalog; source JSON filenames
and bytes remain unchanged.

## SHA-256 inventory

```text
assets/data/ingratiating/ingratiating-backup-easy-1.json cf856eeb11810a3717add3493ac1b830beb371690404979d3a09268c083c1f3d
assets/data/ingratiating/ingratiating-backup-easy-2.json d997536f371bf713826bfbf7d06a39078ea1430cf732493d1e1fbfad62e77d5d
assets/data/ingratiating/ingratiating-backup-easy-3.json 636c34a60f60880562420823f62c1f51a53e77be6aa8447202810883b46ee48c
assets/data/ingratiating/ingratiating-backup-normal-1.json a0de47aa831205b8f58757425c9901e1d2ed645693d5c3f095b98e7ec9388a0b
assets/data/ingratiating/ingratiating-backup-normal-2.json e4f77f0c43986971862614ff0cd345e4cdb31d56d2a6ba671a363006dfbb61cd
assets/data/percipience/events.json 432672da43308e9d3c5ac89957a329acd8430dcefecece4a5fda3da21587f94b
assets/data/percipience/percipience.json 47f08dccad62b4e681357dfd7b634b7226b503b7acf5c891b305a7f86749e065
assets/songs/corrosion/Inst.ogg 2773029bcc353b75e7c3bc5844dcfeedd79dc440876d3fd8fc896caa60fc0389
assets/songs/ingratiating/Inst.ogg b7bc27c90407e73efb15d00f13a55bb4b75ba7e74606d02a6dada2c201268205
assets/songs/percipience/Inst.ogg e81334df8a1c314f39a03b1e4c56dcaa73dc3bd93fc9445f7d714650933129ef
source-duplicates/percipience-Voices-identical-to-Inst.ogg e81334df8a1c314f39a03b1e4c56dcaa73dc3bd93fc9445f7d714650933129ef
```

The identical `percipience` Inst/Voices hashes reflect the bytes supplied by
Drive. The original Voices bytes are retained under `source-duplicates`, but
kept outside automatic stem discovery so the same waveform is not mixed twice.
PulseForge does not rewrite either source stem.
