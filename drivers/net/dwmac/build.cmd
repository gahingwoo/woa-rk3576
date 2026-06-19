@echo off
rem Build dwmac.sys for Windows on ARM64. Run from an EWDK build environment
rem (LaunchBuildEnv.cmd) or a WDK Developer Command Prompt; builds natively on
rem ARM64 Windows. See ..\..\..\docs\BUILDING.md.   Usage: build.cmd [Debug|Release]
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release
where msbuild >nul 2>nul
if errorlevel 1 (
    echo [!] msbuild not found. Launch the EWDK build env or a WDK Developer Prompt.
    exit /b 1
)
msbuild dwmac.vcxproj /p:Configuration=%CONFIG% /p:Platform=ARM64 /m
if errorlevel 1 ( echo [!] build failed & exit /b 1 )
echo.
echo [+] Output: ARM64\%CONFIG%\dwmac\dwmac.sys (and .inf/.cat)
endlocal
