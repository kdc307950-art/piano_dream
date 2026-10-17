#Requires -Version 5.1
[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$Run,
    [switch]$SelfTest,
    [switch]$Package,
    [switch]$Installer,
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
        '--package' { $Package = $true }
        '--installer' { $Installer = $true }
        '-t' { $SelfTest = $true }
        default { throw "未知参数：$a" }
    }
}
$Root = $PSScriptRoot
Set-Location $Root
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) { throw '找不到 CMake。请安装 CMake 3.16 或更高版本并加入 PATH。' }
$ctest = Get-Command ctest -ErrorAction SilentlyContinue

# Keep the embedded font corpus in sync with user-facing strings. Python is
# optional for consumers of a pre-generated source tree; when it is absent we
# keep the checked-in corpus and continue with the normal build.
$glyphPython = Get-Command python -ErrorAction SilentlyContinue
if (-not $glyphPython) { $glyphPython = Get-Command py -ErrorAction SilentlyContinue }
if ($glyphPython) {
    & $glyphPython.Source tools/generate_glyphs.py
    if ($LASTEXITCODE -ne 0) { throw '字形文案生成失败。' }
} else {
    Write-Warning '找不到 Python，跳过字形文案生成；如新增中文文案请手动运行 tools/generate_glyphs.py。'
}

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
    if ($SubstDrive) {
        $substOutput = subst $SubstDrive $Root 2>&1
        if ($LASTEXITCODE -eq 0 -and (Test-Path "$SubstDrive\")) {
            Write-Host "工程路径含非 ASCII 字符，已通过 $SubstDrive 映射构建（等价于原路径）"
            Set-Location "$SubstDrive\"
            # 旧 build 缓存若记录的是原始中文路径，CMake 拒绝跨路径复用，直接重建
            $cache = "$SubstDrive\build\CMakeCache.txt"
            if ((Test-Path $cache) -and -not (Select-String -Path $cache -Pattern ([regex]::Escape("$SubstDrive/")) -Quiet)) {
                Remove-Item -Recurse -Force "$SubstDrive\build"
            }
        } else {
            Write-Warning "无法创建临时 ASCII 盘符映射，改用原始工程路径继续构建。CMake 已关闭 Ninja 目录自动扫描以兼容中文路径。"
            $SubstDrive = $null
        }
    } else {
        Write-Warning '没有空闲盘符可做 subst 映射，改用原始工程路径继续构建。'
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
        if ($ctest) {
            # Run through CTest while still on the ASCII subst drive. This keeps
            # the generated test command and its mapped executable in the same
            # namespace on Windows projects whose real path contains Chinese.
            & $ctest.Source --test-dir build --output-on-failure
        } else {
            & (Join-Path (Get-Location) 'piano-cli.exe') '--selftest'
        }
        if ($LASTEXITCODE -ne 0) { $exitCode = $LASTEXITCODE }
    }
    if ($exitCode -eq 0 -and $Package) {
        # Run CPack through the same ASCII subst path used for the build; a
        # direct cmake --build from a Chinese source path cannot reuse Ninja's
        # mapped cache reliably.
        & $cmake.Source --build build --target package --parallel ([Math]::Max(1, $Jobs))
        if ($LASTEXITCODE -ne 0) { $exitCode = $LASTEXITCODE }
        if ($exitCode -eq 0) {
            $packages = @(Get-ChildItem (Join-Path (Get-Location) 'build') -Filter 'GrandStudioPiano-*.zip' -File)
            if ($packages.Count -eq 0) { throw 'CPack 未生成 GrandStudioPiano ZIP 包。' }
        }
    }
    if ($exitCode -eq 0 -and $Installer) {
        $stage = Join-Path (Get-Location) 'build\installer-stage'
        if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
        & $cmake.Source --install build --prefix $stage
        if ($LASTEXITCODE -ne 0) { $exitCode = $LASTEXITCODE }
        if ($exitCode -eq 0) {
            $requiredStageFiles = @(
                'piano.exe', 'piano-cli.exe', 'README.md', 'README_EN.md',
                'THIRD_PARTY_LICENSES.md', 'assets\samples\FREEPATS_SOURCE.json',
                'assets\samples\FREEPATS_CC0_LICENSE.txt')
            foreach ($relative in $requiredStageFiles) {
                if (-not (Test-Path (Join-Path $stage $relative))) {
                    throw "安装暂存目录缺少必需文件：$relative"
                }
            }
            $makensis = Get-Command makensis -ErrorAction SilentlyContinue
            if (-not $makensis) {
                foreach ($candidate in @(
                    'C:\Program Files (x86)\NSIS\makensis.exe',
                    'C:\Program Files\NSIS\makensis.exe',
                    (Join-Path $Root 'build\nsis\nsis-3.13\Bin\makensis.exe'),
                    (Join-Path $Root 'build\nsis\nsis-3.13\makensis.exe'),
                    (Join-Path $Root 'build\nsis\Bin\makensis.exe'),
                    (Join-Path $Root 'build\nsis\makensis.exe'))) {
                    if (Test-Path $candidate) { $makensis = Get-Command $candidate; break }
                }
            }
            if (-not $makensis) {
                throw '找不到 makensis.exe。请安装 NSIS 3.x 后重试，或把 makensis.exe 加入 PATH。'
            }
            $installerScript = Join-Path (Get-Location) 'installer\GrandStudioPiano.nsi'
            $outputDir = Join-Path (Get-Location) 'build'
            $installerRoot = Join-Path (Get-Location) 'installer'
            & $makensis.Source "/DINPUT_ROOT=$stage" "/DOUTPUT_DIR=$outputDir" "/DINSTALLER_ROOT=$installerRoot" '/DPRODUCT_VERSION=1.0.0' $installerScript
            if ($LASTEXITCODE -ne 0) { $exitCode = $LASTEXITCODE }
        }
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
