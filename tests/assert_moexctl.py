"""Offline executable -> host -> fake CGate acceptance. Never loads a vendor library."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    cli, fixture_builder, fake_library = map(Path, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="moexctl-") as temporary:
        root = Path(temporary)
        subprocess.run([fixture_builder, fake_library, root], check=True)
        env = dict(os.environ, HOST_TEST_ENV="ini=config/t1.ini;key=00000000",
                   HOST_TEST_BROKER="BRK1", HOST_TEST_CLIENT="C01",
                   MOEX_PLAZA2_TEST_CREDENTIALS="privacy-sentinel",
                   MOEX_PLAZA2_CGATE_SOFTWARE_KEY="00000000",
                   MOEX_FAKE_ZERO_POSITION="1", MOEX_FAKE_MISSING_ORDER="1",
                   MOEX_FAKE_CLIENT_CODE="BRK1C01", MOEX_FAKE_PUB_REPLY_ORDER_ID="20003")

        def run(args, expected=0):
            result = subprocess.run([cli, *args], cwd=root, env=env, text=True,
                                    capture_output=True, timeout=15)
            assert result.returncode == expected, (result.returncode, result.stdout, result.stderr)
            assert "privacy-sentinel" not in result.stdout + result.stderr
            return result.stdout

        assert "order-test" in run(["--help"])
        run(["invalid"], 2)
        run(["plaza2", "order-test"], 2)
        run(["plaza2", "qualify", "--armed-test-order-send"], 2)
        base = ["--runtime-root", str(root), "--scheme-dir", str(root / "scheme"),
                "--config-dir", str(root / "config"), "--env-settings-var", "HOST_TEST_ENV",
                "--broker-code-env", "HOST_TEST_BROKER", "--client-code-env", "HOST_TEST_CLIENT",
                "--isin-id", "1001", "--session-id", "321", "--expected-release", "SPECTRA93",
                "--armed-test-network", "--armed-test-session", "--armed-test-plaza2",
                "--wait-ms", "500", "--json"]
        for command in ("status", "qualify"):
            value = json.loads(run(["plaza2", command, *base]))
            assert value["schema"] == "moex.connector-host.v1"
            assert value["observation_ready"] is True
            assert value["cg_pub_msgnew"] == value["cg_pub_post"] == 0
            assert value["trade_replay_complete"] is True
            assert value["trade_anchor"]["trades_rev"] == value["pos_trades_rev"]
            assert len(value["streams"]) == 7
            assert all(row["online"] and row["snapshot_complete"] for row in value["streams"])
            assert all(row["lifenum"] == value["refdata_lifenum"] for row in value["target_provenance"])
        env["MOEX_FAKE_CANCEL_AFTER_DEL"] = "1"
        order = ["--armed-test-order-send", "--plan", str(root / "canonical_plan.json"),
                 "--authorize-sha256", (root / "plan.sha256").read_text(),
                 "--profile-id", "offline-plaza2-test", "--profile-fingerprint", "e" * 64,
                 "--run-id", "cli-order-test", "--journal-root", str(root / "cli-journals"),
                 "--receipt-path", str(root / "cli-receipt.json"), "--base-contract", "RTS",
                 "--side", "sell", "--price", "103000", "--ext-id", "79", "--add-user-id", "701",
                 "--cancel-user-id", "702", "--recovery-user-id", "703"]
        value = json.loads(run(["plaza2", "order-test", *base, *order]))
        assert value["lifecycle_state"] == "cancelled"
        assert value["market_safe"] and value["evidence_consistent"]
        assert value["cg_pub_msgnew"] == value["cg_pub_post"] == 2
        assert (root / "cli-receipt.json").is_file()
        env["MOEX_FAKE_AGGR_CROSSED"] = "1"
        value = json.loads(run(["plaza2", "qualify", *base], 4))
        assert not value["observation_ready"] and not value["target_aggr20_uncrossed"]
        assert value["cg_pub_msgnew"] == value["cg_pub_post"] == 0
    print("moexctl offline acceptance passed")


if __name__ == "__main__":
    main()
