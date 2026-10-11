#!/usr/bin/env bash
# ============================================================================
# build.sh —— 编译 Grand Studio（C++ 88 键钢琴模拟器）
#
#   ./build.sh              编译到 build/piano.exe
#   ./build.sh --clean      先清空 build/
#   ./build.sh --run        编译后直接运行
#   ./build.sh --selftest   编译后运行离线自测
#
# 依赖：third_party/raylib（头文件 + libraylib.a）、third_party/miniaudio/miniaudio.h
# 编译器：优先使用随工程下载的 w64devkit（GCC），也接受 PATH 上的 g++。
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

CLEAN=0
RUN=0
SELFTEST=0
for arg in "$@"; do
  case "$arg" in
    --clean)    CLEAN=1 ;;
    --run)      RUN=1 ;;
    --selftest) SELFTEST=1 ;;
    *) echo "未知参数：$arg"; exit 2 ;;
  esac
done

# ---------------------------------------------------------------- 工具链定位
#
# 重要：MinGW 的 ld.exe 是原生 Windows 程序，用 ANSI API 解析自己的库搜索路径。
# 如果工具链装在含非 ASCII 字符的目录下（例如中文用户名），ld 会找不到 crt2.o、
# -lmingw32 等，链接必然失败。因此这里优先选择纯 ASCII 路径下的安装。
find_cxx() {
  if [ -n "${CXX:-}" ] && command -v "$CXX" >/dev/null 2>&1; then echo "$CXX"; return; fi

  local cand
  for cand in \
      /c/w64devkit/bin/g++.exe \
      /c/tools/w64devkit/bin/g++.exe \
      /c/mingw64/bin/g++.exe \
      "$HOME/w64devkit/bin/g++.exe" ; do
    if [ -x "$cand" ]; then echo "$cand"; return; fi
  done

  for cand in g++ x86_64-w64-mingw32-g++ clang++ ; do
    if command -v "$cand" >/dev/null 2>&1; then echo "$cand"; return; fi
  done

  # 兜底：便携工具链可能被放在带中文的目录里（编译能过，但链接会失败）
  for cand in /c/Users/*/.workbuddy-ai/binaries/w64devkit/bin/g++.exe ; do
    if [ -x "$cand" ]; then echo "$cand"; return; fi
  done
}

CXX="$(find_cxx || true)"
if [ -z "$CXX" ]; then
  echo "错误：找不到 C++ 编译器。"
  echo "      请设置环境变量 CXX，或把 w64devkit 解压到 C:\\w64devkit。"
  exit 1
fi

# 把编译器所在目录加进 PATH —— g++ 需要能找到同目录下的 as / ld / ar
CXX_DIR="$(cd "$(dirname "$CXX")" && pwd)"
case ":$PATH:" in
  *":$CXX_DIR:"*) ;;
  *) PATH="$CXX_DIR:$PATH"; export PATH ;;
esac

# 工具链路径含非 ASCII 字符时给出明确警告（链接一定会失败）
case "$CXX_DIR" in
  *[!\ -~]*) echo "警告：工具链路径含非 ASCII 字符，链接可能失败："
             echo "      $CXX_DIR"
             echo "      建议：cp -r \"$CXX_DIR/..\" /c/w64devkit" ;;
esac

# ------------------------------------------------------------------ 依赖检查
if [ ! -f third_party/raylib/include/raylib.h ]; then
  echo "错误：缺少 third_party/raylib/include/raylib.h"; exit 1
fi
if [ ! -f third_party/raylib/lib/libraylib.a ]; then
  echo "错误：缺少 third_party/raylib/lib/libraylib.a"; exit 1
fi
if [ ! -f third_party/miniaudio/miniaudio.h ]; then
  echo "错误：缺少 third_party/miniaudio/miniaudio.h"; exit 1
fi

# --------------------------------------------------------------------- 准备
if command -v python3 >/dev/null 2>&1; then
  python3 tools/generate_glyphs.py
  python3 tools/generate_glyphs.py --check
elif command -v python >/dev/null 2>&1; then
  python tools/generate_glyphs.py
  python tools/generate_glyphs.py --check
fi
[ "$CLEAN" -eq 1 ] && rm -rf build
mkdir -p build/obj

SOURCES=()
while IFS= read -r f; do SOURCES+=("$f"); done < <(find src tests -name '*.cpp' | sort)

echo "编译器 : $CXX"
echo "版本   : $("$CXX" -dumpversion 2>/dev/null || echo '?')"
echo "源文件 : ${#SOURCES[@]} 个"
echo "------------------------------------------------------------"

INCLUDES=(
  -Isrc
  -Ithird_party/raylib/include
  -Ithird_party/miniaudio
)
FLAGS=(
  -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter
  -fno-strict-aliasing
)

echo "语法预检（g++ -fsyntax-only）..."
for rel in "${SOURCES[@]}"; do
  if ! "$CXX" "${FLAGS[@]}" "${INCLUDES[@]}" -fsyntax-only "$rel" >/dev/null 2>&1; then
    echo "语法检查失败：$rel"; exit 1
  fi
done

# 注意：这里一律使用「相对路径」。
# 本机工程目录含中文，MinGW 的汇编器（as）是独立的原生 Windows 程序，
# 收到含中文的绝对路径时无法创建输出文件；相对路径可以完全绕开这个问题。
LIBS=(
  -Lthird_party/raylib/lib -lraylib
  -lopengl32 -lgdi32 -lwinmm -lole32 -loleaut32 -luuid -lshell32 -luser32
  -static -static-libgcc -static-libstdc++
)

# --------------------------------------------------------------------- 编译
LOGDIR="build/logs"
mkdir -p "$LOGDIR"

# 目标文件名：把路径里的 '/' 换成 '_'。
# 用 bash 参数展开而不是外部 tr —— 在本机环境下 tr 会把文件名里的大部分字母
# 替换成 '_'，产出难以辨认的名字（功能上无害，但排查问题时非常误导）。
flat_name() { local r="${1//\//_}"; echo "$r"; }
obj_of()    { echo "build/obj/$(flat_name "$1").o"; }
log_of()    { echo "$LOGDIR/$(flat_name "$1").log"; }
status_of() { echo "$LOGDIR/$(flat_name "$1").status"; }

JOBS="${JOBS:-4}"
running=0
for rel in "${SOURCES[@]}"; do
  ( "$CXX" "${FLAGS[@]}" "${INCLUDES[@]}" -c "$rel" -o "$(obj_of "$rel")" \
      >"$(log_of "$rel")" 2>&1
    echo $? > "$(status_of "$rel")" ) &
  running=$((running + 1))
  if [ "$running" -ge "$JOBS" ]; then
    wait -n 2>/dev/null || true
    running=$((running - 1))
  fi
done
wait 2>/dev/null || true

# 逐项检查编译退出码。
# 注意：不能只判断目标文件是否存在 —— 编译失败时旧的目标文件仍在，
# 会被误判成成功，然后链接进一个「悄悄过期」的可执行文件（这个坑踩过一次）。
FAILED=0
for rel in "${SOURCES[@]}"; do
  st="$(cat "$(status_of "$rel")" 2>/dev/null || echo 1)"
  if [ "$st" = "0" ]; then
    echo "  ok    $rel"
  else
    echo "  FAIL  $rel  (编译器返回 $st)"
    sed 's/^/        /' "$(log_of "$rel")" | head -40
    FAILED=1
  fi
done
if [ "$FAILED" -ne 0 ]; then
  echo "------------------------------------------------------------"
  echo "编译失败。完整日志在 build/logs/ 下。"
  exit 1
fi

# --------------------------------------------------------------------- 链接
#
# 产出两个可执行文件，各自面向一种用法：
#   piano.exe      GUI 子系统（-mwindows）—— 双击不弹命令框，给用户玩的成品
#   piano-cli.exe  控制台子系统 —— 保留 stdout，供 --selftest 与诊断
#
# 子系统在链接时写死在 PE 头里，GUI 程序的 stdout 没地方去，
# 所以自测必须用控制台版。
#
# 刻意输出到「工程根目录」而不是 build/ 下：
#   - 用户一眼就能看到，双击即可
#   - 双击时工作目录就是工程根，assets/samples、exports/ 都能自然找到
OBJECTS=()
for f in "${SOURCES[@]}"; do OBJECTS+=("$(obj_of "$f")"); done

echo "------------------------------------------------------------"
echo "链接 piano.exe (GUI) 与 piano-cli.exe (控制台) ..."
"$CXX" "${OBJECTS[@]}" -o piano.exe     -mwindows "${LIBS[@]}"
"$CXX" "${OBJECTS[@]}" -o piano-cli.exe            "${LIBS[@]}"
echo "完成："
ls -la piano.exe piano-cli.exe

# --------------------------------------------------------------------- 运行
if [ "$SELFTEST" -eq 1 ]; then
  echo "------------------------------------------------------------"
  ./piano-cli.exe --selftest
fi

if [ "$RUN" -eq 1 ]; then
  echo "------------------------------------------------------------"
  ./piano.exe &
fi
