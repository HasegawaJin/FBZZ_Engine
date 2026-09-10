/// @file    CustomPostProcessPass.cpp
/// @brief   ユーザーシェーダーのパス。HDR (Composite 前) と PostProcess (後) の 2 系統。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// WHY 段があるか (CustomPassStage の WHY と対):
///   PostProcess 段は «最終画を作り直す» 直列チェーンで、トーンマップ後の LDR。
///   ここで足した光は 1 で頭打ちなので滲まないし、露出とも噛み合わない。
///   HDR の段 (AfterOpaque / SceneHDR) はトーンマップ前なので、書いた値がそのまま
///   ブルームと露出へ流れる。
///
/// WHY «置き換え» だけ余計に 1 パス掛かるか:
///   描き先を読みながら書くことはできない。置き換えるシェーダーは入力の色が要るので、
///   一度どこかへ写してから描き戻すしかない。加算で載せるだけの効果は入力を読まない
///   ので、そのまま 1 パスで済む。
///
/// WHY 縮小と反復を HDR の段だけで許すか:
///   縮小結果や反復の途中経過を置く RT が要る。HDR の段では ping-pong 用の 2 枚が
///   まだ誰にも使われていないので借りられるが、PostProcess 段ではその 2 枚を
///   チェーン自身が入力と出力に使っていて、3 枚目が無い。
#include "PostProcessPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/MaterialParamBinding.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/RenderState.hpp>
#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace fbzz::scene {

namespace {

constexpr int kMaxCustomIterations = 8;
constexpr int kMaxCustomDownscale  = 8;

// ── .mat の解決 ────────────────────────────────────────────────────────────
// 解決はシェーダーのロードとリフレクションを伴うので毎ドローやる値段ではない。
// フレームに 1 度だけやり直して、.mat の編集をその場で絵へ出す (DecalPass と同じ作り)。
struct CustomMaterialBinding {
    renderer::Material         material;
    renderer::ShaderDescriptor descriptor;
    renderer::BlendMode        blendMode = renderer::BlendMode::OPAQUE_BLEND;
    uint64_t                   resolvedFrame = 0;
    bool                       ok = false;
};

std::unordered_map<std::string, CustomMaterialBinding> g_customMaterials;
std::unordered_set<std::string>                        g_warnedCustomMaterials;

bool WarnCustomMaterialOnce(const std::string& path)
{
    return g_warnedCustomMaterials.insert(path).second;
}

// materialPath から «シェーダー + テクスチャ + b2» を揃える。解決できなければ nullptr
// (呼び出し側は shaderPath の経路へ落ちる)。
CustomMaterialBinding* ResolveCustomMaterial(renderer::ResourceManager& resources,
                                             const std::string& path)
{
    if (path.empty()) return nullptr;

    const auto assetHandle = asset::AssetManager::LoadMaterial(path);
    const auto* matAsset = assetHandle.IsValid()
        ? asset::AssetManager::GetMaterial(assetHandle) : nullptr;
    if (!matAsset) {
        if (WarnCustomMaterialOnce(path))
            FBZZ_LOG_WARN("Custom pass material load failed '%s' -> falling back to shaderPath.",
                          path.c_str());
        return nullptr;
    }
    // WHY 用途を検査するか: メッシュ用の .mat は頂点入力を前提にしたシェーダーを指す。
    //     カスタムパスは頂点バッファを持たない SV_VertexID 描画なので、割り当てると
    //     入力レイアウト不一致で何も出ないか画面が塗り潰される。絵からは原因が読めない。
    if (matAsset->renderPath != asset::RenderPath::PostProcess) {
        if (WarnCustomMaterialOnce(path))
            FBZZ_LOG_WARN("Custom pass material '%s' is not declared for post process "
                          "(render_path must be \"post_process\") -> falling back to shaderPath.",
                          path.c_str());
        return nullptr;
    }
    if (matAsset->shaderPath.empty()) {
        if (WarnCustomMaterialOnce(path))
            FBZZ_LOG_WARN("Custom pass material '%s' has no shader -> falling back to shaderPath.",
                          path.c_str());
        return nullptr;
    }

    CustomMaterialBinding& binding = g_customMaterials[path];
    if (binding.resolvedFrame == Time::frameCount)
        return binding.ok ? &binding : nullptr;
    binding.resolvedFrame = Time::frameCount;
    binding.ok            = false;

    renderer::Material& material = binding.material;
    material.shaderPath = matAsset->shaderPath;
    material.shader     = resources.LoadShader(matAsset->shaderPath);
    if (!material.shader.IsValid()) {
        if (WarnCustomMaterialOnce(path))
            FBZZ_LOG_WARN("Custom pass material '%s' shader '%s' failed to load -> falling back.",
                          path.c_str(), matAsset->shaderPath.c_str());
        return nullptr;
    }

    // 記述子は値ごと持つ。シェーダーはホットリロードで差し替わりうるので、
    // ポインタで持つと解決時の中身と食い違う瞬間ができる。
    binding.descriptor = {};
    if (auto* shader = resources.Get(material.shader))
        binding.descriptor = shader->GetDescriptor();

    material.paramData.assign(binding.descriptor.cbufferSize, 0u);
    if (binding.descriptor.IsValid()) {
        asset::InitDefaultMaterialParams(binding.descriptor, material.paramData);
        asset::ApplyMaterialAssetParams(*matAsset, binding.descriptor, material.paramData);
    }

    const auto texturePaths = asset::ResolveMaterialTexturePaths(*matAsset);
    material.textures.resize(texturePaths.size());
    for (size_t i = 0; i < texturePaths.size(); ++i) {
        material.textures[i] = texturePaths[i].empty()
            ? renderer::ResourceHandle<renderer::TextureTag>{}
            : resources.LoadTexture(texturePaths[i]);
    }

    material.Init(resources, binding.descriptor.cbufferSize);
    material.Upload(resources, binding.descriptor);

    binding.blendMode = matAsset->blendMode;
    binding.ok        = true;
    return &binding;
}

// ── 共通の組み立て ─────────────────────────────────────────────────────────

/// b5 をカスタムパスの内容で埋める。段が変わっても «同じ名前で同じ意味» にする。
PostProcCB MakeCustomPostProcCB(const renderer::CustomPostProcessSettings& custom,
                                uint32_t width, uint32_t height,
                                float uvScale, int iteration, int iterationCount)
{
    PostProcCB postData = MakeScreenPostProcCB(width, height);
    postData.time = Time::time;
    postData.customIntensity = custom.intensity;
    postData.customBlend = custom.blend;
    for (int i = 0; i < 4; ++i) {
        postData.customParameters[i]  = custom.parameters[i];
        postData.customParameters2[i] = custom.parameters[i + 4];
    }
    postData.customPassInfo[0] = uvScale;
    postData.customPassInfo[1] = static_cast<float>(iteration);
    postData.customPassInfo[2] = static_cast<float>(iterationCount);
    postData.customPassInfo[3] = 0.0f;
    return postData;
}

/// 宣言された追加入力だけを束ねる。
///
/// WHY 全部を無条件に渡さないか:
///   Velocity / GBuffer は «有効なフレームにしか存在しない» リソースで、無条件に
///   束縛すると «たまたま動いていた» 依存が生まれる。宣言させておけば、足りない
///   ものが何かを設定から読み取れる。
///
/// WHY HDR の段では深度を渡さないか:
///   あの段の描き先は hdrRT で、その深度は描画先として束縛されている。同じリソースを
///   同時に SRV として読むことはできない (API が黙って外す)。«読めているつもりで
///   全部 0» という壊れ方をするくらいなら、渡さない方が原因が見える。
///   遮蔽の判定が要るなら、マスクを描く側で済ませておくこと (ObjectMaskPass)。
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

/// blendMode ごとの全画面 PSO。深度は常に切る (全画面三角形に深度の意味が無い)。
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

/// 1 件ぶんの «何で描くか»。materialPath が解決できたときはそちらが全部を決める。
struct ResolvedCustomPass {
    renderer::ResourceHandle<renderer::ShaderTag> shader;
    CustomMaterialBinding* material = nullptr;
    renderer::BlendMode blendMode = renderer::BlendMode::OPAQUE_BLEND;

    [[nodiscard]] bool IsValid() const { return shader.IsValid(); }
};

ResolvedCustomPass ResolveCustomPass(RenderPassContext& ctx, uint32_t customIndex)
{
    ResolvedCustomPass resolved;
    auto& h = ctx.handles;
    const auto& pp = ctx.settings.postProcess;
    if (customIndex >= pp.customEffects.size() || customIndex >= h.customPostProcessShaders.size())
        return resolved;

    const auto& custom = pp.customEffects[customIndex];
    if (!custom.enabled) return resolved;

    resolved.blendMode = custom.blendMode;
    if (auto* binding = ResolveCustomMaterial(ctx.resources, custom.materialPath)) {
        resolved.material  = binding;
        resolved.shader    = binding->material.shader;
        // .mat が合成方法まで持っているなら、それが書き手の意図。設定側は補欠。
        resolved.blendMode = binding->blendMode;
        return resolved;
    }
    resolved.shader = h.customPostProcessShaders[customIndex];
    return resolved;
}

/// マテリアル由来の b2 とテクスチャを束ねる。.mat が無いパスでは何もしない。
void BindMaterial(renderer::DrawCall& dc, const ResolvedCustomPass& resolved)
{
    if (!resolved.material) return;
    const renderer::Material& material = resolved.material->material;
    if (material.paramsBuffer.IsValid()) dc.constantBuffers[2] = material.paramsBuffer;
    for (std::size_t i = 0; i < material.textures.size() && i < 5; ++i)
        dc.textures[i] = material.textures[i];
}

} // namespace

void ExecuteCustomPostProcessPass(RenderPassContext& ctx, uint32_t customIndex, uint32_t outputIndex)
{
    auto& h = ctx.handles;
    const auto& pp = ctx.settings.postProcess;
    if (customIndex >= pp.customEffects.size()) return;

    const auto& custom = pp.customEffects[customIndex];
    const ResolvedCustomPass resolved = ResolveCustomPass(ctx, customIndex);
    if (!resolved.IsValid()) {
        // シェーダーも .mat も解決できない要求は、今までここで黙って捨てていた。
        // 症状が «その効果だけ何も起きない» なので、パス名を名指しで出す。
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

    // 縮小も反復もこの段では効かない (ファイル冒頭の WHY)。uvScale は常に 1。
    const PostProcCB postData = MakeCustomPostProcCB(custom, ctx.width, ctx.height, 1.0f, 0, 1);
    resources.Update(h.postprocCB, &postData, sizeof(PostProcCB));

    renderer::DrawCall customDC;
    customDC.shader = resolved.shader;
    customDC.pipelineState = h.postprocPSO;
    customDC.vertexCount = 3;
    customDC.constantBuffers[5] = h.postprocCB;
    customDC.textures[5] = h.postProcessInput.IsValid()
        ? h.postProcessInput
        : resources.GetColorTexture(ctx.Res().Target("LDR"), 0);
    BindMaterial(customDC, resolved);
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

    // WHY 専用の RT を作らずポストプロセスの ping-pong 枠を借りるか:
    //   全 RenderTarget は RGBA16F (DX11RenderTarget::Init) なので、LDR チェーン用の
    //   枠がそのまま HDR の受け皿になる。あちらが使うのは Composite より後で、
    //   しかも毎回全画面を書き直すため、ここで踏んだ中身は誰にも読まれない。
    //   専用に 2 枚持つと、使っていないプロジェクトでも全解像度ぶんの VRAM を払う。
    const auto scratchA = h.customPostProcessRT[0];
    const auto scratchB = h.customPostProcessRT[1];
    const bool needsScratch = replaces || reduced;
    if (needsScratch && (!scratchA.IsValid() || !h.copyColorShader.IsValid())) return;
    if (reduced && (!scratchB.IsValid() || !h.customComposeShader.IsValid())) return;

    const uint32_t reducedW = (std::max)(1u, ctx.width  / static_cast<uint32_t>(downscale));
    const uint32_t reducedH = (std::max)(1u, ctx.height / static_cast<uint32_t>(downscale));
    const float    uvScale  = reduced ? 1.0f / static_cast<float>(downscale) : 1.0f;

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
                MakeCustomPostProcCB(custom, ctx.width, ctx.height, 1.0f, i, iterations);
            resources.Update(h.postprocCB, &postData, sizeof(PostProcCB));

            // 置き換えるなら «今の HDR» を退避してから描き戻す。
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
            BindMaterial(dc, resolved);
            BindDeclaredInputs(dc, ctx, custom.inputs, false);
            r.Submit(dc, resources);
        }
        return;
    }

    // 縮小して走る。入力を縮小コピー → 反復 → 実寸へ拡大合成。
    //
    // WHY 縮小側を消しておくか: 加算で合成する場合、書かれていない画素は «足さない»
    //     でなければならない。前フレームの残りが入っていると、その分がそのまま光る。
    r.SetRenderTarget(scratchA, resources);
    r.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });
    r.SetRenderTarget(scratchB, resources);
    r.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });

    {
        // 入力の縮小コピー。UV は 0..1 のまま全画面を読み、小さいビューポートへ書く。
        const PostProcCB postData =
            MakeCustomPostProcCB(custom, reducedW, reducedH, uvScale, 0, iterations);
        resources.Update(h.postprocCB, &postData, sizeof(PostProcCB));
        r.SetRenderTarget(scratchA, resources);
        r.SetViewport(0, 0, reducedW, reducedH);
        drawCopy(resources.GetColorTexture(ctx.Res().Target("HDR"), 0));
    }

    auto source = scratchA;
    for (int i = 0; i < iterations; ++i) {
        const auto target = (source == scratchA) ? scratchB : scratchA;
        const PostProcCB postData =
            MakeCustomPostProcCB(custom, reducedW, reducedH, uvScale, i, iterations);
        resources.Update(h.postprocCB, &postData, sizeof(PostProcCB));

        r.SetRenderTarget(target, resources);
        r.SetViewport(0, 0, reducedW, reducedH);

        renderer::DrawCall dc;
        dc.shader = resolved.shader;
        // 縮小側は必ず置き換えで書く。加算するかどうかは最後の合成で決まる。
        dc.pipelineState = h.postprocPSO;
        dc.vertexCount = 3;
        dc.constantBuffers[5] = h.postprocCB;
        dc.textures[5] = resources.GetColorTexture(source, 0);
        BindMaterial(dc, resolved);
        BindDeclaredInputs(dc, ctx, custom.inputs, false);
        r.Submit(dc, resources);

        source = target;
    }

    // 実寸へ戻す。ここで初めて blendMode が効く (加算ならこの 1 枚を足す)。
    r.SetRenderTarget(ctx.Res().Target("HDR"), resources);
    renderer::DrawCall composeDC;
    composeDC.shader = h.customComposeShader;
    composeDC.pipelineState = CustomPassPSO(ctx, resolved.blendMode);
    composeDC.vertexCount = 3;
    composeDC.constantBuffers[5] = h.postprocCB;
    composeDC.textures[5] = resources.GetColorTexture(source, 0);
    r.Submit(composeDC, resources);
}

void ReleaseCustomPassMaterialCache()
{
    g_customMaterials.clear();
    g_warnedCustomMaterials.clear();
}

} // namespace fbzz::scene
