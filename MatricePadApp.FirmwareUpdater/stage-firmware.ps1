<#
.SYNOPSIS
    Stages the Firmware/ folder that MatricePadApp.FirmwareUpdater bundles at
    runtime: a freshly-compiled matrice_pad_tempo .hex plus a copy of
    arduino-cli's own avrdude.exe/avrdude.conf. Not committed to git (see
    .gitignore) -- run this before building/testing/publishing the Updater.

.EXAMPLE
    ./MatricePadApp.FirmwareUpdater/stage-firmware.ps1
#>
$ErrorActionPreference = "Stop"

$repoRoot    = Split-Path -Parent $PSScriptRoot
$sketchPath  = Join-Path $repoRoot "arduino\matrice_pad_tempo"
$inoPath     = Join-Path $sketchPath "matrice_pad_tempo.ino"
$firmwareDir = Join-Path $PSScriptRoot "Firmware"
$buildDir    = Join-Path $firmwareDir "build"

# Pull FIRMWARE_VERSION out of the sketch so the staged .hex's filename (and
# reminder below) always matches what's actually being compiled -- see
# MainWindow.xaml.cs's ExpectedFirmwareVersion/BundledHexFileName constants,
# which must be kept in sync with this by hand until spec section 3's
# "read from a bundled manifest" TODO is done.
$inoContent = Get-Content $inoPath -Raw
if ($inoContent -notmatch '#define\s+FIRMWARE_VERSION\s+"([^"]+)"') {
    Write-Error "Could not find FIRMWARE_VERSION in $inoPath"
    exit 1
}
$firmwareVersion = $Matches[1]
Write-Host "Sketch reports FIRMWARE_VERSION $firmwareVersion" -ForegroundColor Cyan

Write-Host "`nCompiling $sketchPath ..." -ForegroundColor Yellow
if (Test-Path $buildDir) { Remove-Item -Recurse -Force $buildDir }
arduino-cli compile --fqbn arduino:avr:leonardo --libraries (Join-Path $repoRoot "arduino\libraries") `
    --output-dir $buildDir $sketchPath
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$hexDestName = "matrice_pad_tempo-$firmwareVersion.hex"
$hexDest = Join-Path $firmwareDir $hexDestName
Copy-Item (Join-Path $buildDir "matrice_pad_tempo.ino.hex") $hexDest -Force
Remove-Item -Recurse -Force $buildDir
Write-Host "Staged $hexDestName" -ForegroundColor Green

# Locate arduino-cli's own bundled avrdude rather than requiring a separate
# install -- same tool arduino-cli/arduino/build.ps1 already shell out to.
$avrdudeRoot = Join-Path $env:LOCALAPPDATA "Arduino15\packages\arduino\tools\avrdude"
$avrdudeVersionDir = Get-ChildItem $avrdudeRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
if (-not $avrdudeVersionDir) {
    Write-Error "Could not find avrdude under $avrdudeRoot -- is arduino-cli/Arduino IDE installed?"
    exit 1
}

$avrdudeDestDir = Join-Path $firmwareDir "avrdude"
New-Item -ItemType Directory -Force -Path $avrdudeDestDir | Out-Null
Copy-Item (Join-Path $avrdudeVersionDir.FullName "bin\avrdude.exe") $avrdudeDestDir -Force
Copy-Item (Join-Path $avrdudeVersionDir.FullName "etc\avrdude.conf") $avrdudeDestDir -Force
Write-Host "Staged avrdude $($avrdudeVersionDir.Name)" -ForegroundColor Green

Write-Host "`nReminder: if $firmwareVersion doesn't match ExpectedFirmwareVersion/BundledHexFileName" -ForegroundColor Yellow
Write-Host "in MainWindow.xaml.cs, update those constants to match." -ForegroundColor Yellow
