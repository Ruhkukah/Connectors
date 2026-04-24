#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  install_cgate_linux.sh --archive <zip-or-tgz> --install-dir <dir> [--installer <install.sh>]
                         [--evidence-dir <dir>] [--execute]

Inspects and stages an official CGate Linux distributive install. By default this
is a dry run. Pass --execute to run the vendor install.sh from an unpacked temp
directory. The script never installs into system directories by default.
USAGE
}

repair_payload_symlinks() {
  local root="$1"
  local path target dir
  while IFS= read -r -d '' path; do
    target="$(cat "$path" 2>/dev/null || true)"
    case "$target" in
      ""|*/*|*$'\n'*)
        continue
        ;;
    esac
    dir="$(dirname "$path")"
    if [[ -e "$dir/$target" ]]; then
      rm -f "$path"
      ln -s "$target" "$path"
    fi
  done < <(find "$root" -type f -size -256c -print0)
}

archive=""
install_dir=""
installer=""
evidence_dir="$HOME/moex/evidence/cgate-install"
execute=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --archive)
      archive="${2:-}"
      shift 2
      ;;
    --install-dir)
      install_dir="${2:-}"
      shift 2
      ;;
    --installer)
      installer="${2:-}"
      shift 2
      ;;
    --evidence-dir)
      evidence_dir="${2:-}"
      shift 2
      ;;
    --execute)
      execute=true
      shift
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

if [[ -z "$archive" || -z "$install_dir" ]]; then
  echo "--archive and --install-dir are required" >&2
  usage >&2
  exit 2
fi

if [[ ! -f "$archive" ]]; then
  echo "archive not found: $archive" >&2
  exit 1
fi

case "$install_dir" in
  "$HOME"/moex/runtime/*)
    ;;
  *)
    echo "install dir must be under ~/moex/runtime for Phase 4D: $install_dir" >&2
    exit 2
    ;;
esac

mkdir -p "$install_dir" "$evidence_dir"
work_dir="$(mktemp -d "${TMPDIR:-/tmp}/cgate-install.XXXXXX")"
trap 'rm -rf "$work_dir"' EXIT

case "$archive" in
  *.zip)
    if command -v unzip >/dev/null 2>&1; then
      unzip -q "$archive" -d "$work_dir"
    elif command -v python3 >/dev/null 2>&1; then
      python3 -m zipfile -e "$archive" "$work_dir"
    else
      echo "unzip or python3 is required to inspect the CGate archive" >&2
      exit 1
    fi
    ;;
  *.tar.gz|*.tgz)
    tar -xzf "$archive" -C "$work_dir"
    ;;
  *)
    echo "unsupported archive extension: $archive" >&2
    exit 2
    ;;
esac

find "$work_dir" -maxdepth 4 -type f | sort > "$evidence_dir/archive_contents.txt"
payload_dir="$(find "$work_dir" -maxdepth 3 -type d -name cgate | sort | head -n 1)"
if [[ -z "$payload_dir" ]]; then
  echo "CGate payload directory named cgate not found in archive" >&2
  exit 1
fi
if [[ -z "$installer" ]]; then
  installer="$(find "$work_dir" -maxdepth 4 -type f -name install.sh | sort | head -n 1)"
fi
if [[ -z "$installer" ]]; then
  echo "vendor install.sh not found in archive; pass --installer if MOEX distributes it separately" >&2
  exit 1
fi
if [[ ! -f "$installer" ]]; then
  echo "installer not found: $installer" >&2
  exit 1
fi

chmod 755 "$installer"
echo "installer=$installer" > "$evidence_dir/install_plan.txt"
echo "install_dir=$install_dir" >> "$evidence_dir/install_plan.txt"
echo "archive=$(basename "$archive")" >> "$evidence_dir/install_plan.txt"
echo "payload_dir=$payload_dir" >> "$evidence_dir/install_plan.txt"
sha256sum "$archive" > "$evidence_dir/$(basename "$archive").sha256"

if [[ "$execute" != true ]]; then
  echo "dry-run complete; rerun with --execute to install into the explicit user-owned directory"
  exit 0
fi

set +e
(
  cd "$(dirname "$installer")"
  CGATE_INSTALL_DIR="$install_dir" INSTALL_DIR="$install_dir" ./install.sh "$archive" "$install_dir"
) > "$evidence_dir/install.log" 2>&1
installer_status=$?
set -e

if [[ "$installer_status" -eq 0 ]]; then
  echo "install_method=vendor_install_sh" >> "$evidence_dir/install_plan.txt"
else
  echo "install_method=payload_copy_after_vendor_installer_status_$installer_status" >> "$evidence_dir/install_plan.txt"
  rm -rf "$install_dir"
  mkdir -p "$install_dir"
  cp -a "$payload_dir"/. "$install_dir"/
  repair_payload_symlinks "$install_dir"
fi

find "$install_dir" -maxdepth 8 \
  \( -name 'libcgate.so' -o -name 'forts_scheme.ini' -o -name 'change_password*' \
     -o -name 'auth_client.ini' -o -name 'client_router.ini' -o -name 'router.ini' \) \
  2>/dev/null | sort > "$evidence_dir/installed_runtime_paths.txt"

echo "install complete; inspect $evidence_dir for redacted logs and discovered paths"
