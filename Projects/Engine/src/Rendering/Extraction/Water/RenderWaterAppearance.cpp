/// @file    RenderWaterAppearance.cpp
/// @brief   水面と水中の材質定数、シェーダー、テクスチャスロットの解決。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "RenderWaterAppearance.hpp"
#include "RenderWaterResources.hpp"
#include <Core/Profiler/ProfileScope.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Material/MaterialParamBinding.hpp>
#include <Engine/Asset/Streaming/StreamedTextureResolver.hpp>
#include <Engine/Asset/Fluid/Rendering/VelocityFieldAtlas.hpp>
#include <Engine/Asset/Water/WaterOpticalSettings.hpp>
#include <Engine/Core/Timing/Time.hpp>
#include <Engine/Scene/Components/Water/WaterComponent.hpp>
#include <Engine/Scene/Components/Water/WaterZoneComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Hierarchy/Transform.hpp>
#include <Engine/Scene/Water/Surface/WaterDepthField.hpp>
#include <Engine/Scene/Water/Surface/WaterSpline.hpp>
#include <Engine/Scene/Water/Surface/WaterSurfaceQuery.hpp>
#include <Engine/Scene/Water/Surface/WaterZoneSnapshot.hpp>
#include <Fluid/Fields/VectorFieldAsset.hpp>
#include <Graphics/Passes/Geometry/Surfaces/WaterRenderPass.hpp>
#include <Graphics/Renderer/Shaders/IShader.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>
#include <Math/Scalar/MathUtils.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <unordered_map>

namespace fbzz::scene::water_render_detail {

namespace {

math::Vector2 ReadWaterMaterialVector2(const asset::MaterialAssetAuthoring* material, const char* name, math::Vector2 fallback)
{
    if (!material) return fallback;
    auto parameter = material->params.find(name);
    if (parameter != material->params.end() && parameter->second.size() >= 2
        && std::isfinite(parameter->second[0]) && std::isfinite(parameter->second[1]))
        return { parameter->second[0], parameter->second[1] };
    return fallback;
}
math::Vector3 ReadWaterMaterialVector3(const asset::MaterialAssetAuthoring* material, const char* name, math::Vector3 fallback)
{
    if (!material) return fallback;
    auto parameter = material->params.find(name);
    if (parameter != material->params.end() && parameter->second.size() >= 3
        && std::isfinite(parameter->second[0]) && std::isfinite(parameter->second[1]) && std::isfinite(parameter->second[2]))
        return { parameter->second[0], parameter->second[1], parameter->second[2] };
    return fallback;
}

/// @brief 接続先の描画に使う解決済みの値から、その水面の受け側プロファイルを作る。
renderer::WaterMaterialBlendTarget MakeWaterMaterialBlendTarget(const renderer::WaterCB& target,
    const renderer::WaterEffectParams& effects)
{
    return {
        target.shallowColorDepth,
        target.deepColorDepth,
        target.surfaceParams,
        {target.normalParams.w, target.reflectParams.x, target.refractionFlowParams.x, 0.0f},
        target.detailParams,
        target.foamParams,
        target.crestFoamParams,
        target.foamColorRoughness,
        {target.absorptionCoefficients.x, target.absorptionCoefficients.y, target.absorptionCoefficients.z, target.opticalParams.y},
        {target.scatteringCoefficients.x, target.scatteringCoefficients.y, target.scatteringCoefficients.z, target.opticalParams.x},
        target.sssParams,
        {effects.rimGlowStrength, effects.minShallowAlpha, effects.specularStrength, effects.rippleRingStrength},
        {effects.skyReflectTint[0], effects.skyReflectTint[1], effects.skyReflectTint[2], 0.0f},
        {effects.rippleRingColor[0], effects.rippleRingColor[1], effects.rippleRingColor[2], 0.0f},
    };
}

void WriteSurfaceFlowConstants(const WaterComponent& water, renderer::WaterCB& constants,
    renderer::ResourceManager& resources)
{
    /// @note 流れの場が出す形・流れ・質感。本数は y に入れる (枠を 1 つ増やさずに済む)。
    /// @note 高さも流速も CPU で決める。形の式は WaterComponent が正本で、浮力・水中判定と
    /// @note 同じ値でなければならない。シェーダーで導き直すと二重化が 1 本増える。
    const int flowCount = (std::min)(water.surfaceFlowCount, kWaterSurfaceFlowCount);
    constants.waveShapeParams = { math::Clamp01(water.waveSpread),
                           static_cast<float>(flowCount), 0.0f, 0.0f };
    for (int flowIndex = 0; flowIndex < kWaterSurfaceFlowCount; ++flowIndex) {
        if (flowIndex >= flowCount) continue;
        const WaterSurfaceFlow& surfaceFlow = water.surfaceFlows[static_cast<size_t>(flowIndex)];
        constants.surfaceFlowA[flowIndex] = { surfaceFlow.center.x, surfaceFlow.center.y, surfaceFlow.radius, surfaceFlow.height };
        constants.surfaceFlowB[flowIndex] = { surfaceFlow.speed, surfaceFlow.falloffPower,
                               static_cast<float>(static_cast<int>(surfaceFlow.kind)), surfaceFlow.chop };
        /// @note 常駐できなかった場はタイル -1 で渡す。本数を詰めると別の流れに化けるので、
        /// @note 枠は残したまま «無効» を送る (ParticleGpuSim と同じ扱い)。
        float atlasTile = -1.0f;
        float maxMagnitude = 1.0f;
        if (surfaceFlow.kind == FlowFieldType::Baked && surfaceFlow.vectorField != nullptr) {
            atlasTile = static_cast<float>(asset::VelocityFieldAtlas::Acquire(*surfaceFlow.vectorField, resources));
            maxMagnitude = surfaceFlow.vectorField->maxMagnitude;
        }
        constants.surfaceFlowC[flowIndex] = { surfaceFlow.direction.x, surfaceFlow.direction.y, atlasTile, maxMagnitude };
        constants.surfaceFlowD[flowIndex] = { surfaceFlow.inverseRotation.x, surfaceFlow.inverseRotation.y,
                               surfaceFlow.inverseRotation.z, surfaceFlow.inverseRotation.w };
        constants.surfaceFlowE[flowIndex] = surfaceFlow.kind != FlowFieldType::Baked
            ? math::Vector4{ surfaceFlow.swellWavelength, surfaceFlow.swellNearFade, 0.0f, 0.0f }
            : math::Vector4{ surfaceFlow.extents.x, surfaceFlow.extents.y, surfaceFlow.extents.z, surfaceFlow.planeOffsetY };
    }
}

void WriteGerstnerWaveConstants(const WaterComponent& water, renderer::WaterCB& constants)
{
    const size_t waveCount = (std::min)(static_cast<size_t>(water.waveCount), water.waves.size());
    constants.waveControls = {static_cast<float>(waveCount), water.rippleDirectionOverride.x,
        water.rippleDirectionOverride.y, 1.0f};
    const WaterDepthField* depthField = water.EffectiveDepthField(WaterSurfaceReadView::SIMULATION);
    const float attenuationDepth = water.EffectiveWaveAttenuationDepth(WaterSurfaceReadView::SIMULATION);
    const float signedCurrentSpeed = water.current.x * constants.flowParams.x + water.current.y * constants.flowParams.y;
    constants.waveDepthParams = {attenuationDepth,
        depthField && attenuationDepth > 0.0f ? 1.0f : 0.0f,
        depthField ? depthField->outsideFadeWidth : 1.0f, signedCurrentSpeed};
    const float horizontalScale = water.GetHorizontalWaveScale(WaterSurfaceReadView::SIMULATION);
    const float waveSpeed = water.WaveSpeedMultiplier();
    std::array<float, 16> phases{};
    for (size_t waveIndex = 0; waveIndex < waveCount; ++waveIndex) {
        const GerstnerWave& wave = water.waves[static_cast<size_t>(waveIndex)];
        assert(wave.steepness <= 1.0f);

        const float directionLengthSq = wave.direction.LengthSq();
        const bool validWave = water.enableGerstnerWaves && wave.amplitude > 0.0001f
            && wave.wavelength > 0.0001f && std::isfinite(directionLengthSq) && directionLengthSq > 1.0e-8f;
        const math::Vector2 waveDirection = validWave ? wave.direction.Normalized() : math::Vector2{1.0f, 0.0f};
        const float waveNumber = validWave ? math::TWO_PI / wave.wavelength : 0.0f;
        const float angularFrequency = validWave ? std::sqrt(9.8f * waveNumber) * waveSpeed : 0.0f;
        const float steepness = math::Clamp01(wave.steepness) * horizontalScale;
        constants.waveDir[waveIndex] = { waveDirection.x, waveDirection.y, steepness, validWave ? 1.0f : 0.0f };
        constants.waveParams[waveIndex] = { validWave ? wave.amplitude : 0.0f, wave.wavelength, angularFrequency, waveNumber };
        phases[waveIndex] = validWave ? wave.phaseOffsetRadians : 0.0f;
    }
    for (size_t phaseGroupIndex = 0; phaseGroupIndex < 4; ++phaseGroupIndex) {
        const size_t firstPhase = phaseGroupIndex * 4;
        constants.wavePhasePacked[phaseGroupIndex] = {
            phases[firstPhase], phases[firstPhase + 1], phases[firstPhase + 2], phases[firstPhase + 3]};
    }
}

}

asset::MaterialAsset::AuthoringSnapshotPtr LoadWaterMaterial(const std::string& materialPath)
{
    if (materialPath.empty()) return {};
    return asset::ReadMaterialAuthoring(asset::AssetManager::Load<asset::MaterialAsset>(materialPath));
}

float ReadWaterMaterialFloat(const asset::MaterialAssetAuthoring* material, const char* name, float fallback)
{
    if (!material) return fallback;
    auto parameter = material->params.find(name);
    if (parameter != material->params.end() && !parameter->second.empty() && std::isfinite(parameter->second[0])) return parameter->second[0];
    return fallback;
}

/// @note viewProjection は TAA ジッター込みで渡す。カメラから組み直すとジッターが落ちる。
/// @param resources 焼いた速度場を常駐させる (VelocityFieldAtlas) のに要る。
renderer::WaterCB BuildWaterConstants(const WaterComponent& water, const asset::MaterialAssetAuthoring* material,
                     const Transform& transform, const math::Matrix4& viewProjection, float time,
                     float skyReflection, const renderer::WaterDetailNoise& detailNoise,
                     renderer::ResourceManager& resources)
{
    renderer::WaterCB constants{};
    const math::Matrix4 surfaceToWorld = transform.GetWorldMatrix();
    constants.worldMatrix = surfaceToWorld;
    constants.wvpMatrix = viewProjection * surfaceToWorld;

    const auto shallowColor = ReadWaterMaterialVector3(material, "shallowColor", { 0.20f, 0.60f, 0.70f });
    const auto deepColor    = ReadWaterMaterialVector3(material, "deepColor",    { 0.00f, 0.10f, 0.30f });
    constants.shallowColorDepth = { shallowColor.x, shallowColor.y, shallowColor.z,
                              ReadWaterMaterialFloat(material, "shallowDepth", 0.5f) };
    constants.deepColorDepth    = { deepColor.x, deepColor.y, deepColor.z,
                              ReadWaterMaterialFloat(material, "deepDepth", 5.0f) };
    constants.surfaceParams = {
        ReadWaterMaterialFloat(material, "opacity",       0.85f),
        ReadWaterMaterialFloat(material, "reflectivity",  0.5f),
        ReadWaterMaterialFloat(material, "fresnelBias",   0.02f),
        ReadWaterMaterialFloat(material, "fresnelPower",  5.0f)
    };
    const auto optics = (material ? asset::ResolveWaterOpticalSettings(*material) : asset::WaterOpticalSettings{});
    constants.absorptionCoefficients = {optics.absorption.x, optics.absorption.y, optics.absorption.z, 0.0f};
    constants.scatteringCoefficients = {optics.scattering.x, optics.scattering.y, optics.scattering.z, 0.0f};
    constants.opticalParams = {optics.enabled ? 1.0f : 0.0f, optics.phaseG, 0.0f, 0.0f};
    /// @note 水面の «実寸» はローカル extent ではなくワールド実寸。拡大された水面でも、縁のフェードと
    /// @note 波のエイリアシング判定が、画面に出ているとおりの大きさで効くようにする。
    const auto localExtent = WaterSurfaceLocalExtent(water, WaterSurfaceReadView::SIMULATION);
    const float worldExtentX = localExtent.x * std::abs(transform.worldScale.x);
    const float worldExtentZ = localExtent.y * std::abs(transform.worldScale.z);
    /// @note z はさざ波タイルの勾配復号係数。ベイクした値をそのまま渡し、シェーダー側に定数を
    /// @note 写さない (写すとタイルを焼き直してもシェーダーが古い係数で復号し続ける)。
    constants.normalParams = { worldExtentX, worldExtentZ, detailNoise.derivativeScale,
                        ReadWaterMaterialFloat(material, "normalStrength", 1.0f) };
    /// @note xy は描画の基準セル幅。物理の波高はセル幅に依存しない。
    /// @note z はタイルのセル数の逆数。
    constants.timeParams   = { water.cellSize.x, water.cellSize.y, detailNoise.invTileCells, time };
    constants.gridParams = { water.cameraFocusedGrid ? 1.0f : 0.0f,
        (std::clamp)(water.nearCellSize, 0.25f, 10.0f),
        static_cast<float>(water.resolutionX), static_cast<float>(water.resolutionZ) };
    constants.foamParams = {
        ReadWaterMaterialFloat(material, "foamThreshold",     0.3f),
        ReadWaterMaterialFloat(material, "foamFade",          0.5f),
        ReadWaterMaterialFloat(material, "foamStrength",      1.0f),
        /// @note 泡のムラはワールド座標で評価するため、旧 foamTiling(UV 倍率) とは単位が違う。
        ReadWaterMaterialFloat(material, "foamNoiseScale",    0.5f)
    };
    constants.crestFoamParams = {
        math::Clamp01(ReadWaterMaterialFloat(material, "crestFoamThreshold", 0.25f)),
        (std::clamp)(ReadWaterMaterialFloat(material, "crestFoamFade", 0.15f), 0.001f, 1.0f),
        (std::clamp)(ReadWaterMaterialFloat(material, "crestFoamStrength", 0.9f), 0.0f, 4.0f),
        (std::clamp)(ReadWaterMaterialFloat(material, "crestFoamLifetime", 1.2f), 0.0f, 10.0f)
    };
    const auto foamColor = ReadWaterMaterialVector3(material, "foamColor", { 0.95f, 0.98f, 1.0f });
    constants.foamColorRoughness = { math::Clamp01(foamColor.x), math::Clamp01(foamColor.y),
        math::Clamp01(foamColor.z), (std::clamp)(ReadWaterMaterialFloat(material, "foamRoughness", 0.7f), 0.05f, 1.0f) };
    /// @note 外周フェードの幅はメートルで持ち、軸ごとに extent で割って UV へ直す。UV 比で
    /// @note 持つと大きい水面ほど帯が広くなり、縁から何 m で消えるかが大きさに依存してしまう。
    const float edgeFadeMeters = (std::max)(ReadWaterMaterialFloat(material, "edgeFade", 1.5f), 0.0f);
    constants.refractionFlowParams = {
        ReadWaterMaterialFloat(material, "refractionStrength", 0.03f),
        ReadWaterMaterialFloat(material, "flowSpeed",          0.3f),
        edgeFadeMeters / worldExtentX,
        edgeFadeMeters / worldExtentZ
    };
    constants.detailParams = {
        ReadWaterMaterialFloat(material, "detailScale",    0.35f),
        ReadWaterMaterialFloat(material, "detailSpeed",    0.6f),
        ReadWaterMaterialFloat(material, "detailStrength", 1.0f),
        math::Clamp01(ReadWaterMaterialFloat(material, "smoothness", 0.92f))
    };
    const auto sssColor = ReadWaterMaterialVector3(material, "sssColor", { 0.12f, 0.50f, 0.46f });
    constants.sssParams = { sssColor.x, sssColor.y, sssColor.z,
                     math::Clamp01(ReadWaterMaterialFloat(material, "sssStrength", 0.6f)) };
    /// @note 波の山ほど透過光を強くするため、CPU 側と同じ「振幅の合計」を波高の基準として渡す。
    /// @note 群の包絡と方向広がりのぶんも含める。含めないと、山に当たった波だけ waveState.x が 1 で
    /// @note 飽和し、透過光と白波が «そこだけ最大» に張り付く。
    const float amplitudeMultiplier = (1.0f + math::Clamp01(water.waveGrouping))
                     * WaterComponent::WaveSpreadAmplitudeSum(water.waveSpread);
    float waveHeightSum = 0.0f;
    if (water.enableGerstnerWaves)
        for (size_t waveIndex = 0; waveIndex < (std::min)(static_cast<size_t>(water.waveCount), water.waves.size()); ++waveIndex)
            waveHeightSum += (std::max)(water.waves[waveIndex].amplitude, 0.0f);
    constants.reflectParams = {
        skyReflection * math::Clamp01(ReadWaterMaterialFloat(material, "skyReflection", 1.0f)),
        math::Clamp01(water.waveGrouping),
        transform.worldPosition.y,
        (std::max)(waveHeightSum * amplitudeMultiplier, 0.01f)
    };
    const auto flowDirection = ReadWaterMaterialVector2(material, "flowDirection", { 1.0f, 0.0f });
    const float flowDirectionLength = std::sqrt(flowDirection.x * flowDirection.x + flowDirection.y * flowDirection.y);
    /// @note zw はさざ波の «形»。異方比は風向と直交する «うね» の伸び、ワープ幅はうねりの斜面が
    /// @note さざ波を運ぶ距離 [m]。どちらも 1.0 / 0.0 にすれば従来の等方・非追従へ戻る。
    const math::Vector2 detailShape = {
        (std::max)(ReadWaterMaterialFloat(material, "detailAnisotropy", 2.0f), 1.0f),
        (std::max)(ReadWaterMaterialFloat(material, "detailWarp",       0.5f), 0.0f)
    };
    constants.flowParams = flowDirectionLength > 1.0e-4f
        ? math::Vector4{ flowDirection.x / flowDirectionLength, flowDirection.y / flowDirectionLength, detailShape.x, detailShape.y }
        : math::Vector4{ 1.0f, 0.0f, detailShape.x, detailShape.y };
    WriteSurfaceFlowConstants(water, constants, resources);
    WriteGerstnerWaveConstants(water, constants);
    return constants;
}

void ApplySplineWaterConstants(const WaterComponent& water, const WaterSplineSnapshot& spline,
    renderer::WaterCB& constants)
{
    constants.gridParams.x = 0.0f;
    constants.farOceanParams = {};
    const float maximumWorldEdge = spline.maximumWorldEdgeLength;
    if (std::isfinite(maximumWorldEdge) && maximumWorldEdge > 0.0f)
        constants.timeParams.x = constants.timeParams.y = maximumWorldEdge;
    if (spline.shape == WaterSurfaceShape::SPLINE_RIVER && water.enableGerstnerWaves && water.waveCount > 0) {
        /// @note ビュー間で位相の周期を揃え、共有メッシュ用には長い周期の帯域を確保する。
        const float referenceSpeed = (std::max)(spline.riverWaveReferenceSpeed, 0.4f);
        constants.riverWaveParams.x = (std::clamp)(3.8f * constants.timeParams.x / referenceSpeed,
            2.25f, 120.0f);
        constants.riverWaveParams.z = WATER_RIVER_WAVE_REFERENCE_LENGTHS.x;
        constants.riverWaveParams.w = WATER_RIVER_WAVE_REFERENCE_LENGTHS.y;
    }
    if (spline.shape == WaterSurfaceShape::SPLINE_RIVER && spline.outlineIndices.size() >= 4) {
        const auto& outlineIndices = spline.outlineIndices;
        const size_t middleIndex = outlineIndices.size() / 2;
        const auto writeHorizonCap = [&](size_t capIndex, size_t leftOutlineIndex, size_t rightOutlineIndex) {
            const auto& leftBank = spline.vertices[outlineIndices[leftOutlineIndex]].worldPosition;
            const auto& rightBank = spline.vertices[outlineIndices[rightOutlineIndex]].worldPosition;
            const auto center = leftBank + (rightBank - leftBank) * 0.5f;
            const float width = std::hypot(leftBank.x - rightBank.x, leftBank.z - rightBank.z);
            constants.riverHorizonCaps[capIndex] = {center.x, center.z,
                (std::clamp)(width * 1.5f, 8.0f, 48.0f), center.y};
        };
        writeHorizonCap(0, 0, outlineIndices.size() - 1);
        writeHorizonCap(1, middleIndex - 1, middleIndex);
    }
}

renderer::WaterEffectParams BuildWaterEffectParams(const asset::MaterialAssetAuthoring* material)
{
    renderer::WaterEffectParams effects{};
    effects.rimGlowStrength    = ReadWaterMaterialFloat(material, "rimGlowStrength",    effects.rimGlowStrength);
    effects.minShallowAlpha    = ReadWaterMaterialFloat(material, "minShallowAlpha",    effects.minShallowAlpha);
    effects.specularStrength   = ReadWaterMaterialFloat(material, "specularStrength",   effects.specularStrength);
    const math::Vector3 skyTint = ReadWaterMaterialVector3(material, "skyReflectTint", {
        effects.skyReflectTint[0], effects.skyReflectTint[1], effects.skyReflectTint[2]
    });
    effects.skyReflectTint[0] = skyTint.x;
    effects.skyReflectTint[1] = skyTint.y;
    effects.skyReflectTint[2] = skyTint.z;
    const math::Vector3 rippleColor = ReadWaterMaterialVector3(material, "rippleRingColor", {
        effects.rippleRingColor[0], effects.rippleRingColor[1], effects.rippleRingColor[2]
    });
    effects.rippleRingColor[0] = rippleColor.x;
    effects.rippleRingColor[1] = rippleColor.y;
    effects.rippleRingColor[2] = rippleColor.z;
    effects.rippleRingStrength = ReadWaterMaterialFloat(material, "rippleRingStrength", effects.rippleRingStrength);
    return effects;
}

void ResolveZoneWaterMaterials(Scene& scene, EntityID owner, const WaterComponent& water, const Transform& transform,
    renderer::RenderWaterInput& input, const renderer::WaterDetailNoise& detailNoise,
    renderer::ResourceManager& resources)
{
    const auto* zone = scene.GetComponent<WaterZoneComponent>(owner);
    if (zone && zone->simulationSnapshot && !zone->simulationSnapshot->materials.empty()) {
        const auto ownerProfile = MakeWaterMaterialBlendTarget(input.constants, input.effects);
        input.zoneMaterials.reserve(zone->simulationMaterialSources.size());
        for (const auto sourceId : zone->simulationMaterialSources) {
            const auto* sourceWater = scene.GetComponent<WaterComponent>(sourceId);
            if (!sourceWater || sourceWater->materialPath == water.materialPath) {
                input.zoneMaterials.push_back(ownerProfile);
                continue;
            }
            const auto sourceAuthoringSnapshot = LoadWaterMaterial(sourceWater->materialPath);
            const auto* sourceMaterial = sourceAuthoringSnapshot ? &sourceAuthoringSnapshot->Data() : nullptr;
            const auto sourceConstants = BuildWaterConstants(*sourceWater, sourceMaterial, transform,
                math::Matrix4::Identity(), Time::time, 1.0f, detailNoise, resources);
            input.zoneMaterials.push_back(MakeWaterMaterialBlendTarget(sourceConstants,
                BuildWaterEffectParams(sourceMaterial)));
        }
    }
}

void BindWaterAppearance(const asset::MaterialAssetAuthoring* material, WaterRenderCache& cache,
    const WaterCacheKey& key, renderer::RenderWaterInput& input, renderer::ResourceManager& resources)
{
    if (!material || material->shaderPath.empty()) return;

    FBZZ_PROFILE_SCOPE("WaterPrepare::Shader");
    renderer::Material& appearance = cache.AppearanceFor(key);
    appearance.shaderPath = material->shaderPath;
    appearance.shader = resources.LoadShader(material->shaderPath);
    input.shader = appearance.shader;
    if (const auto* shader = resources.Get(appearance.shader)) {
        /// @note ホットリロードで所有者が変わり得るため、この公開分のシェーダーリフレクションを複製する。
        const renderer::ShaderDescriptor descriptor = shader->GetDescriptor();
        asset::InitializeWaterAppearanceParameters(*material, descriptor, appearance.paramData);
        const auto texturePaths = asset::ResolveMaterialTexturePaths(*material);
        appearance.textures.resize(texturePaths.size());
        for (size_t textureSlot = 0; textureSlot < texturePaths.size(); ++textureSlot)
            appearance.textures[textureSlot] = texturePaths[textureSlot].empty()
                ? renderer::ResourceHandle<renderer::TextureTag>{}
                : asset::StreamedTextureResolver::Engine().ResolveGpu(resources, texturePaths[textureSlot]);
        appearance.Upload(resources, descriptor);
        input.appearanceParameters = appearance.paramsBuffer;
        std::copy(appearance.textures.begin(), appearance.textures.end(), input.appearanceTextures.begin());
    }
}

void LinkHorizonWaterMaterials(std::vector<renderer::RenderWaterInput>& waterInputs,
    std::span<const HorizonMaterialLink> materialLinks)
{
    const auto isHorizon = [](const auto& water) { return water.farOceanEnabled; };
    const auto firstHorizon = std::find_if(waterInputs.begin(), waterInputs.end(), isHorizon);
    const bool multipleHorizons = firstHorizon != waterInputs.end()
        && std::find_if(firstHorizon + 1, waterInputs.end(), isHorizon) != waterInputs.end();
    std::unordered_map<uint64_t, const renderer::RenderWaterInput*> horizonReceivers;
    if (multipleHorizons && !materialLinks.empty()) {
        for (auto horizon = firstHorizon; horizon != waterInputs.end(); ++horizon) {
            if (horizon->farOceanEnabled) horizonReceivers.try_emplace(horizon->medium.stableOwnerKey, &*horizon);
        }
    }
    for (const HorizonMaterialLink& link : materialLinks) {
        const renderer::RenderWaterInput* receiver = nullptr;
        if (multipleHorizons) {
            if (const auto found = horizonReceivers.find(link.receiverKey); found != horizonReceivers.end())
                receiver = found->second;
        } else if (firstHorizon != waterInputs.end() && firstHorizon->medium.stableOwnerKey == link.receiverKey) {
            receiver = &*firstHorizon;
        }
        if (!receiver) continue;
        auto& river = waterInputs[link.inputIndex];
        auto& riverConstants = river.constants;
        river.horizonReceiverKey = link.receiverKey;
        river.medium.priority = receiver->medium.priority + 1;
        for (auto& cap : riverConstants.riverHorizonCaps)
            if (cap.x == link.mouth.x && cap.y == link.mouth.y) cap = {};
        riverConstants.materialBlendMouth = link.mouth;
        riverConstants.materialBlendTarget = MakeWaterMaterialBlendTarget(receiver->constants, receiver->effects);
    }
    /// @note 接続先の水平海面を一意に特定できるときだけ、境界の限られた範囲で見た目を遷移させる。
    const bool singleHorizon = firstHorizon != waterInputs.end() && !multipleHorizons && firstHorizon->medium.exclusions.empty();
    for (auto& water : waterInputs) {
        for (auto& cap : water.constants.riverHorizonCaps) {
            if (cap.z <= 0.0f) continue;
            const float receivingSurfaceY = singleHorizon ? firstHorizon->medium.surfaceY : 0.0f;
            if (!singleHorizon || !std::isfinite(receivingSurfaceY) || std::abs(cap.w - receivingSurfaceY) > 1.0f) {
                cap = {};
                continue;
            }
            cap.w = receivingSurfaceY;
        }
    }
}

}
