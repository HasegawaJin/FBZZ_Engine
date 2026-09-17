/// @file    DecalPass.cpp
/// @brief   Deferred デカールパス
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// 各 DecalComponent に対してフルスクリーントライアングルを 1 draw 発行する。
/// PS が深度バッファからワールド座標を復元し、デカール OBB 外のフラグメントを
/// discard することで表面への投影を実現する。
///
/// マテリアルは 2 経路ある:
///   materialPath 空  → 組み込み Decal.hlsl + コンポーネントの色・テクスチャ
///   materialPath 有り → .mat (render_path = "decal") のシェーダーとパラメータ
/// どちらの経路でも投影ボリューム・角度フェード・ライフタイムは b10 の
/// DecalConstants がエンジン側から埋める (DecalCommon.hlsli 参照)。
#include "GeometryPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/MaterialParamBinding.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Math/Matrix4.hpp>
#include <Physics/Layer.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::scene {
namespace {

/// @brief .mat 1 件ぶんの解決結果。
/// @note PSO まで持つ理由: .mat は blend_mode を持ち、デカール既定の ALPHA_BLEND 固定 PSO を使い回すと .mat の blend_mode が黙って無視される。
/// @note 値で持つ理由 (shared_ptr にしない): この構造体は下のキャッシュの要素としてしか存在せず、unordered_map はノード単位で確保するため rehash しても要素のアドレスは動かない。
struct DecalMaterialBinding {
    renderer::Material                                    material;
    /// 上書きを名前でバイトオフセットへ写像するのに要る。
    renderer::ShaderDescriptor                            descriptor;
    renderer::ResourceHandle<renderer::PipelineStateTag>  pso;
    /// PSO を作り直す判断に使う。CreatePipelineState は重複を畳まないため、
    /// 毎フレーム呼ぶとステートオブジェクトが際限なく増える。
    renderer::BlendMode                                   psoBlend = renderer::BlendMode::ALPHA_BLEND;
    bool                                                  psoValid = false;
    /// 上書きを持つデカールだけが通す一時 cbuffer。
    /// @note マテリアルごとに 1 本で足りる理由: Update は Upload Arena のスライスを切るため、同じハンドルへ書いて Submit を繰り返しても記録済みの Draw はそれぞれ自分の書き込み時点の中身を読む。
    renderer::ResourceHandle<renderer::ConstantBufferTag> overrideConstants;
    std::vector<uint8_t>                                  overrideScratch;
    /// このパス呼び出しで既に値を適用したか。弾痕は同じ .mat を数十個が共有するので、
    /// デカールごとにリフレクション適用と Upload をやり直すと数だけ無駄が増える。
    uint64_t                                              resolvedPass = 0;
    bool                                                  resolvedOk   = false;
};

/// .mat のパスで引く。解決はシェーダーのロードとリフレクションを伴うので毎フレーム
/// やる値段ではない。値の適用はパスごとに 1 回だけやり直し、.mat の編集を絵へ出す。
std::unordered_map<std::string, DecalMaterialBinding> g_decalMaterials;
/// 同じ .mat の失敗を毎フレーム記録するとログが埋まる。
std::unordered_set<std::string>                       g_warnedDecalMaterials;
/// ExecuteDecalPass の呼び出し通番。0 は「未解決」を表すため 1 から始める。
uint64_t                                              g_decalPassSerial = 0;

bool WarnDecalMaterialOnce(const std::string& path)
{
    return g_warnedDecalMaterials.insert(path).second;
}

/// materialPath から描画に要るものを揃える。解決できない場合は組み込み経路へ落とす。
DecalMaterialBinding* ResolveDecalMaterial(renderer::ResourceManager& resources,
                                           const DecalComponent& decal)
{
    if (decal.materialPath.empty()) return nullptr;

    const auto assetHandle = asset::AssetManager::Load<asset::MaterialAsset>(decal.materialPath);
    const auto* matAsset = assetHandle.IsValid()
        ? asset::AssetManager::Get<asset::MaterialAsset>(assetHandle) : nullptr;
    if (!matAsset) {
        if (WarnDecalMaterialOnce(decal.materialPath))
            FBZZ_LOG_WARN("Decal material load failed '%s' -> falling back to the built-in decal shader.",
                          decal.materialPath.c_str());
        return nullptr;
    }
    /// @note 用途を検査する理由: メッシュ用の .mat は頂点入力を前提にしたシェーダーを指すが、デカールは頂点バッファを持たない SV_VertexID 描画のため、割り当てると入力レイアウト不一致で何も出ないか画面が塗り潰される。原因が絵からは分からない事故のため名指しで止める。
    if (matAsset->renderPath != asset::RenderPath::Decal) {
        if (WarnDecalMaterialOnce(decal.materialPath))
            FBZZ_LOG_WARN("Decal material '%s' is not declared for decals (render_path must be \"decal\") "
                          "-> falling back to the built-in decal shader.", decal.materialPath.c_str());
        return nullptr;
    }
    if (matAsset->shaderPath.empty()) {
        if (WarnDecalMaterialOnce(decal.materialPath))
            FBZZ_LOG_WARN("Decal material '%s' has no shader -> falling back to the built-in decal shader.",
                          decal.materialPath.c_str());
        return nullptr;
    }

    DecalMaterialBinding& binding = g_decalMaterials[decal.materialPath];
    if (binding.resolvedPass == g_decalPassSerial)
        return binding.resolvedOk ? &binding : nullptr;
    binding.resolvedPass = g_decalPassSerial;
    binding.resolvedOk   = false;

    renderer::Material& material  = binding.material;
    material.shaderPath = matAsset->shaderPath;
    material.shader     = resources.LoadShader(matAsset->shaderPath);
    if (!material.shader.IsValid()) {
        if (WarnDecalMaterialOnce(decal.materialPath))
            FBZZ_LOG_WARN("Decal material '%s' shader '%s' failed to load -> falling back.",
                          decal.materialPath.c_str(), matAsset->shaderPath.c_str());
        return nullptr;
    }

    /// @note 記述子は値ごと持つ。シェーダーはホットリロードで差し替わりうるので、
    ///       ポインタで持つと解決時の中身と食い違う瞬間ができる。
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

    if (binding.overrideScratch.size() != material.paramData.size()) {
        binding.overrideScratch.resize(material.paramData.size());
        if (binding.overrideConstants.IsValid()) {
            resources.Release(binding.overrideConstants);
            binding.overrideConstants = {};
        }
    }
    if (!binding.overrideConstants.IsValid() && binding.descriptor.cbufferSize > 0)
        binding.overrideConstants = resources.CreateConstantBuffer(binding.descriptor.cbufferSize);

    /// @note デカールは深度を書かない。ブレンドだけ .mat に従わせる。
    if (!binding.psoValid || binding.psoBlend != matAsset->blendMode) {
        binding.pso = resources.CreatePipelineState({
            renderer::RasterizerMode::SOLID, matAsset->blendMode, renderer::DepthMode::DEPTH_OFF });
        binding.psoBlend = matAsset->blendMode;
        binding.psoValid = binding.pso.IsValid();
    }
    if (!binding.psoValid) return nullptr;

    binding.resolvedOk = true;
    return &binding;
}

/// 共有マテリアルの上へこのデカールだけの上書きを重ねる。上書きが無ければ何もしない。
void ApplyDecalMaterialOverrides(renderer::ResourceManager& resources,
                                 DecalMaterialBinding& binding,
                                 const DecalComponent& decal,
                                 renderer::DrawCall& call)
{
    if (!decal.materialTextureOverrides.empty()) {
        const auto& slotNames = asset::kMaterialTextureSlotNames;
        for (size_t slot = 0; slot < slotNames.size() && slot < call.textures.size(); ++slot) {
            const auto it = decal.materialTextureOverrides.find(slotNames[slot]);
            if (it == decal.materialTextureOverrides.end() || it->second.empty()) continue;
            if (const auto texture = resources.LoadTexture(it->second); texture.IsValid())
                call.textures[slot] = texture;
        }
    }

    if (decal.materialParamOverrides.empty()) return;
    if (!binding.overrideConstants.IsValid()) return;
    if (binding.overrideScratch.size() != binding.material.paramData.size()) return;

    /// @note 共有側の paramData は触らない — 触ると同じ .mat を使う他のデカールへ波及する。
    std::memcpy(binding.overrideScratch.data(),
                binding.material.paramData.data(),
                binding.material.paramData.size());
    asset::ApplyMaterialParamOverrides(decal.materialParamOverrides,
                                       binding.descriptor, binding.overrideScratch);
    resources.Update(binding.overrideConstants,
                     binding.overrideScratch.data(),
                     static_cast<uint32_t>(binding.overrideScratch.size()));
    call.constantBuffers[2] = binding.overrideConstants;
}

/// @brief 受信レイヤーバッファを 1 フレーム 1 回だけ描く。
/// @note マスクごとにシーンを描き直さない。可視サーフェスのレイヤー番号を書いておけば判定はデカール側のビットテストで済み、描画は 1 回に畳める。
/// @param markMask 番号を書き込む必要があるレイヤーの集合。どのデカールも受信を許すレイヤーは書かなくてよい (Decal 側は「未描画 = 受信」と解釈する)。
/// @return 描けたら true。false の場合、呼び出し側はフィルタ自体を無効にする。
bool RenderDecalReceiverLayers(RenderPassContext& ctx, fbzz::LayerMask markMask)
{
    auto& r         = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    if (!h.decalMaskRT.IsValid() || !h.decalMaskShader.IsValid() ||
        !h.decalMaskPSO.IsValid() || !h.decalReceiverCB.IsValid())
        return false;

    const auto depthTex = resources.GetDepthTexture(ctx.Res().Target("DecalDepth"));
    if (!depthTex.IsValid()) return false;

    r.SetRenderTarget(h.decalMaskRT, resources);
    r.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });

    for (auto& go : ctx.scene.GameObjects()) {
        if (!go.activeInHierarchy()) continue;
        if (!fbzz::Layer::Contains(ctx.cullingMask, go.layer)) continue;

        auto* mr  = go.GetComponent<MeshRenderer>();
        auto* smr = go.GetComponent<SkinnedMeshRenderer>();
        const bool drawStatic =
            mr && mr->enabled && mr->lodVisible && mr->mesh && !mr->mesh->isSkinned &&
            mr->mesh->vertexBuffer.IsValid() && mr->mesh->indexBuffer.IsValid();
        const bool drawSkinned = h.decalMaskSkinnedShader.IsValid() &&
            smr && smr->enabled && smr->lodVisible && smr->model;
        /// @note Static は GameObject に手で付ける層ではなく、静的メッシュを表す受信分類。
        ///       インポート済みの環境オブジェクトへ一つずつ層を設定する必要をなくす。
        const bool markStatic = drawStatic &&
            fbzz::Layer::Contains(markMask, fbzz::Layer::Static);
        const bool markSkinned = !markStatic && drawSkinned &&
            fbzz::Layer::Contains(markMask, go.layer);
        if (!markStatic && !markSkinned) continue;

        PerObjectCB objData{};
        objData.world             = go.transform.GetWorldMatrix();
        objData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(objData.world);
        resources.Update(h.objectCB, &objData, sizeof(PerObjectCB));

        DecalReceiverCB layerData{};
        const int receiverLayer = markStatic ? fbzz::Layer::Static : go.layer;
        layerData.layerEncoded = static_cast<float>((receiverLayer & 31) + 1);
        resources.Update(h.decalReceiverCB, &layerData, sizeof(DecalReceiverCB));

        if (markStatic)
        {
            renderer::DrawCall dc;
            dc.vertexBuffer        = mr->mesh->vertexBuffer;
            dc.indexBuffer         = mr->mesh->indexBuffer;
            dc.indexCount          = mr->mesh->indexCount;
            dc.shader              = h.decalMaskShader;
            dc.pipelineState       = h.decalMaskPSO;
            dc.constantBuffers[0]  = h.frameCB;
            dc.constantBuffers[1]  = h.objectCB;
            dc.constantBuffers[10] = h.decalReceiverCB;
            dc.textures[7]         = depthTex;
            r.Submit(dc, resources);
        }

        if (!markSkinned) continue;

        /// @note Animator はモデルルート側、SkinnedMeshRenderer は submesh 子 GO に分かれる構成が一般的で、自 GO だけを見ると bind pose へ落ち、可視サーフェス判定が T ポーズの深度で走り全画素が捨てられる。
        auto* anim = FindAnimator(go);
        const auto skinCB = ResolveSkinningCB(
            anim ? anim->skinningBuffer : decltype(anim->skinningBuffer){},
            smr->model, h.bindPoseSkinningCB);

        const auto* mat = go.GetComponent<MaterialComponent>();
        /// @note mi はローカルスロット番号 (submeshIndices 対応)。
        for (size_t mi = 0; mi < smr->SubmeshCount(); ++mi) {
            renderer::Mesh* meshPtr = smr->SubmeshMesh(mi);
            if (!meshPtr) continue;
            if (!meshPtr->vertexBuffer.IsValid() || !meshPtr->indexBuffer.IsValid()) continue;
            if (mat && !mat->SlotAt(mi).visible) continue;

            renderer::DrawCall dc;
            dc.vertexBuffer        = smr->ResolveSlotVertexBuffer(mi, meshPtr->vertexBuffer);
            dc.indexBuffer         = meshPtr->indexBuffer;
            dc.indexCount          = meshPtr->indexCount;
            dc.shader              = h.decalMaskSkinnedShader;
            dc.pipelineState       = h.decalMaskPSO;
            dc.constantBuffers[0]  = h.frameCB;
            dc.constantBuffers[1]  = h.objectCB;
            dc.constantBuffers[7]  = skinCB;
            dc.constantBuffers[10] = h.decalReceiverCB;
            dc.textures[7]         = depthTex;
            r.Submit(dc, resources);
        }
    }
    return true;
}

/// このフレームに描くデカール 1 件。走査を 2 周に分けるのは、受信レイヤーバッファを
/// 描くのに「どのレイヤーを書く必要があるか」を先に知る必要があるため。
struct PendingDecal {
    GameObject*     go    = nullptr;
    DecalComponent* decal = nullptr;
    float           fade  = 1.0f;
};

bool DecalHasFlipbook(const DecalComponent& decal)
{
    return decal.frameCount > 1;
}

/// @brief age を進める必要があるか。
/// @note 永続デカール (lifetime < 0) は age を進めない。無条件に足すと、編集中にシーンを開いているだけで age が増え、保存するたびに «誰も触っていない差分» が出る。
bool DecalNeedsAge(const DecalComponent& decal)
{
    return decal.lifetime >= 0.0f || decal.fadeInTime > 0.0f || DecalHasFlipbook(decal);
}

/// アトラスの 1 コマぶんの UV スケールと、今のコマ番号を求める。
/// @return フリップブックが有効なら true (無効時は恒等な (1,1) / 0 を書く)
bool ResolveDecalFlipbook(const DecalComponent& decal, float outScale[2], float& outIndex)
{
    outScale[0] = 1.0f;
    outScale[1] = 1.0f;
    outIndex    = 0.0f;
    if (!DecalHasFlipbook(decal)) return false;

    const int frames = decal.frameCount;
    const int perRow = std::clamp(decal.framesPerRow, 1, frames);
    const int rows   = (frames + perRow - 1) / perRow;

    /// @note frameRate 0 は「寿命いっぱいで 1 周」。血の乾きや焦げの定着は消えるまでに
    ///       終わるのが正しく、lifetime を触るたびに fps を計算し直させたくない。
    float progress = 0.0f;
    if (decal.frameRate > 0.0f)
        progress = decal.age * decal.frameRate / static_cast<float>(frames);
    else if (decal.lifetime > 0.0f)
        progress = decal.age / decal.lifetime;

    if (decal.frameLoop) progress -= std::floor(progress);
    /// @note 止める側は 1.0 を含めない。含めると frames 番目 (存在しないコマ) を指す。
    else                 progress = std::clamp(progress, 0.0f, 0.9999f);

    const int index = std::clamp(static_cast<int>(progress * static_cast<float>(frames)),
                                 0, frames - 1);
    outScale[0] = 1.0f / static_cast<float>(perRow);
    outScale[1] = 1.0f / static_cast<float>(rows);
    outIndex    = static_cast<float>(index);
    return true;
}

} // namespace

/// @brief キャッシュが持つシェーダー・テクスチャ・cbuffer のハンドルを破棄する。
/// @note ハンドルはデバイス世代に属するため、リセット後に古い世代のハンドルを再利用しない。Release は呼ばない (リセット済みの ResourceManager では既に実体が無い)。
void ReleaseDecalMaterialCache()
{
    g_decalMaterials.clear();
    g_warnedDecalMaterials.clear();
}

void ExecuteDecalPass(RenderPassContext& ctx)
{
    auto& r         = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    /// @note DecalDepthCopy が decalDepthRT を束縛したままここへ入ってくる。以降のパスは
    ///       HDR が束縛されている前提なので、デカールが 1 つも無い経路も含めて必ず戻す。
    r.SetRenderTarget(ctx.Res().Target("HDR"), resources);

    if (!h.decalShader.IsValid() || !h.decalPSO.IsValid() ||
        !h.decalCB.IsValid() || !h.decalMaterialCB.IsValid())
        return;

    /// @note .mat の再適用はこの呼び出しで 1 マテリアルにつき 1 回だけにする。
    ++g_decalPassSerial;

    /// @note decalDepthRT はデカールパス直前にコピーされた深度専用 RT。
    ///       hdrRT の深度をそのまま使うと DX11 が DSV/SRV 競合で SRV をサイレント解除する。
    const auto depthTex = resources.GetDepthTexture(ctx.Res().Target("DecalDepth"));
    if (!depthTex.IsValid())
        return;

    const float dt = Time::deltaTime;
    std::vector<EntityID> expiredDecals;
    std::vector<PendingDecal> pending;

    /// @note 全デカールがフィルタで受信を許すレイヤーの積。ここに含まれるレイヤーは
    ///       受信バッファへ書かなくても「未描画 = 受信」で正しく判定できる。
    fbzz::LayerMask alwaysReceive = fbzz::Layer::Everything;
    bool anyFiltered = false;

    for (auto& go : ctx.scene.GameObjects()) {
        if (!go.activeInHierarchy()) continue;
        if (!fbzz::Layer::Contains(ctx.cullingMask, go.layer)) continue;
        auto* decal = go.GetComponent<DecalComponent>();
        if (!decal || !decal->enabled) continue;

        if (DecalNeedsAge(*decal)) decal->age += dt;
        if (decal->lifetime >= 0.0f && decal->age >= decal->lifetime) {
            decal->enabled = false;
            expiredDecals.push_back(go.GetID());
            continue;
        }

        float fade = 1.0f;
        if (decal->lifetime >= 0.0f && decal->fadeTime > 0.0f)
            fade = std::min(1.0f, (decal->lifetime - decal->age) / decal->fadeTime);
        /// @note 出現側のフェード。永続デカールでも効く。寿命が fadeInTime + fadeTime より短いと両窓が重なるため掛けずに小さい方を採る (掛けると «出きる前に消え始める» 山が二重に低くなり短命な痕がはっきり見えないまま終わる)。
        if (decal->fadeInTime > 0.0f)
            fade = std::min(fade, decal->age / decal->fadeInTime);
        fade = std::clamp(fade, 0.0f, 1.0f);
        if (fade < 0.001f) continue;

        if (decal->receiverLayerMask != fbzz::Layer::Everything) {
            alwaysReceive &= decal->receiverLayerMask;
            anyFiltered = true;
        }
        pending.push_back({ &go, decal, fade });
    }

    /// @note 重なった痕の前後。デカールは深度を書かないので、合成する順番だけが前後を決める。
    ///       stable_sort なので sortOrder が同じデカールは走査順 —— 従来の順序 —— のまま。
    std::stable_sort(pending.begin(), pending.end(),
                     [](const PendingDecal& a, const PendingDecal& b) {
                         return a.decal->sortOrder < b.decal->sortOrder;
                     });

    /// @note 受信バッファは全デカールで共有する。RT の張り替えは DX12 でバリアを
    ///       1 回発行するので、デカールごとに往復させない。
    const bool receiverBufferReady =
        anyFiltered && RenderDecalReceiverLayers(ctx, ~alwaysReceive);
    if (receiverBufferReady)
        r.SetRenderTarget(ctx.Res().Target("HDR"), resources);

    for (const auto& entry : pending) {
        GameObject& go          = *entry.go;
        const DecalComponent& d = *entry.decal;
        auto* binding           = ResolveDecalMaterial(resources, d);

        DecalCB decalData{};
        decalData.invDecalWorld  = math::Matrix4::Inverse(go.transform.GetWorldMatrix());
        decalData.decalTangent   = go.transform.right.Normalized();
        decalData.decalBitangent = go.transform.forward.Normalized();
        decalData.decalNormal    = go.transform.up.Normalized();
        /// @note 組み込み経路のアルベドアルファはコンポーネント側の値。.mat 経路では
        ///       材質が tint を持つため、ここはライフタイムフェードと opacity だけにする。
        decalData.alpha = entry.fade * std::clamp(d.opacity, 0.0f, 1.0f)
                        * (binding ? 1.0f : d.albedoColor[3]);
        decalData.angleFadeStrength = std::clamp(d.angleFadeStrength, 0.0f, 1.0f);
        /// @note 角度は度で持ち、シェーダーへは cos で渡す。ピクセルごとに acos を取るのは無駄。
        ///       90 度で cos=0 になり全ての面が残るため、89 度で上限を切って「必ず何かは消える」形にする。
        decalData.angleFadeCos = std::cos(
            std::clamp(d.angleFadeDegrees, 0.0f, 89.0f) * (3.14159265358979323846f / 180.0f));
        const bool filtered = receiverBufferReady &&
                              d.receiverLayerMask != fbzz::Layer::Everything;
        decalData.flags             = filtered ? kDecalFlagReceiverFilter : 0u;
        decalData.receiverLayerMask = d.receiverLayerMask;
        ResolveDecalFlipbook(d, decalData.frameScale, decalData.frameIndex);
        resources.Update(h.decalCB, &decalData, sizeof(DecalCB));

        renderer::DrawCall drawCall;
        drawCall.vertexCount         = 3;
        drawCall.constantBuffers[0]  = h.frameCB;
        drawCall.constantBuffers[3]  = h.lightCB;
        drawCall.constantBuffers[10] = h.decalCB;
        drawCall.textures[7]         = depthTex;
        if (filtered)
            drawCall.textures[13] = resources.GetColorTexture(h.decalMaskRT, 0);

        if (binding) {
            drawCall.shader             = binding->material.shader;
            drawCall.pipelineState      = binding->pso;
            drawCall.constantBuffers[2] = binding->material.paramsBuffer;
            for (size_t i = 0; i < binding->material.textures.size() && i < drawCall.textures.size(); ++i)
                if (binding->material.textures[i].IsValid())
                    drawCall.textures[i] = binding->material.textures[i];
            ApplyDecalMaterialOverrides(resources, *binding, d, drawCall);
        } else {
            auto loadTex = [&](const std::string& path) {
                return path.empty()
                    ? renderer::ResourceHandle<renderer::TextureTag>{}
                    : resources.LoadTexture(path);
            };
            const auto albedoTex   = loadTex(d.albedoTexPath);
            const auto normalTex   = loadTex(d.normalTexPath);
            const auto emissiveTex = loadTex(d.emissiveTexPath);

            DecalMaterialCB matData{};
            matData.albedoTint[0]    = d.albedoColor[0];
            matData.albedoTint[1]    = d.albedoColor[1];
            matData.albedoTint[2]    = d.albedoColor[2];
            matData.albedoTint[3]    = 1.0f;
            matData.emissiveColor[0] = d.emissiveColor[0];
            matData.emissiveColor[1] = d.emissiveColor[1];
            matData.emissiveColor[2] = d.emissiveColor[2];
            matData.emissiveScale    = d.emissiveScale;
            matData.normalStrength   = d.normalStrength;
            /// @note ビット位置はテクスチャスロット番号。renderer::Material::Upload と同じ規則に
            ///       揃えてあるので、.mat 経路と組み込み経路でシェーダーを共有できる。
            matData.textureMask = (albedoTex.IsValid()   ? 1u : 0u)
                                | (normalTex.IsValid()   ? 2u : 0u)
                                | (emissiveTex.IsValid() ? 8u : 0u);
            resources.Update(h.decalMaterialCB, &matData, sizeof(DecalMaterialCB));

            drawCall.shader             = h.decalShader;
            drawCall.pipelineState      = h.decalPSO;
            drawCall.constantBuffers[2] = h.decalMaterialCB;
            if (albedoTex.IsValid())   drawCall.textures[0] = albedoTex;
            if (normalTex.IsValid())   drawCall.textures[1] = normalTex;
            if (emissiveTex.IsValid()) drawCall.textures[3] = emissiveTex;
        }

        SubmitCounted(ctx, drawCall);
    }

    /// @note GameObjects() の走査中に即時削除すると iterator が無効化されるため、pass 後に破棄キューへ積む。
    for (EntityID id : expiredDecals) {
        ctx.scene.DestroyGameObject(id);
    }
}

void ExecuteDecalDepthCopyPass(RenderPassContext& ctx)
{
    auto& r         = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    r.SetRenderTarget(ctx.Res().Target("DecalDepth"), resources);
    r.ClearDepth();
    if (!h.depthCopyShader.IsValid()) return;

    renderer::DrawCall dc;
    dc.shader        = h.depthCopyShader;
    dc.pipelineState = h.defaultPSO;
    dc.vertexCount   = 3;
    dc.textures[7]   = ctx.isDeferred ? resources.GetDepthTexture(ctx.Res().Target("GBuffer"))
                                      : resources.GetDepthTexture(ctx.Res().Target("HDR"));
    r.Submit(dc, resources);
}


void DecalDepthCopyPass::Setup(PassBuilder& builder, const RenderPassContext& ctx) const
{
    builder.Read(ctx.isDeferred ? "GBuffer" : "HDR").Write("DecalDepth");
}

void DecalDepthCopyPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteDecalDepthCopyPass(ctx);
}

void DecalPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.Read("DecalDepth").ReadWrite("HDR");
}

void DecalPass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteDecalPass(ctx);
}
} // namespace fbzz::scene
