# Event Backpressure and Loss Policy

Private streams:

- private trades: lossless, never silently drop
- private order status: lossless, never silently drop
- full order log: lossless, never silently drop

Public streams:

- public L1: configurable `drop_oldest`
- diagnostics: configurable `drop_oldest`

Every queue or ring must expose:

- `produced`
- `polled`
- `dropped`
- `high_watermark`
- overflow state
