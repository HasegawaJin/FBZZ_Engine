// FBZZ Engine
// RenderPasses/GeometryPassHelpers.cpp | fbzz::scene
// ジオメトリパス共有ヘルパー関数
#include "GeometryPasses.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Components/MaterialComponent.hpp"
#include "Engine/Renderer/Material.hpp"
#include "Engine/Renderer/IShader.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include <Math/Matrix4.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <memory>
#include <string>

namespace fbzz::scene {

renderer::Material* SyncMaterial(MaterialComponent& mc, renderer::ResourceManager& resources)
{
    if (!mc.enabled) return nullptr;

    if (!mc.material)
        mc.material = std::make_shared<renderer::Material>();

    auto& material = *mc.material;
    material.shaderPath = mc.shaderPath;
    material.shader = mc.shaderPath.empty()
        ? renderer::ResourceHandle<renderer::ShaderTag>{}
        : resources.LoadShader(mc.shaderPath);

    const renderer::ShaderDescriptor* desc = nullptr;
    if (auto* shader = resources.Get(material.shader))
        desc = &shader->GetDescriptor();

    if (desc && mc.paramData.size() != desc->cbufferSize)
        mc.InitFromDescriptor(*desc);

    material.paramData = mc.paramData;

    const size_t slotCount = mc.texturePaths.size();
    material.textures.resize(slotCount);
    for (size_t i = 0; i < slotCount; ++i)
    {
        material.textures[i] = mc.texturePaths[i].empty()
            ? renderer::ResourceHandle<renderer::TextureTag>{}
            : resources.LoadTexture(mc.texturePaths[i]);
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
