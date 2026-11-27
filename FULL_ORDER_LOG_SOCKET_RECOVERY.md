# Writer command socket recovery

The writer constructs CgateTradingHost before CommandSocket. EventJournal acquires exclusive locks on both journal and identity state during that construction. A second same-state writer fails before touching the command socket.

The command endpoint already checked ownership, socket type, ECONNREFUSED and unchanged device/inode before unlinking a stale socket. This change additionally requires explicit proof that the host holds the exclusive identity state lock. Standalone socket construction cannot remove an existing stale endpoint without that proof. Live, unresponsive, foreign-owned, non-socket and symlink endpoints remain refused.

The writer itself removes the proven stale endpoint and binds its replacement; no operator unlink helper is part of startup. A launcher must poll a successful status response from the new process, not just test whether the socket pathname exists. A stale pathname can exist before the new owner binds it. Never remove the state lock or reset journal/state to make startup work.

Regression coverage: the actual FOL executable against fake CGate is killed after status readiness, leaving its socket behind. A same-journal/state restart must recover command access and quit with zero order posts. A competing live owner must retain its socket unchanged. Socket tests also refuse cleanup without identity ownership and retain active endpoints. This is offline coverage, not authorization for a live crash test. Any native SIGKILL still requires the owner's explicit message naming SIGKILL.
