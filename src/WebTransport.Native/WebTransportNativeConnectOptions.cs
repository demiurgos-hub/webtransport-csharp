using System;
using System.Runtime.InteropServices;

namespace WebTransport.Native
{
    /// <summary>
    /// Native connection options passed across the C ABI.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct WebTransportNativeConnectOptions
    {
        public IntPtr UrlUtf8;
        public nuint UrlLength;
        public IntPtr HeadersUtf8;
        public nuint HeadersLength;
        public uint ConnectTimeoutMilliseconds;
        public uint IdleTimeoutMilliseconds;
        public byte AllowUntrustedCertificates;
        public byte EnableDatagrams;
        public ushort Reserved;
    }
}
