#!/usr/bin/env bash
# Execute only on the already-qualified Linux T1 evidence host. Never manages the router.
set -euo pipefail
umask 077

if [[ $# -lt 3 || $# -gt 4 ]]; then
    echo 'Usage: bash plaza2_day_observer.sh ABS_BINARY EXPECTED_BINARY_SHA NEW_EVIDENCE_DIR [SECONDS]' >&2
    exit 64
fi
observer_bin=$1
observer_sha=$2
observer_evidence=$3
observer_seconds=${4:-86400}
observer_source_sha=${MOEX_OBSERVER_SOURCE_SHA:?Set the reviewed observer source SHA}
[[ "$observer_source_sha" =~ ^[a-fA-F0-9]{40}$ ]]
[[ "$observer_bin" = /* && "$observer_evidence" = /* && "$observer_sha" =~ ^[a-f0-9]{64}$ ]]
[[ "$observer_seconds" =~ ^[0-9]+$ && "$observer_seconds" -ge 1 && "$observer_seconds" -le 604800 ]]
[[ ! -e "$observer_evidence" ]]
# Resolve the existing parent before creating the exact new directory to seal.
observer_parent=$(realpath -e -- "$(dirname -- "$observer_evidence")")
observer_name=$(basename -- "$observer_evidence")
[[ "$observer_name" != . && "$observer_name" != .. && -n "$observer_name" ]]
observer_evidence="$observer_parent/$observer_name"
[[ ! -e "$observer_evidence" ]]

runtime_root=/home/azgaldov/moex/vendor/cgate-test-9.9.1853/staged/deb-root/opt/moex/cgate
runtime_library="$runtime_root/lib/libcgate.so.6.102.0.6118"
scheme_dir="$runtime_root/scheme/latest"
config_dir=/home/azgaldov/moex/vendor/cgate-test-9.9.1853/runtime
router_bin="$runtime_root/bin/P2MQRouter-229.123.0.7233"
router_config="$config_dir/t1.ini"
software_key_env=/home/azgaldov/.config/moex-connector/secrets/plaza2_test.env

verify_sha() {
    [[ -r "$1" ]]
    [[ "$(sha256sum "$1" | awk '{print $1}')" == "$2" ]]
}
verify_sha "$observer_bin" "$observer_sha"
verify_sha "$runtime_library" f63e726a8482b793c3af755a8dc2b9ebb5cd727d88fb58ebb3fe9704a155ce6f
verify_sha "$scheme_dir/forts_scheme.ini" 7b93117ee435fd0cb2849b677fc32a9d581364b6ee9afeac9c6c002875400746
verify_sha "$router_bin" 6ea6ad50d6e3300fee99e43900eb22a21c207166f2455d2fc850f7e30bc91423
verify_sha "$router_config" 30ff2ec566edac9e7b8d8d255c1fa2161edc77f4741076ded9b80fd9ad573d68
[[ -x "$observer_bin" && -r "$software_key_env" ]]
router_pid=$(ps -eo pid=,args= | awk -v expected="$router_bin /ini:$router_config" '
    {pid=$1; $1=""; sub(/^ +/, ""); if ($0 == expected) {print pid; count++}}
    END {if (count != 1) exit 1}')
[[ "$(readlink "/proc/$router_pid/exe")" == "$router_bin" ]]
[[ -r "/proc/$router_pid/fd/5" ]]
# Require the pinned T1 process itself to own port 4101, not just any listening socket.
socket_state=$(ss -ltnp 'sport = :4101')
[[ "$socket_state" == *"pid=$router_pid,"* ]]
native_log=$(readlink "/proc/$router_pid/fd/5")
[[ -r "$native_log" ]]

mkdir -m 700 "$observer_evidence"
{
    date --iso-8601=seconds
    printf 'router_pid=%s\nnative_log=%s\nobservation_seconds=%s\n' "$router_pid" "$native_log" "$observer_seconds"
    printf '%s\n' 'endpoint=127.0.0.1:4101' 'read_only=true;order_entry_allowed=false'
    printf 'observer_source_sha=%s\n' "$observer_source_sha"
    sha256sum "$observer_bin" "$runtime_library" "$scheme_dir/forts_scheme.ini" "$router_bin" "$router_config"
} > "$observer_evidence/provenance.txt"

set -a
. "$software_key_env"
set +a
[[ -n "${MOEX_PLAZA2_CGATE_SOFTWARE_KEY:-}" ]]

observer_pid=''
forward_stop() {
    if [[ -n "$observer_pid" ]]; then kill -TERM "$observer_pid" 2>/dev/null || true; fi
}
trap forward_stop TERM INT
"$observer_bin" --output "$observer_evidence/events.jsonl" \
    --runtime-root "$runtime_root" --library-path "$runtime_library" \
    --scheme-dir "$scheme_dir" --config-dir "$config_dir" \
    --armed-test-read-only --observation-seconds "$observer_seconds" \
    > "$observer_evidence/observer.stdout" 2> "$observer_evidence/observer.stderr" &
observer_pid=$!
set +e
wait "$observer_pid"
observer_exit=$?
if kill -0 "$observer_pid" 2>/dev/null; then
    wait "$observer_pid"
    observer_exit=$?
fi
set -e
trap - TERM INT
printf 'exit_status=%s\n' "$observer_exit" > "$observer_evidence/exit.txt"
date --iso-8601=seconds >> "$observer_evidence/exit.txt"
sha256sum "$observer_evidence"/events.jsonl "$observer_evidence"/provenance.txt \
    "$observer_evidence"/observer.stdout "$observer_evidence"/observer.stderr \
    "$observer_evidence"/exit.txt > "$observer_evidence/SHA256SUMS"
sha256sum -c "$observer_evidence/SHA256SUMS" > "$observer_evidence/SHA256SUMS.verify"
chmod -R a-w -- "$observer_evidence"
exit "$observer_exit"
