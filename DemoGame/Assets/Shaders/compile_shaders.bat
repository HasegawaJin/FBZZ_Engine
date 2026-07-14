@echo off
setlocal

REM No argument builds both DX11 and DX12 shader sets.
if "%~1"=="" (
    call "%~f0" DX11
    if errorlevel 1 exit /b 1
    call "%~f0" DX12
    if errorlevel 1 exit /b 1
    exit /b 0
)

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
if /I "%~1"=="DX12" (
    set OUT=%~dp0compiled_dx12
    set BACKEND_DEFINE=/D FBZZ_BACKEND_DX12=1
) else (
    set OUT=%~dp0compiled
    set BACKEND_DEFINE=/D FBZZ_BACKEND_DX11=1
)
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
call :CompileVSPS Terrain\TerrainGBuffer.hlsl Terrain.TerrainGBuffer || goto :error

REM =========================================================================
REM Water
REM =========================================================================

call :CompileVSPS Water\Water.hlsl Water.Water || goto :error

REM Terrain Detail

call :CompileVSPS Detail\Detail.hlsl Detail.Detail || goto :error
call :CompileVSPS Detail\DetailGrass.hlsl Detail.DetailGrass || goto :error
call :CompileVSPS Detail\DetailGBuffer.hlsl Detail.DetailGBuffer || goto :error
call :CompileVSPS Detail\DetailGrassGBuffer.hlsl Detail.DetailGrassGBuffer || goto :error

REM Foliage

call :CompileVSPS Foliage\Foliage.hlsl Foliage.Foliage || goto :error
call :CompileVSPS Foliage\FoliageGBuffer.hlsl Foliage.FoliageGBuffer || goto :error

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
call :CompileVSPS Material\Surface\Fallback.hlsl Material.Surface.Fallback || goto :error
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
call :CompileVSPS Material\Skinned\FallbackSkinned.hlsl Material.Skinned.FallbackSkinned || goto :error
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
call :CompileVSPS Material\Effects\ParticleGPU.hlsl Material.Effects.ParticleGPU || goto :error
call :CompileCS Material\Effects\ParticleGpuSim.cs.hlsl Material.Effects.ParticleGpuSim.cs || goto :error
call :CompileVSPS Material\Effects\Trail.hlsl Material.Effects.Trail || goto :error
call :CompileVSPS Material\Effects\MeshTrail.hlsl Material.Effects.MeshTrail || goto :error
call :CompileVSPS Material\Effects\SkinnedMeshTrail.hlsl Material.Effects.SkinnedMeshTrail || goto :error
call :CompileVSPS Material\Sky\Skybox.hlsl Material.Sky.Skybox || goto :error
call :CompileVSPS Material\Sky\Skydome.hlsl Material.Sky.Skydome || goto :error
call :CompileVSPS Material\Sky\SunMoon.hlsl Material.Sky.SunMoon || goto :error
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
REM GTAO (Ground Truth Ambient Occlusion) — Horizon-Based AO
call :CompileCS PostProcess\AmbientOcclusion\GTAO.cs.hlsl PostProcess.AmbientOcclusion.GTAO.cs || goto :error
call :CompileCS PostProcess\AmbientOcclusion\GTAOBlur.cs.hlsl PostProcess.AmbientOcclusion.GTAOBlur.cs || goto :error
REM IBL BRDF LUT — 起動時 1 回だけ実行するプリインテグレーション CS
call :CompileCS PostProcess\AmbientOcclusion\BRDFIntegration.cs.hlsl PostProcess.AmbientOcclusion.BRDFIntegration.cs || goto :error
call :CompileCS PostProcess\Bloom\BloomDownsample.cs.hlsl PostProcess.Bloom.BloomDownsample.cs || goto :error
call :CompileCS PostProcess\Bloom\BloomUpsample.cs.hlsl PostProcess.Bloom.BloomUpsample.cs || goto :error
call :CompileVSPS PostProcess\Color\CopyColor.hlsl PostProcess.Color.CopyColor || goto :error
call :CompileVSPS PostProcess\Color\Composite.hlsl PostProcess.Color.Composite || goto :error
call :CompileVSPS PostProcess\Water\Caustics.hlsl PostProcess.Water.Caustics || goto :error
REM Volumetric Cloud — レイマーチ雲の VS/PS パス (ハーフ解像度 RT へ) + アップスケール合成
call :CompileVSPS PostProcess\Cloud\VolumetricCloud.hlsl PostProcess.Cloud.VolumetricCloud || goto :error
call :CompileVSPS PostProcess\Cloud\CloudUpscale.hlsl PostProcess.Cloud.CloudUpscale || goto :error
for %%S in ("%SRC%PostProcess\Custom\*.hlsl") do call :CompileVSPS PostProcess\Custom\%%~nxS PostProcess.Custom.%%~nS || goto :error
REM TAA (Temporal Anti-Aliasing) — ping-pong 履歴バッファへの VS/PS パス
call :CompileVSPS PostProcess\AntiAliasing\TAA.hlsl PostProcess.AntiAliasing.TAA || goto :error
call :CompileVSPS PostProcess\AntiAliasing\FXAA.hlsl PostProcess.AntiAliasing.FXAA || goto :error
call :CompileVSPS PostProcess\Outline\SelectionOutline.hlsl PostProcess.Outline.SelectionOutline || goto :error
REM Screen Space Reflections — Compute Shader
call :CompileCS PostProcess\Reflections\SSR.cs.hlsl PostProcess.Reflections.SSR.cs || goto :error
REM Volumetric Lighting — Henyey-Greenstein 散乱 Compute Shader
call :CompileCS PostProcess\Lighting\VolumetricLight.cs.hlsl PostProcess.Lighting.VolumetricLight.cs || goto :error
REM Contact Shadows — View Space レイマーチ Compute Shader
call :CompileCS PostProcess\Shadow\ContactShadows.cs.hlsl PostProcess.Shadow.ContactShadows.cs || goto :error
REM Motion Blur — 深度再投影カメラブラー Compute Shader
call :CompileCS PostProcess\Motion\MotionBlur.cs.hlsl PostProcess.Motion.MotionBlur.cs || goto :error
REM Lens Flare — スクリーンスペースゴースト + ハロー ADDITIVE VS/PS
call :CompileVSPS PostProcess\Flare\LensFlare.hlsl PostProcess.Flare.LensFlare || goto :error

REM IBL Baking — Editor 側でオフラインベイクに使う Compute Shader 群
REM   EquirectToCubemap     : Equirectangular HDR -> 6 面 Cubemap
REM   IrradianceConvolution : Env Cubemap -> Diffuse Irradiance Cubemap
REM   PrefilteredEnvMap     : Env Cubemap -> Specular Prefiltered Cubemap (roughness per mip)
call :CompileCS IBL\EquirectToCubemap.cs.hlsl IBL.EquirectToCubemap.cs || goto :error
call :CompileCS IBL\IrradianceConvolution.cs.hlsl IBL.IrradianceConvolution.cs || goto :error
call :CompileCS IBL\PrefilteredEnvMap.cs.hlsl IBL.PrefilteredEnvMap.cs || goto :error

echo.
echo Done. All shaders compiled successfully.
echo Log: %LOG%
endlocal
exit /b 0

:CompileVSPS
echo [VS/PS] %~1
"%FXC%" %INC% %BACKEND_DEFINE% /T vs_5_0 /E VSMain /Fo "%OUT%\%~2.vs.cso" "%SRC%%~1" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] %~1 VS & exit /b 1 )
"%FXC%" %INC% %BACKEND_DEFINE% /T ps_5_0 /E PSMain /Fo "%OUT%\%~2.ps.cso" "%SRC%%~1" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] %~1 PS & exit /b 1 )
exit /b 0

:CompileCS
echo [CS] %~1
"%FXC%" %INC% %BACKEND_DEFINE% /T cs_5_0 /E CSMain /Fo "%OUT%\%~2.cso" "%SRC%%~1" >> "%LOG%" 2>&1
if %ERRORLEVEL% neq 0 ( echo [FAILED] %~1 CS & exit /b 1 )
exit /b 0

:error
echo.
echo *** FAILED - see compile_log.txt for details ***
echo Log: %LOG%
endlocal
exit /b 1
