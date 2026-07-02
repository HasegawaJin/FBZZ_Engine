// FBZZ Engine
// RenderPasses/SkyCapturePass.cpp | fbzz::scene
// 空連動 IBL (環境システム設計 Phase A) の「①SkyCapture」パス。
// 既存スカイドームシェーダーを 6 面それぞれの view/projection で描き、空を低解像度
// キューブマップ "SkyEnvCube" へ焼く。畳み込み (irradiance/prefilter) は後段の SkyLightBake が行う。
//
// WHY (新規 HLSL 不要):
//   Skydome.hlsl は色を「オブジェクト空間のレイ方向 (= ドーム頂点)」から計算し、view/projection は
//   ドームを画面のどこに置くかにしか使わない。そのため面ごとの view(回転)+90°射影を差し替えるだけで、
//   各面に正しい方向の空が描ける。スカイシェーダーには一切手を入れない。
#include "GeometryPasses.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Components/SkyRenderer.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Core/Time.hpp"
#include <Math/MathUtils.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>

namespace fbzz::scene {

namespace {

// D3D11 キューブマップ 6 面のカメラ基準 (forward, up)。
// 順序は D3D11_TEXTURECUBE_FACE (+X,-X,+Y,-Y,+Z,-Z) / DX11RenderTarget::InitCubemap の面順に一致させること。
struct CubeFaceBasis { math::Vector3 forward; math::Vector3 up; };
constexpr CubeFaceBasis kCubeFaces[6] = {
    {{ 1.0f,  0.0f,  0.0f}, { 0.0f, 1.0f,  0.0f}}, // +X
    {{-1.0f,  0.0f,  0.0f}, { 0.0f, 1.0f,  0.0f}}, // -X
    {{ 0.0f,  1.0f,  0.0f}, { 0.0f, 0.0f, -1.0f}}, // +Y
    {{ 0.0f, -1.0f,  0.0f}, { 0.0f, 0.0f,  1.0f}}, // -Y
    {{ 0.0f,  0.0f,  1.0f}, { 0.0f, 1.0f,  0.0f}}, // +Z
    {{ 0.0f,  0.0f, -1.0f}, { 0.0f, 1.0f,  0.0f}}, // -Z
};

} // namespace

void ExecuteSkyCapturePass(RenderPassContext& ctx)
{
    auto& renderer  = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    if (!h.skyShader.IsValid() || !h.skyVB.IsValid() || !h.skyIB.IsValid()) return;
    if (!h.skyEnvCubeRT.IsValid() || !h.skyCaptureFrameCB.IsValid())        return;

    // 有効な SkyRenderer を 1 つ探す (SkyPass と同じ先着優先)。
    SkyRenderer* sky = nullptr;
    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        if (auto* s = go.GetComponent<SkyRenderer>(); s && s->enabled) { sky = s; break; }
    }
    if (!sky) return;

    // dirty 判定: 太陽 (= ディレクショナルライト) 方向と大気パラメータが前回と同じなら再ベイクしない。
    // WHY: キューブマップ生成は毎フレーム行うと無駄なコスト。EnvironmentResources がキャッシュする。
    if (ctx.environmentResources) {
        EnvironmentResources::SkySignature sig;
        sig.sunDirection     = ctx.lightData.lightDir.Normalized();
        sig.rayleigh         = sky->rayleighScattering;
        sig.mieScattering    = sky->mieScattering;
        sig.sunIntensity     = sky->sunIntensity;
        sig.mieG             = sky->mieG;
        sig.planetRadius     = sky->planetRadius;
        sig.atmosphereRadius = sky->atmosphereRadius;
        if (!ctx.environmentResources->ConsumeDirty(sig, Time::time))
            return; // 変化なし / 間引き中 → キャッシュ済みキューブを再利用
    }

    // ライト CB (b3) を更新する。
    // WHY: SkyCapture はグラフ実行前に走るため、ForwardPasses が lightCB を更新するより前。
    //      Skydome は b3 の太陽方向/色/強度を読むので、ここで ctx.lightData から確定させておく。
    resources.Update(h.lightCB, &ctx.lightData, sizeof(ctx.lightData));

    // 大気 / ポスト CB を SkyPass と同じ値で更新する (b5 / b6)。
    PostProcCB skyPostData{};
    skyPostData.exposure = 1.0f;
    skyPostData.time     = Time::time;
    resources.Update(h.postprocCB, &skyPostData, sizeof(PostProcCB));

    AtmosphereCB atmData{};
    atmData.rayleighScattering[0] = sky->rayleighScattering.x;
    atmData.rayleighScattering[1] = sky->rayleighScattering.y;
    atmData.rayleighScattering[2] = sky->rayleighScattering.z;
    atmData.mieScattering         = sky->mieScattering;
    atmData.planetRadius          = sky->planetRadius;
    atmData.atmosphereRadius      = sky->atmosphereRadius;
    atmData.sunIntensity          = sky->sunIntensity;
    atmData.mieG                  = sky->mieG;
    resources.Update(h.atmosphereCB, &atmData, sizeof(AtmosphereCB));

    // 90° FOV・アスペクト 1.0 の射影。Skydome.hlsl は svPosition.xyww で z を上書きするため
    // near/far は xy に影響しない (値は形式的)。
    const math::Matrix4 proj = math::Matrix4::Perspective(math::ToRad(90.0f), 1.0f, 0.1f, 10.0f);

    for (uint32_t face = 0; face < 6; ++face) {
        // 面ごとの view。カメラは原点固定 (Skydome は (float3x3)view = 回転のみ使用)。
        // ForwardPasses と同じ規約で view/projection/viewProjection を格納する (CB レイアウト一致)。
        const math::Matrix4 faceView = math::Matrix4::LookAt(
            math::Vector3::ZERO, kCubeFaces[face].forward, kCubeFaces[face].up);

        PerFrameCB faceFrame{};
        faceFrame.view              = faceView;
        faceFrame.projection        = proj;
        faceFrame.viewProjection    = proj * faceView;                       // GetViewProjection と同じ proj*view 規約
        faceFrame.invViewProjection = math::Matrix4::Inverse(faceFrame.viewProjection);
        faceFrame.cameraPos         = math::Vector3::ZERO;
        faceFrame.nearZ             = 0.1f;
        faceFrame.farZ              = 10.0f;
        resources.Update(h.skyCaptureFrameCB, &faceFrame, sizeof(PerFrameCB));

        // 当該面 (mip0) を描画先にバインドし、スカイドームを 1 回描く。
        renderer.SetRenderTargetFace(h.skyEnvCubeRT, face, 0, resources);

        renderer::DrawCall dc;
        dc.vertexBuffer       = h.skyVB;
        dc.indexBuffer        = h.skyIB;
        dc.indexCount         = h.skyIndexCount;
        dc.shader             = h.skyShader;
        dc.pipelineState      = h.skyPSO;
        dc.constantBuffers[0] = h.skyCaptureFrameCB; // b0: 面ごとの view/projection
        dc.constantBuffers[3] = h.lightCB;           // b3: 太陽方向・色・強度
        dc.constantBuffers[5] = h.postprocCB;        // b5: time
        dc.constantBuffers[6] = h.atmosphereCB;      // b6: 散乱パラメータ
        renderer.Submit(dc, resources);
    }

    // キューブ面の RTV を OM から外す。
    // WHY: 直後の SkyLightBake はこのキューブを SRV (t0) として読むため、RTV と SRV の同時バインドによる
    //      DX11 HAZARD を避ける。空ハンドルでバックバッファへ戻し、PS/CS SRV もここで解除される。
    renderer.SetRenderTarget({}, resources);

    // 焼いたキューブを EnvironmentResources へ公開し、後段 SkyLightBake に畳み込みを要求する。
    if (ctx.environmentResources) {
        ctx.environmentResources->skyEnvCube       = resources.GetCubemapTexture(h.skyEnvCubeRT);
        ctx.environmentResources->needsConvolution = true;
    }
}

} // namespace fbzz::scene
