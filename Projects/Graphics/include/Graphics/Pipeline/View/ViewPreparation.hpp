/// @file    ViewPreparation.hpp
/// @brief   抽出済みの境界・照明から描画定数を作る Graphics の入口。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/Pipeline/Resources/RenderResources.hpp>
#include <functional>

namespace fbzz::renderer {
/// @note BindPassHandles 後に呼ぶ。資源の世代・寸法と必須の Raster 経路を確認し、view に保存する。
/// @note 未実装の Ray Scene / パス / RasterSurface は未準備。Scene の被覆対応を推測しない。
/// @return view.renderPlan と同じ構成。要求設定は変更しない。
[[nodiscard]] FBZZ_GRAPHICS_API ResolvedRenderPlan PrepareViewRenderPlan(ResourceManager& resources,
    IRenderer& renderer, const RenderSettings& settings, RenderViewResources& view,
    const RenderSharedResources& shared, const RenderPassHandles& handles,
    bool experimentalRayTracingEnabled = false);

struct ShadowBounds {
    math::Vector3 center = math::Vector3::ZERO; ///< 影の描画候補を包む球の中心（ワールド座標）。
    float radius = 0.0f; ///< 影の描画候補を包む球の半径（ワールド単位）。
    bool valid = false; ///< 描画候補から有効な境界球を算出できたか。
    uint64_t terrainRevision = 0; ///< 境界の計算に使用した地形の変更世代。
};
struct PreparedShadows {
    ShadowCascade cascades[kMaxShadowCascades]{}; ///< ビューから近い順に並ぶカスケードの投影と描画領域。
    int count = 0; ///< cascades の有効要素数。
    math::Matrix4 lightVP; ///< 光源から見たビュー射影行列。
    math::Matrix4 lightView; ///< ワールド座標を光源のビュー空間に移す行列。
    math::Vector3 lightPos; ///< 影の投影に用いる光源のワールド位置。
};
/// @param[in] bounds ワールド空間の描画候補を覆う球。半径は正であること。
FBZZ_GRAPHICS_API PreparedShadows PrepareShadows(const Camera& camera, const RenderSettings& settings,
    const ShadowBounds& bounds, const math::Vector3& lightDirection,
    float shadowDistance, float shadowBias);
/// @note キャッシュ対象の投影に再利用用の余白を加える。
FBZZ_GRAPHICS_API PreparedShadows PrepareShadows(const Camera& camera, const RenderSettings& settings,
    const ShadowBounds& bounds, const math::Vector3& lightDirection,
    float shadowDistance, float shadowBias, bool padCachedCascades);
/// @return 再利用するときは true。対応する投影も前回の深度とそろえる。
FBZZ_GRAPHICS_API bool PrepareShadowCascadeCache(ShadowCascadeCache& cache, PreparedShadows& shadows,
    const Camera& camera, const math::Vector3& lightDirection, const FarShadowCacheKey& key, uint64_t frame, int index);
struct AdvancedViewInput {
    bool dynamicIblReady = false; ///< ビューの照明で動的環境マップを参照できるか。
    float reflectionProbeIntensity = 1.0f; ///< 選択した反射プローブの照明への寄与を調整する倍率。
    int dynamicIblMipCount = 0; ///< 動的環境マップで参照できるミップレベル数。
    float screenAoStrength = 0.0f; ///< スクリーン空間の環境遮蔽を合成する強さ。
    float screenContactShadowStrength = 0.0f; ///< スクリーン空間の接触影を合成する強さ。
    float weatherWetness = 0.0f; ///< 天候による表面の濡れの度合い。
    float weatherDarkening = 0.0f; ///< 濡れた面の色を暗くする強さ。
    float weatherPuddle = 0.0f; ///< 水たまりの見え方に適用する天候の寄与。
    bool reflectionProbeSelected = false; ///< このビューに適用する反射プローブが選ばれたか。
};
/// @note Probe のアセット更新はホスト側で同期実行する。コールバックは保持しない。
FBZZ_GRAPHICS_API void PrepareAdvancedConstants(RenderPassContext& context, RenderViewResources& view,
    const AdvancedViewInput& input,
    const std::function<void(AdvancedGraphicsCB&)>& prepareProbes = {});

/// @pre Reflection の記録可否が確定した TAA 実行直前に呼ぶ。
/// @note provider 変更・非連続フレームでは現在の画像だけで履歴を再開する。画素別 RT motion は別契約。
/// @note 先行 Draw/Dispatch が確保した定数は IConstantBuffer の immutable submit snapshot により変更されない。
FBZZ_GRAPHICS_API void PrepareTaaProviderHistory(RenderPassContext& context, RenderViewResources& view);
}
