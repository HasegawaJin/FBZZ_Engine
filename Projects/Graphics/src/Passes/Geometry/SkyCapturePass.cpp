/// @file    SkyCapturePass.cpp
/// @brief   空連動 IBL (環境システム設計 Phase A) の「①SkyCapture」パス。
/// @author  Hasegawa Jin
/// @date    2026-07-01

/// @note 既存スカイドームシェーダーを 6 面それぞれの view/projection で描き、空を低解像度
/// @note キューブマップ "SkyEnvCube" へ焼く。畳み込み (irradiance/prefilter) は後段の SkyLightBake が行う。

/// @note Skydome.hlsl は色を「オブジェクト空間のレイ方向 (= ドーム頂点)」から計算し、view/projection は
/// @note ドームを画面のどこに置くかにしか使わない。面ごとの view (回転) + 90° 射影を差し替えるだけで
/// @note 各面に正しい方向の空が描け、新規 HLSL は不要になる。
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include "Graphics/Renderer/DrawCall.hpp"
#include <Math/MathUtils.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>

namespace fbzz::renderer {

namespace {

/// @note D3D11 キューブマップ 6 面のカメラ基準 (forward, up)。
/// @note 順序は D3D11_TEXTURECUBE_FACE (+X,-X,+Y,-Y,+Z,-Z) / DX11RenderTarget::InitCubemap の面順に一致させること。
struct CubeFaceBasis { math::Vector3 forward; math::Vector3 up; };
constexpr CubeFaceBasis kCubeFaces[6] = {
    /// @note +X
    {{ 1.0f,  0.0f,  0.0f}, { 0.0f, 1.0f,  0.0f}},
    /// @note -X
    {{-1.0f,  0.0f,  0.0f}, { 0.0f, 1.0f,  0.0f}},
    /// @note +Y
    {{ 0.0f,  1.0f,  0.0f}, { 0.0f, 0.0f, -1.0f}},
    /// @note -Y
    {{ 0.0f, -1.0f,  0.0f}, { 0.0f, 0.0f,  1.0f}},
    /// @note +Z
    {{ 0.0f,  0.0f,  1.0f}, { 0.0f, 1.0f,  0.0f}},
    /// @note -Z
    {{ 0.0f,  0.0f, -1.0f}, { 0.0f, 1.0f,  0.0f}},
};

} /// @note namespace

void ExecuteSkyCapturePass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    if (!h.skyShader.IsValid() || !h.skyVB.IsValid() || !h.skyIB.IsValid()) return;
    if (!h.skyEnvCubeRT.IsValid() || !h.skyCaptureFrameCB.IsValid())        return;

    /// @note 有効な SkyRenderer を 1 つ探す (SkyPass と同じ先着優先)。
    const auto* sky = ctx.environment.sky ? &*ctx.environment.sky : nullptr;
    if (!sky) return;

    /// @note dirty 判定: 太陽 (= ディレクショナルライト) 方向と大気パラメータが前回と同じなら
    /// @note 再ベイクしない。キューブマップ生成は毎フレーム行うと無駄なコストなので
    /// @note EnvironmentResources がキャッシュする。
    if (ctx.environmentResources) {
        EnvironmentResources::SkySignature sig;
        sig.sunDirection     = ctx.lightData.lightDir.Normalized();
        sig.rayleigh         = sky->rayleighScattering;
        sig.mieScattering    = sky->mieScattering;
        sig.skyScatterIntensity = sky->skyScatterIntensity;
        sig.mieG             = sky->mieG;
        sig.planetRadius     = sky->planetRadius;
        sig.atmosphereRadius = sky->atmosphereRadius;
        if (!ctx.environmentResources->ConsumeDirty(sig, ctx.time))
            /// @note 変化なし / 間引き中 → キャッシュ済みキューブを再利用
            return;
    }

    /// @note ライト CB (b3) を更新する。SkyCapture はグラフ実行前に走るため、ForwardPasses が
    /// @note lightCB を更新するより前になる。Skydome は b3 の太陽方向/色/強度を読むため、
    /// @note ここで ctx.lightData から確定させておく。
    resources.Update(h.lightCB, &ctx.lightData, sizeof(ctx.lightData));

    /// @note 大気 / ポスト CB を SkyPass と同じ値で更新する (b5 / b6)。
    PostProcCB skyPostData{};
    skyPostData.exposure = 1.0f;
    skyPostData.time     = ctx.time;
    resources.Update(h.postprocCB, &skyPostData, sizeof(PostProcCB));

    AtmosphereCB atmData{};
    atmData.rayleighScattering[0] = sky->rayleighScattering.x;
    atmData.rayleighScattering[1] = sky->rayleighScattering.y;
    atmData.rayleighScattering[2] = sky->rayleighScattering.z;
    atmData.mieScattering         = sky->mieScattering;
    atmData.planetRadius          = sky->planetRadius;
    atmData.atmosphereRadius      = sky->atmosphereRadius;
    atmData.sunIntensity          = sky->skyScatterIntensity;
    atmData.mieG                  = sky->mieG;
    resources.Update(h.atmosphereCB, &atmData, sizeof(AtmosphereCB));

    /// @note 90° FOV・アスペクト 1.0 の射影。Skydome.hlsl は svPosition の z を 0 (Reversed-Z の最遠) で上書きし、面は深度を持たないため
    /// @note near/far は xy に影響しない (値は形式的)。
    const math::Matrix4 proj = math::Matrix4::Perspective(math::ToRad(90.0f), 1.0f, 0.1f, 10.0f);

    for (uint32_t face = 0; face < 6; ++face) {
        /// @note 面ごとの view。カメラは原点固定 (Skydome は (float3x3)view = 回転のみ使用)。
        /// @note ForwardPasses と同じ規約で view/projection/viewProjection を格納する (CB レイアウト一致)。
        const math::Matrix4 faceView = math::Matrix4::LookAt(
            math::Vector3::ZERO, kCubeFaces[face].forward, kCubeFaces[face].up);

        PerFrameCB faceFrame{};
        faceFrame.view              = faceView;
        faceFrame.projection        = proj;
        /// @note GetViewProjection と同じ proj*view 規約
        faceFrame.viewProjection    = proj * faceView;
        faceFrame.invViewProjection = math::Matrix4::Inverse(faceFrame.viewProjection);
        faceFrame.cameraPos         = math::Vector3::ZERO;
        faceFrame.nearZ             = 0.1f;
        faceFrame.farZ              = 10.0f;
        resources.Update(h.skyCaptureFrameCB, &faceFrame, sizeof(PerFrameCB));

        /// @note 当該面 (mip0) を描画先にバインドし、スカイドームを 1 回描く。
        renderer.SetRenderTargetFace(h.skyEnvCubeRT, face, 0, resources);

        renderer::DrawCall dc;
        dc.vertexBuffer       = h.skyVB;
        dc.indexBuffer        = h.skyIB;
        dc.indexCount         = h.skyIndexCount;
        dc.shader             = h.skyShader;
        dc.pipelineState      = h.skyPSO;
        /// @note b0: 面ごとの view/projection
        dc.constantBuffers[0] = h.skyCaptureFrameCB;
        /// @note b3: 太陽方向・色・強度
        dc.constantBuffers[3] = h.lightCB;
        /// @note b5: time
        dc.constantBuffers[5] = h.postprocCB;
        /// @note b6: 散乱パラメータ
        dc.constantBuffers[6] = h.atmosphereCB;
        renderer.Submit(dc, resources);
    }

    /// @note キューブ面の RTV を OM から外す。直後の SkyLightBake がこのキューブを SRV (t0) として
    /// @note 読むため、RTV と SRV の同時バインドによる DX11 ハザードを避ける。空ハンドルでバック
    /// @note バッファへ戻し、PS/CS SRV もここで解除される。
    renderer.SetRenderTarget({}, resources);

    /// @note 焼いたキューブを EnvironmentResources へ公開し、後段 SkyLightBake に畳み込みを要求する。
    if (ctx.environmentResources) {
        ctx.environmentResources->skyEnvCube       = resources.GetCubemapTexture(h.skyEnvCubeRT);
        ctx.environmentResources->needsConvolution = true;
    }
}

} /// @note namespace fbzz::renderer
