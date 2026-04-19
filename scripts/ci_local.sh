#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BUILD_DIR="${MOEX_BUILD_DIR:-build}"
export BUILD_DIR

python3 - <<'PY'
import shutil
import os
from pathlib import Path

build_dir = Path(os.environ["BUILD_DIR"])
if build_dir.exists():
    for child in build_dir.iterdir():
        if child.is_dir():
            shutil.rmtree(child, ignore_errors=True)
        else:
            child.unlink(missing_ok=True)
else:
    build_dir.mkdir()
PY

python3 tools/unicode_guard.py \
  .github \
  CMakeLists.txt \
  include \
  src \
  protocols \
  connectors \
  tools \
  tests \
  matrix \
  profiles \
  cert \
  docs

cmake -S . -B "$BUILD_DIR"
cmake --build "$BUILD_DIR"
ctest --test-dir "$BUILD_DIR" --output-on-failure
"$BUILD_DIR"/tools/matrix_validate --matrix-dir matrix
"$BUILD_DIR"/tools/twime_schema_indexer --schema protocols/twime_sbe/schema/twime_spectra-7.7.xml --out matrix/protocol_inventory
"$BUILD_DIR"/tools/twime_codegen --schema protocols/twime_sbe/schema/twime_spectra-7.7.xml --out protocols/twime_sbe/generated --check
"$BUILD_DIR"/tools/twime_fixture_check --fixtures tests/fixtures/twime_sbe --check
"$BUILD_DIR"/tools/source_style_check --require-clang-format \
  connectors/twime_trade \
  protocols/twime_sbe \
  tests/twime_trade \
  tests/twime_sbe \
  tools/twime_codegen.py \
  tools/twime_schema_common.py \
  tools/twime_schema_indexer.py \
  tools/twime_schema_materialize.py
