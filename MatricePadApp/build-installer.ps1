# PowerShell script to build the Matrice Pad Sound Panel installer.
# This script reads the version from the .csproj, publishes MatricePadApp
# self-contained (so the installer has no .NET runtime prerequisite to
# check), and builds the NSIS installer. Modeled on
# InvoicingApp/build-installer.ps1.

param(
    [string]$Configuration = "Release"
)

Write-Host "Building Matrice Pad Sound Panel Installer..." -ForegroundColor Cyan

$projectDir = $PSScriptRoot
$csprojPath = Join-Path $projectDir "MatricePadApp.csproj"
$nsiPath = Join-Path $projectDir "Package\Installer.nsi"
$publishDir = Join-Path $projectDir "publish\win-x64"

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

Write-Host "`nInstaller built successfully!" -ForegroundColor Green
Write-Host "Installer location: $projectDir\Package\Matrice Pad Sound Panel $version Installer.exe" -ForegroundColor Cyan
