// FBZZ Engine
// MaterialPreview.cpp | fbzz::editor
// Material 専用オフスクリーンプレビュー
// WHAT: 選択中の .mat を AssetBrowser と同じ球体・照明・レンダー設定で描画する。
//       Inspector と Preview パネルの見た目がアセットの場所で変わらないようにする。
#include <Editor/Panels/MaterialPreview.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/VFXEditorLauncher.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/LightSystem.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/PrimitiveMesh.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Renderer/ShaderDescriptor.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <imgui.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace fbzz::editor {

namespace {

// .mat 用の球体プレビュー GPU 状態。
// WHY: Animation Preview のスキニング状態と分離し、マテリアル選択を切り替えても
//      アニメーションの再生時刻・カメラ・レンダーターゲットを壊さない。
struct MaterialPreviewGpu {
    renderer::ResourceHandle<renderer::ShaderTag>         shader;
    renderer::ResourceHandle<renderer::PipelineStateTag>  pso;
    renderer::ResourceHandle<renderer::ConstantBufferTag> frameCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> materialCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> lightCB;
    renderer::ResourceHandle<renderer::ConstantBufferTag> shadowCB;
    renderer::ResourceHandle<renderer::RenderTargetTag>   renderTarget;
    renderer::Mesh* sphere = nullptr;
    renderer::Mesh* skinnedSphere = nullptr;
    renderer::ResourceHandle<renderer::ConstantBufferTag> skinningCB;
    std::string shaderPath;
    uint32_t materialCBSize = 0;
    std::array<renderer::ResourceHandle<renderer::TextureTag>, 8> textures{};
};

struct MaterialPreviewState {
    // AssetBrowser のサムネイルと同じ 3/4 視点を初期値にする。
    float yaw = -2.51034f;
    float pitch = 0.342295f;
    float distance = 1.90675f;
};

enum class MaterialPreviewFlavor {
    Surface,
    Skinned,
    Terrain,
    Water,
    Unsupported,
};

struct MaterialPreviewSkinningCB {
    math::Matrix4 boneMatrices[128];
};

MaterialPreviewGpu s_materialPreviewGpu;
MaterialPreviewState s_materialPreviewState;
constexpr int PREVIEW_RT_SIZE = 512;

ImTextureID ToImTextureID(void* ptr)
{
    return static_cast<ImTextureID>(std::bit_cast<std::uintptr_t>(ptr));
}

std::string MaterialPreviewLower(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string MaterialPreviewTextureLoadPath(const std::string& path,
                                           const EditorContext& ctx)
{
    const std::string normalized = util::FileSystem::NormalizePathSeparators(path);
    const std::string projectRoot = util::FileSystem::NormalizePathSeparators(ctx.projectRoot);
    if (normalized.starts_with("Assets/") && !projectRoot.empty())
        return projectRoot + "/" + normalized;
    return normalized;
}

const std::vector<float>* FindMaterialPreviewParam(const asset::MaterialAsset& material,
                                                   std::string_view name)
{
    auto it = material.params.find(std::string(name));
    if (it != material.params.end()) return &it->second;
    if (name == "albedo") it = material.params.find("base_color");
    else if (name == "metallic") it = material.params.find("metallic_factor");
    else if (name == "roughness") it = material.params.find("roughness_factor");
    else if (name == "normalStrength") it = material.params.find("normal_strength");
    else if (name == "emissiveColor") it = material.params.find("emissive_color");
    else if (name == "emissiveScale") it = material.params.find("emissive_scale");
    return it != material.params.end() ? &it->second : nullptr;
}

std::string FindMaterialPreviewTexture(const asset::MaterialAsset& material,
                                       const renderer::ShaderTexBindDesc& binding)
{
    if (auto it = material.textures.find(binding.name); it != material.textures.end())
        return it->second;

    const std::string name = MaterialPreviewLower(binding.name);
    const auto findSlot = [&material](const char* slot) {
        if (auto it = material.textures.find(slot); it != material.textures.end())
            return it->second;
        return std::string{};
    };

    // AssetBrowser の FindMaterialTexturePath と同じ標準・Terrain/Water 別名解決。
    if (name == "g_normalmap1") return findSlot("normalMap1");
    if (name == "g_normalmap2") return findSlot("normalMap2");
    if (name == "g_foamtex")    return findSlot("foamTex");
    if (name == "g_foammask")   return findSlot("foamMask");
    if (name == "g_envtex")     return findSlot("envCubemap");
    if (name == "g_flowmap")    return findSlot("flowMap");
    if (name == "g_splatmap")   return findSlot("splatmap");
    if (name.starts_with("g_diffuse") && binding.slot >= 1 && binding.slot <= 4)
        return findSlot(("layer" + std::to_string(binding.slot - 1) + "_diffuse").c_str());
    if (name.starts_with("g_normal") && binding.slot >= 5 && binding.slot <= 8)
        return findSlot(("layer" + std::to_string(binding.slot - 5) + "_normal").c_str());
    if (name.starts_with("g_aoroughness") && binding.slot >= 9 && binding.slot <= 12)
        return findSlot(("layer" + std::to_string(binding.slot - 9) + "_ao_roughness").c_str());

    static constexpr const char* kSlots[] = {
        "albedo", "normal", "metallic", "emissive", "ao", "tex5", "tex6", "tex7"
    };
    if (binding.slot < std::size(kSlots)) {
        const std::string standard = findSlot(kSlots[binding.slot]);
        if (!standard.empty()) return standard;
        if (std::string_view(kSlots[binding.slot]) == "albedo")
            return findSlot("diffuse");
        if (std::string_view(kSlots[binding.slot]) == "ao")
            return findSlot("ao_roughness");
    }
    return {};
}

std::string ResolveMaterialPreviewShaderPath(const asset::MaterialAsset& material)
{
    // AssetBrowser の Material サムネイルと同じ空 shader のフォールバックを使う。
    const std::string shaderPath = material.shaderPath.empty()
        ? "Assets/Shaders/Material/Surface/Fallback.hlsl"
        : material.shaderPath;
    const std::string lower = MaterialPreviewLower(shaderPath);
    const bool unsupportedGeometry =
        material.renderPath == asset::RenderPath::Particle ||
        material.renderPath == asset::RenderPath::Trail ||
        material.renderPath == asset::RenderPath::UI ||
        material.renderPath == asset::RenderPath::Decal ||
        lower.find("/terrain/") != std::string::npos ||
        lower.find("/water/") != std::string::npos;
    return unsupportedGeometry
        ? "Assets/Shaders/Material/Surface/Lit.hlsl"
        : shaderPath;
}

MaterialPreviewFlavor DetectMaterialPreviewFlavor(const asset::MaterialAsset& material)
{
    // UI マテリアルは矩形と ortho 前提、Decal は深度からの投影前提。
    // どちらも球メッシュのプレビューでは成立しない。
    if (material.renderPath == asset::RenderPath::Particle ||
        material.renderPath == asset::RenderPath::Trail ||
        material.renderPath == asset::RenderPath::UI ||
        material.renderPath == asset::RenderPath::Decal)
        return MaterialPreviewFlavor::Unsupported;
    if (material.meshType == asset::MeshType::Skinned)
        return MaterialPreviewFlavor::Skinned;

    const std::string lower = MaterialPreviewLower(material.shaderPath);
    if (lower.find("/material/effects/") != std::string::npos ||
        lower.find("/effects/particle") != std::string::npos ||
        lower.find("/effects/trail") != std::string::npos ||
        lower.find("/effects/meshtrail") != std::string::npos)
        return MaterialPreviewFlavor::Unsupported;
    if (lower.find("/water/") != std::string::npos)
        return MaterialPreviewFlavor::Water;
    if (lower.find("/terrain/") != std::string::npos)
        return MaterialPreviewFlavor::Terrain;
    return MaterialPreviewFlavor::Surface;
}

renderer::Mesh* CreateSkinnedPreviewSphere(renderer::ResourceManager& resources, int segments)
{
    static std::unordered_map<int, std::shared_ptr<renderer::Mesh>> cache;
    if (auto it = cache.find(segments); it != cache.end()) return it->second.get();

    auto* surface = renderer::PrimitiveMesh::Sphere(resources, segments);
    if (!surface) return nullptr;

    std::vector<renderer::SkinnedVertex> vertices;
    vertices.reserve(surface->cpuVertices.size());
    for (const auto& vertex : surface->cpuVertices) {
        renderer::SkinnedVertex skinned{};
        skinned.position = vertex.position;
        skinned.normal = vertex.normal;
        skinned.tangent = vertex.tangent;
        skinned.uv = vertex.uv;
        skinned.boneIndices[0] = 0;
        skinned.boneWeights[0] = 1.0f;
        vertices.push_back(skinned);
    }

    auto mesh = std::make_shared<renderer::Mesh>();
    mesh->vertexBuffer = resources.CreateVertexBuffer(
        vertices.data(), vertices.size() * sizeof(renderer::SkinnedVertex),
        sizeof(renderer::SkinnedVertex));
    mesh->indexBuffer = resources.CreateIndexBuffer(
        surface->cpuIndices.data(), static_cast<uint32_t>(surface->cpuIndices.size()));
    mesh->vertexCount = static_cast<uint32_t>(vertices.size());
    mesh->indexCount = static_cast<uint32_t>(surface->cpuIndices.size());
    mesh->isSkinned = true;
    mesh->cpuSkinnedVertices = std::move(vertices);
    mesh->cpuIndices = surface->cpuIndices;
    mesh->ComputeBounds();
    cache[segments] = mesh;
    return mesh.get();
}

void SetMaterialPreviewDefault(std::vector<uint8_t>& data,
                               const renderer::ShaderDescriptor& descriptor)
{
    for (const auto& variable : descriptor.vars) {
        if (variable.varType != renderer::ShaderVarType::Float) continue;
        for (uint32_t component = 0; component < variable.columns; ++component) {
            const uint32_t offset = variable.offset + component * sizeof(float);
            if (offset + sizeof(float) <= data.size()) {
                const float value = 1.0f;
                std::memcpy(data.data() + offset, &value, sizeof(value));
            }
        }
    }

    const auto setFloat = [&](std::string_view name, float value) {
        const auto* variable = descriptor.FindVar(name);
        if (!variable || variable->varType != renderer::ShaderVarType::Float ||
            variable->columns != 1 || variable->offset + sizeof(float) > data.size()) return;
        std::memcpy(data.data() + variable->offset, &value, sizeof(value));
    };
    const auto setFloat2 = [&](std::string_view name, const float value[2]) {
        const auto* variable = descriptor.FindVar(name);
        if (!variable || variable->varType != renderer::ShaderVarType::Float ||
            variable->columns < 2 || variable->offset + 2u * sizeof(float) > data.size()) return;
        std::memcpy(data.data() + variable->offset, value, 2u * sizeof(float));
    };
    const auto setFloat3 = [&](std::string_view name, const float value[3]) {
        const auto* variable = descriptor.FindVar(name);
        if (!variable || variable->varType != renderer::ShaderVarType::Float ||
            variable->columns < 3 || variable->offset + 3u * sizeof(float) > data.size()) return;
        std::memcpy(data.data() + variable->offset, value, 3u * sizeof(float));
    };
    setFloat("metallic", 0.0f);
    setFloat("roughness", 0.65f);
    setFloat("emissiveScale", 0.0f);
    setFloat("alphaCutoff", 0.5f);
    const float uvTiling[2] = { 1.0f, 1.0f };
    const float uvOffset[2] = { 0.0f, 0.0f };
    const float white3[3] = { 1.0f, 1.0f, 1.0f };
    setFloat2("uvTiling", uvTiling);
    setFloat2("uvOffset", uvOffset);
    setFloat3("emissiveColor", white3);
}

void ApplyMaterialPreviewParams(const asset::MaterialAsset& material,
                                const renderer::ShaderDescriptor& descriptor,
                                std::vector<uint8_t>& data)
{
    for (const auto& variable : descriptor.vars) {
        if (variable.varType != renderer::ShaderVarType::Float ||
            variable.offset + variable.size > data.size()) continue;
        const auto* values = FindMaterialPreviewParam(material, variable.name);
        if (!values || values->empty()) continue;
        const size_t count = std::min<size_t>(variable.columns, values->size());
        std::memcpy(data.data() + variable.offset, values->data(), count * sizeof(float));
    }
}

bool EnsureMaterialPreviewGpu(EditorContext& ctx, const asset::MaterialAsset& material)
{
    if (!ctx.resources) return false;
    auto& resources = *ctx.resources;
    asset::MaterialAsset fallbackAsset;
    const asset::MaterialAsset* renderAsset = &material;
    if (material.shaderPath.empty()) {
        fallbackAsset.shaderPath = "Assets/Shaders/Material/Surface/Fallback.hlsl";
        fallbackAsset.params["albedo"] = { 1.0f, 0.0f, 1.0f, 1.0f };
        renderAsset = &fallbackAsset;
    }

    const std::string shaderPath = ResolveMaterialPreviewShaderPath(*renderAsset);
    if (shaderPath != s_materialPreviewGpu.shaderPath) {
        s_materialPreviewGpu.shaderPath = shaderPath;
        s_materialPreviewGpu.shader = {};
        if (s_materialPreviewGpu.materialCB.IsValid()) {
            resources.Release(s_materialPreviewGpu.materialCB);
            s_materialPreviewGpu.materialCB = {};
            s_materialPreviewGpu.materialCBSize = 0;
        }
    }
    if (!s_materialPreviewGpu.shader.IsValid())
        s_materialPreviewGpu.shader = resources.LoadShader(shaderPath);
    if (!s_materialPreviewGpu.sphere)
        s_materialPreviewGpu.sphere = renderer::PrimitiveMesh::Sphere(resources, 64);
    if (!s_materialPreviewGpu.pso.IsValid()) {
        // AssetBrowser の Material サムネイルと同じ固定パイプラインにする。
        // WHY: Inspector だけ材質自身の blend/depth 設定を使うと、同じ .mat でも
        //      表示場所によって輪郭・透過・陰影が変わってしまう。
        s_materialPreviewGpu.pso = resources.CreatePipelineState({
            renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND,
            renderer::DepthMode::DEPTH_ON
        });
    }
    if (!s_materialPreviewGpu.frameCB.IsValid())
        s_materialPreviewGpu.frameCB = resources.CreateConstantBuffer(sizeof(scene::PerFrameCB));
    if (!s_materialPreviewGpu.objectCB.IsValid())
        s_materialPreviewGpu.objectCB = resources.CreateConstantBuffer(sizeof(scene::PerObjectCB));
    if (!s_materialPreviewGpu.lightCB.IsValid())
        s_materialPreviewGpu.lightCB = resources.CreateConstantBuffer(sizeof(renderer::LightConstantsCB));
    if (!s_materialPreviewGpu.shadowCB.IsValid())
        s_materialPreviewGpu.shadowCB = resources.CreateConstantBuffer(sizeof(scene::ShadowConstantsCB));
    if (!s_materialPreviewGpu.renderTarget.IsValid())
        s_materialPreviewGpu.renderTarget = resources.CreateRenderTarget(PREVIEW_RT_SIZE, PREVIEW_RT_SIZE);
    auto* shader = resources.Get(s_materialPreviewGpu.shader);
    if (!shader || !s_materialPreviewGpu.sphere || !s_materialPreviewGpu.pso.IsValid()) return false;

    if (DetectMaterialPreviewFlavor(*renderAsset) == MaterialPreviewFlavor::Skinned) {
        if (!s_materialPreviewGpu.skinnedSphere)
            s_materialPreviewGpu.skinnedSphere = CreateSkinnedPreviewSphere(resources, 64);
        if (!s_materialPreviewGpu.skinnedSphere) return false;
    }

    const auto& descriptor = shader->GetDescriptor();
    if (descriptor.IsValid() && s_materialPreviewGpu.materialCBSize != descriptor.cbufferSize) {
        if (s_materialPreviewGpu.materialCB.IsValid()) resources.Release(s_materialPreviewGpu.materialCB);
        s_materialPreviewGpu.materialCB = resources.CreateConstantBuffer(descriptor.cbufferSize);
        s_materialPreviewGpu.materialCBSize = descriptor.cbufferSize;
    }
    if (!descriptor.IsValid()) {
        s_materialPreviewGpu.textures = {};
        return true;
    }

    std::vector<uint8_t> data(descriptor.cbufferSize, 0u);
    SetMaterialPreviewDefault(data, descriptor);
    ApplyMaterialPreviewParams(*renderAsset, descriptor, data);
    std::array<renderer::ResourceHandle<renderer::TextureTag>, 8> textures{};
    uint32_t textureMask = 0;
    for (const auto& binding : descriptor.textures) {
        if (binding.slot >= textures.size()) continue;
        const std::string path = FindMaterialPreviewTexture(*renderAsset, binding);
        if (path.empty()) continue;
        textures[binding.slot] = resources.LoadTexture(MaterialPreviewTextureLoadPath(path, ctx));
        if (textures[binding.slot].IsValid() && binding.slot < 8)
            textureMask |= 1u << binding.slot;
    }
    if (descriptor.textureMaskOffset != UINT32_MAX &&
        descriptor.textureMaskOffset + sizeof(textureMask) <= data.size()) {
        std::memcpy(data.data() + descriptor.textureMaskOffset, &textureMask, sizeof(textureMask));
    }
    if (s_materialPreviewGpu.materialCB.IsValid())
        resources.Update(s_materialPreviewGpu.materialCB, data.data(), data.size());

    // 描画直前に使うテクスチャを一時保存する。LoadTexture は ResourceManager のキャッシュを使う。
    s_materialPreviewGpu.textures = textures;
    return true;
}

bool RenderMaterialPreviewFrame(EditorContext& ctx,
                                const asset::MaterialAsset& material)
{
    if (!ctx.renderer || !ctx.resources) return false;
    if (!EnsureMaterialPreviewGpu(ctx, material)) return false;

    auto& resources = *ctx.resources;
    auto& renderer = *ctx.renderer;
    auto* shader = resources.Get(s_materialPreviewGpu.shader);
    if (!shader || !s_materialPreviewGpu.sphere ||
        !s_materialPreviewGpu.renderTarget.IsValid()) return false;

    constexpr float radius = 0.5f;
    const math::Vector3 center = math::Vector3::ZERO;
    const float cosPitch = std::cos(s_materialPreviewState.pitch);
    const math::Vector3 orbitOffset{
        cosPitch * std::sin(s_materialPreviewState.yaw) * s_materialPreviewState.distance,
        std::sin(s_materialPreviewState.pitch) * s_materialPreviewState.distance,
        cosPitch * std::cos(s_materialPreviewState.yaw) * s_materialPreviewState.distance
    };
    renderer::Camera camera;
    camera.m_position = center + orbitOffset;
    // AssetBrowser は常に正方形 RT / aspect=1.0 で描画するため、同じ投影を使う。
    camera.m_aspect = 1.0f;
    camera.m_fovY = 38.0f;
    camera.m_near = 0.01f;
    camera.m_far = 10.0f;
    camera.LookAt(center);

    scene::PerFrameCB frameData{};
    frameData.view = camera.GetViewMatrix();
    frameData.projection = camera.GetProjectionMatrix();
    frameData.viewProjection = camera.GetViewProjection();
    frameData.invViewProjection = math::Matrix4::Inverse(frameData.viewProjection);
    frameData.cameraPos = camera.m_position;
    frameData.nearZ = camera.m_near;
    frameData.farZ = camera.m_far;
    resources.Update(s_materialPreviewGpu.frameCB, &frameData, sizeof(frameData));

    scene::PerObjectCB objectData{};
    objectData.world = math::Matrix4::Identity();
    objectData.worldInvTranspose = math::Matrix4::Identity();
    resources.Update(s_materialPreviewGpu.objectCB, &objectData, sizeof(objectData));

    const MaterialPreviewFlavor flavor = DetectMaterialPreviewFlavor(material);
    renderer::Mesh* previewMesh = s_materialPreviewGpu.sphere;
    if (flavor == MaterialPreviewFlavor::Skinned) {
        previewMesh = s_materialPreviewGpu.skinnedSphere;
        if (!s_materialPreviewGpu.skinningCB.IsValid())
            s_materialPreviewGpu.skinningCB = resources.CreateConstantBuffer(
                sizeof(MaterialPreviewSkinningCB));
        if (!previewMesh || !s_materialPreviewGpu.skinningCB.IsValid()) return false;

        MaterialPreviewSkinningCB skinningData{};
        for (auto& bone : skinningData.boneMatrices)
            bone = math::Matrix4::Identity();
        resources.Update(s_materialPreviewGpu.skinningCB, &skinningData, sizeof(skinningData));
    }

    // ── 3 点照明リグ (キー / フィル / リム) ──
    // WHY: AssetBrowser のサムネイルと同じ照明を使い、Inspector / Preview の表示差を
    //      なくす。単一の平行光では陰側が平坦になりやすいため、弱い寒色フィルと
    //      背面リムを加えて、マテリアルの粗さ・金属感・輪郭を読み取りやすくする。
    renderer::LightConstantsCB lightData{};
    const math::Vector3 keyLight =
        (camera.m_position + math::Vector3{ radius * 1.4f, radius * 1.8f, radius * 0.8f } - center).Normalized();
    lightData.lightDir = { -keyLight.x, -keyLight.y, -keyLight.z };
    lightData.lightColor = { 1.0f, 0.96f, 0.90f };
    lightData.lightIntensity = 1.8f / 3.14159265358979323846f;
    lightData.ambientColor = { 0.10f, 0.11f, 0.14f };

    // WHY: LightAttenuation は距離の二乗減衰を含むため、球の半径に比例して
    //      ライト位置と強度を調整し、表示サイズが変わっても明るさを一定にする。
    constexpr float kPreviewUnitScale = 3.14159265358979323846f;
    const auto placePreviewLight = [&](renderer::PointLight& light,
                                       const math::Vector3& offsetFromCenter,
                                       const math::Vector3& color,
                                       float targetIntensity) {
        light.position = center + offsetFromCenter;
        light.color = color;
        light.range = radius * 20.0f;
        light.intensity = targetIntensity *
            std::max(offsetFromCenter.LengthSq(), 0.01f) / kPreviewUnitScale;
    };
    placePreviewLight(lightData.pointLights[0],
                      { radius * 2.6f, -radius * 1.4f, -radius * 2.2f },
                      { 0.55f, 0.65f, 1.0f }, 0.4f);
    placePreviewLight(lightData.pointLights[1],
                      { radius * 1.6f, radius * 2.4f, radius * 2.8f },
                      { 1.0f, 1.0f, 1.0f }, 1.1f);
    lightData.pointLightCount = 2;
    lightData.spotLightCount = 0;
    resources.Update(s_materialPreviewGpu.lightCB, &lightData, sizeof(lightData));

    scene::ShadowConstantsCB shadowData{};
    shadowData.lightViewProjection = math::Matrix4::Translate({ 16.0f, 16.0f, 0.0f });
    shadowData.shadowMapTexelSize[0] = 0.0f;
    shadowData.shadowMapTexelSize[1] = 0.0f;
    shadowData.shadowBias = 1.0f;
    resources.Update(s_materialPreviewGpu.shadowCB, &shadowData, sizeof(shadowData));

    renderer.SetRenderTarget(s_materialPreviewGpu.renderTarget, resources);
    renderer.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });
    renderer.ClearDepth();
    renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC);
    renderer.SetSampler(1, renderer::SamplerMode::CLAMP_LINEAR);
    renderer.SetSampler(2, renderer::SamplerMode::CLAMP_ANISOTROPIC);

    renderer::DrawCall drawCall;
    drawCall.vertexBuffer = previewMesh->vertexBuffer;
    drawCall.indexBuffer = previewMesh->indexBuffer;
    drawCall.indexCount = previewMesh->indexCount;
    drawCall.vertexCount = previewMesh->vertexCount;
    drawCall.shader = s_materialPreviewGpu.shader;
    drawCall.pipelineState = s_materialPreviewGpu.pso;
    drawCall.constantBuffers[0] = s_materialPreviewGpu.frameCB;
    drawCall.constantBuffers[1] = s_materialPreviewGpu.objectCB;
    drawCall.constantBuffers[2] = s_materialPreviewGpu.materialCB;
    drawCall.constantBuffers[3] = s_materialPreviewGpu.lightCB;
    drawCall.constantBuffers[4] = s_materialPreviewGpu.shadowCB;
    if (flavor == MaterialPreviewFlavor::Skinned)
        drawCall.constantBuffers[7] = s_materialPreviewGpu.skinningCB;
    for (size_t i = 0; i < s_materialPreviewGpu.textures.size(); ++i)
        drawCall.textures[i] = s_materialPreviewGpu.textures[i];
    renderer.Submit(drawCall, resources);
    renderer.SetRenderTarget(renderer::ResourceHandle<renderer::RenderTargetTag>{}, resources);
    return true;
}

} // namespace

static void DrawMaterialPreviewFrame(ImDrawList* drawList, ImVec2 origin, float size, bool hovered)
{
    // AssetBrowser の DrawThumbnailFrame と同じ背景グラデーションを使う。
    const ImU32 bg = hovered ? IM_COL32(42, 45, 52, 255) : IM_COL32(30, 32, 38, 255);
    drawList->AddRectFilled(origin, { origin.x + size, origin.y + size }, bg, 4.0f);
    const ImU32 gradTop = hovered ? IM_COL32(56, 60, 70, 255) : IM_COL32(44, 47, 56, 255);
    const ImU32 gradBottom = hovered ? IM_COL32(30, 32, 38, 255) : IM_COL32(19, 20, 24, 255);
    drawList->AddRectFilledMultiColor(
        { origin.x + 2.0f, origin.y + 2.0f },
        { origin.x + size - 2.0f, origin.y + size - 2.0f },
        gradTop, gradTop, gradBottom, gradBottom);
    drawList->AddRect(origin, { origin.x + size, origin.y + size },
                      IM_COL32(95, 100, 112, 230), 4.0f, 0, 1.0f);
}

bool DrawMaterialPreviewWidget(EditorContext& ctx,
                               const asset::MaterialAsset& material,
                               float previewHeight)
{
    ImGui::PushID("##MaterialPreviewWidget");
    ImGui::TextDisabled("Sphere  |  3-Point Light");

    // AssetBrowser と同じ正方形表示にする。横長の Inspector 幅へ引き伸ばすと、
    // 同じ RT でも球の投影とハイライトの位置が別物に見えるため。
    const float width = (std::max)(ImGui::GetContentRegionAvail().x, 64.0f);
    const float height = (std::max)(previewHeight, 96.0f);
    const float size = (std::max)((std::min)(width, height), 64.0f);
    const ImVec2 imageOrigin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##MaterialPreviewImage", ImVec2(size, size));
    const bool imageHovered = ImGui::IsItemHovered();
    const bool imageActive = ImGui::IsItemActive();
    if (imageHovered) ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);

    if (imageActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        s_materialPreviewState.yaw -= delta.x * 0.012f;
        s_materialPreviewState.pitch = std::clamp(
            s_materialPreviewState.pitch + delta.y * 0.010f, -1.35f, 1.35f);
    }
    if (imageHovered && ImGui::GetIO().MouseWheel != 0.0f) {
        s_materialPreviewState.distance *=
            ImGui::GetIO().MouseWheel > 0.0f ? 0.88f : 1.14f;
        s_materialPreviewState.distance = std::clamp(
            s_materialPreviewState.distance, 0.75f, 4.0f);
    }
    if (imageHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        s_materialPreviewState = {};
    }

    const bool rendered = RenderMaterialPreviewFrame(ctx, material);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 imageMax(imageOrigin.x + size, imageOrigin.y + size);
    DrawMaterialPreviewFrame(drawList, imageOrigin, size, imageHovered);
    if (rendered && ctx.imguiRenderer && ctx.resources) {
        if (void* rawId = ctx.imguiRenderer->GetImTextureID(
                s_materialPreviewGpu.renderTarget, *ctx.resources, 0)) {
            drawList->AddImage(ToImTextureID(rawId), imageOrigin, imageMax);
        }
    } else {
        const char* message = "Material preview unavailable";
        const ImVec2 textSize = ImGui::CalcTextSize(message);
        drawList->AddText(
            ImVec2(imageOrigin.x + (size - textSize.x) * 0.5f,
                   imageOrigin.y + (size - textSize.y) * 0.5f),
            IM_COL32(150, 158, 170, 255), message);
    }
    if (imageHovered) {
        drawList->AddText(
            ImVec2(imageOrigin.x + 8.0f,
                   imageMax.y - ImGui::GetTextLineHeight() - 6.0f),
            IM_COL32(170, 178, 190, 210),
            "Drag: Orbit | Wheel: Zoom | Double Click: Reset");
    }
    ImGui::PopID();
    return rendered;
}


} // namespace fbzz::editor
