param(
    [string]$Configuration = "Release",
    [string]$BuildDirectory = "artifacts/native-build",
    [string]$Preset = ""
)

$ErrorActionPreference = "Stop"

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

if ($Preset) {
    if ($Preset -like "*msquic*" -and [string]::IsNullOrWhiteSpace($env:VCPKG_ROOT)) {
        throw "Preset '$Preset' requires VCPKG_ROOT. Install vcpkg, set VCPKG_ROOT, then rerun this command."
    }

    Push-Location $repositoryRoot
    try {
        cmake --preset $Preset
        cmake --build --preset $Preset
    }
    finally {
        Pop-Location
    }

    exit
}

$nativeSourceDirectory = Join-Path $repositoryRoot "native/webtransport_native"
$buildDirectoryPath = Join-Path $repositoryRoot $BuildDirectory

cmake -S $nativeSourceDirectory -B $buildDirectoryPath
cmake --build $buildDirectoryPath --config $Configuration
