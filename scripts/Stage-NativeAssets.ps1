param(
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string]$Rid = "win-x64",
    [string]$Configuration = "Release",
    [string]$NativeBuildDirectory = "",
    [string]$NuGetRuntimesDirectory = "",
    [string]$UpmPackageDirectory = "",
    [switch]$SkipNuGet,
    [switch]$SkipUpm,
    [switch]$Clean,
    [switch]$AllowMissing
)

$ErrorActionPreference = "Stop"

function Get-NativeLibraryName {
    param([string]$RuntimeIdentifier)

    if ($RuntimeIdentifier -like "win-*") {
        return "webtransport_native.dll"
    }

    if ($RuntimeIdentifier -eq "ios") {
        return "libwebtransport_native.a"
    }

    if ($RuntimeIdentifier -like "osx-*") {
        return "libwebtransport_native.dylib"
    }

    return "libwebtransport_native.so"
}

function Get-UnityPluginPath {
    param([string]$RuntimeIdentifier)

    switch ($RuntimeIdentifier) {
        "win-x64" { return "Plugins/x86_64" }
        "linux-x64" { return "Plugins/Linux/x86_64" }
        "osx-arm64" { return "Plugins/macOS/arm64" }
        "android-arm64" { return "Plugins/Android/arm64-v8a" }
        "ios" { return "Plugins/iOS" }
        default { return "Plugins/$RuntimeIdentifier" }
    }
}

function Find-NativeLibrary {
    param(
        [string]$FileName,
        [string[]]$BuildRoots
    )

    foreach ($buildRoot in $BuildRoots) {
        if (-not (Test-Path $buildRoot)) {
            continue
        }

        $directPath = Join-Path $buildRoot "native/webtransport_native/$Configuration/$FileName"
        if (Test-Path $directPath) {
            return (Resolve-Path $directPath).Path
        }

        $match = Get-ChildItem -Path $buildRoot -Recurse -File -Filter $FileName -ErrorAction SilentlyContinue |
            Select-Object -First 1

        if ($match) {
            return $match.FullName
        }
    }

    return $null
}

function Get-CompanionLibrarySpecs {
    param([string]$RuntimeIdentifier)

    switch -Wildcard ($RuntimeIdentifier) {
        "win-*" {
            return @(
                [pscustomobject]@{
                    Pattern = "msquic.dll"
                    Required = $true
                }
            )
        }
        "linux-*" {
            return @(
                [pscustomobject]@{
                    Pattern = "libmsquic.so*"
                    Required = $false
                }
            )
        }
        "osx-*" {
            return @(
                [pscustomobject]@{
                    Pattern = "libmsquic*.dylib"
                    Required = $false
                }
            )
        }
        default {
            return @()
        }
    }
}

function Get-CompanionLibraryPriority {
    param([System.IO.FileInfo]$Library)

    $normalizedPath = $Library.FullName.Replace("/", "\")

    if ($normalizedPath -like "*\bin\*") {
        return 0
    }

    if ($normalizedPath -like "*\$Configuration\*") {
        return 1
    }

    if ($normalizedPath -like "*\debug\*" -or $normalizedPath -like "*\Debug\*") {
        return 50
    }

    return 10
}

function Find-CompanionLibraries {
    param(
        [string]$Pattern,
        [string[]]$BuildRoots
    )

    $matches = @()

    foreach ($buildRoot in $BuildRoots) {
        if (-not (Test-Path $buildRoot)) {
            continue
        }

        $matches += Get-ChildItem -Path $buildRoot -Recurse -File -Filter $Pattern -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -notlike "*webtransport_native*" }
    }

    return @(
        $matches |
            Sort-Object @{ Expression = { Get-CompanionLibraryPriority -Library $_ } }, FullName |
            Group-Object Name |
            ForEach-Object { $_.Group[0] }
    )
}

function Copy-CompanionLibraries {
    param(
        [string]$RuntimeIdentifier,
        [string[]]$BuildRoots,
        [string[]]$DestinationDirectories,
        [switch]$AllowMissing
    )

    $copiedLibraries = @()

    foreach ($spec in (Get-CompanionLibrarySpecs -RuntimeIdentifier $RuntimeIdentifier)) {
        $companionLibraries = Find-CompanionLibraries -Pattern $spec.Pattern -BuildRoots $BuildRoots

        if ($companionLibraries.Count -eq 0) {
            $message = "Could not find required companion native library '$($spec.Pattern)' for $RuntimeIdentifier. Build the native library with its runtime dependencies or pass -AllowMissing."
            if ($spec.Required -and -not $AllowMissing) {
                throw $message
            }

            if ($spec.Required) {
                Write-Warning $message
            }

            continue
        }

        foreach ($match in $companionLibraries) {
            foreach ($destinationDirectory in $DestinationDirectories) {
                Copy-Item -Path $match.FullName -Destination (Join-Path $destinationDirectory $match.Name) -Force
            }

            $copiedLibraries += $match.Name
        }
    }

    return @($copiedLibraries | Sort-Object -Unique)
}

if ([string]::IsNullOrWhiteSpace($NuGetRuntimesDirectory)) {
    $NuGetRuntimesDirectory = Join-Path $RepositoryRoot "runtimes"
}

if ([string]::IsNullOrWhiteSpace($UpmPackageDirectory)) {
    $UpmPackageDirectory = Join-Path $RepositoryRoot "artifacts/upm/io.demiurgos.webtransport"
}

$libraryName = Get-NativeLibraryName -RuntimeIdentifier $Rid
$buildRoots = @()

if (-not [string]::IsNullOrWhiteSpace($NativeBuildDirectory)) {
    if ([System.IO.Path]::IsPathRooted($NativeBuildDirectory)) {
        $buildRoots += $NativeBuildDirectory
    }
    else {
        $buildRoots += (Join-Path $RepositoryRoot $NativeBuildDirectory)
    }
}

$buildRoots += @(
    (Join-Path $RepositoryRoot "artifacts/native-build-msquic-release"),
    (Join-Path $RepositoryRoot "artifacts/native-build-msquic"),
    (Join-Path $RepositoryRoot "artifacts/native-build")
)

$nativeLibrary = Find-NativeLibrary -FileName $libraryName -BuildRoots $buildRoots

if (-not $nativeLibrary) {
    $message = "Could not find $libraryName for $Rid. Build the native library first or pass -NativeBuildDirectory."
    if ($AllowMissing) {
        Write-Warning $message
        return
    }

    throw $message
}

$destinationDirectories = @()

if (-not $SkipNuGet) {
    $nugetNativeDirectory = Join-Path $NuGetRuntimesDirectory "$Rid/native"
    if ($Clean -and (Test-Path $nugetNativeDirectory)) {
        Remove-Item -Path $nugetNativeDirectory -Recurse -Force
    }

    New-Item -ItemType Directory -Path $nugetNativeDirectory -Force | Out-Null
    Copy-Item -Path $nativeLibrary -Destination (Join-Path $nugetNativeDirectory $libraryName) -Force
    $destinationDirectories += $nugetNativeDirectory
}

if (-not $SkipUpm) {
    $unityPluginDirectory = Join-Path $UpmPackageDirectory (Get-UnityPluginPath -RuntimeIdentifier $Rid)
    if ($Clean -and (Test-Path $unityPluginDirectory)) {
        Remove-Item -Path $unityPluginDirectory -Recurse -Force
    }

    New-Item -ItemType Directory -Path $unityPluginDirectory -Force | Out-Null
    Copy-Item -Path $nativeLibrary -Destination (Join-Path $unityPluginDirectory $libraryName) -Force
    $destinationDirectories += $unityPluginDirectory
}

$companionLibraries = Copy-CompanionLibraries -RuntimeIdentifier $Rid -BuildRoots $buildRoots -DestinationDirectories $destinationDirectories -AllowMissing:$AllowMissing

Write-Host "Staged $libraryName for $Rid from $nativeLibrary"
if ($companionLibraries.Count -gt 0) {
    Write-Host "Staged companion native libraries for ${Rid}: $($companionLibraries -join ', ')"
}
