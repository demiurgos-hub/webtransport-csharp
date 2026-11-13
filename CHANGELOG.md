# Changelog

## Unreleased

### Added

- Native QUIC idle expiry reports `WebTransportErrorCode.IdleTimeout`, with the original platform status retained in `NativeErrorCode` and the diagnostic message.
- `WebTransportSession.CloseCompletion` preserves the peer's WebTransport application close code and UTF-8 reason, including normal code-zero closes. Transport loss and local disposal fault the task instead of inventing application metadata.

### Fixed

- Preserve the first transport failure for late observers after subsequent shutdown events or disposal.
- Parse fragmented close capsules across HTTP/3 DATA frames, bound close reasons to 1024 bytes, reject invalid UTF-8, and retain the first terminal result across subsequent stream shutdown.

### Changed

- **Breaking:** custom `IWebTransportSessionBackend` implementations must provide `CloseCompletion`. Native ABI 2 is required; ship the matching native and managed 0.3.0 assemblies together.

## 0.1.3

- Marks the package as a Windows x64 preview release.
- Packages `webtransport_native.dll` with its required `msquic.dll` companion library for `win-x64`.
- Adds a native ABI version export and managed startup validation for native/managed package compatibility.
- Improves release packaging checks, Unity package assembly, and SourceLink metadata.

