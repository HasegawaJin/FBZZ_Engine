/// @file    UIRenderResources.cpp
/// @brief   UI の GPU 資源、マテリアル、フォントアトラスの寿命とキャッシュを管理する。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "UIRendering.hpp"

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/MaterialParamBinding.hpp>
#include <Engine/Asset/StreamedTextureResolver.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/UITheme.hpp>
#include <cstring>

namespace fbzz::scene {
namespace ui_render_detail {
namespace {

template<typename Tag>
void ReleaseUIHandle(renderer::ResourceHandle<Tag>& handle)
{
    if (auto* owner = renderer::ResourceManager::FindOwner(handle.owner)) owner->Release(handle);
    handle = {};
}

void ReleaseMaterialCache(UISystemContext& context)
{
    for (auto& [key, binding] : context.materialCache) {
        ReleaseUIHandle(binding.overrideConstants);
        ReleaseUIHandle(binding.screenPso);
        ReleaseUIHandle(binding.worldPso);
    }
    context.materialCache.clear();
}

/// @brief キャッシュの作り直しが要る «外の変化» を拾う。
/// @note UISystemFlushCache を呼ぶべき瞬間 (デバイスロスト・アセットの再取り込み) を知って
/// @note いるのは呼び出し側 (Editor / Runtime) ではなく、版数を持つ ResourceManager と
/// @note AssetManager。呼び出し側に «忘れずに呼ぶ» を期待すると «UI の .mat を編集しても
/// @note 再起動まで反映されない» が長く残ったため、入口で版数を突き合わせて自分で捨てる。
void SyncResourceGenerations(UISystemContext& context, renderer::ResourceManager& resources)
{
    const std::uint64_t resetVersion    = resources.GetResetVersion();
    const std::uint64_t shaderVersion   = resources.GetShaderVersion();
    const int           assetGeneration = asset::AssetManager::GetAssetGeneration();

    if (context.cachedResourceOwner != resources.GetInstanceId() || context.cachedResetVersion != resetVersion) {
        UISystemReleaseGpuResources(context);
        context.cachedResourceOwner   = resources.GetInstanceId();
        context.cachedResetVersion    = resetVersion;
        context.cachedShaderVersion   = shaderVersion;
        context.cachedAssetGeneration = assetGeneration;
        return;
    }

    if (context.cachedAssetGeneration != assetGeneration || context.cachedShaderVersion != shaderVersion) {
        context.cachedAssetGeneration = assetGeneration;
        context.cachedShaderVersion = shaderVersion;
        /// @note マテリアルだけを捨てる。フォントアトラスは .ttf / .fnt を直接読んでおり
        /// @note AssetManager のストアに載っていない。どのアセットが変わっても版数は動くので、
        /// @note アトラスまで捨てると «無関係な .png を保存しただけ» で動的 SDF の焼き直しが
        /// @note 走る (体感できるほど止まる)。
        ReleaseMaterialCache(context);
    }
}

/// @name UISystemContext 初期化
void InitializeUIResources(UISystemContext& context, renderer::ResourceManager& resources)
{
    if (context.initialized) return;
    context.initialized = true;

    context.shader     = resources.LoadShader("Assets/shaders/UI/UISprite.hlsl");
    context.textShader = resources.LoadShader("Assets/shaders/UI/UIText.hlsl");
    context.constants  = resources.CreateConstantBuffer(sizeof(UIConstantsCB));
    context.pso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_OFF
    });
    /// @note WorldSpace / ScreenSpaceCamera UI: 深度テストあり・深度書き込みなし
    context.worldPso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::ALPHA_BLEND,
        renderer::DepthMode::DEPTH_READ
    });
    /// @note 選択マスク: 塗るのは被覆だけなので不透明。深度は書き込む (理由は UISystem.hpp を参照)。
    context.selectionMaskPso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });

    /// @note 共有の 1x1 白を借りる。ここで作ると、コンテキストの数だけ・デバイスリセットの
    /// @note 数だけ «誰も返さない 1 枚» が増える。
    context.whiteTexture = resources.GetWhiteTexture();

    if (!context.shader.IsValid() || !context.textShader.IsValid() || !context.constants.IsValid() ||
        !context.pso.IsValid() || !context.worldPso.IsValid() || !context.whiteTexture.IsValid()) {
        FBZZ_LOG_ERROR("UISystem init failed: shader=%d textShader=%d cb=%d pso=%d worldPso=%d white=%d",
                       static_cast<int>(context.shader.IsValid()),
                       static_cast<int>(context.textShader.IsValid()),
                       static_cast<int>(context.constants.IsValid()),
                       static_cast<int>(context.pso.IsValid()),
                       static_cast<int>(context.worldPso.IsValid()),
                       static_cast<int>(context.whiteTexture.IsValid()));
    }
}

} /// @note namespace

void PrepareUIResources(UISystemContext& context, renderer::ResourceManager& resources)
{
    SyncResourceGenerations(context, resources);
    InitializeUIResources(context, resources);
}

/// @name UI マテリアル解決
/// @note メッシュ側の SyncMaterial は MaterialComponent を入口に持つので、同じ入口に載せると
/// @note UIImage 全部へ MaterialComponent を要求することになる (UI に submesh は無く、
/// @note .mat を持たない要素が多数)。共有するのは入口ではなく「.mat をシェーダーへ束縛する
/// @note 規則」の方で、そちらは Engine/Asset/MaterialParamBinding.hpp にある。
UIMaterialBinding* ResolveUIMaterial(UISystemContext& context,
                                     renderer::ResourceManager& resources,
                                     const std::string& materialPath)
{
    if (materialPath.empty()) return nullptr;

    if (const auto cachedMaterial = context.materialCache.find(materialPath);
        cachedMaterial != context.materialCache.end()) {
        auto& cachedBinding = cachedMaterial->second;
        if (!cachedBinding.valid) return nullptr;
        for (std::size_t textureSlot = 0; textureSlot < cachedBinding.texturePaths.size(); ++textureSlot) {
            cachedBinding.material.textures[textureSlot] = asset::StreamedTextureResolver::Engine().ResolveGpu(
                resources, cachedBinding.texturePaths[textureSlot]);
        }
        /// @note 非同期完了・品質変更で texture mask と bindless 添字も変わりうる。
        cachedBinding.material.Upload(resources, cachedBinding.descriptor);
        return &cachedBinding;
    }

    /// @note 失敗も含めてキャッシュへ入れる。読めない .mat を毎フレーム開き直すと、
    /// @note 壊れた 1 件がフレーム時間を持っていくうえログが埋まる。
    UIMaterialBinding& binding = context.materialCache[materialPath];

    const auto assetHandle = asset::AssetManager::Load<asset::MaterialAsset>(materialPath);
    const auto authoringSnapshot = assetHandle.IsValid()
        ? asset::ReadMaterialAuthoring(assetHandle) : asset::MaterialAsset::AuthoringSnapshotPtr{};
    const auto* materialAsset = authoringSnapshot ? &authoringSnapshot->Data() : nullptr;
    if (!materialAsset) {
        FBZZ_LOG_WARN("UI material load failed '%s' -> falling back to the built-in sprite shader.",
                      materialPath.c_str());
        return nullptr;
    }

    /// @note メッシュ用の .mat は b0 を CameraConstants として読むが、UI パスはそこへ ortho を
    /// @note 入れる。割り当てると頂点が飛ぶか真っ黒になり、絵からは原因が分からない。
    if (materialAsset->renderPath != asset::RenderPath::UI) {
        FBZZ_LOG_WARN("UI material '%s' is not declared for UI (render_path must be \"ui\") "
                      "-> falling back to the built-in sprite shader.", materialPath.c_str());
        return nullptr;
    }
    if (materialAsset->shaderPath.empty()) {
        FBZZ_LOG_WARN("UI material '%s' has no shader -> falling back to the built-in sprite shader.",
                      materialPath.c_str());
        return nullptr;
    }

    renderer::Material& material = binding.material;
    material.shaderPath = materialAsset->shaderPath;
    material.shader     = resources.LoadShader(materialAsset->shaderPath);
    if (!material.shader.IsValid()) {
        FBZZ_LOG_WARN("UI material '%s' shader '%s' failed to load -> falling back.",
                      materialPath.c_str(), materialAsset->shaderPath.c_str());
        return nullptr;
    }

    /// @note 記述子はここで値ごと持つ。シェーダーは ResourceManager がホットリロードで
    /// @note 差し替えうるので、ポインタで持つと解決時の中身と食い違う瞬間ができる。
    if (auto* shader = resources.Get(material.shader))
        binding.descriptor = shader->GetDescriptor();

    material.paramData.assign(binding.descriptor.cbufferSize, 0u);
    if (binding.descriptor.IsValid()) {
        asset::InitDefaultMaterialParams(binding.descriptor, material.paramData);
        asset::ApplyMaterialAssetParams(*materialAsset, binding.descriptor, material.paramData);
    }

    const auto texturePaths = asset::ResolveMaterialTexturePaths(*materialAsset);
    binding.texturePaths.assign(texturePaths.begin(), texturePaths.end());
    material.textures.resize(texturePaths.size());
    for (std::size_t textureSlot = 0; textureSlot < texturePaths.size(); ++textureSlot) {
        material.textures[textureSlot] = texturePaths[textureSlot].empty()
            ? renderer::ResourceHandle<renderer::TextureTag>{}
            : asset::StreamedTextureResolver::Engine().ResolveGpu(resources, texturePaths[textureSlot]);
    }

    material.Init(resources, binding.descriptor.cbufferSize);
    material.Upload(resources, binding.descriptor);

    if (binding.descriptor.cbufferSize > 0) {
        /// @note 上書きを持つ要素だけが使う。持たない要素は共有の paramsBuffer を直接読む。
        /// @note 作業領域は解決時に確保しきる。描画中に伸ばすことは無い。
        binding.overrideScratch.resize(binding.descriptor.cbufferSize);
        binding.overrideConstants = resources.CreateConstantBuffer(binding.descriptor.cbufferSize);
    }

    /// @note UI は常に深度を書かない。ScreenSpace は深度テストもしない、
    /// @note WorldSpace は 3D の手前後関係に従う ─ 既定 PSO と同じ使い分けを blend だけ差し替える。
    binding.screenPso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL, materialAsset->blendMode, renderer::DepthMode::DEPTH_OFF });
    binding.worldPso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL, materialAsset->blendMode, renderer::DepthMode::DEPTH_READ });
    binding.valid    = true;
    return &binding;
}

void BindUIMaterial(const CanvasDrawContext& draw, const UIMaterialBinding& binding,
    renderer::DrawCall& drawCall, std::size_t firstTextureSlot)
{
    const renderer::Material& material = binding.material;
    drawCall.shader = material.shader;
    /// @note マテリアルの blend_mode を焼いた PSO が無い場合は、呼び出し側の PSO がフォールバックになる。
    const auto materialPipeline = draw.pipelineState == draw.context.worldPso ? binding.worldPso : binding.screenPso;
    if (materialPipeline.IsValid()) drawCall.pipelineState = materialPipeline;
    /// @note b2 は UICommon.hlsli の MaterialConstants。空のテクスチャスロットは既定の画像を保持する。
    drawCall.constantBuffers[2] = material.paramsBuffer;
    for (std::size_t slot = firstTextureSlot; slot < material.textures.size() && slot < drawCall.textures.size(); ++slot) {
        if (material.textures[slot].IsValid()) drawCall.textures[slot] = material.textures[slot];
    }
}

/// @note 要素ごとの上書きを DrawCall へ適用する。上書きが無ければ何もしない。
void ApplyUIMaterialOverrides(renderer::ResourceManager& resources,
                              UIMaterialBinding& binding,
                              const UIImage& image,
                              renderer::DrawCall& drawCall)
{
    if (!image.materialTextureOverrides.empty()) {
        const auto& slotNames = asset::kMaterialTextureSlotNames;
        for (size_t slot = 0; slot < slotNames.size() && slot < drawCall.textures.size(); ++slot) {
            const auto it = image.materialTextureOverrides.find(slotNames[slot]);
            if (it == image.materialTextureOverrides.end() || it->second.empty()) continue;
            if (const auto texture = asset::StreamedTextureResolver::Engine().ResolveGpu(resources, it->second); texture.IsValid())
                drawCall.textures[slot] = texture;
        }
    }

    if (image.materialParamOverrides.empty()) return;
    if (!binding.overrideConstants.IsValid()) return;
    if (binding.overrideScratch.size() != binding.material.paramData.size()) return;

    /// @note 共有マテリアルの値を土台に、この要素ぶんだけ重ねて別の cbuffer へ流す。
    /// @note 共有側の paramData は触らない (同じ .mat を使う他の要素へ波及する)。
    std::memcpy(binding.overrideScratch.data(),
                binding.material.paramData.data(),
                binding.material.paramData.size());
    asset::ApplyMaterialParamOverrides(image.materialParamOverrides,
                                       binding.descriptor, binding.overrideScratch);
    resources.Update(binding.overrideConstants,
                     binding.overrideScratch.data(),
                     static_cast<uint32_t>(binding.overrideScratch.size()));
    drawCall.constantBuffers[2] = binding.overrideConstants;
}

/// @name フォントアトラス
renderer::FontAtlas& GetOrLoadFontAtlas(const std::string& basePath,
                                        float rasterPixelHeight,
                                        UISystemContext& context,
                                        renderer::ResourceManager& resources)
{
    /// @note 静的アトラスは焼く解像度を持たない (PNG が決め打ち) ので、解像度をキーへ混ぜると
    /// @note 同じ PNG を段の数だけ読み込むだけになる。動的モードのときだけ分ける。
    const bool dynamicFont = renderer::FontAtlas::IsDynamicFontPath(basePath)
        || (basePath.starts_with("guid:")
            && renderer::FontAtlas::IsDynamicFontPath(ResolveUIFontBasePath(basePath)));
    const std::string key = dynamicFont
        ? basePath + '@' + std::to_string(static_cast<int>(rasterPixelHeight))
        : basePath;

    /// @note キャッシュは指定されたパスで引く。解決結果でキーを作ると、同じ指定が
    /// @note プロジェクト側と共有側で二重にロードされうる。
    const auto cachedAtlas = context.fontAtlasCache.find(key);
    if (cachedAtlas != context.fontAtlasCache.end())
        return cachedAtlas->second;

    renderer::FontAtlas& atlas = context.fontAtlasCache[key];
    const std::string resolved = ResolveUIFontBasePath(basePath);
    if (!atlas.Load(resolved, resources, rasterPixelHeight)) {
        FBZZ_LOG_ERROR("UISystem: failed to load FontAtlas [%s]\n  resolved: %s",
                       basePath.c_str(), resolved.c_str());
    }

    return atlas;
}

} /// @note namespace ui_render_detail

void UISystemReleaseGpuResources(UISystemContext& context, renderer::ResourceManager&)
{
    UISystemReleaseGpuResources(context);
}

void UISystemReleaseGpuResources(UISystemContext& context)
{
    context.vertexPool.ReleaseAll();
    ui_render_detail::ReleaseUIHandle(context.constants);
    ui_render_detail::ReleaseUIHandle(context.pso);
    ui_render_detail::ReleaseUIHandle(context.worldPso);
    ui_render_detail::ReleaseUIHandle(context.selectionMaskPso);
    ui_render_detail::ReleaseUIHandle(context.depthCopyConstants);
    ui_render_detail::ReleaseUIHandle(context.depthCopyPso);
    context.shader = {};
    context.textShader = {};
    context.depthCopyShader = {};
    context.whiteTexture = {};
    context.initialized = false;
    context.cachedResourceOwner = 0;
    context.cachedResetVersion = 0;
    UISystemFlushCache(context, nullptr);
}

bool UICopySceneDepth(renderer::IRenderer& renderer, renderer::ResourceManager& resources,
                      UISystemContext& context, renderer::ResourceHandle<renderer::TextureTag> sceneDepth,
                      bool reversedOutput)
{
    context.reversedDepth = reversedOutput;
    if (!sceneDepth.IsValid()) return false;
    ui_render_detail::SyncResourceGenerations(context, resources);
    if (!context.depthCopyShader.IsValid()) context.depthCopyShader = resources.LoadShader("Assets/Shaders/UI/UIDepthCopy.hlsl");
    if (!context.depthCopyConstants.IsValid()) context.depthCopyConstants = resources.CreateConstantBuffer(sizeof(math::Vector4));
    if (!context.depthCopyPso.IsValid()) context.depthCopyPso = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID_NOCULL, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON});
    if (!context.depthCopyShader.IsValid() || !context.depthCopyConstants.IsValid() || !context.depthCopyPso.IsValid()) return false;
    const math::Vector4 options{reversedOutput ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
    resources.Update(context.depthCopyConstants, &options, sizeof(options));
    renderer.ClearDepth();
    renderer::DrawCall drawCall;
    drawCall.shader = context.depthCopyShader;
    drawCall.pipelineState = context.depthCopyPso;
    drawCall.vertexCount = 3;
    drawCall.constantBuffers[0] = context.depthCopyConstants;
    drawCall.textures[7] = sceneDepth;
    renderer.Submit(drawCall, resources);
    return true;
}


void UISystemFlushCache(UISystemContext& context, renderer::ResourceManager*)
{
    ui_render_detail::ReleaseMaterialCache(context);
    /// @note アトラスのページも記録済みの作成元へ返す。Reset 後の古い世代は資源プールが拒否する。
    context.fontAtlasCache.clear();
    /// @note 子リストの作業領域は破棄済み GameObject を指したままになりうる。
    /// @note 中身だけ捨てて、確保済みの容量は次のシーンでそのまま使い回す。
    for (auto& children : context.childScratch) children.clear();
}

} /// @note namespace fbzz::scene
