#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  plaza2_aggr20_md_test_evidence.sh --bundle-root <dir> --profile <yaml> --secret-env-file <file> --output-dir <dir>
                                    --armed-test-network --armed-test-session --armed-test-plaza2
                                    --armed-test-market-data [--max-polls <n>]
                                    [--git-sha <sha>] [--docker-image <image>]

Runs a bounded PLAZA II AGGR20 TEST market-data smoke and writes redacted evidence.
USAGE
}

bundle_root=""
profile=""
secret_env_file=""
output_dir=""
max_polls=512
git_sha="unknown"
docker_image="${MOEX_BUILD_IMAGE:-unknown}"
armed_network=false
armed_session=false
armed_plaza2=false
armed_market_data=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --bundle-root) bundle_root="${2:-}"; shift 2 ;;
    --profile) profile="${2:-}"; shift 2 ;;
    --secret-env-file) secret_env_file="${2:-}"; shift 2 ;;
    --output-dir) output_dir="${2:-}"; shift 2 ;;
    --max-polls) max_polls="${2:-}"; shift 2 ;;
    --git-sha) git_sha="${2:-}"; shift 2 ;;
    --docker-image) docker_image="${2:-}"; shift 2 ;;
    --armed-test-network) armed_network=true; shift ;;
    --armed-test-session) armed_session=true; shift ;;
    --armed-test-plaza2) armed_plaza2=true; shift ;;
    --armed-test-market-data) armed_market_data=true; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ -z "$bundle_root" || -z "$profile" || -z "$secret_env_file" || -z "$output_dir" ]]; then
  echo "--bundle-root, --profile, --secret-env-file, and --output-dir are required" >&2
  usage >&2
  exit 2
fi

if [[ "$armed_network" != true || "$armed_session" != true || "$armed_plaza2" != true || "$armed_market_data" != true ]]; then
  echo "all AGGR20 TEST arm flags are required" >&2
  exit 2
fi

case "$max_polls" in
  ''|*[!0-9]*)
    echo "--max-polls must be a positive integer" >&2
    exit 2
    ;;
esac

runner="$bundle_root/build-docker-linux/apps/moex_plaza2_aggr20_md_runner"
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
if [[ -z "${MOEX_PLAZA2_CGATE_SOFTWARE_KEY:-}" ]]; then
  echo "MOEX_PLAZA2_CGATE_SOFTWARE_KEY is not set by secret env file" >&2
  exit 1
fi

cat > "$output_dir/startup.json" <<EOF
{
  "phase": "5D",
  "profile": "$profile",
  "git_sha": "$git_sha",
  "docker_image": "$docker_image",
  "software_key": "[REDACTED]"
}
EOF

cat > "$output_dir/run_manifest.json" <<EOF
{
  "phase": "5D",
  "bounded_by": "max_polls",
  "max_polls": "$max_polls",
  "public_stream": "FORTS_AGGR20_REPL",
  "deferred_streams": "FORTS_ORDLOG_REPL,FORTS_ORDBOOK_REPL,FORTS_DEALS_REPL"
}
EOF

set +e
"$runner" \
  --profile "$profile" \
  --output-dir "$output_dir" \
  --armed-test-network \
  --armed-test-session \
  --armed-test-plaza2 \
  --armed-test-market-data \
  --max-polls "$max_polls" > "$operator_log" 2>&1
runner_status=$?
set -e

sed -i.bak -E \
  -e 's/(password|passwd|pwd|secret|token|credential|key)=([^ ;]+)/\1=[REDACTED]/Ig' \
  -e 's/MOEX_PLAZA2_TEST_CREDENTIALS=[^ ;]+/MOEX_PLAZA2_TEST_CREDENTIALS=[REDACTED]/g' \
  -e 's/MOEX_PLAZA2_CGATE_SOFTWARE_KEY=[^ ;]+/MOEX_PLAZA2_CGATE_SOFTWARE_KEY=[REDACTED]/g' \
  "$operator_log"
rm -f "$operator_log.bak"

summary="$(find "$output_dir" -maxdepth 1 -type f -name '*.aggr20.summary.json' | sort | head -n 1)"
if [[ -n "$summary" ]]; then
  cp "$summary" "$output_dir/final_health.json"
else
  cat > "$output_dir/final_health.json" <<EOF
{
  "result": "failed",
  "last_error": "runner did not produce AGGR20 summary"
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
        "source": "moex_plaza2_aggr20_md_test_runner",
        "runtime_probe_ok": health.get("runtime_probe_ok", "false"),
        "runner_state": health.get("runner_state", "Unknown"),
        "last_error": health.get("last_error", ""),
    },
)
write(
    "scheme_drift.json",
    {
        "source": "moex_plaza2_aggr20_md_test_runner",
        "scheme_drift_ok": health.get("scheme_drift_ok", "false"),
        "scheme_drift_status": health.get("scheme_drift_status", "Unknown"),
        "scheme_drift_warning_count": health.get("scheme_drift_warning_count", "0"),
        "scheme_drift_fatal_count": health.get("scheme_drift_fatal_count", "0"),
        "last_error": health.get("last_error", ""),
    },
)
write(
    "readiness.json",
    {
        "source": "moex_plaza2_aggr20_md_test_runner",
        "ready": health.get("ready", "false"),
        "stream_opened": health.get("stream_opened", "false"),
        "stream_online": health.get("stream_online", "false"),
        "stream_snapshot_complete": health.get("stream_snapshot_complete", "false"),
        "failure_classification": health.get("failure_classification", ""),
        "last_error": health.get("last_error", ""),
    },
)
PY

if [[ "$runner_status" -ne 0 ]]; then
  echo "PLAZA II AGGR20 TEST evidence run failed; inspect redacted artifacts in $output_dir" >&2
  exit "$runner_status"
fi

echo "PLAZA II AGGR20 TEST evidence written to $output_dir"
