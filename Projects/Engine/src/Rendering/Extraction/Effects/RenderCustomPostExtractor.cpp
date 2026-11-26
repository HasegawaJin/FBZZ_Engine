/// @file    RenderCustomPostExtractor.cpp
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
#include "../../Passes/PostProcess/PostProcessPasses.hpp"
#include <Engine/Rendering/Extraction/Effects/RenderCustomPostExtractor.hpp>
#include <Graphics/Scene/RenderScene.hpp>
#include <Engine/Rendering/Passes/RenderPassContext.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Streaming/StreamedTextureResolver.hpp>
#include <Engine/Asset/Material/MaterialAsset.hpp>
#include <Engine/Asset/Material/MaterialParamBinding.hpp>
#include <Engine/Core/Diagnostics/Logger.hpp>
#include <Engine/Core/Timing/Time.hpp>
#include <Graphics/Renderer/Commands/DrawCall.hpp>
#include <Graphics/Renderer/Materials/Material.hpp>
#include <Graphics/Renderer/State/RenderState.hpp>
#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace fbzz::scene {

namespace {

/// @name .mat の解決
/// @note 解決はシェーダーのロードとリフレクションを伴うので毎ドローやる値段ではない。
/// @note フレームに 1 度だけやり直して、.mat の編集をその場で絵へ出す (DecalPass と同じ作り)。
struct CustomMaterialBinding {
    renderer::Material         material;
    renderer::ShaderDescriptor descriptor;
    renderer::BlendMode        blendMode = renderer::BlendMode::OPAQUE_BLEND;
    uint64_t                   resolvedFrame = UINT64_MAX;
    bool                       resolved = false;
};

class CustomPostMaterialCache {
public:
    CustomMaterialBinding* Resolve(renderer::ResourceManager& resources, const std::string& path);
    void Clear();

private:
    bool WarnOnce(const std::string& path);
    std::unordered_map<std::string, CustomMaterialBinding> m_materials;
    std::unordered_set<std::string> m_warnedMaterials;
};

CustomPostMaterialCache s_materials;

bool CustomPostMaterialCache::WarnOnce(const std::string& path)
{
    return m_warnedMaterials.insert(path).second;
}

/// @note materialPath から «シェーダー + テクスチャ + b2» を揃える。解決できなければ nullptr
/// @note (呼び出し側は shaderPath の経路へ落ちる)。
CustomMaterialBinding* CustomPostMaterialCache::Resolve(renderer::ResourceManager& resources,
                                             const std::string& path)
{
    if (path.empty()) {
        return nullptr;
    }

    const auto assetHandle = asset::AssetManager::Load<asset::MaterialAsset>(path);
    const auto authoringSnapshot = assetHandle.IsValid()
        ? asset::ReadMaterialAuthoring(assetHandle) : asset::MaterialAsset::AuthoringSnapshotPtr{};
    const auto* authoring = authoringSnapshot ? &authoringSnapshot->Data() : nullptr;
    if (!authoring) {
        if (WarnOnce(path))
            FBZZ_LOG_WARN("Custom pass material load failed '%s' -> falling back to shaderPath.",
                          path.c_str());
        return nullptr;
    }
    // 用途を検査するのは、メッシュ用の .mat が頂点入力を前提にしたシェーダーを指す
    // ため。カスタムパスは頂点バッファを持たない SV_VertexID 描画なので、割り当てると
    // 入力レイアウト不一致で何も出ないか画面が塗り潰され、絵からは原因が読めない。
    if (authoring->renderPath != asset::RenderPath::PostProcess) {
        if (WarnOnce(path))
            FBZZ_LOG_WARN("Custom pass material '%s' is not declared for post process "
                          "(render_path must be \"post_process\") -> falling back to shaderPath.",
                          path.c_str());
        return nullptr;
    }
    if (authoring->shaderPath.empty()) {
        if (WarnOnce(path))
            FBZZ_LOG_WARN("Custom pass material '%s' has no shader -> falling back to shaderPath.",
                          path.c_str());
        return nullptr;
    }

    CustomMaterialBinding& binding = m_materials[path];
    if (binding.resolvedFrame == resources.FrameStamp()) {
        return binding.resolved ? &binding : nullptr;
    }
    binding.resolvedFrame = resources.FrameStamp();
    binding.resolved            = false;

    renderer::Material& material = binding.material;
    material.shaderPath = authoring->shaderPath;
    material.shader     = resources.LoadShader(authoring->shaderPath);
    if (!material.shader.IsValid()) {
        if (WarnOnce(path))
            FBZZ_LOG_WARN("Custom pass material '%s' shader '%s' failed to load -> falling back.",
                          path.c_str(), authoring->shaderPath.c_str());
        return nullptr;
    }

    // 記述子は値ごと持つ。シェーダーはホットリロードで差し替わりうるので、
    // ポインタで持つと解決時の中身と食い違う瞬間ができる。
    binding.descriptor = {};
    if (auto* shader = resources.Get(material.shader)) {
        binding.descriptor = shader->GetDescriptor();
    }

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

    binding.blendMode = authoring->blendMode;
    binding.resolved        = true;
    return &binding;
}

void CustomPostMaterialCache::Clear()
{
    m_materials.clear();
    m_warnedMaterials.clear();
}

void BindCustomPostMaterial(renderer::RenderCustomPostInput& input, const CustomMaterialBinding& binding)
{
    input.shader = binding.material.shader;
    input.paramsBuffer = binding.material.paramsBuffer;
    input.parameters = binding.material.paramData;
    input.blendMode = binding.blendMode;
    for (size_t textureSlot = 0; textureSlot < input.textures.size() && textureSlot < binding.material.textures.size(); ++textureSlot) {
        input.textures[textureSlot] = binding.material.textures[textureSlot];
    }
}

} // namespace

void ExtractRenderCustomPost(RenderPassContext& context, renderer::RenderScene& output)
{
    auto& inputs = output.customPost;
    const auto& effects = context.settings.postProcess.customEffects;
    inputs.resize(effects.size());
    for (size_t effectIndex = 0; effectIndex < effects.size(); ++effectIndex) {
        const auto& effect = effects[effectIndex];
        if (!effect.enabled) {
            continue;
        }
        auto& input = inputs[effectIndex];
        input.blendMode = effect.blendMode;
        if (auto* binding = s_materials.Resolve(context.resources, effect.materialPath)) {
            BindCustomPostMaterial(input, *binding);
        } else if (effectIndex < context.handles.customPostProcessShaders.size()) {
            input.shader = context.handles.customPostProcessShaders[effectIndex];
        }
    }
}

void ReleaseCustomPassMaterialCache()
{
    s_materials.Clear();
}

} // namespace fbzz::scene
