namespace WebTransport.Native
{
    /// <summary>
    /// Event kinds emitted by the native completion queue.
    /// </summary>
    public enum WebTransportNativeEventType
    {
        None = 0,
        ClientConnected = 1,
        ClientClosed = 2,
        SessionConnected = 3,
        SessionClosed = 4,
        BidirectionalStreamOpened = 5,
        UnidirectionalStreamOpened = 6,
        StreamDataReceived = 7,
        StreamClosed = 8,
        DatagramReceived = 9,
        Error = 10
    }
}
