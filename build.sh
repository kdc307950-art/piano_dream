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

# 非 ASCII 路径兜底：CMake 以 UTF-8 写构建文件，而 Ninja / MinGW 按系统 ANSI
# 代码页（GBK）解析路径 —— 中文路径会让 ninja 在 FindFirstFileExA 阶段直接报
# "The filename, directory name, or volume label syntax is incorrect"。
# 检测到非 ASCII 路径时，用 subst 把工程映射到一个纯 ASCII 盘符再构建；
# 产物经过映射仍写回真实目录，对用户完全透明。
SUBST_LETTER=""
if printf '%s' "$ROOT" | LC_ALL=C grep -q '[^ -~]'; then
  for L in P Q R S T U V W X Y Z; do
    if [ ! -e "/$L" ]; then SUBST_LETTER="$L"; break; fi
  done
  [ -n "$SUBST_LETTER" ] || { echo "工程路径含非 ASCII 字符，且没有空闲盘符可做 subst 映射。" >&2; exit 1; }
  subst "$SUBST_LETTER:" "$(cygpath -w "$ROOT")" >/dev/null
  echo "工程路径含非 ASCII 字符，已通过 $SUBST_LETTER: 映射构建（等价于原路径）"
  cd "/$SUBST_LETTER"
  # 旧 build 缓存若记录的是原始中文路径，CMake 拒绝跨路径复用，直接重建
  if [ -f build/CMakeCache.txt ] && ! grep -q "$SUBST_LETTER:/" build/CMakeCache.txt; then
    rm -rf build
  fi
fi
unmap() { if [ -n "$SUBST_LETTER" ]; then subst "$SUBST_LETTER:" /d >/dev/null 2>&1 || true; fi }
trap unmap EXIT

if [ "$CLEAN" -eq 1 ]; then rm -rf build; fi
GENERATOR_ARGS=()
if command -v ninja >/dev/null; then GENERATOR_ARGS=(-G Ninja); fi
cmake -S . -B build "${GENERATOR_ARGS[@]}" -DCMAKE_BUILD_TYPE=Release -DPIANO_TSAN=OFF
cmake --build build --parallel "${JOBS:-4}"
if [ "$SELFTEST" -eq 1 ]; then ./piano-cli --selftest; fi
if [ "$RUN" -eq 1 ]; then ./piano & fi
