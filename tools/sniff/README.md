# PulseForge SNIFF bridge

This directory contains a guarded, opt-in wrapper for a user-supplied SNIFF
executable. It does **not** contain or automatically download `sniff-rusted.exe`.

The Mods menu calls this wrapper for **Install an FLP chart
(SNIFF-RUSTED)**, but only after the user selects the FLP and confirms the
audited executable hash on a second screen. The normal startup and mod scanner
never launch it. A local executable can be placed under
`local-tools/sniff/sniff-rusted.exe`; that directory is deliberately excluded
from source control and packaged builds.

Why the separation matters:

- the supplied executable is not Authenticode-signed;
- the official `HaxePixel/SNIFF-RUSTED` repository has no `LICENSE` file and
  its `Cargo.toml` does not declare a license;
- GitHub does not publish an official release binary that can be compared with
  the supplied executable;
- matching program strings and expected PE imports are evidence of consistency,
  not a reproducible proof that the executable was built from the visible source.

The wrapper verifies a SHA-256 allowlist before process creation, passes paths as
separate arguments, refuses in-place conversion, writes to a same-directory
temporary file, validates the output signature and only then moves it to the
requested destination. A failed conversion cannot replace an existing chart.

## FLP to Psych-compatible JSON

```powershell
.\tools\sniff\import-sniff.ps1 `
  -SniffExecutable 'C:\path\to\sniff-rusted.exe' `
  -InputPath 'C:\charts\song.flp' `
  -OutputPath 'C:\charts\song.json'
```

## JSON to FLP

```powershell
.\tools\sniff\import-sniff.ps1 `
  -SniffExecutable 'C:\path\to\sniff-rusted.exe' `
  -InputPath 'C:\charts\song.json' `
  -OutputPath 'C:\charts\song.flp'
```

`-Direction auto` is the default. Optional conversion properties include
`-Bpm`, `-Multiplier`, `-OffsetMs`, `-ScrollSpeed`, `-SongName`, `-Player1`,
`-Player2`, `-Girlfriend`, `-Stage`, `-Pretty`, `-NoVoices` and `-NoLength`.
Use `-Force` only when replacing an existing output is intentional. `-HashOutput`
adds a full SHA-256 pass over the result; it is off by default so very large
charts are not read a second time.

The default allowlisted hash is the locally audited executable. To use a build
compiled independently from the official source, calculate its SHA-256 and pass
that deliberate trust decision with `-ExpectedSha256`. This does not grant a
right to redistribute the executable.

The in-engine flow intentionally exposes single-file FLP-to-JSON conversion.
It installs the result as a Psych-style chart under the Mods directory and can
then copy a separately selected instrumental. Character merge, difficulty
split, batch mode and JSON-to-FLP remain command-line-only until a licensed
in-process implementation can provide multi-output transaction semantics.

In the source tree, see `docs/SNIFF_INTEGRATION.md`. In an installed binary
package, it is located at `../../../docs/SNIFF_INTEGRATION.md` relative to this
README. It contains the complete CLI contract, formats, limitations and source
audit.
