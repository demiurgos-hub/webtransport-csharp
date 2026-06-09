using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using WebTransport.Native;

namespace WebTransport
{
    internal interface INativeEventSource
    {
        bool TryPollEvent(out WebTransportNativeEvent nativeEvent);
    }

    internal sealed class NativeEventCoordinator : IAsyncDisposable
    {
        private static readonly TimeSpan DefaultPollDelay = TimeSpan.FromMilliseconds(10);
        private const int MaxQueuedIncomingStreamsPerSession = 1024;

        private readonly INativeEventSource _eventSource;
        private readonly Action<ulong>? _releaseStream;
        private readonly TimeSpan _pollDelay;
        private readonly object _gate = new object();
        private readonly CancellationTokenSource _disposeCts = new CancellationTokenSource();
        private readonly Task _pumpTask;
        private readonly Dictionary<ulong, List<NativeEventWaiter>> _sessionConnectedWaiters = new Dictionary<ulong, List<NativeEventWaiter>>();
        private readonly Dictionary<ulong, List<NativeEventWaiter>> _datagramWaiters = new Dictionary<ulong, List<NativeEventWaiter>>();
        private readonly Dictionary<ulong, List<NativeEventWaiter>> _streamDataWaiters = new Dictionary<ulong, List<NativeEventWaiter>>();
        private readonly Dictionary<ulong, List<NativeEventWaiter>> _bidiStreamWaiters = new Dictionary<ulong, List<NativeEventWaiter>>();
        private readonly Dictionary<ulong, List<NativeEventWaiter>> _uniStreamWaiters = new Dictionary<ulong, List<NativeEventWaiter>>();
        private readonly Dictionary<ulong, Queue<ulong>> _bidiStreams = new Dictionary<ulong, Queue<ulong>>();
        private readonly Dictionary<ulong, Queue<ulong>> _uniStreams = new Dictionary<ulong, Queue<ulong>>();
        private readonly HashSet<ulong> _connectedSessions = new HashSet<ulong>();
        private readonly HashSet<ulong> _closedSessions = new HashSet<ulong>();
        private readonly HashSet<ulong> _closedStreams = new HashSet<ulong>();
        private Task? _disposeTask;
        private Exception? _terminalException;
        private bool _disposed;

        public NativeEventCoordinator(INativeEventSource eventSource, Action<ulong>? releaseStream = null)
            : this(eventSource, DefaultPollDelay, releaseStream)
        {
        }

        internal NativeEventCoordinator(INativeEventSource eventSource, TimeSpan pollDelay, Action<ulong>? releaseStream = null)
        {
            _eventSource = eventSource ?? throw new ArgumentNullException(nameof(eventSource));
            _releaseStream = releaseStream;
            _pollDelay = pollDelay <= TimeSpan.Zero ? DefaultPollDelay : pollDelay;
            _pumpTask = Task.Run(PumpAsync);
        }

        public async ValueTask DisposeAsync()
        {
            Task disposeTask;
            lock (_gate)
            {
                if (_disposeTask == null)
                {
                    _disposed = true;
                    _disposeCts.Cancel();
                    _disposeTask = DisposeCoreAsync();
                }

                disposeTask = _disposeTask;
            }

            CompleteAll(new ObjectDisposedException(nameof(NativeEventCoordinator)));
            await disposeTask.ConfigureAwait(false);
        }

        private async Task DisposeCoreAsync()
        {
            try
            {
                await _pumpTask.ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
            }

            _disposeCts.Dispose();
        }

        public ValueTask WaitForSessionConnectedAsync(ulong sessionId, TimeSpan timeout, CancellationToken cancellationToken)
        {
            NativeEventWaiter waiter;
            lock (_gate)
            {
                ThrowIfDisposed();
                ThrowIfTerminal();

                if (_connectedSessions.Contains(sessionId))
                {
                    return default;
                }

                if (_closedSessions.Contains(sessionId))
                {
                    throw new WebTransportException(WebTransportErrorCode.TransportError, "The WebTransport session closed before it was accepted.");
                }

                waiter = AddWaiter(_sessionConnectedWaiters, sessionId, cancellationToken);
            }

            return new ValueTask(WaitWithTimeoutAsync(waiter, timeout, cancellationToken));
        }

        public NativeEventWaiter RegisterDatagramWaiter(ulong sessionId, CancellationToken cancellationToken)
        {
            lock (_gate)
            {
                ThrowIfDisposed();
                ThrowIfTerminal();

                if (_closedSessions.Contains(sessionId))
                {
                    throw new WebTransportException(WebTransportErrorCode.TransportError, "The WebTransport session is closed.");
                }

                return AddWaiter(_datagramWaiters, sessionId, cancellationToken);
            }
        }

        public NativeEventWaiter RegisterBidirectionalStreamWaiter(ulong sessionId, CancellationToken cancellationToken)
        {
            lock (_gate)
            {
                ThrowIfDisposed();
                ThrowIfTerminal();

                if (_closedSessions.Contains(sessionId))
                {
                    throw new WebTransportException(WebTransportErrorCode.TransportError, "The WebTransport session is closed.");
                }

                if (HasQueuedStream(_bidiStreams, sessionId))
                {
                    return NativeEventWaiter.Completed;
                }

                return AddWaiter(_bidiStreamWaiters, sessionId, cancellationToken);
            }
        }

        public NativeEventWaiter RegisterUnidirectionalStreamWaiter(ulong sessionId, CancellationToken cancellationToken)
        {
            lock (_gate)
            {
                ThrowIfDisposed();
                ThrowIfTerminal();

                if (_closedSessions.Contains(sessionId))
                {
                    throw new WebTransportException(WebTransportErrorCode.TransportError, "The WebTransport session is closed.");
                }

                if (HasQueuedStream(_uniStreams, sessionId))
                {
                    return NativeEventWaiter.Completed;
                }

                return AddWaiter(_uniStreamWaiters, sessionId, cancellationToken);
            }
        }

        public bool TryDequeueBidirectionalStream(ulong sessionId, out ulong streamId)
        {
            lock (_gate)
            {
                return TryDequeueStream(_bidiStreams, sessionId, out streamId);
            }
        }

        public bool TryDequeueUnidirectionalStream(ulong sessionId, out ulong streamId)
        {
            lock (_gate)
            {
                return TryDequeueStream(_uniStreams, sessionId, out streamId);
            }
        }

        public NativeEventWaiter RegisterStreamDataWaiter(ulong streamId, CancellationToken cancellationToken)
        {
            lock (_gate)
            {
                ThrowIfDisposed();
                ThrowIfTerminal();

                if (_closedStreams.Contains(streamId))
                {
                    return NativeEventWaiter.Completed;
                }

                return AddWaiter(_streamDataWaiters, streamId, cancellationToken);
            }
        }

        public bool IsStreamClosed(ulong streamId)
        {
            lock (_gate)
            {
                return _closedStreams.Contains(streamId);
            }
        }

        private async Task PumpAsync()
        {
            try
            {
                while (!_disposeCts.IsCancellationRequested)
                {
                    bool processedEvent = false;
                    while (!_disposeCts.IsCancellationRequested && _eventSource.TryPollEvent(out WebTransportNativeEvent nativeEvent))
                    {
                        processedEvent = true;
                        Route(nativeEvent);
                    }

                    await Task.Delay(processedEvent ? TimeSpan.Zero : _pollDelay, _disposeCts.Token).ConfigureAwait(false);
                }
            }
            catch (OperationCanceledException) when (_disposeCts.IsCancellationRequested)
            {
            }
            catch (Exception ex)
            {
                CompleteAll(ex);
            }
        }

        private void Route(WebTransportNativeEvent nativeEvent)
        {
            switch (nativeEvent.Type)
            {
                case WebTransportNativeEventType.SessionConnected:
                    if (nativeEvent.Status == WebTransportNativeStatus.Ok)
                    {
                        CompleteSessionConnected(nativeEvent.SessionId);
                    }
                    else
                    {
                        CompleteSession(nativeEvent.SessionId, ToException(nativeEvent, "The WebTransport session was not accepted."));
                    }
                    break;

                case WebTransportNativeEventType.DatagramReceived:
                    CompleteWaiters(_datagramWaiters, nativeEvent.SessionId, null);
                    break;

                case WebTransportNativeEventType.StreamDataReceived:
                    CompleteWaiters(_streamDataWaiters, nativeEvent.StreamId, null);
                    break;

                case WebTransportNativeEventType.BidirectionalStreamOpened:
                    EnqueueOpenedStream(_bidiStreams, _bidiStreamWaiters, nativeEvent.SessionId, nativeEvent.StreamId);
                    break;

                case WebTransportNativeEventType.UnidirectionalStreamOpened:
                    EnqueueOpenedStream(_uniStreams, _uniStreamWaiters, nativeEvent.SessionId, nativeEvent.StreamId);
                    break;

                case WebTransportNativeEventType.StreamClosed:
                    lock (_gate)
                    {
                        _closedStreams.Add(nativeEvent.StreamId);
                    }

                    CompleteWaiters(_streamDataWaiters, nativeEvent.StreamId, null);
                    break;

                case WebTransportNativeEventType.SessionClosed:
                    lock (_gate)
                    {
                        _closedSessions.Add(nativeEvent.SessionId);
                    }

                    CompleteSession(nativeEvent.SessionId, ToException(nativeEvent, "The WebTransport session closed."));
                    break;

                case WebTransportNativeEventType.ClientClosed:
                case WebTransportNativeEventType.Error:
                    CompleteAll(ToException(nativeEvent, "The native WebTransport backend reported an error."));
                    break;
            }
        }

        private void CompleteSessionConnected(ulong sessionId)
        {
            lock (_gate)
            {
                _connectedSessions.Add(sessionId);
            }

            CompleteWaiters(_sessionConnectedWaiters, sessionId, null);
        }

        private void CompleteSession(ulong sessionId, Exception exception)
        {
            CompleteWaiters(_sessionConnectedWaiters, sessionId, exception);
            CompleteWaiters(_datagramWaiters, sessionId, exception);
            CompleteWaiters(_bidiStreamWaiters, sessionId, exception);
            CompleteWaiters(_uniStreamWaiters, sessionId, exception);
        }

        private void CompleteAll(Exception exception)
        {
            List<NativeEventWaiter> waiters = new List<NativeEventWaiter>();
            lock (_gate)
            {
                _terminalException = exception;
                DrainWaiters(_sessionConnectedWaiters, waiters);
                DrainWaiters(_datagramWaiters, waiters);
                DrainWaiters(_streamDataWaiters, waiters);
                DrainWaiters(_bidiStreamWaiters, waiters);
                DrainWaiters(_uniStreamWaiters, waiters);
            }

            foreach (NativeEventWaiter waiter in waiters)
            {
                waiter.TrySetException(exception);
            }
        }

        private void CompleteWaiters(Dictionary<ulong, List<NativeEventWaiter>> waitersById, ulong id, Exception? exception)
        {
            List<NativeEventWaiter>? waiters;
            lock (_gate)
            {
                if (waitersById.TryGetValue(id, out waiters))
                {
                    waitersById.Remove(id);
                }
            }

            if (waiters == null)
            {
                return;
            }

            foreach (NativeEventWaiter waiter in waiters)
            {
                if (exception == null)
                {
                    waiter.TrySetResult();
                }
                else
                {
                    waiter.TrySetException(exception);
                }
            }
        }

        private NativeEventWaiter AddWaiter(Dictionary<ulong, List<NativeEventWaiter>> waitersById, ulong id, CancellationToken cancellationToken)
        {
            var waiter = new NativeEventWaiter(this, waitersById, id, cancellationToken);
            if (!waitersById.TryGetValue(id, out List<NativeEventWaiter> waiters))
            {
                waiters = new List<NativeEventWaiter>();
                waitersById.Add(id, waiters);
            }

            waiters.Add(waiter);
            return waiter;
        }

        private void EnqueueOpenedStream(
            Dictionary<ulong, Queue<ulong>> streamsBySession,
            Dictionary<ulong, List<NativeEventWaiter>> waitersBySession,
            ulong sessionId,
            ulong streamId)
        {
            ulong droppedStreamId = 0;
            bool dropped = false;
            lock (_gate)
            {
                if (!streamsBySession.TryGetValue(sessionId, out Queue<ulong> streams))
                {
                    streams = new Queue<ulong>();
                    streamsBySession.Add(sessionId, streams);
                }

                // Bound the number of un-accepted peer-initiated streams so a peer
                // cannot grow managed memory without limit. When the cap is hit,
                // drop the oldest queued stream and release its native handle.
                if (streams.Count >= MaxQueuedIncomingStreamsPerSession)
                {
                    droppedStreamId = streams.Dequeue();
                    dropped = true;
                }

                streams.Enqueue(streamId);
            }

            if (dropped)
            {
                _releaseStream?.Invoke(droppedStreamId);
            }

            CompleteWaiters(waitersBySession, sessionId, null);
        }

        private static bool HasQueuedStream(Dictionary<ulong, Queue<ulong>> streamsBySession, ulong sessionId)
        {
            return streamsBySession.TryGetValue(sessionId, out Queue<ulong> streams) && streams.Count != 0;
        }

        private static bool TryDequeueStream(Dictionary<ulong, Queue<ulong>> streamsBySession, ulong sessionId, out ulong streamId)
        {
            if (!streamsBySession.TryGetValue(sessionId, out Queue<ulong> streams) || streams.Count == 0)
            {
                streamId = 0;
                return false;
            }

            streamId = streams.Dequeue();
            if (streams.Count == 0)
            {
                streamsBySession.Remove(sessionId);
            }

            return true;
        }

        private void RemoveWaiter(Dictionary<ulong, List<NativeEventWaiter>> waitersById, ulong id, NativeEventWaiter waiter)
        {
            lock (_gate)
            {
                if (!waitersById.TryGetValue(id, out List<NativeEventWaiter> waiters))
                {
                    return;
                }

                waiters.Remove(waiter);
                if (waiters.Count == 0)
                {
                    waitersById.Remove(id);
                }
            }
        }

        private static void DrainWaiters(Dictionary<ulong, List<NativeEventWaiter>> waitersById, List<NativeEventWaiter> destination)
        {
            foreach (List<NativeEventWaiter> waiters in waitersById.Values)
            {
                destination.AddRange(waiters);
            }

            waitersById.Clear();
        }

        private async Task WaitWithTimeoutAsync(NativeEventWaiter waiter, TimeSpan timeout, CancellationToken cancellationToken)
        {
            using (waiter)
            {
                if (timeout <= TimeSpan.Zero)
                {
                    await waiter.Task.ConfigureAwait(false);
                    return;
                }

                Task delayTask = Task.Delay(timeout, cancellationToken);
                Task completedTask = await Task.WhenAny(waiter.Task, delayTask).ConfigureAwait(false);
                if (completedTask == waiter.Task)
                {
                    await waiter.Task.ConfigureAwait(false);
                    return;
                }

                cancellationToken.ThrowIfCancellationRequested();
                throw new WebTransportException(WebTransportErrorCode.TransportError, "The WebTransport session was not accepted before the connect timeout elapsed.");
            }
        }

        private static WebTransportException ToException(WebTransportNativeEvent nativeEvent, string defaultMessage)
        {
            WebTransportNativeStatus status = nativeEvent.Status == WebTransportNativeStatus.Ok
                ? WebTransportNativeStatus.TransportError
                : nativeEvent.Status;

            WebTransportException mapped = NativeErrorMapper.Map(new WebTransportNativeException(status, defaultMessage));
            if (nativeEvent.ErrorCode == 0)
            {
                return mapped;
            }

            return new WebTransportException(mapped.ErrorCode, mapped.Message, mapped, nativeEvent.ErrorCode);
        }

        private void ThrowIfTerminal()
        {
            if (_terminalException != null)
            {
                throw _terminalException;
            }
        }

        private void ThrowIfDisposed()
        {
            if (_disposed)
            {
                throw new ObjectDisposedException(nameof(NativeEventCoordinator));
            }
        }

        internal sealed class NativeEventWaiter : IDisposable
        {
            private static readonly Task CompletedTask = Task.FromResult(true);

            private readonly NativeEventCoordinator? _owner;
            private readonly Dictionary<ulong, List<NativeEventWaiter>>? _waitersById;
            private readonly ulong _id;
            private readonly TaskCompletionSource<bool>? _completion;
            private readonly CancellationTokenRegistration _cancellationRegistration;
            private bool _disposed;

            private NativeEventWaiter(Task task)
            {
                Task = task;
            }

            public NativeEventWaiter(
                NativeEventCoordinator owner,
                Dictionary<ulong, List<NativeEventWaiter>> waitersById,
                ulong id,
                CancellationToken cancellationToken)
            {
                _owner = owner;
                _waitersById = waitersById;
                _id = id;
                _completion = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
                Task = _completion.Task;

                if (cancellationToken.CanBeCanceled)
                {
                    _cancellationRegistration = cancellationToken.Register(state =>
                    {
                        var waiter = (NativeEventWaiter)state;
                        waiter._owner!.RemoveWaiter(waiter._waitersById!, waiter._id, waiter);
                        waiter._completion!.TrySetCanceled();
                    }, this);
                }
            }

            public static NativeEventWaiter Completed { get; } = new NativeEventWaiter(CompletedTask);

            public Task Task { get; }

            public void TrySetResult()
            {
                _completion!.TrySetResult(true);
            }

            public void TrySetException(Exception exception)
            {
                _completion!.TrySetException(exception);
            }

            public void Dispose()
            {
                if (_disposed || _owner == null)
                {
                    return;
                }

                _owner.RemoveWaiter(_waitersById!, _id, this);
                _cancellationRegistration.Dispose();
                _disposed = true;
            }
        }
    }
}
