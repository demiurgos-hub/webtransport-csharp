using System;
using System.Threading;
using System.Threading.Tasks;
using WebTransport.Native;

namespace WebTransport
{
    internal sealed class NativeStreamBackend : IWebTransportStreamBackend
    {
        private readonly INativeWebTransportOperations _native;
        private readonly NativeEventCoordinator _events;
        private readonly ulong _streamId;
        private bool _disposed;

        public NativeStreamBackend(NativeWebTransportBackend native, NativeEventCoordinator events, ulong streamId)
            : this(new NativeWebTransportOperations(native), events, streamId)
        {
        }

        internal NativeStreamBackend(INativeWebTransportOperations native, NativeEventCoordinator events, ulong streamId)
        {
            _native = native ?? throw new ArgumentNullException(nameof(native));
            _events = events ?? throw new ArgumentNullException(nameof(events));
            _streamId = streamId;
        }

        public async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken)
        {
            ThrowIfDisposed();
            cancellationToken.ThrowIfCancellationRequested();

            if (buffer.Length == 0)
            {
                return 0;
            }

            int bytesRead = InvokeNative(() => _native.ReadStream(_streamId, buffer.Span));
            if (bytesRead != 0 || _events.IsStreamClosed(_streamId))
            {
                return bytesRead;
            }

            using (NativeEventCoordinator.NativeEventWaiter waiter = _events.RegisterStreamDataWaiter(_streamId, cancellationToken))
            {
                bytesRead = InvokeNative(() => _native.ReadStream(_streamId, buffer.Span));
                if (bytesRead != 0 || _events.IsStreamClosed(_streamId))
                {
                    return bytesRead;
                }

                await waiter.Task.ConfigureAwait(false);
            }

            cancellationToken.ThrowIfCancellationRequested();
            return InvokeNative(() => _native.ReadStream(_streamId, buffer.Span));
        }

        public ValueTask WriteAsync(ReadOnlyMemory<byte> payload, CancellationToken cancellationToken)
        {
            ThrowIfDisposed();
            cancellationToken.ThrowIfCancellationRequested();
            InvokeNative(() => _native.WriteStream(_streamId, payload.Span, endStream: false));
            return default;
        }

        public ValueTask FinishAsync(CancellationToken cancellationToken)
        {
            ThrowIfDisposed();
            cancellationToken.ThrowIfCancellationRequested();
            InvokeNative(() => _native.FinishStream(_streamId));
            return default;
        }

        public void Reset(ulong errorCode)
        {
            ThrowIfDisposed();
            InvokeNative(() => _native.ResetStream(_streamId, errorCode));
        }

        public ValueTask DisposeAsync()
        {
            if (_disposed)
            {
                return default;
            }

            InvokeNative(() => _native.Release(_streamId));
            _disposed = true;
            return default;
        }

        private void ThrowIfDisposed()
        {
            if (_disposed)
            {
                throw new ObjectDisposedException(nameof(NativeStreamBackend));
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
    }
}
