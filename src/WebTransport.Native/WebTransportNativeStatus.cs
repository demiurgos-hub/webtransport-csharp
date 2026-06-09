namespace WebTransport.Native
{
    /// <summary>
    /// Status codes returned by the native WebTransport ABI.
    /// </summary>
    public enum WebTransportNativeStatus
    {
        Ok = 0,
        Pending = 1,
        NotFound = 2,
        InvalidArgument = -1,
        InvalidState = -2,
        TransportError = -3,
        ProtocolError = -4,
        TlsError = -5,
        Cancelled = -6,
        OutOfMemory = -7,
        Unsupported = -8,
        Unknown = -255
    }
}
