using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using WebTransport.Native;
using Xunit;

namespace WebTransport.Interop.Tests;

public sealed class NativeCloseCapsuleTests
{
    [Theory]
    [InlineData(1)]
    [InlineData(2)]
    [InlineData(17)]
    [InlineData(4096)]
    public void FragmentedCapsulesRetainFirstCodeAndUtf8Reason(int fragment)
    {
        if (!RequiredHooks()) return;
        byte[] reason = Encoding.UTF8.GetBytes("admission_denied 拒否");
        var bytes = new List<byte> { 0x10, 0 }; // Unknown empty capsule.
        bytes.AddRange(Capsule(0xfedcba98, reason));
        bytes.AddRange(Capsule(1, "later"u8.ToArray()));
        byte[] output = new byte[1024];
        Assert.Equal(WebTransportNativeStatus.Ok, Parse(bytes.ToArray(), (nuint)bytes.Count, (nuint)fragment,
            out uint code, output, (nuint)output.Length, out nuint length));
        Assert.Equal(0xfedcba98u, code);
        Assert.Equal(reason, output[..(int)length]);
    }

    [Fact]
    public void CloseCapsulesEnforceBoundsUtf8AndCompleteness()
    {
        if (!RequiredHooks()) return;
        AssertStatus(Capsule(0, Array.Empty<byte>()), WebTransportNativeStatus.Ok);
        AssertStatus(Capsule(0, Encoding.UTF8.GetBytes(new string('a', 1024))), WebTransportNativeStatus.Ok);
        AssertStatus(Capsule(0, new byte[1025]), WebTransportNativeStatus.ProtocolError);
        AssertStatus(Capsule(0, new byte[] { 0xc0, 0x80 }), WebTransportNativeStatus.ProtocolError);
        AssertStatus(Capsule(0, new byte[] { 0xed, 0xa0, 0x80 }), WebTransportNativeStatus.ProtocolError);
        AssertStatus(new byte[] { 0x68, 0x43, 3, 0, 0, 0 }, WebTransportNativeStatus.ProtocolError);
        AssertStatus(Capsule(0, "x"u8.ToArray())[..^1], WebTransportNativeStatus.Pending);
    }

    private static byte[] Capsule(uint code, byte[] reason)
    {
        int length = reason.Length + 4;
        var bytes = new List<byte> { 0x68, 0x43 };
        if (length < 64) bytes.Add((byte)length);
        else { bytes.Add((byte)(0x40 | (length >> 8))); bytes.Add((byte)length); }
        bytes.AddRange(new[] { (byte)(code >> 24), (byte)(code >> 16), (byte)(code >> 8), (byte)code });
        bytes.AddRange(reason);
        return bytes.ToArray();
    }

    private static void AssertStatus(byte[] bytes, WebTransportNativeStatus expected)
    {
        Assert.Equal(expected, Parse(bytes, (nuint)bytes.Length, 1, out _, new byte[1024], 1024, out _));
    }

    private static bool RequiredHooks()
    {
        try { AssertStatus(Array.Empty<byte>(), WebTransportNativeStatus.Pending); return true; }
        catch (Exception ex) when ((ex is DllNotFoundException || ex is EntryPointNotFoundException) &&
            Environment.GetEnvironmentVariable("WEBTRANSPORT_REQUIRE_NATIVE_HOOKS") != "1") { return false; }
    }

    [DllImport("webtransport_native", EntryPoint = "wt_test_close_capsules", CallingConvention = CallingConvention.Cdecl)]
    private static extern WebTransportNativeStatus Parse(byte[] data, nuint length, nuint fragment,
        out uint code, byte[] reason, nuint capacity, out nuint reasonLength);
}
