/// @file    ViewPreparation.cpp
/// @brief   カスケードのフィッティングとビュー別 GPU 定数の組み立て。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <Graphics/Pipeline/ViewPreparation.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::renderer {
namespace {
/// @note 視錐台スライス [nearZ, farZ] の外接球。centerDistance はカメラ前方への距離。
/// @note 8 頂点へ合わせるとカメラの回転で箱の大きさが変わり、影の精細度が脈動する。
/// @note 外接球の半径は向きに依存しない。
struct FrustumSliceSphere {
    float centerDistance = 0.0f;
    float radius         = 0.0f;
};

FrustumSliceSphere ComputeFrustumSliceSphere(const renderer::Camera& camera,
                                             float nearZ, float farZ)
{
    constexpr float DEG_TO_RAD = 0.01745329251994329577f;
    nearZ = (std::max)(nearZ, 0.01f);
    farZ  = (std::max)(farZ, nearZ + 0.01f);

    /// @note 平行投影は錐台ではなく直方体なので、対角の傾きという概念が無い。
    /// @note スライスの外接球は「中央 + 半対角」でそのまま求まる。
    if (camera.m_projection == renderer::ProjectionMode::Orthographic) {
        const float halfH = (std::max)(camera.m_orthoHeight, 0.01f) * 0.5f;
        const float halfW = halfH * camera.m_aspect;
        const float halfD = (farZ - nearZ) * 0.5f;
        FrustumSliceSphere box;
        box.centerDistance = (nearZ + farZ) * 0.5f;
        box.radius = std::sqrt(halfW * halfW + halfH * halfH + halfD * halfD);
        return box;
    }

    /// @note 視錐台の対角方向の傾き。k = |(±aspect*t, ±t, 1)| の xy 成分の長さ。
    const float tanHalfFov = std::tan(camera.m_fovY * 0.5f * DEG_TO_RAD);
    const float k  = tanHalfFov * std::sqrt(1.0f + camera.m_aspect * camera.m_aspect);
    const float k2 = k * k;

    FrustumSliceSphere sphere;
    /// @note near 面が far 面より広いほど中心は手前へ寄る。k² が十分大きいときは
    /// @note far 面の外接円がスライス全体を包むので、中心は far 面上に載る。
    if (k2 >= (farZ - nearZ) / (farZ + nearZ)) {
        sphere.centerDistance = farZ;
        sphere.radius         = farZ * k;
        return sphere;
    }

    const float sum  = farZ + nearZ;
    const float diff = farZ - nearZ;
    sphere.centerDistance = 0.5f * sum * (1.0f + k2);
    sphere.radius = 0.5f * std::sqrt(diff * diff
                                   + 2.0f * (farZ * farZ + nearZ * nearZ) * k2
                                   + sum * sum * k2 * k2);
    return sphere;
}

/// @note practical split scheme。対数分割 (手前を細かく) と等分割 (遠方を細かく) を lambda で補間する。
/// @note 対数だけだと遠景の影が溶け、等分だけだと足元が粗くなる。
/// @note outSplits[i] は「カスケード i が担当する far 距離」。outSplits[count-1] == shadowDistance。
void ComputeCascadeSplits(float nearZ, float shadowDistance, int cascadeCount,
                          float lambda, float* outSplits)
{
    const float clampedLambda = std::clamp(lambda, 0.0f, 1.0f);
    const float range         = shadowDistance - nearZ;

    for (int i = 0; i < cascadeCount; ++i) {
        const float ratio = static_cast<float>(i + 1) / static_cast<float>(cascadeCount);
        const float logSplit     = nearZ * std::pow(shadowDistance / nearZ, ratio);
        const float uniformSplit = nearZ + range * ratio;
        outSplits[i] = clampedLambda * logSplit + (1.0f - clampedLambda) * uniformSplit;
    }
    /// @note 丸め誤差で最遠が shadowDistance を下回ると、影の到達距離が設定より短くなる。
    outSplits[cascadeCount - 1] = shadowDistance;
}
}
PreparedShadows PrepareShadows(const Camera& camera, const RenderSettings& rs,
    const ShadowBounds& shadowBounds, const math::Vector3& lightDir,
    float dirShadowDistance, float dirShadowBias)
{
    PreparedShadows result;
    /// @name カスケードシャドウ (CSM) のフィッティング
    /// @note 精細さを決めるのは解像度ではなく「1 テクセルが覆うワールド距離」。単一マップでは
    /// @note 到達距離を伸ばすと分母が伸びるだけで、近距離の精細さと両立しない。
    /// @note 視錐台を距離で区切り、手前ほど狭い範囲へ 1 タイルを割り当てて足元の密度だけ上げる。
    /// @note shadowBounds (シーン全体) は「これ以上大きくしない」上限としてだけ使う。
    auto& cascadeCount = result.count;
    cascadeCount =
        std::clamp(rs.shadow.cascadeCount, 1, fbzz::renderer::kMaxShadowCascades);

    /// @note アトラス配置: 1 分割なら全面、2 分割以上なら 2x2 タイル。
    /// @note 1 枚に収めれば影を読む 20 以上のシェーダーがバインドもサンプラーも変えずに済む。
    /// @note 解像度とメモリは分割数によらず一定で、変わるのは面積の配分だけ。
    const uint32_t atlasResolution = (std::max)(rs.shadow.mapResolution, 1u);
    const uint32_t tilesPerSide    = (cascadeCount > 1) ? 2u : 1u;
    const uint32_t tileSize        = (std::max)(atlasResolution / tilesPerSide, 1u);

    /// @note 影の最大到達距離。LightComponent::shadowDistance > 0 は従来どおり手動指定を優先する。
    const float shadowDistance = (dirShadowDistance > 0.0f)
        ? (std::max)(dirShadowDistance, 1.0f)
        : (std::max)(rs.shadow.autoFitDistance, 1.0f);

    float cascadeSplits[fbzz::renderer::kMaxShadowCascades] = {};
    ComputeCascadeSplits((std::max)(camera.m_near, 0.01f), shadowDistance,
                         cascadeCount, rs.shadow.cascadeSplitLambda, cascadeSplits);

    const math::Vector3 up = (std::abs(lightDir.y) > 0.99f)
                             ? math::Vector3{ 1.0f, 0.0f, 0.0f }
                             : math::Vector3{ 0.0f, 1.0f, 0.0f };
    /// @note 位置を持たない回転だけのライト空間。テクセルスナップの量子化格子として使う。
    const math::Matrix4 snapView     = math::Matrix4::LookAt(math::Vector3::ZERO, lightDir, up);
    const math::Matrix4 snapViewInv  = math::Matrix4::Inverse(snapView);

    auto& cascades = result.cascades;
    float cascadeSliceNear = (std::max)(camera.m_near, 0.01f);

    for (int i = 0; i < cascadeCount; ++i) {
        const float sliceFar = cascadeSplits[i];

        /// @note このカスケードが担当する視錐台スライスの外接球 (向きに依存しないので回転で脈動しない)。
        const FrustumSliceSphere slice =
            ComputeFrustumSliceSphere(camera, cascadeSliceNear, sliceFar);

        /// @note シーン全体より大きい影ボリュームを作っても無駄なテクセルが増えるだけ。
        float radius = (std::max)((std::min)(slice.radius, shadowBounds.radius), 1.0f);

        math::Vector3 center = camera.m_position + camera.GetForward() * slice.centerDistance;
        if (radius < shadowBounds.radius) {
            /// @note シーン球からはみ出さないよう、中心をシーン球内へ引き戻す。
            const math::Vector3 offset   = center - shadowBounds.center;
            const float         distance = offset.Length();
            const float         limit    = (std::max)(shadowBounds.radius - radius, 0.0f);
            if (distance > limit && distance > 0.0001f)
                center = shadowBounds.center + offset * (limit / distance);
        } else {
            center = shadowBounds.center;
        }

        /// @note テクセルスナップ。中心がカメラに追従するとサブテクセルのずれで輪郭が波打つ
        /// @note (shadow swimming)。ライト空間で 1 テクセル単位へ量子化すると標本位置が固定される。
        const float texelWorldSize = (radius * 2.0f) / static_cast<float>(tileSize);
        {
            math::Vector4 lightSpace =
                snapView * math::Vector4{ center.x, center.y, center.z, 1.0f };
            lightSpace.x = std::floor(lightSpace.x / texelWorldSize) * texelWorldSize;
            lightSpace.y = std::floor(lightSpace.y / texelWorldSize) * texelWorldSize;
            const math::Vector4 snapped = snapViewInv * lightSpace;
            center = { snapped.x, snapped.y, snapped.z };
        }

        /// @note 深度レンジ。ボリュームの外にいる背の高い caster も影を落とせるよう、
        /// @note ライト方向の引きはシーン全体の広がりから取る (near/far を広げても塗る面積は増えない)。
        const float pullback   = (std::max)(shadowBounds.radius, radius) + 20.0f;
        const float depthRange = pullback + radius + 20.0f;

        const math::Vector3 eye  = center - lightDir * pullback;
        const math::Matrix4 view = math::Matrix4::LookAt(eye, center, up);
        const math::Matrix4 proj = math::Matrix4::Orthographic(-radius, radius,
                                                               -radius, radius,
                                                               1.0f, depthRange);

        ShadowCascade& cascade = cascades[i];
        cascade.viewProjection = proj * view;
        cascade.view           = view;
        cascade.eyePos         = eye;
        cascade.frustum        = math::Frustum::FromViewProjection(cascade.viewProjection);
        cascade.texelWorldSize = texelWorldSize;
        /// @note ワールド空間で約 5mm 相当の一定バイアスになるよう深度レンジで正規化する。
        /// @note カスケードごとにレンジが違うので、値もカスケードごとに持つ。
        cascade.biasNDC = (0.005f * dirShadowBias) / (std::max)(depthRange - 1.0f, 1.0f);

        /// @note アトラス内のタイル位置 (2x2 を左上から Z 字順に埋める)。
        const uint32_t tileX = static_cast<uint32_t>(i) % tilesPerSide;
        const uint32_t tileY = static_cast<uint32_t>(i) / tilesPerSide;
        cascade.viewportX    = tileX * tileSize;
        cascade.viewportY    = tileY * tileSize;
        cascade.viewportSize = tileSize;

        const float uvScale = static_cast<float>(tileSize) / static_cast<float>(atlasResolution);
        cascade.atlasRect = {
            static_cast<float>(cascade.viewportX) / static_cast<float>(atlasResolution),
            static_cast<float>(cascade.viewportY) / static_cast<float>(atlasResolution),
            uvScale, uvScale
        };

        cascadeSliceNear = sliceFar;
    }

    /// @note 単一のライト行列で足りるパス (パーティクル自己影など) 向けの代表値。
    /// @note 到達範囲を最も広く覆う最遠カスケードを渡す。ビルボードの正対に view の内訳が要るので、
    /// @note view と viewProjection は必ず同じカスケードから対で渡す。
    const ShadowCascade& widestCascade = cascades[cascadeCount - 1];
    result.lightVP = widestCascade.viewProjection;
    result.lightView = widestCascade.view;
    result.lightPos = widestCascade.eyePos;
    return result;
}

void PrepareAdvancedConstants(RenderPassContext& passCtx, RenderViewResources& viewTargets,
    const AdvancedViewInput& input, const std::function<void(AdvancedGraphicsCB&)>& prepareProbes)
{
    const auto& rs = passCtx.settings;
    const auto& camera = passCtx.camera;
    auto& resources = passCtx.resources;
    auto& passHandles = passCtx.handles;
    const auto sHdrW = viewTargets.width;
    const auto sHdrH = viewTargets.height;
    constexpr float kHalfResScale = 0.5f;
    /// @name AdvancedGraphicsCB (b8) を毎フレーム更新
    /// @note 各パスはここで書いたデータを読むだけなので、更新はこの 1 か所に集中させる。
    if (viewTargets.advancedGraphicsCB.IsValid()) {
        AdvancedGraphicsCB agData{};
        /// @note 未バインド SRV をサンプルさせず、確実に ambient へフォールバックさせるための判定。
        /// @note 動的 IBL は .dds を持たないので input.dynamicIblReady を別経路として許可する。
        const bool iblResourcesReady =
            (input.dynamicIblReady || (rs.ibl.enabled && rs.HasValidIblAssets())) &&
            passHandles.iblIrradiance.IsValid() && passHandles.iblPrefilter.IsValid();
        agData.iblIntensity          = iblResourcesReady ? rs.ibl.intensity * input.reflectionProbeIntensity : 0.0f;
        agData.iblDiffuseScale       = rs.ibl.diffuseScale;
        agData.iblSpecularScale      = rs.ibl.specularScale;
        /// @note 動的 IBL は SkyLightBake が焼いた prefilter mip 数に合わせる (maxMip = mip 数 - 1)。
        agData.iblMaxMipLevel        = input.dynamicIblReady
            ? input.dynamicIblMipCount - 1
            : rs.ibl.maxMipLevel;
        agData.ssrMaxDistance        = rs.ssr.maxDistance;
        agData.ssrThickness          = rs.ssr.thickness;
        agData.ssrSteps              = rs.ssr.steps;
        agData.ssrIntensity          = rs.ssr.enabled ? rs.ssr.intensity : 0.0f;
        agData.volLightIntensity     = rs.volumetricLight.enabled ? rs.volumetricLight.intensity : 0.0f;
        agData.volScattering         = rs.volumetricLight.scattering;
        agData.volSteps              = rs.volumetricLight.steps;
        agData.volMaxDist            = rs.volumetricLight.maxDist;
        agData.volMinDist            = rs.volumetricLight.minDist;
        agData.volDensity            = rs.volumetricLight.density;
        agData.volHeightFalloff      = rs.volumetricLight.heightFalloff;
        agData.volHeightStart        = rs.volumetricLight.heightStart;
        agData.volTintR              = rs.volumetricLight.tint[0];
        agData.volTintG              = rs.volumetricLight.tint[1];
        agData.volTintB              = rs.volumetricLight.tint[2];
        agData.volEdgeFade           = rs.volumetricLight.edgeFade;

    /// @note 前方描画のマテリアルが画面空間 AO / 接触影をどれだけ受けるか。
    /// @note 値そのものは上の「前方描画のマテリアルへ渡す画面空間の遮蔽」ブロックで決めてある。
    agData.screenAoStrength            = input.screenAoStrength;
    agData.screenContactShadowStrength = input.screenContactShadowStrength;
    agData.screenAoScale               = kHalfResScale;
    agData.screenContactShadowScale    = kHalfResScale;

    /// @note 自動露出。key <= 0 が「無効」の印なので、切ってあるときは 0 のまま渡す。
    /// @note 0.18 は反射率 18% のグレーカード = 写真の露出計が基準にしている明るさ。
    /// @note 結果バッファが無いと Composite は t29 を束縛しない。key を立てたままだと 0 を平均輝度として読み、
    /// @note 露出が上限へ張り付いて白飛びするので、同じ条件で無効にする (CompositePass の束縛条件と対)。
    agData.autoExposureKey          = (rs.autoExposure.enabled && viewTargets.exposureResult.IsValid()) ? 0.18f : 0.0f;
    agData.autoExposureCompensation = rs.autoExposure.compensation;
    agData.autoExposureMinEV        = rs.autoExposure.minExposureEV;
    agData.autoExposureMaxEV        = (std::max)(rs.autoExposure.maxExposureEV,
                                                 rs.autoExposure.minExposureEV);
        agData.taaFeedback           = viewTargets.taaHistoryValid ? rs.taa.feedback : 0.0f;
        agData.taaJitterX            = passCtx.taaJitterNdcX;
        agData.taaJitterY            = passCtx.taaJitterNdcY;
        agData.motionBlurStrength    = rs.motionBlur.enabled ? rs.motionBlur.strength : 0.0f;
        agData.motionBlurSamples     = rs.motionBlur.samples;
        agData.screenWidth           = static_cast<float>(sHdrW);
        agData.screenHeight          = static_cast<float>(sHdrH);
        agData.gtaoIntensity         = rs.IsGtaoActive()   ? rs.gtao.intensity : 0.0f;
        agData.gtaoRadius            = rs.gtao.radius;
        agData.gtaoSlices            = rs.gtao.slices;
        agData.gtaoStepsPerSlice     = rs.gtao.stepsPerSlice;
        agData.contactShadowStrength = rs.contactShadow.enabled ? rs.contactShadow.strength  : 0.0f;
        agData.contactShadowRayLen   = rs.contactShadow.rayLength;
        agData.contactShadowSteps    = rs.contactShadow.steps;
        agData.contactShadowThick    = rs.contactShadow.thickness;
        agData.lensFlareIntensity    = rs.lensFlare.enabled ? rs.lensFlare.intensity : 0.0f;
        agData.lensFlareGhostCount   = rs.lensFlare.ghostCount;
        agData.lensFlareHaloWidth    = rs.lensFlare.haloWidth;
        agData.lensFlareDistort      = rs.lensFlare.distortion;
        agData.pcssLightRadius       = rs.shadow.pcssLightRadius;
        agData.pcssEnabled           = rs.shadow.pcssEnabled ? 1 : 0;
        agData.lutBlend              = (rs.lutColorGrading.enabled && resources.Get(passHandles.proceduralColorLut) != nullptr)
            ? rs.lutColorGrading.blend : 0.0f;
        agData.weatherWetness = input.weatherWetness;
        agData.weatherDarkening = input.weatherDarkening;
        agData.weatherPuddle = input.weatherPuddle;
        /// @note 前フレームの VP 行列 — TAA / Motion Blur が深度再投影で使う。ビュー別に持つ。
        agData.prevViewProjection    = viewTargets.prevViewProjection;
        agData.invPrevViewProjection = viewTargets.invPrevViewProjection;
        /// @note Light Probe Volume: 焼きを進め、焼き上がったボリュームの拡散 GI を IBL キューブに差し替える。
        /// @note 拡散 GI は IBL の拡散項の置き換えなので、IBL が引けないフレームでは効かせない (マテリアルが IBL 分岐に入らない)。
        if (iblResourcesReady && prepareProbes) prepareProbes(agData);
        resources.Update(viewTargets.advancedGraphicsCB, &agData, sizeof(AdvancedGraphicsCB));
        /// @note ここはジッターを載せない。両方に載せるとジッター差分がそのまま「動き」として
        /// @note 現れ、履歴が毎フレームずれて収束しない。履歴はピクセル中心で収束した絵なので、
        /// @note 引く座標もピクセル中心でなければならない。
        /// @note シェーダーが今フレームの深度 (Reversed-Z) と並べて使うので GPU 用の行列で持つ。
        viewTargets.prevViewProjection    = camera.GetGpuViewProjection();
        viewTargets.invPrevViewProjection = math::Matrix4::Inverse(viewTargets.prevViewProjection);
    }
}
}
