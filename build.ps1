# build.ps1 - Build bddview (Windows/SDL2) using VS 2022
# powershell -ExecutionPolicy Bypass -File build.ps1

param(
    [string]$SourceDir = "\\wsl.localhost\Ubuntu\home\alex\midway-bddview",
    [string]$BuildRoot = "$env:LOCALAPPDATA\bddview-build",
    [string]$Sdl2Ver  = ""
)

$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# 1. VS 2022 - check Community then BuildTools
$vsRoots = @(
    "C:\Program Files\Microsoft Visual Studio\2022\Community",
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools",
    "C:\Program Files\Microsoft Visual Studio\2022\Professional",
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
)
$vsRoot = $vsRoots | Where-Object { Test-Path "$_\VC\Auxiliary\Build\vcvarsall.bat" } | Select-Object -First 1
if (-not $vsRoot) { Write-Error "VS 2022 not found"; exit 1 }
$vcvarsall = "$vsRoot\VC\Auxiliary\Build\vcvarsall.bat"
$cmakeRel  = "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$vsCmake   = ($vsRoots | ForEach-Object { "$_\$cmakeRel" } | Where-Object { Test-Path $_ } | Select-Object -First 1)
if ($vsCmake) { $vsCmake = Split-Path $vsCmake } else { $vsCmake = "" }
Write-Host "[1/4] Found VS 2022 at $vsRoot" -ForegroundColor Cyan
if ($vsCmake) { Write-Host "      cmake: $vsCmake" -ForegroundColor Cyan } else { Write-Host "      cmake not found in VS installs, relying on PATH" -ForegroundColor Yellow }

# 2. SDL2 version
if (-not $Sdl2Ver) {
    try {
        $r = (Invoke-RestMethod "https://api.github.com/repos/libsdl-org/SDL/releases?per_page=20") |
             Where-Object { $_.tag_name -match '^release-2\.' } | Select-Object -First 1
        $Sdl2Ver = $r.tag_name -replace '^release-', ''
        Write-Host "[2/4] Latest SDL2: $Sdl2Ver" -ForegroundColor Cyan
    } catch { $Sdl2Ver = "2.30.2"; Write-Host "[2/4] SDL2 fallback $Sdl2Ver" -ForegroundColor Cyan }
} else { Write-Host "[2/4] SDL2 $Sdl2Ver" -ForegroundColor Cyan }

$depsDir   = "$BuildRoot\deps"
$sdl2Root  = "$depsDir\SDL2-$Sdl2Ver"
$sdl2Cmake = "$sdl2Root\cmake"
$buildDir  = "$BuildRoot\build"

New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null
New-Item -ItemType Directory -Force -Path $depsDir   | Out-Null
New-Item -ItemType Directory -Force -Path $buildDir  | Out-Null

# 3. SDL2
if (-not (Test-Path $sdl2Root)) {
    $url = "https://github.com/libsdl-org/SDL/releases/download/release-$Sdl2Ver/SDL2-devel-$Sdl2Ver-VC.zip"
    $zip = "$depsDir\sdl2.zip"
    Write-Host "[3/4] Downloading SDL2 $Sdl2Ver ..." -ForegroundColor Cyan
    (New-Object Net.WebClient).DownloadFile($url, $zip)
    Expand-Archive -Path $zip -DestinationPath $depsDir -Force
    Remove-Item $zip
} else { Write-Host "[3/4] SDL2 $Sdl2Ver already present" -ForegroundColor Cyan }

if (-not (Test-Path $sdl2Cmake)) { Write-Error "SDL2 cmake dir not found at $sdl2Cmake"; exit 1 }

# 4. Build - write batch file without here-strings to avoid encoding issues
Write-Host "[4/4] Configuring and building (x64 Release)..." -ForegroundColor Cyan

$lines = @(
    "@echo off",
    "call `"$vcvarsall`" x64",
    "if errorlevel 1 exit /b 1",
    "set PATH=$vsCmake;%PATH%",
    "cmake -B `"$buildDir`" -G `"Visual Studio 17 2022`" -A x64 -DSDL2_DIR=`"$sdl2Cmake`" `"$SourceDir`"",
    "if errorlevel 1 exit /b 1",
    "cmake --build `"$buildDir`" --config Release",
    "if errorlevel 1 exit /b 1"
)
$batFile = "$env:TEMP\build_bddview.bat"
[System.IO.File]::WriteAllLines($batFile, $lines, [System.Text.Encoding]::ASCII)
& cmd.exe /c $batFile
$exitCode = $LASTEXITCODE

if ($exitCode -eq 0) {
    $exe    = "$buildDir\Release\bddview.exe"
    $sdlDll = "$sdl2Root\lib\x64\SDL2.dll"
    if (Test-Path $sdlDll) { Copy-Item $sdlDll (Split-Path $exe) -Force }
    Write-Host ""
    Write-Host "*** Build succeeded ***" -ForegroundColor Green
    Write-Host "EXE: $exe" -ForegroundColor Green
} else {
    Write-Host ""
    Write-Host "*** Build FAILED (exit $exitCode) ***" -ForegroundColor Red
    exit $exitCode
}
