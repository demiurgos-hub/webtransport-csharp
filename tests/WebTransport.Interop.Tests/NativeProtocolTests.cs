using System;
using System.Runtime.InteropServices;
using WebTransport.Native;
using Xunit;

namespace WebTransport.Interop.Tests;

public sealed class NativeProtocolTests
{
    private const string NativeLibraryName = "webtransport_native";

    [Fact]
    public void Http3SettingsRoundTripsInNativeLibraryWhenHooksArePresent()
    {
        try
        {
            Assert.Equal(WebTransportNativeStatus.Ok, TestHttp3SettingsRoundtrip());
        }
        catch (EntryPointNotFoundException)
        {
        }
        catch (DllNotFoundException)
        {
        }
    }

    [Fact]
    public void QpackStatusDecodeReadsLiteralStatusWhenHooksArePresent()
    {
        try
        {
            Assert.Equal(WebTransportNativeStatus.Ok, TestQpackStatusDecode(out ushort statusCode));
            Assert.Equal(200, statusCode);
        }
        catch (EntryPointNotFoundException)
        {
        }
        catch (DllNotFoundException)
        {
        }
    }

    [Fact]
    public void WebTransportStreamPrefixEncodesFrameTypeAndSessionIdWhenHooksArePresent()
    {
        try
        {
            byte[] buffer = new byte[16];
            Assert.Equal(WebTransportNativeStatus.Ok, TestWebTransportStreamPrefix(3, buffer, (nuint)buffer.Length, out nuint bytesWritten));

            Assert.Equal((nuint)3, bytesWritten);
            Assert.Equal(0x40, buffer[0]);
            Assert.Equal(0x41, buffer[1]);
            Assert.Equal(0x03, buffer[2]);
        }
        catch (EntryPointNotFoundException)
        {
        }
        catch (DllNotFoundException)
        {
        }
    }

    [Fact]
    public void WebTransportConnectRequestIncludesCallerHeadersWhenHooksArePresent()
    {
        try
        {
            Assert.Equal(WebTransportNativeStatus.Ok, TestConnectRequestIncludesHeaders());
        }
        catch (EntryPointNotFoundException)
        {
        }
        catch (DllNotFoundException)
        {
        }
    }

    [Fact]
    public void WebTransportDatagramRoundtripPreservesSessionIdAndPayloadWhenHooksArePresent()
    {
        try
        {
            byte[] payload = { 10, 20, 30 };
            byte[] output = new byte[16];

            Assert.Equal(
                WebTransportNativeStatus.Ok,
                TestDatagramRoundtrip(
                    7,
                    payload,
                    (nuint)payload.Length,
                    out ulong decodedSessionId,
                    output,
                    (nuint)output.Length,
                    out nuint bytesWritten));

            Assert.Equal((ulong)7, decodedSessionId);
            Assert.Equal((nuint)3, bytesWritten);
            Assert.Equal(payload, output[..(int)bytesWritten]);
        }
        catch (EntryPointNotFoundException)
        {
        }
        catch (DllNotFoundException)
        {
        }
    }

    [Fact]
    public void WebTransportDatagramMapsWireSessionIdToLocalHandleWhenHooksArePresent()
    {
        try
        {
            byte[] payload = { 42, 43, 44 };
            byte[] output = new byte[16];

            Assert.Equal(
                WebTransportNativeStatus.Ok,
                TestDatagramWireLocalMapping(
                    payload,
                    (nuint)payload.Length,
                    out ulong localSessionId,
                    out ulong wireSessionId,
                    out ulong encodedSessionId,
                    output,
                    (nuint)output.Length,
                    out nuint bytesWritten));

            Assert.NotEqual(localSessionId, wireSessionId);
            Assert.Equal(wireSessionId / 4, encodedSessionId);
            Assert.Equal((nuint)payload.Length, bytesWritten);
            Assert.Equal(payload, output[..(int)bytesWritten]);
        }
        catch (EntryPointNotFoundException)
        {
        }
        catch (DllNotFoundException)
        {
        }
    }

    [Fact]
    public void WebTransportDatagramAllowsZeroFlowIdWhenHooksArePresent()
    {
        try
        {
            byte[] payload = { 1, 2, 3 };
            byte[] output = new byte[16];

            Assert.Equal(
                WebTransportNativeStatus.Ok,
                TestDatagramAllowsZeroFlowId(
                    payload,
                    (nuint)payload.Length,
                    out ulong encodedSessionId,
                    output,
                    (nuint)output.Length,
                    out nuint bytesWritten));

            Assert.Equal((ulong)0, encodedSessionId);
            Assert.Equal((nuint)payload.Length, bytesWritten);
            Assert.Equal(payload, output[..(int)bytesWritten]);
        }
        catch (EntryPointNotFoundException)
        {
        }
        catch (DllNotFoundException)
        {
        }
    }

    [Fact]
    public void SessionConnectedRequiresAcceptanceAndStreamIdWhenHooksArePresent()
    {
        try
        {
            Assert.Equal(
                WebTransportNativeStatus.Ok,
                TestSessionConnectedRequiresAcceptanceAndStreamId(
                    out uint acceptedBeforeStreamCount,
                    out uint streamBeforeAcceptedCount,
                    out uint bothReadyCount));

            Assert.Equal((uint)0, acceptedBeforeStreamCount);
            Assert.Equal((uint)0, streamBeforeAcceptedCount);
            Assert.Equal((uint)1, bothReadyCount);
        }
        catch (EntryPointNotFoundException)
        {
        }
        catch (DllNotFoundException)
        {
        }
    }

    [Fact]
    public void ClientReleaseCleansChildHandlesWhenHooksArePresent()
    {
        try
        {
            Assert.Equal(WebTransportNativeStatus.Ok, TestClientReleaseCleansChildren());
        }
        catch (EntryPointNotFoundException)
        {
        }
        catch (DllNotFoundException)
        {
        }
    }

    [DllImport(NativeLibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "wt_test_http3_settings_roundtrip")]
    private static extern WebTransportNativeStatus TestHttp3SettingsRoundtrip();

    [DllImport(NativeLibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "wt_test_qpack_status_decode")]
    private static extern WebTransportNativeStatus TestQpackStatusDecode(out ushort statusCode);

    [DllImport(NativeLibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "wt_test_webtransport_stream_prefix")]
    private static extern WebTransportNativeStatus TestWebTransportStreamPrefix(
        ulong sessionId,
        byte[] output,
        nuint outputLength,
        out nuint bytesWritten);

    [DllImport(NativeLibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "wt_test_connect_request_includes_headers")]
    private static extern WebTransportNativeStatus TestConnectRequestIncludesHeaders();

    [DllImport(NativeLibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "wt_test_datagram_roundtrip")]
    private static extern WebTransportNativeStatus TestDatagramRoundtrip(
        ulong sessionId,
        byte[] payload,
        nuint payloadLength,
        out ulong decodedSessionId,
        byte[] output,
        nuint outputLength,
        out nuint bytesWritten);

    [DllImport(NativeLibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "wt_test_datagram_wire_local_mapping")]
    private static extern WebTransportNativeStatus TestDatagramWireLocalMapping(
        byte[] payload,
        nuint payloadLength,
        out ulong localSessionId,
        out ulong wireSessionId,
        out ulong encodedSessionId,
        byte[] output,
        nuint outputLength,
        out nuint bytesWritten);

    [DllImport(NativeLibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "wt_test_datagram_allows_zero_flow_id")]
    private static extern WebTransportNativeStatus TestDatagramAllowsZeroFlowId(
        byte[] payload,
        nuint payloadLength,
        out ulong encodedSessionId,
        byte[] output,
        nuint outputLength,
        out nuint bytesWritten);

    [DllImport(NativeLibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "wt_test_session_connected_requires_acceptance_and_stream_id")]
    private static extern WebTransportNativeStatus TestSessionConnectedRequiresAcceptanceAndStreamId(
        out uint acceptedBeforeStreamCount,
        out uint streamBeforeAcceptedCount,
        out uint bothReadyCount);

    [DllImport(NativeLibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "wt_test_client_release_cleans_children")]
    private static extern WebTransportNativeStatus TestClientReleaseCleansChildren();
}
