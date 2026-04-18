using System.Text.Json;
using System.Text.Json.Serialization;
using MoexConnector.AlorEngine;

#if ALORENGINE_INTEGRATION
using System.Collections.Concurrent;
using System.Runtime.CompilerServices;
using System.Threading.Channels;
using AlorEngine.Dtc;
using AlorEngine.MarketData;
using AlorEngine.Storage;
using AlorEngine.Trading;
using DTC_PB;
#endif

if (args.Length != 4)
{
    throw new InvalidOperationException("expected native library path, replay fixture path, expected report path, and output report path");
}

var libraryPath = Path.GetFullPath(args[0]);
var replayPath = Path.GetFullPath(args[1]);
var expectedPath = Path.GetFullPath(args[2]);
var outputPath = Path.GetFullPath(args[3]);

Directory.CreateDirectory(Path.GetDirectoryName(outputPath)!);

#if !ALORENGINE_INTEGRATION
var missingReport = new
{
    success = false,
    reason = "ALORENGINE_PROJECT was not provided. Set /p:AlorEngineProject=/path/to/AlorEngine.csproj to enable the shadow replay harness."
};

File.WriteAllText(outputPath, JsonSerializer.Serialize(missingReport, new JsonSerializerOptions
{
    PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
    WriteIndented = true
}));
Console.WriteLine("shadow replay skipped: AlorEngine project path not configured");
return;
#else
if (!File.Exists(libraryPath))
{
    throw new FileNotFoundException("native library not found", libraryPath);
}

if (!File.Exists(replayPath))
{
    throw new FileNotFoundException("synthetic replay fixture not found", replayPath);
}

if (!File.Exists(expectedPath))
{
    throw new FileNotFoundException("expected diff baseline not found", expectedPath);
}

using var library = MoexNativeLibrary.Load(libraryPath);
using var connector = library.CreateConnectorClient("shadow-replay", "phase1");

var lowRateCallbacks = new List<CallbackRecord>();
connector.RegisterLowRateCallback(evt =>
{
    lowRateCallbacks.Add(new CallbackRecord(evt.Header.EventType.ToString(), evt.ReplayState.ToString(), evt.InfoText));
});
connector.LoadProfile("profiles/replay.yaml", armed: false);
connector.LoadSyntheticReplay(replayPath);
connector.Start();

var polledEvents = new List<MoexConnectorEvent>();
while (true)
{
    var batch = connector.PollEvents(2);
    if (batch.Count == 0)
    {
        break;
    }

    polledEvents.AddRange(batch);
}

var health = connector.GetHealth();
var backpressure = connector.GetBackpressureCounters();
connector.FlushRecoveryState();
connector.Stop();

var primaryTradingClient = new NoOpTradingClient();
var primaryRouter = new NoOpTradingClientRouter(primaryTradingClient);
var shadowReadOnlyClient = new ShadowReadOnlyTradingClient();
var router = new ShadowPassThroughTradingClientRouter(primaryRouter, shadowReadOnlyClient);

var instrumentCatalog = new ShadowInstrumentCatalog();
SeedInstrumentCatalog(instrumentCatalog, polledEvents);
var instrument = instrumentCatalog.All.Single();

var orderBookService = new ShadowOrderBookService();
var marketDataService = new ShadowMarketDataPublisherService();
var orderStatusBus = new OrderStatusBus();
var tradeExecutionBus = new TradeExecutionBus();
var positionBus = new PositionBus();

var orderStatuses = new List<OrderStatusRecord>();
var tradeExecutions = new List<TradeExecutionRecord>();
var positions = new List<PositionRecord>();
orderStatusBus.OnOrderStatus += evt => orderStatuses.Add(new OrderStatusRecord(
    evt.ServerOrderId,
    evt.ClientOrderId,
    evt.Symbol,
    evt.Status.ToString(),
    evt.FilledQuantity,
    evt.RemainingQuantity,
    evt.Price,
    evt.Info,
    FormatUtcNullable(evt.TransactionUtc),
    FormatUtcNullable(evt.EngineUtc)));
tradeExecutionBus.OnTradeExecution += evt => tradeExecutions.Add(new TradeExecutionRecord(
    evt.ServerOrderId,
    evt.ClientOrderId,
    evt.Symbol,
    evt.TradeAccount,
    evt.ExecutedQuantity,
    evt.CumulativeFilledQuantity,
    evt.ExecutionPrice,
    evt.AveragePrice,
    FormatUtcNullable(evt.ExecutionUtc),
    evt.Info));
positionBus.OnPosition += evt => positions.Add(new PositionRecord(
    evt.Portfolio,
    evt.Symbol,
    evt.Exchange,
    evt.Quantity,
    evt.AveragePrice,
    evt.OpenProfitLoss,
    FormatUtcNullable(evt.UpdateUtc)));

var orderBookSubscription = await orderBookService.SubscribeAsync(
    instrument.Key.Symbol,
    instrument.Key.Board,
    1,
    "shadow-connection",
    10,
    CancellationToken.None).ConfigureAwait(false);
var marketDataSubscription = await marketDataService.SubscribeAsync(
    instrument,
    "shadow-connection",
    _ => { },
    CancellationToken.None).ConfigureAwait(false);

var orderBookTask = CollectAsync(orderBookSubscription!.Updates, evt => new OrderBookRecord(
    evt.Symbol,
    evt.Board,
    evt.Side ? "bid" : "ask",
    evt.Level,
    evt.Price,
    evt.Quantity,
    evt.UpdateType,
            evt.Sequence,
            FormatUtcValue(evt.TsMoexUtc)));
var marketDataTask = CollectAsync(marketDataSubscription.Updates, evt => ToMarketEnvelopeRecord(evt));

var projector = new ShadowEventProjector(instrumentCatalog, orderBookService, marketDataService, orderStatusBus, tradeExecutionBus, positionBus);
await projector.ApplyAsync(polledEvents).ConfigureAwait(false);

orderBookService.Complete();
marketDataService.Complete();

var orderBookUpdates = await orderBookTask.ConfigureAwait(false);
var marketDataUpdates = await marketDataTask.ConfigureAwait(false);
var marketDataSnapshot = await marketDataService.SnapshotAsync(instrument, CancellationToken.None).ConfigureAwait(false);

var actual = new ShadowReplayReport(
    new RouterRecord(
        ReferenceEquals(router.CurrentTradingClient, primaryRouter.CurrentTradingClient),
        ReferenceEquals(router.LiveTradingClient, primaryRouter.LiveTradingClient),
        router.VirtualTradingEnabled,
        shadowReadOnlyClient.IsReadOnly),
    lowRateCallbacks,
    instrumentCatalog.All
        .OrderBy(item => item.Key.Symbol, StringComparer.Ordinal)
        .ThenBy(item => item.Key.Board, StringComparer.Ordinal)
        .Select(item => new InstrumentRecord(item.Key.Symbol, item.Key.Board, item.InstrumentGroup, item.PreferOrderBookL1))
        .ToList(),
    new OrderBookSection(
        orderBookSubscription.SnapshotEvents.Select(evt => new OrderBookRecord(
            evt.Symbol,
            evt.Board,
            evt.Side ? "bid" : "ask",
            evt.Level,
            evt.Price,
            evt.Quantity,
            evt.UpdateType,
            evt.Sequence,
            FormatUtcValue(evt.TsMoexUtc))).ToList(),
        orderBookUpdates),
    new MarketDataSection(
        new MarketDataSnapshotRecord(
            marketDataSnapshot.Sequence,
            marketDataSnapshot.FeedAvailable,
            marketDataSnapshot.LastTrade is null ? null : new MarketTradeRecord(
                marketDataSnapshot.LastTrade.TradeId,
                marketDataSnapshot.LastTrade.Key.Symbol,
                marketDataSnapshot.LastTrade.Key.Board,
                marketDataSnapshot.LastTrade.InstrumentGroup,
                marketDataSnapshot.LastTrade.Price,
                marketDataSnapshot.LastTrade.Quantity,
                marketDataSnapshot.LastTrade.AggressorSide.ToString(),
                marketDataSnapshot.LastTrade.Existing,
                marketDataSnapshot.LastTrade.SourceKind.ToString(),
                FormatUtcValue(marketDataSnapshot.LastTrade.TradeUtc),
                marketDataSnapshot.LastTrade.CaptureEpochId,
                marketDataSnapshot.LastTrade.CaptureSequence),
            marketDataSnapshot.L1 is null ? null : new L1Record(
                marketDataSnapshot.L1.Key.Symbol,
                marketDataSnapshot.L1.Key.Board,
                marketDataSnapshot.L1.BidPrice,
                marketDataSnapshot.L1.BidQuantity,
                marketDataSnapshot.L1.AskPrice,
                marketDataSnapshot.L1.AskQuantity,
                FormatUtcValue(marketDataSnapshot.L1.TimestampUtc),
                marketDataSnapshot.L1.Source)),
        marketDataUpdates),
    orderStatuses,
    tradeExecutions,
    positions,
    new HealthRecord(health.ConnectorState, health.ActiveProfileKind, health.ProdArmed, health.ShadowModeEnabled),
    new BackpressureRecord(backpressure.Produced, backpressure.Polled, backpressure.Dropped, backpressure.HighWatermark, backpressure.Overflowed));

var expected = JsonSerializer.Deserialize<ShadowReplayReport>(File.ReadAllText(expectedPath), JsonOptions())
               ?? throw new InvalidOperationException("failed to deserialize expected shadow replay baseline");

var actualJson = JsonSerializer.Serialize(actual, JsonOptions());
var expectedJson = JsonSerializer.Serialize(expected, JsonOptions());

var mismatches = new List<string>();
if (!string.Equals(actualJson, expectedJson, StringComparison.Ordinal))
{
    mismatches.Add("canonical_json_mismatch");
}

var diff = new ShadowReplayDiffReport(
    mismatches.Count == 0,
    expectedPath,
    replayPath,
    mismatches,
    actual);

File.WriteAllText(outputPath, JsonSerializer.Serialize(diff, JsonOptions()));

if (mismatches.Count != 0)
{
    throw new InvalidOperationException($"shadow replay diff mismatch: {string.Join(", ", mismatches)}");
}

Console.WriteLine("shadow replay diff passed");

static JsonSerializerOptions JsonOptions() => new()
{
    PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
    WriteIndented = true,
    DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull
};

static string? FormatUtcNullable(DateTime? value) => value?.ToUniversalTime().ToString("O");

static string FormatUtcValue(DateTime value) => value.ToUniversalTime().ToString("O");

static void SeedInstrumentCatalog(ShadowInstrumentCatalog catalog, IReadOnlyList<MoexConnectorEvent> events)
{
    foreach (var evt in events.Where(item => item.Header.EventType == MoexEventType.Instrument))
    {
        catalog.Upsert(new InstrumentDefinition(
            new InstrumentKey(evt.Symbol, evt.Board),
            string.IsNullOrWhiteSpace(evt.InstrumentGroup) ? evt.Board : evt.InstrumentGroup,
            evt.PreferOrderBookL1));
    }
}

static async Task<List<T>> CollectAsync<TSource, T>(IAsyncEnumerable<TSource> source, Func<TSource, T> projection)
{
    var output = new List<T>();
    await foreach (var item in source.ConfigureAwait(false))
    {
        output.Add(projection(item));
    }

    return output;
}

static MarketEnvelopeRecord ToMarketEnvelopeRecord(MarketDataEnvelope envelope) =>
    new(
        envelope.Sequence,
        envelope.Kind.ToString(),
        envelope.FeedStatus?.ToString(),
        envelope.Trade is null ? null : new MarketTradeRecord(
            envelope.Trade.TradeId,
            envelope.Trade.Key.Symbol,
            envelope.Trade.Key.Board,
            envelope.Trade.InstrumentGroup,
            envelope.Trade.Price,
            envelope.Trade.Quantity,
            envelope.Trade.AggressorSide.ToString(),
            envelope.Trade.Existing,
            envelope.Trade.SourceKind.ToString(),
            FormatUtcValue(envelope.Trade.TradeUtc),
            envelope.Trade.CaptureEpochId,
            envelope.Trade.CaptureSequence),
        envelope.L1 is null ? null : new L1Record(
            envelope.L1.Key.Symbol,
            envelope.L1.Key.Board,
            envelope.L1.BidPrice,
            envelope.L1.BidQuantity,
            envelope.L1.AskPrice,
            envelope.L1.AskQuantity,
            FormatUtcValue(envelope.L1.TimestampUtc),
            envelope.L1.Source));

file sealed record CallbackRecord(string EventType, string ReplayState, string InfoText);
file sealed record RouterRecord(bool CurrentIsPrimary, bool LiveIsPrimary, bool VirtualTradingEnabled, bool ShadowReadOnly);
file sealed record InstrumentRecord(string Symbol, string Board, string InstrumentGroup, bool PreferOrderBookL1);
file sealed record OrderBookRecord(string Symbol, string Board, string Side, int Level, float Price, int Quantity, int UpdateType, long Sequence, string? TsMoexUtc);
file sealed record OrderBookSection(IReadOnlyList<OrderBookRecord> Snapshot, IReadOnlyList<OrderBookRecord> Updates);
file sealed record MarketTradeRecord(long TradeId, string Symbol, string Board, string InstrumentGroup, double Price, int Quantity, string AggressorSide, bool Existing, string SourceKind, string? TradeUtc, string CaptureEpochId, long CaptureSequence);
file sealed record L1Record(string Symbol, string Board, float BidPrice, float BidQuantity, float AskPrice, float AskQuantity, string? TimestampUtc, string Source);
file sealed record MarketEnvelopeRecord(long Sequence, string Kind, string? FeedStatus, MarketTradeRecord? Trade, L1Record? L1);
file sealed record MarketDataSnapshotRecord(long Sequence, bool FeedAvailable, MarketTradeRecord? LastTrade, L1Record? L1);
file sealed record MarketDataSection(MarketDataSnapshotRecord Snapshot, IReadOnlyList<MarketEnvelopeRecord> Updates);
file sealed record OrderStatusRecord(string ServerOrderId, string ClientOrderId, string Symbol, string Status, double FilledQuantity, double RemainingQuantity, double Price, string? Info, string? TransactionUtc, string? EngineUtc);
file sealed record TradeExecutionRecord(string ServerOrderId, string ClientOrderId, string Symbol, string TradeAccount, double ExecutedQuantity, double CumulativeFilledQuantity, double ExecutionPrice, double AveragePrice, string? ExecutionUtc, string? Info);
file sealed record PositionRecord(string Portfolio, string Symbol, string Exchange, double Quantity, double AveragePrice, double? OpenProfitLoss, string? UpdateUtc);
file sealed record HealthRecord(uint ConnectorState, uint ActiveProfileKind, bool ProdArmed, bool ShadowModeEnabled);
file sealed record BackpressureRecord(ulong Produced, ulong Polled, ulong Dropped, ulong HighWatermark, bool Overflowed);
file sealed record ShadowReplayReport(
    RouterRecord Router,
    IReadOnlyList<CallbackRecord> LowRateCallbacks,
    IReadOnlyList<InstrumentRecord> Instruments,
    OrderBookSection OrderBook,
    MarketDataSection MarketData,
    IReadOnlyList<OrderStatusRecord> OrderStatuses,
    IReadOnlyList<TradeExecutionRecord> TradeExecutions,
    IReadOnlyList<PositionRecord> Positions,
    HealthRecord Health,
    BackpressureRecord Backpressure);
file sealed record ShadowReplayDiffReport(bool Success, string ExpectedPath, string ReplayPath, IReadOnlyList<string> Mismatches, ShadowReplayReport Actual);

file sealed class ShadowReadOnlyTradingClient : ITradingClient
{
    public bool IsReadOnly => true;

    public Task<SubmitResult> SubmitOrderAsync(SubmitOrderRequest req) =>
        Task.FromResult(new SubmitResult
        {
            Accepted = false,
            ClientOrderId = req.ClientOrderId ?? string.Empty,
            Status = OrderStatusEnum.OrderStatusRejected,
            InfoText = "MOEX native shadow mode is read-only."
        });

    public Task<CancelResult> CancelOrderAsync(CancelOrderRequest req) =>
        Task.FromResult(new CancelResult
        {
            Accepted = false,
            InfoText = "MOEX native shadow mode is read-only."
        });
}

file sealed class ShadowPassThroughTradingClientRouter : ITradingClientRouter
{
    public ShadowPassThroughTradingClientRouter(ITradingClientRouter primaryRouter, ShadowReadOnlyTradingClient shadowClient)
    {
        PrimaryRouter = primaryRouter;
        ShadowClient = shadowClient;
    }

    public ITradingClientRouter PrimaryRouter { get; }

    public ShadowReadOnlyTradingClient ShadowClient { get; }

    public ITradingClient CurrentTradingClient => PrimaryRouter.CurrentTradingClient;

    public ITradingClient LiveTradingClient => PrimaryRouter.LiveTradingClient;

    public ITradingClient? VirtualTradingClient => PrimaryRouter.VirtualTradingClient;

    public bool VirtualTradingEnabled => PrimaryRouter.VirtualTradingEnabled;
}

file sealed class ShadowInstrumentCatalog : IInstrumentCatalog
{
    private readonly Dictionary<InstrumentKey, InstrumentDefinition> _definitions = new();

    public IReadOnlyList<InstrumentDefinition> All =>
        _definitions.Values
            .OrderBy(item => item.Key.Symbol, StringComparer.Ordinal)
            .ThenBy(item => item.Key.Board, StringComparer.Ordinal)
            .ToList();

    public void Upsert(InstrumentDefinition definition)
    {
        _definitions[definition.Key] = definition;
    }

    public bool TryGet(InstrumentKey key, out InstrumentDefinition definition) => _definitions.TryGetValue(key, out definition!);

    public bool TryResolve(string symbol, string? boardHint, out InstrumentDefinition definition, out string rejectText)
    {
        definition = default!;
        rejectText = string.Empty;
        var key = new InstrumentKey(symbol, boardHint ?? string.Empty);
        if (_definitions.TryGetValue(key, out var resolved))
        {
            definition = resolved;
            return true;
        }

        rejectText = $"Instrument '{key}' is not registered in shadow catalog.";
        return false;
    }

    public bool TryResolveKey(string symbol, string? boardHint, out InstrumentKey key, out string rejectText)
    {
        key = new InstrumentKey(symbol, boardHint ?? string.Empty);
        if (_definitions.ContainsKey(key))
        {
            rejectText = string.Empty;
            return true;
        }

        rejectText = $"Instrument '{key}' is not registered in shadow catalog.";
        return false;
    }

    public bool IsConfigured(InstrumentKey key) => _definitions.ContainsKey(key);

    public string ResolveInstrumentGroup(InstrumentKey key) => _definitions.TryGetValue(key, out var definition) ? definition.InstrumentGroup : string.Empty;

    public IReadOnlyList<InstrumentDefinition> ForBoard(string board) =>
        _definitions.Values.Where(item => string.Equals(item.Key.Board, board, StringComparison.OrdinalIgnoreCase)).ToList();
}

file sealed class ShadowOrderBookService : IOrderBookService
{
    private sealed class SubscriberState
    {
        public required Channel<OrderbookUpdateEvent> Channel { get; init; }
    }

    private sealed class State
    {
        public List<OrderbookUpdateEvent> Snapshot { get; } = [];
        public Dictionary<string, SubscriberState> Subscribers { get; } = new(StringComparer.Ordinal);
    }

    private readonly Dictionary<(string Symbol, string Board), State> _states = new();

    public Task<OrderBookSubscription?> SubscribeAsync(string symbol, string exchange, uint symbolId, string connectionId, int levels, CancellationToken cancellationToken)
    {
        var state = GetOrCreate(symbol, exchange);
        var channel = Channel.CreateUnbounded<OrderbookUpdateEvent>(new UnboundedChannelOptions
        {
            SingleReader = true,
            SingleWriter = false
        });
        state.Subscribers[connectionId] = new SubscriberState { Channel = channel };
        return Task.FromResult<OrderBookSubscription?>(new OrderBookSubscription
        {
            SnapshotEvents = state.Snapshot.ToList(),
            Updates = ReadAllAsync(channel.Reader, cancellationToken)
        });
    }

    public Task UnsubscribeAsync(string symbol, string exchange, string connectionId, CancellationToken cancellationToken)
    {
        if (_states.TryGetValue((symbol.ToUpperInvariant(), exchange.ToUpperInvariant()), out var state) &&
            state.Subscribers.Remove(connectionId, out var subscriber))
        {
            subscriber.Channel.Writer.TryComplete();
        }

        return Task.CompletedTask;
    }

    public void Apply(OrderbookUpdateEvent evt)
    {
        var state = GetOrCreate(evt.Symbol, evt.Board);
        var index = state.Snapshot.FindIndex(item => item.Side == evt.Side && item.Level == evt.Level);
        if (index >= 0)
        {
            state.Snapshot[index] = evt;
        }
        else
        {
            state.Snapshot.Add(evt);
        }

        foreach (var subscriber in state.Subscribers.Values)
        {
            subscriber.Channel.Writer.TryWrite(evt);
        }
    }

    public void Complete()
    {
        foreach (var state in _states.Values)
        {
            foreach (var subscriber in state.Subscribers.Values)
            {
                subscriber.Channel.Writer.TryComplete();
            }
        }
    }

    private State GetOrCreate(string symbol, string board)
    {
        var key = (symbol.ToUpperInvariant(), board.ToUpperInvariant());
        if (!_states.TryGetValue(key, out var state))
        {
            state = new State();
            _states[key] = state;
        }

        return state;
    }

    private static async IAsyncEnumerable<OrderbookUpdateEvent> ReadAllAsync(
        ChannelReader<OrderbookUpdateEvent> reader,
        [EnumeratorCancellation] CancellationToken cancellationToken)
    {
        await foreach (var item in reader.ReadAllAsync(cancellationToken).ConfigureAwait(false))
        {
            yield return item;
        }
    }
}

file sealed class ShadowMarketDataPublisherService : IMarketDataPublisherService
{
    private sealed class SubscriberState
    {
        public required Channel<MarketDataEnvelope> Channel { get; init; }
        public required Action<string> OnOverflow { get; init; }
    }

    private sealed class State
    {
        public long Sequence { get; set; }
        public bool FeedAvailable { get; set; }
        public CommittedMarketTrade? LastTrade { get; set; }
        public InstrumentL1Snapshot? L1 { get; set; }
        public Dictionary<string, SubscriberState> Subscribers { get; } = new(StringComparer.Ordinal);
    }

    private readonly ConcurrentDictionary<InstrumentKey, State> _states = new();

    public Task<MarketDataSnapshotState> SnapshotAsync(InstrumentDefinition instrument, CancellationToken cancellationToken)
    {
        var state = GetOrCreate(instrument.Key);
        return Task.FromResult(new MarketDataSnapshotState(state.Sequence, state.FeedAvailable, state.LastTrade, state.L1));
    }

    public Task<MarketDataLiveSubscription> SubscribeAsync(InstrumentDefinition instrument, string connectionId, Action<string> onOverflow, CancellationToken cancellationToken)
    {
        var state = GetOrCreate(instrument.Key);
        var channel = Channel.CreateUnbounded<MarketDataEnvelope>(new UnboundedChannelOptions
        {
            SingleReader = true,
            SingleWriter = false
        });
        state.Subscribers[connectionId] = new SubscriberState { Channel = channel, OnOverflow = onOverflow };
        return Task.FromResult(new MarketDataLiveSubscription
        {
            Snapshot = new MarketDataSnapshotState(state.Sequence, state.FeedAvailable, state.LastTrade, state.L1),
            Updates = ReadAllAsync(channel.Reader, cancellationToken)
        });
    }

    public Task UnsubscribeAsync(InstrumentKey key, string connectionId, CancellationToken cancellationToken)
    {
        if (_states.TryGetValue(key, out var state) &&
            state.Subscribers.Remove(connectionId, out var subscriber))
        {
            subscriber.Channel.Writer.TryComplete();
        }

        return Task.CompletedTask;
    }

    public void ApplyL1(InstrumentL1Snapshot snapshot)
    {
        var state = GetOrCreate(snapshot.Key);
        if (!state.FeedAvailable)
        {
            state.FeedAvailable = true;
            state.Sequence++;
            Publish(state, new MarketDataEnvelope(state.Sequence, MarketDataEnvelopeKind.FeedStatus, MarketDataFeedStatusEnum.MarketDataFeedAvailable, null, null));
        }

        state.Sequence++;
        state.L1 = snapshot;
        Publish(state, new MarketDataEnvelope(state.Sequence, MarketDataEnvelopeKind.BidAsk, null, null, snapshot));
    }

    public void ApplyTrade(CommittedMarketTrade trade)
    {
        var state = GetOrCreate(trade.Key);
        state.Sequence++;
        state.LastTrade = trade;
        Publish(state, new MarketDataEnvelope(state.Sequence, MarketDataEnvelopeKind.Trade, null, trade, null));
    }

    public void Complete()
    {
        foreach (var state in _states.Values)
        {
            foreach (var subscriber in state.Subscribers.Values)
            {
                subscriber.Channel.Writer.TryComplete();
            }
        }
    }

    private State GetOrCreate(InstrumentKey key) => _states.GetOrAdd(key, _ => new State());

    private static void Publish(State state, MarketDataEnvelope envelope)
    {
        foreach (var subscriber in state.Subscribers.Values)
        {
            subscriber.Channel.Writer.TryWrite(envelope);
        }
    }

    private static async IAsyncEnumerable<MarketDataEnvelope> ReadAllAsync(
        ChannelReader<MarketDataEnvelope> reader,
        [EnumeratorCancellation] CancellationToken cancellationToken)
    {
        await foreach (var item in reader.ReadAllAsync(cancellationToken).ConfigureAwait(false))
        {
            yield return item;
        }
    }
}

file sealed class ShadowEventProjector
{
    private readonly ShadowInstrumentCatalog _catalog;
    private readonly ShadowOrderBookService _orderBookService;
    private readonly ShadowMarketDataPublisherService _marketDataService;
    private readonly IOrderStatusBus _orderStatusBus;
    private readonly ITradeExecutionBus _tradeExecutionBus;
    private readonly IPositionBus _positionBus;

    public ShadowEventProjector(
        ShadowInstrumentCatalog catalog,
        ShadowOrderBookService orderBookService,
        ShadowMarketDataPublisherService marketDataService,
        IOrderStatusBus orderStatusBus,
        ITradeExecutionBus tradeExecutionBus,
        IPositionBus positionBus)
    {
        _catalog = catalog;
        _orderBookService = orderBookService;
        _marketDataService = marketDataService;
        _orderStatusBus = orderStatusBus;
        _tradeExecutionBus = tradeExecutionBus;
        _positionBus = positionBus;
    }

    public Task ApplyAsync(IReadOnlyList<MoexConnectorEvent> events)
    {
        long orderBookSequence = 0;
        foreach (var evt in events)
        {
            switch (evt.Header.EventType)
            {
                case MoexEventType.Instrument:
                    _catalog.Upsert(new InstrumentDefinition(
                        new InstrumentKey(evt.Symbol, evt.Board),
                        string.IsNullOrWhiteSpace(evt.InstrumentGroup) ? evt.Board : evt.InstrumentGroup,
                        evt.PreferOrderBookL1));
                    break;

                case MoexEventType.PublicL1:
                    _marketDataService.ApplyL1(new InstrumentL1Snapshot(
                        new InstrumentKey(evt.Symbol, evt.Board),
                        (float)evt.Price,
                        (float)evt.Quantity,
                        (float)evt.SecondaryPrice,
                        (float)evt.SecondaryQuantity,
                        ToUtc(evt.Header.ExchangeTimeUtcNs) ?? DateTime.UnixEpoch,
                        "synthetic-shadow"));
                    break;

                case MoexEventType.PublicTrade:
                    _marketDataService.ApplyTrade(new CommittedMarketTrade(
                        new InstrumentKey(evt.Symbol, evt.Board),
                        string.IsNullOrWhiteSpace(evt.InstrumentGroup) ? evt.Board : evt.InstrumentGroup,
                        ParseTradeId(evt),
                        ToUtc(evt.Header.ExchangeTimeUtcNs) ?? DateTime.UnixEpoch,
                        evt.Price,
                        (int)evt.Quantity,
                        evt.Side == 1 ? BuySellEnum.Buy : evt.Side == 2 ? BuySellEnum.Sell : BuySellEnum.BuySellUnset,
                        null,
                        evt.Existing,
                        evt.Existing ? MarketTradeSourceKind.LiveWsExisting : MarketTradeSourceKind.LiveWs,
                        "shadow-replay",
                        (long)evt.Header.SourceSequence));
                    break;

                case MoexEventType.OrderBook:
                    orderBookSequence++;
                    _orderBookService.Apply(new OrderbookUpdateEvent
                    {
                        EventId = orderBookSequence,
                        Sequence = orderBookSequence,
                        TsMoexUtc = ToUtc(evt.Header.ExchangeTimeUtcNs) ?? DateTime.UnixEpoch,
                        TsAtWsReceiveUtc = ToUtc(evt.Header.SourceTimeUtcNs) ?? DateTime.UnixEpoch,
                        TsEngineUtc = ToUtc(evt.Header.ExchangeTimeUtcNs) ?? DateTime.UnixEpoch,
                        IngressTimestampMs = evt.Header.ExchangeTimeUtcNs > 0 ? evt.Header.ExchangeTimeUtcNs / 1_000_000 : 0,
                        EmitTimestampMs = evt.Header.ExchangeTimeUtcNs > 0 ? evt.Header.ExchangeTimeUtcNs / 1_000_000 : 0,
                        QueueDelayMs = 0,
                        EmitDelayMs = 0,
                        FinalUpdateInBatch = OrderbookUpdateEvent.FinalTrue,
                        Symbol = evt.Symbol,
                        Board = evt.Board,
                        Side = evt.Side == 1,
                        Level = evt.Level,
                        Price = (float)evt.Price,
                        Quantity = (int)evt.Quantity,
                        UpdateType = evt.UpdateType
                    });
                    break;

                case MoexEventType.OrderStatus:
                    _orderStatusBus.Publish(new OrderStatusEvent(
                        evt.ServerOrderId,
                        evt.ClientOrderId,
                        evt.Symbol,
                        evt.Status switch
                        {
                            MoexNativeOrderStatus.New => OrderLifecycleStatus.New,
                            MoexNativeOrderStatus.PartialFill => OrderLifecycleStatus.PartialFill,
                            MoexNativeOrderStatus.Filled => OrderLifecycleStatus.Filled,
                            MoexNativeOrderStatus.Canceled => OrderLifecycleStatus.Canceled,
                            MoexNativeOrderStatus.Rejected => OrderLifecycleStatus.Rejected,
                            _ => OrderLifecycleStatus.Rejected
                        },
                        evt.CumulativeQuantity,
                        evt.RemainingQuantity,
                        evt.Price,
                        evt.InfoText,
                        ToUtc(evt.Header.ExchangeTimeUtcNs),
                        ToUtc(evt.Header.SourceTimeUtcNs)));
                    break;

                case MoexEventType.PrivateTrade:
                    _tradeExecutionBus.Publish(new TradeExecutionEvent(
                        evt.ServerOrderId,
                        evt.ClientOrderId,
                        evt.Symbol,
                        evt.TradeAccount,
                        evt.Quantity,
                        evt.CumulativeQuantity,
                        evt.Price,
                        evt.AveragePrice,
                        ToUtc(evt.Header.ExchangeTimeUtcNs),
                        evt.InfoText));
                    break;

                case MoexEventType.Position:
                    _positionBus.Publish(new PositionStatusEvent(
                        evt.Portfolio,
                        evt.Symbol,
                        evt.Exchange,
                        evt.Quantity,
                        evt.AveragePrice,
                        evt.OpenProfitLoss,
                        ToUtc(evt.Header.ExchangeTimeUtcNs)));
                    break;
            }
        }

        return Task.CompletedTask;
    }

    private static DateTime? ToUtc(long utcNs) =>
        utcNs <= 0 ? null : DateTimeOffset.FromUnixTimeMilliseconds(utcNs / 1_000_000).UtcDateTime;

    private static long ParseTradeId(MoexConnectorEvent evt)
    {
        var digits = new string(evt.InfoText.Where(char.IsDigit).ToArray());
        return long.TryParse(digits, out var parsed) ? parsed : (long)evt.Header.SourceSequence;
    }
}
#endif
