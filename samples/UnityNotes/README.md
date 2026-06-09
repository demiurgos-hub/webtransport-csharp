# Unity Notes

`Demiurgos.WebTransport.Client` targets `.NET Standard 2.1` and does not expose Unity-specific APIs.

Unity consumers should prefer the generated UPM package at:

```text
artifacts/upm/io.demiurgos.webtransport
```

Release packaging is assembled with:

```powershell
.\scripts\Build-Packages.ps1 -Configuration Release -NativePreset windows-msquic-release -Rid win-x64
```

Manual Unity imports should include:

- `WebTransport.Client.dll`
- `WebTransport.Native.dll`
- the native `webtransport_native` binary for each target platform

The native binaries should be placed using Unity's plugin import layout, for example:

```text
Assets/Plugins/x86_64/webtransport_native.dll
Assets/Plugins/Android/arm64-v8a/libwebtransport_native.so
Assets/Plugins/iOS/libwebtransport_native.a
```

WebGL is not supported by the native backend. A browser JavaScript backend would be a separate package.
