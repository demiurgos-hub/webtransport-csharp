using System;
using System.Threading;
using System.Threading.Tasks;

namespace WebTransport
{
    /// <summary>
    /// Client entry point for opening WebTransport sessions.
    /// </summary>
    public sealed class WebTransportClient : IAsyncDisposable
    {
        private const int DisposeTimeoutMilliseconds = 1000;

        private readonly WebTransportClientOptions _options;
        private readonly IWebTransportBackend _backend;
        private readonly object _disposeGate = new object();
        private Task? _disposeTask;
        private bool _disposed;
        private const int MaxCloseReasonBytes = 1024;

        public WebTransportClient()
            : this(new WebTransportClientOptions())
        {
        }

        public WebTransportClient(WebTransportClientOptions options)
            : this(options, new NativeBackendAdapter())
        {
        }

        internal WebTransportClient(WebTransportClientOptions options, IWebTransportBackend backend)
        {
            _options = options ?? throw new ArgumentNullException(nameof(options));
            _backend = backend ?? throw new ArgumentNullException(nameof(backend));
        }

        public async ValueTask<WebTransportSession> ConnectAsync(Uri uri, CancellationToken cancellationToken = default)
        {
            ThrowIfDisposed();

            if (uri is null)
            {
                throw new ArgumentNullException(nameof(uri));
            }

            if (!string.Equals(uri.Scheme, Uri.UriSchemeHttps, StringComparison.OrdinalIgnoreCase))
            {
                throw new ArgumentException("WebTransport sessions must use an https URI.", nameof(uri));
            }

            ValidateOptions(_options);
            IWebTransportSessionBackend sessionBackend = await _backend.ConnectAsync(uri, _options, cancellationToken).ConfigureAwait(false);
            return new WebTransportSession(sessionBackend);
        }

        public ValueTask DisposeAsync()
        {
            Task disposeTask;
            lock (_disposeGate)
            {
                if (_disposeTask == null)
                {
                    _disposed = true;
                    _disposeTask = DisposeBackendAsync();
                }

                disposeTask = _disposeTask;
            }

            return new ValueTask(disposeTask);
        }

        private async Task DisposeBackendAsync()
        {
            Task backendDisposeTask = _backend.DisposeAsync().AsTask();
            Task completedTask = await Task.WhenAny(
                backendDisposeTask,
                Task.Delay(DisposeTimeoutMilliseconds)).ConfigureAwait(false);

            if (completedTask == backendDisposeTask)
            {
                await backendDisposeTask.ConfigureAwait(false);
            }
        }

        private void ThrowIfDisposed()
        {
            if (_disposed)
            {
                throw new ObjectDisposedException(nameof(WebTransportClient));
            }
        }

        private static void ValidateOptions(WebTransportClientOptions options)
        {
            if (options.ConnectTimeout <= TimeSpan.Zero)
            {
                throw new ArgumentOutOfRangeException(nameof(options), "ConnectTimeout must be positive.");
            }

            if (options.IdleTimeout <= TimeSpan.Zero)
            {
                throw new ArgumentOutOfRangeException(nameof(options), "IdleTimeout must be positive.");
            }

            foreach (System.Collections.Generic.KeyValuePair<string, string> header in options.Headers)
            {
                ValidateHeader(header.Key, header.Value);
            }
        }

        private static void ValidateHeader(string name, string value)
        {
            if (string.IsNullOrWhiteSpace(name))
            {
                throw new ArgumentException("Header names must not be empty.", nameof(WebTransportClientOptions.Headers));
            }

            if (name[0] == ':')
            {
                throw new ArgumentException("Pseudo-headers cannot be overridden through WebTransportClientOptions.Headers.", nameof(WebTransportClientOptions.Headers));
            }

            for (int i = 0; i < name.Length; i++)
            {
                char c = name[i];
                if (c <= 32 || c >= 127 || c == ':')
                {
                    throw new ArgumentException("Header names must contain only HTTP token characters and must not include ':'.", nameof(WebTransportClientOptions.Headers));
                }
            }

            if (value == null)
            {
                throw new ArgumentException("Header values must not be null.", nameof(WebTransportClientOptions.Headers));
            }

            if (value.Length > MaxCloseReasonBytes)
            {
                throw new ArgumentOutOfRangeException(nameof(WebTransportClientOptions.Headers), "Header values are too large.");
            }

            for (int i = 0; i < value.Length; i++)
            {
                char c = value[i];
                if (c == '\r' || c == '\n' || c == '\0')
                {
                    throw new ArgumentException("Header values must not contain CR, LF, or NUL characters.", nameof(WebTransportClientOptions.Headers));
                }
            }
        }
    }
}
