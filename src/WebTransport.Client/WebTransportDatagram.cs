using System;

namespace WebTransport
{
    /// <summary>
    /// Represents a WebTransport datagram payload.
    /// </summary>
    public readonly struct WebTransportDatagram
    {
        public WebTransportDatagram(byte[] payload)
        {
            Payload = payload ?? throw new ArgumentNullException(nameof(payload));
            PayloadMemory = payload;
        }

        public byte[] Payload { get; }

        public ReadOnlyMemory<byte> PayloadMemory { get; }
    }
}
