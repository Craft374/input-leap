@echo off
setlocal
cd /d "%~dp0"

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0clean_build.ps1"
set "build_status=%ERRORLEVEL%"

if not "%build_status%"=="0" (
    echo.
    echo Build failed. See the error above.
) else (
    echo.
    echo Build completed: %~dp0build\input-leap-install
)

pause
exit /b %build_status%
