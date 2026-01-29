# Build a self-contained Windows release zip:
#   dist/blackhole-sim-<version>-win64.zip
#     blackhole_sim.exe   (C++ runtime linked statically: no redistributable needed)
#     shaders/
#     README.md, LICENSE, THIRD_PARTY_LICENSES.md
#
# Usage (from a "x64 Native Tools Command Prompt for VS 2022", with CMake on PATH):
#   powershell -ExecutionPolicy Bypass -File scripts/package_release.ps1 [-Version 1.0.0]
param([string]$Version = "1.0.0")
$ErrorActionPreference = "Stop"

$root  = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build-release"
$stage = Join-Path $root "dist\blackhole-sim-$Version-win64"
$zip   = "$stage.zip"

# Prefer Ninja (uses whatever compiler the prompt set up). Otherwise use the
# Visual Studio generator, forced to x64: the bundled GLFW libraries are x64,
# and on ARM64 Windows the generator would default to ARM64.
if (Get-Command ninja -ErrorAction SilentlyContinue) { $gen = @("-G", "Ninja") } else { $gen = @("-A", "x64") }
cmake -S $root -B $build @gen -DCMAKE_BUILD_TYPE=Release -DBH_STATIC_RUNTIME=ON
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
cmake --build $build --config Release
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

# Visual Studio generators put the exe in build/Release; Ninja/Makefiles in build/
$exe = Get-ChildItem $build -Recurse -Filter blackhole_sim.exe | Select-Object -First 1
if (-not $exe) { throw "blackhole_sim.exe not found under $build" }

Remove-Item $stage, $zip -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $exe.FullName $stage
Copy-Item (Join-Path $root "src\shaders") (Join-Path $stage "shaders") -Recurse
Copy-Item (Join-Path $root "README.md"), (Join-Path $root "LICENSE") $stage
Copy-Item (Join-Path $root "third_party\LICENSES.md") (Join-Path $stage "THIRD_PARTY_LICENSES.md")

Compress-Archive -Path "$stage\*" -DestinationPath $zip
Write-Host "Created $zip"
