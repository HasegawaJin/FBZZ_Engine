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
#include "RenderPasses/PostProcess/PostProcessPasses.hpp"
#include <Engine/Scene/Systems/RenderCustomPostExtractor.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/StreamedTextureResolver.hpp>
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

/// @name .mat の解決
/// @note 解決はシェーダーのロードとリフレクションを伴うので毎ドローやる値段ではない。
/// @note フレームに 1 度だけやり直して、.mat の編集をその場で絵へ出す (DecalPass と同じ作り)。
struct CustomMaterialBinding {
    renderer::Material         material;
    renderer::ShaderDescriptor descriptor;
    renderer::BlendMode        blendMode = renderer::BlendMode::OPAQUE_BLEND;
    uint64_t                   resolvedFrame = UINT64_MAX;
    bool                       ok = false;
};

std::unordered_map<std::string, CustomMaterialBinding> g_customMaterials;
std::unordered_set<std::string>                        g_warnedCustomMaterials;

bool WarnCustomMaterialOnce(const std::string& path)
{
    return g_warnedCustomMaterials.insert(path).second;
}

/// @note materialPath から «シェーダー + テクスチャ + b2» を揃える。解決できなければ nullptr
/// @note (呼び出し側は shaderPath の経路へ落ちる)。
CustomMaterialBinding* ResolveCustomMaterial(renderer::ResourceManager& resources,
                                             const std::string& path)
{
    if (path.empty()) return nullptr;

    const auto assetHandle = asset::AssetManager::Load<asset::MaterialAsset>(path);
    const auto* matAsset = assetHandle.IsValid()
        ? asset::AssetManager::Get<asset::MaterialAsset>(assetHandle) : nullptr;
    if (!matAsset) {
        if (WarnCustomMaterialOnce(path))
            FBZZ_LOG_WARN("Custom pass material load failed '%s' -> falling back to shaderPath.",
                          path.c_str());
        return nullptr;
    }
    /// @note 用途を検査するのは、メッシュ用の .mat が頂点入力を前提にしたシェーダーを指す
    /// @note ため。カスタムパスは頂点バッファを持たない SV_VertexID 描画なので、割り当てると
    /// @note 入力レイアウト不一致で何も出ないか画面が塗り潰され、絵からは原因が読めない。
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
    if (binding.resolvedFrame == resources.FrameStamp())
        return binding.ok ? &binding : nullptr;
    binding.resolvedFrame = resources.FrameStamp();
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

    binding.blendMode = matAsset->blendMode;
    binding.ok        = true;
    return &binding;
}

}
void ExtractRenderCustomPost(RenderPassContext& ctx, renderer::RenderScene& output)
{
    auto& inputs = output.customPost;
    const auto& effects = ctx.settings.postProcess.customEffects;
    inputs.resize(effects.size());
    for (size_t i = 0; i < effects.size(); ++i) {
        const auto& effect = effects[i];
        if (!effect.enabled) continue;
        auto& input = inputs[i];
        input.blendMode = effect.blendMode;
        if (auto* binding = ResolveCustomMaterial(ctx.resources, effect.materialPath)) {
            input.shader = binding->material.shader;
            input.paramsBuffer = binding->material.paramsBuffer;
            input.parameters = binding->material.paramData;
            input.blendMode = binding->blendMode;
            for (size_t j = 0; j < input.textures.size() && j < binding->material.textures.size(); ++j)
                input.textures[j] = binding->material.textures[j];
        } else if (i < ctx.handles.customPostProcessShaders.size()) {
            input.shader = ctx.handles.customPostProcessShaders[i];
        }
    }
}
void ReleaseCustomPassMaterialCache()
{
    g_customMaterials.clear();
    g_warnedCustomMaterials.clear();
}
}
