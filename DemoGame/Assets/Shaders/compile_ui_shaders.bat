@echo off
setlocal

if "%~1"=="" (
    call "%~f0" DX11
    if errorlevel 1 exit /b 1
    call "%~f0" DX12
    if errorlevel 1 exit /b 1
    exit /b 0
)

set "FXC="
if defined FBZZ_FXC set "FXC=%FBZZ_FXC%" & goto :found_fxc
for /f "delims=" %%F in ('where fxc.exe 2^>nul') do set "FXC=%%F" & goto :found_fxc
if defined WindowsSdkDir if defined WindowsSDKVersion if exist "%WindowsSdkDir%bin\%WindowsSDKVersion%x64\fxc.exe" set "FXC=%WindowsSdkDir%bin\%WindowsSDKVersion%x64\fxc.exe" & goto :found_fxc

:found_fxc
if "%FXC%"=="" (
    echo fxc.exe was not found. Install the Windows SDK or set FBZZ_FXC to fxc.exe.
    exit /b 1
)
set SRC=%~dp0
if /I "%~1"=="DX12" (
    set OUT=%~dp0compiled_dx12
    set BACKEND_DEFINE=/D FBZZ_BACKEND_DX12=1
) else (
    set OUT=%~dp0compiled
    set BACKEND_DEFINE=/D FBZZ_BACKEND_DX11=1
)
set LOG=%~dp0compile_ui_log.txt
set SRCDIR=%SRC:~0,-1%
set INC=/nologo /I "%SRCDIR%"

if not exist "%OUT%" mkdir "%OUT%"

echo [compile_ui_shaders] %date% %time% > "%LOG%"

REM =========================================================================
REM UI shaders
REM =========================================================================

echo [UI] UISprite.hlsl...
"%FXC%" %INC% %BACKEND_DEFINE% /T vs_5_0 /E VSMain /Fo "%OUT%\UI.UISprite.vs.cso" "%SRC%UI\UISprite.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] UI/UISprite VS & goto :error )
"%FXC%" %INC% %BACKEND_DEFINE% /T ps_5_0 /E PSMain /Fo "%OUT%\UI.UISprite.ps.cso" "%SRC%UI\UISprite.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] UI/UISprite PS & goto :error )

echo [UI] UIText.hlsl...
"%FXC%" %INC% %BACKEND_DEFINE% /T vs_5_0 /E VSMain /Fo "%OUT%\UI.UIText.vs.cso" "%SRC%UI\UIText.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] UI/UIText VS & goto :error )
"%FXC%" %INC% %BACKEND_DEFINE% /T ps_5_0 /E PSMain /Fo "%OUT%\UI.UIText.ps.cso" "%SRC%UI\UIText.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] UI/UIText PS & goto :error )

echo.
echo Done. UI shaders compiled successfully.
echo Log: %LOG%
endlocal
exit /b 0

:error
echo.
echo *** FAILED - see compile_ui_log.txt for details ***
echo Log: %LOG%
endlocal
exit /b 1
