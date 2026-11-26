/// @file    RenderScenePassHelpers.hpp
/// @brief   抽出済みメッシュ入力からビュー別の描画コマンドを組み立てる。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#pragma once
#include <Graphics/Renderer/Commands/ShaderBindingSlots.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Passes/Geometry/RenderSceneDraw.hpp>
#include <Graphics/Scene/RenderScene.hpp>
#include <Graphics/Renderer/Visibility/RenderVisibility.hpp>
#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace fbzz::renderer {

inline const renderer::RenderScene& SceneInput(const RenderPassContext& ctx)
{
    assert(ctx.renderScene && "RenderScene must be extracted after skinning and before drawing");
    return *ctx.renderScene;
}

inline bool MatchesView(const RenderPassContext& ctx, const renderer::RenderObject& object)
{
    return object.lodVisible && (object.layer < 32 && (ctx.cullingMask & (uint32_t{1} << object.layer)) != 0);
}

inline WorldBounds RenderBounds(const RenderPassContext& ctx, const renderer::RenderObject& object)
{
    return { object.boundsCenter, object.boundsRadius > 0.0f
        ? object.boundsRadius + (std::max)(ctx.cullingBoundsPadding, 0.0f) : 0.0f };
}

/// @note 実形状内部でも切り抜き・変形・フェード・深度移動があれば不透明被覆の証明には使えない。
/// @see Assets/Shaders/Material/Surface/PBR.hlsl MaterialConstants とアルファ・葉の頂点契約。
inline float OccluderRadius(const RenderPassContext& ctx, const RenderObject& object,
    const RenderMeshItem& item, const RenderMaterial& material)
{
    if (object.skinned || object.lodDither != 0 || ctx.settings.IsWireframe()
        || !material.valid || material.capabilities.blend != BlendMode::OPAQUE_BLEND
        || material.shaderCapabilities.vertex != ShaderVertexContract::STANDARD_SURFACE
        || !material.shaderCapabilities.HasStandardVariants()
        || material.shaderCapabilities.opacity == ShaderOpacityContract::UNKNOWN
        || material.depthBias != 0 || material.depthBiasSlope != 0) {
        return 0;
    }
    uint32_t textureMask;
    std::memcpy(&textureMask, material.gbufferParams.data() + 80, sizeof(textureMask));
    // 浮動小数点のアルベドは負のアルファを持ち得るため、cutoff=0 だけでは被覆を証明できない。
    if (!material.shaderCapabilities.HasOpaqueOutput() && ((textureMask & (1u << srv::ALBEDO)) != 0 || material.textures[srv::ALBEDO].IsValid())) {
        return 0;
    }
    float alpha, cutoff, wind;
    std::memcpy(&alpha, material.gbufferParams.data() + 12, sizeof(alpha));
    std::memcpy(&cutoff, material.gbufferParams.data() + 64, sizeof(cutoff));
    std::memcpy(&wind, material.gbufferParams.data() + 68, sizeof(wind));
    if (!std::isfinite(alpha) || alpha < 0 || !std::isfinite(cutoff) || cutoff > 0
        || !std::isfinite(wind) || wind != 0) {
        return 0;
    }
    return item.occluderRadius;
}

inline renderer::RenderCullingView CullingView(const RenderPassContext& ctx)
{
    return { ctx.camera.m_position, ctx.cullCameraForward,
        ctx.frustumCullingEnabled ? ctx.cameraFrustum : nullptr,
        ctx.cullProjScaleY, ctx.smallObjectScreenHeight, ctx.cullDistanceSpherical, ctx.cullOrthographic };
}

inline float DrawDistance(const RenderPassContext& ctx, const renderer::RenderObject& object)
{
    const float layerDistance = ctx.cullLayerDistances[object.layer & (32 - 1)];
    return ctx.hasLayerCullDistances && layerDistance > 0.0f ? layerDistance : ctx.cullMaxDistance;
}

inline bool IsRenderObjectVisible(RenderPassContext& ctx, const renderer::RenderObject& object, bool gpuFrustum = false)
{
    const auto bounds = RenderBounds(ctx, object);
    auto view = CullingView(ctx);
    bool gpuEstimateOutside = false;
    // インスタンスバッチが GPU 上で視錐台を判定し、CPU カウンターは推定値だけを記録する。
    if (gpuFrustum && view.frustum) {
        gpuEstimateOutside = !view.frustum->IntersectsSphere(bounds.center, bounds.radius);
        view.frustum = nullptr;
    }
    switch (renderer::EvaluateVisibility(view,
        { bounds.center, bounds.radius, DrawDistance(ctx, object) })) {
    case renderer::VisibilityResult::DISTANCE_CULLED: ++ctx.statsDistanceCulled; return false;
    case renderer::VisibilityResult::SMALL_OBJECT_CULLED: ++ctx.statsSmallObjectCulled; return false;
    case renderer::VisibilityResult::FRUSTUM_CULLED: ++ctx.statsFrustumCulled; return false;
    case renderer::VisibilityResult::VISIBLE:
        if (gpuEstimateOutside) {
            ++ctx.statsFrustumCulled;
        }
        if (IsViewOccluded(ctx, bounds.center, bounds.radius)) {
            if (!gpuEstimateOutside) {
                ++ctx.statsOcclusionCulled;
            }
            return false;
        }
        return true;
    }
    return true;
}

/// @note t14/t15 は VS のボーンパレット専用で、粒子やメッシュレットの描画には束縛しない。
inline void BindGpuBonePalette(DrawCall& draw, const RenderObject& object)
{
    if (object.gpuBones.current.IsValid()) { draw.vsBuffers[VertexBufferIndex(vsSlot::BUFFER0)] = object.gpuBones.current; draw.constantBuffers[cb::SKINNING] = {}; }
}

inline auto MaterialPipeline(RenderPassContext& ctx, const renderer::RenderMaterial& material)
{
    return GetOrCreateMaterialPSO(ctx.resources, material.capabilities.blend,
        material.doubleSided, material.depthBias, material.depthBiasSlope, ctx.settings.IsWireframe());
}

/// @brief メインビューのシャドウ描画より前に使うプローブ撮影用の定数を更新する。
/// @param[in,out] ctx 現在の照明設定とプローブ撮影用の定数バッファ。メインビューの定数は保持する。
/// @return 撮影用の定数バッファを更新できた場合は true。不足時は定数を更新せず false。
[[nodiscard]] bool PrepareProbeCaptureShadowConstants(RenderPassContext& ctx);

/// @brief プローブ撮影に線形ライト一覧と撮影専用の影設定を束縛する。
/// @param[in,out] draw プローブ撮影の描画コマンド。
/// @param[in] ctx プローブ撮影用の定数バッファと照明リソース。
inline void BindProbeCaptureLightingResources(DrawCall& draw, const RenderPassContext& ctx)
{
    BindForwardShadingResources(draw, ctx, true);
    draw.constantBuffers[cb::SHADOW] = ctx.handles.reflectionProbeCaptureShadowCB;
    draw.constantBuffers[cb::PUNCTUAL_SHADOW] = ctx.handles.reflectionProbeCapturePunctualCB;

    // メインビュー用のアトラスと画面空間テクスチャは、描画前のプローブ撮影では参照できない。
    for (const uint32_t slot : {srv::SHADOW, srv::SCREEN_AO, srv::CONTACT_SHADOW, srv::PUNCTUAL_SHADOW, srv::LIGHT_COOKIE}) {
        draw.textures[slot] = {};
    }
}

inline bool HasColorGeometry(const renderer::RenderMeshItem& item)
{
    return item.visible && item.vertexBuffer.IsValid() && item.indexBuffer.IsValid();
}

/// @note 現在のビューで解決した資源を借用する。登録簿やハンドルをパス・ビューの外に保持しない。
/// @note 長いリソース名の一時文字列確保を個体ごとに繰り返さないよう、走査の前に解決する。
/// @see https://github.com/microsoft/STL/blob/main/stl/inc/xstring basic_string の短い文字列の格納と string_view からの構築。
inline DrawCall ForwardPassBindings(RenderPassContext& ctx, bool bindIbl = true)
{
    const auto& h = ctx.handles;
    DrawCall draw;
    draw.constantBuffers[cb::CAMERA] = h.frameCB;
    draw.constantBuffers[cb::OBJECT] = h.objectCB;
    draw.constantBuffers[cb::LIGHT] = h.lightCB;
    draw.constantBuffers[cb::SHADOW] = h.shadowCB;
    draw.constantBuffers[cb::ADVANCED_GRAPHICS] = h.advancedGraphicsCB;
    BindForwardShadingResources(draw, ctx);
    draw.textures[srv::SHADOW] = ctx.resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));
    if (bindIbl) {
        draw.textures[srv::IBL_IRRADIANCE] = h.iblIrradiance;
        draw.textures[srv::IBL_PREFILTER] = h.iblPrefilter;
        draw.textures[srv::IBL_BRDF_LUT] = h.iblBrdfLut;
    }
    return draw;
}

inline DrawCall ForwardDrawWithBindings(RenderPassContext& ctx, const RenderObject& object,
                                       const RenderMeshItem& item, const DrawCall& bindings)
{
    const auto& material = item.forwardMaterial;
    DrawCall draw = bindings;
    draw.vertexBuffer = object.skinned ? item.skinningVertexBuffer : item.vertexBuffer;
    draw.indexBuffer = item.indexBuffer;
    draw.indexCount = item.indexCount;
    draw.vertexCount = item.vertexCount;
    draw.shader = material.shader;
    draw.pipelineState = MaterialPipeline(ctx, item.material);
    draw.layer = material.capabilities.blend == BlendMode::OPAQUE_BLEND
        ? RenderLayer::OPAQUE_LAYER : RenderLayer::TRANSPARENT_LAYER;
    draw.constantBuffers[cb::MATERIAL] = material.paramsBuffer;
    if (object.skinned) { draw.constantBuffers[cb::SKINNING] = object.skinningPalette; BindGpuBonePalette(draw, object); }
    for (size_t i = 0; i < material.textures.size(); ++i) {
        draw.textures[i] = material.textures[i];
    }
    return draw;
}

}
