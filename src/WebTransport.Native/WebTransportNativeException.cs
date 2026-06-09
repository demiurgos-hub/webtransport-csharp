using System;

namespace WebTransport.Native
{
    /// <summary>
    /// Exception thrown when the native WebTransport backend returns an error.
    /// </summary>
    public sealed class WebTransportNativeException : Exception
    {
        public WebTransportNativeException(WebTransportNativeStatus status)
            : base($"Native WebTransport backend returned {status}.")
        {
            Status = status;
        }

        public WebTransportNativeException(WebTransportNativeStatus status, string message)
            : base(message)
        {
            Status = status;
        }

        public WebTransportNativeStatus Status { get; }
    }
}
