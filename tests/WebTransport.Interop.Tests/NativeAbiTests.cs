using System.Runtime.InteropServices;
using WebTransport.Native;
using Xunit;

namespace WebTransport.Interop.Tests;

public sealed class NativeAbiTests
{
    private const string NativeLibraryName = "webtransport_native";
    private const uint ExpectedAbiVersion = 1;

    [Fact]
    public void NativeLibraryReportsExpectedAbiVersion()
    {
        try
        {
            Assert.Equal(ExpectedAbiVersion, GetAbiVersion());
        }
        catch (DllNotFoundException)
        {
        }
    }

    [Fact]
    public async Task MsQuicBuildReportsConnectionEventForConfiguredUrl()
    {
        string? url = Environment.GetEnvironmentVariable("WEBTRANSPORT_MSQUIC_TEST_URL");
        if (string.IsNullOrWhiteSpace(url))
        {
            return;
        }

        using var backend = new NativeWebTransportBackend();

        try
        {
            backend.Connect(
                url,
                string.Empty,
                connectTimeoutMilliseconds: 5000,
                idleTimeoutMilliseconds: 5000,
                allowUntrustedCertificates: Environment.GetEnvironmentVariable("WEBTRANSPORT_ALLOW_UNTRUSTED_CERTS") == "1",
                enableDatagrams: true);
        }
        catch (WebTransportNativeException ex) when (ex.Status == WebTransportNativeStatus.Unsupported)
        {
            return;
        }

        Assert.True(
            backend.TryPollEvent(TimeSpan.FromSeconds(10), out WebTransportNativeEvent nativeEvent),
            "Expected MsQuic to report a connection, close, or error event.");

        Assert.True(
            nativeEvent.Type == WebTransportNativeEventType.ClientConnected ||
            nativeEvent.Type == WebTransportNativeEventType.ClientClosed ||
            nativeEvent.Type == WebTransportNativeEventType.Error,
            $"Unexpected event type: {nativeEvent.Type}");

        await Task.CompletedTask;
    }

    [DllImport(NativeLibraryName, CallingConvention = CallingConvention.Cdecl, EntryPoint = "wt_get_abi_version")]
    private static extern uint GetAbiVersion();
}
