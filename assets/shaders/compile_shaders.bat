@echo off
setlocal

REM Use fxc.exe (SM5.0 / DXBC) for DX11. dxc.exe outputs DXIL (DX12 only).
set FXC="C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe"
set SRC=%~dp0
set OUT=%~dp0compiled
REM %~dp0 ends with a backslash; strip it so /I "path\" does not mis-parse the closing quote
set SRCDIR=%SRC:~0,-1%
REM /nologo suppresses the fxc banner; 2>&1 on each call merges stderr so CMake sees all errors
set INC=/nologo /I "%SRCDIR%"

if not exist "%OUT%" mkdir "%OUT%"

REM =========================================================================
REM Legacy shaders (root)
REM =========================================================================

echo [Legacy] Unlit.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\Unlit.vs.cso" "%SRC%Unlit.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Unlit VS & exit /b 1 )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\Unlit.ps.cso" "%SRC%Unlit.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Unlit PS & exit /b 1 )

echo [Legacy] Debug.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\Debug.vs.cso" "%SRC%Debug.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Debug VS & exit /b 1 )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\Debug.ps.cso" "%SRC%Debug.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Debug PS & exit /b 1 )

echo [Legacy] Mesh.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\Mesh.vs.cso" "%SRC%Mesh.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Mesh VS & exit /b 1 )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\Mesh.ps.cso" "%SRC%Mesh.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Mesh PS & exit /b 1 )

REM =========================================================================
REM Material shaders
REM =========================================================================

echo [Material] Unlit.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\Material.Unlit.vs.cso" "%SRC%Material\Unlit.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/Unlit VS & exit /b 1 )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\Material.Unlit.ps.cso" "%SRC%Material\Unlit.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/Unlit PS & exit /b 1 )

echo [Material] Lit.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\Material.Lit.vs.cso" "%SRC%Material\Lit.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/Lit VS & exit /b 1 )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\Material.Lit.ps.cso" "%SRC%Material\Lit.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/Lit PS & exit /b 1 )

echo [Material] Phong.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\Material.Phong.vs.cso" "%SRC%Material\Phong.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/Phong VS & exit /b 1 )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\Material.Phong.ps.cso" "%SRC%Material\Phong.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/Phong PS & exit /b 1 )

echo [Material] BlinnPhong.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\Material.BlinnPhong.vs.cso" "%SRC%Material\BlinnPhong.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/BlinnPhong VS & exit /b 1 )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\Material.BlinnPhong.ps.cso" "%SRC%Material\BlinnPhong.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/BlinnPhong PS & exit /b 1 )

echo [Material] PBR.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\Material.PBR.vs.cso" "%SRC%Material\PBR.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/PBR VS & exit /b 1 )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\Material.PBR.ps.cso" "%SRC%Material\PBR.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/PBR PS & exit /b 1 )

echo [Material] Sky/Skybox.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\Material.Skybox.vs.cso" "%SRC%Material\Sky\Skybox.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/Skybox VS & exit /b 1 )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\Material.Skybox.ps.cso" "%SRC%Material\Sky\Skybox.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/Skybox PS & exit /b 1 )

echo [Material] Sky/Skydome.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\Material.Skydome.vs.cso" "%SRC%Material\Sky\Skydome.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/Skydome VS & exit /b 1 )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\Material.Skydome.ps.cso" "%SRC%Material\Sky\Skydome.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/Skydome PS & exit /b 1 )

REM =========================================================================
REM Pipeline shaders
REM =========================================================================

echo [Pipeline] GBuffer.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\Pipeline.GBuffer.vs.cso" "%SRC%Pipeline\GBuffer.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Pipeline/GBuffer VS & exit /b 1 )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\Pipeline.GBuffer.ps.cso" "%SRC%Pipeline\GBuffer.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Pipeline/GBuffer PS & exit /b 1 )

echo [Pipeline] DeferredLighting.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\Pipeline.DeferredLighting.vs.cso" "%SRC%Pipeline\DeferredLighting.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Pipeline/DeferredLighting VS & exit /b 1 )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\Pipeline.DeferredLighting.ps.cso" "%SRC%Pipeline\DeferredLighting.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Pipeline/DeferredLighting PS & exit /b 1 )

echo [Pipeline] ShadowMap.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\Pipeline.ShadowMap.vs.cso" "%SRC%Pipeline\ShadowMap.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Pipeline/ShadowMap VS & exit /b 1 )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\Pipeline.ShadowMap.ps.cso" "%SRC%Pipeline\ShadowMap.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Pipeline/ShadowMap PS & exit /b 1 )

REM =========================================================================
REM PostProcess shaders (CS requires SM5.0)
REM =========================================================================

echo [PostProcess] SSAO.cs.hlsl...
%FXC% %INC% /T cs_5_0 /E CSMain /Fo "%OUT%\PostProcess.SSAO.cs.cso" "%SRC%PostProcess\SSAO.cs.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] PostProcess/SSAO CS & exit /b 1 )

echo [PostProcess] SSAOBlur.cs.hlsl...
%FXC% %INC% /T cs_5_0 /E CSMain /Fo "%OUT%\PostProcess.SSAOBlur.cs.cso" "%SRC%PostProcess\SSAOBlur.cs.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] PostProcess/SSAOBlur CS & exit /b 1 )

echo [PostProcess] BloomDownsample.cs.hlsl...
%FXC% %INC% /T cs_5_0 /E CSMain /Fo "%OUT%\PostProcess.BloomDownsample.cs.cso" "%SRC%PostProcess\BloomDownsample.cs.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] PostProcess/BloomDownsample CS & exit /b 1 )

echo [PostProcess] BloomUpsample.cs.hlsl...
%FXC% %INC% /T cs_5_0 /E CSMain /Fo "%OUT%\PostProcess.BloomUpsample.cs.cso" "%SRC%PostProcess\BloomUpsample.cs.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] PostProcess/BloomUpsample CS & exit /b 1 )

echo [PostProcess] Composite.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\PostProcess.Composite.vs.cso" "%SRC%PostProcess\Composite.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] PostProcess/Composite VS & exit /b 1 )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\PostProcess.Composite.ps.cso" "%SRC%PostProcess\Composite.hlsl" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] PostProcess/Composite PS & exit /b 1 )

echo.
echo Done. All shaders compiled successfully.
endlocal
