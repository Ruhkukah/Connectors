#!/usr/bin/env bash
set -u

ROOT=/home/azgaldov/moex/qualification/authority-probe-d16-5-20260918T0900MSK
BIN="$ROOT/bin/moex_plaza2_aggr20_authority_probe"
EVIDENCE="$ROOT/evidence"
RUNTIME_ROOT=/home/azgaldov/moex/vendor/cgate-test-9.9.1853/staged/deb-root/opt/moex/cgate
LIBRARY_PATH="$RUNTIME_ROOT/lib/libcgate.so.6.102.0.6118"
SCHEME_DIR="$RUNTIME_ROOT/scheme/latest"
SCHEME_PATH="$SCHEME_DIR/forts_scheme.ini"
CONFIG_DIR=/home/azgaldov/moex/vendor/cgate-test-9.9.1853/runtime
ROUTER_BIN="$RUNTIME_ROOT/bin/P2MQRouter-229.123.0.7233"
ROUTER_CONFIG="$CONFIG_DIR/t1.ini"
SOFTWARE_KEY_ENV=/home/azgaldov/.config/moex-connector/secrets/plaza2_test.env
APP_NAME=read_only_aggr20_probe_d16_5_20260918T0900MSK
EXPECTED_BINARY_SHA=279bee891355e4920c4e5531d237604b02752d50477167cd96a2846f16f63e3c
EXPECTED_LIBRARY_SHA=f63e726a8482b793c3af755a8dc2b9ebb5cd727d88fb58ebb3fe9704a155ce6f
EXPECTED_SCHEME_SHA=7b93117ee435fd0cb2849b677fc32a9d581364b6ee9afeac9c6c002875400746
EXPECTED_ROUTER_SHA=6ea6ad50d6e3300fee99e43900eb22a21c207166f2455d2fc850f7e30bc91423
EXPECTED_CONFIG_SHA=30ff2ec566edac9e7b8d8d255c1fa2161edc77f4741076ded9b80fd9ad573d68

umask 077
mkdir -p "$EVIDENCE"

fail_preflight() {
    printf 'PREFLIGHT=FAIL\nreason=%s\n' "$1" > "$ROOT/preflight-failure.txt"
    exit 10
}

actual_sha() {
    sha256sum "$1" | awk '{print $1}'
}

test -r "$BIN" || fail_preflight "candidate binary is not readable"
test "$(actual_sha "$BIN")" = "$EXPECTED_BINARY_SHA" || fail_preflight "candidate binary hash mismatch"
test -r "$LIBRARY_PATH" || fail_preflight "runtime library is not readable"
test "$(actual_sha "$LIBRARY_PATH")" = "$EXPECTED_LIBRARY_SHA" || fail_preflight "runtime library hash mismatch"
test -r "$SCHEME_PATH" || fail_preflight "runtime scheme is not readable"
test "$(actual_sha "$SCHEME_PATH")" = "$EXPECTED_SCHEME_SHA" || fail_preflight "runtime scheme hash mismatch"
test -r "$ROUTER_BIN" || fail_preflight "router binary is not readable"
test "$(actual_sha "$ROUTER_BIN")" = "$EXPECTED_ROUTER_SHA" || fail_preflight "router binary hash mismatch"
test -r "$ROUTER_CONFIG" || fail_preflight "router config is not readable"
test "$(actual_sha "$ROUTER_CONFIG")" = "$EXPECTED_CONFIG_SHA" || fail_preflight "router config hash mismatch"
test -r "$SOFTWARE_KEY_ENV" || fail_preflight "software-key environment file is not readable"

ROUTER_PID=$(ps -eo pid=,args= | awk '$0 ~ /\/P2MQRouter-229\.123\.0\.7233 \/ini:\/home\/azgaldov\/moex\/vendor\/cgate-test-9\.9\.1853\/runtime\/t1\.ini$/ {print $1; exit}')
test -n "$ROUTER_PID" || fail_preflight "versioned T1 router process was not found"
test -r "/proc/$ROUTER_PID/fd/5" || fail_preflight "router native-log fd 5 is not readable"
NATIVE_LOG=$(readlink "/proc/$ROUTER_PID/fd/5")
test -n "$NATIVE_LOG" || fail_preflight "router native-log fd 5 has no target"
test -r "$NATIVE_LOG" || fail_preflight "router native-log target is not readable"
ss -ltn 'sport = :4101' | grep -q ':4101' || fail_preflight "T1 router endpoint is not listening"

date --iso-8601=seconds > "$ROOT/runner-started.txt"
{
    printf '%s\n' 'preflight=PASS'
    printf '%s\n' 'source_base_sha=deff561dd524633268734a4a7eb3dc0763475027'
    printf '%s\n' 'candidate_source_sha=2c5464a96f2014b3d53f3f32015a4d4a7dbc47d5'
    printf '%s\n' "candidate_binary_sha256=$EXPECTED_BINARY_SHA"
    printf '%s\n' 'runtime_release=SPECTRA9.9.0'
    printf '%s\n' "runtime_root=$RUNTIME_ROOT"
    printf '%s\n' "library_path=$LIBRARY_PATH"
    printf '%s\n' "scheme_path=$SCHEME_PATH"
    printf '%s\n' "config_path=$ROUTER_CONFIG"
    printf '%s\n' "router_pid=$ROUTER_PID"
    printf '%s\n' "native_router_log=$NATIVE_LOG"
    printf '%s\n' 'endpoint=127.0.0.1:4101'
    printf '%s\n' "connection_app_name=$APP_NAME"
    printf '%s\n' 'observation_seconds=120'
    printf '%s\n' 'process_timeout_ms=50'
    printf '%s\n' 'credentials=none'
    printf '%s\n' 'software_key_source=env'
    printf '%s\n' 'read_only_contract=public_refdata_and_aggr20_only;publisher=false;command_api=false;order_api=false'
    sha256sum "$BIN" "$LIBRARY_PATH" "$SCHEME_PATH" "$ROUTER_BIN" "$ROUTER_CONFIG"
    file "$BIN" "$LIBRARY_PATH" "$ROUTER_BIN"
} > "$ROOT/candidate-provenance.txt" 2>&1

{
    date --iso-8601=seconds
    printf 'router_pid=%s\n' "$ROUTER_PID"
    pgrep -af P2MQRouter || true
    ss -ltn 'sport = :4101' || true
    readlink "/proc/$ROUTER_PID/fd/5"
    ls -l "/proc/$ROUTER_PID/fd"
} > "$ROOT/router-before.txt" 2>&1
cp -p "$NATIVE_LOG" "$ROOT/native-router-log-before.txt"
cp -p /home/azgaldov/moex/qualification/20260911-router-pr55/control/router.stdout "$ROOT/router.stdout-before.txt" 2>/dev/null || true
cp -p /home/azgaldov/moex/qualification/20260911-router-pr55/control/router.stderr "$ROOT/router.stderr-before.txt" 2>/dev/null || true

set -a
. "$SOFTWARE_KEY_ENV"
set +a
test -n "${MOEX_PLAZA2_CGATE_SOFTWARE_KEY:-}" || fail_preflight "software-key environment variable is empty"

set +e
"$BIN" \
    --profile-id "$APP_NAME" \
    --output-dir "$EVIDENCE" \
    --endpoint-host 127.0.0.1 \
    --endpoint-port 4101 \
    --runtime-root "$RUNTIME_ROOT" \
    --library-path "$LIBRARY_PATH" \
    --scheme-dir "$SCHEME_DIR" \
    --config-dir "$CONFIG_DIR" \
    --env-open-settings 'ini=t1.ini;key=${MOEX_PLAZA2_CGATE_SOFTWARE_KEY}' \
    --expected-spectra-release SPECTRA9.9.0 \
    --expected-runtime-library-sha256 "$EXPECTED_LIBRARY_SHA" \
    --expected-scheme-sha256 "$EXPECTED_SCHEME_SHA" \
    --connection-settings "p2tcp://127.0.0.1:4101;app_name=$APP_NAME" \
    --refdata-settings 'p2repl://FORTS_REFDATA_REPL;scheme=|FILE|scheme/forts_scheme.ini|REFDATA' \
    --refdata-open-settings 'mode=snapshot+online' \
    --aggr-settings 'p2repl://FORTS_AGGR20_REPL;scheme=|FILE|scheme/forts_scheme.ini|Aggr' \
    --aggr-open-settings 'mode=snapshot+online' \
    --observation-seconds 120 \
    --process-timeout-ms 50 \
    --software-key-source env \
    --software-key-env-var MOEX_PLAZA2_CGATE_SOFTWARE_KEY \
    --armed-test-network \
    --armed-test-session \
    --armed-test-plaza2 \
    > "$ROOT/probe.stdout" 2> "$ROOT/probe.stderr"
PROBE_EXIT=$?
set -u
printf 'probe_exit_status=%s\n' "$PROBE_EXIT" > "$ROOT/probe-exit.txt"
date --iso-8601=seconds > "$ROOT/runner-finished.txt"

ROUTER_PID_AFTER=$(ps -eo pid=,args= | awk '$0 ~ /\/P2MQRouter-229\.123\.0\.7233 \/ini:\/home\/azgaldov\/moex\/vendor\/cgate-test-9\.9\.1853\/runtime\/t1\.ini$/ {print $1; exit}')
{
    date --iso-8601=seconds
    printf 'router_pid=%s\n' "$ROUTER_PID_AFTER"
    pgrep -af P2MQRouter || true
    ss -ltn 'sport = :4101' || true
    if [ -n "$ROUTER_PID_AFTER" ]; then
        readlink "/proc/$ROUTER_PID_AFTER/fd/5" || true
        ls -l "/proc/$ROUTER_PID_AFTER/fd" || true
    fi
} > "$ROOT/router-after.txt" 2>&1
if [ -n "$ROUTER_PID_AFTER" ] && [ -r "/proc/$ROUTER_PID_AFTER/fd/5" ]; then
    cp -p "$(readlink "/proc/$ROUTER_PID_AFTER/fd/5")" "$ROOT/native-router-log-after.txt"
fi
cp -p /home/azgaldov/moex/qualification/20260911-router-pr55/control/router.stdout "$ROOT/router.stdout-after.txt" 2>/dev/null || true
cp -p /home/azgaldov/moex/qualification/20260911-router-pr55/control/router.stderr "$ROOT/router.stderr-after.txt" 2>/dev/null || true

find "$ROOT" -type f ! -name SHA256SUMS ! -name SHA256SUMS.verify -print0 | sort -z | xargs -0 sha256sum > "$ROOT/SHA256SUMS"
sha256sum -c "$ROOT/SHA256SUMS" > "$ROOT/SHA256SUMS.verify" 2>&1
chmod -R a-w "$ROOT"
exit 0
