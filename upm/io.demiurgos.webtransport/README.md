# WebTransport for Unity

`io.demiurgos.webtransport` packages the same `.NET Standard 2.1` managed assemblies used by regular C# consumers, plus native plugins laid out for Unity.

## Install

Use Unity Package Manager and add this package from a Git URL that points at the package subfolder:

```text
https://github.com/demiurgos/webtransport-csharp.git?path=/upm/io.demiurgos.webtransport
```

For release artifacts, prefer the generated package under `artifacts/upm/io.demiurgos.webtransport` because it contains the built managed assemblies and native plugins.

## Usage

```csharp
using System;
using WebTransport;

public static class WebTransportExample
{
    public static async void Connect()
    {
        await using var client = new WebTransportClient();
        await using var session = await client.ConnectAsync(new Uri("https://example.com/transport"));

        await session.SendDatagramAsync(new byte[] { 1, 2, 3 });
        WebTransportDatagram datagram = await session.ReceiveDatagramAsync();
    }
}
```

## Package Contents

The generated UPM package contains:

```text
Runtime/
  WebTransport.Client.dll
  WebTransport.Native.dll
Plugins/
  x86_64/webtransport_native.dll
  Android/arm64-v8a/libwebtransport_native.so
  iOS/libwebtransport_native.a
```

Managed code intentionally has no dependency on `UnityEngine`.

## Platform Notes

WebGL is not supported by the native backend. A browser JavaScript backend would need to be a separate package.

Windows builds that link MsQuic may require companion MsQuic runtime DLLs beside `webtransport_native.dll`. The release packaging scripts copy those companions when they are present in the native build output.

Android and iOS native plugins require platform-specific native builds. If a generated package does not include a plugin for your target platform, Unity will compile but the backend will fail to load at runtime on that platform.
