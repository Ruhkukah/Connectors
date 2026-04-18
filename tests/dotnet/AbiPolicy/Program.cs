using MoexConnector.AlorEngine;

if (args.Length != 2)
{
    throw new InvalidOperationException("expected native library path and replay fixture path");
}

var libraryPath = Path.GetFullPath(args[0]);
var replayPath = Path.GetFullPath(args[1]);

if (!File.Exists(libraryPath))
{
    throw new FileNotFoundException("native library not found", libraryPath);
}

if (!File.Exists(replayPath))
{
    throw new FileNotFoundException("replay fixture not found", replayPath);
}

using var library = MoexNativeLibrary.Load(libraryPath);

var expectedStructs = new[]
{
    "MoexEventHeader",
    "MoexBackpressureCounters",
    "MoexHealthSnapshot",
    "MoexConnectorCreateParams",
    "MoexProfileLoadParams",
    "MoexOrderSubmitRequest",
    "MoexOrderCancelRequest",
    "MoexOrderReplaceRequest",
    "MoexMassCancelRequest",
    "MoexSubscriptionRequest",
    "MoexPolledEvent"
};

if (!expectedStructs.SequenceEqual(library.Layout.Structs.Select(item => item.Name), StringComparer.Ordinal))
{
    throw new InvalidOperationException("unexpected ABI struct coverage set");
}

foreach (var layout in library.Layout.Structs)
{
    if (layout.NativeSize != layout.ManagedSize)
    {
        throw new InvalidOperationException($"{layout.Name} size mismatch");
    }

    if (layout.NativeAlignment != layout.ManagedAlignment)
    {
        throw new InvalidOperationException($"{layout.Name} alignment mismatch");
    }
}

using var connector = library.CreateConnectorClient("abi-policy", "phase1_1");
connector.LoadProfile("profiles/replay.yaml", armed: false);

var callbackEvents = new List<string>();
connector.RegisterLowRateCallback(evt => callbackEvents.Add($"{evt.Header.EventType}:{evt.ReplayState}:{evt.InfoText}"));
GC.Collect();
GC.WaitForPendingFinalizers();
GC.Collect();

connector.LoadSyntheticReplay(replayPath);
connector.Start();

var tooSmallResult = connector.PollEventsWithStrideForTest(
    checked((int)library.Layout.Structs.Single(item => item.Name == "MoexPolledEvent").NativeSize) - 1,
    1,
    out var tooSmallWritten);
if (tooSmallResult != MoexResult.InvalidArgument || tooSmallWritten != 0U)
{
    throw new InvalidOperationException("expected invalid argument for too-small poll stride");
}

var allEvents = new List<MoexConnectorEvent>();
while (true)
{
    var batch = connector.PollEvents(4);
    if (batch.Count == 0)
    {
        break;
    }

    allEvents.AddRange(batch);
}

var expectedCallbacks = new[]
{
    "ReplayState:Started:synthetic replay started",
    "ReplayState:Drained:synthetic replay drained"
};
if (!callbackEvents.SequenceEqual(expectedCallbacks, StringComparer.Ordinal))
{
    throw new InvalidOperationException($"unexpected callback sequence: {string.Join(", ", callbackEvents)}");
}

if (allEvents.Count != 9)
{
    throw new InvalidOperationException($"unexpected event count: {allEvents.Count}");
}

connector.Stop();

try
{
    connector.PollEvents(1);
    throw new InvalidOperationException("poll after stop should fail");
}
catch (MoexNativeException ex) when (ex.Result == MoexResult.NotStarted)
{
}

var healthAfterStop = connector.GetHealth();
if (healthAfterStop.ConnectorState != 1U)
{
    throw new InvalidOperationException($"unexpected connector state after stop: {healthAfterStop.ConnectorState}");
}

connector.Dispose();

try
{
    library.Dispose();
}
catch (Exception ex)
{
    throw new InvalidOperationException("library dispose should succeed after connector disposal", ex);
}

Console.WriteLine("dotnet abi policy passed");
