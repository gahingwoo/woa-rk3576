@echo off
net session >nul 2>&1 || (echo Run this from an ADMINISTRATOR command prompt. & exit /b 1)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0net-debug.ps1"
