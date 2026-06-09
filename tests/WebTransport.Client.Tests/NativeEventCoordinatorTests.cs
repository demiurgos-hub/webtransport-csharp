using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using WebTransport.Native;
using Xunit;

namespace WebTransport.Tests;

public sealed class NativeEventCoordinatorTests
{
    [Fact]
    public async Task WaitForSessionConnectedAsyncCompletesAfterSessionConnectedEvent()
    {
        var eventSource = new FakeEventSource();
        await using var coordinator = new NativeEventCoordinator(eventSource, TimeSpan.FromMilliseconds(1));

        ValueTask waitTask = coordinator.WaitForSessionConnectedAsync(
            sessionId: 42,
            timeout: TimeSpan.FromSeconds(1),
            cancellationToken: CancellationToken.None);

        await Task.Yield();
        Assert.False(waitTask.IsCompleted);

        eventSource.Enqueue(new WebTransportNativeEvent
        {
            Type = WebTransportNativeEventType.SessionConnected,
            Status = WebTransportNativeStatus.Ok,
            SessionId = 42
        });

        await waitTask.AsTask().WaitAsync(TimeSpan.FromSeconds(1));
    }

    [Fact]
    public async Task WaitForSessionConnectedAsyncTimesOutWhenAcceptedEventDoesNotArrive()
    {
        var eventSource = new FakeEventSource();
        await using var coordinator = new NativeEventCoordinator(eventSource, TimeSpan.FromMilliseconds(1));

        WebTransportException ex = await Assert.ThrowsAsync<WebTransportException>(async () =>
            await coordinator.WaitForSessionConnectedAsync(
                sessionId: 42,
                timeout: TimeSpan.FromMilliseconds(10),
                cancellationToken: CancellationToken.None));

        Assert.Equal(WebTransportErrorCode.TransportError, ex.ErrorCode);
    }

    [Fact]
    public async Task DatagramWaiterCompletesAfterDatagramEventForSession()
    {
        var eventSource = new FakeEventSource();
        await using var coordinator = new NativeEventCoordinator(eventSource, TimeSpan.FromMilliseconds(1));

        using NativeEventCoordinator.NativeEventWaiter waiter = coordinator.RegisterDatagramWaiter(42, CancellationToken.None);

        eventSource.Enqueue(new WebTransportNativeEvent
        {
            Type = WebTransportNativeEventType.DatagramReceived,
            Status = WebTransportNativeStatus.Ok,
            SessionId = 42
        });

        await waiter.Task.WaitAsync(TimeSpan.FromSeconds(1));
    }

    [Fact]
    public async Task StreamWaiterCompletesAfterStreamDataEventForStream()
    {
        var eventSource = new FakeEventSource();
        await using var coordinator = new NativeEventCoordinator(eventSource, TimeSpan.FromMilliseconds(1));

        using NativeEventCoordinator.NativeEventWaiter waiter = coordinator.RegisterStreamDataWaiter(7, CancellationToken.None);

        eventSource.Enqueue(new WebTransportNativeEvent
        {
            Type = WebTransportNativeEventType.StreamDataReceived,
            Status = WebTransportNativeStatus.Ok,
            StreamId = 7
        });

        await waiter.Task.WaitAsync(TimeSpan.FromSeconds(1));
    }

    [Fact]
    public async Task BidirectionalStreamOpenedEventQueuesIncomingStreamForSession()
    {
        var eventSource = new FakeEventSource();
        await using var coordinator = new NativeEventCoordinator(eventSource, TimeSpan.FromMilliseconds(1));

        using NativeEventCoordinator.NativeEventWaiter waiter = coordinator.RegisterBidirectionalStreamWaiter(42, CancellationToken.None);

        eventSource.Enqueue(new WebTransportNativeEvent
        {
            Type = WebTransportNativeEventType.BidirectionalStreamOpened,
            Status = WebTransportNativeStatus.Ok,
            SessionId = 42,
            StreamId = 7
        });

        await waiter.Task.WaitAsync(TimeSpan.FromSeconds(1));
        Assert.True(coordinator.TryDequeueBidirectionalStream(42, out ulong streamId));
        Assert.Equal((ulong)7, streamId);
        Assert.False(coordinator.TryDequeueBidirectionalStream(42, out _));
    }

    [Fact]
    public async Task IncomingStreamQueueIsBoundedAndReleasesOldestStream()
    {
        const int cap = 1024;
        var eventSource = new FakeEventSource();
        var releasedStreams = new System.Collections.Concurrent.ConcurrentQueue<ulong>();
        await using var coordinator = new NativeEventCoordinator(
            eventSource,
            TimeSpan.FromMilliseconds(1),
            releasedStreams.Enqueue);

        for (ulong streamId = 1; streamId <= cap + 1; streamId++)
        {
            eventSource.Enqueue(new WebTransportNativeEvent
            {
                Type = WebTransportNativeEventType.BidirectionalStreamOpened,
                Status = WebTransportNativeStatus.Ok,
                SessionId = 42,
                StreamId = streamId
            });
        }

        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
        while (releasedStreams.IsEmpty && !timeout.IsCancellationRequested)
        {
            await Task.Delay(5);
        }

        Assert.True(releasedStreams.TryDequeue(out ulong releasedStreamId));
        Assert.Equal((ulong)1, releasedStreamId);
    }

    [Fact]
    public async Task DisposeAsyncCompletesWhenEventSourceAlwaysHasEvents()
    {
        var eventSource = new InfiniteEventSource();
        var coordinator = new NativeEventCoordinator(eventSource, TimeSpan.FromMilliseconds(1));

        await coordinator.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(1));
    }

    [Fact]
    public async Task DisposeAsyncFaultsPendingWaiters()
    {
        var eventSource = new FakeEventSource();
        var coordinator = new NativeEventCoordinator(eventSource, TimeSpan.FromMilliseconds(1));
        using NativeEventCoordinator.NativeEventWaiter datagramWaiter = coordinator.RegisterDatagramWaiter(42, CancellationToken.None);
        using NativeEventCoordinator.NativeEventWaiter streamWaiter = coordinator.RegisterStreamDataWaiter(7, CancellationToken.None);

        await coordinator.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(1));

        await Assert.ThrowsAsync<ObjectDisposedException>(async () => await datagramWaiter.Task);
        await Assert.ThrowsAsync<ObjectDisposedException>(async () => await streamWaiter.Task);
    }

    [Fact]
    public async Task SessionClosedOnlyFaultsWaitersForThatSession()
    {
        var eventSource = new FakeEventSource();
        await using var coordinator = new NativeEventCoordinator(eventSource, TimeSpan.FromMilliseconds(1));

        using NativeEventCoordinator.NativeEventWaiter closedSessionWaiter = coordinator.RegisterDatagramWaiter(42, CancellationToken.None);
        using NativeEventCoordinator.NativeEventWaiter otherSessionWaiter = coordinator.RegisterDatagramWaiter(43, CancellationToken.None);

        eventSource.Enqueue(new WebTransportNativeEvent
        {
            Type = WebTransportNativeEventType.SessionClosed,
            Status = WebTransportNativeStatus.TransportError,
            SessionId = 42,
            ErrorCode = 123
        });

        WebTransportException ex = await Assert.ThrowsAsync<WebTransportException>(async () =>
            await closedSessionWaiter.Task.WaitAsync(TimeSpan.FromSeconds(1)));
        Assert.Equal((ulong)123, ex.NativeErrorCode);
        Assert.False(otherSessionWaiter.Task.IsCompleted);

        eventSource.Enqueue(new WebTransportNativeEvent
        {
            Type = WebTransportNativeEventType.DatagramReceived,
            Status = WebTransportNativeStatus.Ok,
            SessionId = 43
        });

        await otherSessionWaiter.Task.WaitAsync(TimeSpan.FromSeconds(1));
    }

    private sealed class FakeEventSource : INativeEventSource
    {
        private readonly Queue<WebTransportNativeEvent> _events = new Queue<WebTransportNativeEvent>();

        public void Enqueue(WebTransportNativeEvent nativeEvent)
        {
            lock (_events)
            {
                _events.Enqueue(nativeEvent);
            }
        }

        public bool TryPollEvent(out WebTransportNativeEvent nativeEvent)
        {
            lock (_events)
            {
                if (_events.Count == 0)
                {
                    nativeEvent = default;
                    return false;
                }

                nativeEvent = _events.Dequeue();
                return true;
            }
        }
    }

    private sealed class InfiniteEventSource : INativeEventSource
    {
        public bool TryPollEvent(out WebTransportNativeEvent nativeEvent)
        {
            nativeEvent = new WebTransportNativeEvent
            {
                Type = WebTransportNativeEventType.DatagramReceived,
                Status = WebTransportNativeStatus.Ok,
                SessionId = 42
            };
            return true;
        }
    }
}
