@echo off
setlocal
cd /d "%~dp0"

rem Keep this file ASCII-only: cmd.exe parses the script in the console code page,
rem so UTF-8 Chinese text here would print as mojibake (and can break parsing).

where cmake >nul 2>nul
if errorlevel 1 (
    call :showError "CMake is not available" "cmake not found. Install CMake and add it to PATH."
    exit /b 1
)

set "BUILD_LOG=%TEMP%\PmxStudio_build_%RANDOM%.log"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >"%BUILD_LOG%" 2>&1
set "BUILD_EXIT=%errorlevel%"
type "%BUILD_LOG%"
if not "%BUILD_EXIT%"=="0" (
    call :showError "CMake configuration failed" "%BUILD_LOG%"
    del /q "%BUILD_LOG%" >nul 2>nul
    exit /b %BUILD_EXIT%
)

cmake --build build --config Release >>"%BUILD_LOG%" 2>&1
set "BUILD_EXIT=%errorlevel%"
type "%BUILD_LOG%"
if not "%BUILD_EXIT%"=="0" (
    call :showError "Build failed" "%BUILD_LOG%"
    del /q "%BUILD_LOG%" >nul 2>nul
    exit /b %BUILD_EXIT%
)

echo.
echo Build completed:
echo   %~dp0pmxstudio.exe
echo   %~dp0build\Release\pmx_tests.exe
call :showSuccess "Build completed successfully"
del /q "%BUILD_LOG%" >nul 2>nul
endlocal
exit /b 0

:showError
set "POPUP_TITLE=%~1"
set "POPUP_CONTENT=%~2"
powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -Command "Add-Type -AssemblyName PresentationFramework; $title=$env:POPUP_TITLE; $content=$env:POPUP_CONTENT; if(Test-Path -LiteralPath $content){$content=Get-Content -Raw -LiteralPath $content}; [System.Windows.MessageBox]::Show([string]$content,[string]$title,[System.Windows.MessageBoxButton]::OK,[System.Windows.MessageBoxImage]::Error) | Out-Null"
exit /b 0

:showSuccess
set "POPUP_TITLE=%~1"
set "POPUP_CONTENT=Build completed successfully. %~dp0pmxstudio.exe %~dp0build\Release\pmx_tests.exe"
powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -Command "Add-Type -AssemblyName PresentationFramework; [System.Windows.MessageBox]::Show([string]$env:POPUP_CONTENT,[string]$env:POPUP_TITLE,[System.Windows.MessageBoxButton]::OK,[System.Windows.MessageBoxImage]::Information) | Out-Null"
exit /b 0
