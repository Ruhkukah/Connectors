# TWIME SBE Offline Module

This module contains the offline TWIME schema inventory, deterministic code generation, fixed-layout codec, frame assembler, and certification-style log formatter for the pinned `twime_spectra-7.7.xml` schema.

It intentionally does **not** implement:

- sockets
- session state machines
- live connectivity
- credentials
- order routing through the public C ABI

The active schema lives in `protocols/twime_sbe/schema/` and is materialized only from the local `spec-lock` cache.
