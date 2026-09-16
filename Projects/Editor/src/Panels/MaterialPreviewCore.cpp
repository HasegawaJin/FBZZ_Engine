/// @file    MaterialPreviewCore.cpp
/// @brief   .mat プレビューのパラメータ解決・メッシュ生成・オフスクリーン描画。
/// @author  Hasegawa Jin
/// @date    2026-09-16
#include <Editor/Panels/MaterialPreviewCore.hpp>

#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/LightSystem.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/PrimitiveMesh.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Renderer/ShaderDescriptor.hpp>
#include <Engine/Scene/Systems/RenderPasses/Geometry/WaterRenderPass.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Scene/Systems/UISystem.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Core/Memory/MakeUnique.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <vector>

namespace fbzz::editor::matpreview {

namespace {

constexpr const char* kFallbackShader = "Assets/Shaders/Material/Surface/Fallback.hlsl";
constexpr const char* kMeshFallbackShader = "Assets/Shaders/Material/Surface/Lit.hlsl";
constexpr const char* kChannelShader = "Assets/Shaders/Debug/MaterialChannel.hlsl";

// Lighting.hlsli の LIGHT_UNIT_SCALE。距離補正で相殺する。
constexpr float kLightUnitScale = std::numbers::pi_v<float>;

// Decal の受け面 / PostProcess の入力シーンを焼く中間 RT の一辺。
// WHY 描画先と同じ寸法にしないか: どちらも画面 UV で引くので解像度が違っても
//     位置は合う。プレビューごとに RT を作り分けるほうが VRAM を食う。
constexpr std::uint32_t kEffectRtSize = 512;

// t0-t15 は標準 Material スロット。Terrain / Water は専用名で解決される。
constexpr std::array<const char*, 16> kSlotNames = {
    "albedo", "normal", "metallic", "emissive", "ao",
    "tex5", "tex6", "tex7", "tex8", "tex9",
    "tex10", "tex11", "tex12", "tex13", "tex14", "tex15",
};

std::string Lower(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// エンジンが埋める定数バッファ (Terrain / Water / Particle / Trail / Decal / UI) は
// すべて scene:: の公開レイアウトをそのまま使う。プレビュー専用の写しは持たない。
using scene::DecalCB;
using scene::ParticleRenderCB;
using scene::ParticleVertex;
using scene::PostProcCB;
using scene::TerrainObjectCB;
using scene::TrailCB;
using scene::TrailVertex;
using scene::UIConstantsCB;
using scene::WaterCB;
using scene::WaterVertex;

struct SkinningCB {
    math::Matrix4 boneMatrices[128];
};

// LAYOUT: Assets/Shaders/Debug/MaterialChannel.hlsl の MaterialChannelConstants。
struct ChannelCB {
    math::Vector4 baseColor;
    math::Vector4 pbr;      // x=metallic y=roughness z=occlusionStrength w=normalStrength
    math::Vector4 uv;       // xy=tiling zw=offset
    math::Vector4 emissive; // rgb=emissiveColor a=emissiveScale
    std::uint32_t textureMask = 0;
    std::uint32_t mode = 0;
    float         _pad[2] = {};
};


// 単色フォールバック材質 (Lit.hlsl) の MaterialConstants。
struct FallbackMaterialCB {
    math::Vector4 albedo{ 1.0f, 1.0f, 1.0f, 1.0f };
    std::uint32_t textureMask = 0;
    float         _pad[3] = {};
};

// ── 共有 GPU リソース ──────────────────────────────────────────────────────

struct SharedResources {
    renderer::ResourceHandle<renderer::ShaderTag>         fallbackShader;
    renderer::ResourceHandle<renderer::ShaderTag>         channelShader;
    renderer::ResourceHandle<renderer::PipelineStateTag>  solidPso;
    renderer::ResourceHandle<renderer::PipelineStateTag>  wireframePso;
    renderer::ResourceHandle<renderer::PipelineStateTag>  uiPso;
    renderer::ResourceHandle<renderer::ConstantBufferTag> frameCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> fallbackMaterialCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> channelCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> lightCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> shadowCB;
    // Spot / Point シャドウ (b12) の無効化用。中身は 0 のまま使う。
    // WHY 要るか: 定数バッファの束縛は DrawCall をまたいで残るが、テクスチャ SRV は
    //     ドローごとにクリアされる。b12 だけシーン描画のものが残ると
    //     punctualShadowCount > 0 のままアトラス (t28) が未束縛になり、
    //     比較サンプルが 0 (= 完全な影) を返してプレビューが黒く潰れる。
    renderer::ResourceHandle<renderer::ConstantBufferTag> punctualShadowCB;
    // ライト供給モード (b9) の無効化用。中身は 0 = FBZZ_LIGHT_MODE_LEGACY のまま使う。
    // WHY 要るか: シーン描画は Forward でも統合配列を使う。b9 の束縛は残る一方
    //     ライト配列 (t29) は毎回クリアされるので、渡さないと «本数は残っているのに
    //     中身が全部ゼロ» を読んでライトが一つも当たらない。
    renderer::ResourceHandle<renderer::ConstantBufferTag> clusterCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> terrainCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> waterCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> skinningCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> uiCB;
    renderer::ResourceHandle<renderer::BufferTag>         uiVertexBuffer;
    renderer::ResourceHandle<renderer::BufferTag>         uiIndexBuffer;

    // ── エフェクト系 ──
    renderer::ResourceHandle<renderer::PipelineStateTag>  alphaPso;
    renderer::ResourceHandle<renderer::PipelineStateTag>  additivePso;
    renderer::ResourceHandle<renderer::PipelineStateTag>  premultipliedPso;
    renderer::ResourceHandle<renderer::ConstantBufferTag> particleCB;
    renderer::ResourceHandle<renderer::BufferTag>         particleVertexBuffer;
    renderer::ResourceHandle<renderer::BufferTag>         particleIndexBuffer;
    renderer::ResourceHandle<renderer::ConstantBufferTag> trailCB;
    renderer::ResourceHandle<renderer::BufferTag>         trailVertexBuffer;
    renderer::ResourceHandle<renderer::BufferTag>         trailIndexBuffer;
    renderer::ResourceHandle<renderer::ConstantBufferTag> decalCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> postProcCB;
    // Decal の受け面深度と PostProcess の入力シーン。プレビュー内で 1 枚ずつ焼く。
    renderer::ResourceHandle<renderer::RenderTargetTag>   sceneRT;

    renderer::ResourceHandle<renderer::TextureTag>        whiteTexture;
    renderer::ResourceHandle<renderer::TextureTag>        blackTexture;
    renderer::ResourceHandle<renderer::TextureTag>        flatNormalTexture;
};

SharedResources g_shared;

// ── パラメータ解決 ────────────────────────────────────────────────────────

const std::vector<float>* FindParam(const asset::MaterialAsset& material, std::string_view name)
{
    auto it = material.params.find(std::string(name));
    if (it != material.params.end()) return &it->second;

    // WHY: .mat は PBR 寄りの名前、HLSL は shader ごとの短い変数名を使う場合がある。
    //      プレビューも本編描画と同じ別名吸収を行う。
    if (name == "albedo")              it = material.params.find("base_color");
    else if (name == "metallic")       it = material.params.find("metallic_factor");
    else if (name == "roughness")      it = material.params.find("roughness_factor");
    else if (name == "normalStrength") it = material.params.find("normal_strength");
    else if (name == "emissiveColor")  it = material.params.find("emissive_color");
    else if (name == "emissiveScale")  it = material.params.find("emissive_scale");

    return it != material.params.end() ? &it->second : nullptr;
}

float ParamFloat(const asset::MaterialAsset* material, std::string_view name, float fallback)
{
    if (!material) return fallback;
    const auto* values = FindParam(*material, name);
    return (values && !values->empty()) ? (*values)[0] : fallback;
}

math::Vector3 ParamFloat3(const asset::MaterialAsset* material, std::string_view name, math::Vector3 fallback)
{
    if (!material) return fallback;
    const auto* values = FindParam(*material, name);
    if (!values || values->size() < 3) return fallback;
    return { (*values)[0], (*values)[1], (*values)[2] };
}

std::string FindTexturePath(const asset::MaterialAsset& material,
                            const renderer::ShaderTexBindDesc& binding)
{
    if (auto it = material.textures.find(binding.name); it != material.textures.end())
        return it->second;

    const std::string name = Lower(binding.name);
    const auto slotOf = [&material](const std::string& slot) -> std::string {
        auto it = material.textures.find(slot);
        return it != material.textures.end() ? it->second : std::string{};
    };

    if (name == "g_normalmap1") return slotOf("normalMap1");
    if (name == "g_normalmap2") return slotOf("normalMap2");
    if (name == "g_foamtex")    return slotOf("foamTex");
    if (name == "g_foammask")   return slotOf("foamMask");
    if (name == "g_envtex")     return slotOf("envCubemap");
    if (name == "g_flowmap")    return slotOf("flowMap");
    if (name == "g_splatmap")   return slotOf("splatmap");
    if (name.starts_with("g_diffuse") && binding.slot >= 1 && binding.slot <= 4)
        return slotOf("layer" + std::to_string(binding.slot - 1) + "_diffuse");
    if (name.starts_with("g_normal") && binding.slot >= 5 && binding.slot <= 8)
        return slotOf("layer" + std::to_string(binding.slot - 5) + "_normal");
    if (name.starts_with("g_aoroughness") && binding.slot >= 9 && binding.slot <= 12)
        return slotOf("layer" + std::to_string(binding.slot - 9) + "_ao_roughness");

    if (binding.slot < kSlotNames.size()) {
        const std::string standard = slotOf(kSlotNames[binding.slot]);
        if (!standard.empty()) return standard;
        // Terrain レイヤーの fzmat はプレフィックスなし ("diffuse" / "ao_roughness") を使う。
        if (std::string_view(kSlotNames[binding.slot]) == "albedo") return slotOf("diffuse");
        if (std::string_view(kSlotNames[binding.slot]) == "ao")     return slotOf("ao_roughness");
    }
    return {};
}

void InitDefaultParams(const renderer::ShaderDescriptor& descriptor, std::vector<std::uint8_t>& data)
{
    const float one = 1.0f;
    for (const auto& variable : descriptor.vars) {
        if (variable.varType != renderer::ShaderVarType::Float) continue;
        for (std::uint32_t column = 0; column < variable.columns; ++column) {
            const std::uint32_t offset = variable.offset + column * sizeof(float);
            if (offset + sizeof(float) <= data.size())
                std::memcpy(data.data() + offset, &one, sizeof(one));
        }
    }

    const auto set = [&](std::string_view name, const float* values, std::uint32_t count) {
        const auto* variable = descriptor.FindVar(name);
        if (!variable || variable->varType != renderer::ShaderVarType::Float) return;
        if (variable->columns < count) return;
        if (variable->offset + count * sizeof(float) > data.size()) return;
        std::memcpy(data.data() + variable->offset, values, count * sizeof(float));
    };
    const float metallic = 0.0f;
    const float roughness = 0.65f;
    const float emissiveScale = 0.0f;
    const float alphaCutoff = 0.5f;
    const float uvTiling[2] = { 1.0f, 1.0f };
    const float uvOffset[2] = { 0.0f, 0.0f };
    const float white3[3] = { 1.0f, 1.0f, 1.0f };
    set("metallic", &metallic, 1);
    set("roughness", &roughness, 1);
    set("emissiveScale", &emissiveScale, 1);
    set("alphaCutoff", &alphaCutoff, 1);
    set("uvTiling", uvTiling, 2);
    set("uvOffset", uvOffset, 2);
    set("emissiveColor", white3, 3);
}

void ApplyAssetParams(const asset::MaterialAsset& material,
                      const renderer::ShaderDescriptor& descriptor,
                      std::vector<std::uint8_t>& data)
{
    for (const auto& variable : descriptor.vars) {
        if (variable.varType != renderer::ShaderVarType::Float) continue;
        if (variable.offset + variable.size > data.size()) continue;
        const auto* values = FindParam(material, variable.name);
        if (!values || values->empty()) continue;
        const std::size_t count = std::min<std::size_t>(variable.columns, values->size());
        std::memcpy(data.data() + variable.offset, values->data(), count * sizeof(float));
    }
}

// ── メッシュ生成 ──────────────────────────────────────────────────────────

renderer::Mesh* PrimitiveFor(renderer::ResourceManager& resources, Shape shape)
{
    switch (shape) {
        case Shape::Cube:     return renderer::PrimitiveMesh::Cube(resources);
        case Shape::Plane:    return renderer::PrimitiveMesh::Plane(resources);
        case Shape::Quad:     return renderer::PrimitiveMesh::Quad(resources);
        case Shape::Cylinder: return renderer::PrimitiveMesh::Cylinder(resources, 48);
        case Shape::Cone:     return renderer::PrimitiveMesh::Cone(resources, 48);
        case Shape::Torus:    return renderer::PrimitiveMesh::Torus(resources, 48);
        case Shape::Capsule:  return renderer::PrimitiveMesh::Capsule(resources, 32);
        case Shape::Sphere:
        default:              return renderer::PrimitiveMesh::Sphere(resources, 64);
    }
}

// Terrain / Water 用の細分割された平面。
// WHY 4 頂点の PrimitiveMesh::Plane を使わないか: Water の Gerstner 波は頂点変位なので、
//     4 頂点では «平らな板» にしかならない。Terrain も陰影の補間に頂点密度が要る。
renderer::Mesh* GridMesh(renderer::ResourceManager& resources, int segments)
{
    static std::map<int, std::unique_ptr<renderer::Mesh>> cache;
    if (auto it = cache.find(segments); it != cache.end()) return it->second.get();

    std::vector<renderer::Vertex> vertices;
    std::vector<std::uint32_t> indices;
    vertices.reserve(static_cast<std::size_t>(segments + 1) * (segments + 1));
    for (int z = 0; z <= segments; ++z) {
        for (int x = 0; x <= segments; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(segments);
            const float v = static_cast<float>(z) / static_cast<float>(segments);
            renderer::Vertex vertex{};
            vertex.position = { u - 0.5f, 0.0f, v - 0.5f };
            vertex.normal   = { 0.0f, 1.0f, 0.0f };
            vertex.tangent  = { 1.0f, 0.0f, 0.0f };
            vertex.uv       = { u, v };
            vertices.push_back(vertex);
        }
    }
    for (int z = 0; z < segments; ++z) {
        for (int x = 0; x < segments; ++x) {
            const std::uint32_t base = static_cast<std::uint32_t>(z * (segments + 1) + x);
            const std::uint32_t next = base + static_cast<std::uint32_t>(segments + 1);
            indices.insert(indices.end(), { base, next, next + 1, base, next + 1, base + 1 });
        }
    }

    auto mesh = core::MakeUnique<renderer::Mesh>();
    if (!mesh) return nullptr;
    mesh->vertexBuffer = resources.CreateVertexBuffer(
        vertices.data(), vertices.size() * sizeof(renderer::Vertex), sizeof(renderer::Vertex));
    mesh->indexBuffer = resources.CreateIndexBuffer(
        indices.data(), static_cast<std::uint32_t>(indices.size()));
    mesh->vertexCount = static_cast<std::uint32_t>(vertices.size());
    mesh->indexCount  = static_cast<std::uint32_t>(indices.size());
    mesh->cpuVertices = std::move(vertices);
    mesh->cpuIndices  = std::move(indices);
    mesh->ComputeBounds();
    renderer::Mesh* result = mesh.get();
    cache[segments] = std::move(mesh);
    return result;
}

renderer::Mesh* SkinnedCopy(renderer::ResourceManager& resources, Shape shape)
{
    static std::map<int, std::unique_ptr<renderer::Mesh>> cache;
    const int key = static_cast<int>(shape);
    if (auto it = cache.find(key); it != cache.end()) return it->second.get();

    auto* surface = PrimitiveFor(resources, shape);
    if (!surface) return nullptr;

    std::vector<renderer::SkinnedVertex> vertices;
    vertices.reserve(surface->cpuVertices.size());
    for (const auto& source : surface->cpuVertices) {
        renderer::SkinnedVertex skinned{};
        skinned.position = source.position;
        skinned.normal   = source.normal;
        skinned.tangent  = source.tangent;
        skinned.uv       = source.uv;
        skinned.boneIndices[0] = 0;
        skinned.boneWeights[0] = 1.0f;
        vertices.push_back(skinned);
    }

    auto mesh = core::MakeUnique<renderer::Mesh>();
    if (!mesh) return nullptr;
    mesh->vertexBuffer = resources.CreateVertexBuffer(
        vertices.data(), vertices.size() * sizeof(renderer::SkinnedVertex),
        sizeof(renderer::SkinnedVertex));
    mesh->indexBuffer = resources.CreateIndexBuffer(
        surface->cpuIndices.data(), static_cast<std::uint32_t>(surface->cpuIndices.size()));
    mesh->vertexCount = static_cast<std::uint32_t>(vertices.size());
    mesh->indexCount  = static_cast<std::uint32_t>(surface->cpuIndices.size());
    mesh->isSkinned   = true;
    mesh->cpuSkinnedVertices = std::move(vertices);
    mesh->cpuIndices  = surface->cpuIndices;
    mesh->ComputeBounds();
    renderer::Mesh* result = mesh.get();
    cache[key] = std::move(mesh);
    return result;
}

renderer::Mesh* WaterGrid(renderer::ResourceManager& resources, int segments)
{
    static std::map<int, std::unique_ptr<renderer::Mesh>> cache;
    if (auto it = cache.find(segments); it != cache.end()) return it->second.get();

    auto* surface = GridMesh(resources, segments);
    if (!surface) return nullptr;

    std::vector<WaterVertex> vertices;
    vertices.reserve(surface->cpuVertices.size());
    for (const auto& source : surface->cpuVertices)
        vertices.push_back({ source.position, source.uv });

    auto mesh = core::MakeUnique<renderer::Mesh>();
    if (!mesh) return nullptr;
    mesh->vertexBuffer = resources.CreateVertexBuffer(
        vertices.data(), vertices.size() * sizeof(WaterVertex), sizeof(WaterVertex));
    mesh->indexBuffer = resources.CreateIndexBuffer(
        surface->cpuIndices.data(), static_cast<std::uint32_t>(surface->cpuIndices.size()));
    mesh->vertexCount = static_cast<std::uint32_t>(vertices.size());
    mesh->indexCount  = static_cast<std::uint32_t>(surface->cpuIndices.size());
    mesh->cpuVertices = surface->cpuVertices;
    mesh->cpuIndices  = surface->cpuIndices;
    mesh->ComputeBounds();
    renderer::Mesh* result = mesh.get();
    cache[segments] = std::move(mesh);
    return result;
}

// ── 境界 ─────────────────────────────────────────────────────────────────

math::Vector3 BoundsCenter(const renderer::Mesh& mesh)
{
    if (mesh.boundsRadius > 0.0f) return mesh.boundsCenter;

    constexpr float big = (std::numeric_limits<float>::max)();
    math::Vector3 minP{ big, big, big };
    math::Vector3 maxP{ -big, -big, -big };
    const auto visit = [&](const math::Vector3& p) {
        minP = { std::min(minP.x, p.x), std::min(minP.y, p.y), std::min(minP.z, p.z) };
        maxP = { std::max(maxP.x, p.x), std::max(maxP.y, p.y), std::max(maxP.z, p.z) };
    };
    if (mesh.isSkinned) for (const auto& v : mesh.cpuSkinnedVertices) visit(v.position);
    else                for (const auto& v : mesh.cpuVertices)        visit(v.position);
    if (minP.x > maxP.x) return {};
    return (minP + maxP) * 0.5f;
}

float BoundsRadius(const renderer::Mesh& mesh, const math::Vector3& center)
{
    if (mesh.boundsRadius > 0.0f) return mesh.boundsRadius;

    float radiusSq = 0.0f;
    const auto visit = [&](const math::Vector3& p) {
        radiusSq = std::max(radiusSq, (p - center).LengthSq());
    };
    if (mesh.isSkinned) for (const auto& v : mesh.cpuSkinnedVertices) visit(v.position);
    else                for (const auto& v : mesh.cpuVertices)        visit(v.position);
    return std::sqrt(std::max(radiusSq, 0.0001f));
}

// ── 照明 ─────────────────────────────────────────────────────────────────

struct RigProfile {
    math::Vector3 keyColor;
    float         keyIntensity;
    math::Vector3 ambient;
    math::Vector3 fillColor;
    float         fillIntensity;
    math::Vector3 rimColor;
    float         rimIntensity;
};

RigProfile ProfileFor(LightPreset preset)
{
    switch (preset) {
        case LightPreset::Outdoor:
            return { { 1.0f, 0.95f, 0.84f }, 3.1f, { 0.16f, 0.20f, 0.28f },
                     { 0.45f, 0.62f, 1.0f }, 0.8f, { 1.0f, 0.98f, 0.92f }, 0.7f };
        case LightPreset::Night:
            return { { 0.55f, 0.66f, 1.0f }, 0.5f, { 0.03f, 0.035f, 0.05f },
                     { 0.30f, 0.40f, 0.80f }, 0.2f, { 0.70f, 0.90f, 1.0f }, 1.8f };
        case LightPreset::Flat:
            // 陰影をほぼ消してテクスチャの絵柄だけを読ませる。リムは輪郭を歪めるので切る。
            return { { 1.0f, 1.0f, 1.0f }, 0.8f, { 0.55f, 0.55f, 0.58f },
                     { 1.0f, 1.0f, 1.0f }, 0.35f, { 1.0f, 1.0f, 1.0f }, 0.0f };
        case LightPreset::Studio:
        default:
            return { { 1.0f, 0.96f, 0.90f }, 1.8f, { 0.10f, 0.11f, 0.14f },
                     { 0.55f, 0.65f, 1.0f }, 0.4f, { 1.0f, 1.0f, 1.0f }, 1.1f };
    }
}

// リグ全体をワールド Y 回り → ワールド X 回りの順で回す。
math::Vector3 RotateRig(const math::Vector3& v, float yaw, float pitch)
{
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const math::Vector3 afterYaw{ v.x * cy + v.z * sy, v.y, -v.x * sy + v.z * cy };
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    return { afterYaw.x, afterYaw.y * cp - afterYaw.z * sp, afterYaw.y * sp + afterYaw.z * cp };
}

// ── 共有リソースの確保 ────────────────────────────────────────────────────

bool EnsureDefaultTextures(renderer::ResourceManager& resources)
{
    if (!g_shared.whiteTexture.IsValid()) {
        const std::uint8_t rgba[4] = { 255, 255, 255, 255 };
        g_shared.whiteTexture = resources.CreateTexture(rgba, 1, 1);
    }
    if (!g_shared.blackTexture.IsValid()) {
        const std::uint8_t rgba[4] = { 0, 0, 0, 255 };
        g_shared.blackTexture = resources.CreateTexture(rgba, 1, 1);
    }
    if (!g_shared.flatNormalTexture.IsValid()) {
        const std::uint8_t rgba[4] = { 128, 128, 255, 255 };
        g_shared.flatNormalTexture = resources.CreateTexture(rgba, 1, 1);
    }
    return g_shared.whiteTexture.IsValid() && g_shared.blackTexture.IsValid() &&
           g_shared.flatNormalTexture.IsValid();
}

renderer::ResourceHandle<renderer::TextureTag> DefaultTextureForSlot(Flavor flavor, std::uint32_t slot)
{
    if (flavor == Flavor::Water) {
        if (slot == 0 || slot == 1 || slot == 7) return g_shared.flatNormalTexture;
        if (slot == 4 || slot == 6 || slot == 8) return g_shared.blackTexture;
        return g_shared.whiteTexture;
    }
    if (flavor == Flavor::Terrain) {
        if (slot >= 5 && slot <= 8) return g_shared.flatNormalTexture;
        return g_shared.whiteTexture;
    }
    return {};
}

bool EnsureShared(renderer::ResourceManager& resources, bool needFallbackShader)
{
    if (needFallbackShader && !g_shared.fallbackShader.IsValid())
        g_shared.fallbackShader = resources.LoadShader(kMeshFallbackShader);
    if (needFallbackShader && !g_shared.fallbackShader.IsValid()) return false;

    if (!g_shared.solidPso.IsValid()) {
        // WHY 材質自身の blend / depth を使わないか: そうすると同じ .mat でも
        //     表示場所によって輪郭・透過・陰影が変わってしまう。
        g_shared.solidPso = resources.CreatePipelineState({
            renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND,
            renderer::DepthMode::DEPTH_ON });
    }
    if (!g_shared.frameCB.IsValid())
        g_shared.frameCB = resources.CreateConstantBuffer(sizeof(scene::PerFrameCB));
    if (!g_shared.objectCB.IsValid())
        g_shared.objectCB = resources.CreateConstantBuffer(sizeof(scene::PerObjectCB));
    if (!g_shared.lightCB.IsValid())
        g_shared.lightCB = resources.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
    if (!g_shared.shadowCB.IsValid())
        g_shared.shadowCB = resources.CreateConstantBuffer(sizeof(scene::ShadowConstantsCB));
    if (needFallbackShader && !g_shared.fallbackMaterialCB.IsValid())
        g_shared.fallbackMaterialCB = resources.CreateConstantBuffer(sizeof(FallbackMaterialCB));
    if (!g_shared.punctualShadowCB.IsValid()) {
        g_shared.punctualShadowCB =
            resources.CreateConstantBuffer(sizeof(scene::PunctualShadowConstantsCB));
        const scene::PunctualShadowConstantsCB empty{};
        resources.Update(g_shared.punctualShadowCB, &empty, sizeof(empty));
    }
    if (!g_shared.clusterCB.IsValid()) {
        g_shared.clusterCB = resources.CreateConstantBuffer(sizeof(scene::ClusterConstantsCB));
        const scene::ClusterConstantsCB legacy{};  // clusterLightMode = 0 = LEGACY
        resources.Update(g_shared.clusterCB, &legacy, sizeof(legacy));
    }

    return g_shared.solidPso.IsValid() && g_shared.frameCB.IsValid() &&
           g_shared.objectCB.IsValid() && g_shared.lightCB.IsValid() &&
           g_shared.shadowCB.IsValid() &&
           (!needFallbackShader || g_shared.fallbackMaterialCB.IsValid());
}

// ── Terrain / Water の定数 ────────────────────────────────────────────────

TerrainObjectCB BuildTerrainCB(const asset::MaterialAsset* material, const math::Matrix4& viewProjection)
{
    TerrainObjectCB cb{};
    cb.worldMatrix = math::Matrix4::Identity();
    cb.wvpMatrix = viewProjection;
    for (int i = 0; i < 4; ++i) {
        const std::string prefix = "layer" + std::to_string(i) + "_";
        cb.layerTiling[i] = { ParamFloat(material, prefix + "tilingX", 2.0f),
                              ParamFloat(material, prefix + "tilingZ", 2.0f), 0.0f, 0.0f };
        const float normalStrength = ParamFloat(material, prefix + "normalStrength", 1.0f);
        if (i == 0)      cb.layerNormalStrength.x = normalStrength;
        else if (i == 1) cb.layerNormalStrength.y = normalStrength;
        else if (i == 2) cb.layerNormalStrength.z = normalStrength;
        else             cb.layerNormalStrength.w = normalStrength;
        cb.layerMaterial[i] = { ParamFloat(material, prefix + "roughness", 0.8f),
                                ParamFloat(material, prefix + "ambientOcclusion", 1.0f), 0.0f, 0.0f };
        cb.layerAutoHeight[i] = { ParamFloat(material, prefix + "autoMinHeight", -10000.0f),
                                  ParamFloat(material, prefix + "autoMaxHeight", 10000.0f),
                                  ParamFloat(material, prefix + "autoHeightFade", 1.0f),
                                  ParamFloat(material, prefix + "autoBlendEnabled", 0.0f) };
        cb.layerAutoSlope[i] = { ParamFloat(material, prefix + "autoMinSlope", 0.0f),
                                 ParamFloat(material, prefix + "autoMaxSlope", 1.0f),
                                 ParamFloat(material, prefix + "autoSlopeFade", 0.1f),
                                 ParamFloat(material, prefix + "autoBlendStrength", 1.0f) };
    }
    return cb;
}

WaterCB BuildWaterCB(const asset::MaterialAsset* material,
                           const math::Matrix4& viewProjection,
                           float time,
                           const scene::WaterDetailNoise& detailNoise)
{
    WaterCB cb{};
    cb.worldMatrix = math::Matrix4::Identity();
    cb.wvpMatrix = viewProjection;
    const math::Vector3 shallow = ParamFloat3(material, "shallowColor", { 0.20f, 0.60f, 0.70f });
    const math::Vector3 deep    = ParamFloat3(material, "deepColor",    { 0.00f, 0.10f, 0.30f });
    cb.shallowColorDepth = { shallow.x, shallow.y, shallow.z, ParamFloat(material, "shallowDepth", 0.5f) };
    cb.deepColorDepth    = { deep.x, deep.y, deep.z, ParamFloat(material, "deepDepth", 5.0f) };
    cb.surfaceParams = { ParamFloat(material, "opacity", 0.85f),
                         ParamFloat(material, "reflectivity", 0.35f),
                         ParamFloat(material, "fresnelBias", 0.02f),
                         ParamFloat(material, "fresnelPower", 5.0f) };
    cb.normalParams = { 0.0f, 0.0f, detailNoise.derivativeScale,
                        ParamFloat(material, "normalStrength", 0.75f) };
    cb.timeParams   = { 0.0f, 0.0f, detailNoise.invTileCells, time };
    cb.foamParams = { ParamFloat(material, "foamThreshold", 0.3f),
                      ParamFloat(material, "foamFade", 0.5f),
                      ParamFloat(material, "foamStrength", 0.6f),
                      ParamFloat(material, "foamNoiseScale", 0.5f) };
    cb.refractionFlowParams = { ParamFloat(material, "refractionStrength", 0.02f),
                                ParamFloat(material, "flowSpeed", 0.3f), 0.0f, 0.0f };
    cb.detailParams = { ParamFloat(material, "detailScale", 0.35f),
                        ParamFloat(material, "detailSpeed", 0.6f),
                        ParamFloat(material, "detailStrength", 1.0f),
                        ParamFloat(material, "smoothness", 0.92f) };
    const math::Vector3 sss = ParamFloat3(material, "sssColor", { 0.12f, 0.50f, 0.46f });
    cb.sssParams = { sss.x, sss.y, sss.z, ParamFloat(material, "sssStrength", 0.6f) };
    // プレビューは IBL キューブを持たないので、空反射はフラット色へフォールバックさせる。
    // y は波の «群» の深さ。プレビューの波は極小 (振幅 0.012 m) なので、群まで掛けると
    // 水面が止まって見える瞬間ができる。ここは 0 のまま «常に同じうねり» で見せる。
    cb.reflectParams = { 0.0f, 0.0f, 0.0f, 0.35f };
    cb.flowParams    = { 1.0f, 0.0f,
                         (std::max)(ParamFloat(material, "detailAnisotropy", 2.0f), 1.0f),
                         (std::max)(ParamFloat(material, "detailWarp",       0.5f), 0.0f) };

    // 波が 1 本も指定されていない .mat でも «水面» に見せる。
    // WHY: 波の指定は Water コンポーネント側に持つことが多く、.mat だけ見ると
    //      全部ゼロ = 完全な鏡面になり、法線マップもフォームも読み取れない。
    cb.waveDir[0]    = { 1.0f, 0.0f, 0.0f, 0.0f };
    cb.waveParams[0] = { 0.012f, 0.55f, 1.1f, 0.0f };
    cb.waveDir[1]    = { 0.35f, 0.94f, 0.0f, 0.0f };
    cb.waveParams[1] = { 0.008f, 0.31f, 1.7f, 0.0f };
    // 方向広がりはプレビューの極小波では読み取れないうえ、頂点評価を 2 倍にする。切っておく。
    cb.waveShapeParams = { 0.0f, 0.0f, 0.0f, 0.0f };
    return cb;
}

// ── チャンネル表示 ────────────────────────────────────────────────────────

std::uint32_t ChannelModeValue(Channel channel)
{
    switch (channel) {
        case Channel::Normal:    return 1;
        case Channel::Roughness: return 2;
        case Channel::Metallic:  return 3;
        case Channel::Occlusion: return 4;
        case Channel::Emissive:  return 5;
        case Channel::Uv:        return 6;
        case Channel::Albedo:
        default:                 return 0;
    }
}

ChannelCB BuildChannelCB(const asset::MaterialAsset* material, const GpuData* gpu, Channel channel)
{
    ChannelCB cb{};
    const math::Vector4 albedo = material ? AlbedoColor(*material) : math::Vector4{ 0.8f, 0.8f, 0.8f, 1.0f };
    cb.baseColor = albedo;
    cb.pbr = { ParamFloat(material, "metallic", 0.0f),
               ParamFloat(material, "roughness", 0.65f),
               ParamFloat(material, "occlusionStrength", 1.0f),
               ParamFloat(material, "normalStrength", 1.0f) };
    cb.uv = { 1.0f, 1.0f, 0.0f, 0.0f };
    if (material) {
        if (const auto* tiling = FindParam(*material, "uvTiling"); tiling && tiling->size() >= 2) {
            cb.uv.x = (*tiling)[0];
            cb.uv.y = (*tiling)[1];
        }
        if (const auto* offset = FindParam(*material, "uvOffset"); offset && offset->size() >= 2) {
            cb.uv.z = (*offset)[0];
            cb.uv.w = (*offset)[1];
        }
    }
    const math::Vector3 emissive = ParamFloat3(material, "emissiveColor", { 1.0f, 1.0f, 1.0f });
    cb.emissive = { emissive.x, emissive.y, emissive.z, ParamFloat(material, "emissiveScale", 0.0f) };
    cb.mode = ChannelModeValue(channel);
    if (gpu) {
        for (std::uint32_t slot = 0; slot < 5 && slot < gpu->textures.size(); ++slot) {
            if (gpu->textures[slot].IsValid()) cb.textureMask |= (1u << slot);
        }
    }
    return cb;
}

// ── フレーム / 照明の共通セットアップ ─────────────────────────────────────
// WHY 切り出すか: Decal は «受け面» を、PostProcess は «入力シーン» を先に焼く。
//     どちらも本番と同じカメラ・同じ照明で焼かないと、後段のパスが別の絵を読む。

struct FrameState {
    renderer::Camera  camera;
    scene::PerFrameCB frame{};
    math::Vector3     center{};
    float             radius = 0.5f;
};

FrameState UploadFrame(renderer::ResourceManager& resources,
                       const Orbit& orbit,
                       const math::Vector3& center,
                       float radius)
{
    FrameState state;
    state.center = center;
    state.radius = std::max(radius, 0.0001f);
    const float distance = state.radius * std::max(orbit.distance, 0.05f);

    // WHY 望遠にしないか: FOV 30 + 遠距離はパースがほぼ消えて正射影に近づき、
    //     球が円板のように平坦に見える。FOV を広げてカメラを寄せる。
    const float cosPitch = std::cos(orbit.pitch);
    state.camera.m_position = center + math::Vector3{
        cosPitch * std::sin(orbit.yaw) * distance,
        std::sin(orbit.pitch) * distance,
        cosPitch * std::cos(orbit.yaw) * distance };
    state.camera.m_aspect = 1.0f;  // 描画先は常に正方形 RT
    state.camera.m_fovY   = 38.0f;
    state.camera.m_near   = 0.01f;
    state.camera.m_far    = std::max(10.0f, distance + state.radius * 6.0f);
    state.camera.LookAt(center);

    state.frame.view              = state.camera.GetViewMatrix();
    state.frame.projection        = state.camera.GetProjectionMatrix();
    state.frame.viewProjection    = state.camera.GetViewProjection();
    state.frame.invViewProjection = math::Matrix4::Inverse(state.frame.viewProjection);
    state.frame.cameraPos         = state.camera.m_position;
    state.frame.nearZ             = state.camera.m_near;
    state.frame.farZ              = state.camera.m_far;
    resources.Update(g_shared.frameCB, &state.frame, sizeof(state.frame));
    return state;
}

// ── 3 点照明リグ (キー / フィル / リム) ──
// WHY: 単一平行光 + 高いアンビエントだと球が円板に見える。アンビエントを落として
//      明暗差を作り、寒色フィルで陰側の丸みを、リムで輪郭を背景から分離する。
void UploadRig(renderer::ResourceManager& resources, const Rig& rig, const FrameState& fs)
{
    const RigProfile profile = ProfileFor(rig.preset);
    const float exposure = std::max(rig.exposure, 0.0f);
    const float radius = fs.radius;

    renderer::LightConstantsCB lightData{};
    const math::Vector3 keyOffset = RotateRig(
        fs.camera.m_position + math::Vector3{ radius * 1.4f, radius * 1.8f, radius * 0.8f } - fs.center,
        rig.yaw, rig.pitch);
    const math::Vector3 keyDirection = keyOffset.Normalized();
    lightData.lightDir = { -keyDirection.x, -keyDirection.y, -keyDirection.z };
    lightData.lightColor = profile.keyColor;
    lightData.lightIntensity = profile.keyIntensity * exposure / kLightUnitScale;
    lightData.ambientColor = profile.ambient * exposure;

    // WHY 距離補正を掛けるか: LightAttenuation は 1/dist^2 を含むので、そのままだと
    //     メッシュ半径で効きが変わる。range = radius*20 に対し dist は radius*3 前後で
    //     range 窓はほぼ 1.0 なので、逆二乗と LIGHT_UNIT_SCALE だけ打ち消せばよい。
    const auto placeLight = [&](renderer::PointLight& light,
                                const math::Vector3& offsetFromCenter,
                                const math::Vector3& color,
                                float targetIntensity) {
        const math::Vector3 rotated = RotateRig(offsetFromCenter, rig.yaw, rig.pitch);
        light.position = fs.center + rotated;
        light.color    = color;
        light.range    = radius * 20.0f;
        // シェーダー側の特異点ガード max(d*d, 0.01) と同じ下限を掛け、
        // 極小メッシュで補正が過剰にならないようにする。
        light.intensity = targetIntensity * exposure *
            std::max(rotated.LengthSq(), 0.01f) / kLightUnitScale;
    };
    placeLight(lightData.pointLights[0],
               { radius * 2.6f, -radius * 1.4f, -radius * 2.2f },
               profile.fillColor, profile.fillIntensity);
    placeLight(lightData.pointLights[1],
               { radius * 1.6f, radius * 2.4f, radius * 2.8f },
               profile.rimColor, profile.rimIntensity);
    lightData.pointLightCount = profile.rimIntensity > 0.0f ? 2 : 1;
    lightData.spotLightCount = 0;
    resources.Update(g_shared.lightCB, &lightData, sizeof(lightData));

    scene::ShadowConstantsCB shadowData{};
    shadowData.lightViewProjection = math::Matrix4::Translate({ 16.0f, 16.0f, 0.0f });
    shadowData.shadowMapTexelSize[0] = 0.0f;
    shadowData.shadowMapTexelSize[1] = 0.0f;
    // NDC 深度最大値 (1.0) をバイアスにすると depth - bias <= 0 が常に成立し、
    // SampleCmpLevelZero が必ず 1.0 (照らされている) を返して影を無効化できる。
    shadowData.shadowBias = 1.0f;
    resources.Update(g_shared.shadowCB, &shadowData, sizeof(shadowData));
}

// b0 / b3 / b4 を埋めたあとに呼ぶ、単色 Lit のメッシュ 1 枚。
// Decal の受け面と PostProcess の入力シーンがこれで «舞台» を作る。
void DrawNeutralMesh(renderer::IRenderer& renderer,
                     renderer::ResourceManager& resources,
                     const renderer::Mesh& mesh,
                     const math::Vector4& color)
{
    FallbackMaterialCB materialData{};
    materialData.albedo = color;
    materialData.textureMask = 0;
    resources.Update(g_shared.fallbackMaterialCB, &materialData, sizeof(materialData));

    renderer::DrawCall drawCall;
    drawCall.vertexBuffer = mesh.vertexBuffer;
    drawCall.indexBuffer  = mesh.indexBuffer;
    drawCall.indexCount   = mesh.indexCount;
    drawCall.vertexCount  = mesh.vertexCount;
    drawCall.shader        = g_shared.fallbackShader;
    drawCall.pipelineState = g_shared.solidPso;
    drawCall.constantBuffers[0]  = g_shared.frameCB;
    drawCall.constantBuffers[1]  = g_shared.objectCB;
    drawCall.constantBuffers[2]  = g_shared.fallbackMaterialCB;
    drawCall.constantBuffers[3]  = g_shared.lightCB;
    drawCall.constantBuffers[4]  = g_shared.shadowCB;
    drawCall.constantBuffers[9]  = g_shared.clusterCB;
    drawCall.constantBuffers[12] = g_shared.punctualShadowCB;
    renderer.Submit(drawCall, resources);
}

void UploadObjectCB(renderer::ResourceManager& resources, const math::Matrix4& world)
{
    scene::PerObjectCB objectData{};
    objectData.world = world;
    objectData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(world);
    resources.Update(g_shared.objectCB, &objectData, sizeof(objectData));
}

void UploadIdentityObjectCB(renderer::ResourceManager& resources)
{
    UploadObjectCB(resources, math::Matrix4::Identity());
}

renderer::ResourceHandle<renderer::PipelineStateTag> BlendPso(
    renderer::ResourceManager& resources, renderer::BlendMode blend)
{
    const auto make = [&](renderer::ResourceHandle<renderer::PipelineStateTag>& slot,
                          renderer::BlendMode mode) {
        if (!slot.IsValid()) {
            // WHY DEPTH_OFF か: プレビューには遮蔽物が無く、粒子どうしの前後も
            //     «重なって見える» ほうが素材を読み取りやすい。
            slot = resources.CreatePipelineState({
                renderer::RasterizerMode::SOLID_NOCULL, mode, renderer::DepthMode::DEPTH_OFF });
        }
        return slot;
    };
    switch (blend) {
        case renderer::BlendMode::ADDITIVE:
            return make(g_shared.additivePso, renderer::BlendMode::ADDITIVE);
        case renderer::BlendMode::PREMULTIPLIED:
            return make(g_shared.premultipliedPso, renderer::BlendMode::PREMULTIPLIED);
        default:
            return make(g_shared.alphaPso, renderer::BlendMode::ALPHA_BLEND);
    }
}

// ── UI マテリアル ─────────────────────────────────────────────────────────
// UI パスは b0 を CameraConstants ではなく UIConstants として使い、頂点も
// float2 pos / float2 uv / float4 color しか持たない。3D 経路とは別の描画にする。

// プレビューの Canvas 寸法 [px]。矩形の角丸・枠線は «何ピクセルぶん» で決まるので、
// 表示サイズに依らず同じ形になるよう固定値にする。
constexpr float kUiCanvasSize = 256.0f;
constexpr float kUiMargin     = 18.0f;

bool EnsureUiResources(renderer::ResourceManager& resources)
{
    if (!g_shared.uiPso.IsValid()) {
        g_shared.uiPso = resources.CreatePipelineState({
            renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::ALPHA_BLEND,
            renderer::DepthMode::DEPTH_OFF });
    }
    if (!g_shared.uiCB.IsValid())
        g_shared.uiCB = resources.CreateConstantBuffer(sizeof(UIConstantsCB));
    if (!g_shared.uiVertexBuffer.IsValid()) {
        const float lo = kUiMargin;
        const float hi = kUiCanvasSize - kUiMargin;
        const scene::UIVertex2D vertices[4] = {
            { { lo, lo }, { 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } },
            { { hi, lo }, { 1.0f, 0.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } },
            { { hi, hi }, { 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } },
            { { lo, hi }, { 0.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f } },
        };
        g_shared.uiVertexBuffer = resources.CreateVertexBuffer(
            vertices, sizeof(vertices), sizeof(scene::UIVertex2D));
    }
    if (!g_shared.uiIndexBuffer.IsValid()) {
        const std::uint32_t indices[6] = { 0, 1, 2, 0, 2, 3 };
        g_shared.uiIndexBuffer = resources.CreateIndexBuffer(indices, 6);
    }
    return g_shared.uiPso.IsValid() && g_shared.uiCB.IsValid() &&
           g_shared.uiVertexBuffer.IsValid() && g_shared.uiIndexBuffer.IsValid();
}

bool RenderUi(renderer::IRenderer& renderer,
              renderer::ResourceManager& resources,
              const RenderDesc& desc)
{
    if (!desc.gpu || !desc.gpu->shader.IsValid()) return false;
    if (!EnsureUiResources(resources) || !EnsureDefaultTextures(resources)) return false;

    const float rectSize = kUiCanvasSize - kUiMargin * 2.0f;
    UIConstantsCB uiData{};
    uiData.ortho  = math::Matrix4::Orthographic(0.0f, kUiCanvasSize, kUiCanvasSize, 0.0f, 0.0f, 1.0f);
    uiData.color  = { 1.0f, 1.0f, 1.0f, 1.0f };
    uiData.uvRect = { 0.0f, 0.0f, 1.0f, 1.0f };
    uiData.rect   = { rectSize, rectSize, 0.0f, 0.0f };
    resources.Update(g_shared.uiCB, &uiData, sizeof(uiData));

    renderer.SetRenderTarget(desc.target, resources);
    if (desc.clear) {
        renderer.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });
        renderer.ClearDepth();
    }

    renderer::DrawCall drawCall;
    drawCall.vertexBuffer = g_shared.uiVertexBuffer;
    drawCall.indexBuffer  = g_shared.uiIndexBuffer;
    drawCall.indexCount   = 6;
    drawCall.vertexCount  = 4;
    drawCall.shader        = desc.gpu->shader;
    drawCall.pipelineState = g_shared.uiPso;
    drawCall.constantBuffers[0] = g_shared.uiCB;
    drawCall.constantBuffers[2] = desc.gpu->materialCB;
    // 素材を持たない UI マテリアルでも図形は描ける。白 1px を差して
    // «テクスチャ未束縛で真っ黒» にならないようにする。
    drawCall.textures[0] = (!desc.gpu->textures.empty() && desc.gpu->textures[0].IsValid())
        ? desc.gpu->textures[0] : g_shared.whiteTexture;
    for (std::size_t i = 1; i < std::min(drawCall.textures.size(), desc.gpu->textures.size()); ++i)
        drawCall.textures[i] = desc.gpu->textures[i];

    renderer.Submit(drawCall, resources);
    renderer.SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>{}, resources);
    return true;
}

// ── Particle ─────────────────────────────────────────────────────────────
// ビルボードは ParticleBillboardVS が VS で展開する。CPU 側は «粒子 1 個 = 四隅»
// を ParticleVertex で積むだけ。本編の ParticlePass とまったく同じ入力になる。

// プレビューに置く粒子。1 個だけだと «重なったときの合成» が読めないので、
// 大きい 1 個を先に描き、その手前へ小さい 2 個を重ねる (深度は切ってあるので描画順が前後)。
struct PreviewParticle {
    math::Vector3 center;
    float         size;
    float         alpha;
};
constexpr std::array<PreviewParticle, 3> kPreviewParticles = {{
    { {  0.00f,  0.00f,  0.00f }, 0.70f, 1.00f },
    { { -0.22f, -0.10f,  0.12f }, 0.42f, 0.75f },
    { {  0.24f,  0.14f,  0.10f }, 0.34f, 0.65f },
}};

bool EnsureParticleGeometry(renderer::ResourceManager& resources)
{
    if (g_shared.particleVertexBuffer.IsValid() && g_shared.particleIndexBuffer.IsValid())
        return true;

    std::vector<ParticleVertex> vertices;
    std::vector<std::uint32_t>  indices;
    vertices.reserve(kPreviewParticles.size() * 4);
    indices.reserve(kPreviewParticles.size() * 6);
    for (const PreviewParticle& particle : kPreviewParticles) {
        const auto base = static_cast<std::uint32_t>(vertices.size());
        constexpr float kCornerUv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
        for (const auto& uv : kCornerUv) {
            ParticleVertex vertex{};
            vertex.center[0] = particle.center.x;
            vertex.center[1] = particle.center.y;
            vertex.center[2] = particle.center.z;
            vertex.uv[0] = uv[0];
            vertex.uv[1] = uv[1];
            vertex.color[0] = vertex.color[1] = vertex.color[2] = 1.0f;
            vertex.color[3] = particle.alpha;
            vertex.size = particle.size;
            vertex.rotation = 0.0f;
            vertex.uvRect[2] = vertex.uvRect[3] = 1.0f;
            vertex.nextUvRect[2] = vertex.nextUvRect[3] = 1.0f;
            vertices.push_back(vertex);
        }
        indices.insert(indices.end(),
                       { base, base + 1, base + 2, base, base + 2, base + 3 });
    }

    g_shared.particleVertexBuffer = resources.CreateVertexBuffer(
        vertices.data(), vertices.size() * sizeof(ParticleVertex), sizeof(ParticleVertex));
    g_shared.particleIndexBuffer = resources.CreateIndexBuffer(
        indices.data(), static_cast<std::uint32_t>(indices.size()));
    return g_shared.particleVertexBuffer.IsValid() && g_shared.particleIndexBuffer.IsValid();
}

bool RenderParticle(renderer::IRenderer& renderer,
                    renderer::ResourceManager& resources,
                    const RenderDesc& desc)
{
    if (!desc.gpu || !desc.gpu->shader.IsValid()) return false;
    if (!EnsureShared(resources, false) || !EnsureParticleGeometry(resources)) return false;
    if (!g_shared.particleCB.IsValid())
        g_shared.particleCB = resources.CreateConstantBuffer(sizeof(ParticleRenderCB));
    if (!g_shared.particleCB.IsValid()) return false;

    const FrameState frameState = UploadFrame(resources, desc.orbit, math::Vector3::ZERO, 0.5f);
    UploadRig(resources, desc.rig, frameState);
    UploadIdentityObjectCB(resources);

    const renderer::BlendMode blend = desc.material
        ? desc.material->blendMode : renderer::BlendMode::ALPHA_BLEND;

    ParticleRenderCB particleData{};
    particleData.maxParticles = static_cast<std::uint32_t>(kPreviewParticles.size());
    particleData.screenWidth  = static_cast<float>(kEffectRtSize);
    particleData.screenHeight = static_cast<float>(kEffectRtSize);
    // 影・ソフトパーティクル・歪みはシーンの深度とカラーを要る。プレビューには
    // どちらも無いので切る (未束縛 SRV を読むと真っ黒になる)。
    particleData.softParticles = 0;
    particleData.shadowStrength = 0.0f;
    particleData.selfShadowStrength = 0.0f;
    particleData.effectsFlags = 0;
    if (blend == renderer::BlendMode::ADDITIVE)
        particleData.effectsFlags |= scene::kParticleFxAdditive;
    if (blend == renderer::BlendMode::PREMULTIPLIED)
        particleData.effectsFlags |= scene::kParticleFxPremultiplied;
    if (desc.material) {
        if (auto it = desc.material->textures.find("albedo");
            it != desc.material->textures.end() && !it->second.empty() &&
            scene::IsEffectTextureSrgb(it->second)) {
            particleData.effectsFlags |= scene::kParticleFxSrgbTexture;
        }
        particleData.tintColor = AlbedoColor(*desc.material);
    }
    resources.Update(g_shared.particleCB, &particleData, sizeof(particleData));

    renderer.SetRenderTarget(desc.target, resources);
    if (desc.clear) {
        renderer.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });
        renderer.ClearDepth();
    }

    renderer::DrawCall drawCall;
    drawCall.vertexBuffer = g_shared.particleVertexBuffer;
    drawCall.indexBuffer  = g_shared.particleIndexBuffer;
    drawCall.indexCount   = static_cast<std::uint32_t>(kPreviewParticles.size() * 6);
    drawCall.vertexCount  = static_cast<std::uint32_t>(kPreviewParticles.size() * 4);
    drawCall.shader        = desc.gpu->shader;
    drawCall.pipelineState = BlendPso(resources, blend);
    drawCall.constantBuffers[0]  = g_shared.frameCB;
    drawCall.constantBuffers[1]  = g_shared.objectCB;
    drawCall.constantBuffers[2]  = desc.gpu->materialCB;
    drawCall.constantBuffers[3]  = g_shared.lightCB;
    drawCall.constantBuffers[4]  = g_shared.shadowCB;
    drawCall.constantBuffers[9]  = g_shared.clusterCB;
    drawCall.constantBuffers[12] = g_shared.punctualShadowCB;
    drawCall.constantBuffers[scene::kParticleConstantSlot] = g_shared.particleCB;
    const std::size_t count = std::min(drawCall.textures.size(), desc.gpu->textures.size());
    for (std::size_t i = 0; i < count; ++i) drawCall.textures[i] = desc.gpu->textures[i];
    if (!drawCall.textures[0].IsValid() && EnsureDefaultTextures(resources))
        drawCall.textures[0] = g_shared.whiteTexture;

    renderer.Submit(drawCall, resources);
    renderer.SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>{}, resources);
    return true;
}

// ── Trail ────────────────────────────────────────────────────────────────
// Trail.hlsl の b2 は材質ではなく TrailConstants (エンジンが埋める)。帯の形も
// CPU で作るので、ここは ParticlePass / TrailRenderPass と同じ形を一本作るだけ。

constexpr int kTrailSegments = 48;

bool EnsureTrailGeometry(renderer::ResourceManager& resources)
{
    if (g_shared.trailVertexBuffer.IsValid() && g_shared.trailIndexBuffer.IsValid())
        return true;

    // XZ 平面の弧。既定カメラが斜め上から見るので、帯の «面» がそのまま見える。
    std::vector<TrailVertex> vertices;
    std::vector<std::uint32_t> indices;
    vertices.reserve((kTrailSegments + 1) * 2);
    for (int i = 0; i <= kTrailSegments; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kTrailSegments);
        const float angle = (t - 0.5f) * 2.4f;
        const math::Vector3 center{ std::sin(angle) * 0.55f, 0.0f, std::cos(angle) * 0.35f - 0.3f };
        // 先端へ向かって細くする。幅が一定だと «軌跡» ではなく «板» に見える。
        const float halfWidth = 0.20f * (1.0f - t * 0.85f);
        const math::Vector3 across{ std::cos(angle), 0.0f, -std::sin(angle) };
        vertices.push_back({ center + across * halfWidth, t, 0.0f, t });
        vertices.push_back({ center - across * halfWidth, t, 1.0f, t });
        if (i == 0) continue;
        const auto base = static_cast<std::uint32_t>((i - 1) * 2);
        indices.insert(indices.end(),
                       { base, base + 1, base + 2, base + 1, base + 3, base + 2 });
    }

    g_shared.trailVertexBuffer = resources.CreateVertexBuffer(
        vertices.data(), vertices.size() * sizeof(TrailVertex), sizeof(TrailVertex));
    g_shared.trailIndexBuffer = resources.CreateIndexBuffer(
        indices.data(), static_cast<std::uint32_t>(indices.size()));
    return g_shared.trailVertexBuffer.IsValid() && g_shared.trailIndexBuffer.IsValid();
}

math::Vector4 TrailParamColor(const asset::MaterialAsset* material,
                              std::string_view name,
                              const math::Vector4& fallback)
{
    if (!material) return fallback;
    const auto* values = FindParam(*material, name);
    if (!values || values->size() < 3) return fallback;
    return { (*values)[0], (*values)[1], (*values)[2],
             values->size() >= 4 ? (*values)[3] : 1.0f };
}

bool RenderTrail(renderer::IRenderer& renderer,
                 renderer::ResourceManager& resources,
                 const RenderDesc& desc)
{
    if (!desc.gpu || !desc.gpu->shader.IsValid()) return false;
    if (!EnsureShared(resources, false) || !EnsureTrailGeometry(resources)) return false;
    if (!g_shared.trailCB.IsValid())
        g_shared.trailCB = resources.CreateConstantBuffer(sizeof(TrailCB));
    if (!g_shared.trailCB.IsValid()) return false;

    const FrameState frameState = UploadFrame(resources, desc.orbit, math::Vector3::ZERO, 0.6f);
    UploadRig(resources, desc.rig, frameState);
    UploadIdentityObjectCB(resources);

    TrailCB trailData{};
    // 帯の色は TrailComponent が持つ値で、.mat には無いことが多い。
    // 同名の [params] があればそれを使い、無ければ «白 → 透明» の既定で形を見せる。
    trailData.colorStart = TrailParamColor(desc.material, "colorStart", { 1.0f, 1.0f, 1.0f, 1.0f });
    trailData.colorEnd   = TrailParamColor(desc.material, "colorEnd",   { 1.0f, 1.0f, 1.0f, 0.0f });
    trailData.uvScrollSpeed = ParamFloat(desc.material, "uvScrollSpeed", 0.0f);
    trailData.uvTiling      = ParamFloat(desc.material, "uvTiling", 1.0f);
    trailData.time          = desc.time;
    if (desc.material) {
        if (auto it = desc.material->textures.find("albedo");
            it != desc.material->textures.end() && !it->second.empty() &&
            scene::IsEffectTextureSrgb(it->second)) {
            trailData.flags |= scene::kTrailFlagSrgbTexture;
        }
    }
    resources.Update(g_shared.trailCB, &trailData, sizeof(trailData));

    renderer.SetRenderTarget(desc.target, resources);
    if (desc.clear) {
        renderer.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });
        renderer.ClearDepth();
    }

    renderer::DrawCall drawCall;
    drawCall.vertexBuffer = g_shared.trailVertexBuffer;
    drawCall.indexBuffer  = g_shared.trailIndexBuffer;
    drawCall.indexCount   = static_cast<std::uint32_t>(kTrailSegments * 6);
    drawCall.vertexCount  = static_cast<std::uint32_t>((kTrailSegments + 1) * 2);
    drawCall.shader        = desc.gpu->shader;
    drawCall.pipelineState = BlendPso(resources,
        desc.material ? desc.material->blendMode : renderer::BlendMode::ALPHA_BLEND);
    drawCall.constantBuffers[0] = g_shared.frameCB;
    drawCall.constantBuffers[1] = g_shared.objectCB;
    drawCall.constantBuffers[2] = g_shared.trailCB;
    drawCall.constantBuffers[3] = g_shared.lightCB;
    const std::size_t count = std::min(drawCall.textures.size(), desc.gpu->textures.size());
    for (std::size_t i = 0; i < count; ++i) drawCall.textures[i] = desc.gpu->textures[i];
    if (!drawCall.textures[0].IsValid() && EnsureDefaultTextures(resources))
        drawCall.textures[0] = g_shared.whiteTexture;

    renderer.Submit(drawCall, resources);
    renderer.SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>{}, resources);
    return true;
}

// ── Decal ────────────────────────────────────────────────────────────────
// デカールは «受け面の深度からワールド座標を復元して OBB へ投影する» ので、
// 投影先が要る。受け面を 1 枚別の RT へ焼き、その深度を読ませて本番と同じ経路を通す。

bool EnsureSceneRT(renderer::ResourceManager& resources)
{
    if (!g_shared.sceneRT.IsValid())
        g_shared.sceneRT = resources.CreateRenderTarget(kEffectRtSize, kEffectRtSize);
    return g_shared.sceneRT.IsValid();
}

bool RenderDecal(renderer::IRenderer& renderer,
                 renderer::ResourceManager& resources,
                 const RenderDesc& desc)
{
    if (!desc.gpu || !desc.gpu->shader.IsValid()) return false;
    if (!EnsureShared(resources, true) || !EnsureSceneRT(resources)) return false;
    if (!g_shared.decalCB.IsValid())
        g_shared.decalCB = resources.CreateConstantBuffer(sizeof(DecalCB));
    if (!g_shared.decalCB.IsValid()) return false;

    renderer::Mesh* receiver = GridMesh(resources, 8);
    if (!receiver) return false;

    // WHY 受け面を投影ボリュームより広く取るか: 平面と箱を同じ大きさにすると
    //     デカールが画面いっぱいになり «どこまで乗るか» が見えない。周囲を残して
    //     投影範囲と角の落ち方を読めるようにする。
    constexpr float kReceiverScale = 2.2f;
    const FrameState frameState =
        UploadFrame(resources, desc.orbit, math::Vector3::ZERO, kReceiverScale * 0.5f);
    UploadRig(resources, desc.rig, frameState);
    UploadObjectCB(resources, math::Matrix4::Scale(
        { kReceiverScale, kReceiverScale, kReceiverScale }));

    // 受け面 (中性グレー) を 2 回焼く。1 回目は深度を SRV として読むため別 RT へ、
    // 2 回目は «デカールが乗る下地» としてプレビュー本体へ。
    // WHY 別 RT が要るか: 描画先の深度バッファを同時に SRV として読めないため。
    constexpr math::Vector4 kReceiverColor{ 0.62f, 0.63f, 0.66f, 1.0f };
    renderer.SetRenderTarget(g_shared.sceneRT, resources);
    renderer.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });
    renderer.ClearDepth();
    DrawNeutralMesh(renderer, resources, *receiver, kReceiverColor);

    renderer.SetRenderTarget(desc.target, resources);
    if (desc.clear) {
        renderer.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });
        renderer.ClearDepth();
    }
    DrawNeutralMesh(renderer, resources, *receiver, kReceiverColor);

    // 投影ボリュームは受け面 (1x1 の平面) をちょうど覆う立方体。
    // デカールローカルは [-0.5, 0.5]^3 で、+Y が投影軸・+X/+Z が UV 軸。
    DecalCB decalData{};
    decalData.invDecalWorld     = math::Matrix4::Identity();
    decalData.decalTangent      = { 1.0f, 0.0f, 0.0f };
    decalData.decalBitangent    = { 0.0f, 0.0f, 1.0f };
    decalData.decalNormal       = { 0.0f, 1.0f, 0.0f };
    decalData.alpha             = 1.0f;
    decalData.angleFadeStrength = 0.0f; // 受け面は平らなので角度フェードは掛けない
    decalData.flags             = 0;    // 受信レイヤーフィルタなし (t13 を読ませない)
    decalData.receiverLayerMask = ~0u;
    resources.Update(g_shared.decalCB, &decalData, sizeof(decalData));

    // 頂点バッファを持たない全画面三角形 (DecalVertexMain が SV_VertexID から組む)。
    renderer::DrawCall drawCall;
    drawCall.shader        = desc.gpu->shader;
    drawCall.pipelineState = BlendPso(resources, renderer::BlendMode::ALPHA_BLEND);
    drawCall.vertexCount   = 3;
    drawCall.constantBuffers[0]  = g_shared.frameCB;
    drawCall.constantBuffers[2]  = desc.gpu->materialCB;
    drawCall.constantBuffers[3]  = g_shared.lightCB;
    drawCall.constantBuffers[10] = g_shared.decalCB;
    const std::size_t count = std::min(drawCall.textures.size(), desc.gpu->textures.size());
    for (std::size_t i = 0; i < count; ++i) drawCall.textures[i] = desc.gpu->textures[i];
    drawCall.textures[7] = resources.GetDepthTexture(g_shared.sceneRT); // TEX_DEPTH

    renderer.Submit(drawCall, resources);
    renderer.SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>{}, resources);
    return true;
}

// ── PostProcess ──────────────────────────────────────────────────────────
// 全画面フィルタは «通す絵» が無いと何も分からない。標準の球をいつものリグで
// 焼き、その結果を入力 (t5) にして材質のシェーダーを 1 回通す。

bool RenderPostProcess(renderer::IRenderer& renderer,
                       renderer::ResourceManager& resources,
                       const RenderDesc& desc)
{
    if (!desc.gpu || !desc.gpu->shader.IsValid()) return false;
    if (!EnsureShared(resources, true) || !EnsureSceneRT(resources)) return false;
    if (!g_shared.postProcCB.IsValid())
        g_shared.postProcCB = resources.CreateConstantBuffer(sizeof(PostProcCB));
    if (!g_shared.postProcCB.IsValid()) return false;

    renderer::Mesh* subject = PrimitiveFor(resources, Shape::Sphere);
    if (!subject) return false;

    const FrameState frameState = UploadFrame(resources, desc.orbit, math::Vector3::ZERO, 0.5f);
    UploadRig(resources, desc.rig, frameState);
    UploadIdentityObjectCB(resources);

    renderer.SetRenderTarget(g_shared.sceneRT, resources);
    renderer.Clear({ 0.06f, 0.07f, 0.09f, 1.0f });
    renderer.ClearDepth();
    DrawNeutralMesh(renderer, resources, *subject, { 0.62f, 0.66f, 0.72f, 1.0f });

    PostProcCB postProcData = scene::MakeScreenPostProcCB(kEffectRtSize, kEffectRtSize);
    postProcData.time            = desc.time;
    postProcData.exposure        = 1.0f;
    postProcData.customIntensity = 1.0f;
    postProcData.customBlend     = 1.0f;
    // customPassInfo.x は «縮小して走った割合»。等倍で焼くので 1。
    postProcData.customPassInfo[0] = 1.0f;

    // .mat の [params] を b5 の custom* へ名前で束縛する。オフセットはリフレクション
    // (ShaderDescriptor::postProcessVars) 由来なので、ここで名前を書き並べずに済む。
    if (auto* shader = resources.Get(desc.gpu->shader); shader && desc.material) {
        auto* raw = reinterpret_cast<std::uint8_t*>(&postProcData);
        for (const auto& variable : shader->GetDescriptor().postProcessVars) {
            if (variable.varType != renderer::ShaderVarType::Float) continue;
            if (variable.offset + variable.size > sizeof(PostProcCB)) continue;
            const auto* values = FindParam(*desc.material, variable.name);
            if (!values || values->empty()) continue;
            const std::size_t count = std::min<std::size_t>(variable.columns, values->size());
            std::memcpy(raw + variable.offset, values->data(), count * sizeof(float));
        }
    }
    resources.Update(g_shared.postProcCB, &postProcData, sizeof(postProcData));

    renderer.SetRenderTarget(desc.target, resources);
    if (desc.clear) {
        renderer.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });
        renderer.ClearDepth();
    }

    renderer::DrawCall drawCall;
    drawCall.shader        = desc.gpu->shader;
    drawCall.pipelineState = g_shared.solidPso;
    drawCall.vertexCount   = 3;
    drawCall.constantBuffers[0] = g_shared.frameCB;
    drawCall.constantBuffers[2] = desc.gpu->materialCB;
    drawCall.constantBuffers[5] = g_shared.postProcCB;
    drawCall.textures[5] = resources.GetColorTexture(g_shared.sceneRT); // TEX_GBUFFER0
    // 深度を読むフィルタ (被写界深度・フォグ) も «この絵» の深度で通せるようにする。
    drawCall.textures[7] = resources.GetDepthTexture(g_shared.sceneRT);

    renderer.Submit(drawCall, resources);
    renderer.SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>{}, resources);
    return true;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────

Flavor DetectFlavor(const asset::MaterialAsset& material)
{
    const std::string lower = Lower(material.shaderPath);

    // 先に «プレビューの形を作れない» シェーダーを落とす。render_path より
    // シェーダー本体の入力のほうが強い制約で、ここを後回しにすると
    // render_path = 'trail' の MeshTrail をリボンの頂点で描いて崩す。
    //
    //   MeshTrail / SkinnedMeshTrail … 帯ではなくメッシュを流す (TrailVertex ではない)
    //   GPU パーティクル             … 頂点バッファを持たず StructuredBuffer から引く
    if (lower.find("meshtrail") != std::string::npos) return Flavor::Unsupported;
    if (lower.find("/effects/particlegpu") != std::string::npos ||
        lower.find("/effects/particlereactivegpu") != std::string::npos ||
        lower.find("/effects/particlegpumesh") != std::string::npos)
        return Flavor::Unsupported;

    // 以降の判定は render_path を信頼元にする。
    // WHY: Particle / Trail / Decal の .mat は MeshRenderer と頂点入力も定数バッファも
    //      違うため、3D メッシュのプレビューへ流すと不正な IA レイアウトになる。
    switch (material.renderPath) {
        case asset::RenderPath::UI:          return Flavor::Ui;
        case asset::RenderPath::Particle:    return Flavor::Particle;
        case asset::RenderPath::Trail:       return Flavor::Trail;
        case asset::RenderPath::Decal:       return Flavor::Decal;
        case asset::RenderPath::PostProcess: return Flavor::PostProcess;
        default: break;
    }
    if (material.meshType == asset::MeshType::Skinned) return Flavor::Skinned;

    // render_path = 'auto' のまま置き場所で意図を示している .mat の救済。
    if (lower.find("/ui/") != std::string::npos)                return Flavor::Ui;
    if (lower.find("/postprocess/") != std::string::npos)       return Flavor::PostProcess;
    if (lower.find("/material/decal/") != std::string::npos)    return Flavor::Decal;
    if (lower.find("/effects/trail") != std::string::npos)      return Flavor::Trail;
    if (lower.find("/effects/particle") != std::string::npos)   return Flavor::Particle;
    if (lower.find("/effects/raindrop") != std::string::npos)   return Flavor::Particle;
    if (lower.find("/material/effects/") != std::string::npos)  return Flavor::Particle;
    if (lower.find("/water/") != std::string::npos)             return Flavor::Water;
    if (lower.find("/terrain/") != std::string::npos)           return Flavor::Terrain;
    if (lower.find("/material/skinned/") != std::string::npos)  return Flavor::Skinned;
    return Flavor::Surface;
}

const char* UnsupportedBadge(const asset::MaterialAsset& material)
{
    switch (material.renderPath) {
        case asset::RenderPath::Particle:    return "PARTICLE";
        case asset::RenderPath::Trail:       return "TRAIL";
        case asset::RenderPath::UI:          return "UI";
        case asset::RenderPath::Decal:       return "DECAL";
        case asset::RenderPath::PostProcess: return "POST";
        default: break;
    }
    // render_path = 'auto' のまま Effects へ置いてある .mat はここに来る。
    const std::string lower = Lower(material.shaderPath);
    if (lower.find("decal") != std::string::npos)    return "DECAL";
    if (lower.find("trail") != std::string::npos)    return "TRAIL";
    if (lower.find("particle") != std::string::npos) return "PARTICLE";
    return "FX";
}

math::Vector4 SwatchColor(const asset::MaterialAsset& material)
{
    static constexpr const char* kPriority[] = {
        "base_color", "baseColor", "albedo", "coreColor", "tint", "color",
        "startColor", "mainColor", "emissiveColor", "edgeColor",
    };
    const std::vector<float>* found = nullptr;
    for (const char* name : kPriority) {
        if (auto it = material.params.find(name); it != material.params.end() && it->second.size() >= 3) {
            found = &it->second;
            break;
        }
    }
    if (!found) {
        // 名前が独自でも «...Color» は色として扱える。params は unordered なので、
        // 名前の小さい方に決めておかないと起動のたびに色が変わる。
        const std::string* pick = nullptr;
        for (const auto& [name, values] : material.params) {
            if (values.size() < 3) continue;
            if (!name.ends_with("Color") && !name.ends_with("color")) continue;
            if (pick == nullptr || name < *pick) { pick = &name; found = &values; }
        }
    }
    if (!found) return { 0.55f, 0.58f, 0.66f, 1.0f };

    const float peak = (std::max)({ (*found)[0], (*found)[1], (*found)[2], 1.0f });
    return { (*found)[0] / peak, (*found)[1] / peak, (*found)[2] / peak, 1.0f };
}

math::Vector4 AlbedoColor(const asset::MaterialAsset& material)
{
    const auto find = [&material]() -> const std::vector<float>* {
        for (const char* name : { "base_color", "baseColor", "albedo" }) {
            if (auto it = material.params.find(name); it != material.params.end() && it->second.size() >= 3)
                return &it->second;
        }
        return nullptr;
    };
    if (const std::vector<float>* values = find()) {
        const float alpha = values->size() >= 4 ? (*values)[3] : 1.0f;
        return { (*values)[0], (*values)[1], (*values)[2], alpha };
    }
    return { 0.20f, 0.70f, 0.80f, 1.0f };
}

std::string RepresentativeTexturePath(const asset::MaterialAsset& material)
{
    static constexpr const char* kPriority[] = {
        "albedo", "base_color", "diffuse", "layer0_diffuse", "foamTex"
    };
    for (const char* slot : kPriority) {
        if (auto it = material.textures.find(slot); it != material.textures.end() && !it->second.empty())
            return it->second;
    }
    for (const auto& [slot, path] : material.textures) {
        if (!path.empty()) return path;
    }
    return {};
}

std::string TextureLoadPath(const std::string& path, std::string_view projectRoot)
{
    const std::string normalized = util::FileSystem::NormalizePathSeparators(path);
    const std::string root = util::FileSystem::NormalizePathSeparators(std::string(projectRoot));
    // .mat 内のテクスチャ参照は Assets/ 相対で保存される。
    if (normalized.starts_with("Assets/") && !root.empty()) return root + "/" + normalized;
    return normalized;
}

const char* ShapeLabel(Shape shape)
{
    switch (shape) {
        case Shape::Cube:     return "Cube";
        case Shape::Plane:    return "Plane";
        case Shape::Quad:     return "Quad";
        case Shape::Cylinder: return "Cylinder";
        case Shape::Cone:     return "Cone";
        case Shape::Torus:    return "Torus";
        case Shape::Capsule:  return "Capsule";
        case Shape::Sphere:
        default:              return "Sphere";
    }
}

const char* ChannelLabel(Channel channel)
{
    switch (channel) {
        case Channel::Albedo:    return "Albedo";
        case Channel::Normal:    return "Normal";
        case Channel::Roughness: return "Roughness";
        case Channel::Metallic:  return "Metallic";
        case Channel::Occlusion: return "Occlusion";
        case Channel::Emissive:  return "Emissive";
        case Channel::Uv:        return "UV Checker";
        case Channel::Wireframe: return "Wireframe";
        case Channel::Shaded:
        default:                 return "Shaded";
    }
}

const char* LightPresetLabel(LightPreset preset)
{
    switch (preset) {
        case LightPreset::Outdoor: return "Outdoor";
        case LightPreset::Night:   return "Night";
        case LightPreset::Flat:    return "Flat";
        case LightPreset::Studio:
        default:                   return "Studio";
    }
}

bool ChannelSupported(Flavor flavor, Channel channel)
{
    if (channel == Channel::Shaded) return true;
    if (flavor == Flavor::Unsupported) return false;
    // Wireframe は材質自身のシェーダーのまま RasterizerMode だけ差し替える経路なので、
    // 3D メッシュを焼く Flavor にしか効かない (矩形・ビルボード・リボン・全画面は対象外)。
    if (channel == Channel::Wireframe) return !UsesOwnGeometry(flavor);
    // Terrain / Water / UI / エフェクト系はテクスチャスロットの意味が標準と違い、
    // MaterialChannel.hlsl の t0-t4 の前提が成立しない。
    return flavor == Flavor::Surface || flavor == Flavor::Skinned;
}

bool UsesOwnGeometry(Flavor flavor)
{
    switch (flavor) {
        case Flavor::Ui:
        case Flavor::Particle:
        case Flavor::Trail:
        case Flavor::Decal:
        case Flavor::PostProcess: return true;
        default:                  return false;
    }
}

renderer::Mesh* ShapeMesh(renderer::ResourceManager& resources, Shape shape, Flavor flavor)
{
    switch (flavor) {
        case Flavor::Skinned: return SkinnedCopy(resources, shape);
        // Terrain / Water は «地面» と «水面» なので形状指定を無視して平面へ焼く。
        case Flavor::Water:   return WaterGrid(resources, 64);
        case Flavor::Terrain: return GridMesh(resources, 32);
        case Flavor::Unsupported: return nullptr;
        // 矩形・ビルボード・リボン・全画面は Render が自前で持つ。
        default:
            return UsesOwnGeometry(flavor) ? nullptr : PrimitiveFor(resources, shape);
    }
}

bool BuildGpuData(GpuData& gpu,
                  const asset::MaterialAsset& material,
                  renderer::ResourceManager& resources,
                  std::string_view projectRoot)
{
    asset::MaterialAsset fallbackAsset;
    const asset::MaterialAsset* renderAsset = &material;
    if (material.shaderPath.empty()) {
        fallbackAsset.shaderPath = kFallbackShader;
        fallbackAsset.params["albedo"] = { 1.0f, 0.0f, 1.0f, 1.0f };
        renderAsset = &fallbackAsset;
    }

    const std::string shaderPath = renderAsset->shaderPath;
    if (shaderPath != gpu.shaderPath) {
        gpu.shaderPath = shaderPath;
        gpu.shader = {};
        if (gpu.materialCB.IsValid()) {
            resources.Release(gpu.materialCB);
            gpu.materialCB = {};
        }
        gpu.textures.clear();
        gpu.paramData.clear();
    }

    if (!gpu.shader.IsValid()) gpu.shader = resources.LoadShader(gpu.shaderPath);
    auto* shader = resources.Get(gpu.shader);
    if (!shader) return false;

    const renderer::ShaderDescriptor& descriptor = shader->GetDescriptor();
    const Flavor flavor = DetectFlavor(*renderAsset);
    if (flavor == Flavor::Unsupported) return false;

    // b2 をエンジンが所有するシェーダーは «MaterialConstants» という名前の cbuffer を
    // 持たないため、リフレクションが空で返る。これは «壊れている» ではないので通す。
    // (Terrain は b1 の TerrainObjectCB、Trail は b2 の TrailConstants が正本)
    const bool engineOwnsMaterialCB = flavor == Flavor::Terrain || flavor == Flavor::Trail;
    if (!descriptor.IsValid() && !engineOwnsMaterialCB) return false;

    gpu.textures.assign(16, {});
    for (const auto& binding : descriptor.textures) {
        if (binding.slot >= gpu.textures.size()) continue;
        const std::string path = FindTexturePath(*renderAsset, binding);
        if (path.empty()) continue;
        gpu.textures[binding.slot] = resources.LoadTexture(TextureLoadPath(path, projectRoot));
    }
    if (!descriptor.IsValid()) {
        // cbuffer のリフレクションが無い分、標準スロット名だけは名前で解決しておく。
        // 素材が 1 枚も付かないと «何の絵か» が消えてしまう。
        for (std::uint32_t slot = 0; slot < 5; ++slot) {
            if (gpu.textures[slot].IsValid()) continue;
            auto it = renderAsset->textures.find(kSlotNames[slot]);
            if (it != renderAsset->textures.end() && !it->second.empty())
                gpu.textures[slot] = resources.LoadTexture(TextureLoadPath(it->second, projectRoot));
        }
    }
    if (flavor == Flavor::Terrain) {
        const auto loadSlot = [&](std::uint32_t slot, const std::string& name) {
            auto it = renderAsset->textures.find(name);
            if (it != renderAsset->textures.end() && !it->second.empty())
                gpu.textures[slot] = resources.LoadTexture(TextureLoadPath(it->second, projectRoot));
        };
        loadSlot(0, "splatmap");
        for (std::uint32_t layer = 0; layer < 4; ++layer) {
            const std::string prefix = "layer" + std::to_string(layer);
            loadSlot(1 + layer, prefix + "_diffuse");
            loadSlot(5 + layer, prefix + "_normal");
            loadSlot(9 + layer, prefix + "_ao_roughness");
        }
    }

    if (!descriptor.IsValid()) {
        if (gpu.materialCB.IsValid()) resources.Release(gpu.materialCB);
        gpu.materialCB = {};
        gpu.paramData.clear();
        return true;
    }

    gpu.paramData.assign(descriptor.cbufferSize, 0u);
    InitDefaultParams(descriptor, gpu.paramData);
    ApplyAssetParams(*renderAsset, descriptor, gpu.paramData);

    if (descriptor.textureMaskOffset != UINT32_MAX &&
        descriptor.textureMaskOffset + sizeof(std::uint32_t) <= gpu.paramData.size()) {
        std::uint32_t mask = 0;
        for (const auto& binding : descriptor.textures) {
            if (binding.slot >= 8 || binding.slot >= gpu.textures.size()) continue;
            if (gpu.textures[binding.slot].IsValid()) mask |= (1u << binding.slot);
        }
        std::memcpy(gpu.paramData.data() + descriptor.textureMaskOffset, &mask, sizeof(mask));
    }

    if (!gpu.materialCB.IsValid())
        gpu.materialCB = resources.CreateConstantBuffer(descriptor.cbufferSize);
    if (!gpu.materialCB.IsValid()) return false;
    resources.Update(gpu.materialCB, gpu.paramData.data(), gpu.paramData.size());
    return true;
}

void ResetGpuData(GpuData& gpu, renderer::ResourceManager* resources)
{
    gpu.shaderPath.clear();
    gpu.shader = {};
    if (resources && gpu.materialCB.IsValid()) resources->Release(gpu.materialCB);
    gpu.materialCB = {};
    gpu.textures.clear();
    gpu.paramData.clear();
}

bool Render(renderer::IRenderer& renderer, renderer::ResourceManager& resources, const RenderDesc& desc)
{
    if (!desc.target.IsValid()) return false;
    // 自前の形を持つ経路は先に分岐させる。ここから下はメッシュ 1 枚の描画に閉じる。
    switch (desc.flavor) {
        case Flavor::Ui:          return RenderUi(renderer, resources, desc);
        case Flavor::Particle:    return RenderParticle(renderer, resources, desc);
        case Flavor::Trail:       return RenderTrail(renderer, resources, desc);
        case Flavor::Decal:       return RenderDecal(renderer, resources, desc);
        case Flavor::PostProcess: return RenderPostProcess(renderer, resources, desc);
        case Flavor::Unsupported: return false;
        default: break;
    }
    if (!desc.mesh) return false;
    const renderer::Mesh& mesh = *desc.mesh;
    if (!mesh.vertexBuffer.IsValid() || !mesh.indexBuffer.IsValid()) return false;

    const bool useMaterial = desc.gpu && desc.gpu->shader.IsValid();
    if (!EnsureShared(resources, !useMaterial)) return false;

    const bool useChannel = useMaterial && desc.channel != Channel::Shaded &&
                            desc.channel != Channel::Wireframe &&
                            ChannelSupported(desc.flavor, desc.channel);
    if (useChannel && !g_shared.channelShader.IsValid())
        g_shared.channelShader = resources.LoadShader(kChannelShader);
    const bool channelReady = useChannel && g_shared.channelShader.IsValid();

    const bool wireframe = desc.channel == Channel::Wireframe &&
                           ChannelSupported(desc.flavor, Channel::Wireframe);
    if (wireframe && !g_shared.wireframePso.IsValid()) {
        g_shared.wireframePso = resources.CreatePipelineState({
            renderer::RasterizerMode::WIREFRAME,
            renderer::BlendMode::OPAQUE_BLEND,
            renderer::DepthMode::DEPTH_ON });
    }

    const math::Vector3 center = (desc.overrideRadius >= 0.0f) ? desc.overrideCenter : BoundsCenter(mesh);
    const float radius = std::max(0.0001f,
        (desc.overrideRadius >= 0.0f) ? desc.overrideRadius : BoundsRadius(mesh, center));
    const FrameState frameState = UploadFrame(resources, desc.orbit, center, radius);
    const scene::PerFrameCB& frameData = frameState.frame;

    // ── b1: Flavor ごとに別の ObjectCB を差す ──
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectCBForDraw = g_shared.objectCB;
    if (desc.flavor == Flavor::Terrain && !channelReady) {
        if (!g_shared.terrainCB.IsValid())
            g_shared.terrainCB = resources.CreateConstantBuffer(sizeof(TerrainObjectCB));
        if (!g_shared.terrainCB.IsValid()) return false;
        const TerrainObjectCB terrainData = BuildTerrainCB(desc.material, frameData.viewProjection);
        resources.Update(g_shared.terrainCB, &terrainData, sizeof(terrainData));
        objectCBForDraw = g_shared.terrainCB;
    } else if (desc.flavor == Flavor::Water && !channelReady) {
        if (!g_shared.waterCB.IsValid())
            g_shared.waterCB = resources.CreateConstantBuffer(sizeof(WaterCB));
        if (!g_shared.waterCB.IsValid()) return false;
        const WaterCB waterData = BuildWaterCB(desc.material, frameData.viewProjection, desc.time,
                                               scene::GetWaterDetailNoise(resources));
        resources.Update(g_shared.waterCB, &waterData, sizeof(waterData));
        objectCBForDraw = g_shared.waterCB;
    } else {
        UploadIdentityObjectCB(resources);
    }

    if (desc.flavor == Flavor::Skinned) {
        if (!g_shared.skinningCB.IsValid())
            g_shared.skinningCB = resources.CreateConstantBuffer(sizeof(SkinningCB));
        if (!g_shared.skinningCB.IsValid()) return false;
        SkinningCB skinningData{};
        for (auto& bone : skinningData.boneMatrices) bone = math::Matrix4::Identity();
        resources.Update(g_shared.skinningCB, &skinningData, sizeof(skinningData));
    }

    // ── b2: 材質 / チャンネル / 単色フォールバック ──
    renderer::ResourceHandle<renderer::ConstantBufferTag> materialCBForDraw;
    if (channelReady) {
        if (!g_shared.channelCB.IsValid())
            g_shared.channelCB = resources.CreateConstantBuffer(sizeof(ChannelCB));
        if (!g_shared.channelCB.IsValid()) return false;
        const ChannelCB channelData = BuildChannelCB(desc.material, desc.gpu, desc.channel);
        resources.Update(g_shared.channelCB, &channelData, sizeof(channelData));
        materialCBForDraw = g_shared.channelCB;
    } else if (useMaterial) {
        materialCBForDraw = desc.gpu->materialCB;
    } else {
        FallbackMaterialCB fallbackData{};
        fallbackData.albedo = { std::clamp(desc.fallbackColor.x, 0.0f, 1.0f),
                                std::clamp(desc.fallbackColor.y, 0.0f, 1.0f),
                                std::clamp(desc.fallbackColor.z, 0.0f, 1.0f),
                                std::clamp(desc.fallbackColor.w, 0.0f, 1.0f) };
        fallbackData.textureMask = desc.fallbackTexture.IsValid() ? 1u : 0u;
        resources.Update(g_shared.fallbackMaterialCB, &fallbackData, sizeof(fallbackData));
        materialCBForDraw = g_shared.fallbackMaterialCB;
    }

    UploadRig(resources, desc.rig, frameState);

    renderer.SetRenderTarget(desc.target, resources);
    if (desc.clear) {
        renderer.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });
        renderer.ClearDepth();
    }

    renderer::DrawCall drawCall;
    drawCall.vertexBuffer = mesh.vertexBuffer;
    drawCall.indexBuffer  = mesh.indexBuffer;
    drawCall.indexCount   = mesh.indexCount;
    drawCall.vertexCount  = mesh.vertexCount;
    drawCall.shader = channelReady ? g_shared.channelShader
                    : useMaterial  ? desc.gpu->shader
                                   : g_shared.fallbackShader;
    drawCall.pipelineState = (wireframe && g_shared.wireframePso.IsValid())
        ? g_shared.wireframePso : g_shared.solidPso;
    drawCall.constantBuffers[0]  = g_shared.frameCB;
    drawCall.constantBuffers[1]  = channelReady ? g_shared.objectCB : objectCBForDraw;
    drawCall.constantBuffers[2]  = materialCBForDraw;
    drawCall.constantBuffers[3]  = g_shared.lightCB;
    drawCall.constantBuffers[4]  = g_shared.shadowCB;
    drawCall.constantBuffers[9]  = g_shared.clusterCB;
    drawCall.constantBuffers[12] = g_shared.punctualShadowCB;
    if (desc.flavor == Flavor::Skinned && !channelReady)
        drawCall.constantBuffers[7] = g_shared.skinningCB;

    if (useMaterial) {
        const std::size_t count = std::min(drawCall.textures.size(), desc.gpu->textures.size());
        for (std::size_t i = 0; i < count; ++i) drawCall.textures[i] = desc.gpu->textures[i];
        if ((desc.flavor == Flavor::Terrain || desc.flavor == Flavor::Water) &&
            !channelReady && EnsureDefaultTextures(resources)) {
            for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(drawCall.textures.size()); ++i) {
                if (drawCall.textures[i].IsValid()) continue;
                drawCall.textures[i] = DefaultTextureForSlot(desc.flavor, i);
            }
        }
        // WHY 上書きするか: t4 は Water ではさざ波タイル。白や黒で埋めると勾配が一様な傾きへ
        //     復号され、プレビューの水面だけ «斜めに倒れた鏡» になる。
        if (desc.flavor == Flavor::Water && !channelReady) {
            const auto& detailNoise = scene::GetWaterDetailNoise(resources);
            if (detailNoise.texture.IsValid()) drawCall.textures[4] = detailNoise.texture;
        }
    } else {
        drawCall.textures[0] = desc.fallbackTexture;
    }

    renderer.Submit(drawCall, resources);
    // WHY: ImGui 描画の途中で一時 RT へ切り替えているため、必ずバックバッファへ戻す。
    renderer.SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>{}, resources);
    return true;
}

} // namespace fbzz::editor::matpreview
