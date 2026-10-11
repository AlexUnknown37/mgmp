# Build mgmp and copy it into a Mewtator mod folder.
#
#   powershell -ExecutionPolicy Bypass -File tools\deploy.ps1
#   powershell -ExecutionPolicy Bypass -File tools\deploy.ps1 -ModsDir "D:\_Games\Mods\Mewtator\mods"
#
# mgmp.json is only copied when the mod folder has none yet, so a role/address
# set by hand (host / client, LAN IP) survives every redeploy.
param(
    [string]$ModsDir = $env:MGMP_MEWTATOR_MODS,
    [string]$ModName = "MewgenicsOnline",
    [switch]$NoBuild
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

if (-not $ModsDir) { throw "Pass -ModsDir <Mewtator mods folder> or set MGMP_MEWTATOR_MODS." }

if (-not $NoBuild) {
    $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    if (-not $cmake) {
        $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        $cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    }
    if (-not (Test-Path (Join-Path $root "build"))) { & $cmake -S $root -B (Join-Path $root "build") -A x64 }
    & $cmake --build (Join-Path $root "build") --config Release --target mgmp
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

$dest = Join-Path $ModsDir $ModName
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item (Join-Path $root "build\Release\mgmp.dll") $dest -Force
Copy-Item (Join-Path $root "packaging\mewtator\info.json") $dest -Force
$cfg = Join-Path $dest "mgmp.json"
if (-not (Test-Path $cfg)) { Copy-Item (Join-Path $root "mgmp.json.template") $cfg }
Write-Host "deployed to $dest"
