# Broker Topology

Phase 0 treats broker topology as a first-class configuration dimension.

Tracked fields:

- direct-to-MOEX vs broker-proxied order flow
- broker synchronous risk-check placement
- market-data origin: native exchange vs broker redistribution
- P2MQRouter location
- local vs remote CGate runtime

The runtime must log the discovered or configured topology at startup.
