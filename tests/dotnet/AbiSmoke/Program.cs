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
if (library.AbiVersion != 1)
{
    throw new InvalidOperationException($"unexpected ABI version {library.AbiVersion}");
}

if (!library.ProductionRequiresExplicitArm())
{
    throw new InvalidOperationException("prod explicit arm policy should be enabled");
}

if (!library.IsEnvironmentStartAllowed("test", false))
{
    throw new InvalidOperationException("test environment should not require an explicit prod arm");
}

if (library.IsEnvironmentStartAllowed("prod", false))
{
    throw new InvalidOperationException("prod environment arming gate should reject unarmed startup");
}

var callbacks = new List<string>();

using var connector = library.CreateConnectorClient("abi-smoke", "phase1");
connector.LoadProfile("profiles/replay.yaml", armed: false);
connector.RegisterLowRateCallback(evt =>
{
    callbacks.Add($"{evt.Header.EventType}:{evt.ReplayState}:{evt.InfoText}");
});
connector.LoadSyntheticReplay(replayPath);
connector.Start();

var allEvents = new List<MoexConnectorEvent>();
while (true)
{
    var batch = connector.PollEvents(3);
    if (batch.Count == 0)
    {
        break;
    }

    allEvents.AddRange(batch);
}

var health = connector.GetHealth();
if (!health.ShadowModeEnabled)
{
    throw new InvalidOperationException("expected shadow mode health flag");
}

var counters = connector.GetBackpressureCounters();
if (counters.Produced != (ulong)allEvents.Count)
{
    throw new InvalidOperationException($"unexpected produced count {counters.Produced}");
}

if (counters.Polled != (ulong)allEvents.Count)
{
    throw new InvalidOperationException($"unexpected polled count {counters.Polled}");
}

if (counters.Dropped != 0 || counters.Overflowed)
{
    throw new InvalidOperationException("unexpected backpressure loss in abi smoke");
}

var expectedCallbacks = new[]
{
    "ReplayState:Started:synthetic replay started",
    "ReplayState:Drained:synthetic replay drained"
};

if (!callbacks.SequenceEqual(expectedCallbacks, StringComparer.Ordinal))
{
    throw new InvalidOperationException($"unexpected callback sequence: {string.Join(", ", callbacks)}");
}

if (allEvents.Count != 9)
{
    throw new InvalidOperationException($"unexpected replay event count: {allEvents.Count}");
}

if (allEvents[0].Header.EventType != MoexEventType.Instrument ||
    allEvents[^1].Header.EventType != MoexEventType.ReplayState)
{
    throw new InvalidOperationException("unexpected replay event ordering");
}

connector.FlushRecoveryState();
connector.Stop();

Console.WriteLine("dotnet abi smoke passed");
