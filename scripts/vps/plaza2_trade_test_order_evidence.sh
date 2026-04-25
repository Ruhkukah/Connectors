#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  plaza2_trade_test_order_evidence.sh --bundle-root <dir> --profile <yaml> --secret-env-file <file>
                                     --output-dir <dir> --armed-test-network --armed-test-session
                                     --armed-test-plaza2 --armed-test-order-entry --armed-test-tiny-order
                                     (--dry-run | --send-test-order) [--max-polls <n>]

Runs a bounded PLAZA II TEST AddOrder/DelOrder bring-up path and writes redacted evidence.
USAGE
}

bundle_root=""
profile=""
secret_env_file=""
output_dir=""
max_polls=512
armed_network=false
armed_session=false
armed_plaza2=false
armed_order_entry=false
armed_tiny_order=false
send_test_order=false
dry_run=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --bundle-root) bundle_root="${2:-}"; shift 2 ;;
    --profile) profile="${2:-}"; shift 2 ;;
    --secret-env-file) secret_env_file="${2:-}"; shift 2 ;;
    --output-dir) output_dir="${2:-}"; shift 2 ;;
    --max-polls) max_polls="${2:-}"; shift 2 ;;
    --armed-test-network) armed_network=true; shift ;;
    --armed-test-session) armed_session=true; shift ;;
    --armed-test-plaza2) armed_plaza2=true; shift ;;
    --armed-test-order-entry) armed_order_entry=true; shift ;;
    --armed-test-tiny-order) armed_tiny_order=true; shift ;;
    --send-test-order) send_test_order=true; shift ;;
    --dry-run) dry_run=true; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ -z "$bundle_root" || -z "$profile" || -z "$secret_env_file" || -z "$output_dir" ]]; then
  echo "--bundle-root, --profile, --secret-env-file, and --output-dir are required" >&2
  usage >&2
  exit 2
fi

if [[ "$armed_network" != true || "$armed_session" != true || "$armed_plaza2" != true ||
      "$armed_order_entry" != true || "$armed_tiny_order" != true ]]; then
  echo "all PLAZA II TEST order-entry arm flags are required" >&2
  exit 2
fi

if [[ "$send_test_order" == true && "$dry_run" == true ]]; then
  echo "--send-test-order and --dry-run are mutually exclusive" >&2
  exit 2
fi
if [[ "$send_test_order" != true ]]; then
  dry_run=true
fi

case "$max_polls" in
  ''|*[!0-9]*)
    echo "--max-polls must be a positive integer" >&2
    exit 2
    ;;
esac

runner="$bundle_root/build-docker-linux/apps/moex_plaza2_trade_test_order_runner"
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

set +e
command=(
  "$runner"
  --profile "$profile"
  --output-dir "$output_dir"
  --armed-test-network
  --armed-test-session
  --armed-test-plaza2
  --armed-test-order-entry
  --armed-test-tiny-order
  --max-polls "$max_polls"
)
if [[ "$send_test_order" == true ]]; then
  command+=(--send-test-order)
else
  command+=(--dry-run)
fi
"${command[@]}" > "$operator_log" 2>&1
runner_status=$?
set -e

sed -i.bak -E \
  -e 's/(password|passwd|pwd|secret|token|credential|key)=([^ ;]+)/\1=[REDACTED]/Ig' \
  -e 's/MOEX_PLAZA2_TEST_CREDENTIALS=[^ ;]+/MOEX_PLAZA2_TEST_CREDENTIALS=[REDACTED]/g' \
  -e 's/MOEX_PLAZA2_CGATE_SOFTWARE_KEY=[^ ;]+/MOEX_PLAZA2_CGATE_SOFTWARE_KEY=[REDACTED]/g' \
  "$operator_log"
rm -f "$operator_log.bak"

if [[ "$runner_status" -ne 0 ]]; then
  echo "PLAZA II TEST order-entry evidence run failed; inspect redacted artifacts in $output_dir" >&2
  exit "$runner_status"
fi

echo "PLAZA II TEST order-entry evidence written to $output_dir"
