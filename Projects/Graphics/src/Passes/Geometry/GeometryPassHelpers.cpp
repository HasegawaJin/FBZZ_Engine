/// @file    GeometryPassHelpers.cpp
/// @brief   Graphics の共通 PSO と定数バッファ。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <map>
namespace fbzz::renderer {
renderer::ResourceHandle<renderer::PipelineStateTag> GetOrCreateMaterialPSO(
    renderer::ResourceManager& resources,
    renderer::BlendMode        blend,
    bool                       doubleSided,
    int32_t                    depthBias,
    float                      depthBiasSlope,
    bool                       wireframe)
{
    /// @note 両面描画はバックフェースカリングを無効化する。
    const renderer::RasterizerMode raster = wireframe
        ? (doubleSided ? renderer::RasterizerMode::WIREFRAME_NOCULL : renderer::RasterizerMode::WIREFRAME)
        : (doubleSided ? renderer::RasterizerMode::SOLID_NOCULL : renderer::RasterizerMode::SOLID);
    /// @note 半透明・加算は深度書き込みをオフにし、背後のオブジェクトが透けて見えるようにする。
    const renderer::DepthMode depth = (blend == renderer::BlendMode::OPAQUE_BLEND)
        ? renderer::DepthMode::DEPTH_ON
        : renderer::DepthMode::DEPTH_READ;

    /// @note ビットパッキングは enum 値追加時にサイレントなキー衝突が起きるため、構造体を直接比較する std::map を使う。
    struct DescLess {
        bool operator()(const renderer::PipelineStateDesc& a,
                        const renderer::PipelineStateDesc& b) const noexcept {
            if (a.rasterizer != b.rasterizer) return a.rasterizer < b.rasterizer;
            if (a.blend      != b.blend)      return a.blend      < b.blend;
            if (a.depth      != b.depth)      return a.depth      < b.depth;
            if (a.depthBias  != b.depthBias)  return a.depthBias  < b.depthBias;
            return a.depthBiasSlope < b.depthBiasSlope;
        }
    };
    static std::map<renderer::PipelineStateDesc,
                    renderer::ResourceHandle<renderer::PipelineStateTag>,
                    DescLess> s_cache;
    const renderer::PipelineStateDesc desc{ raster, blend, depth, depthBias, depthBiasSlope };
    auto it = s_cache.find(desc);
    if (it != s_cache.end()) {
        /// @note Reset や別 ResourceManager で同じ番号が再利用されても、失効・別設定の PSO を返さない。
        if (const auto* existing = resources.Get(it->second)) {
            const auto& cached = existing->GetDesc();
            if (!DescLess{}(cached, desc) && !DescLess{}(desc, cached)) return it->second;
        }
    }
    auto handle = resources.CreatePipelineState(desc);
    s_cache[desc] = handle;
    return handle;
}


ShadowConstantsCB MakeShadowConstants(const RenderPassContext& ctx)
{
    const auto& rs = ctx.settings;
    ShadowConstantsCB data{};

    /// @note 全カスケードが共有する 1 枚のアトラスなので、テクセルサイズはアトラス全体基準。
    /// @note カスケード内 UV → アトラス UV への写像は HLSL 側 (cascadeAtlasRect) が行う。
    const float texel = 1.0f / static_cast<float>((std::max)(rs.shadow.mapResolution, 1u));
    data.shadowMapTexelSize[0] = texel;
    data.shadowMapTexelSize[1] = texel;

    const int count = std::clamp(ctx.shadowCascadeCount, 1, renderer::kMaxShadowCascades);
    data.cascadeCount     = count;
    data.cascadeBlend     = std::clamp(rs.shadow.cascadeBlend, 0.0f, 0.5f);
    /// @note 可視化は分割している時だけ意味がある。1 分割で有効なままだと画面全体が
    /// @note カスケード 0 の色に染まるだけなので、ここで落とす。
    data.cascadeDebugView = (rs.shadow.debugVisualizeCascades && count > 1) ? 1 : 0;

    float bias[renderer::kMaxShadowCascades] = {};
    for (int i = 0; i < renderer::kMaxShadowCascades; ++i) {
        /// @note 未使用スロットは最遠カスケードで埋める。HLSL 側は cascadeCount までしか
        /// @note 見ないが、未初期化の行列が残ると RenderDoc 等で追うときに紛らわしい。
        const ShadowCascade& cascade = ctx.shadowCascades[(i < count) ? i : count - 1];
        data.cascadeViewProjection[i] = cascade.viewProjection;
        data.cascadeAtlasRect[i]      = cascade.atlasRect;
        bias[i]                       = cascade.biasNDC;
    }
    data.cascadeBias = { bias[0], bias[1], bias[2], bias[3] };
    data.cascadeSplitFar = {
        ctx.shadowCascades[0].splitFar,
        ctx.shadowCascades[(std::min)(1, count - 1)].splitFar,
        ctx.shadowCascades[(std::min)(2, count - 1)].splitFar,
        ctx.shadowCascades[(std::min)(3, count - 1)].splitFar
    };
    const math::Vector3 forward = ctx.camera.GetForward();
    data.shadowCameraPosition = { ctx.camera.m_position.x, ctx.camera.m_position.y, ctx.camera.m_position.z, 0.0f };
    data.shadowCameraForward = { forward.x, forward.y, forward.z, 0.0f };

    /// @note 単一のライト行列で足りるパス向け (= 最遠カスケード)。
    /// @note cascadeCount == 1 のときはカスケード 0 と同一なので、従来の単一シャドウマップ経路と一致する。
    data.lightViewProjection = ctx.lightVP;
    data.shadowBias          = ctx.shadowBiasNDC;
    data.shadowStrength      = ctx.shadowStrength;
    data.shadowPcfRadius     = rs.shadow.pcfRadius;

    data.cloudShadowStrength = ctx.cloudShadowStrength;
    data.cloudShadowCoverage = ctx.cloudShadowCoverage;
    data.cloudShadowScale    = ctx.cloudShadowScale;
    data.cloudShadowSpeed    = ctx.cloudShadowSpeed;
    data.cloudShadowTime     = ctx.cloudShadowTime;
    data.cloudShadowWindX    = ctx.cloudShadowWindX;
    data.cloudShadowWindZ    = ctx.cloudShadowWindZ;

    return data;
}

void UpdateShadowConstants(RenderPassContext& ctx)
{
    const auto data = MakeShadowConstants(ctx);
    ctx.resources.Update(ctx.handles.shadowCB, &data, sizeof(data));
}

PunctualShadowConstantsCB MakePunctualShadowConstants(const RenderPassContext& ctx)
{
    const auto& rs = ctx.settings;
    PunctualShadowConstantsCB data{};

    /// @note 全スロットが 1 枚のアトラスを共有するので、テクセルサイズはアトラス全体基準。
    /// @note タイル内 UV → アトラス UV への写像は HLSL 側 (punctualShadowRect) が行う。
    const float texel =
        1.0f / static_cast<float>((std::max)(ctx.punctualShadowResolution, 1u));
    data.punctualShadowTexel[0] = texel;
    data.punctualShadowTexel[1] = texel;
    data.punctualShadowPcf      = std::clamp(rs.shadow.punctualPcfRadius, 0, 3);

    /// @note 未使用スロットは 0 のまま残す。HLSL 側は punctualShadowCount までしか見ない。
    const int count = std::clamp(ctx.punctualShadowViewCount, 0, kMaxPunctualShadows);
    data.punctualShadowCount = count;
    for (int i = 0; i < count; ++i) {
        const PunctualShadowView& view = ctx.punctualShadowViews[i];
        data.punctualShadowVP[i]     = view.viewProjection;
        data.punctualShadowRect[i]   = view.atlasRect;
        data.punctualShadowParams[i] =
            { view.biasNDC, view.shadowStrength, view.penumbraTexels, 0.0f };
    }

    data.lightCookieTexel[0] = 1.0f / static_cast<float>(kLightCookieAtlasWidth);
    data.lightCookieTexel[1] = 1.0f / static_cast<float>(kLightCookieAtlasHeight);
    const int cookieCount = std::clamp(ctx.lightCookieViewCount, 0, kMaxLightCookies);
    data.lightCookieCount = cookieCount;
    for (int i = 0; i < cookieCount; ++i) {
        data.lightCookieVP[i]   = ctx.lightCookieViews[i].viewProjection;
        data.lightCookieRect[i] = ctx.lightCookieViews[i].atlasRect;
    }

    /// @note レガシー経路の「大きさを持つ光源」。b3 に型が無いので実体ごと載せる。
    /// @note レイアウトは PunctualShadowConstants.hlsli のコメントと FBZZ_PunctualAt が正本。
    const int shapedCount = std::clamp(ctx.legacyShapedLightCount, 0, kMaxLegacyShapedLights);
    data.legacyShapedLightCount = shapedCount;
    for (int i = 0; i < shapedCount; ++i) {
        const PunctualLightGPU& a = ctx.legacyShapedLights[i];
        math::Vector4* dst = &data.legacyShapedLight[i * kLegacyShapedLightStride];
        dst[0] = { a.position,  a.range };
        dst[1] = { a.color,     a.intensity };
        dst[2] = { a.direction, 0.0f };
        dst[3] = { a.tangent,   a.halfWidth };
        dst[4] = { a.bitangent, a.halfHeight };
        /// @note y は両面フラグ。PunctualLightGPU では outerCos の枠に載せてある。
        /// @note z は影のスロット番号。legacyPunctualSlots は b3 の 12 枠に紐付いた表なので、
        /// @note b3 に席の無い「大きさを持つ光源」はそこから引けない。空いている枠へ載せる。
        dst[5] = { static_cast<float>(a.type), a.outerCos,
                   static_cast<float>(a.shadowIndex), 0.0f };
    }

    /// @note レガシー経路のスロット番号と光源半径。「無し」は -1 (ctx 側の既定値がそう)。0 は「スロット 0」という有効な番号のため 0 埋めでは済ませられず、影を持たないライトが他のライトのシャドウマップを引いてしまう。
    for (int i = 0; i < kMaxLegacyPunctualLights; ++i) {
        data.legacyPunctualSlots[i] = {
            static_cast<float>(ctx.legacyShadowSlots[i]),
            static_cast<float>(ctx.legacyCookieSlots[i]),
            ctx.legacySourceRadius[i],
            0.0f
        };
    }

    return data;
}

void UpdatePunctualShadowConstants(RenderPassContext& ctx)
{
    if (!ctx.handles.punctualShadowCB.IsValid()) return;
    const auto data = MakePunctualShadowConstants(ctx);
    ctx.resources.Update(ctx.handles.punctualShadowCB, &data, sizeof(data));
}


math::Vector3 ComputeCameraFacingRibbonNormal(
    const math::Vector3& direction, const math::Vector3& cameraPos, const math::Vector3& point)
{
    /// @note 帯の面をカメラへ向けるには、幅方向を「進行方向 × 視線方向」に取る。
    math::Vector3 up = cameraPos - point;
    if (up.LengthSq() > math::EPSILON * math::EPSILON) up = up.Normalized();
    else up = math::Vector3::UP;
    /// @note 進行方向と視線がほぼ平行だと外積が退化して帯が消える。安定な軸へ逃がす。
    if (std::abs(math::Vector3::Dot(direction, up)) > 0.99f) up = math::Vector3::UP;
    if (std::abs(math::Vector3::Dot(direction, up)) > 0.99f) up = math::Vector3::RIGHT;

    const math::Vector3 normal = math::Vector3::Cross(direction, up);
    return normal.LengthSq() > math::EPSILON * math::EPSILON
        ? normal.Normalized() : math::Vector3::RIGHT;
}



}
