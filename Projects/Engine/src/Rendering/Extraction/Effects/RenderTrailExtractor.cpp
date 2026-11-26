/// @file    RenderTrailExtractor.cpp
/// @brief   軌跡の描画資源を準備してサンプル履歴を描画へ渡す。
/// @author  Hasegawa Jin
/// @date    2026-06-18

#include "RenderTrailSampling.hpp"
#include <Engine/Rendering/Extraction/Effects/RenderTrailExtractor.hpp>
#include <Engine/Rendering/Passes/RenderPassContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/Particles/ParticleColorSpace.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Material/MaterialAsset.hpp>
#include <Engine/Asset/Streaming/StreamedTextureResolver.hpp>
#include <Engine/Core/Timing/Time.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>
#include <Graphics/Scene/RenderScene.hpp>
#include <algorithm>

namespace fbzz::scene {

namespace {

void PrepareTrailResources(TrailComponent& trail, RenderPassContext& context)
{
    auto& resources = context.resources;
    trail.maxPoints = (std::max)(trail.maxPoints, 2);
    trail.duration = (std::max)(trail.duration, 0.01f);
    trail.sampleInterval = (std::max)(trail.sampleInterval, 0.0f);
    trail.minVertexDist = (std::max)(trail.minVertexDist, 0.0f);
    trail.smoothSubdivisions = (std::max)(trail.smoothSubdivisions, 0);
    trail.uvTiling = (std::max)(trail.uvTiling, 0.001f);

    if (trail.pointBuffer.size() != static_cast<size_t>(trail.maxPoints)) {
        trail_extraction_detail::ResetTrailSampleStorage(trail);
    }

    if (!trail.trailCB.IsValid()) {
        trail.trailCB = resources.CreateConstantBuffer(sizeof(TrailCB));
    }

    // materialPath が設定されている場合: .mat の albedo テクスチャを優先する。
    trail.texture = resources.GetWhiteTexture();
    if (!trail.materialPath.empty()) {
        const bool matChanged = (trail.loadedMaterialPath != trail.materialPath);
        if (matChanged) {
            trail.loadedMaterialPath = trail.materialPath;
            trail.loadedTexturePath.clear();
        }
        const auto matHandle = asset::AssetManager::Load<asset::MaterialAsset>(trail.materialPath);
        if (const auto matAuthoringSnapshot = asset::ReadMaterialAuthoring(matHandle); matAuthoringSnapshot) {
            const auto* material = &matAuthoringSnapshot->Data();
            const auto it = material->textures.find("albedo");
            const std::string& resolvedTex = (it != material->textures.end()) ? it->second : std::string{};
            {
                if (resolvedTex.empty()) {
                    trail.texture = resources.GetWhiteTexture();
                } else {
                    trail.texture = asset::StreamedTextureResolver::Engine().ResolveGpu(resources, resolvedTex);
                }
                trail.loadedTexturePath = resolvedTex;
            }
        }
    } else {
        // 共有の 1 枚を借りる。実体ごとに作ると、その実体が畳まれたぶんだけ GPU に残る。
        trail.texture = resources.GetWhiteTexture();
        trail.loadedTexturePath.clear();
    }
}

renderer::RenderTrailInput BuildTrailInput(const GameObject& owner, const TrailComponent& trail)
{
    renderer::RenderTrailInput input;
    input.layer = static_cast<uint32_t>(owner.layer);
    input.duration = trail.duration;
    input.widthStart = trail.widthStart;
    input.widthEnd = trail.widthEnd;
    input.widthCurveEnabled = trail.widthCurveEnabled;
    input.widthCurve = trail.widthCurve;
    input.colorGradientEnabled = trail.colorGradientEnabled;
    input.colorGradient = trail.colorGradient;
    input.widthEasing = trail.widthEasing;
    input.colorStart = trail.colorStart;
    input.colorEnd = trail.colorEnd;
    input.alignment = trail.alignment;
    input.smoothSubdivisions = trail.smoothSubdivisions;
    input.uvMode = trail.uvMode;
    input.uvScrollSpeed = trail.uvScrollSpeed;
    input.uvTiling = trail.uvTiling;
    input.texture = trail.texture;
    input.trailCB = trail.trailCB;
    input.srgbTexture = IsEffectTextureSrgb(trail.loadedTexturePath);
    input.points.reserve(static_cast<size_t>(trail.ringCount));
    for (int pointIndex = 0; pointIndex < trail.ringCount; ++pointIndex) {
        input.points.push_back(trail_extraction_detail::ReadTrailPoint(trail, pointIndex));
    }
    return input;
}

} // namespace

void ExtractRenderTrails(RenderPassContext& context, renderer::RenderScene& output)
{
    if (context.scene.GetEntities<TrailComponent>().empty()) {
        return;
    }
    const float currentTime = Time::time;
    for (auto& owner : context.scene.GameObjects()) {
        if (!owner.activeInHierarchy()) {
            continue;
        }

        auto* trail = owner.GetComponent<TrailComponent>();
        if (!trail) {
            continue;
        }

        if (!trail->enabled && trail->clearOnDisable) {
            trail_extraction_detail::ClearTrailSamples(*trail);
            continue;
        }

        PrepareTrailResources(*trail, context);

        trail_extraction_detail::AdvanceTrailSamples(context.scene, owner, *trail, currentTime, context.resources.FrameStamp());
        if (trail->ringCount < 2) {
            continue;
        }

        output.trails.push_back(BuildTrailInput(owner, *trail));
    }
}

} // namespace fbzz::scene
