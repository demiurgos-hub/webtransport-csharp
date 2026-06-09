using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using WebTransport.Native;
using Xunit;

namespace WebTransport.Tests;

public sealed class WebTransportClientTests
{
    [Fact]
    public async Task ConnectAsyncRejectsNonHttpsUri()
    {
        await using var client = new WebTransportClient(new WebTransportClientOptions(), new FakeBackend());

        await Assert.ThrowsAsync<ArgumentException>(async () =>
            await client.ConnectAsync(new Uri("http://example.com/transport")));
    }

    [Fact]
    public async Task ConnectAsyncRejectsInvalidOptions()
    {
        var options = new WebTransportClientOptions
        {
            ConnectTimeout = TimeSpan.Zero
        };

        await using var client = new WebTransportClient(options, new FakeBackend());

        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(async () =>
            await client.ConnectAsync(new Uri("https://example.com/transport")));
    }

    [Fact]
    public async Task ConnectAsyncRejectsPseudoHeaderOverride()
    {
        var options = new WebTransportClientOptions();
        options.Headers[":authority"] = "example.com";

        await using var client = new WebTransportClient(options, new FakeBackend());

        await Assert.ThrowsAsync<ArgumentException>(async () =>
            await client.ConnectAsync(new Uri("https://example.com/transport")));
    }

    [Fact]
    public async Task SessionCanOpenStreamAndWritePayload()
    {
        var backend = new FakeBackend();
        await using var client = new WebTransportClient(new WebTransportClientOptions(), backend);

        await using WebTransportSession session = await client.ConnectAsync(new Uri("https://example.com/transport"));
        await using WebTransportStream stream = await session.OpenBidirectionalStreamAsync();
        byte[] payload = { 1, 2, 3 };

        await stream.WriteAsync(payload);

        Assert.Equal(payload, backend.Session.Stream.WrittenPayloads[0]);
    }

    [Fact]
    public async Task SessionCanSendAndReceiveDatagram()
    {
        var backend = new FakeBackend();
        await using var client = new WebTransportClient(new WebTransportClientOptions(), backend);

        await using WebTransportSession session = await client.ConnectAsync(new Uri("https://example.com/transport"));
        byte[] outbound = { 4, 5, 6 };
        byte[] inbound = { 7, 8, 9 };
        backend.Session.InboundDatagrams.Enqueue(new WebTransportDatagram(inbound));

        await session.SendDatagramAsync(outbound);
        WebTransportDatagram received = await session.ReceiveDatagramAsync();

        Assert.Equal(outbound, backend.Session.SentDatagrams[0]);
        Assert.Equal(inbound, received.Payload);
    }

    [Fact]
    public async Task DisposeAsyncIsConcurrentAndIdempotent()
    {
        var backend = new FakeBackend(blockDispose: true);
        await using var client = new WebTransportClient(new WebTransportClientOptions(), backend);

        ValueTask firstDispose = client.DisposeAsync();
        ValueTask secondDispose = client.DisposeAsync();

        backend.CompleteDispose();

        await firstDispose.AsTask().WaitAsync(TimeSpan.FromSeconds(1));
        await secondDispose.AsTask().WaitAsync(TimeSpan.FromSeconds(1));

        Assert.Equal(1, backend.DisposeCallCount);
        await Assert.ThrowsAsync<ObjectDisposedException>(async () =>
            await client.ConnectAsync(new Uri("https://example.com/transport")));
    }

    [Fact]
    public async Task DisposeAsyncCompletesWhenBackendDisposeDoesNotComplete()
    {
        var backend = new FakeBackend(blockDispose: true);
        var client = new WebTransportClient(new WebTransportClientOptions(), backend);

        await client.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(2));

        Assert.Equal(1, backend.DisposeCallCount);
        await Assert.ThrowsAsync<ObjectDisposedException>(async () =>
            await client.ConnectAsync(new Uri("https://example.com/transport")));
    }

    [Fact]
    public void NativeErrorMapperMapsInvalidStateToWebTransportException()
    {
        WebTransportException ex = NativeErrorMapper.Map(new WebTransportNativeException(WebTransportNativeStatus.InvalidState));

        Assert.Equal(WebTransportErrorCode.InvalidState, ex.ErrorCode);
        Assert.IsType<WebTransportNativeException>(ex.InnerException);
    }

    private sealed class FakeBackend : IWebTransportBackend
    {
        private readonly TaskCompletionSource<bool> _disposeCompletion = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        private int _disposeCallCount;

        public FakeBackend(bool blockDispose = false)
        {
            if (!blockDispose)
            {
                _disposeCompletion.SetResult(true);
            }
        }

        public FakeSessionBackend Session { get; } = new FakeSessionBackend();

        public int DisposeCallCount => _disposeCallCount;

        public ValueTask<IWebTransportSessionBackend> ConnectAsync(Uri uri, WebTransportClientOptions options, CancellationToken cancellationToken)
        {
            return new ValueTask<IWebTransportSessionBackend>(Session);
        }

        public void CompleteDispose()
        {
            _disposeCompletion.TrySetResult(true);
        }

        public async ValueTask DisposeAsync()
        {
            Interlocked.Increment(ref _disposeCallCount);
            await _disposeCompletion.Task.ConfigureAwait(false);
        }
    }

    private sealed class FakeSessionBackend : IWebTransportSessionBackend
    {
        public FakeStreamBackend Stream { get; } = new FakeStreamBackend();

        public List<byte[]> SentDatagrams { get; } = new List<byte[]>();

        public Queue<WebTransportDatagram> InboundDatagrams { get; } = new Queue<WebTransportDatagram>();

        public ValueTask<IWebTransportStreamBackend> OpenBidirectionalStreamAsync(CancellationToken cancellationToken)
        {
            return new ValueTask<IWebTransportStreamBackend>(Stream);
        }

        public ValueTask<IWebTransportStreamBackend> OpenUnidirectionalStreamAsync(CancellationToken cancellationToken)
        {
            return new ValueTask<IWebTransportStreamBackend>(Stream);
        }

        public ValueTask<IWebTransportStreamBackend> AcceptBidirectionalStreamAsync(CancellationToken cancellationToken)
        {
            return new ValueTask<IWebTransportStreamBackend>(Stream);
        }

        public ValueTask<IWebTransportStreamBackend> AcceptUnidirectionalStreamAsync(CancellationToken cancellationToken)
        {
            return new ValueTask<IWebTransportStreamBackend>(Stream);
        }

        public ValueTask SendDatagramAsync(ReadOnlyMemory<byte> payload, CancellationToken cancellationToken)
        {
            SentDatagrams.Add(payload.ToArray());
            return default;
        }

        public ValueTask<WebTransportDatagram> ReceiveDatagramAsync(CancellationToken cancellationToken)
        {
            return new ValueTask<WebTransportDatagram>(InboundDatagrams.Dequeue());
        }

        public ValueTask CloseAsync(WebTransportCloseInfo closeInfo, CancellationToken cancellationToken)
        {
            return default;
        }

        public ValueTask DisposeAsync()
        {
            return default;
        }
    }

    private sealed class FakeStreamBackend : IWebTransportStreamBackend
    {
        public List<byte[]> WrittenPayloads { get; } = new List<byte[]>();

        public ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken)
        {
            return new ValueTask<int>(0);
        }

        public ValueTask WriteAsync(ReadOnlyMemory<byte> payload, CancellationToken cancellationToken)
        {
            WrittenPayloads.Add(payload.ToArray());
            return default;
        }

        public ValueTask FinishAsync(CancellationToken cancellationToken)
        {
            return default;
        }

        public void Reset(ulong errorCode)
        {
        }

        public ValueTask DisposeAsync()
        {
            return default;
        }
    }
}
