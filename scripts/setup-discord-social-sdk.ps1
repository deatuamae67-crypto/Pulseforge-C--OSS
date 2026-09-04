[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [string]$SdkPath,

    [string]$AndroidAar,

    [string]$Destination = (Join-Path (Split-Path $PSScriptRoot -Parent) 'third_party\discord_social_sdk'),

    [switch]$Force,
    [switch]$OpenDeveloperPortal
)

$ErrorActionPreference = 'Stop'

function Resolve-SdkRoot([string]$Path) {
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    if (Test-Path -LiteralPath $resolved -PathType Leaf) {
        if ([IO.Path]::GetExtension($resolved) -ne '.zip') {
            throw "Desktop Discord SDK archive must be a .zip or an already extracted directory: $resolved"
        }
        $temp = Join-Path ([IO.Path]::GetTempPath()) ("pulseforge-discord-sdk-" + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $temp | Out-Null
        Expand-Archive -LiteralPath $resolved -DestinationPath $temp -Force
        $script:TemporarySdkRoot = $temp
        $resolved = $temp
    }

    $header = Get-ChildItem -LiteralPath $resolved -Recurse -File -Filter 'discordpp.h' |
        Where-Object { $_.Directory.Name -eq 'include' } |
        Select-Object -First 1
    if ($null -eq $header) {
        throw "discordpp.h was not found under $resolved. Use the C++ Discord Social SDK archive from the Developer Portal."
    }
    $root = Split-Path $header.Directory.FullName -Parent
    if (-not (Test-Path -LiteralPath (Join-Path $root 'include\cdiscord.h') -PathType Leaf)) {
        throw "cdiscord.h is missing beside discordpp.h under $root\include"
    }
    return $root
}

$TemporarySdkRoot = $null
try {
    $sourceRoot = Resolve-SdkRoot $SdkPath
    $destinationFull = [IO.Path]::GetFullPath($Destination)

    if (Test-Path -LiteralPath $destinationFull) {
        $hasExisting = Get-ChildItem -LiteralPath $destinationFull -Force -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $hasExisting -and -not $Force) {
            throw "Destination already contains files: $destinationFull. Re-run with -Force to replace the installed SDK copy."
        }
        if ($Force) {
            Remove-Item -LiteralPath $destinationFull -Recurse -Force
        }
    }
    New-Item -ItemType Directory -Path $destinationFull -Force | Out-Null

    Copy-Item -Path (Join-Path $sourceRoot '*') -Destination $destinationFull -Recurse -Force

    $include = Join-Path $destinationFull 'include'
    $windowsLib = Join-Path $destinationFull 'lib\release\discord_partner_sdk.lib'
    $windowsDll = Join-Path $destinationFull 'bin\release\discord_partner_sdk.dll'
    $linuxSo = Join-Path $destinationFull 'lib\release\libdiscord_partner_sdk.so'
    $macDylib = Join-Path $destinationFull 'lib\release\libdiscord_partner_sdk.dylib'

    if (-not (Test-Path -LiteralPath (Join-Path $include 'discordpp.h'))) {
        throw 'Installed SDK validation failed: include\discordpp.h is missing.'
    }

    if (-not $AndroidAar) {
        $bundledAar = Join-Path $destinationFull 'lib\release\discord_partner_sdk.aar'
        if (Test-Path -LiteralPath $bundledAar -PathType Leaf) {
            $AndroidAar = $bundledAar
        }
    }
    if ($AndroidAar) {
        $aarSource = (Resolve-Path -LiteralPath $AndroidAar).Path
        if ([IO.Path]::GetExtension($aarSource) -ne '.aar') {
            throw "Android SDK must be the Discord Social SDK .aar: $aarSource"
        }
        $androidDir = Join-Path $destinationFull 'android'
        New-Item -ItemType Directory -Path $androidDir -Force | Out-Null
        Copy-Item -LiteralPath $aarSource -Destination (Join-Path $androidDir 'discord_partner_sdk.aar') -Force
    }

    Write-Host ''
    Write-Host 'Discord Social SDK installed for PulseForge:' -ForegroundColor Green
    Write-Host "  $destinationFull"
    Write-Host ''
    Write-Host 'Detected desktop runtimes:'
    Write-Host ("  Windows import library: {0}" -f (Test-Path $windowsLib))
    Write-Host ("  Windows runtime DLL:   {0}" -f (Test-Path $windowsDll))
    Write-Host ("  Linux shared library:  {0}" -f (Test-Path $linuxSo))
    Write-Host ("  macOS dylib:            {0}" -f (Test-Path $macDylib))
    Write-Host ("  Android AAR:            {0}" -f (Test-Path (Join-Path $destinationFull 'android\discord_partner_sdk.aar')))
    Write-Host ''

    Get-ChildItem -LiteralPath $destinationFull -Recurse -File |
        Where-Object { $_.Name -match '^(discord_partner_sdk|libdiscord_partner_sdk)\.' -or $_.Name -in @('discordpp.h','cdiscord.h') } |
        ForEach-Object {
            $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            Write-Host ("  {0}  {1}" -f $hash, $_.FullName.Substring($destinationFull.Length + 1))
        }

    Write-Host ''
    Write-Host 'Next: set a real Discord Application ID either in assets/settings.json or at configure time:'
    Write-Host '  cmake -S . -B build -DPULSEFORGE_DISCORD_APPLICATION_ID=123456789012345678'
    Write-Host 'PulseForge will auto-detect this SDK root on the next CMake configure.'

    if ($OpenDeveloperPortal) {
        Start-Process 'https://discord.com/developers/applications'
    }
}
finally {
    if ($TemporarySdkRoot -and (Test-Path -LiteralPath $TemporarySdkRoot)) {
        Remove-Item -LiteralPath $TemporarySdkRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
