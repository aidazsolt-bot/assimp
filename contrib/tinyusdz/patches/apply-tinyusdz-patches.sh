#!/usr/bin/env bash
set -euo pipefail
# Manual helper only. CMake FetchContent PATCH_COMMAND uses `git apply` directly
# (see code/CMakeLists.txt) so Windows MSVC configures without bash.
ROOT="$(cd "$(dirname "$0")" && pwd)"
git apply "${ROOT}/tinyusdz.patch"
git apply "${ROOT}/tinyusdz-unknown-metas.patch"
