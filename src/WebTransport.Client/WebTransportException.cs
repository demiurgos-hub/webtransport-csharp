using System;

namespace WebTransport
{
    /// <summary>
    /// Exception thrown by the managed WebTransport client.
    /// </summary>
    public sealed class WebTransportException : Exception
    {
        public WebTransportException(WebTransportErrorCode errorCode, string message)
            : base(message)
        {
            ErrorCode = errorCode;
        }

        public WebTransportException(WebTransportErrorCode errorCode, string message, Exception innerException)
            : base(message, innerException)
        {
            ErrorCode = errorCode;
        }

        public WebTransportException(WebTransportErrorCode errorCode, string message, ulong nativeErrorCode)
            : base(message)
        {
            ErrorCode = errorCode;
            NativeErrorCode = nativeErrorCode;
        }

        public WebTransportException(WebTransportErrorCode errorCode, string message, Exception innerException, ulong nativeErrorCode)
            : base(message, innerException)
        {
            ErrorCode = errorCode;
            NativeErrorCode = nativeErrorCode;
        }

        public WebTransportErrorCode ErrorCode { get; }

        public ulong NativeErrorCode { get; }
    }
}
