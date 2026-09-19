using System.Buffers.Binary;
using System.Diagnostics;
using System.Net.Sockets;
using Google.Protobuf;
using MoexConnector.Dtc507Fixture;

static void Require(bool value, string message)
{
    if (!value)
        throw new InvalidOperationException(message);
}

static byte[] Varint(ulong value)
{
    var bytes = new List<byte>();
    while (value >= 128)
    {
        bytes.Add((byte)(value | 0x80));
        value >>= 7;
    }
    bytes.Add((byte)value);
    return bytes.ToArray();
}

static void Field(List<byte> payload, int number, ulong value)
{
    payload.AddRange(Varint((ulong)(number << 3)));
    payload.AddRange(Varint(value));
}

static void TextField(List<byte> payload, int number, string value)
{
    var bytes = System.Text.Encoding.UTF8.GetBytes(value);
    payload.AddRange(Varint((ulong)((number << 3) | 2)));
    payload.AddRange(Varint((ulong)bytes.Length));
    payload.AddRange(bytes);
}

static byte[] Frame(ushort type, byte[] payload)
{
    var result = new byte[payload.Length + 4];
    BinaryPrimitives.WriteUInt16LittleEndian(result, checked((ushort)result.Length));
    BinaryPrimitives.WriteUInt16LittleEndian(result.AsSpan(2), type);
    payload.CopyTo(result, 4);
    return result;
}

static async Task<byte[]> ReadExact(NetworkStream stream, int size, CancellationToken token)
{
    var result = new byte[size];
    var offset = 0;
    while (offset < size)
    {
        var read = await stream.ReadAsync(result.AsMemory(offset), token);
        if (read == 0)
            throw new EndOfStreamException($"socket closed after {offset} of {size} bytes");
        offset += read;
    }
    return result;
}

static async Task<(ushort Type, byte[] Payload)> ReadFrame(NetworkStream stream, CancellationToken token)
{
    var header = await ReadExact(stream, 4, token);
    var size = BinaryPrimitives.ReadUInt16LittleEndian(header);
    var type = BinaryPrimitives.ReadUInt16LittleEndian(header.AsSpan(2));
    Require(size >= 4, "invalid DTC frame size");
    return (type, await ReadExact(stream, size - 4, token));
}

static async Task CheckSession(string executable, uint symbolId, string mode)
{
    using var process = new Process
    {
        StartInfo = new ProcessStartInfo(executable, $"--serve-live507-fixture {symbolId} {mode}")
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false
        }
    };
    Require(process.Start(), "failed to start C++ DTC socket fixture");
    var portLine = await process.StandardOutput.ReadLineAsync().WaitAsync(TimeSpan.FromSeconds(5));
    if (!ushort.TryParse(portLine, out var port) || port == 0)
    {
        await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(2));
        throw new InvalidOperationException(
            $"fixture did not publish its loopback port (exit={process.ExitCode}): {await process.StandardError.ReadToEndAsync()}");
    }
    using var client = new TcpClient();
    using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
    await client.ConnectAsync("127.0.0.1", port, timeout.Token);
    await using var stream = client.GetStream();

    // Actual DTC binary negotiation -> protobuf LOGON -> protobuf 506/507.
    byte[] negotiation = [16, 0, 6, 0, 8, 0, 0, 0, 4, 0, 0, 0, (byte)'D', (byte)'T', (byte)'C', 0];
    await stream.WriteAsync(negotiation, timeout.Token);
    var encoding = await ReadFrame(stream, timeout.Token);
    Require(encoding.Type == 7 && encoding.Payload.SequenceEqual(negotiation.AsSpan(4).ToArray()),
        "DTC binary negotiation failed");

    var logon = new List<byte>();
    Field(logon, 1, 8);
    Field(logon, 7, 10);
    TextField(logon, 11, "independent-protobuf-test");
    await stream.WriteAsync(Frame(1, logon.ToArray()), timeout.Token);
    var logonFrame = await ReadFrame(stream, timeout.Token);
    Require(logonFrame.Type == 2, "LOGON did not return LOGON_RESPONSE");
    var logonResponse = LogonResponse.Parser.ParseFrom(logonFrame.Payload);
    Require(logonResponse.Result == 1 && logonResponse.TradingIsSupported == 0 &&
            logonResponse.OCOOrdersSupported == 0 && logonResponse.OrderCancelReplaceSupported == 0 &&
            logonResponse.SecurityDefinitionsSupported == 1 && logonResponse.MarketDepthIsSupported == 1,
        "read-only capabilities changed or account/trading capability was advertised");

    var definitionRequest = new List<byte>();
    Field(definitionRequest, 1, 41);
    TextField(definitionRequest, 2, "ALRS-12.26");
    TextField(definitionRequest, 3, "FORTS");
    await stream.WriteAsync(Frame(506, definitionRequest.ToArray()), timeout.Token);
    (ushort Type, byte[] Payload) definitionFrame = default;
    for (var attempt = 0; attempt < 16; attempt++)
    {
        definitionFrame = await ReadFrame(stream, timeout.Token);
        if (definitionFrame.Type is 507 or 509)
            break;
    }
    Require(definitionFrame.Type == 507, $"expected507, received DTC message {definitionFrame.Type}");
    var definition = SecurityDefinitionResponse.Parser.ParseFrom(definitionFrame.Payload);
    Require(definition.RequestID == 41 && definition.Symbol == "ALRS-12.26" && definition.Exchange == "FORTS",
        "507 instrument identity changed");
    Require((int)definition.SecurityType == 1,
        $"official generated schema decoded SecurityType={(int)definition.SecurityType}, expected FUTURE=1");
    Require(definition.IsFinalMessage == 1,
        $"official generated schema decoded IsFinalMessage={definition.IsFinalMessage}, expected final=1");

    // SymbolID belongs to MARKET_DEPTH_REQUEST. It must not be serialized as
    // SecurityType in the preceding official 507 response.
    var depthRequest = new List<byte>();
    Field(depthRequest, 1, 1);
    Field(depthRequest, 2, symbolId);
    TextField(depthRequest, 3, "ALRS-12.26");
    TextField(depthRequest, 4, "FORTS");
    Field(depthRequest, 5, 20);
    await stream.WriteAsync(Frame(102, depthRequest.ToArray()), timeout.Token);
    for (var level = 0; level < 2; level++)
    {
        (ushort Type, byte[] Payload) depth;
        do
        {
            depth = await ReadFrame(stream, timeout.Token);
            Require(depth.Type != 121 && depth.Type != 5, "depth request rejected or session logged off");
        } while (depth.Type != 145);
        Require(ReadFirstVarintField(depth.Payload, 1) == symbolId,
            "requested SymbolID was not preserved in the depth snapshot");
    }

    await stream.WriteAsync(Frame(400, []), timeout.Token);
    var accountsResponse = await ReadFrame(stream, timeout.Token);
    Require(accountsResponse.Type == 5, "read-only endpoint accepted an account request");

    client.Close();
    await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(3));
    Require(process.ExitCode == 0, $"C++ fixture exited {process.ExitCode}: {await process.StandardError.ReadToEndAsync()}");
    Console.WriteLine($"PASS mode={mode} depth_symbol_id={symbolId} SecurityType=FUTURE IsFinalMessage=1");
}

static ulong ReadFirstVarintField(byte[] payload, int fieldNumber)
{
    var offset = 0;
    while (offset < payload.Length)
    {
        ulong tag = 0;
        var shift = 0;
        byte current;
        do
        {
            Require(offset < payload.Length && shift < 70, "invalid depth protobuf varint");
            current = payload[offset++];
            tag |= (ulong)(current & 0x7f) << shift;
            shift += 7;
        } while ((current & 0x80) != 0);
        var wire = (int)(tag & 7);
        var number = (int)(tag >> 3);
        if (number == fieldNumber && wire == 0)
        {
            ulong value = 0;
            shift = 0;
            do
            {
                Require(offset < payload.Length && shift < 70, "invalid depth SymbolID varint");
                current = payload[offset++];
                value |= (ulong)(current & 0x7f) << shift;
                shift += 7;
            } while ((current & 0x80) != 0);
            return value;
        }
        switch (wire)
        {
            case 0:
                while (true)
                {
                    Require(offset < payload.Length, "truncated depth varint");
                    if ((payload[offset++] & 0x80) == 0) break;
                }
                break;
            case 1: offset += 8; break;
            case 2:
                ulong length = 0;
                shift = 0;
                do
                {
                    Require(offset < payload.Length && shift < 70, "invalid depth length varint");
                    current = payload[offset++];
                    length |= (ulong)(current & 0x7f) << shift;
                    shift += 7;
                } while ((current & 0x80) != 0);
                offset = checked(offset + (int)length);
                break;
            case 5: offset += 4; break;
            default: throw new InvalidOperationException("unsupported depth protobuf wire type");
        }
        Require(offset <= payload.Length, "depth protobuf field exceeds payload");
    }
    throw new InvalidOperationException("depth SymbolID is missing");
}

if (args.Length != 1)
    throw new ArgumentException("usage: Dtc507WireRegression <connector_host_dtc_server_test executable>");

foreach (var mode in new[] { "live", "replay" })
foreach (var symbolId in new uint[] { 1, 7, 101 })
    await CheckSession(Path.GetFullPath(args[0]), symbolId, mode);
