using WebTransport.Native;

namespace WebTransport
{
    internal static class NativeErrorMapper
    {
        public static WebTransportException Map(WebTransportNativeException exception)
        {
            WebTransportErrorCode errorCode;
            switch (exception.Status)
            {
                case WebTransportNativeStatus.InvalidArgument:
                    errorCode = WebTransportErrorCode.InvalidArgument;
                    break;
                case WebTransportNativeStatus.InvalidState:
                    errorCode = WebTransportErrorCode.InvalidState;
                    break;
                case WebTransportNativeStatus.TransportError:
                    errorCode = WebTransportErrorCode.TransportError;
                    break;
                case WebTransportNativeStatus.ProtocolError:
                    errorCode = WebTransportErrorCode.ProtocolError;
                    break;
                case WebTransportNativeStatus.TlsError:
                    errorCode = WebTransportErrorCode.TlsError;
                    break;
                case WebTransportNativeStatus.Cancelled:
                    errorCode = WebTransportErrorCode.Cancelled;
                    break;
                case WebTransportNativeStatus.Unsupported:
                    errorCode = WebTransportErrorCode.Unsupported;
                    break;
                default:
                    errorCode = WebTransportErrorCode.Unknown;
                    break;
            }

            return new WebTransportException(errorCode, exception.Message, exception);
        }
    }
}
