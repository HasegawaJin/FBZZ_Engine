/// @file    BoundsDebugPass.cpp
/// @brief   Terrain の範囲箱と LODGroup の判定球を描く。
/// @author  Hasegawa Jin
/// @date    2026-09-16
#include "DebugPasses.hpp"
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Components/LODGroupComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::scene {

namespace {

/// @brief LOD 段ごとの色。0 = 緑 → 段が下がるほど赤、-1 (未決定 / カリング) は灰。
math::Vector4 LodColor(int level)
{
    static constexpr math::Vector4 kLevels[] = {
        { 0.30f, 1.00f, 0.40f, 1.0f }, { 0.85f, 1.00f, 0.30f, 1.0f },
        { 1.00f, 0.75f, 0.25f, 1.0f }, { 1.00f, 0.40f, 0.25f, 1.0f },
    };
    if (level < 0) return { 0.5f, 0.5f, 0.55f, 1.0f };
    return kLevels[(std::min)(level, 3)];
}

void DrawTerrainBounds(RenderPassContext& ctx)
{
    constexpr math::Vector4 kTerrainColor = { 0.75f, 0.60f, 0.35f, 1.0f };
    for (EntityID id : ctx.scene.GetEntities<TerrainComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        auto* terrain = ctx.scene.GetComponent<TerrainComponent>(id);
        if (!go || !terrain || !terrain->enabled || !go->activeInHierarchy()) continue;
        /// @note 原点は角 (worldPosition) で +X / +Z へ広がる。高さは heightData × maxHeight で [-max, +max]。
        const float halfWidth = static_cast<float>((std::max)(terrain->columns - 1, 0)) * terrain->cellSize * 0.5f;
        const float halfDepth = static_cast<float>((std::max)(terrain->rows - 1, 0)) * terrain->cellSize * 0.5f;
        const math::Vector3 center = go->transform.worldPosition + math::Vector3{ halfWidth, 0.0f, halfDepth };
        renderer::DebugDraw::Box(ctx.renderer, center, { halfWidth, std::abs(terrain->maxHeight), halfDepth },
                                 kTerrainColor);
    }
}

void DrawLodBounds(RenderPassContext& ctx)
{
    for (EntityID id : ctx.scene.GetEntities<LODGroupComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        auto* lod = ctx.scene.GetComponent<LODGroupComponent>(id);
        if (!go || !lod || !lod->enabled || !go->activeInHierarchy()) continue;
        /// @note LODSystem と同じ判定球 (中心 = worldPosition、直径 = size × 最大スケール)。
        const math::Vector3& scale = go->transform.worldScale;
        const float maxScale = (std::max)({ std::abs(scale.x), std::abs(scale.y), std::abs(scale.z) });
        const float radius = lod->size * maxScale * 0.5f;
        renderer::DebugDraw::Sphere(ctx.renderer, go->transform.worldPosition, radius, LodColor(lod->activeLevel));
    }
}

} // namespace

std::string_view BoundsDebugPass::Name() const { return "BoundsDebug"; }

bool BoundsDebugPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.showTerrainBounds || ctx.settings.showLODBounds;
}

void BoundsDebugPass::Execute(PassResources&, RenderPassContext& ctx)
{
    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetViewProjection());
    if (ctx.settings.showTerrainBounds) DrawTerrainBounds(ctx);
    if (ctx.settings.showLODBounds) DrawLodBounds(ctx);
    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
