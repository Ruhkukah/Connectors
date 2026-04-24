#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  plaza2_change_test_password.sh --utility <path> --host <host> --port <port> [--app-name <name>]

Starts the vendor change_password utility for an operator-assisted PLAZA II TEST
password change. Passwords are never accepted as command-line arguments and are
not logged by this wrapper.
USAGE
}

utility=""
host=""
port=""
app_name=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --utility)
      utility="${2:-}"
      shift 2
      ;;
    --host)
      host="${2:-}"
      shift 2
      ;;
    --port)
      port="${2:-}"
      shift 2
      ;;
    --app-name)
      app_name="${2:-}"
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

if [[ -z "$utility" || -z "$host" || -z "$port" ]]; then
  echo "--utility, --host, and --port are required" >&2
  usage >&2
  exit 2
fi

if [[ ! -x "$utility" ]]; then
  echo "change_password utility missing or not executable: $utility" >&2
  exit 1
fi

cat <<'NOTICE'
The vendor password-change utility is about to run interactively.

Rules:
  - Do not paste passwords into shell history.
  - Do not redirect this command to shared logs.
  - After success, update ~/.config/moex-connector/secrets/plaza2_test.env.
  - If auth_client.ini or router auth config stores credentials, update it
    according to MOEX/broker instructions and restart the local router if needed.
NOTICE

command=("$utility" "--host" "$host" "--port" "$port")
if [[ -n "$app_name" ]]; then
  command+=("--app-name" "$app_name")
fi

exec "${command[@]}"
