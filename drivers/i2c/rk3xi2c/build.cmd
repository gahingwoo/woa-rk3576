@echo off
rem ---------------------------------------------------------------------------
rem Build rk3xi2c.sys for Windows on ARM64. Run from an EWDK build environment
rem (LaunchBuildEnv.cmd) or a Developer Command Prompt with the WDK installed.
rem On an ARM64 Windows box this builds natively; see ..\..\..\docs\BUILDING.md.
rem
rem Usage:   build.cmd [Debug|Release]
rem ---------------------------------------------------------------------------
setlocal

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release

where msbuild >nul 2>nul
if errorlevel 1 (
    echo [!] msbuild not found. Launch the EWDK build env or a WDK Developer Prompt.
    exit /b 1
)

msbuild rk3xi2c.vcxproj /p:Configuration=%CONFIG% /p:Platform=ARM64 /m
if errorlevel 1 (
    echo [!] build failed
    exit /b 1
)

echo.
echo [+] Output: ARM64\%CONFIG%\rk3xi2c\rk3xi2c.sys (and .inf/.cat)
endlocal
