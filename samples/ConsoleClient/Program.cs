using WebTransport;

if (args.Length == 0 || !Uri.TryCreate(args[0], UriKind.Absolute, out Uri? uri))
{
    Console.Error.WriteLine("Usage: ConsoleClient https://host/path");
    return 2;
}

var options = new WebTransportClientOptions
{
    AllowUntrustedCertificates = false,
    EnableDatagrams = true
};

await using var client = new WebTransportClient(options);

try
{
    await using WebTransportSession session = await client.ConnectAsync(uri);
    await using WebTransportStream stream = await session.OpenBidirectionalStreamAsync();

    byte[] payload = "hello webtransport"u8.ToArray();
    await stream.WriteAsync(payload);
    await session.SendDatagramAsync(payload);

    Console.WriteLine("Connected, wrote one stream payload, and sent one datagram.");
}
catch (WebTransportException ex)
{
    Console.Error.WriteLine($"{ex.ErrorCode}: {ex.Message}");
    return 1;
}

return 0;
