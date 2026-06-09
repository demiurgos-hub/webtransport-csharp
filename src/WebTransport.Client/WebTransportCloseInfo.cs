namespace WebTransport
{
    /// <summary>
    /// Close information sent when ending a WebTransport session.
    /// </summary>
    public readonly struct WebTransportCloseInfo
    {
        public WebTransportCloseInfo(ulong errorCode, string reason)
        {
            ErrorCode = errorCode;
            Reason = reason ?? string.Empty;
        }

        public ulong ErrorCode { get; }

        public string Reason { get; }
    }
}
