using System;
using WebTransport.Native;

namespace WebTransport
{
    internal interface INativeWebTransportOperations
    {
        ulong OpenBidirectionalStream(ulong sessionId);

        ulong OpenUnidirectionalStream(ulong sessionId);

        ulong SendDatagram(ulong sessionId, ReadOnlySpan<byte> payload);

        bool TryReceiveDatagram(ulong sessionId, Span<byte> buffer, out int bytesRead);

        int ReadStream(ulong streamId, Span<byte> buffer);

        ulong WriteStream(ulong streamId, ReadOnlySpan<byte> payload, bool endStream);

        ulong FinishStream(ulong streamId);

        void ResetStream(ulong streamId, ulong errorCode);

        void CloseSession(ulong sessionId, ulong errorCode, string reason);

        void Release(ulong handle);
    }

    internal sealed class NativeWebTransportOperations : INativeWebTransportOperations
    {
        private readonly NativeWebTransportBackend _native;

        public NativeWebTransportOperations(NativeWebTransportBackend native)
        {
            _native = native ?? throw new ArgumentNullException(nameof(native));
        }

        public ulong OpenBidirectionalStream(ulong sessionId)
        {
            return _native.OpenBidirectionalStream(sessionId);
        }

        public ulong OpenUnidirectionalStream(ulong sessionId)
        {
            return _native.OpenUnidirectionalStream(sessionId);
        }

        public ulong SendDatagram(ulong sessionId, ReadOnlySpan<byte> payload)
        {
            return _native.SendDatagram(sessionId, payload);
        }

        public bool TryReceiveDatagram(ulong sessionId, Span<byte> buffer, out int bytesRead)
        {
            return _native.TryReceiveDatagram(sessionId, buffer, out bytesRead);
        }

        public int ReadStream(ulong streamId, Span<byte> buffer)
        {
            return _native.ReadStream(streamId, buffer);
        }

        public ulong WriteStream(ulong streamId, ReadOnlySpan<byte> payload, bool endStream)
        {
            return _native.WriteStream(streamId, payload, endStream);
        }

        public ulong FinishStream(ulong streamId)
        {
            return _native.FinishStream(streamId);
        }

        public void ResetStream(ulong streamId, ulong errorCode)
        {
            _native.ResetStream(streamId, errorCode);
        }

        public void CloseSession(ulong sessionId, ulong errorCode, string reason)
        {
            _native.CloseSession(sessionId, errorCode, reason);
        }

        public void Release(ulong handle)
        {
            _native.Release(handle);
        }
    }
}
