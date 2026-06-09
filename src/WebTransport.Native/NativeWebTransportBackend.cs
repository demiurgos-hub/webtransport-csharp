using System;
using System.Text;

namespace WebTransport.Native
{
    /// <summary>
    /// Thin managed facade over the native WebTransport C ABI.
    /// </summary>
    public sealed class NativeWebTransportBackend : IDisposable
    {
        private ulong _clientId;
        private bool _disposed;

        public NativeWebTransportBackend()
        {
            EnsureCompatibleAbi();
            EnsureOk(NativeMethods.ClientCreate(out _clientId));
        }

        ~NativeWebTransportBackend()
        {
            Dispose();
        }

        public ulong ClientId => _clientId;

        public ulong Connect(string url, string headers, uint connectTimeoutMilliseconds, uint idleTimeoutMilliseconds, bool allowUntrustedCertificates, bool enableDatagrams)
        {
            if (url is null)
            {
                throw new ArgumentNullException(nameof(url));
            }

            ThrowIfDisposed();

            byte[] urlBytes = Encoding.UTF8.GetBytes(url);
            byte[] headerBytes = Encoding.UTF8.GetBytes(headers ?? string.Empty);

            unsafe
            {
                fixed (byte* urlPtr = urlBytes)
                fixed (byte* headerPtr = headerBytes)
                {
                    var options = new WebTransportNativeConnectOptions
                    {
                        UrlUtf8 = (IntPtr)urlPtr,
                        UrlLength = (nuint)urlBytes.Length,
                        HeadersUtf8 = (IntPtr)headerPtr,
                        HeadersLength = (nuint)headerBytes.Length,
                        ConnectTimeoutMilliseconds = connectTimeoutMilliseconds,
                        IdleTimeoutMilliseconds = idleTimeoutMilliseconds,
                        AllowUntrustedCertificates = allowUntrustedCertificates ? (byte)1 : (byte)0,
                        EnableDatagrams = enableDatagrams ? (byte)1 : (byte)0
                    };

                    EnsureOk(NativeMethods.ClientConnect(_clientId, in options, out ulong operationId));
                    return operationId;
                }
            }
        }

        public bool TryPollEvent(out WebTransportNativeEvent nativeEvent)
        {
            ThrowIfDisposed();

            WebTransportNativeStatus status = NativeMethods.PollEvent(_clientId, out nativeEvent);
            if (status == WebTransportNativeStatus.NotFound || status == WebTransportNativeStatus.Pending)
            {
                return false;
            }

            EnsureOk(status);
            return true;
        }

        public bool TryPollEvent(TimeSpan timeout, out WebTransportNativeEvent nativeEvent)
        {
            ThrowIfDisposed();

            DateTime deadline = DateTime.UtcNow + timeout;
            do
            {
                if (TryPollEvent(out nativeEvent))
                {
                    return true;
                }

                System.Threading.Thread.Sleep(10);
            }
            while (DateTime.UtcNow < deadline);

            nativeEvent = default;
            return false;
        }

        public ulong OpenBidirectionalStream(ulong sessionId)
        {
            ThrowIfDisposed();
            EnsureOk(NativeMethods.SessionOpenBidirectionalStream(sessionId, out ulong operationId));
            return operationId;
        }

        public ulong OpenUnidirectionalStream(ulong sessionId)
        {
            ThrowIfDisposed();
            EnsureOk(NativeMethods.SessionOpenUnidirectionalStream(sessionId, out ulong operationId));
            return operationId;
        }

        public unsafe ulong SendDatagram(ulong sessionId, ReadOnlySpan<byte> payload)
        {
            ThrowIfDisposed();

            fixed (byte* payloadPtr = payload)
            {
                EnsureOk(NativeMethods.SessionSendDatagram(sessionId, (IntPtr)payloadPtr, (nuint)payload.Length, out ulong operationId));
                return operationId;
            }
        }

        public unsafe bool TryReceiveDatagram(ulong sessionId, Span<byte> buffer, out int bytesRead)
        {
            ThrowIfDisposed();

            fixed (byte* bufferPtr = buffer)
            {
                WebTransportNativeStatus status = NativeMethods.SessionReceiveDatagram(sessionId, (IntPtr)bufferPtr, (nuint)buffer.Length, out nuint nativeBytesRead);
                if (status == WebTransportNativeStatus.NotFound)
                {
                    bytesRead = 0;
                    return false;
                }

                EnsureOk(status);
                bytesRead = checked((int)nativeBytesRead);
                return true;
            }
        }

        public unsafe int ReadStream(ulong streamId, Span<byte> buffer)
        {
            ThrowIfDisposed();

            fixed (byte* bufferPtr = buffer)
            {
                WebTransportNativeStatus status = NativeMethods.StreamRead(streamId, (IntPtr)bufferPtr, (nuint)buffer.Length, out nuint bytesRead);
                if (status == WebTransportNativeStatus.NotFound)
                {
                    return 0;
                }

                EnsureOk(status);
                return checked((int)bytesRead);
            }
        }

        public unsafe ulong WriteStream(ulong streamId, ReadOnlySpan<byte> payload, bool endStream)
        {
            ThrowIfDisposed();

            fixed (byte* payloadPtr = payload)
            {
                EnsureOk(NativeMethods.StreamWrite(streamId, (IntPtr)payloadPtr, (nuint)payload.Length, endStream ? (byte)1 : (byte)0, out ulong operationId));
                return operationId;
            }
        }

        public ulong FinishStream(ulong streamId)
        {
            ThrowIfDisposed();
            EnsureOk(NativeMethods.StreamFinish(streamId, out ulong operationId));
            return operationId;
        }

        public void ResetStream(ulong streamId, ulong errorCode)
        {
            ThrowIfDisposed();
            EnsureOk(NativeMethods.StreamReset(streamId, errorCode));
        }

        public unsafe void CloseSession(ulong sessionId, ulong errorCode, string reason)
        {
            ThrowIfDisposed();

            byte[] reasonBytes = Encoding.UTF8.GetBytes(reason ?? string.Empty);
            fixed (byte* reasonPtr = reasonBytes)
            {
                EnsureOk(NativeMethods.SessionClose(sessionId, errorCode, (IntPtr)reasonPtr, (nuint)reasonBytes.Length));
            }
        }

        public void Release(ulong handle)
        {
            if (handle != 0)
            {
                NativeMethods.Release(handle);
            }
        }

        public void Shutdown(ulong errorCode = 0)
        {
            if (_clientId != 0)
            {
                EnsureOk(NativeMethods.ClientShutdown(_clientId, errorCode));
            }
        }

        public void Dispose()
        {
            if (_disposed)
            {
                return;
            }

            Shutdown();
            Release(_clientId);
            _clientId = 0;
            _disposed = true;
            GC.SuppressFinalize(this);
        }

        private static void EnsureOk(WebTransportNativeStatus status)
        {
            if (status != WebTransportNativeStatus.Ok)
            {
                throw new WebTransportNativeException(status);
            }
        }

        private static void EnsureCompatibleAbi()
        {
            uint actualAbiVersion = NativeMethods.GetAbiVersion();
            if (actualAbiVersion != NativeMethods.ExpectedAbiVersion)
            {
                throw new WebTransportNativeException(
                    WebTransportNativeStatus.Unsupported,
                    $"Native WebTransport backend ABI version {actualAbiVersion} is not compatible with managed ABI version {NativeMethods.ExpectedAbiVersion}.");
            }
        }

        private void ThrowIfDisposed()
        {
            if (_disposed)
            {
                throw new ObjectDisposedException(nameof(NativeWebTransportBackend));
            }
        }
    }
}
