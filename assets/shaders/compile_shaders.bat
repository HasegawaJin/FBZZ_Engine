@echo off
setlocal

REM Use fxc.exe (SM5.0 / DXBC) for DX11. dxc.exe outputs DXIL (DX12 only).
set FXC="C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe"
set SRC=%~dp0
set OUT=%~dp0compiled

if not exist "%OUT%" mkdir "%OUT%"

echo Compiling Unlit.hlsl...
%FXC% /T vs_5_0 /E VSMain /Fo "%OUT%\Unlit.vs.cso" "%SRC%Unlit.hlsl"
if %ERRORLEVEL% neq 0 ( echo [FAILED] Unlit VS & exit /b 1 )
%FXC% /T ps_5_0 /E PSMain /Fo "%OUT%\Unlit.ps.cso" "%SRC%Unlit.hlsl"
if %ERRORLEVEL% neq 0 ( echo [FAILED] Unlit PS & exit /b 1 )

echo Compiling Debug.hlsl...
%FXC% /T vs_5_0 /E VSMain /Fo "%OUT%\Debug.vs.cso" "%SRC%Debug.hlsl"
if %ERRORLEVEL% neq 0 ( echo [FAILED] Debug VS & exit /b 1 )
%FXC% /T ps_5_0 /E PSMain /Fo "%OUT%\Debug.ps.cso" "%SRC%Debug.hlsl"
if %ERRORLEVEL% neq 0 ( echo [FAILED] Debug PS & exit /b 1 )

echo Compiling Mesh.hlsl...
%FXC% /T vs_5_0 /E VSMain /Fo "%OUT%\Mesh.vs.cso" "%SRC%Mesh.hlsl"
if %ERRORLEVEL% neq 0 ( echo [FAILED] Mesh VS & exit /b 1 )
%FXC% /T ps_5_0 /E PSMain /Fo "%OUT%\Mesh.ps.cso" "%SRC%Mesh.hlsl"
if %ERRORLEVEL% neq 0 ( echo [FAILED] Mesh PS & exit /b 1 )

echo Done.
endlocal
