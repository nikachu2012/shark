@echo off
rem build_win.bat - entry point for cmd.exe. See tools\build_win.ps1 (Japanese).
rem   tools\build_win.bat [build|test|clean]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_win.ps1" %*
exit /b %errorlevel%
