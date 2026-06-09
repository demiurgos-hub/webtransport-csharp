using System;
using System.Threading;
using System.Threading.Tasks;

namespace WebTransport
{
    /// <summary>
    /// Represents a reliable WebTransport stream.
    /// </summary>
    public sealed class WebTransportStream : IAsyncDisposable
    {
        private readonly IWebTransportStreamBackend _backend;
        private bool _disposed;

        internal WebTransportStream(IWebTransportStreamBackend backend)
        {
            _backend = backend ?? throw new ArgumentNullException(nameof(backend));
        }

        public ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default)
        {
            ThrowIfDisposed();
            return _backend.ReadAsync(buffer, cancellationToken);
        }

        public ValueTask WriteAsync(ReadOnlyMemory<byte> payload, CancellationToken cancellationToken = default)
        {
            ThrowIfDisposed();
            return _backend.WriteAsync(payload, cancellationToken);
        }

        public ValueTask FinishAsync(CancellationToken cancellationToken = default)
        {
            ThrowIfDisposed();
            return _backend.FinishAsync(cancellationToken);
        }

        public void Reset(ulong errorCode)
        {
            ThrowIfDisposed();
            _backend.Reset(errorCode);
        }

        public async ValueTask DisposeAsync()
        {
            if (_disposed)
            {
                return;
            }

            await _backend.DisposeAsync().ConfigureAwait(false);
            _disposed = true;
        }

        private void ThrowIfDisposed()
        {
            if (_disposed)
            {
                throw new ObjectDisposedException(nameof(WebTransportStream));
            }
        }
    }
}
