@echo off
setlocal

REM Use fxc.exe (SM5.0 / DXBC) for DX11. dxc.exe outputs DXIL (DX12 only).
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
set OUT=%~dp0compiled
REM %~dp0 ends with a backslash; strip it so /I "path" does not mis-parse the closing quote.
set SRCDIR=%SRC:~0,-1%
set INC=/nologo /I "%SRCDIR%"
set LOG=%~dp0compile_log.txt

if not exist "%OUT%" mkdir "%OUT%"

echo [compile_shaders] %date% %time% > "%LOG%"

REM =========================================================================
REM Debug
REM =========================================================================

call :CompileVSPS Debug\DebugDraw.hlsl Debug.DebugDraw || goto :error
call :CompileVSPS Debug\SelectionMask.hlsl Debug.SelectionMask || goto :error
call :CompileVSPS Debug\SelectionMaskSkinnedMesh.hlsl Debug.SelectionMaskSkinnedMesh || goto :error

REM =========================================================================
REM Material
REM =========================================================================

call :CompileVSPS Material\Surface\Unlit.hlsl Material.Surface.Unlit || goto :error
call :CompileVSPS Material\Surface\Lit.hlsl Material.Surface.Lit || goto :error
call :CompileVSPS Material\Surface\Phong.hlsl Material.Surface.Phong || goto :error
call :CompileVSPS Material\Surface\BlinnPhong.hlsl Material.Surface.BlinnPhong || goto :error
call :CompileVSPS Material\Surface\PBR.hlsl Material.Surface.PBR || goto :error
call :CompileVSPS Material\Surface\Toon.hlsl Material.Surface.Toon || goto :error
call :CompileVSPS Material\Skinned\SkinnedPBR.hlsl Material.Skinned.SkinnedPBR || goto :error
call :CompileVSPS Material\Effects\Particle.hlsl Material.Effects.Particle || goto :error
call :CompileVSPS Material\Sky\Skybox.hlsl Material.Sky.Skybox || goto :error
call :CompileVSPS Material\Sky\Skydome.hlsl Material.Sky.Skydome || goto :error

REM =========================================================================
REM Pipeline
REM =========================================================================

call :CompileVSPS Pipeline\Deferred\GBuffer.hlsl Pipeline.Deferred.GBuffer || goto :error
call :CompileVSPS Pipeline\Deferred\DeferredLighting.hlsl Pipeline.Deferred.DeferredLighting || goto :error
call :CompileVSPS Pipeline\Shadow\ShadowMap.hlsl Pipeline.Shadow.ShadowMap || goto :error
call :CompileVSPS Pipeline\Shadow\SkinnedShadowMap.hlsl Pipeline.Shadow.SkinnedShadowMap || goto :error

REM =========================================================================
REM PostProcess
REM =========================================================================

call :CompileCS PostProcess\AmbientOcclusion\SSAO.cs.hlsl PostProcess.AmbientOcclusion.SSAO.cs || goto :error
call :CompileCS PostProcess\AmbientOcclusion\SSAOBlur.cs.hlsl PostProcess.AmbientOcclusion.SSAOBlur.cs || goto :error
call :CompileCS PostProcess\Bloom\BloomDownsample.cs.hlsl PostProcess.Bloom.BloomDownsample.cs || goto :error
call :CompileCS PostProcess\Bloom\BloomUpsample.cs.hlsl PostProcess.Bloom.BloomUpsample.cs || goto :error
call :CompileVSPS PostProcess\Color\Composite.hlsl PostProcess.Color.Composite || goto :error
call :CompileVSPS PostProcess\AntiAliasing\FXAA.hlsl PostProcess.AntiAliasing.FXAA || goto :error
call :CompileVSPS PostProcess\Outline\SelectionOutline.hlsl PostProcess.Outline.SelectionOutline || goto :error

echo.
echo Done. All shaders compiled successfully.
echo Log: %LOG%
endlocal
exit /b 0

:CompileVSPS
echo [VS/PS] %~1
"%FXC%" %INC% /T vs_5_0 /E VSMain /Fo "%OUT%\%~2.vs.cso" "%SRC%%~1" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] %~1 VS & exit /b 1 )
"%FXC%" %INC% /T ps_5_0 /E PSMain /Fo "%OUT%\%~2.ps.cso" "%SRC%%~1" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] %~1 PS & exit /b 1 )
exit /b 0

:CompileCS
echo [CS] %~1
"%FXC%" %INC% /T cs_5_0 /E CSMain /Fo "%OUT%\%~2.cso" "%SRC%%~1" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] %~1 CS & exit /b 1 )
exit /b 0

:error
echo.
echo *** FAILED - see compile_log.txt for details ***
echo Log: %LOG%
endlocal
exit /b 1
