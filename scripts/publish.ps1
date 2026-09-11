param([ValidateSet('win-x64', 'win-arm64')][string]$Runtime = 'win-x64')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$output = Join-Path $projectRoot "artifacts/$Runtime"
$architecture = $Runtime -replace '^win-', ''
& (Join-Path $projectRoot 'native/build.ps1') -Architecture $architecture
$exe = Join-Path $projectRoot "artifacts/native-$architecture/WinDuo.exe"
$releaseFile = Join-Path $projectRoot "artifacts/WinDuo-$Runtime.exe"
Copy-Item -LiteralPath $exe -Destination $releaseFile -Force
$hash = (Get-FileHash -LiteralPath $releaseFile -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  WinDuo-$Runtime.exe" | Set-Content -LiteralPath "$releaseFile.sha256" -Encoding ascii
Write-Output $releaseFile
