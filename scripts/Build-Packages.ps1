param(
    [string]$Configuration = "Release",
    [string]$NativePreset = "windows-msquic-release",
    [string]$Rid = "win-x64",
    [switch]$SkipNativeBuild,
    [switch]$SkipTests,
    [switch]$AllowMissingNativeAssets
)

$ErrorActionPreference = "Stop"

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

if (-not $SkipTests) {
    dotnet test (Join-Path $repositoryRoot "WebTransport.sln") -c $Configuration
}

dotnet build (Join-Path $repositoryRoot "src/WebTransport.Native/WebTransport.Native.csproj") -c $Configuration
dotnet build (Join-Path $repositoryRoot "src/WebTransport.Client/WebTransport.Client.csproj") -c $Configuration

if (-not $SkipNativeBuild) {
    if ($NativePreset -like "*msquic*" -and [string]::IsNullOrWhiteSpace($env:VCPKG_ROOT)) {
        throw "Preset '$NativePreset' requires VCPKG_ROOT. Install vcpkg and set VCPKG_ROOT, or rerun with -SkipNativeBuild."
    }

    cmake --preset $NativePreset
    cmake --build --preset $NativePreset
}

$stageArgs = @{
    RepositoryRoot = $repositoryRoot
    Rid = $Rid
    Configuration = $Configuration
    SkipUpm = $true
    Clean = $true
}

if ($AllowMissingNativeAssets) {
    $stageArgs.AllowMissing = $true
}

& (Join-Path $PSScriptRoot "Stage-NativeAssets.ps1") @stageArgs

$packArgs = @{
    RepositoryRoot = $repositoryRoot
    Configuration = $Configuration
    RequireNativeAssets = -not $AllowMissingNativeAssets
}

& (Join-Path $PSScriptRoot "Pack-NuGet.ps1") @packArgs

$upmArgs = @{
    RepositoryRoot = $repositoryRoot
    Configuration = $Configuration
    Rid = $Rid
    NoBuild = $true
    Clean = $true
}

if ($AllowMissingNativeAssets) {
    $upmArgs.AllowMissingNativeAssets = $true
}

& (Join-Path $PSScriptRoot "Assemble-UpmPackage.ps1") @upmArgs

Write-Host "Package artifacts are available under:"
Write-Host "  $(Join-Path $repositoryRoot "artifacts/nuget")"
Write-Host "  $(Join-Path $repositoryRoot "artifacts/upm/io.demiurgos.webtransport")"
