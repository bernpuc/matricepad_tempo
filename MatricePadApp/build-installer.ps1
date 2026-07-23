# PowerShell script to build the Matrice Pad Tempo Companion installer.
# This script reads the version from the .csproj, publishes MatricePadApp
# self-contained (so the installer has no .NET runtime prerequisite to
# check), and builds the NSIS installer. Modeled on
# InvoicingApp/build-installer.ps1.

param(
    [string]$Configuration = "Release"
)

Write-Host "Building Matrice Pad Tempo Companion Installer..." -ForegroundColor Cyan

$projectDir = $PSScriptRoot
$repoRoot = Split-Path -Parent $projectDir
$csprojPath = Join-Path $projectDir "MatricePadApp.csproj"
$nsiPath = Join-Path $projectDir "Package\Installer.nsi"
$publishDir = Join-Path $projectDir "publish\win-x64"

$updaterProjectDir = Join-Path $repoRoot "MatricePadApp.FirmwareUpdater"
$updaterCsprojPath = Join-Path $updaterProjectDir "MatricePadApp.FirmwareUpdater.csproj"
$updaterPublishDir = Join-Path $updaterProjectDir "publish\win-x64"
$stageFirmwareScript = Join-Path $updaterProjectDir "stage-firmware.ps1"

# Read version from .csproj
Write-Host "Reading version from $csprojPath..." -ForegroundColor Yellow
[xml]$csproj = Get-Content $csprojPath
$version = $csproj.Project.PropertyGroup.Version | Select-Object -First 1

if (-not $version) {
    Write-Host "ERROR: Could not find <Version> in .csproj file!" -ForegroundColor Red
    exit 1
}

Write-Host "Version detected: $version" -ForegroundColor Green

# Publish self-contained so the installer needs no .NET runtime prerequisite.
if (Test-Path $publishDir) {
    Remove-Item -Recurse -Force $publishDir
}

Write-Host "`nPublishing self-contained win-x64 in $Configuration mode..." -ForegroundColor Yellow
dotnet publish $csprojPath -c $Configuration -r win-x64 --self-contained true -o $publishDir

if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Publish failed!" -ForegroundColor Red
    exit 1
}

Write-Host "Publish successful!" -ForegroundColor Green

# Stage the Firmware Updater's bundled .hex/avrdude, then publish it
# self-contained too -- same zero-prerequisite policy as MatricePadApp above.
Write-Host "`nStaging firmware/avrdude for the Firmware Updater..." -ForegroundColor Yellow
& $stageFirmwareScript
if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Firmware staging failed!" -ForegroundColor Red
    exit 1
}

if (Test-Path $updaterPublishDir) {
    Remove-Item -Recurse -Force $updaterPublishDir
}

Write-Host "`nPublishing Firmware Updater self-contained win-x64 in $Configuration mode..." -ForegroundColor Yellow
dotnet publish $updaterCsprojPath -c $Configuration -r win-x64 --self-contained true -o $updaterPublishDir

if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Firmware Updater publish failed!" -ForegroundColor Red
    exit 1
}

Write-Host "Firmware Updater publish successful!" -ForegroundColor Green

# Find NSIS
$nsisPath = $null
$possiblePaths = @(
    "C:\Program Files (x86)\NSIS\makensis.exe",
    "C:\Program Files\NSIS\makensis.exe"
)

foreach ($path in $possiblePaths) {
    if (Test-Path $path) {
        $nsisPath = $path
        break
    }
}

if (-not $nsisPath) {
    Write-Host "ERROR: NSIS not found! Please install NSIS from https://nsis.sourceforge.io/" -ForegroundColor Red
    exit 1
}

Write-Host "`nFound NSIS at: $nsisPath" -ForegroundColor Green

# Build the installer
Write-Host "`nBuilding installer with version $version..." -ForegroundColor Yellow
& $nsisPath "-DVERSION=$version" $nsiPath

if ($LASTEXITCODE -ne 0) {
    Write-Host "ERROR: Installer build failed!" -ForegroundColor Red
    exit 1
}

$packageDir = Join-Path $projectDir "Package"
$builtInstaller = Get-ChildItem $packageDir -Filter "*$version Installer.exe" | Select-Object -First 1

Write-Host "`nInstaller built successfully!" -ForegroundColor Green
Write-Host "Installer location: $($builtInstaller.FullName)" -ForegroundColor Cyan
