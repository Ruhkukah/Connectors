# Friday short TEST session: operator contingency

The private deployment card on TEST contains the exact recorded router
argv/environment/cwd and the actual CLI/socket commands. It is private, mode
0600, and must stay on TEST. No credentials belong in this repository or reports.
The card is prepared at sealing; preparing it does not execute recovery.

Friday Day orders belong to Monday's session. **Friday expiry is not a cleanup
backstop.** Remain reachable during the short session and review its terminal
receipt. Stop new exposure on any uncertain order outcome.

1. If the verified product is alive, send `kill on`, then one `cancel-all ISIN`
   for each freshly resolved instrument through actual moexctl. This risk-reducing
   attempt must not depend on having all four upstreams restored. Verify matching
   successful replies, later committed TRADE and complete reconstructed inventory.
   A socket acknowledgment alone is not cancellation confirmation.
2. If the router is down, first establish its exact owned identity/absence and
   that the fault worker has finished. Use the recorded private launch command
   with its original argv, environment and cwd under the canonical fault/launch
   locks. Never launch beside an unknown or still-running router. Never SIGKILL.
3. If the planned replacement cannot be attached, cancellation may use that same
   already-started, exactly verified child. There is no automatic third live
   startup: the ID budget permits only the initial start and planned restart.
   `orders_working_no_product` means cancellation and flatness are UNKNOWN. Do not
   reset/reuse IDs, relax the capacity gate or assume orders expired.
4. If venue cancellation cannot be proved, use the owner's authenticated broker
   emergency channel and escalate the TEST/router problem to MOEX support:
   **help@moex.com, +7 495 363-32-32**, verified on the
   [official MOEX contact page](https://www.moex.com/s3439). Supply the instance,
   section, timestamps and sanitized errors; no credentials or software key.
5. A one-shot **15:10 MSK** publisher-free independent check is scheduled after
   the session, before Friday's **23:50** close. It checks only after the owned
   product has stopped, no CGate clients remain and the canonical fault lock is
   free. It proves eight complete streams, zero working orders/positions, exit 0,
   zero clients and four router upstreams. It never sends orders or recovery
   commands. Nonflat or incomplete evidence is an action-required alarm; an
   absent probe is not flatness. The old app timer remains paused.

Preserve the initiating failure, all attempted cleanup receipts and the original
BLOCKED/FAIL verdict. Successful later reconciliation does not erase them. The
native rate-window correction is deferred until after this session and before
certification submission; the report records both strict and 1 ms slack counts.
