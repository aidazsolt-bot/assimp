#!/usr/bin/env bash
set -euo pipefail
# Run from tinyusdz_repo-src (FetchContent PATCH_COMMAND cwd).
ROOT="$(cd "$(dirname "$0")" && pwd)"
git apply "${ROOT}/tinyusdz.patch"
git apply "${ROOT}/tinyusdz-unknown-metas.patch"
