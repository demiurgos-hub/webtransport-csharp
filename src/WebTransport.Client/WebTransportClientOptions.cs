using System;
using System.Collections.Generic;

namespace WebTransport
{
    /// <summary>
    /// Options used when creating and connecting a WebTransport client.
    /// </summary>
    public sealed class WebTransportClientOptions
    {
        public WebTransportClientOptions()
        {
            Headers = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            ConnectTimeout = TimeSpan.FromSeconds(10);
            IdleTimeout = TimeSpan.FromSeconds(30);
            EnableDatagrams = true;
        }

        /// <summary>
        /// Additional HTTP headers sent with the WebTransport CONNECT request.
        /// </summary>
        public IDictionary<string, string> Headers { get; }

        /// <summary>
        /// Maximum time allowed for the QUIC, HTTP/3, and WebTransport handshake.
        /// </summary>
        public TimeSpan ConnectTimeout { get; set; }

        /// <summary>
        /// Maximum idle time before the native backend closes the connection.
        /// </summary>
        public TimeSpan IdleTimeout { get; set; }

        /// <summary>
        /// Enables HTTP/3 datagram negotiation.
        /// </summary>
        public bool EnableDatagrams { get; set; }

        /// <summary>
        /// Allows development certificates. This should not be enabled in production.
        /// </summary>
        public bool AllowUntrustedCertificates { get; set; }
    }
}
