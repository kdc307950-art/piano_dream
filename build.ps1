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
if ($Clean -and (Test-Path 'build')) { Remove-Item -Recurse -Force 'build' }
& $cmake.Source -S . -B build -DCMAKE_BUILD_TYPE=Release -DPIANO_TSAN=OFF
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $cmake.Source --build build --parallel ([Math]::Max(1, $Jobs))
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($SelfTest) {
    & (Join-Path $Root 'piano-cli.exe') '--selftest'
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
if ($Run) { Start-Process -FilePath (Join-Path $Root 'piano.exe') -WorkingDirectory $Root }
