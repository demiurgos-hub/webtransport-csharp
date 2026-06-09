using System;
using System.Buffers;
using System.Threading;
using System.Threading.Tasks;
using WebTransport.Native;

namespace WebTransport
{
    internal sealed class NativeSessionBackend : IWebTransportSessionBackend
    {
        private const int MaxCloseReasonBytes = 1024;

        private readonly INativeWebTransportOperations _native;
        private readonly NativeEventCoordinator _events;
        private readonly ulong _sessionId;
        private bool _disposed;

        public NativeSessionBackend(NativeWebTransportBackend native, NativeEventCoordinator events, ulong sessionId)
            : this(new NativeWebTransportOperations(native), events, sessionId)
        {
        }

        internal NativeSessionBackend(INativeWebTransportOperations native, NativeEventCoordinator events, ulong sessionId)
        {
            _native = native ?? throw new ArgumentNullException(nameof(native));
            _events = events ?? throw new ArgumentNullException(nameof(events));
            _sessionId = sessionId;
        }

        public ValueTask<IWebTransportStreamBackend> OpenBidirectionalStreamAsync(CancellationToken cancellationToken)
        {
            ThrowIfDisposed();
            cancellationToken.ThrowIfCancellationRequested();
            ulong streamId = InvokeNative(() => _native.OpenBidirectionalStream(_sessionId));
            return new ValueTask<IWebTransportStreamBackend>(new NativeStreamBackend(_native, _events, streamId));
        }

        public ValueTask<IWebTransportStreamBackend> OpenUnidirectionalStreamAsync(CancellationToken cancellationToken)
        {
            ThrowIfDisposed();
            cancellationToken.ThrowIfCancellationRequested();
            ulong streamId = InvokeNative(() => _native.OpenUnidirectionalStream(_sessionId));
            return new ValueTask<IWebTransportStreamBackend>(new NativeStreamBackend(_native, _events, streamId));
        }

        public async ValueTask<IWebTransportStreamBackend> AcceptBidirectionalStreamAsync(CancellationToken cancellationToken)
        {
            return await AcceptStreamAsync(bidirectional: true, cancellationToken).ConfigureAwait(false);
        }

        public async ValueTask<IWebTransportStreamBackend> AcceptUnidirectionalStreamAsync(CancellationToken cancellationToken)
        {
            return await AcceptStreamAsync(bidirectional: false, cancellationToken).ConfigureAwait(false);
        }

        public ValueTask SendDatagramAsync(ReadOnlyMemory<byte> payload, CancellationToken cancellationToken)
        {
            ThrowIfDisposed();
            cancellationToken.ThrowIfCancellationRequested();
            InvokeNative(() => _native.SendDatagram(_sessionId, payload.Span));
            return default;
        }

        public async ValueTask<WebTransportDatagram> ReceiveDatagramAsync(CancellationToken cancellationToken)
        {
            byte[] buffer = ArrayPool<byte>.Shared.Rent(64 * 1024);
            try
            {
                while (true)
                {
                    ThrowIfDisposed();
                    cancellationToken.ThrowIfCancellationRequested();

                    int bytesRead;
                    if (TryReceiveDatagram(buffer, out bytesRead))
                    {
                        return CreateDatagram(buffer, bytesRead);
                    }

                    using (NativeEventCoordinator.NativeEventWaiter waiter = _events.RegisterDatagramWaiter(_sessionId, cancellationToken))
                    {
                        if (TryReceiveDatagram(buffer, out bytesRead))
                        {
                            return CreateDatagram(buffer, bytesRead);
                        }

                        await waiter.Task.ConfigureAwait(false);
                    }
                }
            }
            finally
            {
                ArrayPool<byte>.Shared.Return(buffer);
            }
        }

        public ValueTask CloseAsync(WebTransportCloseInfo closeInfo, CancellationToken cancellationToken)
        {
            ThrowIfDisposed();
            cancellationToken.ThrowIfCancellationRequested();
            ValidateCloseInfo(closeInfo);
            InvokeNative(() => _native.CloseSession(_sessionId, closeInfo.ErrorCode, closeInfo.Reason));
            return default;
        }

        public ValueTask DisposeAsync()
        {
            if (_disposed)
            {
                return default;
            }

            try
            {
                InvokeNative(() => _native.CloseSession(_sessionId, 0, string.Empty));
            }
            finally
            {
                InvokeNative(() => _native.Release(_sessionId));
            }

            _disposed = true;
            return default;
        }

        private void ThrowIfDisposed()
        {
            if (_disposed)
            {
                throw new ObjectDisposedException(nameof(NativeSessionBackend));
            }
        }

        private async ValueTask<IWebTransportStreamBackend> AcceptStreamAsync(bool bidirectional, CancellationToken cancellationToken)
        {
            while (true)
            {
                ThrowIfDisposed();
                cancellationToken.ThrowIfCancellationRequested();

                if (TryDequeueIncomingStream(bidirectional, out ulong streamId))
                {
                    return new NativeStreamBackend(_native, _events, streamId);
                }

                using (NativeEventCoordinator.NativeEventWaiter waiter = bidirectional
                    ? _events.RegisterBidirectionalStreamWaiter(_sessionId, cancellationToken)
                    : _events.RegisterUnidirectionalStreamWaiter(_sessionId, cancellationToken))
                {
                    if (TryDequeueIncomingStream(bidirectional, out streamId))
                    {
                        return new NativeStreamBackend(_native, _events, streamId);
                    }

                    await waiter.Task.ConfigureAwait(false);
                }
            }
        }

        private bool TryDequeueIncomingStream(bool bidirectional, out ulong streamId)
        {
            return bidirectional
                ? _events.TryDequeueBidirectionalStream(_sessionId, out streamId)
                : _events.TryDequeueUnidirectionalStream(_sessionId, out streamId);
        }

        private static WebTransportDatagram CreateDatagram(byte[] buffer, int bytesRead)
        {
            byte[] payload = new byte[bytesRead];
            Array.Copy(buffer, payload, bytesRead);
            return new WebTransportDatagram(payload);
        }

        private static void ValidateCloseInfo(WebTransportCloseInfo closeInfo)
        {
            if (closeInfo.Reason.Length > MaxCloseReasonBytes)
            {
                throw new ArgumentOutOfRangeException(nameof(closeInfo), "Close reason is too large.");
            }

            for (int i = 0; i < closeInfo.Reason.Length; i++)
            {
                char c = closeInfo.Reason[i];
                if (c == '\r' || c == '\n' || c == '\0')
                {
                    throw new ArgumentException("Close reason must not contain CR, LF, or NUL characters.", nameof(closeInfo));
                }
            }
        }

        private static void InvokeNative(Action action)
        {
            try
            {
                action();
            }
            catch (WebTransportNativeException ex)
            {
                throw NativeErrorMapper.Map(ex);
            }
        }

        private static T InvokeNative<T>(Func<T> action)
        {
            try
            {
                return action();
            }
            catch (WebTransportNativeException ex)
            {
                throw NativeErrorMapper.Map(ex);
            }
        }

        private bool TryReceiveDatagram(byte[] buffer, out int bytesRead)
        {
            try
            {
                return _native.TryReceiveDatagram(_sessionId, buffer, out bytesRead);
            }
            catch (WebTransportNativeException ex)
            {
                throw NativeErrorMapper.Map(ex);
            }
        }
    }
}
