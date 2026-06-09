using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using WebTransport.Native;
using Xunit;

namespace WebTransport.Tests;

public sealed class NativeSessionBackendTests
{
    private const ulong SessionId = 42;

    [Fact]
    public async Task ReceiveDatagramAsyncTreatsStaleDatagramEventAsAvailabilityHint()
    {
        var native = new FakeNativeOperations();
        var eventSource = new FakeEventSource();
        await using var coordinator = new NativeEventCoordinator(eventSource, TimeSpan.FromMilliseconds(1));
        var backend = new NativeSessionBackend(native, coordinator, SessionId);

        byte[] firstPayload = { 1, 2, 3 };
        native.EnqueueDatagram(firstPayload);

        WebTransportDatagram first = await backend.ReceiveDatagramAsync(CancellationToken.None);
        Assert.Equal(firstPayload, first.Payload);

        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(1));
        Task<WebTransportDatagram> secondReceive = backend.ReceiveDatagramAsync(timeout.Token).AsTask();

        eventSource.Enqueue(DatagramReceivedEvent());
        await eventSource.WaitForPollCountAsync(1, TimeSpan.FromSeconds(1));
        await Task.Yield();
        Assert.False(secondReceive.IsCompleted, "A stale datagram event must not complete or fault the receive.");

        byte[] secondPayload = { 4, 5, 6 };
        native.EnqueueDatagram(secondPayload);
        eventSource.Enqueue(DatagramReceivedEvent());

        WebTransportDatagram second = await secondReceive.WaitAsync(TimeSpan.FromSeconds(1));
        Assert.Equal(secondPayload, second.Payload);
    }

    private static WebTransportNativeEvent DatagramReceivedEvent()
    {
        return new WebTransportNativeEvent
        {
            Type = WebTransportNativeEventType.DatagramReceived,
            Status = WebTransportNativeStatus.Ok,
            SessionId = SessionId
        };
    }

    private sealed class FakeNativeOperations : INativeWebTransportOperations
    {
        private readonly Queue<byte[]> _datagrams = new Queue<byte[]>();

        public void EnqueueDatagram(byte[] payload)
        {
            lock (_datagrams)
            {
                _datagrams.Enqueue(payload);
            }
        }

        public ulong OpenBidirectionalStream(ulong sessionId)
        {
            throw new NotSupportedException();
        }

        public ulong OpenUnidirectionalStream(ulong sessionId)
        {
            throw new NotSupportedException();
        }

        public ulong SendDatagram(ulong sessionId, ReadOnlySpan<byte> payload)
        {
            throw new NotSupportedException();
        }

        public bool TryReceiveDatagram(ulong sessionId, Span<byte> buffer, out int bytesRead)
        {
            lock (_datagrams)
            {
                if (_datagrams.Count == 0)
                {
                    bytesRead = 0;
                    return false;
                }

                byte[] payload = _datagrams.Dequeue();
                payload.CopyTo(buffer);
                bytesRead = payload.Length;
                return true;
            }
        }

        public int ReadStream(ulong streamId, Span<byte> buffer)
        {
            throw new NotSupportedException();
        }

        public ulong WriteStream(ulong streamId, ReadOnlySpan<byte> payload, bool endStream)
        {
            throw new NotSupportedException();
        }

        public ulong FinishStream(ulong streamId)
        {
            throw new NotSupportedException();
        }

        public void ResetStream(ulong streamId, ulong errorCode)
        {
            throw new NotSupportedException();
        }

        public void CloseSession(ulong sessionId, ulong errorCode, string reason)
        {
            throw new NotSupportedException();
        }

        public void Release(ulong handle)
        {
        }
    }

    private sealed class FakeEventSource : INativeEventSource
    {
        private readonly Queue<WebTransportNativeEvent> _events = new Queue<WebTransportNativeEvent>();
        private readonly List<TaskCompletionSource<bool>> _pollWaiters = new List<TaskCompletionSource<bool>>();
        private int _pollCount;

        public void Enqueue(WebTransportNativeEvent nativeEvent)
        {
            lock (_events)
            {
                _events.Enqueue(nativeEvent);
            }
        }

        public bool TryPollEvent(out WebTransportNativeEvent nativeEvent)
        {
            List<TaskCompletionSource<bool>> completedWaiters = new List<TaskCompletionSource<bool>>();
            lock (_events)
            {
                if (_events.Count == 0)
                {
                    nativeEvent = default;
                    return false;
                }

                nativeEvent = _events.Dequeue();
                _pollCount++;
                for (int i = _pollWaiters.Count - 1; i >= 0; i--)
                {
                    completedWaiters.Add(_pollWaiters[i]);
                    _pollWaiters.RemoveAt(i);
                }
            }

            foreach (TaskCompletionSource<bool> waiter in completedWaiters)
            {
                waiter.TrySetResult(true);
            }

            return true;
        }

        public Task WaitForPollCountAsync(int pollCount, TimeSpan timeout)
        {
            TaskCompletionSource<bool> waiter;
            lock (_events)
            {
                if (_pollCount >= pollCount)
                {
                    return Task.CompletedTask;
                }

                waiter = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
                _pollWaiters.Add(waiter);
            }

            return waiter.Task.WaitAsync(timeout);
        }
    }
}
