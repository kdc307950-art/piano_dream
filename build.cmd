@echo off
REM ===========================================================================
REM build.cmd -- double-click to build. Forwards everything to build.ps1.
REM
REM   build.cmd                 build to build\piano.exe
REM   build.cmd -SelfTest       build, then run the offline self-test
REM   build.cmd -Run            build, then launch the GUI
REM   build.cmd -Clean          wipe build\ first
REM
REM This file is deliberately ASCII-only.
REM   cmd.exe parses batch files using the console code page. If the file
REM   encoding does not match the console code page, non-ASCII bytes (even in
REM   REM comments) can break the parser and produce bogus errors like
REM   "'xxx' is not recognized as an internal or external command".
REM   All Chinese messages live in build.ps1 instead, which handles encoding
REM   properly via its UTF-8 BOM.
REM
REM Also note: it passes -ExecutionPolicy Bypass, so it works even when the
REM machine policy forbids running .ps1 scripts (the Windows default).
REM ===========================================================================
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
set RC=%ERRORLEVEL%

if not "%RC%"=="0" (
    echo.
    echo Build failed, exit code %RC%
)

REM Pause only when double-clicked (no arguments), so scripted calls don't hang
if "%~1"=="" pause

exit /b %RC%
