// FBZZ Engine
// RenderPasses/GeometryPassHelpers.cpp | fbzz::scene
// ジオメトリパス共有ヘルパー関数
#include "GeometryPasses.hpp"
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Asset/MaterialAsset.hpp"
#include "Engine/Asset/TexDescSerializer.hpp"
#include "Engine/Asset/TextureAsset.hpp"
#include "Engine/Core/Logger.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Components/AnimatorComponent.hpp"
#include "Engine/Scene/Components/MaterialComponent.hpp"
#include "Engine/Scene/Components/SkinnedMeshRenderer.hpp"
#include "Engine/Scene/Components/WindZoneComponent.hpp"
#include "Engine/Renderer/Material.hpp"
#include "Engine/Renderer/IShader.hpp"
#include "Engine/Renderer/ITexture.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include "Engine/Util/FileSystem.hpp"
#include <Math/MathUtils.hpp>
#include <Math/Matrix4.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace fbzz::scene {

namespace {

// t0-t4: 標準 PBR スロット。t5-t7: カスタムシェーダー用汎用スロット。
// t8 = TEX_SHADOW はエンジン側で予約済みのため除外する。
constexpr std::array<const char*, 8> kTextureSlotNames = {
    "albedo",
    "normal",
    "metallic",
    "emissive",
    "ao",
    "tex5",
    "tex6",
    "tex7",
};

const std::vector<float>* FindMaterialParam(const asset::MaterialAsset& asset, std::string_view shaderVarName)
{
    auto it = asset.params.find(std::string(shaderVarName));
    if (it != asset.params.end()) return &it->second;

    // snake_case 別名 (手書き .mat 向け)
    if (shaderVarName == "normalStrength") it = asset.params.find("normal_strength");
    else if (shaderVarName == "emissiveColor")  it = asset.params.find("emissive_color");
    else if (shaderVarName == "emissiveScale")  it = asset.params.find("emissive_scale");
    else if (shaderVarName == "clearcoatRoughness") it = asset.params.find("clearcoat_roughness");
    else if (shaderVarName == "sheenColor") it = asset.params.find("sheen_color");

    return it != asset.params.end() ? &it->second : nullptr;
}

void ApplyMaterialAssetParams(const asset::MaterialAsset& asset,
                              const renderer::ShaderDescriptor& desc,
                              std::vector<uint8_t>& paramData)
{
    for (const auto& v : desc.vars) {
        if (v.varType != renderer::ShaderVarType::Float) continue;
        if (v.offset + v.size > static_cast<uint32_t>(paramData.size())) continue;

        const auto* values = FindMaterialParam(asset, v.name);
        if (!values || values->empty()) continue;

        const size_t count = (std::min<size_t>)(v.columns, values->size());
        std::memcpy(paramData.data() + v.offset, values->data(), count * sizeof(float));
    }
}

// MaterialComponent::paramOverrides を共有アセット適用後の paramData へ「この GO 専用」で重ねる。
// WHY: 同じ .mat を共有する複数インスタンスでも、ディゾルブ量・色などを個別に動かせるようにする。
void ApplyMaterialParamOverrides(
    const std::unordered_map<std::string, std::vector<float>>& overrides,
    const renderer::ShaderDescriptor& desc,
    std::vector<uint8_t>& paramData)
{
    for (const auto& [name, values] : overrides) {
        if (values.empty()) continue;
        const auto* v = desc.FindVar(name);
        if (!v || v->varType != renderer::ShaderVarType::Float) continue;
        if (v->offset + v->size > static_cast<uint32_t>(paramData.size())) continue;
        const size_t count = (std::min<size_t>)(v->columns, values.size());
        std::memcpy(paramData.data() + v->offset, values.data(), count * sizeof(float));
    }
}

void InitDefaultMaterialParams(const renderer::ShaderDescriptor& desc, std::vector<uint8_t>& paramData)
{
    // Step 1: シェーダーの全 float 変数を 1.0f で初期化する。
    // WHY: 未知のカスタムパラメータが 0 のままだと乗算スケール系変数が非表示になる。
    //      1.0f は乗算の単位元であり、加算オフセット (uvOffset 等) は次ステップで 0 に上書きされる。
    const float one = 1.0f;
    for (const auto& v : desc.vars) {
        if (v.varType != renderer::ShaderVarType::Float) continue;
        for (uint32_t col = 0; col < v.columns; ++col) {
            const uint32_t byteOff = v.offset + col * sizeof(float);
            if (byteOff + sizeof(float) <= static_cast<uint32_t>(paramData.size()))
                std::memcpy(paramData.data() + byteOff, &one, sizeof(float));
        }
    }

    // Step 2: 標準 PBR パラメータを正しいデフォルト値で上書きする。
    auto setFloat = [&](std::string_view name, float value) {
        const auto* v = desc.FindVar(name);
        if (!v || v->varType != renderer::ShaderVarType::Float || v->columns != 1) return;
        if (v->offset + sizeof(float) > static_cast<uint32_t>(paramData.size())) return;
        std::memcpy(paramData.data() + v->offset, &value, sizeof(float));
    };
    auto setFloat2 = [&](std::string_view name, const float value[2]) {
        const auto* v = desc.FindVar(name);
        if (!v || v->varType != renderer::ShaderVarType::Float || v->columns < 2) return;
        if (v->offset + 2u * sizeof(float) > static_cast<uint32_t>(paramData.size())) return;
        std::memcpy(paramData.data() + v->offset, value, 2u * sizeof(float));
    };
    auto setFloat3 = [&](std::string_view name, const float value[3]) {
        const auto* v = desc.FindVar(name);
        if (!v || v->varType != renderer::ShaderVarType::Float || v->columns < 3) return;
        if (v->offset + 3u * sizeof(float) > static_cast<uint32_t>(paramData.size())) return;
        std::memcpy(paramData.data() + v->offset, value, 3u * sizeof(float));
    };

    const float uvTiling[2] = { 1.0f, 1.0f };
    const float uvOffset[2] = { 0.0f, 0.0f };

    const float white3[3] = { 1.0f, 1.0f, 1.0f };
    setFloat("metallic",       0.0f);
    setFloat("roughness",      0.65f);
    // 拡張 PBR ローブは既定で無効にする。1.0f のままだと既存マテリアルの見た目と
    // エネルギー配分が変わるため、明示的に有効化された場合だけ Forward へ送る。
    setFloat("clearcoat",             0.0f);
    setFloat("clearcoatRoughness",    0.10f);
    setFloat("sheen",                 0.0f);
    setFloat("anisotropy",             0.0f);
    setFloat3("sheenColor",           white3);
    setFloat("emissiveScale",  0.0f);
    setFloat2("uvTiling",      uvTiling);
    setFloat2("uvOffset",      uvOffset);
    setFloat("alphaCutoff",    0.5f);
    setFloat3("emissiveColor", white3);
}

// albedo に Sprite サブアセットが指定された場合、Sprite矩形を標準UV変換へ合成する。
// WHY: GPU Texture 自体はatlas全体を共有するため、3D Materialで個別Spriteを使うには
//      頂点UVを矩形のscale/offsetへ写像する必要がある。
void ApplyAlbedoSpriteUv(const asset::MaterialAsset& materialAsset,
                         const renderer::ShaderDescriptor& desc,
                         renderer::ResourceManager& resources,
                         std::vector<uint8_t>& paramData)
{
    const auto albedo = materialAsset.textures.find("albedo");
    if (albedo == materialAsset.textures.end()) return;

    std::string texturePath;
    std::string spriteName;
    if (!asset::ParseSpriteReference(albedo->second, texturePath, spriteName)) return;

    struct CachedTransform {
        std::filesystem::file_time_type metaWriteTime{};
        float scaleX = 1.0f;
        float scaleY = 1.0f;
        float offsetX = 0.0f;
        float offsetY = 0.0f;
        bool resolved = false;
    };
    static std::unordered_map<std::string, CachedTransform> s_cache;

    const std::string absoluteTexturePath = asset::AssetManager::ResolveAssetPath(texturePath);
    const std::string metaPath = absoluteTexturePath + ".meta";
    std::error_code ec;
    const auto metaWriteTime = std::filesystem::last_write_time(metaPath, ec);
    CachedTransform& transform = s_cache[albedo->second];
    if (!transform.resolved || transform.metaWriteTime != metaWriteTime) {
        transform = {};
        transform.metaWriteTime = metaWriteTime;
        transform.resolved = true;
        if (!ec) {
            asset::TextureAsset textureAsset;
            asset::TexDescSerializer serializer;
            if (serializer.Load(metaPath, textureAsset)) {
                asset::SpriteRect implicitSingle;
                const asset::SpriteRect* sprite = asset::FindSprite(textureAsset.settings, spriteName);
                if (sprite == nullptr
                    && textureAsset.settings.type == asset::TextureType::Sprite
                    && textureAsset.settings.spriteMode == asset::SpriteMode::Single) {
                    implicitSingle.name = util::FileSystem::PathToUtf8(
                        util::FileSystem::PathFromUtf8(absoluteTexturePath).stem());
                    if (implicitSingle.name == spriteName) sprite = &implicitSingle;
                }
                const auto textureHandle = resources.LoadTexture(absoluteTexturePath);
                const renderer::ITexture* texture = resources.Get(textureHandle);
                if (sprite != nullptr && texture != nullptr) {
                    const float width = static_cast<float>(std::max<uint32_t>(1, texture->GetWidth()));
                    const float height = static_cast<float>(std::max<uint32_t>(1, texture->GetHeight()));
                    const float sourceSpriteWidth = sprite->width > 0
                        ? static_cast<float>(sprite->width) : width;
                    const float sourceSpriteHeight = sprite->height > 0
                        ? static_cast<float>(sprite->height) : height;
                    const float spriteX = std::clamp(static_cast<float>(sprite->x), 0.0f, width);
                    const float spriteY = std::clamp(static_cast<float>(sprite->y), 0.0f, height);
                    const float spriteWidth = std::clamp(sourceSpriteWidth, 0.0f, width - spriteX);
                    const float spriteHeight = std::clamp(sourceSpriteHeight, 0.0f, height - spriteY);
                    transform.scaleX = spriteWidth / width;
                    transform.scaleY = spriteHeight / height;
                    transform.offsetX = spriteX / width;
                    transform.offsetY = spriteY / height;
                }
            }
        }
    }

    const auto* tilingVar = desc.FindVar("uvTiling");
    const auto* offsetVar = desc.FindVar("uvOffset");
    if (tilingVar == nullptr || offsetVar == nullptr
        || tilingVar->varType != renderer::ShaderVarType::Float || tilingVar->columns < 2
        || offsetVar->varType != renderer::ShaderVarType::Float || offsetVar->columns < 2
        || tilingVar->offset + 2u * sizeof(float) > paramData.size()
        || offsetVar->offset + 2u * sizeof(float) > paramData.size())
        return;

    float tiling[2] = { 1.0f, 1.0f };
    float offset[2] = { 0.0f, 0.0f };
    std::memcpy(tiling, paramData.data() + tilingVar->offset, sizeof(tiling));
    std::memcpy(offset, paramData.data() + offsetVar->offset, sizeof(offset));
    tiling[0] *= transform.scaleX;
    tiling[1] *= transform.scaleY;
    offset[0] = offset[0] * transform.scaleX + transform.offsetX;
    offset[1] = offset[1] * transform.scaleY + transform.offsetY;
    std::memcpy(paramData.data() + tilingVar->offset, tiling, sizeof(tiling));
    std::memcpy(paramData.data() + offsetVar->offset, offset, sizeof(offset));
}

} // namespace

AnimatorComponent* FindAnimator(GameObject& go)
{
    // Skinned submesh はモデル構造により複数階層下へ配置されるため、直親だけで打ち切らない。
    // WHY: Animatorを見失うとbind pose用CBへフォールバックし、Trailの初期位置もずれる。
    for (GameObject* current = &go; current; current = current->GetParent())
        if (auto* animator = current->GetComponent<AnimatorComponent>())
            return animator;
    return nullptr;
}

const char* GetFallbackMaterialPath(bool skinned)
{
    return skinned ? "Assets/Materials/Fallback/FallbackSkinned.mat"
                   : "Assets/Materials/Fallback/Fallback.mat";
}

void LogSkinnedSurfaceFallbackWarningOnce(std::string_view shaderPath)
{
    static std::unordered_set<std::string> s_warnedSurfaceOnSkinned;
    std::string path(shaderPath);
    if (path.empty()) path = "<empty>";
    if (!s_warnedSurfaceOnSkinned.insert(path).second) return;
    FBZZ_LOG_WARN("SkinnedMeshRenderer has a Surface shader assigned: %s -> using %s. Use shaders under Skinned/.",
                  path.c_str(),
                  GetFallbackMaterialPath(true));
}

renderer::Material* GetFallbackMaterial(renderer::ResourceManager& resources, bool skinned)
{
    static MaterialComponent s_surfaceFallback;
    static MaterialComponent s_skinnedFallback;
    MaterialComponent& fallback = skinned ? s_skinnedFallback : s_surfaceFallback;
    fallback.materialPath = GetFallbackMaterialPath(skinned);
    if (!fallback.EnsureMaterialAsset()) return nullptr;
    return SyncMaterial(fallback, resources, skinned);
}

// 実体。MaterialComponent とスロットを分けて受け取り、
// 「コンポーネント全体の有効/無効」と「スロット単体の有効/無効」を両方尊重する。
static renderer::Material* SyncMaterialSlotImpl(MaterialSlot& mc,
                                                bool componentEnabled,
                                                renderer::ResourceManager& resources,
                                                bool preferSkinnedFallback)
{
    if (!componentEnabled || !mc.visible) return nullptr;

    if (!mc.materialPath.empty() && !mc.materialAsset.IsValid())
        mc.materialAsset = asset::AssetManager::LoadMaterial(mc.materialPath);

    auto activeAsset = mc.materialAsset;
    if (!activeAsset.IsValid()) {
        // .mat が読めない / 未割当でも、メッシュを画面から絶対に消さない。
        // 原色紫のフォールバック材質で描画を続け、問題を可視化する (Unity のマゼンタ相当)。
        // WHY: mc.materialAsset には書き戻さず毎フレーム再解決させる。壊れた .mat を
        //      修復・再インポートした瞬間 (FlushFailed 後) に正規材質へ自動復帰できる。
        const char* fallbackPath = GetFallbackMaterialPath(preferSkinnedFallback);
        static std::unordered_set<std::string> s_warnedMissingMaterials;
        const std::string warnKey =
            mc.materialPath.empty() ? std::string("<unassigned>") : mc.materialPath;
        if (s_warnedMissingMaterials.insert(warnKey).second) {
            FBZZ_LOG_WARN("Material load failed '%s' -> using fallback %s.",
                          warnKey.c_str(), fallbackPath);
        }
        activeAsset = asset::AssetManager::LoadMaterial(fallbackPath);
        // フォールバック .mat 自体が存在しない場合だけは描画を諦める。
        if (!activeAsset.IsValid()) return nullptr;
    }

    if (!mc.material)
        mc.material = std::make_unique<renderer::Material>();

    auto& material = *mc.material;
    const std::string& shaderPath = mc.GetShaderPath();
    const auto* matAsset = asset::AssetManager::GetMaterial(activeAsset);
    if (matAsset && shaderPath.empty()) {
        const char* fallbackPath = GetFallbackMaterialPath(preferSkinnedFallback);
        // WHY: shader 未設定の .mat を PBR 推定で描くと、未設定と意図した PBR の区別が付かない。
        //      原色紫の Unlit フォールバック材質へ明示的に差し替え、問題箇所を見つけやすくする。
        static std::unordered_set<std::string> s_warnedEmptyShaderMaterials;
        const std::string warnKey = mc.materialPath.empty() ? std::string("<unnamed>") : mc.materialPath;
        if (s_warnedEmptyShaderMaterials.insert(warnKey + "|" + fallbackPath).second) {
            FBZZ_LOG_WARN("Material '%s' has an empty shader path -> using %s.",
                          warnKey.c_str(), fallbackPath);
        }
        activeAsset = asset::AssetManager::LoadMaterial(fallbackPath);
        matAsset = asset::AssetManager::GetMaterial(activeAsset);
        if (!matAsset) return nullptr;
    }
    const std::string effectiveShaderPath = matAsset ? matAsset->shaderPath : std::string{};
    material.shaderPath = effectiveShaderPath;
    material.shader = effectiveShaderPath.empty()
        ? renderer::ResourceHandle<renderer::ShaderTag>{}
        : resources.LoadShader(effectiveShaderPath);

    const renderer::ShaderDescriptor* desc = nullptr;
    if (auto* shader = resources.Get(material.shader))
        desc = &shader->GetDescriptor();

    material.paramData.assign(desc ? desc->cbufferSize : 0u, 0u);
    if (desc)
        InitDefaultMaterialParams(*desc, material.paramData);
    if (desc && matAsset)
        ApplyMaterialAssetParams(*matAsset, *desc, material.paramData);
    // 共有アセット適用後にこの GO 専用の上書きを重ねる (per-instance パラメータ)。
    if (desc && !mc.paramOverrides.empty())
        ApplyMaterialParamOverrides(mc.paramOverrides, *desc, material.paramData);
    if (desc && matAsset)
        ApplyAlbedoSpriteUv(*matAsset, *desc, resources, material.paramData);

    std::array<std::string, kTextureSlotNames.size()> texturePaths{};
    if (matAsset) {
        for (size_t i = 0; i < kTextureSlotNames.size(); ++i) {
            const auto it = matAsset->textures.find(kTextureSlotNames[i]);
            texturePaths[i] = it != matAsset->textures.end() ? it->second : std::string{};
        }
    }
    for (size_t i = 0; i < kTextureSlotNames.size(); ++i) {
        const auto overrideIt = mc.textureOverrides.find(kTextureSlotNames[i]);
        if (overrideIt != mc.textureOverrides.end())
            texturePaths[i] = overrideIt->second;
    }

    const size_t slotCount = texturePaths.size();
    material.textures.resize(slotCount);
    for (size_t i = 0; i < slotCount; ++i)
    {
        material.textures[i] = texturePaths[i].empty()
            ? renderer::ResourceHandle<renderer::TextureTag>{}
            : resources.LoadTexture(texturePaths[i]);
    }

    static renderer::ShaderDescriptor s_fallback;
    material.Upload(resources, desc ? *desc : s_fallback);
    return &material;
}

renderer::Material* SyncMaterial(MaterialComponent& mc, renderer::ResourceManager& resources, bool preferSkinnedFallback)
{
    return SyncMaterialSlotImpl(mc, mc.enabled, resources, preferSkinnedFallback);
}

renderer::Material* SyncMaterialSlot(MaterialComponent& mc, size_t slotIndex,
                                     renderer::ResourceManager& resources, bool preferSkinnedFallback)
{
    // mc.enabled は基底 (スロット 0) の enabled であり、コンポーネント全体の有効判定を兼ねる。
    return SyncMaterialSlotImpl(mc.SlotAt(slotIndex), mc.enabled, resources, preferSkinnedFallback);
}

renderer::ResourceHandle<renderer::PipelineStateTag> GetOrCreateMaterialPSO(
    renderer::ResourceManager& resources,
    renderer::BlendMode        blend,
    bool                       doubleSided)
{
    // 両面描画はバックフェースカリングを無効化する。
    const renderer::RasterizerMode raster = doubleSided
        ? renderer::RasterizerMode::SOLID_NOCULL
        : renderer::RasterizerMode::SOLID;
    // 半透明・加算は深度書き込みをオフにし、背後のオブジェクトが透けて見えるようにする。
    const renderer::DepthMode depth = (blend == renderer::BlendMode::OPAQUE_BLEND)
        ? renderer::DepthMode::DEPTH_ON
        : renderer::DepthMode::DEPTH_READ;

    // WHY: ビットパッキング (旧実装) は enum 値追加時にサイレントなキー衝突が起きるため、
    //      構造体を直接比較する std::map に変更した。
    struct DescLess {
        bool operator()(const renderer::PipelineStateDesc& a,
                        const renderer::PipelineStateDesc& b) const noexcept {
            if (a.rasterizer != b.rasterizer) return a.rasterizer < b.rasterizer;
            if (a.blend      != b.blend)      return a.blend      < b.blend;
            return a.depth < b.depth;
        }
    };
    static std::map<renderer::PipelineStateDesc,
                    renderer::ResourceHandle<renderer::PipelineStateTag>,
                    DescLess> s_cache;
    const renderer::PipelineStateDesc desc{ raster, blend, depth };
    auto it = s_cache.find(desc);
    if (it != s_cache.end()) return it->second;
    auto handle = resources.CreatePipelineState(desc);
    s_cache[desc] = handle;
    return handle;
}

bool ShouldRenderGameObject(const GameObject& go, fbzz::LayerMask mask)
{
    return go.activeInHierarchy() && fbzz::Layer::Contains(mask, go.layer);
}

bool IsForwardOnly(const MaterialSlot& mc)
{
    const auto* a = asset::AssetManager::GetMaterial(mc.materialAsset);
    if (a) {
        if (a->renderPath == asset::RenderPath::Forward)  return true;
        // WHY: 2枚のGBufferにはclearcoat/sheen/anisotropyと接線基底を保持できない。
        //      拡張ローブをDeferredへ落とすと情報が欠落し、物理的なエネルギー配分も
        //      変わるため、Surface PBRだけは拡張値が有効な場合にForwardで完全評価する。
        const auto hasFeature = [&](std::string_view name) {
            const auto overrideIt = mc.paramOverrides.find(std::string(name));
            if (overrideIt != mc.paramOverrides.end())
                return !overrideIt->second.empty() && std::abs(overrideIt->second.front()) > 1.0e-4f;
            const auto* values = FindMaterialParam(*a, name);
            return values && !values->empty() && std::abs(values->front()) > 1.0e-4f;
        };
        const bool advancedPbr = a->meshType != asset::MeshType::Skinned &&
            (hasFeature("clearcoat") || hasFeature("sheen") || hasFeature("anisotropy"));
        if (advancedPbr) return true;
        if (a->renderPath == asset::RenderPath::Deferred) return false;
        if (a->shaderPath.empty()) return true;
    }
    return a == nullptr;
}

bool IsSurfaceMaterial(const MaterialSlot& mc)
{
    const auto* a = asset::AssetManager::GetMaterial(mc.materialAsset);
    if (a) {
        if (a->meshType == asset::MeshType::Surface) return true;
        if (a->meshType == asset::MeshType::Skinned) return false;
        if (a->shaderPath.empty()) return false;
    }
    return false;
}

// ── カリング ヘルパー ────────────────────────────────────────────────────────

void UpdateShadowConstants(RenderPassContext& ctx)
{
    const auto& rs = ctx.settings;
    ShadowConstantsCB data{};

    // 全カスケードが共有する 1 枚のアトラスなので、テクセルサイズはアトラス全体基準。
    // カスケード内 UV → アトラス UV への写像は HLSL 側 (cascadeAtlasRect) が行う。
    const float texel = 1.0f / static_cast<float>((std::max)(rs.shadow.mapResolution, 1u));
    data.shadowMapTexelSize[0] = texel;
    data.shadowMapTexelSize[1] = texel;

    const int count = std::clamp(ctx.shadowCascadeCount, 1, renderer::kMaxShadowCascades);
    data.cascadeCount     = count;
    data.cascadeBlend     = std::clamp(rs.shadow.cascadeBlend, 0.0f, 0.5f);
    // 可視化は分割している時だけ意味がある。1 分割で有効なままだと画面全体が
    // カスケード 0 の色に染まるだけなので、ここで落とす。
    data.cascadeDebugView = (rs.shadow.debugVisualizeCascades && count > 1) ? 1 : 0;

    float bias[renderer::kMaxShadowCascades] = {};
    for (int i = 0; i < renderer::kMaxShadowCascades; ++i) {
        // 未使用スロットは最遠カスケードで埋める。HLSL 側は cascadeCount までしか
        // 見ないが、未初期化の行列が残ると RenderDoc 等で追うときに紛らわしい。
        const ShadowCascade& cascade = ctx.shadowCascades[(i < count) ? i : count - 1];
        data.cascadeViewProjection[i] = cascade.viewProjection;
        data.cascadeAtlasRect[i]      = cascade.atlasRect;
        bias[i]                       = cascade.biasNDC;
    }
    data.cascadeBias = { bias[0], bias[1], bias[2], bias[3] };

    // 単一のライト行列で足りるパス向け (= 最遠カスケード)。
    // cascadeCount == 1 のときはカスケード 0 と同一なので、従来の単一シャドウマップ経路と一致する。
    data.lightViewProjection = ctx.lightVP;
    data.shadowBias          = ctx.shadowBiasNDC;
    data.shadowStrength      = ctx.shadowStrength;
    data.shadowPcfRadius     = rs.shadow.pcfRadius;

    data.cloudShadowStrength = ctx.cloudShadowStrength;
    data.cloudShadowCoverage = ctx.cloudShadowCoverage;
    data.cloudShadowScale    = ctx.cloudShadowScale;
    data.cloudShadowSpeed    = ctx.cloudShadowSpeed;
    data.cloudShadowTime     = ctx.cloudShadowTime;
    data.cloudShadowWindX    = ctx.cloudShadowWindX;
    data.cloudShadowWindZ    = ctx.cloudShadowWindZ;

    ctx.resources.Update(ctx.handles.shadowCB, &data, sizeof(ShadowConstantsCB));
}

WorldBounds ComputeWorldBounds(const Transform& tf, const renderer::Mesh& mesh)
{
    const math::Matrix4& world = tf.GetWorldMatrix();

    // ローカル空間バウンディング球中心をワールド空間に変換する。
    // 行列は列ベクトル規則 (M * v) なので:
    //   wx = m[0][0]*bx + m[0][1]*by + m[0][2]*bz + m[0][3]
    const float bx = mesh.boundsCenter.x;
    const float by = mesh.boundsCenter.y;
    const float bz = mesh.boundsCenter.z;
    const math::Vector3 worldCenter = {
        world.m[0][0]*bx + world.m[0][1]*by + world.m[0][2]*bz + world.m[0][3],
        world.m[1][0]*bx + world.m[1][1]*by + world.m[1][2]*bz + world.m[1][3],
        world.m[2][0]*bx + world.m[2][1]*by + world.m[2][2]*bz + world.m[2][3],
    };

    // ワールド行列の各軸ベクトルのノルムからスケールを取得し、最大値を掛ける。
    // WHY: 非一様スケールの場合は最大成分で保守的な球にする。
    //      球半径を過大評価しても偽カリング (見えているのに消える) は発生しない。
    const float sx = std::sqrt(world.m[0][0]*world.m[0][0] + world.m[1][0]*world.m[1][0] + world.m[2][0]*world.m[2][0]);
    const float sy = std::sqrt(world.m[0][1]*world.m[0][1] + world.m[1][1]*world.m[1][1] + world.m[2][1]*world.m[2][1]);
    const float sz = std::sqrt(world.m[0][2]*world.m[0][2] + world.m[1][2]*world.m[1][2] + world.m[2][2]*world.m[2][2]);
    const float maxScale = std::max({ sx, sy, sz });

    return { worldCenter, mesh.boundsRadius * maxScale };
}

bool IsVisibleInFrustum(const math::Frustum& frustum,
                        const Transform& tf,
                        const renderer::Mesh& mesh)
{
    // boundsRadius が 0 なら ComputeBounds 未実行メッシュ → カリングしない
    if (mesh.boundsRadius <= 0.0f) return true;

    const auto bounds = ComputeWorldBounds(tf, mesh);
    return frustum.IntersectsSphere(bounds.center, bounds.radius);
}

bool ComputeSkinnedWorldBounds(const Transform& tf,
                               const SkinnedMeshRenderer& smr,
                               WorldBounds& outBounds)
{
    if (!smr.model) return false;

    bool hasBounds = false;
    math::Vector3 weightedCenter = math::Vector3::ZERO;
    float totalWeight = 0.0f;

    // WHAT: 各 submesh のワールド球を半径重みで平均し、最後に全 submesh を包む半径へ拡張する。
    // WHY: 毎フレーム CPU スキニングして厳密 bounds を取ると頂点数に比例して重い。
    //      バインドポーズ球は保守的だが、視錐台外の遠いキャラクターを安く除外できる。
    for (const auto& meshPtr : smr.model->meshes) {
        if (!meshPtr || meshPtr->boundsRadius <= 0.0f) continue;
        const WorldBounds bounds = ComputeWorldBounds(tf, *meshPtr);
        const float weight = (std::max)(bounds.radius, 0.001f);
        weightedCenter = weightedCenter + bounds.center * weight;
        totalWeight += weight;
        hasBounds = true;
    }

    if (!hasBounds || totalWeight <= 0.0f) return false;

    outBounds.center = weightedCenter * (1.0f / totalWeight);
    outBounds.radius = 0.0f;
    for (const auto& meshPtr : smr.model->meshes) {
        if (!meshPtr || meshPtr->boundsRadius <= 0.0f) continue;
        const WorldBounds bounds = ComputeWorldBounds(tf, *meshPtr);
        const math::Vector3 delta = bounds.center - outBounds.center;
        outBounds.radius = (std::max)(outBounds.radius, delta.Length() + bounds.radius);
    }

    return true;
}

bool IsSkinnedVisibleInFrustum(const math::Frustum& frustum,
                               const Transform& tf,
                               const SkinnedMeshRenderer& smr)
{
    WorldBounds bounds{};
    if (!ComputeSkinnedWorldBounds(tf, smr, bounds)) return true;
    return frustum.IntersectsSphere(bounds.center, bounds.radius);
}

ActiveWindZone FindActiveWindZone(Scene& scene)
{
    ActiveWindZone result;
    for (auto& go : scene.GameObjects()) {
        if (!go.activeInHierarchy()) continue;
        const auto* wind = go.GetComponent<WindZoneComponent>();
        if (!wind || !wind->enabled) continue;
        // direction はローカル指定。GameObject を回せば風向きも回る。
        const math::Vector3 worldDir = go.transform.worldRotation * wind->direction;
        const float len = worldDir.Length();
        if (len > 1.0e-4f)
            result.direction = worldDir * (1.0f / len);
        result.active         = true;
        result.strength       = (std::max)(wind->strength, 0.0f);
        result.turbulence     = (std::max)(wind->turbulence, 0.0f);
        result.pulseFrequency = (std::max)(wind->pulseFrequency, 0.0f);
        break; // シーンに 1 つ想定。複数ある場合は最初の有効な 1 つを使う

    }
    return result;
}

math::Vector3 ComputeCameraFacingRibbonNormal(
    const math::Vector3& direction, const math::Vector3& cameraPos, const math::Vector3& point)
{
    // 帯の面をカメラへ向けるには、幅方向を「進行方向 × 視線方向」に取る。
    math::Vector3 up = cameraPos - point;
    if (up.LengthSq() > math::EPSILON * math::EPSILON) up = up.Normalized();
    else up = math::Vector3::UP;
    // 進行方向と視線がほぼ平行だと外積が退化して帯が消える。安定な軸へ逃がす。
    if (std::abs(math::Vector3::Dot(direction, up)) > 0.99f) up = math::Vector3::UP;
    if (std::abs(math::Vector3::Dot(direction, up)) > 0.99f) up = math::Vector3::RIGHT;

    const math::Vector3 normal = math::Vector3::Cross(direction, up);
    return normal.LengthSq() > math::EPSILON * math::EPSILON
        ? normal.Normalized() : math::Vector3::RIGHT;
}

} // namespace fbzz::scene
