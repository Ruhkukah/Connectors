#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  plaza2_repl_test_evidence.sh --bundle-root <dir> --profile <yaml> --secret-env-file <file> --output-dir <dir>
                                --armed-test-network --armed-test-session --armed-test-plaza2
                                [--max-polls <n>] [--git-sha <sha>] [--docker-image <image>]
                                [--deploy-package-sha256 <sha>] [--cgate-archive <file>]
                                [--cgate-archive-sha256 <sha>] [--cgate-install-dir <dir>]

Runs a bounded PLAZA II TEST read-side smoke and writes redacted evidence.
USAGE
}

bundle_root=""
profile=""
secret_env_file=""
output_dir=""
max_polls=256
git_sha="unknown"
docker_image="${MOEX_BUILD_IMAGE:-unknown}"
deploy_package_sha256="unknown"
cgate_archive=""
cgate_archive_sha256="unknown"
cgate_install_dir=""
armed_network=false
armed_session=false
armed_plaza2=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --bundle-root) bundle_root="${2:-}"; shift 2 ;;
    --profile) profile="${2:-}"; shift 2 ;;
    --secret-env-file) secret_env_file="${2:-}"; shift 2 ;;
    --output-dir) output_dir="${2:-}"; shift 2 ;;
    --max-polls) max_polls="${2:-}"; shift 2 ;;
    --git-sha) git_sha="${2:-}"; shift 2 ;;
    --docker-image) docker_image="${2:-}"; shift 2 ;;
    --deploy-package-sha256) deploy_package_sha256="${2:-}"; shift 2 ;;
    --cgate-archive) cgate_archive="${2:-}"; shift 2 ;;
    --cgate-archive-sha256) cgate_archive_sha256="${2:-}"; shift 2 ;;
    --cgate-install-dir) cgate_install_dir="${2:-}"; shift 2 ;;
    --armed-test-network) armed_network=true; shift ;;
    --armed-test-session) armed_session=true; shift ;;
    --armed-test-plaza2) armed_plaza2=true; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ -z "$bundle_root" || -z "$profile" || -z "$secret_env_file" || -z "$output_dir" ]]; then
  echo "--bundle-root, --profile, --secret-env-file, and --output-dir are required" >&2
  usage >&2
  exit 2
fi

if [[ "$armed_network" != true || "$armed_session" != true || "$armed_plaza2" != true ]]; then
  echo "all PLAZA II TEST arm flags are required" >&2
  exit 2
fi

case "$max_polls" in
  ''|*[!0-9]*)
    echo "--max-polls must be a positive integer" >&2
    exit 2
    ;;
esac

runner="$bundle_root/build-docker-linux/apps/moex_plaza2_cert_runner"
if [[ ! -x "$runner" ]]; then
  echo "runner missing or not executable: $runner" >&2
  exit 1
fi
if [[ ! -f "$profile" || ! -f "$secret_env_file" ]]; then
  echo "profile or secret env file missing" >&2
  exit 1
fi

mkdir -p "$output_dir"
operator_log="$output_dir/operator.log"

set +o history
# shellcheck disable=SC1090
source "$secret_env_file"
set -o history
if [[ -z "${MOEX_PLAZA2_TEST_CREDENTIALS:-}" ]]; then
  echo "MOEX_PLAZA2_TEST_CREDENTIALS is not set by secret env file" >&2
  exit 1
fi

cat > "$output_dir/startup.json" <<EOF
{
  "phase": "4D",
  "git_sha": "$git_sha",
  "docker_image": "$docker_image",
  "deploy_package_sha256": "$deploy_package_sha256",
  "credential_value": "[REDACTED]",
  "profile": "$profile"
}
EOF

cat > "$output_dir/run_manifest.json" <<EOF
{
  "phase": "4D",
  "bounded_by": "max_polls",
  "max_polls": "$max_polls",
  "cgate_distributive": "$(basename "${cgate_archive:-unknown}")",
  "cgate_distributive_sha256": "$cgate_archive_sha256",
  "cgate_install_dir": "$cgate_install_dir"
}
EOF

set +e
"$runner" \
  --profile "$profile" \
  --output-dir "$output_dir" \
  --armed-test-network \
  --armed-test-session \
  --armed-test-plaza2 \
  --max-polls "$max_polls" > "$operator_log" 2>&1
runner_status=$?
set -e

sed -i.bak -E \
  -e 's/(password|passwd|pwd|secret|token|credential|key)=([^ ;]+)/\1=[REDACTED]/Ig' \
  -e 's/MOEX_PLAZA2_TEST_CREDENTIALS=[^ ;]+/MOEX_PLAZA2_TEST_CREDENTIALS=[REDACTED]/g' \
  "$operator_log"
rm -f "$operator_log.bak"

summary="$(find "$output_dir" -maxdepth 1 -type f -name '*.summary.json' | sort | head -n 1)"
if [[ -n "$summary" ]]; then
  cp "$summary" "$output_dir/final_health.json"
else
  cat > "$output_dir/final_health.json" <<EOF
{
  "result": "failed",
  "last_error": "runner did not produce summary"
}
EOF
fi

python3 - "$output_dir/final_health.json" "$output_dir" <<'PY'
import json
import sys
from pathlib import Path

final_health = Path(sys.argv[1])
output_dir = Path(sys.argv[2])
health = json.loads(final_health.read_text(encoding="utf-8"))

def write(name: str, payload: dict) -> None:
    (output_dir / name).write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")

write(
    "runtime_probe.json",
    {
        "source": "moex_plaza2_test_runner",
        "runtime_probe_ok": health.get("runtime_probe_ok", "false"),
        "runner_state": health.get("runner_state", "Unknown"),
        "last_error": health.get("last_error", ""),
    },
)
write(
    "scheme_drift.json",
    {
        "source": "moex_plaza2_test_runner",
        "scheme_drift_ok": health.get("scheme_drift_ok", "false"),
        "scheme_drift_status": health.get("scheme_drift_status", "Unknown"),
        "scheme_drift_warning_count": health.get("scheme_drift_warning_count", "0"),
        "scheme_drift_fatal_count": health.get("scheme_drift_fatal_count", "0"),
        "scheme_drift_warning_tables": health.get("scheme_drift_warning_tables", ""),
        "scheme_drift_fatal_tables": health.get("scheme_drift_fatal_tables", ""),
        "last_scheme_drift_warning": health.get("last_scheme_drift_warning", ""),
        "last_scheme_drift_fatal": health.get("last_scheme_drift_fatal", ""),
        "last_error": health.get("last_error", ""),
    },
)
write(
    "readiness.json",
    {
        "source": "moex_plaza2_test_runner",
        "ready": health.get("ready", "false"),
        "runner_state": health.get("runner_state", "Unknown"),
        "counts": {
            "sessions": health.get("session_count", "0"),
            "instruments": health.get("instrument_count", "0"),
            "matching_map_rows": health.get("matching_map_count", "0"),
            "limits": health.get("limit_count", "0"),
            "positions": health.get("position_count", "0"),
            "own_orders": health.get("own_order_count", "0"),
            "own_trades": health.get("own_trade_count", "0"),
        },
        "last_error": health.get("last_error", ""),
        "operator_warning": (
            "P2MQRouter listens on all interfaces. Verify provider firewall or host firewall restricts access to "
            "trusted hosts only."
        ),
    },
)
PY

if [[ "$runner_status" -ne 0 ]]; then
  echo "PLAZA II TEST evidence run failed; inspect redacted artifacts in $output_dir" >&2
  exit "$runner_status"
fi

echo "PLAZA II TEST evidence written to $output_dir"
