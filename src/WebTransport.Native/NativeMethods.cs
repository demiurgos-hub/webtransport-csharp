using System;
using System.Runtime.InteropServices;

namespace WebTransport.Native
{
    internal static class NativeMethods
    {
        internal const string LibraryName = "webtransport_native";
        internal const uint ExpectedAbiVersion = 1;

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "wt_get_abi_version")]
        internal static extern uint GetAbiVersion();

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "wt_client_create")]
        internal static extern WebTransportNativeStatus ClientCreate(out ulong clientId);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "wt_client_connect")]
        internal static extern WebTransportNativeStatus ClientConnect(
            ulong clientId,
            in WebTransportNativeConnectOptions options,
            out ulong operationId);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "wt_client_shutdown")]
        internal static extern WebTransportNativeStatus ClientShutdown(
            ulong clientId,
            ulong errorCode);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "wt_poll_event")]
        internal static extern WebTransportNativeStatus PollEvent(
            ulong clientId,
            out WebTransportNativeEvent nativeEvent);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "wt_session_open_bidi_stream")]
        internal static extern WebTransportNativeStatus SessionOpenBidirectionalStream(
            ulong sessionId,
            out ulong operationId);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "wt_session_open_uni_stream")]
        internal static extern WebTransportNativeStatus SessionOpenUnidirectionalStream(
            ulong sessionId,
            out ulong operationId);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "wt_session_send_datagram")]
        internal static extern WebTransportNativeStatus SessionSendDatagram(
            ulong sessionId,
            IntPtr payload,
            nuint payloadLength,
            out ulong operationId);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "wt_session_receive_datagram")]
        internal static extern WebTransportNativeStatus SessionReceiveDatagram(
            ulong sessionId,
            IntPtr buffer,
            nuint bufferLength,
            out nuint bytesRead);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "wt_stream_read")]
        internal static extern WebTransportNativeStatus StreamRead(
            ulong streamId,
            IntPtr buffer,
            nuint bufferLength,
            out nuint bytesRead);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "wt_stream_write")]
        internal static extern WebTransportNativeStatus StreamWrite(
            ulong streamId,
            IntPtr payload,
            nuint payloadLength,
            byte endStream,
            out ulong operationId);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "wt_stream_finish")]
        internal static extern WebTransportNativeStatus StreamFinish(
            ulong streamId,
            out ulong operationId);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "wt_stream_reset")]
        internal static extern WebTransportNativeStatus StreamReset(
            ulong streamId,
            ulong errorCode);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "wt_session_close")]
        internal static extern WebTransportNativeStatus SessionClose(
            ulong sessionId,
            ulong errorCode,
            IntPtr reasonUtf8,
            nuint reasonLength);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "wt_release")]
        internal static extern void Release(ulong handle);
    }
}
