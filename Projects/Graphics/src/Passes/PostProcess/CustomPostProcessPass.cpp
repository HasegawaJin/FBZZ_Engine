/// @file    CustomPostProcessPass.cpp
/// @brief   ユーザーシェーダーのパス。HDR (Composite 前) と PostProcess (後) の 2 系統。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// @note PostProcess 段は最終画を作り直す直列チェーン (トーンマップ後の LDR) で、足した光は
/// @note 1 で頭打ちのため滲まず露出とも噛み合わない。HDR の段 (AfterOpaque/SceneHDR) は
/// @note トーンマップ前で、書いた値がそのままブルームと露出へ流れる。
/// @note 「置き換え」だけ 1 パス余計に掛かるのは描き先を読みながら書けないため (一度写して
/// @note 描き戻す)。加算は入力を読まないので 1 パスで済む。縮小・反復は途中経過を置く RT が
/// @note 要り、HDR 段の ping-pong 用 2 枚は借りられるが PostProcess 段はその 2 枚を入出力に
/// @note 使い切っていて 3 枚目が無いため HDR 段限定にする。
#include <Graphics/Passes/PostProcess/PostProcessPasses.hpp>
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Core/Logger.hpp>
namespace fbzz::renderer {
namespace {
constexpr int kMaxCustomIterations = 8;
constexpr int kMaxCustomDownscale = 8;
/// @name 共通の組み立て

/// @note b5 をカスタムパスの内容で埋める。段が変わっても «同じ名前で同じ意味» にする。
PostProcCB MakeCustomPostProcCB(const renderer::CustomPostProcessSettings& custom,
                                float time, uint32_t width, uint32_t height,
                                float uvScaleX, float uvScaleY, int iteration, int iterationCount)
{
    PostProcCB postData = MakeScreenPostProcCB(width, height);
    postData.time = time;
    postData.customIntensity = custom.intensity;
    postData.customBlend = custom.blend;
    for (int i = 0; i < 4; ++i) {
        postData.customParameters[i]  = custom.parameters[i];
        postData.customParameters2[i] = custom.parameters[i + 4];
    }
    postData.customPassInfo[0] = uvScaleX;
    postData.customPassInfo[1] = static_cast<float>(iteration);
    postData.customPassInfo[2] = static_cast<float>(iterationCount);
    /// @note 縮小した RT の幅と高さは別々に切り捨てられるので、縦の倍率は横と一致しない。
    postData.customPassInfo[3] = uvScaleY;
    return postData;
}

/// @brief 宣言された追加入力だけを束ねる。
/// @note 全部を無条件に渡さないのは、Velocity/GBuffer が有効なフレームにしか存在しない
/// @note リソースで、無条件束縛は「たまたま動いていた」依存を生むため。宣言させれば足りない
/// @note ものが設定から読み取れる。
/// @note HDR の段では深度を渡さない。その段の描き先は hdrRT で深度は描画先として束縛済み
/// @note のため、同じリソースを同時に SRV で読めない (API が黙って外し「読めているつもりで
/// @note 全部 0」という壊れ方をする)。遮蔽判定が要るならマスク側 (ObjectMaskPass) で済ませる。
void BindDeclaredInputs(renderer::DrawCall& dc, RenderPassContext& ctx,
                        uint32_t inputs, bool allowDepth)
{
    auto& h = ctx.handles;
    auto& resources = ctx.resources;

    if ((inputs & renderer::CUSTOM_PASS_INPUT_OBJECT_MASK) != 0u
        && ctx.objectMaskEnabled && ctx.Res().Target("ObjectMask").IsValid()) {
        dc.textures[6] = resources.GetColorTexture(ctx.Res().Target("ObjectMask"), 0);
        if (allowDepth) dc.textures[8] = resources.GetDepthTexture(ctx.Res().Target("ObjectMask"));
    }
    if ((inputs & renderer::CUSTOM_PASS_INPUT_SCENE_DEPTH) != 0u && allowDepth)
        dc.textures[7] = resources.GetDepthTexture(ctx.Res().Target("HDR"));
    if ((inputs & renderer::CUSTOM_PASS_INPUT_SSAO) != 0u && ctx.Res().Texture("SSAO").IsValid())
        dc.textures[9] = ctx.Res().Texture("SSAO");
    if ((inputs & renderer::CUSTOM_PASS_INPUT_BLOOM) != 0u && ctx.Res().Texture("Bloom").IsValid())
        dc.textures[10] = ctx.Res().Texture("Bloom");
    if ((inputs & renderer::CUSTOM_PASS_INPUT_VELOCITY) != 0u && ctx.Res().Target("Velocity").IsValid())
        dc.textures[11] = resources.GetColorTexture(ctx.Res().Target("Velocity"), 0);
    if ((inputs & renderer::CUSTOM_PASS_INPUT_NORMAL) != 0u && ctx.Res().Target("GBuffer").IsValid())
        dc.textures[12] = resources.GetColorTexture(ctx.Res().Target("GBuffer"), 1);
}

/// @note blendMode ごとの全画面 PSO。深度は常に切る (全画面三角形に深度の意味が無い)。
renderer::ResourceHandle<renderer::PipelineStateTag> CustomPassPSO(
    RenderPassContext& ctx, renderer::BlendMode blend)
{
    if (blend == renderer::BlendMode::OPAQUE_BLEND) return ctx.handles.postprocPSO;

    static uint32_t s_resetVersion = ctx.resources.GetResetVersion();
    static renderer::ResourceHandle<renderer::PipelineStateTag> s_pso[4];

    if (s_resetVersion != ctx.resources.GetResetVersion()) {
        s_resetVersion = ctx.resources.GetResetVersion();
        for (auto& pso : s_pso) pso = {};
    }

    const auto slot = static_cast<std::size_t>(blend);
    if (slot >= 4) return ctx.handles.postprocPSO;
    if (!s_pso[slot].IsValid()) {
        s_pso[slot] = ctx.resources.CreatePipelineState(
            { renderer::RasterizerMode::SOLID, blend, renderer::DepthMode::DEPTH_OFF });
    }
    return s_pso[slot];
}

using ResolvedCustomPass = RenderCustomPostInput;
ResolvedCustomPass ResolveCustomPass(RenderPassContext& ctx, uint32_t index)
{
    if (!ctx.renderScene || index >= ctx.renderScene->customPost.size()) return {};
    return ctx.renderScene->customPost[index];
}
void BindMaterial(DrawCall& dc, const ResolvedCustomPass& input, ResourceManager& resources)
{
    dc.constantBuffers[2] = input.paramsBuffer;
    if (input.paramsBuffer.IsValid() && !input.parameters.empty())
        resources.Update(input.paramsBuffer, input.parameters.data(), input.parameters.size());
    for (size_t i = 0; i < input.textures.size(); ++i) dc.textures[i] = input.textures[i];
}
}
void ExecuteCustomPostProcessPass(RenderPassContext& ctx, uint32_t customIndex, uint32_t outputIndex)
{
    auto& h = ctx.handles;
    const auto& pp = ctx.settings.postProcess;
    if (customIndex >= pp.customEffects.size()) return;

    const auto& custom = pp.customEffects[customIndex];
    const ResolvedCustomPass resolved = ResolveCustomPass(ctx, customIndex);
    if (!resolved.IsValid()) {
        /// @note シェーダーも .mat も解決できない要求は、今までここで黙って捨てていた。
        /// @note 症状が «その効果だけ何も起きない» なので、パス名を名指しで出す。
        static std::string sLastUnresolved;
        if (sLastUnresolved != custom.name) {
            sLastUnresolved = custom.name;
            FBZZ_LOG_WARN("CustomPostProcess: '%s' のシェーダーを解決できません "
                          "(shaderPath='%s' materialPath='%s')",
                          custom.name.c_str(), custom.shaderPath.c_str(),
                          custom.materialPath.c_str());
        }
        return;
    }

    const bool needsIntermediate = outputIndex < 2;
    if (needsIntermediate && !h.customPostProcessRT[outputIndex].IsValid())
        return;

    auto& r = ctx.renderer;
    auto& resources = ctx.resources;
    r.SetRenderTarget(needsIntermediate ? h.customPostProcessRT[outputIndex] : ctx.chainOutputRT, resources);

    /// @note 縮小も反復もこの段では効かない (理由はファイル冒頭を参照)。uvScale は常に 1。
    const PostProcCB postData = MakeCustomPostProcCB(custom, ctx.time, ctx.width, ctx.height, 1.0f, 1.0f, 0, 1);
    resources.Update(h.postprocCB, &postData, sizeof(PostProcCB));

    renderer::DrawCall customDC;
    customDC.shader = resolved.shader;
    customDC.pipelineState = h.postprocPSO;
    customDC.vertexCount = 3;
    customDC.constantBuffers[5] = h.postprocCB;
    customDC.textures[5] = h.postProcessInput.IsValid()
        ? h.postProcessInput
        : resources.GetColorTexture(ctx.Res().Target("LDR"), 0);
    BindMaterial(customDC, resolved, resources);
    BindDeclaredInputs(customDC, ctx, custom.inputs, true);
    r.Submit(customDC, resources);

    if (needsIntermediate) {
        h.postProcessInput = resources.GetColorTexture(h.customPostProcessRT[outputIndex], 0);
        h.fxaaInput = h.postProcessInput;
    }
}

void ExecuteCustomHdrPass(RenderPassContext& ctx, uint32_t customIndex)
{
    auto& h = ctx.handles;
    const auto& pp = ctx.settings.postProcess;
    if (customIndex >= pp.customEffects.size() || !ctx.Res().Target("HDR").IsValid()) return;

    const auto& custom = pp.customEffects[customIndex];
    const ResolvedCustomPass resolved = ResolveCustomPass(ctx, customIndex);
    if (!resolved.IsValid()) return;

    auto& r = ctx.renderer;
    auto& resources = ctx.resources;

    const bool replaces   = resolved.blendMode == renderer::BlendMode::OPAQUE_BLEND;
    const int  iterations = std::clamp(custom.iterations, 1, kMaxCustomIterations);
    const int  downscale  = std::clamp(custom.downscale, 1, kMaxCustomDownscale);
    const bool reduced    = downscale > 1;

    /// @note 専用の RT を作らずポストプロセスの ping-pong 枠を借りるのは、全 RenderTarget が
    /// @note RGBA16F (`DX11RenderTarget::Init`) で LDR チェーン用の枠がそのまま HDR の受け皿に
    /// @note なり、あちらの使用は Composite より後で毎回全画面を書き直すため踏んだ中身が誰にも
    /// @note 読まれないから。専用に 2 枚持つと使わないプロジェクトでも全解像度分の VRAM を払う。
    const auto scratchA = h.customPostProcessRT[0];
    const auto scratchB = h.customPostProcessRT[1];
    const bool needsScratch = replaces || reduced;
    if (needsScratch && (!scratchA.IsValid() || !h.copyColorShader.IsValid())) return;
    if (reduced && (!scratchB.IsValid() || !h.customComposeShader.IsValid())) return;

    const uint32_t reducedW = (std::max)(1u, ctx.width  / static_cast<uint32_t>(downscale));
    const uint32_t reducedH = (std::max)(1u, ctx.height / static_cast<uint32_t>(downscale));
    /// @note 倍率は «縮小後の画素数 / 実寸の画素数» で縦横別に出す。1/downscale だと割り切れない解像度で
    /// @note 有効範囲の外 (クリアした 0) まで読み、右端と下端が暗くなる。
    const float    uvScaleX = static_cast<float>(reducedW) / static_cast<float>((std::max)(ctx.width, 1u));
    const float    uvScaleY = static_cast<float>(reducedH) / static_cast<float>((std::max)(ctx.height, 1u));

    const auto drawCopy = [&](renderer::ResourceHandle<renderer::TextureTag> source) {
        renderer::DrawCall copyDC;
        copyDC.shader = h.copyColorShader;
        copyDC.pipelineState = h.postprocPSO;
        copyDC.vertexCount = 3;
        copyDC.textures[5] = source;
        r.Submit(copyDC, resources);
    };

    if (!reduced) {
        for (int i = 0; i < iterations; ++i) {
            const PostProcCB postData =
                MakeCustomPostProcCB(custom, ctx.time, ctx.width, ctx.height, 1.0f, 1.0f, i, iterations);
            resources.Update(h.postprocCB, &postData, sizeof(PostProcCB));

            /// @note 置き換えるなら «今の HDR» を退避してから描き戻す。
            if (replaces) {
                r.SetRenderTarget(scratchA, resources);
                drawCopy(resources.GetColorTexture(ctx.Res().Target("HDR"), 0));
            }

            r.SetRenderTarget(ctx.Res().Target("HDR"), resources);
            renderer::DrawCall dc;
            dc.shader = resolved.shader;
            dc.pipelineState = CustomPassPSO(ctx, resolved.blendMode);
            dc.vertexCount = 3;
            dc.constantBuffers[5] = h.postprocCB;
            if (replaces) dc.textures[5] = resources.GetColorTexture(scratchA, 0);
            BindMaterial(dc, resolved, resources);
            BindDeclaredInputs(dc, ctx, custom.inputs, false);
            r.Submit(dc, resources);
        }
        return;
    }

    /// @note 縮小して走る。入力を縮小コピー → 反復 → 実寸へ拡大合成。縮小側を先に消すのは、
    /// @note 加算合成で書かれていない画素は「足さない」でなければならず、前フレームの残りが
    /// @note 入っているとその分がそのまま光ってしまうため。
    r.SetRenderTarget(scratchA, resources);
    r.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });
    r.SetRenderTarget(scratchB, resources);
    r.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });

    {
        /// @note 入力の縮小コピー。UV は 0..1 のまま全画面を読み、小さいビューポートへ書く。
        const PostProcCB postData =
            MakeCustomPostProcCB(custom, ctx.time, reducedW, reducedH, uvScaleX, uvScaleY, 0, iterations);
        resources.Update(h.postprocCB, &postData, sizeof(PostProcCB));
        r.SetRenderTarget(scratchA, resources);
        r.SetViewport(0, 0, reducedW, reducedH);
        drawCopy(resources.GetColorTexture(ctx.Res().Target("HDR"), 0));
    }

    auto source = scratchA;
    for (int i = 0; i < iterations; ++i) {
        const auto target = (source == scratchA) ? scratchB : scratchA;
        const PostProcCB postData =
            MakeCustomPostProcCB(custom, ctx.time, reducedW, reducedH, uvScaleX, uvScaleY, i, iterations);
        resources.Update(h.postprocCB, &postData, sizeof(PostProcCB));

        r.SetRenderTarget(target, resources);
        r.SetViewport(0, 0, reducedW, reducedH);

        renderer::DrawCall dc;
        dc.shader = resolved.shader;
        /// @note 縮小側は必ず置き換えで書く。加算するかどうかは最後の合成で決まる。
        dc.pipelineState = h.postprocPSO;
        dc.vertexCount = 3;
        dc.constantBuffers[5] = h.postprocCB;
        dc.textures[5] = resources.GetColorTexture(source, 0);
        BindMaterial(dc, resolved, resources);
        BindDeclaredInputs(dc, ctx, custom.inputs, false);
        r.Submit(dc, resources);

        source = target;
    }

    /// @note 実寸へ戻す。ここで初めて blendMode が効く (加算ならこの 1 枚を足す)。
    r.SetRenderTarget(ctx.Res().Target("HDR"), resources);
    renderer::DrawCall composeDC;
    composeDC.shader = h.customComposeShader;
    composeDC.pipelineState = CustomPassPSO(ctx, resolved.blendMode);
    composeDC.vertexCount = 3;
    composeDC.constantBuffers[5] = h.postprocCB;
    composeDC.textures[5] = resources.GetColorTexture(source, 0);
    r.Submit(composeDC, resources);
}

}
