#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"
CLEAN=0; RUN=0; SELFTEST=0
for arg in "$@"; do
  case "$arg" in
    --clean) CLEAN=1 ;;
    --run) RUN=1 ;;
    --selftest) SELFTEST=1 ;;
    *) echo "未知参数：$arg" >&2; exit 2 ;;
  esac
done
command -v cmake >/dev/null || { echo "找不到 CMake 3.16 或更高版本" >&2; exit 1; }
if [ "$CLEAN" -eq 1 ]; then rm -rf build; fi
GENERATOR_ARGS=()
if command -v ninja >/dev/null; then GENERATOR_ARGS=(-G Ninja); fi
cmake -S . -B build "${GENERATOR_ARGS[@]}" -DCMAKE_BUILD_TYPE=Release -DPIANO_TSAN=OFF
cmake --build build --parallel "${JOBS:-4}"
if [ "$SELFTEST" -eq 1 ]; then ./piano-cli --selftest; fi
if [ "$RUN" -eq 1 ]; then ./piano & fi
