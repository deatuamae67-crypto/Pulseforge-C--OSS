param(
    [string]$EngineRoot = (Get-Location).Path,
    [string]$Repository = 'deatuamae67-crypto/Pulseforge-C--OSS',
    [string]$Tag = 'v1.0.0-complete'
)

$ErrorActionPreference = 'Stop'
$target = if (Test-Path -LiteralPath (Join-Path $EngineRoot 'bin') -PathType Container) {
    Join-Path $EngineRoot 'bin'
} else {
    $EngineRoot
}
New-Item -ItemType Directory -Force -Path (Join-Path $target 'mods'), (Join-Path $target 'assets/menu') | Out-Null

$temp = Join-Path ([IO.Path]::GetTempPath()) ('pulseforge-complete-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
try {
    $headers = @{ 'Accept' = 'application/vnd.github+json'; 'User-Agent' = 'PulseForge-Complete-Installer' }
    $release = Invoke-RestMethod -Headers $headers -Uri "https://api.github.com/repos/$Repository/releases/tags/$Tag"
    if ($release.draft -or $release.tag_name -ne 'v1.0.0-complete') {
        throw 'Complete release is not public or has an unexpected tag.'
    }
    $assets = @{}
    foreach ($asset in $release.assets) { $assets[$asset.name] = $asset.browser_download_url }

    function Download-Asset([string]$Name) {
        if (-not $assets.ContainsKey($Name)) { throw "Release asset not found: $Name" }
        $path = Join-Path $temp $Name
        Invoke-WebRequest -Headers $headers -Uri $assets[$Name] -OutFile $path
        return $path
    }

    function Test-ArchiveHash([string]$ChecksumPath, [string]$ArchivePath) {
        $expected = ((Get-Content -LiteralPath $ChecksumPath | Where-Object { $_.Trim() } | Select-Object -First 1) -split '\s+')[0].ToLowerInvariant()
        $actual = (Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $expected) { throw "SHA-256 mismatch: $([IO.Path]::GetFileName($ArchivePath))" }
    }

    $menuName = 'PulseForge-v1.0.0-complete-menu-music.zip'
    $menu = Download-Asset $menuName
    $menuChecksum = Download-Asset "$menuName.SHA256SUMS.txt"
    Test-ArchiveHash $menuChecksum $menu
    Expand-Archive -LiteralPath $menu -DestinationPath $target -Force

    $modsListName = 'PulseForge-v1.0.0-complete-modsList.txt'
    $modsList = Download-Asset $modsListName
    Copy-Item -LiteralPath $modsList -Destination (Join-Path $target 'mods/modsList.txt') -Force

    $manifests = @($assets.Keys | Where-Object { $_ -match '^PulseForge-v1\.0\.0-complete-mod-.*\.manifest\.json$' } | Sort-Object)
    if ($manifests.Count -eq 0) { throw 'No Complete mod manifests were found.' }

    foreach ($manifest in $manifests) {
        $base = $manifest.Substring(0, $manifest.Length - '.manifest.json'.Length)
        $zipName = "$base.zip"
        $checksumName = "$zipName.SHA256SUMS.txt"
        Write-Host "Installing $($base -replace '^PulseForge-v1\.0\.0-complete-mod-','')..."
        $checksum = Download-Asset $checksumName
        $zipPath = Join-Path $temp $zipName

        if ($assets.ContainsKey($zipName)) {
            [void](Download-Asset $zipName)
        } else {
            $parts = @($assets.Keys | Where-Object { $_.StartsWith("$zipName.part", [StringComparison]::Ordinal) } | Sort-Object)
            if ($parts.Count -eq 0) { throw "Missing archive or split parts for $base" }
            $out = [IO.File]::Open($zipPath, [IO.FileMode]::Create, [IO.FileAccess]::Write)
            try {
                foreach ($part in $parts) {
                    $partPath = Download-Asset $part
                    $input = [IO.File]::OpenRead($partPath)
                    try { $input.CopyTo($out) } finally { $input.Dispose() }
                    Remove-Item -LiteralPath $partPath -Force
                }
            } finally { $out.Dispose() }
        }

        Test-ArchiveHash $checksum $zipPath
        Expand-Archive -LiteralPath $zipPath -DestinationPath $target -Force
        Remove-Item -LiteralPath $zipPath, $checksum -Force -ErrorAction SilentlyContinue
    }

    Write-Host "PulseForge Complete content installed into: $target"
}
finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
