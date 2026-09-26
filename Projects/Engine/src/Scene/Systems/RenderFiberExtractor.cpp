/// @file    RenderFiberExtractor.cpp
/// @brief   共通 RHI で表面繊維を描き、Fin の GPU データをメッシュごとに共有する。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <Engine/Asset/StreamedTextureResolver.hpp>
#include <Engine/Scene/Systems/RenderPasses/Geometry/FiberRenderPass.hpp>
#include "RenderPasses/Geometry/GeometryPasses.hpp"
#include <Engine/Scene/Systems/RenderFiberExtractor.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/FiberMaterialSettings.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/FiberGeometry.hpp>
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/FiberComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <unordered_map>
#include <Engine/Renderer/DynamicBufferPool.hpp>
#include <Engine/Scene/Fields/FlowFieldFrame.hpp>
#include "RenderPasses/Geometry/FiberSurfaceSources.hpp"
#include "RenderPasses/Geometry/FlowFieldGpu.hpp"
#include "RenderPasses/Debug/SelectionPasses.hpp"

namespace fbzz::scene {
namespace {



/// @note Fin は頂点/インデックスバッファ、Blade は根元の StructuredBuffer だけを持つ。
struct FiberFinBuffers {
    renderer::ResourceHandle<renderer::BufferTag> m_vertices;
    renderer::ResourceHandle<renderer::BufferTag> m_indices;
    uint32_t m_indexCount = 0;
    renderer::ResourceHandle<renderer::StructuredBufferTag> m_blades;
    uint32_t m_bladeCount = 0;
    uint64_t m_seen = 0;
};




/// @note 1 フレーム内の全パス・全カスケードで共有する解決結果。フレームが進むと読み直す。
struct FiberMaterialEntry {
    asset::FiberMaterialSettings m_settings;
    FiberMaskSlot m_mask;
    uint64_t m_frame = (std::numeric_limits<uint64_t>::max)();
    bool m_valid = false;
};

/// @note シーンに置いた FlowField を 1 フレーム 1 回だけ詰め、前フレームの一覧を velocity 用に後ろへ連結する。
/// @note 環境流は FiberFrameCB の風が持つため含めない (FlowFieldFrame::sceneFieldCount で切る)。
struct FiberFlowCache {
    uint64_t m_frame = (std::numeric_limits<uint64_t>::max)();
    /// @note 物体ごとの交差判定用。前フレームの vectorField は解放済みかもしれないので null にして持つ。
    std::vector<ActiveFlowField> m_current;
    std::vector<ActiveFlowField> m_previous;
    std::vector<GpuFlowField> m_currentPacked;
    std::vector<GpuFlowField> m_previousPacked;
    std::vector<GpuFlowField> m_upload;
    renderer::ResourceHandle<renderer::StructuredBufferTag> m_buffer;
};

/// @note 直前に転送した内容。一致する CB 更新を省く。


/// @note ResourceManager が GPU 実体を単独所有する。保持するのは失効検出付き参照だけ。
struct FiberResources {
    renderer::ResourceManager* m_owner = nullptr;
    uint64_t m_resetVersion = 0;
    uint64_t m_sweptFrame = (std::numeric_limits<uint64_t>::max)();
    std::map<std::array<uint32_t, 8>, FiberFinBuffers> m_fins;
    std::map<std::pair<const Scene*,std::string>,FiberTerrainCache> m_terrains;
    std::map<const Scene*,FiberContactCache> m_contactStates;
    std::unordered_map<std::string, FiberMaterialEntry> m_materials;
    /// @note FiberComponent::m_maskPath による個体ごとの差し替え。パスごとに 1 枠。
    std::unordered_map<std::string, FiberMaskSlot> m_maskOverrides;
    std::map<const Scene*, FiberFlowCache> m_flows;
    /// @note 流れの一覧は毎フレーム書き換える。GPU が読んでいる前フレームのバッファを上書きしないよう借り直す。
    /// @note プールの既定コンストラクターは explicit。集成体の {} 初期化から呼べるよう明示的に構築する。
    renderer::DynamicStructuredBufferPool m_flowBuffers = renderer::DynamicStructuredBufferPool();
};

/// @brief シーンの FlowField を今フレームぶん詰め、前フレームと連結して VS 用 StructuredBuffer へ送る。
/// @note 連続しないフレーム (初回・描画の空白) は前フレーム = 今フレームとし、流れの変化を速度に出さない。
FiberFlowCache& UpdateFiberFlows(FiberResources& cache, Scene& scene, renderer::ResourceManager& resources)
{
    auto& flow = cache.m_flows[&scene];
    if (flow.m_frame == Time::frameCount) return flow;
    const bool consecutive = flow.m_frame != (std::numeric_limits<uint64_t>::max)() && Time::frameCount == flow.m_frame + 1;
    flow.m_frame = Time::frameCount;
    std::swap(flow.m_previous, flow.m_current);
    std::swap(flow.m_previousPacked, flow.m_currentPacked);
    const auto& frame = scene.FlowFrame();
    const auto fields = SelectFlowFields(frame, false);
    flow.m_current.assign(fields.begin(), fields.end());
    flow.m_currentPacked.clear();
    for (const auto& field : flow.m_current) flow.m_currentPacked.push_back(PackGpuFlowField(field, resources));
    for (auto& field : flow.m_current) field.vectorField = nullptr;
    if (!consecutive) {
        flow.m_previous = flow.m_current;
        flow.m_previousPacked = flow.m_currentPacked;
    }
    flow.m_upload = flow.m_currentPacked;
    flow.m_upload.insert(flow.m_upload.end(), flow.m_previousPacked.begin(), flow.m_previousPacked.end());
    /// @note 0 本でも 1 要素は借りる。未束縛の SRV を VS が静的に参照しないようにする。
    flow.m_buffer = cache.m_flowBuffers.Acquire(resources, (std::max)(flow.m_upload.size(), size_t{ 1 }),
                                                static_cast<uint32_t>(sizeof(GpuFlowField)));
    if (flow.m_buffer.IsValid() && !flow.m_upload.empty())
        resources.Update(flow.m_buffer, flow.m_upload.data(), flow.m_upload.size() * sizeof(GpuFlowField));
    return flow;
}

/// @return 球 (ワールド [m]) に届き、channels が重なる場の本数。0 本なら VS で場を評価しない。
/// @note radius < 0 は境界不明 (スキン) として全本数を返す。
uint32_t CountFiberFlows(const std::vector<ActiveFlowField>& fields, uint32_t channels,
                         const math::Vector3& center, float radius)
{
    if (channels == 0u) return 0;
    for (const auto& field : fields)
        if (FlowReceiver{true, channels}.Intersects(field, center, radius))
            return static_cast<uint32_t>(fields.size());
    return 0;
}

/// @note DX12 の CB は Update のたびに次の提出でアリーナへ複製し直す。同じ内容の再転送を省く。
const FiberMaterialEntry& GetFiberMaterial(FiberResources& cache, renderer::ResourceManager& resources,
    const std::string& path)
{
    auto& entry = cache.m_materials[path];
    if (entry.m_frame == Time::frameCount) return entry;
    entry.m_frame = Time::frameCount;
    const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(path);
    const auto* material = asset::AssetManager::Get<asset::MaterialAsset>(handle);
    entry.m_valid = material != nullptr;
    if (material) {
        entry.m_settings = asset::ResolveFiberMaterial(material);
        ResolveFiberMaskTexture(entry.m_settings, material, resources, entry.m_mask);
    }
    return entry;
}

/// @brief FiberComponent の個体ごとの倍率を共有 .mat の解決結果へ掛ける。
/// @note 長さ・密度・毛束は ResolveFiberMaterial と同じ上限で丸める。色の乗算は 1 を超えてよい (HDR)。非有限は 1 とみなす。
asset::FiberMaterialSettings ApplyFiberOverrides(const asset::FiberMaterialSettings& base, const FiberComponent& fiber,
    FiberResources& cache, renderer::ResourceManager& resources)
{
    const auto scale = [](float value) { return std::isfinite(value) ? std::max(value, 0.0f) : 1.0f; };
    asset::FiberMaterialSettings settings = base;
    const math::Vector3 tint{ scale(fiber.m_colorTint.x), scale(fiber.m_colorTint.y), scale(fiber.m_colorTint.z) };
    settings.m_rootColor = { base.m_rootColor.x * tint.x, base.m_rootColor.y * tint.y, base.m_rootColor.z * tint.z, base.m_rootColor.w };
    settings.m_tipColor = { base.m_tipColor.x * tint.x, base.m_tipColor.y * tint.y, base.m_tipColor.z * tint.z, base.m_tipColor.w };
    settings.m_length = std::clamp(base.m_length * scale(fiber.m_lengthScale), 0.0f, 2.0f);
    settings.m_density = std::clamp(base.m_density * scale(fiber.m_densityScale), 0.0f, 1.0f);
    settings.m_clumping = std::clamp(base.m_clumping * scale(fiber.m_clumpingScale), 0.0f, 1.0f);
    if (!fiber.m_maskPath.empty())
        ResolveFiberMaskTexture(settings, fiber.m_maskPath, resources, cache.m_maskOverrides[fiber.m_maskPath]);
    return settings;
}

FiberResources& GetFiberResources(renderer::ResourceManager& resources)
{
    static FiberResources cache;
    if (cache.m_owner != &resources || cache.m_resetVersion != resources.GetResetVersion()) {
        cache = FiberResources{};
        cache.m_owner = &resources;
        cache.m_resetVersion = resources.GetResetVersion();
    }
    /// @note 世代切れの掃除は 1 フレーム 1 回。影のカスケード・面ごとに繰り返さない。
    if (cache.m_sweptFrame == Time::frameCount) return cache;
    cache.m_sweptFrame = Time::frameCount;
    for (auto it = cache.m_materials.begin(); it != cache.m_materials.end();) {
        if (Time::frameCount > it->second.m_frame && Time::frameCount - it->second.m_frame > 120) it = cache.m_materials.erase(it);
        else ++it;
    }
    for (auto it=cache.m_terrains.begin();it!=cache.m_terrains.end();) {
        if (Time::frameCount>it->second.m_seen && Time::frameCount-it->second.m_seen>120) {
            ReleaseFiberTerrain(it->second,resources);
            it=cache.m_terrains.erase(it);
        } else ++it;
    }
    for (auto it = cache.m_flows.begin(); it != cache.m_flows.end();) {
        if (Time::frameCount > it->second.m_frame && Time::frameCount - it->second.m_frame > 120) it = cache.m_flows.erase(it);
        else ++it;
    }
    for (auto it=cache.m_contactStates.begin();it!=cache.m_contactStates.end();) {
        if (Time::frameCount>it->second.m_frame && Time::frameCount-it->second.m_frame>120)
            it=cache.m_contactStates.erase(it);
        else ++it;
    }
    for (auto it = cache.m_fins.begin(); it != cache.m_fins.end();) {
        const auto& key = it->first;
        if (!resources.Get(renderer::ResourceHandle<renderer::BufferTag>{key[0], key[1]})
            || !resources.Get(renderer::ResourceHandle<renderer::BufferTag>{key[2], key[3]})
            || (Time::frameCount>it->second.m_seen && Time::frameCount-it->second.m_seen>120)) {
            resources.Release(it->second.m_vertices);
            resources.Release(it->second.m_indices);
            resources.Release(it->second.m_blades);
            it = cache.m_fins.erase(it);
        } else ++it;
    }
    return cache;
}

const FiberFinBuffers& GetFiberFinBuffers(FiberResources& cache, renderer::ResourceManager& resources,
                                         const renderer::Mesh& mesh, const FiberComponent& fiber, bool blades,
                                         bool densityFromAlpha = false)
{
    const std::array<uint32_t,8> key{mesh.vertexBuffer.id,mesh.vertexBuffer.gen,mesh.indexBuffer.id,mesh.indexBuffer.gen,
        blades ? 1u : 0u, blades ? std::bit_cast<uint32_t>(fiber.m_bladeDensity) : 0u,
        blades ? std::bit_cast<uint32_t>(fiber.m_bladeWidth) : 0u, blades ? static_cast<uint32_t>(fiber.m_seed) : 0u};
    const auto [it, inserted] = cache.m_fins.try_emplace(key);
    auto& result = it->second;
    result.m_seen=Time::frameCount;
    if (!inserted) return result;
    if (blades) {
        /// @note 頂点は GPU で SV_VertexID から組み立てる。CPU で作って送るのは 1 葉 64 バイトの根元だけ。
        std::vector<renderer::FiberBladeRoot> roots;
        if (!renderer::BuildFiberBlades(mesh, fiber.m_bladeDensity, fiber.m_bladeWidth, static_cast<uint32_t>(fiber.m_seed), roots,
                densityFromAlpha)) {
            core::Logger::Warn("Fiber: blade roots rejected invalid input or budget above 32768; reduce density/patch size");
            return result;
        }
        /// @note 面積×密度が 1 未満なら葉は 0 本。黙って描かない。
        if (roots.empty()) return result;
        result.m_blades = resources.CreateGpuLocalStructuredBuffer(roots.data(), static_cast<uint32_t>(roots.size()),
            sizeof(renderer::FiberBladeRoot));
        result.m_bladeCount = result.m_blades.IsValid() ? static_cast<uint32_t>(roots.size()) : 0;
        return result;
    }
    renderer::FiberFinMesh fins;
    if (!renderer::BuildFiberFins(mesh, fins)) {
        core::Logger::Warn("Fiber: geometry rejected invalid input, unsupported surface or blade budget above 32768; reduce density/patch size");
        return result;
    }
    if (fins.m_indices.empty()) {
        core::Logger::Warn("Fiber: Fin requires CPU triangle data; this mesh has no usable triangles");
        return result;
    }
    result.m_vertices = resources.CreateVertexBuffer(fins.m_vertices.data(),
        fins.m_vertices.size() * sizeof(renderer::FiberFinVertex), sizeof(renderer::FiberFinVertex));
    result.m_indices = resources.CreateIndexBuffer(fins.m_indices.data(), static_cast<uint32_t>(fins.m_indices.size()));
    result.m_indexCount = static_cast<uint32_t>(fins.m_indices.size());
    return result;
}

bool HasFiberSurface(GameObject& go, fbzz::LayerMask mask)
{
    if (!ShouldRenderGameObject(go, mask)) return false;
    const auto* fiber = go.GetComponent<FiberComponent>();
    if (!fiber || !fiber->m_enabled || fiber->m_materialPath.empty()) return false;
    if (const auto* mesh=go.GetComponent<MeshRenderer>(); mesh && mesh->enabled && mesh->lodVisible && mesh->mesh) return true;
    if (const auto* skin=go.GetComponent<SkinnedMeshRenderer>(); skin && skin->enabled && skin->lodVisible && skin->model) return true;
    const auto* terrain=go.GetComponent<TerrainComponent>();
    return terrain && terrain->enabled;
}

}
void ResolveFiberMaskTexture(asset::FiberMaterialSettings& settings, const asset::MaterialAsset* material,
    renderer::ResourceManager& resources, FiberMaskSlot& slot)
{
    std::string path;
    if (material) {
        if (const auto it = material->textures.find(asset::FIBER_MASK_TEXTURE_KEY); it != material->textures.end())
            path = it->second;
    }
    ResolveFiberMaskTexture(settings, path, resources, slot);
}

void ResolveFiberMaskTexture(asset::FiberMaterialSettings& settings, const std::string& path,
    renderer::ResourceManager& resources, FiberMaskSlot& slot)
{
    /// @note 毎フレーム利用権を更新する。読込失敗は resolver が保持し、同期 I/O を繰り返さない。
    {
        slot.m_path = path;
        slot.m_texture = path.empty() ? renderer::ResourceHandle<renderer::TextureTag>{} : asset::StreamedTextureResolver::Engine().ResolveGpu(resources, path);
    }
    const renderer::ITexture* texture = resources.Get(slot.m_texture);
    if (!texture) texture = resources.Get(resources.GetWhiteTexture());
    settings.m_maskIndex = texture ? texture->GetBindlessIndex() : renderer::INVALID_BINDLESS_INDEX;
}
void ExtractRenderFibers(RenderPassContext& ctx, renderer::RenderScene& output) {
    auto& resources = ctx.resources;
    auto& cache = GetFiberResources(resources);
    const auto wind = ctx.scene.FlowFrame().ambient;
    auto& contacts=cache.m_contactStates[&ctx.scene];
    UpdateFiberContacts(contacts,ctx.scene);

    const auto& flows = UpdateFiberFlows(cache, ctx.scene, resources);
    /// @note 風・時刻はパス内で共通。物体・面ごとに検証し直さない。
    FiberFrameCB frameBase;
    frameBase.m_time = std::isfinite(Time::time) ? Time::time : 0.0f;
    if (wind.active && std::isfinite(wind.direction.x) && std::isfinite(wind.direction.y)
        && std::isfinite(wind.direction.z) && std::isfinite(wind.speed)) {
        frameBase.m_wind = wind.direction * wind.speed;
        if (!std::isfinite(frameBase.m_wind.x) || !std::isfinite(frameBase.m_wind.y) || !std::isfinite(frameBase.m_wind.z))
            frameBase.m_wind = {};
        frameBase.m_turbulence = std::isfinite(wind.turbulence) ? wind.turbulence : 0.0f;
        frameBase.m_pulseFrequency = std::isfinite(wind.pulseFrequency) ? wind.pulseFrequency : 0.0f;
    }

    struct Surface {
        renderer::Mesh* mesh=nullptr;
        renderer::ResourceHandle<renderer::BufferTag> vertices;
        renderer::ResourceHandle<renderer::ConstantBufferTag> skin,previousSkin;
        bool cast=true;
        bool validPreviousSkin=true;
        float dither=0;
        bool densityFromAlpha=false;  ///< @brief 地形の層指定時だけ true。葉の密度を頂点色 A で間引く
        /// @note コンピュートスキニング済みの頂点 (静的メッシュと同じ並び)。有効なら Shell は VS でスキニングし直さない。
        renderer::ResourceHandle<renderer::BufferTag> skinnedVertices;
        /// @note スキンの今の姿勢のワールド球 (毛丈込み)。得られないときは false で、カリングせずに描く。
        bool hasSkinBounds=false;
        WorldBounds skinBounds{};
    };
    static std::vector<Surface> surfaces;
    static uint64_t nextIdentity = 0;
    for (auto& go : ctx.scene.GameObjects()) {
        if (!HasFiberSurface(go, ~0u)) continue;
        auto& fiber = *go.GetComponent<FiberComponent>();
        const auto& materialEntry = GetFiberMaterial(cache, resources, fiber.m_materialPath);
        if (!materialEntry.m_valid) continue;
        const asset::FiberMaterialSettings settings = ApplyFiberOverrides(materialEntry.m_settings, fiber, cache, resources);
        if (settings.m_length <= 0.0f || settings.m_density <= 0.0f) continue;
        /// @note 毛丈と曲げを含めたカリング球の余白。静的メッシュとスキンで同じ値を使う。
        const float fiberPadding = ctx.cullingBoundsPadding + settings.m_length * 3.0f + settings.m_maxBend;
        surfaces.clear();
        if (auto* mr=go.GetComponent<MeshRenderer>(); mr && mr->enabled && mr->lodVisible && mr->mesh && !mr->mesh->isSkinned)
            surfaces.push_back({mr->mesh,mr->mesh->vertexBuffer,{}, {},mr->castShadows,true,mr->lodDither});
        if (auto* smr=go.GetComponent<SkinnedMeshRenderer>(); smr && smr->enabled && smr->lodVisible && smr->model) {
            auto* anim=FindAnimator(go);
            const auto skin=ResolveSkinningCB(anim ? anim->skinningBuffer : renderer::ResourceHandle<renderer::ConstantBufferTag>{},
                smr->model,ctx.handles.bindPoseSkinningCB);
            const bool valid=anim && anim->prevBoneMatricesValid && anim->prevSkinningBuffer.IsValid();
            const auto prev=valid ? anim->prevSkinningBuffer : skin;
            const auto* baseMaterial=go.GetComponent<MaterialComponent>();
            /// @note 骨の今の広がりから作る球 (Animator が焼く)。以前はスキンをカリングせず、画面外でも全カスケードへ全層を描いていた。
            WorldBounds skinBounds{};
            const bool hasSkinBounds=ComputeSkinnedWorldBounds(go,*smr,skinBounds,fiberPadding);
            for (size_t i=0;i<smr->SubmeshCount();++i) {
                auto* mesh=smr->SubmeshMesh(i);
                if (!mesh || (baseMaterial && !baseMaterial->SlotAt(i).visible)) continue;
                Surface surface{mesh,smr->ResolveSlotVertexBuffer(i,mesh->vertexBuffer),skin,prev,smr->castShadows,valid,smr->lodDither};
                surface.skinnedVertices=smr->ResolveSlotSkinnedVertexBuffer(i);
                surface.hasSkinBounds=hasSkinBounds;
                surface.skinBounds=skinBounds;
                surfaces.push_back(surface);
            }
        }
        if (auto* terrain=go.GetComponent<TerrainComponent>(); terrain && terrain->enabled) {
            auto& patches=cache.m_terrains[{&ctx.scene,go.instanceId}];
            UpdateFiberTerrain(patches,*terrain,fiber.m_terrainPatchCells,fiber.m_terrainLayer,fiber.m_terrainLayerThreshold,resources);
            const bool layered=fiber.m_terrainLayer>=0;
            for (auto& mesh:patches.m_patches) surfaces.push_back({&mesh,mesh.vertexBuffer,{},{},true,true,0,layered});
        }
        if (!fiber.m_renderIdentity) fiber.m_renderIdentity = ++nextIdentity;
        for (const auto& surface : surfaces) {
            const auto* mesh = surface.mesh;
            if (!surface.vertices.IsValid() || !mesh->indexBuffer.IsValid()) continue;
            renderer::RenderFiberInput input;
            input.identity = fiber.m_renderIdentity;
            input.layer = static_cast<uint32_t>(go.layer);
            input.selected = IsSelectedForOutline(go, ctx.settings);
            input.skinned = mesh->isSkinned;
            input.cast = surface.cast;
            input.validPreviousSkin = surface.validPreviousSkin;
            input.object.world = go.transform.GetWorldMatrix();
            input.object.worldInvTranspose = math::Matrix4::InverseTransposeAffine(input.object.world);
            input.object.objectParams.x = surface.dither;
            input.hasBounds = input.skinned ? surface.hasSkinBounds : mesh->boundsRadius > 0;
            const auto bounds = input.skinned ? surface.skinBounds : ComputeWorldBounds(go.transform, *mesh, fiberPadding);
            input.boundsCenter = input.hasBounds ? bounds.center : go.transform.worldPosition;
            input.boundsRadius = input.hasBounds ? bounds.radius : -1;
            const auto center = input.object.world * math::Vector4{mesh->boundsCenter.x, mesh->boundsCenter.y, mesh->boundsCenter.z, 1};
            input.lodCenter = input.skinned && surface.hasSkinBounds ? bounds.center : math::Vector3{center.x,center.y,center.z};
            input.settings.m_mode = fiber.m_mode;
            input.settings.m_shellCount = fiber.m_shellCount;
            input.settings.m_minShellCount = fiber.m_minShellCount;
            input.settings.m_shadowShellCount = fiber.m_shadowShellCount;
            input.settings.m_distanceLod = fiber.m_distanceLod;
            input.settings.m_lodNear = fiber.m_lodNear;
            input.settings.m_lodFar = fiber.m_lodFar;
            input.material = settings;
            input.frame = frameBase;
            input.frame.m_flowChannels = static_cast<uint32_t>(fiber.m_flowChannels);
            input.frame.m_flowCount = CountFiberFlows(flows.m_current, input.frame.m_flowChannels, input.boundsCenter, input.boundsRadius);
            input.frame.m_flowPreviousFirst = static_cast<uint32_t>(flows.m_currentPacked.size());
            input.frame.m_flowPreviousCount = CountFiberFlows(flows.m_previous, input.frame.m_flowChannels, input.boundsCenter, input.boundsRadius);
            input.contacts = contacts.m_data;
            input.vertices = surface.vertices; input.indices = mesh->indexBuffer; input.indexCount = mesh->indexCount;
            input.skinnedVertices = surface.skinnedVertices; input.skin = surface.skin; input.previousSkin = surface.previousSkin;
            input.flows = flows.m_buffer;
            input.velocityField = asset::VelocityFieldAtlas::Texture(resources);
            const auto resolveShape = [&](bool blades) {
                const auto& buffers = GetFiberFinBuffers(cache, resources, *mesh, fiber, blades, surface.densityFromAlpha);
                return renderer::FiberResolvedGeometry{buffers.m_vertices, buffers.m_indices, buffers.m_indexCount, buffers.m_blades, buffers.m_bladeCount};
            };
            if (fiber.m_mode == FiberRenderMode::FIN || fiber.m_mode == FiberRenderMode::HYBRID) input.fins = resolveShape(false);
            if (fiber.m_mode == FiberRenderMode::BLADE && !input.skinned) input.blades = resolveShape(true);
            output.fibers.push_back(std::move(input));
        }
    }
}
}
