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
foreach ($a in $Rest) {
    switch ($a.ToLower()) {
        '--clean' { $Clean = $true }
        '--run' { $Run = $true }
        '--selftest' { $SelfTest = $true }
        '-t' { $SelfTest = $true }
        default { throw "未知参数：$a" }
    }
}
$Root = $PSScriptRoot
Set-Location $Root
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) { throw '找不到 CMake。请安装 CMake 3.16 或更高版本并加入 PATH。' }

# 非 ASCII 路径兜底：CMake 以 UTF-8 写构建文件，而 Ninja / MinGW 按系统 ANSI
# 代码页（GBK）解析路径 —— 中文路径会让 ninja 在 FindFirstFileExA 阶段直接报
# "The filename, directory name, or volume label syntax is incorrect"。
# 检测到非 ASCII 路径时，用 subst 把工程映射到一个纯 ASCII 盘符再构建；
# 产物经过映射仍写回真实目录，对用户完全透明。
$SubstDrive = $null
if ($Root -match '[^\x20-\x7E]') {
    foreach ($letter in [char[]]'PQRSTUVWXYZ') {
        if (-not (Test-Path "${letter}:\")) { $SubstDrive = "${letter}:"; break }
    }
    if (-not $SubstDrive) { throw '工程路径含非 ASCII 字符，且没有空闲盘符可做 subst 映射。' }
    subst $SubstDrive $Root | Out-Null
    Write-Host "工程路径含非 ASCII 字符，已通过 $SubstDrive 映射构建（等价于原路径）"
    Set-Location "$SubstDrive\"
    # 旧 build 缓存若记录的是原始中文路径，CMake 拒绝跨路径复用，直接重建
    $cache = "$SubstDrive\build\CMakeCache.txt"
    if ((Test-Path $cache) -and -not (Select-String -Path $cache -Pattern ([regex]::Escape("$SubstDrive/")) -Quiet)) {
        Remove-Item -Recurse -Force "$SubstDrive\build"
    }
}

$exitCode = 0
$launch = $false
try {
    if ($Clean -and (Test-Path 'build')) { Remove-Item -Recurse -Force 'build' }
    # 全新配置且本机有 Ninja 时显式指定生成器（与 build.sh 一致），
    # 否则 CMake 在无 VS 的机器上会误选 nmake；已有缓存则沿用缓存里的生成器。
    $generatorArgs = @()
    if (-not (Test-Path 'build\CMakeCache.txt') -and (Get-Command ninja -ErrorAction SilentlyContinue)) {
        $generatorArgs = @('-G', 'Ninja')
    }
    & $cmake.Source -S . -B build @generatorArgs -DCMAKE_BUILD_TYPE=Release -DPIANO_TSAN=OFF
    if ($LASTEXITCODE -ne 0) { $exitCode = $LASTEXITCODE } else {
        & $cmake.Source --build build --parallel ([Math]::Max(1, $Jobs))
        if ($LASTEXITCODE -ne 0) { $exitCode = $LASTEXITCODE }
    }
    if ($exitCode -eq 0 -and $SelfTest) {
        & (Join-Path $Root 'piano-cli.exe') '--selftest'
        if ($LASTEXITCODE -ne 0) { $exitCode = $LASTEXITCODE }
    }
    if ($exitCode -eq 0 -and $Run) { $launch = $true }
} finally {
    if ($SubstDrive) {
        Set-Location $Root
        subst $SubstDrive /d | Out-Null
    }
}
if ($launch) { Start-Process -FilePath (Join-Path $Root 'piano.exe') -WorkingDirectory $Root }
exit $exitCode
