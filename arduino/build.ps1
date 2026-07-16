<#
.SYNOPSIS
    Compile (and optionally upload) a MatricePad Tempo sketch with arduino-cli,
    resolving the shared TempoCore library from this repo instead of the
    sketchbook libraries folder.

.PARAMETER Sketch
    Path to the sketch folder, e.g. arduino/matrice_pad_tempo

.PARAMETER Port
    COM port to upload to (e.g. COM7). If omitted, only compiles.

.PARAMETER Fqbn
    Board FQBN. Defaults to arduino:avr:leonardo (same ATmega32U4 chip as the
    SparkFun Pro Micro 5V/16MHz target -- see CLAUDE.md).

.PARAMETER DiscoveryTimeout
    How long arduino-cli waits for the bootloader port to (re)appear after
    triggering reset, e.g. "20s". arduino-cli's own default is only 1s, which
    is too tight to hit by manually double-tapping the reset pin -- see the
    upload tip in CLAUDE.md. Bumped to 20s by default here for slack.

.EXAMPLE
    ./arduino/build.ps1 -Sketch arduino/matrice_pad_tempo
    ./arduino/build.ps1 -Sketch arduino/matrice_pad_tempo -Port COM7
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$Sketch,

    [string]$Port,

    [string]$Fqbn = "arduino:avr:leonardo",

    [string]$DiscoveryTimeout = "20s"
)

$ErrorActionPreference = "Stop"

$repoRoot     = Split-Path -Parent $PSScriptRoot
$librariesDir = Join-Path $PSScriptRoot "libraries"
$sketchPath   = Join-Path $repoRoot $Sketch

if (-not (Test-Path $sketchPath)) {
    Write-Error "Sketch folder not found: $sketchPath"
    exit 1
}

if ($Port) {
    # arduino-cli upload doesn't recompile (and doesn't accept --libraries), so
    # compile+upload has to happen in one `compile --upload` call to resolve
    # TempoCore from this repo instead of the sketchbook libraries folder.
    Write-Host "Compiling and uploading $Sketch ($Fqbn) to $Port (discovery timeout $DiscoveryTimeout) ..."
    arduino-cli compile --fqbn $Fqbn --libraries $librariesDir --upload --port $Port --discovery-timeout $DiscoveryTimeout $sketchPath
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} else {
    Write-Host "Compiling $Sketch ($Fqbn) with libraries from $librariesDir ..."
    arduino-cli compile --fqbn $Fqbn --libraries $librariesDir $sketchPath
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
