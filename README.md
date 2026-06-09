# WebTransport C# Client

`WebTransport.Client` is a general-purpose C# WebTransport client package targeting `.NET Standard 2.1`.

The package is designed to be usable from ordinary C# projects and Unity without exposing Unity-specific APIs.

## Current Status

This package is currently a Windows x64 preview. The supported runtime is `win-x64`, with a native backend built on MsQuic and packaged with its required `msquic.dll` companion library.

The Windows x64 path is the production-readiness focus: client sessions, bidirectional streams, datagrams, managed/native lifetime handling, and native package validation are in place. Other native runtimes are not release-supported yet, even where experimental build presets exist. Unity desktop builds can use the native backend; Unity WebGL, mobile, Linux, and macOS release packages still need dedicated backend/runtime work before they should be treated as supported.

## Why WebTransport

WebTransport brings QUIC and HTTP/3 to client application networking: multiplexed streams, unreliable datagrams, modern congestion control, and TLS by default without the head-of-line blocking tradeoffs of a single WebSocket stream. For real-time games, simulation, collaboration, media, and interactive tools, it gives one connection that can carry reliable stream data and low-latency datagrams side by side. Browser support and server support are still maturing, but WebTransport is expected to become a mainstream option for latency-sensitive web and app networking.

## Usage Shape

```csharp
await using var client = new WebTransportClient();
await using WebTransportSession session = await client.ConnectAsync(
    new Uri("https://example.com/transport"));

await using WebTransportStream stream = await session.OpenBidirectionalStreamAsync();
await stream.WriteAsync(new byte[] { 1, 2, 3 });

await session.SendDatagramAsync(new byte[] { 4, 5, 6 });
WebTransportDatagram datagram = await session.ReceiveDatagramAsync();
```

`ConnectAsync` returns only after the remote endpoint accepts the WebTransport extended CONNECT request.

## Packages

- `WebTransport.Client`: public API for C# consumers.
- `WebTransport.Native`: native loader and ABI wrapper.
- `io.demiurgos.webtransport`: Unity Package Manager package assembled from the same managed and native release artifacts.

The current release-supported NuGet native runtime layout is:

```text
runtimes/win-x64/native/webtransport_native.dll
runtimes/win-x64/native/msquic.dll
```

The managed package checks the loaded native backend ABI version at startup so mismatched managed/native package versions fail explicitly.

## Native Loading

On Windows x64, `WebTransport.Native` loads `webtransport_native.dll` through .NET's normal native library resolution. The native DLL depends on `msquic.dll`; both files must be present in `runtimes/win-x64/native` in the NuGet package or in Unity's `Plugins/x86_64` directory for the UPM package.

If native loading fails, first verify that the application is running as a 64-bit Windows process and that `webtransport_native.dll` and `msquic.dll` are copied next to each other. MsQuic is redistributed as the native QUIC/TLS transport used by this package.

## Unity

Unity users should consume the same `.NET Standard 2.1` managed assemblies as other C# users. The UPM package template lives under `upm/io.demiurgos.webtransport`, and generated release packages are written to `artifacts/upm/io.demiurgos.webtransport`.

Unity desktop builds can use the packaged Windows x64 native plugin. Unity WebGL builds cannot use this native backend because WebGL runs inside the browser sandbox and cannot load the MsQuic DLL through P/Invoke. A WebGL-capable backend is possible, but it would need to call the browser's WebTransport JavaScript API and would be shipped as separate backend/runtime support.

## Build

Managed build and tests:

```powershell
dotnet test WebTransport.sln
```

The native backend requires MsQuic. Configure `VCPKG_ROOT` before building native presets.

## Contributor Native Dependencies

This repo uses vcpkg manifest mode for native dependencies. Install vcpkg once somewhere under your user or developer tools directory:

```powershell
mkdir $env:USERPROFILE\dev
git clone https://github.com/microsoft/vcpkg.git $env:USERPROFILE\dev\vcpkg
& "$env:USERPROFILE\dev\vcpkg\bootstrap-vcpkg.bat"
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", "$env:USERPROFILE\dev\vcpkg", "User")
$env:VCPKG_ROOT = "$env:USERPROFILE\dev\vcpkg"
```

Then build the native library with MsQuic:

```powershell
.\scripts\build-native.ps1 -Preset windows-msquic
```

The `windows-msquic` preset uses `vcpkg.json` to restore `msquic`. Release packages should use `windows-msquic-release`, which keeps MsQuic enabled and disables native test hooks.

With the MsQuic presets, Windows native outputs are written under:

```text
artifacts/native-build-msquic/native/webtransport_native/Release/
artifacts/native-build-msquic-release/native/webtransport_native/Release/
```

Stage native runtime assets, pack NuGet packages, and assemble the UPM package:

```powershell
.\scripts\Build-Packages.ps1 -Configuration Release -NativePreset windows-msquic-release -Rid win-x64
```

For local script validation without rebuilding native code:

```powershell
.\scripts\Build-Packages.ps1 -Configuration Release -SkipNativeBuild -SkipTests -AllowMissingNativeAssets
```

NuGet packages are written to `artifacts/nuget`. The generated UPM package is written to `artifacts/upm/io.demiurgos.webtransport`.

For local interop tests, copy the built native library next to the test assembly:

```powershell
Copy-Item artifacts/native-build-msquic/native/webtransport_native/Release/webtransport_native.dll `
  tests/WebTransport.Interop.Tests/bin/Debug/net8.0/ -Force
```

vcpkg manifest installs are local to the CMake build tree by default, under `artifacts/native-build-msquic/vcpkg_installed`. If vcpkg produces an additional runtime DLL for your triplet, copy that DLL next to `webtransport_native.dll` as well.

Run the opt-in MsQuic connection test:

```powershell
$env:WEBTRANSPORT_MSQUIC_TEST_URL = "https://example.com/transport"
$env:WEBTRANSPORT_ALLOW_UNTRUSTED_CERTS = "1"
dotnet test WebTransport.sln
```

The opt-in test expects a native build linked with MsQuic and reports either a connection event or a meaningful transport/TLS failure. It does not yet establish a complete WebTransport session.
