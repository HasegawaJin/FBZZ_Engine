/// @file    SkyCapturePass.cpp
/// @brief   空連動 IBL (環境システム設計 Phase A) の「①SkyCapture」パス。
/// @author  Hasegawa Jin
/// @date    2026-07-01

/// @note 既存スカイドームシェーダーを 6 面それぞれの view/projection で描き、空を低解像度
/// @note キューブマップ "SkyEnvCube" へ焼く。畳み込み (irradiance/prefilter) は後段の SkyLightBake が行う。

/// @note Skydome.hlsl は色を「オブジェクト空間のレイ方向 (= ドーム頂点)」から計算し、view/projection は
/// @note ドームを画面のどこに置くかにしか使わない。面ごとの view (回転) + 90° 射影を差し替えるだけで
/// @note 各面の大気へ SkyCloudCapture が同じワールド空間の雲を重ねる。
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include "Graphics/Renderer/DrawCall.hpp"
#include <Math/MathUtils.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>
#include <algorithm>
#include <chrono>

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
    /// @note 捕捉用シェーダーやノイズが欠けたら大気のみへ戻し、未束縛の Texture3D を読まない。
    const bool captureClouds = ctx.environment.cloudEnabled
        && resources.Get(h.skyCloudCaptureShader) && resources.Get(h.volumetricCloudPremultipliedPSO)
        && resources.Get(h.volumetricCloudCB) && resources.Get(h.cloudShapeTex) && resources.Get(h.cloudDetailTex);
    const math::Vector3 capturePosition = ctx.camera.m_position;

    /// @note 大気・照明・雲設定の変更と、間引いた雲時刻・捕捉位置だけを EnvironmentResources が消費する。
    if (ctx.environmentResources) {
        EnvironmentResources::SkySignature sig;
        sig.sunDirection     = ctx.lightData.lightDir.Normalized();
        sig.rayleigh         = sky->rayleighScattering;
        sig.mieScattering    = sky->mieScattering;
        sig.skyScatterIntensity = sky->skyScatterIntensity;
        sig.mieG             = sky->mieG;
        sig.planetRadius     = sky->planetRadius;
        sig.atmosphereRadius = sky->atmosphereRadius;
        sig.lightColor = ctx.lightData.lightColor;
        sig.ambientColor = ctx.lightData.ambientColor;
        sig.skyDimmer = ctx.lightData.skyDimmer;
        sig.shaderVersion = resources.GetShaderVersion();
        sig.cloudEnabled = captureClouds;
        sig.cloud = ctx.environment.cloud;
        sig.capturePosition = capturePosition;
        /// @note 小さな間隔を float で保てるよう、steady_clock の絶対 epoch ではなく起点からの秒を使う。
        static const auto clockOrigin = std::chrono::steady_clock::now();
        const float captureTime = std::chrono::duration<float>(std::chrono::steady_clock::now() - clockOrigin).count();
        if (!ctx.environmentResources->ConsumeDirty(sig, captureTime, ctx.frameStamp))
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
    if (captureClouds)
        resources.Update(h.volumetricCloudCB, &ctx.environment.cloud, sizeof(ctx.environment.cloud));

    /// @note 雲の視線復元も行うため、90°・アスペクト 1 の Reversed-Z 射影と雲の最大距離を使う。
    const float captureFar = captureClouds ? (std::max)(ctx.environment.cloud.cloudNoise.w, 10.0f) : 10.0f;
    const math::Matrix4 proj = math::Matrix4::PerspectiveReversedZ(math::ToRad(90.0f), 1.0f, 0.1f, captureFar);

    for (uint32_t face = 0; face < 6; ++face) {
        /// @note 実カメラの雲柱を捕捉する。大気ドームは回転だけを読み、雲は位置も読む。
        const math::Matrix4 faceView = math::Matrix4::LookAt(
            capturePosition, capturePosition + kCubeFaces[face].forward, kCubeFaces[face].up);

        PerFrameCB faceFrame{};
        faceFrame.view              = faceView;
        faceFrame.projection        = proj;
        /// @note GetViewProjection と同じ proj*view 規約
        faceFrame.viewProjection    = proj * faceView;
        faceFrame.invViewProjection = math::Matrix4::Inverse(faceFrame.viewProjection);
        faceFrame.cameraPos         = capturePosition;
        faceFrame.nearZ             = 0.1f;
        faceFrame.farZ              = captureFar;
        resources.Update(h.skyCaptureFrameCB, &faceFrame, sizeof(PerFrameCB));

        /// @note 当該面 (mip0) を描画先にバインドし、スカイドームを 1 回描く。
        renderer.SetRenderTargetFace(h.skyEnvCubeRT, face, 0, resources);
        renderer.ClearDepth();

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
        if (captureClouds) {
            renderer::DrawCall cloudDraw;
            cloudDraw.shader = h.skyCloudCaptureShader;
            cloudDraw.pipelineState = h.volumetricCloudPremultipliedPSO;
            cloudDraw.vertexCount = 3;
            cloudDraw.constantBuffers[0] = h.skyCaptureFrameCB;
            cloudDraw.constantBuffers[2] = h.volumetricCloudCB;
            cloudDraw.constantBuffers[3] = h.lightCB;
            cloudDraw.textures[26] = h.cloudShapeTex;
            cloudDraw.textures[27] = h.cloudDetailTex;
            renderer.Submit(cloudDraw, resources);
        }
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
