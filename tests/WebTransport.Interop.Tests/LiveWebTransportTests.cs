using System;
using System.Buffers.Binary;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Xunit;
using Xunit.Abstractions;

namespace WebTransport.Interop.Tests;

public sealed class LiveWebTransportTests
{
    private const int MaxReliableFrameLength = 32_000;
    private const string GolemExpectedSnapshot = "world-snapshot";
    private readonly ITestOutputHelper _output;

    public LiveWebTransportTests(ITestOutputHelper output)
    {
        _output = output;
    }

    [Fact]
    public async Task ConnectsToConfiguredWebTransportServer()
    {
        string? url = Environment.GetEnvironmentVariable("WEBTRANSPORT_INTEROP_URL");
        if (string.IsNullOrWhiteSpace(url))
        {
            return;
        }

        await using var client = new WebTransportClient(new WebTransportClientOptions
        {
            AllowUntrustedCertificates = Environment.GetEnvironmentVariable("WEBTRANSPORT_ALLOW_UNTRUSTED_CERTS") == "1"
        });

        await using WebTransportSession session = await client.ConnectAsync(new Uri(url));
        await using WebTransportStream stream = await session.OpenBidirectionalStreamAsync();

        byte[] payload = "interop"u8.ToArray();
        await stream.WriteAsync(payload);
        await session.SendDatagramAsync(payload);
    }

    [Fact]
    public async Task DisposeAsyncCompletesQuicklyForConfiguredWebTransportServer()
    {
        string? url = Environment.GetEnvironmentVariable("WEBTRANSPORT_INTEROP_URL");
        if (string.IsNullOrWhiteSpace(url))
        {
            return;
        }

        var client = new WebTransportClient(new WebTransportClientOptions
        {
            AllowUntrustedCertificates = Environment.GetEnvironmentVariable("WEBTRANSPORT_ALLOW_UNTRUSTED_CERTS") == "1",
            ConnectTimeout = TimeSpan.FromSeconds(5)
        });

        WebTransportSession session = await client.ConnectAsync(new Uri(url));
        WebTransportStream stream = await session.OpenBidirectionalStreamAsync();
        Task<WebTransportDatagram> datagramReceive = session.ReceiveDatagramAsync().AsTask();
        Task<int> streamRead = stream.ReadAsync(new byte[16]).AsTask();

        await client.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(2));

        Assert.True(datagramReceive.IsCompleted);
        Assert.True(streamRead.IsCompleted);
    }

    [Fact]
    public async Task GolemWebTransportServerSendsWorldSnapshotFrame()
    {
        // Opt in with GOLEM_WEBTRANSPORT_INTEROP_URL for a manual server, or
        // GOLEM_WEBTRANSPORT_START_TEST_SERVER=1 to launch the local Go helper.
        await using GolemServerProcess? server = await StartGolemServerIfRequestedAsync();
        string? url = Environment.GetEnvironmentVariable("GOLEM_WEBTRANSPORT_INTEROP_URL");
        if (string.IsNullOrWhiteSpace(url))
        {
            url = server?.Url;
        }

        if (string.IsNullOrWhiteSpace(url))
        {
            return;
        }

        _output.WriteLine($"Golem WebTransport URL: {url}");
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        CancellationToken token = timeout.Token;

        var client = new WebTransportClient(new WebTransportClientOptions
        {
            AllowUntrustedCertificates = true,
            ConnectTimeout = TimeSpan.FromSeconds(5)
        });

        WebTransportSession session = await RunStepAsync(
            "ConnectAsync",
            () => client.ConnectAsync(new Uri(url), token).AsTask());
        await RunStepAsync(
            "SendDatagramAsync",
            async () =>
            {
                await session.SendDatagramAsync("ready"u8.ToArray(), token);
                return true;
            });
        WebTransportStream stream = await RunStepAsync(
            "OpenBidirectionalStreamAsync",
            () => session.OpenBidirectionalStreamAsync(token).AsTask());

        await RunStepAsync(
            "PrimeWriteAsync",
            async () =>
            {
                await stream.WriteAsync(new byte[4], token);
                return true;
            });

        byte[] frame = await RunStepAsync(
            "ReadGolemReliableFrameAsync",
            () => ReadGolemReliableFrameAsync(stream, token));
        Assert.Equal(GolemExpectedSnapshot, Encoding.UTF8.GetString(frame));
    }

    private async Task<T> RunStepAsync<T>(string stepName, Func<Task<T>> action)
    {
        _output.WriteLine($"{DateTimeOffset.UtcNow:O} starting {stepName}");
        using var stepTimeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        Task<T> task = Task.Run(action);
        Task completedTask = await Task.WhenAny(task, Task.Delay(Timeout.InfiniteTimeSpan, stepTimeout.Token));
        if (completedTask != task)
        {
            _output.WriteLine($"{DateTimeOffset.UtcNow:O} timed out {stepName}");
            throw new TimeoutException($"Timed out during {stepName}.");
        }

        T result = await task;
        _output.WriteLine($"{DateTimeOffset.UtcNow:O} completed {stepName}");
        return result;
    }

    private static async Task<GolemServerProcess?> StartGolemServerIfRequestedAsync()
    {
        if (Environment.GetEnvironmentVariable("GOLEM_WEBTRANSPORT_START_TEST_SERVER") != "1" ||
            !string.IsNullOrWhiteSpace(Environment.GetEnvironmentVariable("GOLEM_WEBTRANSPORT_INTEROP_URL")))
        {
            return null;
        }

        int port = ReserveLoopbackUdpPort();
        string helperDirectory = Path.GetFullPath(Path.Combine(
            AppContext.BaseDirectory,
            "..",
            "..",
            "..",
            "TestServers",
            "GolemWebTransportServer"));

        var startInfo = new ProcessStartInfo
        {
            FileName = "go",
            Arguments = $"run . -addr 127.0.0.1:{port}",
            WorkingDirectory = helperDirectory,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            RedirectStandardInput = true,
            CreateNoWindow = true
        };

        var process = new Process
        {
            StartInfo = startInfo,
            EnableRaisingEvents = true
        };

        try
        {
            process.Start();
        }
        catch
        {
            process.Dispose();
            throw;
        }

        var stderr = new StringBuilder();
        Task stderrTask = Task.Run(async () =>
        {
            while (!process.StandardError.EndOfStream)
            {
                string? line = await process.StandardError.ReadLineAsync();
                stderr.AppendLine(line);
            }
        });

        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        try
        {
            Task<string?> readLineTask = process.StandardOutput.ReadLineAsync();
            while (!timeout.IsCancellationRequested)
            {
                Task completedTask = await Task.WhenAny(readLineTask, Task.Delay(250, timeout.Token));
                if (completedTask == readLineTask)
                {
                    string? line = await readLineTask;
                    if (line == null)
                    {
                        break;
                    }

                    if (line.StartsWith("GOLEM_WT_URL=", StringComparison.Ordinal))
                    {
                        return new GolemServerProcess(process, line.Substring("GOLEM_WT_URL=".Length), stderrTask);
                    }

                    readLineTask = process.StandardOutput.ReadLineAsync();
                }

                if (process.HasExited)
                {
                    break;
                }
            }

            string errorText = stderr.ToString();
            throw new InvalidOperationException(
                $"Timed out waiting for Golem test server URL. ExitCode={(process.HasExited ? process.ExitCode.ToString() : "running")}. Stderr: {errorText}");
        }
        catch
        {
            KillProcessTree(process);
            process.Dispose();
            throw;
        }
    }

    private static int ReserveLoopbackUdpPort()
    {
        using var udp = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));
        return ((IPEndPoint)udp.Client.LocalEndPoint!).Port;
    }

    private static async Task<byte[]> ReadGolemReliableFrameAsync(WebTransportStream stream, CancellationToken token)
    {
        byte[] header = await ReadExactlyAsync(stream, 4, token);
        int length = BinaryPrimitives.ReadInt32BigEndian(header);
        if (length < 0 || length > MaxReliableFrameLength)
        {
            throw new InvalidOperationException($"Invalid Golem reliable frame length: {length}.");
        }

        return await ReadExactlyAsync(stream, length, token);
    }

    private static async Task<byte[]> ReadExactlyAsync(WebTransportStream stream, int length, CancellationToken token)
    {
        byte[] buffer = new byte[length];
        int offset = 0;
        while (offset < length)
        {
            int bytesRead = await stream.ReadAsync(buffer.AsMemory(offset, length - offset), token);
            if (bytesRead == 0)
            {
                throw new EndOfStreamException($"Unexpected end of stream after reading {offset} of {length} bytes.");
            }

            offset += bytesRead;
        }

        return buffer;
    }

    private static void KillProcessTree(Process process)
    {
        try
        {
            if (!process.HasExited)
            {
                process.Kill(entireProcessTree: true);
            }
        }
        catch (InvalidOperationException)
        {
        }
    }

    private sealed class GolemServerProcess : IAsyncDisposable
    {
        private readonly Process _process;
        private readonly Task _stderrTask;

        public GolemServerProcess(Process process, string url, Task stderrTask)
        {
            _process = process;
            Url = url;
            _stderrTask = stderrTask;
        }

        public string Url { get; }

        public async ValueTask DisposeAsync()
        {
            try
            {
                if (!_process.HasExited)
                {
                    KillProcessTree(_process);
                    Task exitedTask = _process.WaitForExitAsync();
                    Task completedTask = await Task.WhenAny(exitedTask, Task.Delay(TimeSpan.FromSeconds(5)));
                    if (completedTask != exitedTask)
                    {
                        throw new TimeoutException("Timed out waiting for Golem helper process to exit.");
                    }
                }

                await Task.WhenAny(_stderrTask, Task.Delay(TimeSpan.FromSeconds(1)));
            }
            finally
            {
                _process.Dispose();
            }
        }
    }
}
