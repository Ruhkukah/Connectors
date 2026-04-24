#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  package_plaza2_test_bundle.sh --build-dir <dir> --out-dir <dir> [--git-sha <sha>]

Creates a narrow PLAZA II TEST deploy bundle from an existing Linux build tree.
No credentials, endpoints, CGate vendor files, or build-cache junk are packaged.
USAGE
}

build_dir=""
out_dir=""
git_sha=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      build_dir="${2:-}"
      shift 2
      ;;
    --out-dir)
      out_dir="${2:-}"
      shift 2
      ;;
    --git-sha)
      git_sha="${2:-}"
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

if [[ -z "$build_dir" || -z "$out_dir" ]]; then
  echo "--build-dir and --out-dir are required" >&2
  usage >&2
  exit 2
fi

build_dir="$(cd "$build_dir" && pwd)"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out_dir="$(mkdir -p "$out_dir" && cd "$out_dir" && pwd)"
git_sha="${git_sha:-$(git -C "$repo_root" rev-parse HEAD 2>/dev/null || echo unknown)}"

required_apps=(
  "$build_dir/apps/moex_plaza2_test_runner"
  "$build_dir/apps/moex_plaza2_cert_runner"
)

for app in "${required_apps[@]}"; do
  if [[ ! -x "$app" ]]; then
    echo "required built app missing or not executable: $app" >&2
    exit 1
  fi
done

staging="$(mktemp -d "${TMPDIR:-/tmp}/moex-plaza2-bundle.XXXXXX")"
trap 'rm -rf "$staging"' EXIT

mkdir -p "$staging/build-docker-linux/apps"
cp "$build_dir/apps/moex_plaza2_test_runner" "$staging/build-docker-linux/apps/"
if [[ -x "$build_dir/apps/moex_plaza2_twime_integrated_test_runner" ]]; then
  cp "$build_dir/apps/moex_plaza2_twime_integrated_test_runner" "$staging/build-docker-linux/apps/"
fi

cat > "$staging/build-docker-linux/apps/moex_plaza2_cert_runner" <<'EOF'
#!/usr/bin/env sh
set -eu

APP_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
BUNDLE_ROOT="$(CDPATH= cd -- "$APP_DIR/../.." && pwd)"
export PYTHONPATH="$BUNDLE_ROOT/tools${PYTHONPATH+:$PYTHONPATH}"
exec python3 "$BUNDLE_ROOT/apps/moex_plaza2_cert_runner.py" "$@"
EOF
chmod 755 "$staging/build-docker-linux/apps/moex_plaza2_cert_runner"

if [[ -d "$build_dir/lib" ]]; then
  mkdir -p "$staging/build-docker-linux/lib"
  find "$build_dir/lib" -maxdepth 1 -type f \( -name '*.so' -o -name '*.so.*' \) -exec cp {} "$staging/build-docker-linux/lib/" \;
fi

mkdir -p "$staging/apps" "$staging/tools" "$staging/profiles" "$staging/scripts/vps" "$staging/docs" \
  "$staging/spec-lock/test/plaza2"
cp "$repo_root/apps/moex_plaza2_cert_runner.py" "$staging/apps/"
cp "$repo_root/tools/moex_phase0_common.py" "$staging/tools/"
cp "$repo_root/tools/plaza2_runtime_scheme_lock.py" "$staging/tools/"
cp "$repo_root/profiles/test_plaza2_repl_live_session.template.yaml" "$staging/profiles/"
cp "$repo_root"/scripts/vps/*.sh "$staging/scripts/vps/"
cp "$repo_root/docs/plaza2_phase4d_vps_cgate_install_evidence.md" "$staging/docs/"
cp "$repo_root/docs/plaza2_phase4e_runtime_scheme_lock.md" "$staging/docs/"
cp -R "$repo_root/spec-lock/test/plaza2/runtime_scheme" "$staging/spec-lock/test/plaza2/"

cat > "$staging/README.phase4d.txt" <<EOF
MOEX Connectors PLAZA II TEST bundle
git_sha=$git_sha

This bundle contains binaries, scripts, profile templates, and docs only.
It intentionally excludes credentials, endpoints, CGate vendor archives, and evidence.
EOF

bundle="$out_dir/moex-plaza2-test-bundle.tgz"
COPYFILE_DISABLE=1 tar --format ustar -czf "$bundle" -C "$staging" .
sha256sum "$bundle" > "$bundle.sha256"

echo "$bundle"
cat "$bundle.sha256"
