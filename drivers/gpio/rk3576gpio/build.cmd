@echo off
rem ---------------------------------------------------------------------------
rem Build rk3576gpio.sys for Windows on ARM64.
rem
rem Prerequisite: run this from an EWDK build environment, i.e. mount/extract the
rem EWDK ISO and run LaunchBuildEnv.cmd first, OR have Visual Studio + WDK
rem installed and run this from a "Developer Command Prompt".
rem
rem Usage:   build.cmd [Debug|Release]
rem ---------------------------------------------------------------------------
setlocal

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release

where msbuild >nul 2>nul
if errorlevel 1 (
    echo [!] msbuild not found. Launch the EWDK build env ^(LaunchBuildEnv.cmd^)
    echo     or open a Developer Command Prompt for VS with the WDK installed.
    exit /b 1
)

msbuild rk3576gpio.vcxproj /p:Configuration=%CONFIG% /p:Platform=ARM64 /m
if errorlevel 1 (
    echo [!] build failed
    exit /b 1
)

echo.
echo [+] Output: ARM64\%CONFIG%\rk3576gpio\rk3576gpio.sys (and .inf/.cat)
endlocal
