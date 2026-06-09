param(
    [string]$RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string]$Configuration = "Release",
    [string]$Rid = "win-x64",
    [string]$TemplateDirectory = "",
    [string]$OutputDirectory = "",
    [switch]$NoBuild,
    [switch]$SkipNativeAssets,
    [switch]$AllowMissingNativeAssets,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($TemplateDirectory)) {
    $TemplateDirectory = Join-Path $RepositoryRoot "upm/io.demiurgos.webtransport"
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $RepositoryRoot "artifacts/upm/io.demiurgos.webtransport"
}

function Get-PackageVersion {
    param([string]$RepositoryRoot)

    [xml]$props = Get-Content -Path (Join-Path $RepositoryRoot "Directory.Build.props")
    $version = $props.Project.PropertyGroup.PackageVersion

    if ([string]::IsNullOrWhiteSpace($version)) {
        $version = $props.Project.PropertyGroup.Version
    }

    if ([string]::IsNullOrWhiteSpace($version) -or $version.Contains('$(')) {
        return "0.1.0"
    }

    return $version
}

function Get-UnityGuid {
    param([string]$Path)

    $md5 = [System.Security.Cryptography.MD5]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Path.Replace("\", "/").ToLowerInvariant())
        return -join ($md5.ComputeHash($bytes) | ForEach-Object { $_.ToString("x2") })
    }
    finally {
        $md5.Dispose()
    }
}

function Get-RelativePackagePath {
    param([string]$AssetPath)

    $baseUri = [System.Uri]((Resolve-Path $OutputDirectory).Path.TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar)
    $assetUri = [System.Uri](Resolve-Path $AssetPath).Path
    return [System.Uri]::UnescapeDataString($baseUri.MakeRelativeUri($assetUri).ToString())
}

function Write-DefaultMeta {
    param([string]$AssetPath)

    $relativePath = Get-RelativePackagePath -AssetPath $AssetPath
    $guid = Get-UnityGuid -Path "io.demiurgos.webtransport/$relativePath"
    $metaPath = "$AssetPath.meta"

    @"
fileFormatVersion: 2
guid: $guid
DefaultImporter:
  externalObjects: {}
  userData:
  assetBundleName:
  assetBundleVariant:
"@ | Set-Content -Path $metaPath -Encoding UTF8
}

function Write-ManagedPluginMeta {
    param([string]$AssetPath)

    $relativePath = Get-RelativePackagePath -AssetPath $AssetPath
    $guid = Get-UnityGuid -Path "io.demiurgos.webtransport/$relativePath"
    $metaPath = "$AssetPath.meta"

    @"
fileFormatVersion: 2
guid: $guid
PluginImporter:
  externalObjects: {}
  serializedVersion: 2
  iconMap: {}
  executionOrder: {}
  defineConstraints: []
  isPreloaded: 0
  isOverridable: 0
  isExplicitlyReferenced: 0
  validateReferences: 1
  platformData:
  - first:
      Any: Any
    second:
      enabled: 1
      settings: {}
  userData:
  assetBundleName:
  assetBundleVariant:
"@ | Set-Content -Path $metaPath -Encoding UTF8
}

function Write-NativePluginMeta {
    param(
        [string]$AssetPath,
        [string]$PlatformName
    )

    $relativePath = Get-RelativePackagePath -AssetPath $AssetPath
    $guid = Get-UnityGuid -Path "io.demiurgos.webtransport/$relativePath"
    $metaPath = "$AssetPath.meta"
    $platformData = if ($PlatformName -eq "Any") {
@"
  - first:
      Any: Any
    second:
      enabled: 1
      settings: {}
"@
    }
    else {
@"
  - first:
      Any: Any
    second:
      enabled: 0
      settings: {}
  - first:
      ${PlatformName}: $PlatformName
    second:
      enabled: 1
      settings: {}
"@
    }

    @"
fileFormatVersion: 2
guid: $guid
PluginImporter:
  externalObjects: {}
  serializedVersion: 2
  iconMap: {}
  executionOrder: {}
  defineConstraints: []
  isPreloaded: 0
  isOverridable: 0
  isExplicitlyReferenced: 0
  validateReferences: 1
  platformData:
$platformData
  userData:
  assetBundleName:
  assetBundleVariant:
"@ | Set-Content -Path $metaPath -Encoding UTF8
}

function Write-MetaFiles {
    $defaultExtensions = @(".asmdef", ".json", ".md")

    foreach ($asset in (Get-ChildItem -Path $OutputDirectory -Recurse -File | Where-Object { $_.Extension -ne ".meta" })) {
        if ($asset.Extension -eq ".dll" -and $asset.FullName -like "*\Runtime\*") {
            Write-ManagedPluginMeta -AssetPath $asset.FullName
            continue
        }

        if ($asset.Extension -eq ".dll" -and $asset.FullName -like "*\Plugins\*") {
            Write-NativePluginMeta -AssetPath $asset.FullName -PlatformName "Any"
            continue
        }

        if ($asset.Name -eq "libwebtransport_native.so" -and $asset.FullName -like "*\Android\*") {
            Write-NativePluginMeta -AssetPath $asset.FullName -PlatformName "Android"
            continue
        }

        if ($asset.Name -eq "libwebtransport_native.so") {
            Write-NativePluginMeta -AssetPath $asset.FullName -PlatformName "Linux"
            continue
        }

        if ($asset.Name -eq "libwebtransport_native.dylib") {
            Write-NativePluginMeta -AssetPath $asset.FullName -PlatformName "OSX"
            continue
        }

        if ($asset.Name -eq "libwebtransport_native.a") {
            Write-NativePluginMeta -AssetPath $asset.FullName -PlatformName "iOS"
            continue
        }

        if ($defaultExtensions -contains $asset.Extension) {
            Write-DefaultMeta -AssetPath $asset.FullName
        }
    }
}

function Get-RequiredNativePluginPaths {
    param([string]$RuntimeIdentifier)

    switch ($RuntimeIdentifier) {
        "win-x64" {
            return @(
                "Plugins/x86_64/webtransport_native.dll",
                "Plugins/x86_64/msquic.dll"
            )
        }
        "linux-x64" {
            return @("Plugins/Linux/x86_64/libwebtransport_native.so")
        }
        "osx-arm64" {
            return @("Plugins/macOS/arm64/libwebtransport_native.dylib")
        }
        "android-arm64" {
            return @("Plugins/Android/arm64-v8a/libwebtransport_native.so")
        }
        "ios" {
            return @("Plugins/iOS/libwebtransport_native.a")
        }
        default {
            return @()
        }
    }
}

function Test-UpmNativeAssets {
    param([switch]$AllowMissing)

    $pluginDirectory = Join-Path $OutputDirectory "Plugins"
    $requiredPluginPaths = Get-RequiredNativePluginPaths -RuntimeIdentifier $Rid

    if (-not (Test-Path $pluginDirectory)) {
        $message = "UPM package does not contain a Plugins directory with native assets."
        if ($AllowMissing -or $requiredPluginPaths.Count -eq 0) {
            Write-Warning $message
            return
        }

        throw $message
    }

    $danglingMetaFiles = Get-ChildItem -Path $pluginDirectory -Recurse -File -Filter "*.meta" -ErrorAction SilentlyContinue |
        Where-Object {
            $assetPath = $_.FullName.Substring(0, $_.FullName.Length - ".meta".Length)
            -not (Test-Path $assetPath)
        }

    if ($danglingMetaFiles) {
        $relativePaths = $danglingMetaFiles | ForEach-Object { Get-RelativePackagePath -AssetPath $_.FullName }
        throw "UPM package contains native plugin .meta files without matching assets: $($relativePaths -join ', ')"
    }

    $missingPluginPaths = @(
        foreach ($relativePath in $requiredPluginPaths) {
            $assetPath = Join-Path $OutputDirectory $relativePath
            if (-not (Test-Path $assetPath)) {
                $relativePath
            }
        }
    )

    if ($missingPluginPaths.Count -gt 0) {
        $message = "UPM package is missing required native plugin assets: $($missingPluginPaths -join ', ')"
        if ($AllowMissing) {
            Write-Warning $message
            return
        }

        throw $message
    }
}

if ($Clean -and (Test-Path $OutputDirectory)) {
    Remove-Item -Path $OutputDirectory -Recurse -Force
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
Copy-Item -Path (Join-Path $TemplateDirectory "*") -Destination $OutputDirectory -Recurse -Force

$packageJsonPath = Join-Path $OutputDirectory "package.json"
$packageJson = Get-Content -Path $packageJsonPath -Raw | ConvertFrom-Json
$packageJson.version = Get-PackageVersion -RepositoryRoot $RepositoryRoot
$packageJson | ConvertTo-Json -Depth 10 | Set-Content -Path $packageJsonPath -Encoding UTF8

if (-not $NoBuild) {
    dotnet build (Join-Path $RepositoryRoot "src/WebTransport.Native/WebTransport.Native.csproj") -c $Configuration
    dotnet build (Join-Path $RepositoryRoot "src/WebTransport.Client/WebTransport.Client.csproj") -c $Configuration
}

$runtimeDirectory = Join-Path $OutputDirectory "Runtime"
New-Item -ItemType Directory -Path $runtimeDirectory -Force | Out-Null

$managedArtifacts = @(
    @{
        Project = "WebTransport.Native"
        Assembly = "WebTransport.Native.dll"
    },
    @{
        Project = "WebTransport.Client"
        Assembly = "WebTransport.Client.dll"
    }
)

foreach ($artifact in $managedArtifacts) {
    $sourceDirectory = Join-Path $RepositoryRoot "src/$($artifact.Project)/bin/$Configuration/netstandard2.1"
    $assemblyPath = Join-Path $sourceDirectory $artifact.Assembly

    if (-not (Test-Path $assemblyPath)) {
        throw "Managed assembly '$assemblyPath' was not found. Build the project first or run without -NoBuild."
    }

    Copy-Item -Path $assemblyPath -Destination (Join-Path $runtimeDirectory $artifact.Assembly) -Force

    $xmlPath = [System.IO.Path]::ChangeExtension($assemblyPath, ".xml")
    if (Test-Path $xmlPath) {
        Copy-Item -Path $xmlPath -Destination (Join-Path $runtimeDirectory ([System.IO.Path]::GetFileName($xmlPath))) -Force
    }
}

if (-not $SkipNativeAssets) {
    $stageArgs = @{
        RepositoryRoot = $RepositoryRoot
        UpmPackageDirectory = $OutputDirectory
        Rid = $Rid
        Configuration = $Configuration
        SkipNuGet = $true
        Clean = $true
    }

    if ($AllowMissingNativeAssets) {
        $stageArgs.AllowMissing = $true
    }

    & (Join-Path $PSScriptRoot "Stage-NativeAssets.ps1") @stageArgs
}

Write-MetaFiles
Test-UpmNativeAssets -AllowMissing:$AllowMissingNativeAssets

Write-Host "Assembled UPM package at $OutputDirectory"
