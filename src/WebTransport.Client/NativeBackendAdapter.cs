using System;
using System.Collections.Generic;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using WebTransport.Native;

namespace WebTransport
{
    internal sealed class NativeBackendAdapter : IWebTransportBackend, INativeEventSource
    {
        private readonly NativeWebTransportBackend _native;
        private readonly INativeWebTransportOperations _operations;
        private readonly NativeEventCoordinator _events;

        public NativeBackendAdapter()
            : this(new NativeWebTransportBackend())
        {
        }

        internal NativeBackendAdapter(NativeWebTransportBackend native)
        {
            _native = native ?? throw new ArgumentNullException(nameof(native));
            _operations = new NativeWebTransportOperations(_native);
            _events = new NativeEventCoordinator(this, _operations.Release);
        }

        public async ValueTask<IWebTransportSessionBackend> ConnectAsync(Uri uri, WebTransportClientOptions options, CancellationToken cancellationToken)
        {
            if (uri is null)
            {
                throw new ArgumentNullException(nameof(uri));
            }

            if (options is null)
            {
                throw new ArgumentNullException(nameof(options));
            }

            cancellationToken.ThrowIfCancellationRequested();

            ulong sessionId = 0;
            try
            {
                string serializedHeaders = HeaderSerializer.Serialize(options.Headers);
                sessionId = _native.Connect(
                    uri.AbsoluteUri,
                    serializedHeaders,
                    ToMilliseconds(options.ConnectTimeout),
                    ToMilliseconds(options.IdleTimeout),
                    options.AllowUntrustedCertificates,
                    options.EnableDatagrams);

                await _events.WaitForSessionConnectedAsync(sessionId, options.ConnectTimeout, cancellationToken).ConfigureAwait(false);
                return new NativeSessionBackend(_operations, _events, sessionId);
            }
            catch (OperationCanceledException)
            {
                ReleaseSessionIfAllocated(sessionId);
                throw;
            }
            catch (WebTransportException)
            {
                ReleaseSessionIfAllocated(sessionId);
                throw;
            }
            catch (DllNotFoundException ex)
            {
                throw new WebTransportException(WebTransportErrorCode.NativeLibraryUnavailable, "The native WebTransport backend library could not be loaded.", ex);
            }
            catch (EntryPointNotFoundException ex)
            {
                throw new WebTransportException(WebTransportErrorCode.NativeLibraryUnavailable, "The native WebTransport backend ABI does not match this managed package.", ex);
            }
            catch (WebTransportNativeException ex)
            {
                throw NativeErrorMapper.Map(ex);
            }
        }

        private void ReleaseSessionIfAllocated(ulong sessionId)
        {
            if (sessionId == 0)
            {
                return;
            }

            try
            {
                _operations.Release(sessionId);
            }
            catch (WebTransportNativeException)
            {
            }
        }

        public bool TryPollEvent(out WebTransportNativeEvent nativeEvent)
        {
            return _native.TryPollEvent(out nativeEvent);
        }

        public async ValueTask DisposeAsync()
        {
            // wt_client_shutdown severs native event sinks and begins QUIC
            // shutdown (non-blocking). The pump is then stopped, and the native
            // client is released. wt_release now performs the blocking MsQuic
            // teardown on a detached thread, so none of these calls block the
            // caller or leave a managed thread parked inside the native DLL.
            _native.Shutdown();
            await _events.DisposeAsync().ConfigureAwait(false);
            _native.Dispose();
        }

        private static uint ToMilliseconds(TimeSpan value)
        {
            if (value <= TimeSpan.Zero)
            {
                return 0;
            }

            double milliseconds = value.TotalMilliseconds;
            return milliseconds >= uint.MaxValue ? uint.MaxValue : (uint)milliseconds;
        }

        private static class HeaderSerializer
        {
            public static string Serialize(IEnumerable<KeyValuePair<string, string>> headers)
            {
                var builder = new StringBuilder();
                foreach (KeyValuePair<string, string> header in headers)
                {
                    builder.Append(header.Key);
                    builder.Append(": ");
                    builder.Append(header.Value);
                    builder.Append('\n');
                }

                return builder.ToString();
            }
        }
    }
}
