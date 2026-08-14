# Build the aim1k firmware.
# Usage:  .\build.ps1          (configure if needed + build)
#         .\build.ps1 -Clean   (wipe build/ first)
#
# Finds a toolchain in either of the two places one ends up: the standalone
# C:\pico layout, or the Raspberry Pi Pico VS Code extension's ~/.pico-sdk
# (which is what installing that extension gives you, and which brings its own
# SDK, arm-none-eabi, CMake, Ninja and picotool).
param([switch]$Clean)

$ErrorActionPreference = "Stop"

# cmake writes ordinary progress to stderr, and under $ErrorActionPreference =
# "Stop" PowerShell turns any redirected native stderr into a terminating error
# — so the build "fails" while printing a success message. Run native tools with
# that off and judge them by their exit code, which is the only thing that
# actually says whether they worked.
function Invoke-Native($exe, $argList) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $exe @argList } finally { $ErrorActionPreference = $prev }
    if ($LASTEXITCODE -ne 0) { throw "$exe failed with exit code $LASTEXITCODE" }
}

function Find-Newest($path) {
    if (-not (Test-Path $path)) { return $null }
    $d = Get-ChildItem $path -Directory -ErrorAction SilentlyContinue |
         Sort-Object Name -Descending | Select-Object -First 1
    if ($d) { return $d.FullName }
    return $null
}

$Pico = "C:\pico"
if (Test-Path "$Pico\pico-sdk") {
    $sdk       = "$Pico\pico-sdk"
    $toolchain = "$Pico\armgcc"
    $ninjaDir  = "$Pico\ninja"
    $cmakeDir  = "$Pico\cmake\bin"
    $picotool  = "$Pico\picotool-dist\picotool"
} else {
    $root = Join-Path $env:USERPROFILE ".pico-sdk"
    $sdk       = Find-Newest "$root\sdk"
    $toolchain = Find-Newest "$root\toolchain"
    $ninjaDir  = Find-Newest "$root\ninja"
    $cmakeBase = Find-Newest "$root\cmake"
    $cmakeDir  = if ($cmakeBase) { Join-Path $cmakeBase "bin" } else { $null }
    $ptBase    = Find-Newest "$root\picotool"
    $picotool  = if ($ptBase) { Join-Path $ptBase "picotool" } else { $null }
    # The SDK builds boot_stage2 with a Python script, so configure hard-fails
    # without an interpreter. Windows' python.exe is usually the Microsoft Store
    # stub, which find_package(Python3) will not accept, so point at the one the
    # extension ships rather than hoping PATH has a real one.
    $pyBase    = Find-Newest "$root\python"
    $python    = if ($pyBase) { Join-Path $pyBase "python.exe" } else { $null }
    if (-not $sdk) {
        throw "No Pico SDK found. Expected C:\pico\pico-sdk or $root\sdk\<version>."
    }
}

$env:PICO_SDK_PATH       = $sdk
$env:PICO_TOOLCHAIN_PATH = $toolchain
$env:PATH = "$toolchain\bin;$cmakeDir;$ninjaDir;$env:PATH"

$src = $PSScriptRoot
$build = Join-Path $src "build"
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }

if (-not (Test-Path (Join-Path $build "build.ninja"))) {
    $cfg = @(
        "-B", $build, "-S", $src, "-G", "Ninja",
        "-DCMAKE_MAKE_PROGRAM=$ninjaDir\ninja.exe",
        "-DPICO_SDK_PATH=$sdk",
        "-DPICO_TOOLCHAIN_PATH=$toolchain",
        "-DPICO_PLATFORM=rp2350", "-DPICO_BOARD=pico2",
        "-DCMAKE_BUILD_TYPE=Release"
    )
    if ($picotool -and (Test-Path $picotool)) { $cfg += "-Dpicotool_DIR=$picotool" }
    if ($python -and (Test-Path $python))     { $cfg += "-DPython3_EXECUTABLE=$python" }
    Invoke-Native cmake $cfg
}

Invoke-Native cmake @("--build", $build)
Write-Host ""
Write-Host "Built: $build\aim1k.uf2" -ForegroundColor Green
Write-Host "Flash: hold BOOTSEL, plug in, copy aim1k.uf2 to the RP2350 drive."
