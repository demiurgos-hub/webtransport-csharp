using System;
using System.Runtime.InteropServices;

namespace WebTransport.Native
{
    /// <summary>
    /// Event produced by the native WebTransport completion queue.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct WebTransportNativeEvent
    {
        public WebTransportNativeEventType Type;
        public WebTransportNativeStatus Status;
        public ulong ClientId;
        public ulong SessionId;
        public ulong StreamId;
        public IntPtr Data;
        public nuint DataLength;
        public ulong ErrorCode;
    }
}
