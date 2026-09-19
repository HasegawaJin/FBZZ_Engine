/// @file    FiberRenderPass.cpp
/// @brief   共通 RHI で表面繊維を描き、Fin の GPU データをメッシュごとに共有する。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <Engine/Scene/Systems/RenderPasses/Geometry/FiberRenderPass.hpp>
#include "GeometryPasses.hpp"
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/FiberMaterialSettings.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/FiberGeometry.hpp>
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
#include "FiberSurfaceSources.hpp"
#include "FlowFieldGpu.hpp"
#include "../Debug/SelectionPasses.hpp"

namespace fbzz::scene {
namespace {

struct FiberMotionCB {
    math::Matrix4 m_previousWorld;
    math::Matrix4 m_previousNormal;
    math::Vector4 m_previousWindTime;
    math::Vector4 m_previousShape;
    math::Vector4 m_previousGust;
    math::Vector4 m_motion;
};
static_assert(sizeof(FiberMotionCB) == 192);

/// @note Fin は頂点/インデックスバッファ、Blade は根元の StructuredBuffer だけを持つ。
struct FiberFinBuffers {
    renderer::ResourceHandle<renderer::BufferTag> m_vertices;
    renderer::ResourceHandle<renderer::BufferTag> m_indices;
    uint32_t m_indexCount = 0;
    renderer::ResourceHandle<renderer::StructuredBufferTag> m_blades;
    uint32_t m_bladeCount = 0;
    uint64_t m_seen = 0;
};

enum class FiberPassMode { COLOR, GBUFFER, VELOCITY, SHADOW, SELECTION };
enum class FiberShape { SHELL, FIN, BLADE };

struct FiberShaderSlot {
    renderer::ResourceHandle<renderer::ShaderTag> m_handle;
    uint64_t m_attempt = (std::numeric_limits<uint64_t>::max)();
};

/// @note 1 フレーム内の全パス・全カスケードで共有する解決結果。フレームが進むと読み直す。
struct FiberMaterialEntry {
    asset::FiberMaterialSettings m_settings;
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
template<typename T>
struct FiberUpload {
    T m_value{};
    bool m_valid = false;
};

/// @note ResourceManager が GPU 実体を単独所有する。保持するのは失効検出付き参照だけ。
struct FiberResources {
    renderer::ResourceManager* m_owner = nullptr;
    uint64_t m_resetVersion = 0;
    uint64_t m_sweptFrame = (std::numeric_limits<uint64_t>::max)();
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_material;
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_frame;
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_motion;
    renderer::ResourceHandle<renderer::PipelineStateTag> m_deferredPipeline;
    renderer::ResourceHandle<renderer::PipelineStateTag> m_pipeline;
    std::map<std::array<uint32_t, 8>, FiberFinBuffers> m_fins;
    renderer::ResourceHandle<renderer::ConstantBufferTag> m_contacts;
    std::map<std::pair<const Scene*,std::string>,FiberTerrainCache> m_terrains;
    std::map<const Scene*,FiberContactCache> m_contactStates;
    std::array<FiberShaderSlot, 5 * 3 * 2> m_shaders;
    std::unordered_map<std::string, FiberMaterialEntry> m_materials;
    FiberUpload<asset::FiberMaterialSettings> m_uploadedMaterial;
    FiberUpload<FiberFrameCB> m_uploadedFrame;
    FiberUpload<FiberMotionCB> m_uploadedMotion;
    FiberUpload<FiberContactCB> m_uploadedContacts;
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
    const auto& fields = *frame.fields;
    const size_t sceneCount = (std::min)(frame.sceneFieldCount, fields.size());
    flow.m_current.assign(fields.begin(), fields.begin() + static_cast<std::ptrdiff_t>(sceneCount));
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
        if (AffectsEmitter(field, channels) && FlowIntersectsSphere(field, center, radius))
            return static_cast<uint32_t>(fields.size());
    return 0;
}

/// @note DX12 の CB は Update のたびに次の提出でアリーナへ複製し直す。同じ内容の再転送を省く。
template<typename T>
void UploadIfChanged(renderer::ResourceManager& resources, renderer::ResourceHandle<renderer::ConstantBufferTag> handle,
                     FiberUpload<T>& last, const T& value)
{
    if (last.m_valid && std::memcmp(&last.m_value, &value, sizeof(T)) == 0) return;
    resources.Update(handle, &value, sizeof(T));
    last.m_value = value;
    last.m_valid = true;
}

/// @note 失敗したパスは 1 フレーム 1 回だけ再試行する。ファイル修正後の復帰とログの連打防止を両立する。
renderer::ResourceHandle<renderer::ShaderTag> GetFiberShader(FiberResources& cache, renderer::ResourceManager& resources,
                                                             FiberPassMode mode, FiberShape shape, bool skinned)
{
    constexpr const char* SHAPES[]{"Shell", "Fin", "Blade"};
    constexpr const char* SUFFIXES[]{"", "GBuffer", "Velocity", "Shadow", "Selection"};
    auto& slot = cache.m_shaders[(static_cast<size_t>(mode) * 3 + static_cast<size_t>(shape)) * 2 + (skinned ? 1 : 0)];
    if (slot.m_handle.IsValid() && resources.Get(slot.m_handle)) return slot.m_handle;
    if (slot.m_attempt == Time::frameCount) return {};
    slot.m_attempt = Time::frameCount;
    slot.m_handle = resources.LoadShader(std::string("Assets/Shaders/Fiber/Fiber") + SHAPES[static_cast<int>(shape)]
        + (skinned ? "Skinned" : "") + SUFFIXES[static_cast<int>(mode)] + ".hlsl");
    return slot.m_handle;
}

/// @note 材質パラメーターの文字列検索は 1 フレーム 1 回。影の各カスケードや各ビューで繰り返さない。
const FiberMaterialEntry& GetFiberMaterial(FiberResources& cache, const std::string& path)
{
    auto& entry = cache.m_materials[path];
    if (entry.m_frame == Time::frameCount) return entry;
    entry.m_frame = Time::frameCount;
    const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(path);
    const auto* material = asset::AssetManager::Get<asset::MaterialAsset>(handle);
    entry.m_valid = material != nullptr;
    if (material) entry.m_settings = asset::ResolveFiberMaterial(material);
    return entry;
}

FiberResources& GetFiberResources(renderer::ResourceManager& resources)
{
    static FiberResources cache;
    if (cache.m_owner != &resources || cache.m_resetVersion != resources.GetResetVersion()) {
        cache = FiberResources{};
        cache.m_owner = &resources;
        cache.m_resetVersion = resources.GetResetVersion();
    }
    if (!cache.m_material.IsValid()) {
        cache.m_material = resources.CreateConstantBuffer(sizeof(asset::FiberMaterialSettings));
        cache.m_frame = resources.CreateConstantBuffer(sizeof(FiberFrameCB));
        cache.m_contacts = resources.CreateConstantBuffer(sizeof(FiberContactCB));
        cache.m_motion = resources.CreateConstantBuffer(sizeof(FiberMotionCB));
        cache.m_deferredPipeline = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_SKY });
        cache.m_pipeline = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID_NOCULL,
            renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
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

renderer::FiberMotionHistory& AdvanceFiberHistory(FiberComponent& fiber, RenderPassContext& ctx,
    const math::Matrix4& world, const FiberFrameCB& frame, const asset::FiberMaterialSettings& settings, uint64_t surfaceKey)
{
    const auto target = ctx.Res().Target("Velocity");
    const uint64_t key = (static_cast<uint64_t>(target.gen) << 32) | target.id;
    const uint64_t stamp = Time::frameCount;
    auto& histories = fiber.m_motionHistories;
    histories.erase(std::remove_if(histories.begin(), histories.end(), [&](const auto& item) {
        return item.m_resourceVersion != ctx.resources.GetResetVersion()
            || (stamp > item.m_frame && stamp - item.m_frame > 2);
    }), histories.end());
    auto it = std::find_if(histories.begin(), histories.end(),
        [&](const auto& item) { return item.m_viewKey == key && item.m_surfaceKey == surfaceKey; });
    if (it == histories.end()) {
        histories.emplace_back();
        it = histories.end() - 1;
        it->m_viewKey = key;
        it->m_surfaceKey = surfaceKey;
        it->m_resourceVersion = ctx.resources.GetResetVersion();
    }
    renderer::FiberDeformationState state;
    state.m_world = world;
    state.m_wind = frame.m_wind;
    state.m_time = frame.m_time;
    state.m_turbulence = frame.m_turbulence;
    state.m_pulseFrequency = frame.m_pulseFrequency;
    state.m_length = settings.m_length;
    state.m_windResponse = settings.m_windResponse;
    state.m_maxBend = settings.m_maxBend;
    state.m_gravityBend = settings.m_gravityBend;
    state.m_mode = static_cast<int>(fiber.m_mode);
    state.m_shellCount = static_cast<int>(frame.m_shellCount);
    it->Advance(stamp, state);
    return *it;
}

/// @note Shadow の呼び出し元が RT と viewport を管理する。その他はここでターゲットを選ぶ。
void ExecuteFiberGeometry(RenderPassContext& ctx, FiberPassMode mode,
    const PerFrameCB* lightFrame = nullptr, const math::Frustum* lightFrustum = nullptr)
{
    /// @note 対象の収集を 1 回の走査にまとめる。繊維の無いシーンでは GPU 資源を作らない。容量は呼び出し間で使い回す。
    static std::vector<GameObject*> objects;
    objects.clear();
    for (auto& go : ctx.scene.GameObjects()) {
        if (!HasFiberSurface(go, ctx.cullingMask)) continue;
        if (mode == FiberPassMode::SELECTION && !IsSelectedForOutline(go, ctx.settings)) continue;
        objects.push_back(&go);
    }
    if (objects.empty()) return;
    auto& resources = ctx.resources;
    auto& cache = GetFiberResources(resources);
    if (!cache.m_material.IsValid() || !cache.m_frame.IsValid() || !cache.m_pipeline.IsValid()) return;
    if (mode == FiberPassMode::VELOCITY && !cache.m_motion.IsValid()) return;
    if (mode == FiberPassMode::COLOR && ctx.isDeferred && !cache.m_deferredPipeline.IsValid()) return;
    const auto cameraData = lightFrame ? *lightFrame
        : MakeCameraFrameCB(ctx.camera, ctx.taaJitterNdcX, ctx.taaJitterNdcY);
    resources.Update(ctx.handles.frameCB, &cameraData, sizeof(cameraData));
    if (mode != FiberPassMode::SHADOW && mode != FiberPassMode::SELECTION) {
        const char* target = mode == FiberPassMode::GBUFFER ? "GBuffer"
            : mode == FiberPassMode::VELOCITY ? "Velocity" : "HDR";
        if (!ctx.Res().Target(target).IsValid()) return;
        ctx.renderer.SetRenderTarget(ctx.Res().Target(target), resources);
    }
    const auto wind = ctx.scene.FlowFrame().ambient;
    auto& contacts=cache.m_contactStates[&ctx.scene];
    UpdateFiberContacts(contacts,ctx.scene);
    UploadIfChanged(resources, cache.m_contacts, cache.m_uploadedContacts, contacts.m_data);
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
        bool densityFromAlpha=false;  ///< 地形の層指定時だけ true。葉の密度を頂点色 A で間引く
    };
    static std::vector<Surface> surfaces;
    for (auto* fiberObject : objects) {
        auto& go = *fiberObject;
        auto& fiber = *go.GetComponent<FiberComponent>();
        const auto& materialEntry = GetFiberMaterial(cache, fiber.m_materialPath);
        if (!materialEntry.m_valid) continue;
        const auto& settings = materialEntry.m_settings;
        if (settings.m_length <= 0.0f || settings.m_density <= 0.0f) continue;
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
            for (size_t i=0;i<smr->SubmeshCount();++i) {
                auto* mesh=smr->SubmeshMesh(i);
                if (!mesh || (baseMaterial && !baseMaterial->SlotAt(i).visible)) continue;
                surfaces.push_back({mesh,smr->ResolveSlotVertexBuffer(i,mesh->vertexBuffer),skin,prev,smr->castShadows,valid,smr->lodDither});
            }
        }
        if (auto* terrain=go.GetComponent<TerrainComponent>(); terrain && terrain->enabled) {
            auto& patches=cache.m_terrains[{&ctx.scene,go.instanceId}];
            UpdateFiberTerrain(patches,*terrain,fiber.m_terrainPatchCells,fiber.m_terrainLayer,fiber.m_terrainLayerThreshold,resources);
            const bool layered=fiber.m_terrainLayer>=0;
            for (auto& mesh:patches.m_patches) surfaces.push_back({&mesh,mesh.vertexBuffer,{},{},true,true,0,layered});
        }
        if (surfaces.empty()) continue;
        const bool drawFins = fiber.m_mode == FiberRenderMode::FIN || fiber.m_mode == FiberRenderMode::HYBRID;
        const bool drawShells = fiber.m_mode == FiberRenderMode::SHELL || fiber.m_mode == FiberRenderMode::HYBRID;
        const bool drawBlades = fiber.m_mode == FiberRenderMode::BLADE;
        PerObjectCB object{};
        object.world = go.transform.GetWorldMatrix();
        object.worldInvTranspose = math::Matrix4::InverseTransposeAffine(object.world);
        for (const auto& surface:surfaces) {
        if (!surface.vertices.IsValid() || !surface.mesh->indexBuffer.IsValid()) continue;
        if (mode==FiberPassMode::SHADOW && !surface.cast) continue;
        const auto* mesh=surface.mesh;
        const bool skinned=mesh->isSkinned;
        if (mode == FiberPassMode::COLOR) ++ctx.statsTotalObjects;
        /// @note 流れの場の交差判定に使う球。スキンは bounds を持たないので «不明» (負の半径) とする。
        math::Vector3 flowCenter = go.transform.worldPosition;
        float flowRadius = -1.0f;
        if (!skinned && mesh->boundsRadius > 0.0f) {
            /// @note 全パスで同じ変形後 bounds を使う。影はカメラ外の caster も必要なので光源の錐台だけを見る。
            const auto bounds = ComputeWorldBounds(go.transform, *mesh,
                ctx.cullingBoundsPadding + settings.m_length*3.0f + settings.m_maxBend);
            flowCenter = bounds.center;
            flowRadius = bounds.radius;
            if (lightFrustum) {
                if (!lightFrustum->IntersectsSphere(bounds.center, bounds.radius)) continue;
            } else {
                if (!IsWithinCullDistance(ctx, go, bounds)) {
                    if (mode == FiberPassMode::COLOR) ++ctx.statsDistanceCulled;
                    continue;
                }
                if (ctx.frustumCullingEnabled && ctx.cameraFrustum
                    && !ctx.cameraFrustum->IntersectsSphere(bounds.center, bounds.radius)) {
                    if (mode == FiberPassMode::COLOR) ++ctx.statsFrustumCulled;
                    continue;
                }
            }
        }
        FiberFrameCB frame = frameBase;
        frame.m_shellCount = static_cast<float>(std::clamp(fiber.m_shellCount, 1, 64));
        const auto center=object.world*math::Vector4{mesh->boundsCenter.x,mesh->boundsCenter.y,mesh->boundsCenter.z,1};
        const float distance=(math::Vector3{center.x,center.y,center.z}-ctx.camera.m_position).Length();
        if (fiber.m_distanceLod) {
            const float nearDistance=std::max(fiber.m_lodNear,0.0f);
            const float farDistance=std::max(fiber.m_lodFar,nearDistance+0.1f);
            if (distance>=farDistance) continue;
            frame.m_shellCount=static_cast<float>(renderer::FiberLodShellCount(fiber.m_shellCount,fiber.m_minShellCount,distance,nearDistance,farDistance));
            const float t=std::clamp((distance-nearDistance)/(farDistance-nearDistance),0.0f,1.0f);
            frame.m_lod.x=1.0f-0.75f*t;
            frame.m_lod.y=std::clamp((farDistance-distance)/(0.1f*(farDistance-nearDistance)),0.0f,1.0f);
        }
        frame.m_hybrid = fiber.m_mode == FiberRenderMode::HYBRID ? 1.0f : 0.0f;
        /// @note 届く場が 1 本も無い物体は本数 0 で送り、VS の場の評価を丸ごと省く。
        frame.m_flowChannels = static_cast<uint32_t>(fiber.m_flowChannels);
        frame.m_flowCount = CountFiberFlows(flows.m_current, frame.m_flowChannels, flowCenter, flowRadius);
        frame.m_flowPreviousFirst = static_cast<uint32_t>(flows.m_currentPacked.size());
        frame.m_flowPreviousCount = CountFiberFlows(flows.m_previous, frame.m_flowChannels, flowCenter, flowRadius);
        const auto shellShader = drawShells ? GetFiberShader(cache, resources, mode, FiberShape::SHELL, skinned)
                                            : renderer::ResourceHandle<renderer::ShaderTag>{};
        const auto finShader = drawFins ? GetFiberShader(cache, resources, mode, FiberShape::FIN, skinned)
                                        : renderer::ResourceHandle<renderer::ShaderTag>{};
        const auto bladeShader = drawBlades && !skinned ? GetFiberShader(cache, resources, mode, FiberShape::BLADE, false)
                                                        : renderer::ResourceHandle<renderer::ShaderTag>{};
        if (!shellShader.IsValid() && !finShader.IsValid() && !bladeShader.IsValid()) continue;
        object.objectParams.x = surface.dither;
        resources.Update(ctx.handles.objectCB, &object, sizeof(object));
        UploadIfChanged(resources, cache.m_material, cache.m_uploadedMaterial, settings);
        UploadIfChanged(resources, cache.m_frame, cache.m_uploadedFrame, frame);
        renderer::DrawCall call;
        call.pipelineState = cache.m_pipeline;
        if (mode == FiberPassMode::COLOR) {
            /// @note Deferred は GBuffer の深度を転写済み。LESS_EQUAL / 読取専用で固有の繊維照明を上書きする。
            call.pipelineState = ctx.isDeferred ? cache.m_deferredPipeline : cache.m_pipeline;
            if (ctx.settings.IsWireframe()) call.pipelineState = ctx.handles.wireframePSO;
        }
        call.constantBuffers[0] = ctx.handles.frameCB;
        call.constantBuffers[1] = ctx.handles.objectCB;
        call.constantBuffers[2] = cache.m_material;
        call.constantBuffers[5] = cache.m_frame;
        call.constantBuffers[10] = cache.m_contacts;
        /// @note 局所 FlowField は VS の t2 (vsBuffers[1])、Baked の速度場アトラスは t26。GPU 粒子と同じ組。
        call.vsBuffers[1] = flows.m_buffer;
        call.textures[26] = asset::VelocityFieldAtlas::Texture(resources);
        if (skinned) {
            call.constantBuffers[7]=surface.skin;
            call.constantBuffers[9]=surface.previousSkin;
        }
        if (mode==FiberPassMode::SELECTION) call.pipelineState=cache.m_pipeline;
        if (mode == FiberPassMode::COLOR) {
            call.constantBuffers[3] = ctx.handles.lightCB;
            call.constantBuffers[4] = ctx.handles.shadowCB;
            call.constantBuffers[8] = ctx.handles.advancedGraphicsCB;
            BindForwardShadingResources(call, ctx);
            call.textures[8] = resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));
        }
        if (mode == FiberPassMode::VELOCITY) {
            auto& history = AdvanceFiberHistory(fiber, ctx, object.world, frame, settings,
                (static_cast<uint64_t>(mesh->vertexBuffer.gen)<<32)|mesh->vertexBuffer.id);
            const auto& prev = history.m_previous;
            FiberMotionCB motion{};
            motion.m_previousWorld = prev.m_world;
            motion.m_previousNormal = math::Matrix4::InverseTransposeAffine(prev.m_world);
            motion.m_previousWindTime = {prev.m_wind.x, prev.m_wind.y, prev.m_wind.z, prev.m_time};
            motion.m_previousShape = {prev.m_length, prev.m_windResponse, prev.m_maxBend, prev.m_gravityBend};
            motion.m_previousGust = {prev.m_turbulence, prev.m_pulseFrequency, 0.0f, 0.0f};
            motion.m_motion = {ctx.taaJitterNdcX, ctx.taaJitterNdcY, history.m_valid && (!skinned || surface.validPreviousSkin) ? 1.0f : 0.0f, 0.0f};
            UploadIfChanged(resources, cache.m_motion, cache.m_uploadedMotion, motion);
            call.constantBuffers[6] = cache.m_motion;
            call.constantBuffers[8] = ctx.handles.advancedGraphicsCB;
            call.textures[7] = resources.GetDepthTexture(ctx.Res().Target("HDR"));
        }
        if (finShader.IsValid()) {
            const auto& fins = GetFiberFinBuffers(cache, resources, *mesh, fiber, false);
            if (fins.m_vertices.IsValid() && fins.m_indices.IsValid()) {
                call.shader = finShader;
                call.vertexBuffer = fins.m_vertices;
                call.indexBuffer = fins.m_indices;
                call.indexCount = fins.m_indexCount;
                if (mode == FiberPassMode::SHADOW) SubmitCountedShadow(ctx, call);
                else SubmitCounted(ctx, call);
            }
        }
        if (shellShader.IsValid()) {
            call.shader = shellShader;
            call.vertexBuffer = surface.vertices;
            call.indexBuffer = mesh->indexBuffer;
            call.indexCount = mesh->indexCount;
            call.instanceCount = static_cast<uint32_t>(frame.m_shellCount);
            if (mode == FiberPassMode::SHADOW) SubmitCountedShadow(ctx, call);
            else SubmitCounted(ctx, call);
        }
        if (bladeShader.IsValid()) {
            const auto& blades=GetFiberFinBuffers(cache,resources,*mesh,fiber,true,surface.densityFromAlpha);
            if (blades.m_blades.IsValid() && blades.m_bladeCount > 0) {
                /// @note 頂点バッファなしの非インデックス描画。距離 LOD は先頭から提出本数を減らす。
                call.shader=bladeShader;
                call.vertexBuffer={};
                call.indexBuffer={};
                call.indexCount=0;
                call.vsBuffers[0]=blades.m_blades;
                call.vertexCount=static_cast<uint32_t>(static_cast<float>(blades.m_bladeCount)*frame.m_lod.x)*renderer::FIBER_BLADE_VERTICES;
                if (call.vertexCount==0) continue;
                call.instanceCount=1;
                if (mode==FiberPassMode::SHADOW) SubmitCountedShadow(ctx,call); else SubmitCounted(ctx,call);
            }
        }
        }
    }
}
} // namespace

void FiberRenderPass::Setup(PassBuilder& builder, const RenderPassContext& ctx) const
{
    builder.ReadWrite("HDR").Read("ShadowMap").Read("PunctualShadowMap").Read("LightCookieAtlas");
    DeclareScreenSpaceOcclusionReads(builder, ctx);
}

bool FiberRenderPass::IsEnabled(const RenderPassContext& ctx) const
{
    for (auto& go : ctx.scene.GameObjects()) if (HasFiberSurface(go, ctx.cullingMask)) return true;
    return false;
}

void ExecuteFiberSelectionMask(RenderPassContext& ctx) { ExecuteFiberGeometry(ctx,FiberPassMode::SELECTION); }
void ExecuteFiberPass(RenderPassContext& ctx) { ExecuteFiberGeometry(ctx, FiberPassMode::COLOR); }
void ExecuteFiberGBufferPass(RenderPassContext& ctx) { ExecuteFiberGeometry(ctx, FiberPassMode::GBUFFER); }
void ExecuteFiberVelocityPass(RenderPassContext& ctx) { ExecuteFiberGeometry(ctx, FiberPassMode::VELOCITY); }
void SubmitFiberShadowCasters(RenderPassContext& ctx, const PerFrameCB& frame, const math::Frustum& frustum)
{
    ExecuteFiberGeometry(ctx, FiberPassMode::SHADOW, &frame, &frustum);
}
} // namespace fbzz::scene

