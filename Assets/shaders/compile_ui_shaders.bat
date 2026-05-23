@echo off
setlocal

set FXC="C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe"
set SRC=%~dp0
set OUT=%~dp0compiled
set LOG=%~dp0compile_ui_log.txt
set SRCDIR=%SRC:~0,-1%
set INC=/nologo /I "%SRCDIR%"

if not exist "%OUT%" mkdir "%OUT%"

echo [compile_ui_shaders] %date% %time% > "%LOG%"

REM =========================================================================
REM UI shaders
REM =========================================================================

echo [UI] UISprite.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\UI.UISprite.vs.cso" "%SRC%UI\UISprite.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] UI/UISprite VS & goto :error )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\UI.UISprite.ps.cso" "%SRC%UI\UISprite.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] UI/UISprite PS & goto :error )

echo [UI] UIText.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\UI.UIText.vs.cso" "%SRC%UI\UIText.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] UI/UIText VS & goto :error )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\UI.UIText.ps.cso" "%SRC%UI\UIText.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] UI/UIText PS & goto :error )

echo.
echo Done. UI shaders compiled successfully.
echo Log: %LOG%
pause
endlocal
exit /b 0

:error
echo.
echo *** FAILED - see compile_ui_log.txt for details ***
echo Log: %LOG%
pause
endlocal
exit /b 1
