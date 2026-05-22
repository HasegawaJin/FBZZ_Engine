@echo off
setlocal

REM Use fxc.exe (SM5.0 / DXBC) for DX11. dxc.exe outputs DXIL (DX12 only).
set FXC="C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe"
set SRC=%~dp0
set OUT=%~dp0compiled
REM %~dp0 ends with a backslash; strip it so /I "path\" does not mis-parse the closing quote
set SRCDIR=%SRC:~0,-1%
set INC=/nologo /I "%SRCDIR%"
set LOG=%~dp0compile_log.txt

if not exist "%OUT%" mkdir "%OUT%"

REM ログファイルを�E期化
echo [compile_shaders] %date% %time% > "%LOG%"

REM =========================================================================
REM Debug shader (DebugDraw 専用)
REM =========================================================================

echo [Debug] Debug.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\Debug.vs.cso" "%SRC%Debug.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Debug VS & goto :error )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\Debug.ps.cso" "%SRC%Debug.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Debug PS & goto :error )

REM =========================================================================
REM Material shaders
REM =========================================================================

echo [Material] Unlit.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\Material.Unlit.vs.cso" "%SRC%Material\Unlit.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/Unlit VS & goto :error )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\Material.Unlit.ps.cso" "%SRC%Material\Unlit.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/Unlit PS & goto :error )

echo [Material] Lit.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\Material.Lit.vs.cso" "%SRC%Material\Lit.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/Lit VS & goto :error )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\Material.Lit.ps.cso" "%SRC%Material\Lit.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/Lit PS & goto :error )

echo [Material] Phong.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\Material.Phong.vs.cso" "%SRC%Material\Phong.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/Phong VS & goto :error )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\Material.Phong.ps.cso" "%SRC%Material\Phong.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/Phong PS & goto :error )

echo [Material] BlinnPhong.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\Material.BlinnPhong.vs.cso" "%SRC%Material\BlinnPhong.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/BlinnPhong VS & goto :error )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\Material.BlinnPhong.ps.cso" "%SRC%Material\BlinnPhong.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/BlinnPhong PS & goto :error )

echo [Material] PBR.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\Material.PBR.vs.cso" "%SRC%Material\PBR.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/PBR VS & goto :error )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\Material.PBR.ps.cso" "%SRC%Material\PBR.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/PBR PS & goto :error )

echo [Material] Toon.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\Material.Toon.vs.cso" "%SRC%Material\Toon.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/Toon VS & goto :error )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\Material.Toon.ps.cso" "%SRC%Material\Toon.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/Toon PS & goto :error )

echo [Material] Particle.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\Material.Particle.vs.cso" "%SRC%Material\Particle.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/Particle VS & goto :error )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\Material.Particle.ps.cso" "%SRC%Material\Particle.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/Particle PS & goto :error )

echo [Material] Sky/Skybox.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\Material.Skybox.vs.cso" "%SRC%Material\Sky\Skybox.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/Skybox VS & goto :error )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\Material.Skybox.ps.cso" "%SRC%Material\Sky\Skybox.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/Skybox PS & goto :error )

echo [Material] Sky/Skydome.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\Material.Skydome.vs.cso" "%SRC%Material\Sky\Skydome.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/Skydome VS & goto :error )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\Material.Skydome.ps.cso" "%SRC%Material\Sky\Skydome.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Material/Skydome PS & goto :error )

REM =========================================================================
REM Pipeline shaders
REM =========================================================================

echo [Pipeline] GBuffer.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\Pipeline.GBuffer.vs.cso" "%SRC%Pipeline\GBuffer.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Pipeline/GBuffer VS & goto :error )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\Pipeline.GBuffer.ps.cso" "%SRC%Pipeline\GBuffer.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Pipeline/GBuffer PS & goto :error )

echo [Pipeline] DeferredLighting.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\Pipeline.DeferredLighting.vs.cso" "%SRC%Pipeline\DeferredLighting.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Pipeline/DeferredLighting VS & goto :error )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\Pipeline.DeferredLighting.ps.cso" "%SRC%Pipeline\DeferredLighting.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Pipeline/DeferredLighting PS & goto :error )

echo [Pipeline] ShadowMap.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\Pipeline.ShadowMap.vs.cso" "%SRC%Pipeline\ShadowMap.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Pipeline/ShadowMap VS & goto :error )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\Pipeline.ShadowMap.ps.cso" "%SRC%Pipeline\ShadowMap.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] Pipeline/ShadowMap PS & goto :error )

REM =========================================================================
REM PostProcess shaders (CS requires SM5.0)
REM =========================================================================

echo [PostProcess] SSAO.cs.hlsl...
%FXC% %INC% /T cs_5_0 /E CSMain /Fo "%OUT%\PostProcess.SSAO.cs.cso" "%SRC%PostProcess\SSAO.cs.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] PostProcess/SSAO CS & goto :error )

echo [PostProcess] SSAOBlur.cs.hlsl...
%FXC% %INC% /T cs_5_0 /E CSMain /Fo "%OUT%\PostProcess.SSAOBlur.cs.cso" "%SRC%PostProcess\SSAOBlur.cs.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] PostProcess/SSAOBlur CS & goto :error )

echo [PostProcess] BloomDownsample.cs.hlsl...
%FXC% %INC% /T cs_5_0 /E CSMain /Fo "%OUT%\PostProcess.BloomDownsample.cs.cso" "%SRC%PostProcess\BloomDownsample.cs.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] PostProcess/BloomDownsample CS & goto :error )

echo [PostProcess] BloomUpsample.cs.hlsl...
%FXC% %INC% /T cs_5_0 /E CSMain /Fo "%OUT%\PostProcess.BloomUpsample.cs.cso" "%SRC%PostProcess\BloomUpsample.cs.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] PostProcess/BloomUpsample CS & goto :error )

echo [PostProcess] Composite.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\PostProcess.Composite.vs.cso" "%SRC%PostProcess\Composite.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] PostProcess/Composite VS & goto :error )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\PostProcess.Composite.ps.cso" "%SRC%PostProcess\Composite.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] PostProcess/Composite PS & goto :error )

echo [PostProcess] FXAA.hlsl...
%FXC% %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\PostProcess.FXAA.vs.cso" "%SRC%PostProcess\FXAA.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] PostProcess/FXAA VS & goto :error )
%FXC% %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\PostProcess.FXAA.ps.cso" "%SRC%PostProcess\FXAA.hlsl" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] PostProcess/FXAA PS & goto :error )

echo.
echo Done. All shaders compiled successfully.
echo Log: %LOG%
endlocal
exit /b 0

:error
echo.
echo *** FAILED  E詳細は compile_log.txt を確誁E***
echo Log: %LOG%
endlocal
exit /b 1
