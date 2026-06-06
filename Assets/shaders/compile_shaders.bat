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
REM Terrain
REM =========================================================================

call :CompileVSPS Terrain\Terrain.hlsl Terrain.Terrain || goto :error

REM =========================================================================
REM Water
REM =========================================================================

call :CompileVSPS Water\Water.hlsl Water.Water || goto :error

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
call :CompileVSPS Material\Surface\Dissolve.hlsl Material.Surface.Dissolve || goto :error
call :CompileVSPS Material\Surface\Subsurface.hlsl Material.Surface.Subsurface || goto :error
call :CompileVSPS Material\Surface\Anisotropic.hlsl Material.Surface.Anisotropic || goto :error
call :CompileVSPS Material\Surface\RimLight.hlsl Material.Surface.RimLight || goto :error
call :CompileVSPS Material\Skinned\SkinnedUnlit.hlsl Material.Skinned.SkinnedUnlit || goto :error
call :CompileVSPS Material\Skinned\SkinnedLit.hlsl Material.Skinned.SkinnedLit || goto :error
call :CompileVSPS Material\Skinned\SkinnedPhong.hlsl Material.Skinned.SkinnedPhong || goto :error
call :CompileVSPS Material\Skinned\SkinnedBlinnPhong.hlsl Material.Skinned.SkinnedBlinnPhong || goto :error
call :CompileVSPS Material\Skinned\SkinnedPBR.hlsl Material.Skinned.SkinnedPBR || goto :error
call :CompileVSPS Material\Skinned\SkinnedToon.hlsl Material.Skinned.SkinnedToon || goto :error
call :CompileVSPS Material\Skinned\SkinnedDissolve.hlsl Material.Skinned.SkinnedDissolve || goto :error
call :CompileVSPS Material\Skinned\SkinnedSubsurface.hlsl Material.Skinned.SkinnedSubsurface || goto :error
call :CompileVSPS Material\Skinned\SkinnedAnisotropic.hlsl Material.Skinned.SkinnedAnisotropic || goto :error
call :CompileVSPS Material\Skinned\SkinnedRimLight.hlsl Material.Skinned.SkinnedRimLight || goto :error
call :CompileVSPS Material\Effects\Particle.hlsl Material.Effects.Particle || goto :error
call :CompileVSPS Material\Effects\Trail.hlsl Material.Effects.Trail || goto :error
call :CompileVSPS Material\Effects\MeshTrail.hlsl Material.Effects.MeshTrail || goto :error
call :CompileVSPS Material\Effects\SkinnedMeshTrail.hlsl Material.Effects.SkinnedMeshTrail || goto :error
call :CompileVSPS Material\Sky\Skybox.hlsl Material.Sky.Skybox || goto :error
call :CompileVSPS Material\Sky\Skydome.hlsl Material.Sky.Skydome || goto :error
call :CompileVSPS Material\Decal\Decal.hlsl     Material.Decal.Decal     || goto :error
call :CompileVSPS Material\Decal\DecalMask.hlsl Material.Decal.DecalMask || goto :error

REM ユーザー作成のカスタムマテリアルシェーダー (ScriptCodeGen で生成)
if exist "%SRC%Material\Custom\" (
    for %%S in ("%SRC%Material\Custom\*.hlsl") do call :CompileVSPS Material\Custom\%%~nxS Material.Custom.%%~nS || goto :error
)

REM =========================================================================
REM Pipeline
REM =========================================================================

call :CompileVSPS Pipeline\Deferred\GBuffer.hlsl Pipeline.Deferred.GBuffer || goto :error
call :CompileVSPS Pipeline\Deferred\DeferredLighting.hlsl Pipeline.Deferred.DeferredLighting || goto :error
call :CompileVSPS Pipeline\Deferred\DepthCopy.hlsl Pipeline.Deferred.DepthCopy || goto :error
call :CompileVSPS Pipeline\Shadow\ShadowMap.hlsl Pipeline.Shadow.ShadowMap || goto :error
call :CompileVSPS Pipeline\Shadow\SkinnedShadowMap.hlsl Pipeline.Shadow.SkinnedShadowMap || goto :error

REM =========================================================================
REM PostProcess
REM =========================================================================

call :CompileCS PostProcess\AmbientOcclusion\SSAO.cs.hlsl PostProcess.AmbientOcclusion.SSAO.cs || goto :error
call :CompileCS PostProcess\AmbientOcclusion\SSAOBlur.cs.hlsl PostProcess.AmbientOcclusion.SSAOBlur.cs || goto :error
call :CompileCS PostProcess\Bloom\BloomDownsample.cs.hlsl PostProcess.Bloom.BloomDownsample.cs || goto :error
call :CompileCS PostProcess\Bloom\BloomUpsample.cs.hlsl PostProcess.Bloom.BloomUpsample.cs || goto :error
call :CompileVSPS PostProcess\Color\CopyColor.hlsl PostProcess.Color.CopyColor || goto :error
call :CompileVSPS PostProcess\Color\Composite.hlsl PostProcess.Color.Composite || goto :error
call :CompileVSPS PostProcess\Water\Caustics.hlsl PostProcess.Water.Caustics || goto :error
for %%S in ("%SRC%PostProcess\Custom\*.hlsl") do call :CompileVSPS PostProcess\Custom\%%~nxS PostProcess.Custom.%%~nS || goto :error
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
