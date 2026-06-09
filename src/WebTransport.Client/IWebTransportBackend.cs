using System;
using System.Threading;
using System.Threading.Tasks;

namespace WebTransport
{
    /// <summary>
    /// Backend abstraction used by the public client API.
    /// </summary>
    public interface IWebTransportBackend : IAsyncDisposable
    {
        ValueTask<IWebTransportSessionBackend> ConnectAsync(
            Uri uri,
            WebTransportClientOptions options,
            CancellationToken cancellationToken);
    }

    public interface IWebTransportSessionBackend : IAsyncDisposable
    {
        ValueTask<IWebTransportStreamBackend> OpenBidirectionalStreamAsync(CancellationToken cancellationToken);

        ValueTask<IWebTransportStreamBackend> OpenUnidirectionalStreamAsync(CancellationToken cancellationToken);

        ValueTask<IWebTransportStreamBackend> AcceptBidirectionalStreamAsync(CancellationToken cancellationToken);

        ValueTask<IWebTransportStreamBackend> AcceptUnidirectionalStreamAsync(CancellationToken cancellationToken);

        ValueTask SendDatagramAsync(ReadOnlyMemory<byte> payload, CancellationToken cancellationToken);

        ValueTask<WebTransportDatagram> ReceiveDatagramAsync(CancellationToken cancellationToken);

        ValueTask CloseAsync(WebTransportCloseInfo closeInfo, CancellationToken cancellationToken);
    }

    public interface IWebTransportStreamBackend : IAsyncDisposable
    {
        ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken);

        ValueTask WriteAsync(ReadOnlyMemory<byte> payload, CancellationToken cancellationToken);

        ValueTask FinishAsync(CancellationToken cancellationToken);

        void Reset(ulong errorCode);
    }
}
