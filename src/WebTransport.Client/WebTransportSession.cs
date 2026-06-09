using System;
using System.Threading;
using System.Threading.Tasks;

namespace WebTransport
{
    /// <summary>
    /// Represents an established WebTransport session.
    /// </summary>
    public sealed class WebTransportSession : IAsyncDisposable
    {
        private readonly IWebTransportSessionBackend _backend;
        private bool _disposed;

        internal WebTransportSession(IWebTransportSessionBackend backend)
        {
            _backend = backend ?? throw new ArgumentNullException(nameof(backend));
        }

        public async ValueTask<WebTransportStream> OpenBidirectionalStreamAsync(CancellationToken cancellationToken = default)
        {
            ThrowIfDisposed();
            IWebTransportStreamBackend streamBackend = await _backend.OpenBidirectionalStreamAsync(cancellationToken).ConfigureAwait(false);
            return new WebTransportStream(streamBackend);
        }

        public async ValueTask<WebTransportStream> OpenUnidirectionalStreamAsync(CancellationToken cancellationToken = default)
        {
            ThrowIfDisposed();
            IWebTransportStreamBackend streamBackend = await _backend.OpenUnidirectionalStreamAsync(cancellationToken).ConfigureAwait(false);
            return new WebTransportStream(streamBackend);
        }

        public async ValueTask<WebTransportStream> AcceptBidirectionalStreamAsync(CancellationToken cancellationToken = default)
        {
            ThrowIfDisposed();
            IWebTransportStreamBackend streamBackend = await _backend.AcceptBidirectionalStreamAsync(cancellationToken).ConfigureAwait(false);
            return new WebTransportStream(streamBackend);
        }

        public async ValueTask<WebTransportStream> AcceptUnidirectionalStreamAsync(CancellationToken cancellationToken = default)
        {
            ThrowIfDisposed();
            IWebTransportStreamBackend streamBackend = await _backend.AcceptUnidirectionalStreamAsync(cancellationToken).ConfigureAwait(false);
            return new WebTransportStream(streamBackend);
        }

        public ValueTask SendDatagramAsync(ReadOnlyMemory<byte> payload, CancellationToken cancellationToken = default)
        {
            ThrowIfDisposed();
            return _backend.SendDatagramAsync(payload, cancellationToken);
        }

        public ValueTask<WebTransportDatagram> ReceiveDatagramAsync(CancellationToken cancellationToken = default)
        {
            ThrowIfDisposed();
            return _backend.ReceiveDatagramAsync(cancellationToken);
        }

        public ValueTask CloseAsync(WebTransportCloseInfo closeInfo, CancellationToken cancellationToken = default)
        {
            ThrowIfDisposed();
            return _backend.CloseAsync(closeInfo, cancellationToken);
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
                throw new ObjectDisposedException(nameof(WebTransportSession));
            }
        }
    }
}
