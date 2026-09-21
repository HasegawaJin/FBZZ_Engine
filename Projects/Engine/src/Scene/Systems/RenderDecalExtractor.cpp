/// @file    RenderDecalExtractor.cpp
/// @brief   Deferred デカールパス
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// @note 各 DecalComponent に対してフルスクリーントライアングルを 1 draw 発行する。
/// @note PS が深度バッファからワールド座標を復元し、デカール OBB 外のフラグメントを
/// @note discard することで表面への投影を実現する。
///
/// @note マテリアルは 2 経路ある:
/// @note materialPath 空  → 組み込み Decal.hlsl + コンポーネントの色・テクスチャ
/// @note materialPath 有り → .mat (render_path = "decal") のシェーダーとパラメータ
/// @note どちらの経路でも投影ボリューム・角度フェード・ライフタイムは b10 の
/// @note DecalConstants がエンジン側から埋める (DecalCommon.hlsli 参照)。
#include "RenderPasses/Geometry/GeometryPasses.hpp"
#include <Engine/Scene/Systems/RenderDecalExtractor.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/StreamedTextureResolver.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/MaterialParamBinding.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Renderer/Material.hpp>
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
    /// @note 上書きを名前でバイトオフセットへ写像するのに要る。
    renderer::ShaderDescriptor                            descriptor;
    renderer::ResourceHandle<renderer::PipelineStateTag>  pso;
    /// @note PSO を作り直す判断に使う。CreatePipelineState は重複を畳まないため、
    /// @note 毎フレーム呼ぶとステートオブジェクトが際限なく増える。
    renderer::BlendMode                                   psoBlend = renderer::BlendMode::ALPHA_BLEND;
    bool                                                  psoValid = false;
    /// @note 上書きを持つデカールだけが通す一時 cbuffer。
    /// @note マテリアルごとに 1 本で足りる理由: Update は Upload Arena のスライスを切るため、同じハンドルへ書いて Submit を繰り返しても記録済みの Draw はそれぞれ自分の書き込み時点の中身を読む。
    renderer::ResourceHandle<renderer::ConstantBufferTag> overrideConstants;
    std::vector<uint8_t>                                  overrideScratch;
    /// @note このパス呼び出しで既に値を適用したか。弾痕は同じ .mat を数十個が共有するので、
    /// @note デカールごとにリフレクション適用と Upload をやり直すと数だけ無駄が増える。
    uint64_t                                              resolvedPass = 0;
    bool                                                  resolvedOk   = false;
};

/// @note .mat のパスで引く。解決はシェーダーのロードとリフレクションを伴うので毎フレーム
/// @note やる値段ではない。値の適用はパスごとに 1 回だけやり直し、.mat の編集を絵へ出す。
std::unordered_map<std::string, DecalMaterialBinding> g_decalMaterials;
/// @note 同じ .mat の失敗を毎フレーム記録するとログが埋まる。
std::unordered_set<std::string>                       g_warnedDecalMaterials;
/// @note ExecuteDecalPass の呼び出し通番。0 は「未解決」を表すため 1 から始める。
uint64_t                                              g_decalPassSerial = 0;

bool WarnDecalMaterialOnce(const std::string& path)
{
    return g_warnedDecalMaterials.insert(path).second;
}

/// @note materialPath から描画に要るものを揃える。解決できない場合は組み込み経路へ落とす。
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
    /// @note ポインタで持つと解決時の中身と食い違う瞬間ができる。
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
            : asset::StreamedTextureResolver::Engine().ResolveGpu(resources, texturePaths[i]);
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

/// @note 共有マテリアルの上へこのデカールだけの上書きを重ねる。上書きが無ければ何もしない。
void ApplyDecalMaterialOverrides(renderer::ResourceManager& resources,
                                 DecalMaterialBinding& binding,
                                 const DecalComponent& decal,
                                 renderer::DrawCall& call, std::vector<uint8_t>& parameters)
{
    if (!decal.materialTextureOverrides.empty()) {
        const auto& slotNames = asset::kMaterialTextureSlotNames;
        for (size_t slot = 0; slot < slotNames.size() && slot < call.textures.size(); ++slot) {
            const auto it = decal.materialTextureOverrides.find(slotNames[slot]);
            if (it == decal.materialTextureOverrides.end() || it->second.empty()) continue;
            if (const auto texture = asset::StreamedTextureResolver::Engine().ResolveGpu(resources, it->second);
                texture.IsValid())
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

/// @note アトラスの 1 コマぶんの UV スケールと、今のコマ番号を求める。
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
    /// @note 終わるのが正しく、lifetime を触るたびに fps を計算し直させたくない。
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
}
void ReleaseDecalMaterialCache()
{
    g_decalMaterials.clear();
    g_warnedDecalMaterials.clear();
}
void ExtractRenderDecals(RenderPassContext& ctx, renderer::RenderScene& output) {
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    ++g_decalPassSerial;
    const float dt = Time::deltaTime;
    std::vector<EntityID> expiredDecals;
    for (auto& go : ctx.scene.GameObjects()) {
        if (!go.activeInHierarchy()) continue;

        auto* decal = go.GetComponent<DecalComponent>();
        if (!decal || !decal->enabled) continue;

        if (decal->lastExtractionFrame != resources.FrameStamp()) {
            decal->lastExtractionFrame = resources.FrameStamp();
            if (DecalNeedsAge(*decal)) decal->age += dt;
        }
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
        const auto& d = *decal;
        auto* binding = ResolveDecalMaterial(resources, d);
        renderer::RenderDecalInput input;
        input.layer = static_cast<uint32_t>(go.layer);
        input.sortOrder = d.sortOrder;
        auto& decalData = input.projection;
        decalData.invDecalWorld  = math::Matrix4::Inverse(go.transform.GetWorldMatrix());
        decalData.decalTangent   = go.transform.right.Normalized();
        decalData.decalBitangent = go.transform.forward.Normalized();
        decalData.decalNormal    = go.transform.up.Normalized();
        /// @note 組み込み経路のアルベドアルファはコンポーネント側の値。.mat 経路では
        /// @note 材質が tint を持つため、ここはライフタイムフェードと opacity だけにする。
        decalData.alpha = fade * std::clamp(d.opacity, 0.0f, 1.0f)
                        * (binding ? 1.0f : d.albedoColor[3]);
        decalData.angleFadeStrength = std::clamp(d.angleFadeStrength, 0.0f, 1.0f);
        /// @note 角度は度で持ち、シェーダーへは cos で渡す。ピクセルごとに acos を取るのは無駄。
        /// @note 90 度で cos=0 になり全ての面が残るため、89 度で上限を切って「必ず何かは消える」形にする。
        decalData.angleFadeCos = std::cos(
            std::clamp(d.angleFadeDegrees, 0.0f, 89.0f) * (3.14159265358979323846f / 180.0f));
        decalData.receiverLayerMask = d.receiverLayerMask;
        ResolveDecalFlipbook(d, decalData.frameScale, decalData.frameIndex);
        auto& drawCall = input.binding;
        if (binding) {
            drawCall.shader             = binding->material.shader;
            drawCall.pipelineState      = binding->pso;
            drawCall.constantBuffers[2] = binding->material.paramsBuffer;
            for (size_t i = 0; i < binding->material.textures.size() && i < drawCall.textures.size(); ++i)
                if (binding->material.textures[i].IsValid())
                    drawCall.textures[i] = binding->material.textures[i];
            input.customMaterial = true;
            ApplyDecalMaterialOverrides(resources, *binding, d, drawCall, input.materialParameters);
        } else {
            auto loadTex = [&](const std::string& path) {
                return path.empty()
                    ? renderer::ResourceHandle<renderer::TextureTag>{}
                    : asset::StreamedTextureResolver::Engine().ResolveGpu(resources, path);
            };
            const auto albedoTex   = loadTex(d.albedoTexPath);
            const auto normalTex   = loadTex(d.normalTexPath);
            const auto emissiveTex = loadTex(d.emissiveTexPath);

            auto& matData = input.material;
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
            /// @note 揃えてあるので、.mat 経路と組み込み経路でシェーダーを共有できる。
            matData.textureMask = (albedoTex.IsValid()   ? 1u : 0u)
                                | (normalTex.IsValid()   ? 2u : 0u)
                                | (emissiveTex.IsValid() ? 8u : 0u);


            drawCall.shader             = h.decalShader;
            drawCall.pipelineState      = h.decalPSO;
            drawCall.constantBuffers[2] = h.decalMaterialCB;
            if (albedoTex.IsValid())   drawCall.textures[0] = albedoTex;
            if (normalTex.IsValid())   drawCall.textures[1] = normalTex;
            if (emissiveTex.IsValid()) drawCall.textures[3] = emissiveTex;
        }
        output.decals.push_back(std::move(input));
    }
    for (EntityID id : expiredDecals) {
        ctx.scene.DestroyGameObject(id);
    }
}
}
