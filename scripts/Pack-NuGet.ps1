param(
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string]$Configuration = "Release",
    [string]$OutputDirectory = "",
    [switch]$RequireNativeAssets,
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $RepositoryRoot "artifacts/nuget"
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

$packArguments = @(
    "pack",
    (Join-Path $RepositoryRoot "src/WebTransport.Native/WebTransport.Native.csproj"),
    "-c",
    $Configuration,
    "-o",
    $OutputDirectory
)

if ($NoBuild) {
    $packArguments += "--no-build"
}

dotnet @packArguments

$packArguments = @(
    "pack",
    (Join-Path $RepositoryRoot "src/WebTransport.Client/WebTransport.Client.csproj"),
    "-c",
    $Configuration,
    "-o",
    $OutputDirectory
)

if ($NoBuild) {
    $packArguments += "--no-build"
}

dotnet @packArguments

function Assert-PackageEntry {
    param(
        [string]$PackagePath,
        [string]$EntryPattern,
        [string]$Description
    )

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($PackagePath)
    try {
        $entry = $archive.Entries | Where-Object { $_.FullName -like $EntryPattern } | Select-Object -First 1
        if (-not $entry) {
            throw "Package '$PackagePath' is missing $Description ($EntryPattern)."
        }
    }
    finally {
        $archive.Dispose()
    }
}

$nativePackage = Get-ChildItem -Path $OutputDirectory -Filter "Demiurgos.WebTransport.Native.*.nupkg" |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

$clientPackage = Get-ChildItem -Path $OutputDirectory -Filter "Demiurgos.WebTransport.Client.*.nupkg" |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

if (-not $nativePackage) {
    throw "Demiurgos.WebTransport.Native package was not produced."
}

if (-not $clientPackage) {
    throw "Demiurgos.WebTransport.Client package was not produced."
}

Assert-PackageEntry -PackagePath $nativePackage.FullName -EntryPattern "lib/netstandard2.1/WebTransport.Native.dll" -Description "managed native wrapper"
Assert-PackageEntry -PackagePath $clientPackage.FullName -EntryPattern "lib/netstandard2.1/WebTransport.Client.dll" -Description "managed client assembly"

if ($RequireNativeAssets) {
    Assert-PackageEntry -PackagePath $nativePackage.FullName -EntryPattern "runtimes/*/native/*webtransport_native*" -Description "native runtime assets"
    Assert-PackageEntry -PackagePath $nativePackage.FullName -EntryPattern "runtimes/win-x64/native/webtransport_native.dll" -Description "win-x64 native WebTransport library"
    Assert-PackageEntry -PackagePath $nativePackage.FullName -EntryPattern "runtimes/win-x64/native/msquic.dll" -Description "win-x64 MsQuic companion library"
}

Write-Host "Produced NuGet packages:"
Write-Host "  $($nativePackage.FullName)"
Write-Host "  $($clientPackage.FullName)"
