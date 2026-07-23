<#
.SYNOPSIS
    Compile (and optionally upload) the MatricePad Tempo sketch with arduino-cli.

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

$repoRoot   = Split-Path -Parent $PSScriptRoot
$sketchPath = Join-Path $repoRoot $Sketch

if (-not (Test-Path $sketchPath)) {
    Write-Error "Sketch folder not found: $sketchPath"
    exit 1
}

if ($Port) {
    Write-Host "Compiling and uploading $Sketch ($Fqbn) to $Port (discovery timeout $DiscoveryTimeout) ..."
    arduino-cli compile --fqbn $Fqbn --upload --port $Port --discovery-timeout $DiscoveryTimeout $sketchPath
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} else {
    Write-Host "Compiling $Sketch ($Fqbn) ..."
    arduino-cli compile --fqbn $Fqbn $sketchPath
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
