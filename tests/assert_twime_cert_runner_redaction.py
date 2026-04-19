#!/usr/bin/env python3
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: assert_twime_cert_runner_redaction.py <runner> <profile> <output-dir> <credential-env-var>"
        )

    runner = Path(sys.argv[1]).resolve()
    profile = Path(sys.argv[2]).resolve()
    output_dir = Path(sys.argv[3]).resolve()
    credential_env_var = sys.argv[4]
    profile_id = "test_twime_tcp_external_fixture"
    raw_secret = "TWIME-SUPER-SECRET"

    env = dict(os.environ)
    env[credential_env_var] = raw_secret

    subprocess.run(
        [
            str(runner),
            "--profile",
            str(profile),
            "--output-dir",
            str(output_dir),
            "--armed-test-network",
            "--validate-only",
        ],
        check=True,
        env=env,
    )

    log_path = output_dir / f"{profile_id}.cert.log"
    summary_path = output_dir / f"{profile_id}.summary.json"
    log_text = log_path.read_text(encoding="utf-8")
    summary_text = summary_path.read_text(encoding="utf-8")

    if raw_secret in log_text or raw_secret in summary_text:
        raise SystemExit("raw TWIME credential leaked into cert-runner output")
    if "[REDACTED]" not in log_text:
        raise SystemExit("redacted credential marker missing from cert-runner log output")
    if "[REDACTED_CREDENTIALS]" not in log_text:
        raise SystemExit("explicit redaction label missing from cert-runner log output")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
