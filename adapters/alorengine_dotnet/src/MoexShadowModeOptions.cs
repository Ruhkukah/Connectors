namespace MoexConnector.AlorEngine;

public sealed record MoexShadowModeOptions(
    bool Enabled,
    bool NativeReadOnly,
    bool CompareOrderBooks,
    bool CompareTrades,
    bool ComparePositions,
    bool CompareInstrumentMapping,
    bool CompareStatusTransitions);
