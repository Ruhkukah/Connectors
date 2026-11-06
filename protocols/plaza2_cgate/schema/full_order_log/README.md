# Restricted Full Order Log client schemes

`online.ini` selects only `orders_log,heartbeat,sys_events` from group `ORDLOG`.
`snapshot.ini` selects only `orders,info` from group `OrdBook`.
The code consumes `orders_log`, `orders` and `info` for the book. `heartbeat` provides liveness on the same online stream when orders are quiet. `sys_events` provides exchange lifecycle diagnostics.

The official CGate 9.12.0.2144 `schemetool` generated both files offline using the published TEST scheme. The commands below ran from an isolated directory containing `schemetool` and `forts_scheme.ini` with the matching vendor library path. Exact commands and input and output hashes are in `manifest.json`.

```sh
./schemetool makesrc --output-format ini --tables orders_log,heartbeat,sys_events --output online.ini forts_scheme.ini ORDLOG
./schemetool makesrc --output-format ini --tables orders,info --output snapshot.ini forts_scheme.ini OrdBook
```

Source: https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/test/Scheme/9.12.0/forts_scheme.ini
Archive: https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/test/cgate_linux_amd64-9.12.0.2144.zip

The standalone 9.12 source bytes match the pinned archive member. The vendor source header says `Target poligon:prod` in the TEST distribution. The projection uses that published scheme. Native heartbeat delivery on T0 and T1 still needs verification against the actual listener.

Running the same offline commands with the matching CGate 9.9.1853 tool and its vendor source produced byte-identical output for both files.

Runtime `--scheme-dir` retains its full `forts_scheme.ini` for the shared probe and also contains `full_order_log/online.ini` and `full_order_log/snapshot.ini`. The filtered two-file directory alone is not a complete runtime scheme bundle. Do not change the certified AGGR scheme directory.

Generated SHA-256:
- online.ini: `012943b1d16a3c7973daa0e6119befc37265bf7046b232a1885c84019204ec25`
- snapshot.ini: `2479cfc9ca726a3303a9df88fa937a4c3f7c4e8f6743d4c81cdf6f3e6808ef44`
