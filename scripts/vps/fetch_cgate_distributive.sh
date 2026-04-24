#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  fetch_cgate_distributive.sh --url <url> --out-dir <dir> [--allow-non-moex-url]

Downloads one operator-selected CGate distributive and records its SHA256.
The script refuses non-MOEX URLs unless --allow-non-moex-url is supplied.
USAGE
}

url="${CGATE_DISTRIBUTIVE_URL:-}"
out_dir="$HOME/moex/dist"
allow_non_moex=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --url)
      url="${2:-}"
      shift 2
      ;;
    --out-dir)
      out_dir="${2:-}"
      shift 2
      ;;
    --allow-non-moex-url)
      allow_non_moex=true
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

if [[ -z "$url" ]]; then
  echo "--url or CGATE_DISTRIBUTIVE_URL is required" >&2
  usage >&2
  exit 2
fi

case "$url" in
  https://ftp.moex.com/pub/ClientsAPI/Spectra/CGate/*|https://ftp.moex.ru/pub/ClientsAPI/Spectra/CGate/*)
    ;;
  *)
    if [[ "$allow_non_moex" != true ]]; then
      echo "refusing non-MOEX URL without --allow-non-moex-url: $url" >&2
      exit 2
    fi
    ;;
esac

mkdir -p "$out_dir"
filename="$(basename "${url%%\?*}")"
if [[ -z "$filename" || "$filename" == "." || "$filename" == "/" ]]; then
  echo "could not derive filename from URL" >&2
  exit 2
fi

target="$out_dir/$filename"
curl --fail --location --show-error --output "$target" "$url"
sha256sum "$target" > "$target.sha256"

echo "$target"
cat "$target.sha256"
