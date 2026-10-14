"""Real native callback -> production journal -> committed admission observer.

The CGate runtime is the repository's offline fake. No network, publisher
commands or live trading session is permitted by this fixture.
"""
import json
import pathlib
import subprocess
import sys
import tempfile


def main():
    executable, fake_runtime, helpers = sys.argv[1:]
    sys.path.insert(0, helpers)
    from native_admission import CommittedAdmissionObserver

    with tempfile.TemporaryDirectory(prefix="moex-admission-journal-") as directory:
        root = pathlib.Path(directory)
        source = root / "native.ndjson"
        subprocess.run(
            [executable, fake_runtime, "--emit-admission-journal", str(source)],
            check=True,
            timeout=45,
        )
        data = source.read_bytes()
        assert data.endswith(b"\n"), "native fixture journal is incomplete"
        events = [json.loads(line) for line in data.splitlines()]
        assert not any(row["event"] in ("command", "trade") for row in events), \
            "offline admission fixture dispatched an order or credited a fill"
        assert any(
            row["event"] == "stream_row"
            and row["data"].get("stream") == "FORTS_INSTRUMENTSTATE_REPL"
            and row["data"].get("table") == "sys_events"
            for row in events
        ), "production journal dropped the required instrument-session marker"
        assert not any(
            row["event"] == "stream_row"
            and row["data"].get("table") in ("orders_aggr", "heartbeat")
            for row in events
        ), "admission repair restored high-volume suppressed rows"
        receipt = json.loads(pathlib.Path(str(source) + ".checkpoints.json").read_text())
        assert receipt["command_posts"] == 0, "native callback fixture posted a command"
        checkpoints = receipt["checkpoints"]
        assert len(checkpoints) >= 5, "fixture did not exercise admission transitions"
        replay = root / "incremental.ndjson"
        replay.touch()
        observer = CommittedAdmissionObserver(replay)
        previous = 0
        accepted = rejected = 0
        for checkpoint in checkpoints:
            end = checkpoint["journal_end"]
            assert previous < end <= len(data) and data[end - 1:end] == b"\n", \
                "native checkpoint is not a monotonic complete-record boundary"
            with replay.open("ab") as destination:
                destination.write(data[previous:end])
            observer.update()
            actual = observer.native_open(checkpoint["sess_id"], checkpoint["isin_ids"])
            assert checkpoint["native_order_entry_ready"] is checkpoint["expected_admitted"], \
                f"native reference disagrees at {checkpoint['stage']}"
            assert actual is checkpoint["expected_admitted"], \
                f"callback/journal/parser admission mismatch at {checkpoint['stage']}"
            accepted += actual
            rejected += not actual
            previous = end
        assert accepted >= 2 and rejected >= 2, "fixture lacks positive and negative transitions"
        print(json.dumps({
            "scope": "OFFLINE_FAKE_CGATE_CALLBACK_JOURNAL_PARSER",
            "checkpoints": len(checkpoints),
            "accepted": accepted,
            "refused": rejected,
            "trading_commands": 0,
        }))


if __name__ == "__main__":
    main()
