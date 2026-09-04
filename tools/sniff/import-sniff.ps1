[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $SniffExecutable,

    [Parameter(Mandatory = $true)]
    [string] $InputPath,

    [Parameter(Mandatory = $true)]
    [string] $OutputPath,

    [ValidatePattern('^[A-Fa-f0-9]{64}$')]
    [string] $ExpectedSha256 = '182387A573EC9E71BC3E3341632831A63655808153A0DAAB6A228383E26550B8',

    [ValidateSet('auto', 'flp2json', 'json2flp')]
    [string] $Direction = 'auto',

    [double] $Bpm,
    [double] $Multiplier,
    [double] $OffsetMs,
    [double] $ScrollSpeed,
    [string] $SongName,
    [string] $Player1,
    [string] $Player2,
    [string] $Girlfriend,
    [string] $Stage,
    [switch] $Pretty,
    [switch] $NoVoices,
    [switch] $NoLength,
    [switch] $HashOutput,
    [switch] $Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Resolve-ExistingFile {
    param(
        [Parameter(Mandatory = $true)]
        [string] $LiteralPath,

        [Parameter(Mandatory = $true)]
        [string] $Description
    )

    $resolved = @(Resolve-Path -LiteralPath $LiteralPath -ErrorAction Stop)
    if ($resolved.Count -ne 1) {
        throw "$Description must resolve to exactly one file."
    }

    $item = Get-Item -LiteralPath $resolved[0].Path -Force
    if (-not $item.PSIsContainer) {
        return [System.IO.Path]::GetFullPath($item.FullName)
    }
    throw "$Description is not a file: $LiteralPath"
}

function Assert-FiniteNumber {
    param(
        [Parameter(Mandatory = $true)]
        [double] $Value,

        [Parameter(Mandatory = $true)]
        [string] $Name
    )

    if ([double]::IsNaN($Value) -or [double]::IsInfinity($Value)) {
        throw "$Name must be a finite number."
    }
}

function Get-Sha256Hex {
    param(
        [Parameter(Mandatory = $true)]
        [string] $LiteralPath
    )

    # Use the framework primitive directly so the guarded converter remains
    # usable in minimal PowerShell hosts where Microsoft.PowerShell.Utility
    # is not available for Get-FileHash auto-loading.
    $stream = [System.IO.File]::Open(
        $LiteralPath,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::Read
    )
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        return [System.BitConverter]::ToString(
            $sha256.ComputeHash($stream)
        ).Replace('-', '')
    }
    finally {
        $sha256.Dispose()
        $stream.Dispose()
    }
}

function Test-ConvertedFileSignature {
    param(
        [Parameter(Mandatory = $true)]
        [string] $LiteralPath,

        [Parameter(Mandatory = $true)]
        [string] $ExpectedExtension
    )

    $stream = [System.IO.File]::OpenRead($LiteralPath)
    try {
        if ($ExpectedExtension -eq '.flp') {
            $magic = New-Object byte[] 4
            if ($stream.Read($magic, 0, $magic.Length) -ne 4) {
                return $false
            }
            return [System.Text.Encoding]::ASCII.GetString($magic) -eq 'FLhd'
        }

        $prefix = New-Object byte[] 4096
        $read = $stream.Read($prefix, 0, $prefix.Length)
        if ($read -le 0) {
            return $false
        }
        $text = [System.Text.Encoding]::UTF8.GetString($prefix, 0, $read).TrimStart()
        if ($text.Length -gt 0 -and $text[0] -eq [char] 0xFEFF) {
            $text = $text.Substring(1).TrimStart()
        }
        return $text.StartsWith('{')
    }
    finally {
        $stream.Dispose()
    }
}

$resolvedExecutable = Resolve-ExistingFile -LiteralPath $SniffExecutable -Description 'SNIFF executable'
$resolvedInput = Resolve-ExistingFile -LiteralPath $InputPath -Description 'input'

if ([System.IO.Path]::GetExtension($resolvedExecutable).ToLowerInvariant() -ne '.exe') {
    throw 'SNIFF executable must be a Windows .exe file.'
}

$actualSha256 = (Get-Sha256Hex -LiteralPath $resolvedExecutable).ToUpperInvariant()
$wantedSha256 = $ExpectedSha256.ToUpperInvariant()
if ($actualSha256 -ne $wantedSha256) {
    throw "SNIFF SHA-256 mismatch. Expected $wantedSha256 but found $actualSha256. The process was not started."
}

$inputExtension = [System.IO.Path]::GetExtension($resolvedInput).ToLowerInvariant()
if ($Direction -eq 'auto') {
    switch ($inputExtension) {
        '.flp' { $Direction = 'flp2json' }
        '.json' { $Direction = 'json2flp' }
        default { throw 'Automatic direction accepts only .flp or .json input files.' }
    }
}

$expectedInputExtension = if ($Direction -eq 'flp2json') { '.flp' } else { '.json' }
$expectedOutputExtension = if ($Direction -eq 'flp2json') { '.json' } else { '.flp' }
if ($inputExtension -ne $expectedInputExtension) {
    throw "Direction $Direction requires a $expectedInputExtension input file."
}

$resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
if ([System.IO.Path]::GetExtension($resolvedOutput).ToLowerInvariant() -ne $expectedOutputExtension) {
    throw "Direction $Direction requires a $expectedOutputExtension output file."
}
if ($resolvedOutput.Equals($resolvedInput, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'Input and output must be different files.'
}
if ($resolvedOutput.Equals($resolvedExecutable, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'Output cannot overwrite the converter executable.'
}
if ((Test-Path -LiteralPath $resolvedOutput) -and -not $Force) {
    throw "Output already exists: $resolvedOutput. Use -Force to replace it after a successful conversion."
}

foreach ($numericParameter in @('Bpm', 'Multiplier', 'OffsetMs', 'ScrollSpeed')) {
    if ($PSBoundParameters.ContainsKey($numericParameter)) {
        Assert-FiniteNumber -Value ([double] $PSBoundParameters[$numericParameter]) -Name $numericParameter
    }
}
if ($PSBoundParameters.ContainsKey('Bpm') -and $Bpm -le 0) {
    throw 'Bpm must be greater than zero.'
}
if ($PSBoundParameters.ContainsKey('Multiplier') -and $Multiplier -le 0) {
    throw 'Multiplier must be greater than zero.'
}
if ($PSBoundParameters.ContainsKey('ScrollSpeed') -and $ScrollSpeed -le 0) {
    throw 'ScrollSpeed must be greater than zero.'
}

$outputDirectory = [System.IO.Path]::GetDirectoryName($resolvedOutput)
if ([string]::IsNullOrWhiteSpace($outputDirectory)) {
    throw 'Output path must have a parent directory.'
}
if (-not (Test-Path -LiteralPath $outputDirectory -PathType Container)) {
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
}

$temporaryName = '.pulseforge-sniff-' + [guid]::NewGuid().ToString('N') + $expectedOutputExtension
$temporaryOutput = Join-Path $outputDirectory $temporaryName
$arguments = @(
    '--mode', 'single',
    '--dir', $Direction,
    '--out', $temporaryOutput
)

if ($PSBoundParameters.ContainsKey('Bpm')) { $arguments += @('--bpm', $Bpm.ToString('R', [Globalization.CultureInfo]::InvariantCulture)) }
if ($PSBoundParameters.ContainsKey('Multiplier')) { $arguments += @('--mult', $Multiplier.ToString('R', [Globalization.CultureInfo]::InvariantCulture)) }
if ($PSBoundParameters.ContainsKey('OffsetMs')) { $arguments += @('--offset', $OffsetMs.ToString('R', [Globalization.CultureInfo]::InvariantCulture)) }
if ($PSBoundParameters.ContainsKey('ScrollSpeed')) { $arguments += @('--speed', $ScrollSpeed.ToString('R', [Globalization.CultureInfo]::InvariantCulture)) }
if ($PSBoundParameters.ContainsKey('SongName')) { $arguments += @('--name', $SongName) }
if ($PSBoundParameters.ContainsKey('Player1')) { $arguments += @('--p1', $Player1) }
if ($PSBoundParameters.ContainsKey('Player2')) { $arguments += @('--p2', $Player2) }
if ($PSBoundParameters.ContainsKey('Girlfriend')) { $arguments += @('--gf', $Girlfriend) }
if ($PSBoundParameters.ContainsKey('Stage')) { $arguments += @('--stage', $Stage) }
if ($Pretty) { $arguments += '--pretty' }
if ($NoVoices) { $arguments += '--no-voices' }
if ($NoLength) { $arguments += '--no-length' }
$arguments += $resolvedInput

try {
    Write-Host "Verified SNIFF SHA-256: $actualSha256"
    Write-Host "Converting to temporary output: $temporaryOutput"

    # PowerShell's call operator receives each array item as one process argument;
    # input paths are never concatenated into a command line or evaluated as script.
    & $resolvedExecutable @arguments
    $converterExitCode = $LASTEXITCODE
    if ($converterExitCode -ne 0) {
        throw "SNIFF exited with code $converterExitCode."
    }
    if (-not (Test-Path -LiteralPath $temporaryOutput -PathType Leaf)) {
        throw 'SNIFF reported success but did not create the expected output.'
    }
    if ((Get-Item -LiteralPath $temporaryOutput).Length -le 0) {
        throw 'SNIFF produced an empty output file.'
    }
    if (-not (Test-ConvertedFileSignature -LiteralPath $temporaryOutput -ExpectedExtension $expectedOutputExtension)) {
        throw "SNIFF output does not have a valid $expectedOutputExtension signature."
    }

    Move-Item -LiteralPath $temporaryOutput -Destination $resolvedOutput -Force:$Force
    $outputHash = if ($HashOutput) {
        Get-Sha256Hex -LiteralPath $resolvedOutput
    }
    else {
        $null
    }
    Write-Output ([pscustomobject]@{
        Output = $resolvedOutput
        Size = (Get-Item -LiteralPath $resolvedOutput).Length
        Sha256 = $outputHash
        ConverterSha256 = $actualSha256
    })
}
finally {
    if (Test-Path -LiteralPath $temporaryOutput -PathType Leaf) {
        $temporaryFullPath = [System.IO.Path]::GetFullPath($temporaryOutput)
        $expectedParent = [System.IO.Path]::GetFullPath($outputDirectory).TrimEnd('\') + '\'
        if ($temporaryFullPath.StartsWith($expectedParent, [System.StringComparison]::OrdinalIgnoreCase) -and
            [System.IO.Path]::GetFileName($temporaryFullPath).StartsWith('.pulseforge-sniff-', [System.StringComparison]::Ordinal)) {
            Remove-Item -LiteralPath $temporaryFullPath -Force
        }
    }
}
