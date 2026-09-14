[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$url = $env:PULSEFORGE_DISCORD_SDK_ARCHIVE_URL
$require = $env:PULSEFORGE_DISCORD_SDK_REQUIRE
if ([string]::IsNullOrWhiteSpace($require)) {
    $require = 'windows'
}
if ($require -notin @('windows', 'linux', 'macos', 'android', 'all')) {
    throw 'PULSEFORGE_DISCORD_SDK_REQUIRE must be windows, linux, macos, android or all.'
}
if ([string]::IsNullOrWhiteSpace($url)) {
    throw 'PULSEFORGE_DISCORD_SDK_ARCHIVE_URL is required for Discord-enabled release builds.'
}

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$workRoot = if ($env:RUNNER_TEMP) {
    Join-Path $env:RUNNER_TEMP 'pulseforge-discord-sdk'
} else {
    Join-Path ([IO.Path]::GetTempPath()) 'pulseforge-discord-sdk'
}
if (Test-Path -LiteralPath $workRoot) {
    Remove-Item -LiteralPath $workRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $workRoot | Out-Null
$archive = Join-Path $workRoot 'discord-social-sdk.zip'

# Never print the private URL.
Invoke-WebRequest -Uri $url -OutFile $archive -MaximumRedirection 10
if (-not (Test-Path -LiteralPath $archive -PathType Leaf) -or (Get-Item $archive).Length -eq 0) {
    throw 'Downloaded Discord SDK archive is empty.'
}

& python (Join-Path $projectRoot 'scripts\inspect-discord-social-sdk.py') `
    --sdk $archive --require $require --deep
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& (Join-Path $projectRoot 'scripts\setup-discord-social-sdk.ps1') `
    -SdkPath $archive `
    -Destination (Join-Path $projectRoot 'third_party\discord_social_sdk') `
    -Force
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$sdk = Join-Path $projectRoot 'third_party\discord_social_sdk'
$required = @()
if ($require -ne 'android') {
    $required += 'include\discordpp.h'
    $required += 'include\cdiscord.h'
}
switch ($require) {
    'windows' {
        $required += 'lib\release\discord_partner_sdk.lib'
        $required += 'bin\release\discord_partner_sdk.dll'
    }
    'linux' {
        $required += 'lib\release\libdiscord_partner_sdk.so'
    }
    'android' {
        $required += 'android\discord_partner_sdk.aar'
    }
    'all' {
        $required += 'lib\release\discord_partner_sdk.lib'
        $required += 'bin\release\discord_partner_sdk.dll'
        $required += 'lib\release\libdiscord_partner_sdk.so'
        $required += 'android\discord_partner_sdk.aar'
    }
}
foreach ($relative in $required) {
    $candidate = Join-Path $sdk $relative
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        throw "Discord Social SDK release input is missing $relative"
    }
}
if ($require -in @('macos', 'all')) {
    $framework = Get-ChildItem -LiteralPath $sdk -Recurse -Directory -Filter 'discord_partner_sdk.framework' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $framework) {
        throw 'Discord Social SDK release input is missing the macOS discord_partner_sdk.framework.'
    }
}

Write-Host "Discord Social SDK release input staged and validated for target: $require."
