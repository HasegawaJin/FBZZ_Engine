// FBZZ Engine
// RenderPasses/GeometryPassHelpers.cpp | fbzz::scene
// ジオメトリパス共有ヘルパー関数
#include "GeometryPasses.hpp"
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Asset/MaterialAsset.hpp"
#include "Engine/Core/Logger.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Components/MaterialComponent.hpp"
#include "Engine/Renderer/Material.hpp"
#include "Engine/Renderer/IShader.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include <Math/Matrix4.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
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

    // WHY: .fzmat はレビューしやすい PBR 名、HLSL は既存の短い変数名を使っている。
    //      ここで吸収してアセット名を ShaderDescriptor の内部名に依存させない。
    if (shaderVarName == "albedo")              it = asset.params.find("base_color");
    else if (shaderVarName == "metallic")       it = asset.params.find("metallic_factor");
    else if (shaderVarName == "roughness")      it = asset.params.find("roughness_factor");
    else if (shaderVarName == "normalStrength") it = asset.params.find("normal_strength");
    else if (shaderVarName == "emissiveColor")  it = asset.params.find("emissive_color");
    else if (shaderVarName == "emissiveScale")  it = asset.params.find("emissive_scale");

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
    setFloat("emissiveScale",  0.0f);
    setFloat2("uvTiling",      uvTiling);
    setFloat2("uvOffset",      uvOffset);
    setFloat("alphaCutoff",    0.5f);
    setFloat3("emissiveColor", white3);
}

} // namespace

const char* GetFallbackMaterialPath(bool skinned)
{
    return skinned ? "Assets/Materials/FallbackSkinned.fzmat"
                   : "Assets/Materials/Fallback.fzmat";
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

renderer::Material* SyncMaterial(MaterialComponent& mc, renderer::ResourceManager& resources, bool preferSkinnedFallback)
{
    if (!mc.enabled) return nullptr;

    if (!mc.materialPath.empty() && !mc.materialAsset.IsValid())
        mc.materialAsset = asset::AssetManager::LoadMaterial(mc.materialPath);
    if (!mc.materialAsset.IsValid() && !mc.materialPath.empty())
        return nullptr;
    if (!mc.materialAsset.IsValid() && mc.materialPath.empty())
        return nullptr;

    if (!mc.material)
        mc.material = std::make_shared<renderer::Material>();

    auto& material = *mc.material;
    const std::string& shaderPath = mc.GetShaderPath();
    auto activeAsset = mc.materialAsset;
    const auto* matAsset = asset::AssetManager::GetMaterial(activeAsset);
    if (matAsset && shaderPath.empty()) {
        const char* fallbackPath = GetFallbackMaterialPath(preferSkinnedFallback);
        // WHY: shader 未設定の .fzmat を PBR 推定で描くと、未設定と意図した PBR の区別が付かない。
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

    std::array<std::string, kTextureSlotNames.size()> texturePaths{};
    if (matAsset) {
        for (size_t i = 0; i < kTextureSlotNames.size(); ++i) {
            const auto it = matAsset->textures.find(kTextureSlotNames[i]);
            texturePaths[i] = it != matAsset->textures.end() ? it->second : std::string{};
        }
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
    return go.activeSelf() && fbzz::Layer::Contains(mask, go.layer);
}

bool IsSurfaceMaterialShader(std::string_view path)
{
    std::string lower(path);
    std::replace(lower.begin(), lower.end(), '\\', '/');
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("/material/surface/") != std::string::npos;
}

bool IsForwardOnlyShader(std::string_view path)
{
    std::string lower(path);
    std::replace(lower.begin(), lower.end(), '\\', '/');
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // GBuffer に収まらない独自ライティング / エフェクト系シェーダー
    // BlinnPhong / Phong は独自スペキュラモデル (Blinn-Phong shininess) を持つため
    // Deferred の PBR ライティング (GGX) を適用するとスペキュラ形状と roughness マッピングが
    // 変わってしまう。Forward で正しいモデルのまま描画する。
    return lower.find("blinnphong") != std::string::npos
        || lower.find("phong")      != std::string::npos
        || lower.find("lambert")    != std::string::npos
        || lower.find("rimlight")   != std::string::npos
        || lower.find("toon")       != std::string::npos
        || lower.find("subsurface") != std::string::npos
        || lower.find("anisotropic")!= std::string::npos
        || lower.find("dissolve")   != std::string::npos
        || lower.find("unlit")      != std::string::npos;
}

bool IsForwardOnly(const MaterialComponent& mc)
{
    const auto* a = asset::AssetManager::GetMaterial(mc.materialAsset);
    if (a) {
        if (a->renderPath == asset::RenderPath::Forward)  return true;
        if (a->renderPath == asset::RenderPath::Deferred) return false;
        if (a->shaderPath.empty()) return true;
    }
    return IsForwardOnlyShader(mc.GetShaderPath());
}

bool IsSurfaceMaterial(const MaterialComponent& mc)
{
    const auto* a = asset::AssetManager::GetMaterial(mc.materialAsset);
    if (a) {
        if (a->meshType == asset::MeshType::Surface) return true;
        if (a->meshType == asset::MeshType::Skinned) return false;
        if (a->shaderPath.empty()) return false;
    }
    return IsSurfaceMaterialShader(mc.GetShaderPath());
}

// ── カリング ヘルパー ────────────────────────────────────────────────────────

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

} // namespace fbzz::scene
