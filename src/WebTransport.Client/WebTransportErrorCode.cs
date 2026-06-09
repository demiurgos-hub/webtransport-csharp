namespace WebTransport
{
    /// <summary>
    /// High-level error category for WebTransport failures.
    /// </summary>
    public enum WebTransportErrorCode
    {
        Unknown = 0,
        InvalidArgument = 1,
        InvalidState = 2,
        TransportError = 3,
        ProtocolError = 4,
        TlsError = 5,
        Cancelled = 6,
        Unsupported = 7,
        NativeLibraryUnavailable = 8
    }
}
