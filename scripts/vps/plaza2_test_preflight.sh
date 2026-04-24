#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  plaza2_test_preflight.sh --bundle-root <dir> --profile <yaml> --secret-env-file <file> --output-dir <dir>

Performs offline VPS-side checks before a bounded PLAZA II TEST run.
It verifies binaries, profile file, and credential variable presence without
printing credential values.
USAGE
}

bundle_root=""
profile=""
secret_env_file=""
output_dir=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --bundle-root)
      bundle_root="${2:-}"
      shift 2
      ;;
    --profile)
      profile="${2:-}"
      shift 2
      ;;
    --secret-env-file)
      secret_env_file="${2:-}"
      shift 2
      ;;
    --output-dir)
      output_dir="${2:-}"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "$bundle_root" || -z "$profile" || -z "$secret_env_file" || -z "$output_dir" ]]; then
  echo "--bundle-root, --profile, --secret-env-file, and --output-dir are required" >&2
  usage >&2
  exit 2
fi

runner="$bundle_root/build-docker-linux/apps/moex_plaza2_test_runner"
wrapper="$bundle_root/build-docker-linux/apps/moex_plaza2_cert_runner"

for path in "$runner" "$wrapper" "$profile" "$secret_env_file"; do
  if [[ ! -e "$path" ]]; then
    echo "required path missing: $path" >&2
    exit 1
  fi
done

mkdir -p "$output_dir"
set +o history
# shellcheck disable=SC1090
source "$secret_env_file"
set -o history

if [[ -z "${MOEX_PLAZA2_TEST_CREDENTIALS:-}" ]]; then
  echo "MOEX_PLAZA2_TEST_CREDENTIALS is not set by secret env file" >&2
  exit 1
fi

"$runner" --help >/dev/null 2>&1 || true
"$wrapper" --help >/dev/null 2>&1 || true

cat > "$output_dir/preflight.json" <<EOF
{
  "phase": "4D",
  "profile_present": true,
  "runner_present": true,
  "wrapper_present": true,
  "credential_env_present": true,
  "credential_value": "[REDACTED]"
}
EOF

echo "preflight ok: $output_dir/preflight.json"
