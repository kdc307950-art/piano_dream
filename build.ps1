# ============================================================================
# build.ps1 —— 在 PowerShell 里编译 Grand Studio（C++ 88 键钢琴模拟器）
#
#   .\build.ps1                  编译到 build\piano.exe
#   .\build.ps1 -Clean           先清空 build\
#   .\build.ps1 -SelfTest        编译后跑离线自测
#   .\build.ps1 -Run             编译后启动界面
#
# 也接受 bash 风格的参数：.\build.ps1 --selftest / --run / --clean
#
# 与 build.sh 等价，不依赖 bash / Git Bash，纯 PowerShell 实现。
#
# 注意：本文件必须保存为「UTF-8 with BOM」。
#   Windows PowerShell 5.1 读 .ps1 时，如果没有 BOM，会按系统 ANSI 代码页
#   （中文系统是 GBK）解码 —— 中文注释和字符串会全部乱码，直接报语法错误。
# ============================================================================
#Requires -Version 5.1

[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$Run,
    [switch]$SelfTest,
    [int]$Jobs = 4,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Rest
)

$ErrorActionPreference = 'Stop'

# ---- 兼容 --selftest / --run / --clean 这类写法 ----
if ($Rest) {
    foreach ($a in $Rest) {
        switch ($a.ToLower()) {
            '--clean'    { $Clean = $true }
            '--run'      { $Run = $true }
            '--selftest' { $SelfTest = $true }
            '-t'         { $SelfTest = $true }
            default {
                Write-Host "未知参数：$a" -ForegroundColor Yellow
                Write-Host '可用参数：-Clean  -Run  -SelfTest  -Jobs <N>'
                exit 2
            }
        }
    }
}

$Root = $PSScriptRoot
if (-not $Root) { $Root = (Get-Location).Path }
Set-Location $Root

# ---------------------------------------------------------------- 工具链定位
#
# 重要：MinGW 的 ld.exe 是原生 Windows 程序，用 ANSI API 解析自己的库搜索路径。
# 如果工具链装在含非 ASCII 字符的目录下（例如中文用户名），ld 会找不到
# crt2.o、-lmingw32 等，链接必然失败。所以优先选纯 ASCII 路径下的安装。
function Find-Cxx {
    if ($env:CXX -and (Test-Path $env:CXX)) { return (Resolve-Path $env:CXX).Path }

    $candidates = @(
        'C:\w64devkit\bin\g++.exe',
        'C:\tools\w64devkit\bin\g++.exe',
        'C:\mingw64\bin\g++.exe',
        (Join-Path $env:USERPROFILE 'w64devkit\bin\g++.exe')
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return (Resolve-Path $c).Path }
    }

    foreach ($name in @('g++', 'x86_64-w64-mingw32-g++', 'clang++')) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
    }

    # 兜底：便携工具链可能被放在带中文的目录里（编译能过，但链接会失败）
    $fallback = Get-ChildItem -Path 'C:\Users' -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName '.workbuddy-ai\binaries\w64devkit\bin\g++.exe' } |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($fallback) { return (Resolve-Path $fallback).Path }

    return $null
}

$Cxx = Find-Cxx
if (-not $Cxx) {
    Write-Host '错误：找不到 C++ 编译器。' -ForegroundColor Red
    Write-Host '      请把 w64devkit 解压到 C:\w64devkit，或设置环境变量 CXX。'
    exit 1
}

# g++ 需要能找到同目录下的 as / ld / ar
$CxxDir = Split-Path $Cxx -Parent
if ($env:PATH -notlike "*$CxxDir*") { $env:PATH = "$CxxDir;$env:PATH" }
$CxxVersion = (& $Cxx -dumpversion) 2>$null

# ------------------------------------------------------------------ 依赖检查
foreach ($f in @('third_party\raylib\include\raylib.h',
                 'third_party\raylib\lib\libraylib.a',
                 'third_party\miniaudio\miniaudio.h')) {
    if (-not (Test-Path (Join-Path $Root $f))) {
        Write-Host "错误：缺少依赖文件 $f" -ForegroundColor Red
        exit 1
    }
}

# --------------------------------------------------------------------- 准备
if ($Clean -and (Test-Path 'build')) { Remove-Item -Recurse -Force 'build' }
New-Item -ItemType Directory -Force -Path 'build\obj', 'build\logs' | Out-Null

$sources = Get-ChildItem -Path 'src', 'tests' -Recurse -Filter '*.cpp' |
    ForEach-Object { $_.FullName.Substring($Root.Length + 1).Replace('\', '/') } |
    Sort-Object

Write-Host "编译器 : $Cxx"
Write-Host "版本   : $CxxVersion"
Write-Host "源文件 : $($sources.Count) 个"
if ($CxxDir -match '[^\x00-\x7F]') {
    Write-Host '警告   : 工具链路径含非 ASCII 字符，链接很可能失败：' -ForegroundColor Yellow
    Write-Host "         $CxxDir" -ForegroundColor Yellow
    Write-Host '         建议把 w64devkit 整个目录复制到 C:\w64devkit' -ForegroundColor Yellow
}
Write-Host ('-' * 60)

# --------------------------------------------------------------------- 编译
#
# 一律使用相对路径 —— 汇编器 as 处理不了含中文的绝对输出路径。
#
# 这里用 System.Diagnostics.Process 而不是 Start-Process：
# Windows PowerShell 5.1 的 Start-Process 在复制父进程环境变量时，
# 遇到同时存在 HTTP_PROXY / http_proxy（大小写不同的同名变量）会直接抛
# 「Item has already been added」，完全没法用。ProcessStartInfo 在
# UseShellExecute=$false 时子进程自动继承环境，不经过那条有 bug 的路径。
function Start-Child([string]$exe, [string[]]$childArgs, [string]$workDir) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = (($childArgs | ForEach-Object {
        if ($_ -match '\s') { '"' + $_ + '"' } else { $_ }
    }) -join ' ')
    $psi.WorkingDirectory = $workDir
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true

    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $psi
    [void]$p.Start()

    return [pscustomobject]@{
        Proc = $p
        Out  = $p.StandardOutput.ReadToEndAsync()
        Err  = $p.StandardError.ReadToEndAsync()
    }
}

$includes = @('-Isrc', '-Ithird_party/raylib/include', '-Ithird_party/miniaudio')
$flags = @('-std=c++20', '-O2', '-Wall', '-Wextra', '-Wno-unused-parameter', '-fno-strict-aliasing')

$objects = @()
$failed = @()
$batchSize = [Math]::Max(1, $Jobs)

for ($i = 0; $i -lt $sources.Count; $i += $batchSize) {
    $last = [Math]::Min($i + $batchSize - 1, $sources.Count - 1)
    $batch = $sources[$i..$last]
    $running = @()

    foreach ($src in $batch) {
        $flat = $src.Replace('/', '_')
        $obj = "build/obj/$flat.o"
        $objects += $obj
        $a = @()
        $a += $flags
        $a += $includes
        $a += @('-c', $src, '-o', $obj)
        $r = Start-Child -exe $Cxx -childArgs $a -workDir $Root
        $r | Add-Member -NotePropertyName Src  -NotePropertyValue $src  -Force
        $r | Add-Member -NotePropertyName Flat -NotePropertyValue $flat -Force
        $running += $r
    }

    foreach ($r in $running) { $r.Proc.WaitForExit() }
    foreach ($r in $running) {
        $errText = $r.Err.Result
        $outText = $r.Out.Result
        if ($errText) { Set-Content -Path "build/logs/$($r.Flat).err.log" -Value $errText -Encoding UTF8 }
        if ($outText) { Set-Content -Path "build/logs/$($r.Flat).out.log" -Value $outText -Encoding UTF8 }

        if ($r.Proc.ExitCode -ne 0) {
            Write-Host "  FAIL  $($r.Src)  (编译器返回 $($r.Proc.ExitCode))" -ForegroundColor Red
            if ($errText) {
                ($errText -split "`n" | Select-Object -First 40) |
                    ForEach-Object { Write-Host "        $_" }
            }
            $failed += $r.Src
        }
        else {
            Write-Host "  ok    $($r.Src)"
        }
    }
}

# 逐项检查退出码。
# 注意：不能只判断目标文件是否存在 —— 编译失败时旧的目标文件仍在，
# 会被误判成成功，然后链接进一个「悄悄过期」的可执行文件。
if ($failed.Count -gt 0) {
    Write-Host ('-' * 60)
    Write-Host "编译失败 $($failed.Count) 个文件。完整日志在 build\logs\ 下。" -ForegroundColor Red
    exit 1
}

# --------------------------------------------------------------------- 链接
#
# 产出两个可执行文件，各自面向一种用法：
#
#   piano.exe      GUI 子系统（-mwindows）—— 双击运行不会弹出黑色命令框，
#                  这才是给用户「玩」的成品。
#   piano-cli.exe  控制台子系统 —— 保留 stdout，供 --selftest 与诊断输出。
#
# 之所以要两个而不是一个：Windows 的子系统在链接时写死在 PE 头里，
# GUI 程序的 stdout 没有地方可去（printf 等于丢进黑洞），
# 而自测结果必须能打印出来。
#
# 刻意输出到「工程根目录」而不是 build\ 下：
#   - 用户一眼就能看到，双击即可
#   - 双击时工作目录就是工程根，assets\samples、exports\ 都能自然找到
Write-Host ('-' * 60)
Write-Host '链接 piano.exe (GUI) 与 piano-cli.exe (控制台) ...'

$linkCommon = @()
$linkCommon += $objects
$linkCommon += @(
    '-Lthird_party/raylib/lib', '-lraylib',
    '-lopengl32', '-lgdi32', '-lwinmm', '-lole32', '-loleaut32', '-luuid',
    '-lshell32', '-luser32',
    '-static', '-static-libgcc', '-static-libstdc++'
)

$targets = @(
    [pscustomobject]@{ Out = 'piano.exe';     Extra = @('-mwindows') },
    [pscustomobject]@{ Out = 'piano-cli.exe'; Extra = @() }
)

$linkLog = @()
foreach ($t in $targets) {
    $a = @()
    $a += $linkCommon
    $a += @('-o', $t.Out)
    $a += $t.Extra
    $out = & $Cxx @a 2>&1
    if ($out) { $linkLog += "[$($t.Out)]"; $linkLog += $out }
    if ($LASTEXITCODE -ne 0) {
        Write-Host "链接失败：$($t.Out)" -ForegroundColor Red
        if ($out) { $out | Select-Object -First 40 | ForEach-Object { Write-Host "  $_" } }
        if ($linkLog) { $linkLog | Set-Content -Path 'build\logs\link.log' -Encoding UTF8 }
        exit 1
    }
}
if ($linkLog) { $linkLog | Set-Content -Path 'build\logs\link.log' -Encoding UTF8 }

$exe = Join-Path $Root 'piano.exe'
$cli = Join-Path $Root 'piano-cli.exe'
$size = [Math]::Round((Get-Item $exe).Length / 1MB, 2)
Write-Host "完成：" -ForegroundColor Green
Write-Host "  $exe  ($size MB)   <- 双击这个就能玩" -ForegroundColor Green
Write-Host "  $cli  (自测 / 诊断用)" -ForegroundColor DarkGray

# --------------------------------------------------------------------- 运行
#
# 中文乱码的第二个来源：PowerShell 5.1 会用它自己的 [Console]::OutputEncoding
# 去解码原生程序输出的字节。中文系统的控制台是 CP936(GBK)，于是我们的
# UTF-8 字节被当成 GBK 解 ——「钢琴」变成「閿挗鐞?」。
#
# 最稳的做法不是去猜控制台的代码页，而是**明确告诉 .NET 按 UTF-8 解码**：
# ProcessStartInfo.StandardOutputEncoding。这样与用户的控制台设置完全无关。
function Invoke-NativeUtf8([string]$exePath, [string]$arguments) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exePath
    $psi.Arguments = $arguments
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $utf8 = New-Object System.Text.UTF8Encoding $false
    $psi.StandardOutputEncoding = $utf8
    $psi.StandardErrorEncoding = $utf8

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    [void]$proc.Start()
    $outText = $proc.StandardOutput.ReadToEnd()
    $errText = $proc.StandardError.ReadToEnd()
    $proc.WaitForExit()

    # 控制台也切到 UTF-8，否则 Write-Host 又会按 GBK 编码写出去
    $prevEnc = [Console]::OutputEncoding
    try {
        [Console]::OutputEncoding = $utf8
        if ($outText) { Write-Host $outText.TrimEnd() }
        if ($errText) { Write-Host $errText.TrimEnd() }
    }
    finally {
        [Console]::OutputEncoding = $prevEnc
    }
    return $proc.ExitCode
}

if ($SelfTest) {
    Write-Host ('-' * 60)
    # 自测跑控制台版，否则结果没地方打印
    $code = Invoke-NativeUtf8 -exePath $cli -arguments '--selftest'
    exit $code
}

if ($Run) {
    Write-Host ('-' * 60)
    Write-Host '启动界面...' -ForegroundColor Green
    # 不能用 Start-Process —— 它在环境里同时有 HTTP_PROXY / http_proxy 时会抛
    # 「Item has already been added」。ProcessStartInfo 不经过那条有 bug 的路径。
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.WorkingDirectory = $Root
    $psi.UseShellExecute = $false
    [void][System.Diagnostics.Process]::Start($psi)
}
