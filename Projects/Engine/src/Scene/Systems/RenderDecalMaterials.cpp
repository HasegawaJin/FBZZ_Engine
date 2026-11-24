/// @file    RenderDecalMaterials.cpp
/// @brief   デカールの組み込み材質とカスタムシェーダーの資源管理。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "RenderDecalMaterials.hpp"
#include <Engine/Scene/Systems/RenderDecalExtractor.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/MaterialParamBinding.hpp>
#include <Engine/Asset/StreamedTextureResolver.hpp>
#include <Engine/Core/Logger.hpp>
#include <Graphics/Renderer/Material.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::scene::decal_extraction_detail {

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

namespace {

/// @note .mat のパスで引く。解決はシェーダーのロードとリフレクションを伴うので毎フレーム
/// @note やる値段ではない。値の適用はパスごとに 1 回だけやり直し、.mat の編集を絵へ出す。
std::unordered_map<std::string, DecalMaterialBinding> s_decalMaterials;
/// @note 同じ .mat の失敗を毎フレーム記録するとログが埋まる。
std::unordered_set<std::string>                       s_warnedDecalMaterials;
/// @note ExecuteDecalPass の呼び出し通番。0 は「未解決」を表すため 1 から始める。
uint64_t                                              s_decalPassSerial = 0;

bool WarnDecalMaterialOnce(const std::string& path)
{
    return s_warnedDecalMaterials.insert(path).second;
}


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

} /// @note namespace

/// @note materialPath から描画に要るものを揃える。解決できない場合は組み込み経路へ落とす。
DecalMaterialBinding* ResolveDecalMaterial(renderer::ResourceManager& resources,
                                           const DecalComponent& decal)
{
    if (decal.materialPath.empty()) return nullptr;

    const auto assetHandle = asset::AssetManager::Load<asset::MaterialAsset>(decal.materialPath);
    const auto authoringSnapshot = assetHandle.IsValid()
        ? asset::ReadMaterialAuthoring(assetHandle) : asset::MaterialAsset::AuthoringSnapshotPtr{};
    const auto* authoring = authoringSnapshot ? &authoringSnapshot->Data() : nullptr;
    if (!authoring) {
        if (WarnDecalMaterialOnce(decal.materialPath))
            FBZZ_LOG_WARN("Decal material load failed '%s' -> falling back to the built-in decal shader.",
                          decal.materialPath.c_str());
        return nullptr;
    }
    /// @note 用途を検査する理由: メッシュ用の .mat は頂点入力を前提にしたシェーダーを指すが、デカールは頂点バッファを持たない SV_VertexID 描画のため、割り当てると入力レイアウト不一致で何も出ないか画面が塗り潰される。原因が絵からは分からない事故のため名指しで止める。
    if (authoring->renderPath != asset::RenderPath::Decal) {
        if (WarnDecalMaterialOnce(decal.materialPath))
            FBZZ_LOG_WARN("Decal material '%s' is not declared for decals (render_path must be \"decal\") "
                          "-> falling back to the built-in decal shader.", decal.materialPath.c_str());
        return nullptr;
    }
    if (authoring->shaderPath.empty()) {
        if (WarnDecalMaterialOnce(decal.materialPath))
            FBZZ_LOG_WARN("Decal material '%s' has no shader -> falling back to the built-in decal shader.",
                          decal.materialPath.c_str());
        return nullptr;
    }

    DecalMaterialBinding& binding = s_decalMaterials[decal.materialPath];
    if (binding.resolvedPass == s_decalPassSerial)
        return binding.resolvedOk ? &binding : nullptr;
    binding.resolvedPass = s_decalPassSerial;
    binding.resolvedOk   = false;

    renderer::Material& material  = binding.material;
    material.shaderPath = authoring->shaderPath;
    material.shader     = resources.LoadShader(authoring->shaderPath);
    if (!material.shader.IsValid()) {
        if (WarnDecalMaterialOnce(decal.materialPath))
            FBZZ_LOG_WARN("Decal material '%s' shader '%s' failed to load -> falling back.",
                          decal.materialPath.c_str(), authoring->shaderPath.c_str());
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
        asset::ApplyMaterialAssetParams(*authoring, binding.descriptor, material.paramData);
    }

    const auto texturePaths = asset::ResolveMaterialTexturePaths(*authoring);
    material.textures.resize(texturePaths.size());
    for (size_t textureSlot = 0; textureSlot < texturePaths.size(); ++textureSlot) {
        material.textures[textureSlot] = texturePaths[textureSlot].empty()
            ? renderer::ResourceHandle<renderer::TextureTag>{}
            : asset::StreamedTextureResolver::Engine().ResolveGpu(resources, texturePaths[textureSlot]);
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
    if (!binding.psoValid || binding.psoBlend != authoring->blendMode) {
        binding.pso = resources.CreatePipelineState({
            renderer::RasterizerMode::SOLID, authoring->blendMode, renderer::DepthMode::DEPTH_OFF });
        binding.psoBlend = authoring->blendMode;
        binding.psoValid = binding.pso.IsValid();
    }
    if (!binding.psoValid) return nullptr;

    binding.resolvedOk = true;
    return &binding;
}

/// @note 共有マテリアルの上へこのデカールだけの上書きを重ねる。上書きが無ければ何もしない。

void BindCustomDecalMaterial(renderer::ResourceManager& resources, DecalMaterialBinding& binding,
    const DecalComponent& decal, renderer::RenderDecalInput& input)
{
    auto& drawCall = input.binding;
    drawCall.shader             = binding.material.shader;
    drawCall.pipelineState      = binding.pso;
    drawCall.constantBuffers[2] = binding.material.paramsBuffer;
    for (size_t textureSlot = 0; textureSlot < binding.material.textures.size() && textureSlot < drawCall.textures.size(); ++textureSlot)
        if (binding.material.textures[textureSlot].IsValid())
            drawCall.textures[textureSlot] = binding.material.textures[textureSlot];
    input.customMaterial = true;
    ApplyDecalMaterialOverrides(resources, binding, decal, drawCall, input.materialParameters);
}

void BindBuiltInDecalMaterial(renderer::ResourceManager& resources, const RenderPassHandles& handles,
    const DecalComponent& decal, renderer::RenderDecalInput& input)
{
    auto& drawCall = input.binding;
    auto resolveTexture = [&](const std::string& path) {
        return path.empty()
            ? renderer::ResourceHandle<renderer::TextureTag>{}
            : asset::StreamedTextureResolver::Engine().ResolveGpu(resources, path);
    };
    const auto albedoTexture   = resolveTexture(decal.albedoTexPath);
    const auto normalTexture   = resolveTexture(decal.normalTexPath);
    const auto emissiveTexture = resolveTexture(decal.emissiveTexPath);

    auto& materialConstants = input.material;
    materialConstants.albedoTint[0]    = decal.albedoColor[0];
    materialConstants.albedoTint[1]    = decal.albedoColor[1];
    materialConstants.albedoTint[2]    = decal.albedoColor[2];
    materialConstants.albedoTint[3]    = 1.0f;
    materialConstants.emissiveColor[0] = decal.emissiveColor[0];
    materialConstants.emissiveColor[1] = decal.emissiveColor[1];
    materialConstants.emissiveColor[2] = decal.emissiveColor[2];
    materialConstants.emissiveScale    = decal.emissiveScale;
    materialConstants.normalStrength   = decal.normalStrength;
    /// @note ビット位置はテクスチャスロット番号。renderer::Material::Upload と同じ規則に
    /// @note 揃えてあるので、.mat 経路と組み込み経路でシェーダーを共有できる。
    materialConstants.textureMask = (albedoTexture.IsValid()   ? 1u : 0u)
                        | (normalTexture.IsValid()   ? 2u : 0u)
                        | (emissiveTexture.IsValid() ? 8u : 0u);


    drawCall.shader             = handles.decalShader;
    drawCall.pipelineState      = handles.decalPSO;
    drawCall.constantBuffers[2] = handles.decalMaterialCB;
    if (albedoTexture.IsValid())   drawCall.textures[0] = albedoTexture;
    if (normalTexture.IsValid())   drawCall.textures[1] = normalTexture;
    if (emissiveTexture.IsValid()) drawCall.textures[3] = emissiveTexture;
}

void BeginDecalMaterialPass()
{
    ++s_decalPassSerial;
}

void ClearDecalMaterialCache()
{
    s_decalMaterials.clear();
    s_warnedDecalMaterials.clear();
}

} /// @note namespace fbzz::scene::decal_extraction_detail

namespace fbzz::scene {

void ReleaseDecalMaterialCache()
{
    decal_extraction_detail::ClearDecalMaterialCache();
}

} /// @note namespace fbzz::scene
