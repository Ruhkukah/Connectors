# Signed AGGR price execution follow-up

The read-only AGGR20 correction treats `orders_aggr.price` as the generated
SPECTRA `d16.5` contract: precision 16, scale 5, and exact signed integer units
at scale `100000`. The decoder, fake CGate runtime, and authority probe may
therefore preserve negative, zero, and positive finite values without
rounding. This does not authorize an order, publisher, or command API.

Execution-side support remains a separate qualification item. Before any
signed-price order path is considered, reconcile the following against fresh
REFDATA and the command schema for the specific instrument and session:

1. Whether the venue/session permits negative or zero limit prices, and which
   bounds and tick-size rules apply.
2. Exact signed `d16.5` parsing and serialization through the order command,
   including overflow and scale guards.
3. Side-aware tick rounding around zero, passive/non-marketable checks, and
   deep-passive boundary calculations for negative prices.
4. Risk, margin, P&L, persistence, logging, and operator-review behavior for
   signed prices.
5. A separately authorized TEST execution canary with no automatic send or
   production promotion.

The current execution-side positive-price assumptions are intentionally not
changed by this read-only foundation correction.
