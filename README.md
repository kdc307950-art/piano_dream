# Grand Studio · C++ 88 键钢琴模拟器

[![Windows CI](https://github.com/kdc307950-art/piano_dream/actions/workflows/ci.yml/badge.svg)](https://github.com/kdc307950-art/piano_dream/actions/workflows/ci.yml)
[![Latest release](https://img.shields.io/github/v/release/kdc307950-art/piano_dream?display_name=tag)](https://github.com/kdc307950-art/piano_dream/releases/latest)

**中文说明 · English manual:** [README_EN.md](README_EN.md)

**下载：** [最新 Windows 安装器](https://github.com/kdc307950-art/piano_dream/releases/latest) · [Releases 页面](https://github.com/kdc307950-art/piano_dream/releases)

![Grand Studio Piano desktop UI](assets/ui-mockups/desktop-ui-concept.png)

Grand Studio Piano 是一款离线运行的 88 键数字钢琴桌面应用，提供真实钢琴采样、物理合成音色、MIDI 导入导出、录制回放、节拍器和跟练判定。

把原先的单文件网页钢琴（`钢琴模拟.html`）用 C++ 重做的一份完整实现。
不是「演示片段」，而是可以直接运行、可以继续往上加功能的工程：
88 键显示、电脑键盘 / 鼠标 / 多点触控、力度响应、延音踏板、复音管理、
录制回放、内置曲目自动演奏、跟练判定、标准 MIDI 导入导出、节拍器、混响。

技术栈：**C++20 + raylib（图形/输入）+ miniaudio（音频输出）**，Windows / MinGW-w64 下开发。

---

## 零、直接玩（不需要任何命令行）

**双击 `piano.exe` 就能玩。**

```
C:\Users\孔德草\Desktop\游戏开发\钢琴模拟器\piano\piano.exe
```

它是**窗口程序**（GUI 子系统），双击不会弹出黑色命令框。
如果 Windows 提示「已保护你的电脑」，点「更多信息」→「仍要运行」即可 ——
这是所有未做代码签名的程序的正常提示，不是病毒告警。

### 怎么弹

| 方式 | 操作 |
|---|---|
| **电脑键盘** | 下排 `Z S X D C V G B H N J M` = C3 起的 12 个半音；上排 `Q 2 W 3 E R 5 T 6 Y 7 U I 9 O 0 P` = C4 起的 17 个半音。琴键上标了对应的字母 |
| **空格** | 临时延音（踩下踏板） |
| **`[` / `]`** | 整体升降一个八度 |
| **鼠标 / 触屏** | 点琴键发声，**越靠近琴键下沿力度越强**；按住拖动可以滑奏 |
| **滚轮 / 拖动琴盖** | 左右滚动键盘；`−` `+` 按钮按八度跳转 |
| **拖 .mid 文件进窗口** | 导入 MIDI |

界面上的按钮都是中文的，鼠标点一下就知道：

- **播放曲目** —— 自动演奏内置的 5 首曲子，琴键会跟着亮
- **开始录制** → 弹一段 → **结束录制** → **回放** 或 **导出 MIDI**
- **节拍器** —— 打拍子；旁边可以调 BPM
- **仅伴奏跟练** —— 只放伴奏，你来弹旋律，右侧会实时打分

导出的 MIDI 落在 `piano\exports\`。

---

## 一、从源码构建（开发者）

### Windows：直接用 `build.cmd`（推荐，零配置）

```powershell
cd "C:\Users\孔德草\Desktop\游戏开发\钢琴模拟器\piano"

.\build.cmd -SelfTest    # 编译 + 跑离线自测（不需要声卡、不开窗口）
.\build.cmd -Run         # 编译 + 启动界面
.\build.cmd -Clean       # 先清空中间产物再编译
.\build.cmd -Package     # 编译 + 生成可分发 ZIP（build/GrandStudioPiano-*.zip）
.\build.cmd -Installer   # 编译 + 生成 NSIS 安装器（需要 NSIS 3.x / makensis.exe）
```

也可以**直接双击 `build.cmd`**（等价于不带参数运行，编译完会停住等你按键）。

发布包由 CPack 生成，包含 `piano.exe`、`piano-cli.exe`、真实钢琴采样目录和许可证。
如果工程路径含中文，请使用 `build.cmd -Package`；脚本会自动使用临时 ASCII 盘符，
避免 Ninja 无法解析中文路径。不要直接复用脚本创建的 `build/` 目录运行 CMake，
因为其中的构建缓存记录了该临时盘符。

构建前会自动扫描 `src/` 下的 C++ 字符串字面量并更新 `src/ui/GeneratedGlyphs.h`，确保运行时错误信息和状态文字也进入字形图集。可用 `python tools/generate_glyphs.py --check` 检查生成文件是否过期。

`build.cmd` 内部已经带了 `-ExecutionPolicy Bypass`，
所以**不需要改任何系统设置**就能用。

### 构建产物

| 文件 | 说明 |
|---|---|
| `piano.exe` | **GUI 子系统**（`-mwindows`）—— 双击运行，不弹命令框。这是给用户玩的成品 |
| `piano-cli.exe` | 控制台子系统 —— 保留 stdout，用于 `--selftest` 和诊断输出 |

> 为什么要两个？Windows 的子系统在**链接时写死在 PE 头里**，
> GUI 程序的 `stdout` 没有地方可去（`printf` 等于丢进黑洞），
> 而自测结果必须能打印出来。所以拆成两个，各司其职。
>
> 两个 exe 都刻意放在**工程根目录**而不是 `build\` 下：
> 一是用户一眼就能看到、双击即可；二是双击时工作目录就是工程根，
> `assets\samples`、`exports\` 都能自然找到。
> 中间产物（`.o`、编译日志）仍然在 `build\` 里。

### Windows：直接跑 `build.ps1`

`build.ps1` 更"原生"，但 Windows 默认的执行策略（Restricted）会拦住它：

```
.\build.ps1 : 无法加载文件 ...\build.ps1，因为在此系统上禁止运行脚本。
```

三种解法，任选其一：

```powershell
# 1) 单次绕过（不改系统设置）
powershell -ExecutionPolicy Bypass -File .\build.ps1 -SelfTest

# 2) 一劳永逸：允许本地脚本运行（推荐，只影响当前用户，不需要管理员）
Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
.\build.ps1 -SelfTest

# 3) 干脆用 .\build.cmd -SelfTest
```

> `RemoteSigned` 的含义：本地写的脚本可以直接跑，从网上下载的脚本才需要签名。
> 这是微软给开发机的推荐值。

### Git Bash / MSYS / WSL

```bash
cd /c/Users/孔德草/Desktop/游戏开发/钢琴模拟器/piano
./build.sh --selftest
./build.sh --run
```

> ⚠️ **`./build.sh` 只能在 bash 里跑，不能在 PowerShell 里跑** ——
> PowerShell 不认识 `.sh` 文件。在 PowerShell 里请用 `.\build.cmd`。
>
> 工程根目录是 `piano\`，不是用户主目录。第一次用请先 `cd` 进去，
> 或者直接双击 `build.cmd`。

### CMake（可选）

```powershell
cmake -S . -B build-cmake -G "MinGW Makefiles"
cmake --build build-cmake -j
```

CMake 版同样产出 `piano.exe`（GUI）与 `piano-cli.exe`（控制台），
都静态链接，不依赖 MinGW 运行库。

---

### 三个构建入口，按你的终端选

| 文件 | 用在哪儿 | 编码 | 命令 |
|---|---|---|---|
| `build.cmd` | PowerShell / cmd / 双击 | **纯 ASCII** | `.\build.cmd -SelfTest` |
| `build.ps1` | PowerShell | **UTF-8 with BOM** | `.\build.ps1 -SelfTest`（需绕过执行策略） |
| `build.sh` | Git Bash / MSYS / WSL | UTF-8 | `./build.sh --selftest` |

> **为什么三个文件的编码不一样？** 这不是随意选的，是被 Windows 逼的：
> - **`.ps1` 必须带 UTF-8 BOM**。Windows PowerShell 5.1 读取 `.ps1` 时，
>   如果没有 BOM，会按系统 ANSI 代码页（中文系统是 GBK）解码 ——
>   文件里的中文注释和字符串会全部变成乱码，直接报一堆语法错误。
> - **`.cmd` 必须保持纯 ASCII**。cmd.exe 用**控制台代码页**解析批处理文件，
>   一旦文件编码和控制台代码页不一致，连 `REM` 注释里的中文都会把解析器搞乱，
>   报出 `'保存为' 不是内部或外部命令` 这种莫名其妙的错。
>   所以所有中文提示都放在 `build.ps1` 里，`.cmd` 只做转发。
> - **`.sh` 用 UTF-8**，这是 Unix 侧的标准。

### 常见问题

| 现象 | 原因 / 解决 |
|---|---|
| `无法将“./build.sh”项识别为 cmdlet...` | 在 PowerShell 里跑了 `.sh`。改用 `.\build.cmd` |
| `无法加载文件 build.ps1，因为在此系统上禁止运行脚本` | PowerShell 执行策略（Windows 默认 Restricted）。用 `.\build.cmd`，或 `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned` |
| `'xxx' 不是内部或外部命令` （来自 .cmd 文件） | 批处理文件里混了非 ASCII 字符且编码与控制台代码页不符。本工程的 `build.cmd` 已是纯 ASCII |
| `找不到路径“C:\Users\xxx\piano”` | 工程在 `Desktop\游戏开发\钢琴模拟器\piano`，先 `cd` 进去 |
| 链接报 `cannot find crt2.o` / `-lmingw32` | 工具链路径含中文。把它复制到 `C:\w64devkit`（见上文「编译器」） |
| ninja 报 `FindFirstFileExA(...): The filename, directory name, or volume label syntax is incorrect` | 工程路径含中文：CMake 以 UTF-8 写构建文件，而 Ninja / MinGW 按系统 ANSI 代码页解析路径。`build.ps1` / `build.sh` 已自动用 `subst` 映射一个纯 ASCII 盘符兜底，正常重跑脚本即可 |
| 界面中文显示成 `?` | 系统缺中文字体。启动日志会打印实际用了哪个字体、缺哪些码点 |
| 控制台中文显示成 `閿欒鐞?` 这类乱码 | 控制台按 GBK 解码了 UTF-8 字节。程序已自动把控制台代码页切到 UTF-8（退出时恢复）；用 `.\build.cmd -SelfTest` 跑时脚本还会显式按 UTF-8 解码子进程输出，与你的控制台设置无关 |

### 依赖已经随工程带齐

`third_party/` 下已包含：

| 目录 | 内容 |
|---|---|
| `third_party/raylib/` | raylib 6.0 头文件 + `libraylib.a` |
| `third_party/miniaudio/` | `miniaudio.h` v0.11.25 单头文件 |

`libraylib.a` 是**用同一套 GCC 14 从 raylib 6.0 源码编译出来的**，
不是官方预编译包 —— 官方包用较老的 MinGW 构建，会引用 `stat64i32`
这类 GCC 14 的 `libmsvcrt.a` 里没有的符号，链接直接失败。

如果要自己重建它：

```bash
# 1) 取源码
curl -L -o raylib-src.zip https://github.com/raysan5/raylib/archive/refs/tags/6.0.zip
unzip raylib-src.zip            # 得到 raylib-6.0/src
# 2) 编译（PLATFORM_DESKTOP + OpenGL 3.3）
cd raylib-6.0/src
CFLAGS="-O2 -std=gnu99 -DPLATFORM_DESKTOP -DGRAPHICS_API_OPENGL_33 -D_DEFAULT_SOURCE \
        -Wno-missing-braces -fno-strict-aliasing -I. -Iexternal/glfw/include"
mkdir -p /tmp/rl && for f in rcore rshapes rtextures rtext rmodels raudio rglfw; do
  gcc $CFLAGS -c $f.c -o /tmp/rl/$f.o
done
for f in external/glfw/src/*.c; do
  gcc $CFLAGS -c "$f" -o "/tmp/rl/glfw_$(basename $f .c).o"
done
# 3) 打包替换
ar rcs libraylib.a /tmp/rl/*.o
cp libraylib.a <工程>/third_party/raylib/lib/
```

所以**不需要 vcpkg、不需要 FetchContent、不需要联网**即可构建。
如果你希望 CMake 自己拉依赖，改 `CMakeLists.txt` 里的 raylib 段落换成
`FetchContent_Declare` 即可，其余代码不用动。

### 编译器

工程不假设系统已装编译器。`build.sh` 会依次找：

1. 环境变量 `CXX`
2. `~/.workbuddy-ai/binaries/w64devkit/bin/g++.exe`（本机使用的便携工具链）
3. `~/w64devkit/bin/g++.exe`、`C:\w64devkit\bin\g++.exe`
4. PATH 上的 `g++` / `x86_64-w64-mingw32-g++` / `clang++`

本机验证环境：**w64devkit 1.23.0 / GCC 14.1.0**。

> **⚠️ 工具链必须放在纯 ASCII 路径下。**
> MinGW 的 `ld.exe` 是原生 Windows 程序，用 ANSI API 解析自己的库搜索路径。
> 如果工具链装在含中文的目录（例如中文用户名下的 `~/.workbuddy-ai/...`），
> `ld` 会找不到 `crt2.o`、`-lmingw32` 等，链接必然失败（编译却能通过，很有迷惑性）。
> 正确做法：`cp -r <工具链目录> /c/w64devkit`，`build.sh` 会优先使用该路径。
>
> 同理，`build.sh` 内部一律使用**相对路径**：汇编器 `as` 同样处理不了含中文的
> 绝对输出路径。而工程目录本身含中文是没问题的。

---

## 二、操作方式

### 电脑键盘

```
下排   Z S X D C V G B H N J M        → C3 起的 12 个半音
上排   Q 2 W 3 E R 5 T 6 Y 7 U I 9 O 0 P → C4 起的 17 个半音
```

- **空格**：临时延音（松开即抬起踏板）
- **`[` / `]`**：整体升降一个八度
- 映射表在 `src/input/KeyboardMap.h`，是纯数据，改键位不用碰逻辑。

### 鼠标 / 触屏

- 点击琴键发声，**越靠近琴键下沿力度越强**（0.33 ~ 1.0）
- 按住拖动 = 滑奏
- 多点触控：每根手指独立一个声部（取决于平台是否向 raylib 暴露触点）

### 界面

- 滚轮 / 拖动「琴盖」区域横向滚动 88 键；`−` `+` 按钮按八度跳转
- 拖动曲目进度条可以跳转播放位置
- **把 `.mid` 文件拖进窗口即可导入**
- 目标稿式深海军蓝 / 香槟金工作区：曲库、夜景钢琴舞台、四张独立控制卡和圆形传输控制
- 顶部 MIDI 状态、设置扩展面板、循环/上一曲/下一曲，以及 120 BPM 节拍器基准
- 乐谱跟练、钢琴卷帘和练习统计共用同一首曲目的音符与播放时钟
- 导出的 MIDI 落在 `piano/exports/`，收藏记录在 `piano/favorites.txt`

---

## 三、功能对照（相对 HTML 原版）

| 功能 | 状态 | 说明 |
|---|---|---|
| 88 键完整显示 | ✅ | 白键 52 + 黑键 36，横向滚动视口 |
| 电脑键盘演奏 | ✅ | 边沿检测，天然规避系统按键重复 |
| 鼠标 / 触屏 + 力度 | ✅ | 按 Y 位置映射力度，与网页版同算法 |
| 延音踏板 | ✅ | 松键后进入 deferred 状态，踏板抬起才释放 |
| 复音 + 偷声 | ✅ | 上限 64，128 个槽位，偷声走 6ms 淡出而非硬切 |
| 离线合成音色 | ✅ | 16 次谐波加法合成 + 低通 + 击弦噪声瞬态 |
| 采样音色 | ✅ | 扫描 `assets/samples/`，按最近锚点变调 |
| 录制 / 回放 | ✅ | 时间戳用采样帧，回放走音频调度器（精确） |
| 导出 MIDI | ✅ | Format 0，含 Tempo 与结尾补 NoteOff / 踏板抬起 |
| 导入 MIDI | ✅ | Format 0/1，拖拽导入 |
| 内置曲目自动演奏 | ✅ | 5 首，旋律 + 伴奏双轨 |
| 跟练判定 | ✅ | 完美 / 不错 / 漏音 / 多余 + 连击 + 准确率 |
| 节拍器 | ✅ | 咔哒声在音频回调里调度，落点精确 |
| 混响 / 主音量 / 压缩 | ✅ | Freeverb + 软削波 + 软拐点压缩 |
| 波形显示 | ✅ | 取自混音器实际输出峰值 |
| 收藏 | ✅ | 落地到 `favorites.txt` |
| 曲目搜索 | ⚠️ | 仅支持 ASCII 输入（raylib 未接 IME） |
| 真实采样音源 | ✅ | FreePats Kawai Upright Piano 的 CC0 FLAC 锚点采样 |
| 设计稿高保真工作区 | ✅ | 1536 × 1024 基准布局，桌面 / 1280 / 1920 截图矩阵已验证 |
| 钢琴卷帘与统计页 | ✅ | 使用同一份曲目事件数据绘制，跟练判定实时更新 |

### 关于采样与许可

工程随包附带 FreePats 的 Kawai Upright Piano 真实录音采样，来源与许可证文件位于
`assets/samples/FREEPATS_SOURCE.json` 和 `assets/samples/FREEPATS_CC0_LICENSE.txt`。
该库由 FreePats 说明为 Kawai 立式钢琴录音，并以 CC0 发布。也可以把自有采样放入
`assets/samples/`，文件名即锚点音高，支持
`C4.wav` / `Ds4.wav` / `Eb4.wav` / `060.wav`，格式支持 WAV / FLAC / MP3 / OGG。
锚点越密音质越好（建议每 3~4 个半音一个锚点）；距最近锚点超过 12 个半音的音
会自动回退到合成音色。

---

## 四、代码结构

```
piano/
├─ CMakeLists.txt
├─ build.sh
├─ src/
│  ├─ core/         纯数据层（不依赖 raylib / miniaudio，可独立单测）
│  │   ├─ Note.h            MIDI 音高工具、音名解析
│  │   ├─ SpscQueue.h       单生产者/单消费者无锁环形队列
│  │   ├─ Event.h           NoteMsg / RecordedEvent
│  │   └─ Song.h/.cpp       NoteEvent / Song，PPQ=480
│  ├─ audio/
│  │   ├─ Dsp.h             TPT 状态变量滤波器、噪声源、钢琴式包络、软削波
│  │   ├─ Wavetable.h       分级波表（mipmap），按基频选层避免混叠
│  │   ├─ Reverb.h          Freeverb 式混响（8 梳状 + 4 全通 / 声道）
│  │   ├─ SampleBank.h/.cpp 采样加载与锚点变调
│  │   ├─ Voice.h/.cpp      单个发声体（合成路径 + 采样路径）
│  │   ├─ Mixer.h/.cpp      ★ 纯 DSP 复音混音器（离线可测）
│  │   ├─ AudioDevice.h/.cpp ★ miniaudio 设备 + 实时调度入口
│  │   ├─ MaConfig.h        miniaudio 编译配置（所有 TU 必须一致）
│  │   └─ MaImpl.cpp        唯一的 MINIAUDIO_IMPLEMENTATION 单元
│  ├─ midi/
│  │   ├─ MidiReader.h/.cpp VLQ、running status、Meta/SysEx、同音高 FIFO 配对
│  │   └─ MidiWriter.h/.cpp Format 0 写出、录制导出、录制转 Song
│  ├─ sequencer/
│  │   ├─ Player.h/.cpp         曲目演奏器（采样点精确）
│  │   ├─ RecordedPlayer.h      录制回放器（含踏板事件）
│  │   ├─ Recorder.h/.cpp       录制
│  │   ├─ Metronome.h/.cpp      节拍器
│  │   └─ PracticeJudge.h/.cpp  跟练判定
│  ├─ input/
│  │   ├─ KeyboardMap.h     键盘映射表（纯数据）
│  │   └─ InputRouter.h/.cpp 键盘 / 鼠标 / 触屏 → 统一音符事件
│  ├─ ui/
│  │   ├─ Theme.h           配色与布局（UI 绘制与命中测试共用同一份几何）
│  │   ├─ FontBook.h/.cpp   中文字体加载（按需收集码点）
│  │   ├─ UiStrings.h       全部界面文案
│  │   ├─ Widgets.h         即时模式控件
│  │   └─ PianoView.h/.cpp  88 键绘制 / 滚动 / 命中测试
│  ├─ songs/BuiltinSongs.h/.cpp
│  ├─ App.h/.cpp            ★ 主控，唯一同时接触两个线程的地方
│  └─ main.cpp
├─ tests/selftest.cpp       离线自测
├─ assets/samples/          放钢琴采样（可选，默认为空）
└─ third_party/
```

---

## 五、几个关键设计点

### 1. 音频线程绝不阻塞

`Mixer::render()` 与其中的 `noteOn/noteOff` 内部**不分配内存、不加锁、不做 I/O**。
主线程与音频线程之间只有两种通信方式：

- **无锁 SPSC 环形队列**（`core/SpscQueue.h`）传递输入事件
- **原子变量**传递参数（音量、混响、引擎模式）与状态回读（峰值、高亮计数）

队列满时直接丢弃并计数（`AudioDevice::droppedMessages()`），绝不阻塞音频线程。

### 2. 定时用采样点，不用 sleep / timer

HTML 版靠 `setInterval(25ms)` + 140ms 前瞻来对抗抖动。C++ 版直接消灭了这个问题：

- 曲目的每个音符被展开成带**绝对采样帧**的事件表
- 音频回调按「距离下一个事件还有多少帧」把整块切成若干段，逐段渲染
- 因此音符起始误差为 **0 个采样点**，而不是「最多一个音频块」

`Player::process()` 的返回值就是这个契约，`AudioDevice` 的回调据此分段。

自测里有一条测试专门验证这点：BPM 120 / PPQ 480 / 48kHz 下，
tick 480 的音符必须在**第 24000 帧**触发，第 23999 帧不能触发。

### 3. Mixer 与 AudioDevice 分离 —— 让音频可测

`Mixer` 是纯 DSP，不含任何设备或线程代码，可以离线渲染。
所以 `--selftest` 能在**不开声卡、不开窗口**的前提下验证：
复音上限、偷声不爆音、延音踏板行为、包络自然衰减、输出无 NaN/Inf 且不越界。

### 4. 复音与偷声

- 软上限 **64 个「仍在按住 / 被踏板挂住」的声部**（正在释放的尾音不占额度）
- 128 个声部槽位，多出的空间容纳释放尾音与去咔哒补丁
- 偷声优先级：已在释放的非用户音 → 已在释放的用户音 → 被踏板挂住的 → 自动播放的 → 用户实时弹奏的（最不该偷）
- 同档内优先偷当前包络电平最低的
- 偷声时**真正释放槽位**，并把被偷声部最后输出的样本交给一个 64 采样
  （约 1.3ms）的线性淡出补丁，与新声部的起音叠加，听不出接缝

> 这里踩过一个坑：最初的做法是把被偷声部置于「6ms 快速释放」状态而保留槽位。
> 结果 `pickVictim()` 每次都会重新挑中那个已释放的声部（已释放的优先级最高），
> 复音上限被完全架空 —— 自测实测累积到 **99 个活跃声部**。
> 现在自测里有一条断言专门守这一点。

### 5. 音色：分层波表 + 击弦瞬态

16 次谐波加法合成，幅度 `1/i^1.65`，每 3 次谐波衰减 0.78。
如果每样本直接求和需要数十次 `sin()`，成本过高；因此预生成 16 张波表
（谐波数 1~16），按基频选取不超过奈奎斯特的最大谐波层 —— 既避免混叠，
又把每样本成本压到一次查表 + 一次线性插值。

另加两处细节：
- 三个轻微失谐（−0.8 / +0.9 / +2.5 音分）的振荡器叠加，制造弦组拍频
- 起音处一段约 35ms 的带通噪声，模拟琴槌接触琴弦的瞬态（不经过包络）

低通截止频率从 `f0×(5+13·v)` 指数滑向 `f0×(2.3+3·v)`，模拟音板对高频的吸收。

### 6. 钢琴式包络

和常见的「按住就持续」的 ADSR 不同：越过 sustain 电平后**继续指数衰减**，
约 3 倍 Decay 时间后自然消失。这才是钢琴的行为——按住琴键不会永远响。

### 7. 跨线程的播放控制

`Player` 的事件表只由音频线程在 `process()` 临界区内访问。UI 线程要改它
（换曲、跳转、变速）时走 `waitForAudioIdle()`：先置 `playing_=false`，
再自旋等待 `busy_` 变 false，确认音频线程已离开临界区后才独占访问。
代价是最多等待一个音频块（约 5ms），不可察觉，但换来完全无锁的正确性。

### 8. MIDI 解析的坑都处理了

- 变长数量（VLQ）、running status
- Meta 事件只取 Tempo（FF 51），其余跳过；SysEx 跳过
- `NoteOn velocity == 0` 等价于 NoteOff
- **同音高重叠**用每音高一个 FIFO 配对，而不是简单的 map（简单 map 会配错对）
- 轨道结束时仍未收到 NoteOff 的音符补一个合理时长
- 拒绝 SMPTE 时间码格式并给出明确错误

---

## 六、自测覆盖

`piano-cli.exe --selftest` 会跑 70+ 项断言，覆盖：

- 音高工具（音名、黑键判定、88 键排版、音名解析、频率）
- 无锁队列（FIFO 顺序、满队列拒绝、环绕读写）
- MIDI 解析（Tempo、running status、vel=0 视为 NoteOff、非法文件拒绝）
- MIDI 往返（Song → 字节流 → Song，音符 / tick / 时长一致）
- 录制导出（采样帧 → tick 换算）
- 混音器（静音输出、发声、**无 NaN/Inf 且不越界**、自然衰减、复音上限、延音踏板、节拍器）
- 演奏器（**采样点精确触发**：第 23999 帧不响、第 24000 帧响）
- 跟练判定（完美 / 不错 / 漏音 / 多余 / 移调）
- 内置曲目数据合法性

---

## 七、开发中踩过的坑（写给未来的自己）

这些坑都花了不少时间，记下来免得重犯：

1. **构建脚本不能只判断「目标文件是否存在」**。
   编译失败时旧 `.o` 还在，会被判成成功，然后链接出一个悄悄过期的 exe ——
   排查时会非常迷惑（改代码没反应）。`build.sh` 现在捕获每个编译进程的
   **退出码**并逐项检查。
2. **MinGW 的 `ld.exe` 是原生 ANSI 程序，工具链不能放在含中文的路径下**
   （见上文「编译器」一节）。`as` 同样处理不了含中文的绝对输出路径，
   所以脚本内部一律用相对路径。
3. **raylib 字体加载失败会静默返回内置默认字体**（baseSize 10 / 95 个 ASCII 字形），
   `IsFontValid` 依然为真 —— 只看有效性会以为加载成功了。
   必须校验**字形覆盖率**（本项目要求 ≥ 90%）。
   另外默认字体是 raylib 的全局资源，**绝对不能对它调用 `UnloadFont`**。
4. **中文字体普遍不含 `▶ ♪ ♫ ♩ ⌨ ✦` 这些装饰符号**（微软雅黑实测都没有），
   界面上会显示成 `?`。实测可用的只有：
   `△ ▽ ○ ● □ ■ ◇ ◆ ☆ ★ ※ § ◎` 和箭头 `↓ →`、`− · × —`。
   加图标前先用 `--screenshot` 跑一次，看启动日志里的「不存在的码点」。
5. **偷声必须真正释放槽位**，否则 `pickVictim()` 会反复挑中同一个已释放声部，
   复音上限被架空（实测累积到 99 个活跃声部）。详见「复音与偷声」。
6. **MSYS 的 `timeout` 对原生 Windows GUI 程序会误报 124**，
   别用它判断程序是否卡死 —— 直接测 `date` 差值更可靠。
7. **Windows 脚本文件的编码不是小事**（这一条是被用户当场踩到才补的）：
   - `.ps1` **必须带 UTF-8 BOM**，否则 Windows PowerShell 5.1 按 GBK 解码，
     中文全乱、直接语法报错。
   - `.cmd` **必须存 GBK**，否则 cmd.exe 显示乱码。
   - 光有 `.sh` 不够 —— Windows 用户默认在 PowerShell 里，`.sh` 根本跑不起来。
     要么提供 `.ps1`，要么在文档第一行就写清楚怎么切到 bash。

   教训：**交付跨平台工程时，"怎么跑起来" 要和代码同等对待**，
   不能只在自己顺手的那个终端里验证过就算完。
8. **控制台中文乱码是两处叠加的**，只修一处没用：
   - 程序写的是 UTF-8 字节，Windows 控制台默认按 **OEM 代码页**（中文系统 CP936）
     解释 → 变成「閿挗鐞?」。解决：`SetConsoleOutputCP(CP_UTF8)`，
     退出时恢复原值（见 `core/Console.h`）。
   - **PowerShell 5.1 还会用它自己的 `[Console]::OutputEncoding` 再解码一次**
     原生程序的输出。即使子进程切了控制台代码页，PowerShell 仍可能按 GBK 解。
     解决：`build.ps1` 在调用 exe 前后临时对齐 `[Console]::OutputEncoding`。
   - 顺带一提：`<windows.h>` 不能直接进包含 raylib.h 的文件 ——
     wingdi.h 里的 `Rectangle()` 函数会和 raylib 的 `Rectangle` 结构体撞名，
     所以控制台代码单独放在 `core/Console.cpp` 里编译。
9. **文档里的第一条命令，必须在「一台什么都没配过的 Windows」上能跑通。**
   我一开始把 `.\build.ps1` 写在最前面，结果用户直接撞上 Windows 默认的
   Restricted 执行策略。正确做法是把**零配置就能用**的入口（`build.cmd`）
   放在第一位，把需要额外设置的入口放到后面并写清怎么设置。
10. **批处理文件要保持纯 ASCII。** 我一开始给 `build.cmd` 写了中文注释并
    按 GBK 保存，结果在控制台代码页不是 936 的环境里，连 `REM` 注释里的中文
    都会把 cmd.exe 的解析器搞乱，报出 `'保存为' 不是内部或外部命令` 这种错。
    现在 `.cmd` 只做转发（纯 ASCII），所有中文提示都交给 `build.ps1`。

---

## 八、已知限制

1. **真实采样按最近锚点变调**。当前随包提供 FreePats 的 CC0 FLAC 锚点采样；缺少锚点或解码失败时自动回退到离线合成音色。
2. **搜索框只支持 ASCII**。raylib 未接输入法，中文曲名搜索会失效；曲目下拉不受影响。
3. **多点触控取决于平台**。raylib 桌面端通过 `GetTouchPointCount()` 暴露触点，
   部分 Windows 环境可能只上报单点；鼠标路径始终可用。
4. **导入的 MIDI 只取首个 Tempo**。变速的 MIDI 会被按单一 BPM 播放。
5. **跟练只判定旋律轨**（有旋律轨时）。导入的单轨文件按通道区分旋律/伴奏。
6. 控制台窗口会保留（`--selftest` 需要它输出结果，运行日志也在这里）。

---

## 九、后续可以怎么加

- **真实 MIDI 键盘输入**：接 RtMidi，把 `InputRouter` 的第三个来源补上，
  `InputSink` 接口不用改
- **踏板共鸣 / 半踏板**：`Voice` 里加共鸣弦组，`Mixer` 里加踏板深度参数
- **更细的力度曲线**：目前是线性映射，可换成 `velocity^k` 或分段曲线
- **曲目编辑**：`Song` 已是纯数据，配一个网格编辑器即可
- **录音落盘**：`writeRecordingMidi` 已经在写文件，加个工程文件格式很容易

\n
