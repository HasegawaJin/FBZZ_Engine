/// @file    SkinningComputePass.cpp
/// @brief   コンピュートスキニング — ボーン変形を 1 フレームに 1 回だけ計算して共有する。
/// @author  Hasegawa Jin
/// @date    2026-08-14
///
/// @note スキニングは各マテリアルの VS 内で毎回計算されていた。ここで 1 回だけ変形し、静的メッシュと
/// @note 同じ頂点レイアウト (`renderer::Vertex`) へ書き出すことで後続パスは「ただの静的メッシュ」として
/// @note 扱える。次の場合は従来の VS スキニングへフォールバックする: ポーズまたは入力未解決、
/// @note バックエンドが GPU 書き込み頂点バッファ非対応。
#include "GeometryPasses.hpp"
#include <Engine/Scene/Systems/RenderSceneExtractor.hpp>
#include "Engine/Core/Time.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Renderer/Mesh.hpp"
#include "Engine/Renderer/ComputeCall.hpp"
#include "Engine/Renderer/IBuffer.hpp"
#include <Graphics/Renderer/IStructuredBuffer.hpp>
#include "Engine/Renderer/DynamicBufferPool.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include <Graphics/Pipeline/RayTracingPipeline.hpp>
#include "Engine/Scene/Components/MaterialComponent.hpp"
#include "Engine/Scene/Components/AnimatorComponent.hpp"
#include "Engine/Scene/Components/SkinnedMeshRenderer.hpp"
#include <Math/Matrix4.hpp>
#include <cstdint>
#include <bit>
#include <functional>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fbzz::scene {

namespace {

/// @note CS のスレッドグループ幅。SkinningCompute.cs.hlsl の SKINNING_GROUP_SIZE と一致させること。
constexpr uint32_t kSkinningGroupSize = 64u;

/// @note StructuredBuffer は密詰め (cbuffer のような 16 バイト整列パディングが入らない) なので、
/// @note HLSL 側の struct と C++ 側でオフセットが 1 対 1 に対応する。
/// @note ここがずれると頂点が明後日の方向へ飛ぶ形で壊れるため、サイズで固定しておく。
static_assert(sizeof(renderer::SkinnedVertex) == 76,
    "SkinSrcVertex in SkinningCompute.cs.hlsl must match renderer::SkinnedVertex (76 bytes)");
static_assert(sizeof(renderer::Vertex) == 60,
    "SkinnedOutVertex in SkinningCompute.cs.hlsl must match renderer::Vertex (60 bytes)");

/// @note SkinningConstants (b0) — CS 側と一致させること。
struct SkinningCB {
    uint32_t vertexCount = 0;
    uint32_t _pad0 = 0;
    uint32_t _pad1 = 0;
    uint32_t _pad2 = 0;
};

struct EntityIDHash {
    size_t operator()(const EntityID& id) const noexcept
    {
        size_t result = std::hash<uint32_t>{}(id.index);
        result ^= std::hash<uint32_t>{}(id.generation) + 0x9e3779b9u +
                  (result << 6u) + (result >> 2u);
        return result;
    }
};

struct SourceVertexCacheKey {
    const renderer::SkinnedVertex* cpuData = nullptr;
    uint32_t vertexCount = 0;
    renderer::ResourceHandle<renderer::BufferTag> vertexBuffer;
    uint64_t contentVersion = 0;

    bool operator==(const SourceVertexCacheKey&) const = default;
};

struct SourceVertexCacheKeyHash {
    size_t operator()(const SourceVertexCacheKey& key) const noexcept
    {
        size_t result = std::hash<const renderer::SkinnedVertex*>{}(key.cpuData);
        result ^= static_cast<size_t>(key.vertexCount) + 0x9e3779b9u +
                  (result << 6u) + (result >> 2u);
        result ^= static_cast<size_t>(key.vertexBuffer.id) + 0x9e3779b9u +
                  (result << 6u) + (result >> 2u);
        result ^= static_cast<size_t>(key.vertexBuffer.gen) + 0x9e3779b9u +
                  (result << 6u) + (result >> 2u);
        result ^= std::hash<uint64_t>{}(key.contentVersion) + 0x9e3779b9u + (result << 6u) + (result >> 2u);
        return result;
    }
};

/// @note 入力頂点の StructuredBuffer は Mesh 単位で共有する (ポーズに依存しない生データのため)。同じ
/// @note モデルを何体出しても入力は 1 本で足りる。Mesh の破棄・再生成でアドレスが再利用されても古い
/// @note 頂点バッファを掴まないよう、CPU 配列と元 GPU バッファの世代を値としてキーにする。
std::unordered_map<SourceVertexCacheKey,
                   renderer::ResourceHandle<renderer::StructuredBufferTag>,
                   SourceVertexCacheKeyHash> g_srcVertexCache;

/// @note ボーンパレットはポーズが毎フレーム変わるので、Animator ごとに 1 本持たずフレームごとに借りる。
/// @note DX12 の読み取り専用 StructuredBuffer は Upload ヒープへの直 memcpy のため、1 本を使い回すと
/// @note GPU がまだ実行中の前フレームのスキニングが今フレームのポーズを読んでしまう (詳細は
/// @note `DynamicBufferPool.hpp`)。
renderer::DynamicStructuredBufferPool g_bonePalettePool;

/// @note このフレームに借りたパレット。Animator の下に Renderer が何本あっても転送は 1 回で済ませる。
/// @note キーは EntityID: ComponentArray::Remove は末尾要素を swap するため AnimatorComponent* は
/// @note 安定した識別子ではなく、EntityID なら generation を含むので削除後の再利用も区別できる。
std::unordered_map<EntityID,
                   renderer::ResourceHandle<renderer::StructuredBufferTag>,
                   EntityIDHash> g_framePalettes;

/// @note 使用集合はビュー単位ではなくエンジンフレーム全体で累積する。Scene View にだけ見えるメッシュを
/// @note Game View 側の判定末尾で「未使用」と誤判定すると、次フレームに immutable 入力 SRV を
/// @note 再アップロードすることになる。
std::unordered_set<SourceVertexCacheKey, SourceVertexCacheKeyHash> g_usedSourceKeys;
uint64_t g_cacheUsageFrame = (std::numeric_limits<uint64_t>::max)();
renderer::ResourceManager* g_cacheResources = nullptr;
uint64_t g_cacheResetVersion = 0;

/// @note BeginSkinningCacheFrame — 前フレームの全ビューで未使用だったキャッシュだけを回収する。
void BeginSkinningCacheFrame(renderer::ResourceManager& resources, uint64_t frameStamp)
{
    if (g_cacheResources != &resources || g_cacheResetVersion != resources.GetResetVersion()) {
        g_srcVertexCache.clear();
        g_framePalettes.clear();
        g_usedSourceKeys.clear();
        g_bonePalettePool = renderer::DynamicStructuredBufferPool{};
        g_cacheUsageFrame = (std::numeric_limits<uint64_t>::max)();
        g_cacheResources = &resources;
        g_cacheResetVersion = resources.GetResetVersion();
    }
    if (g_cacheUsageFrame == frameStamp) return;

    if (g_cacheUsageFrame != (std::numeric_limits<uint64_t>::max)()) {
        for (auto it = g_srcVertexCache.begin(); it != g_srcVertexCache.end();) {
            if (g_usedSourceKeys.contains(it->first)) {
                ++it;
                continue;
            }
            if (it->second.IsValid()) resources.Release(it->second);
            it = g_srcVertexCache.erase(it);
        }
    }

    g_usedSourceKeys.clear();
    g_framePalettes.clear();
    g_cacheUsageFrame = frameStamp;
}

/// @note EnsureSourceVertexBuffer — スキニング入力の StructuredBuffer を Mesh 単位で用意する。
renderer::ResourceHandle<renderer::StructuredBufferTag> EnsureSourceVertexBuffer(
    renderer::ResourceManager& resources, const renderer::Mesh& mesh,
    renderer::ResourceHandle<renderer::BufferTag> source)
{
    /// @note cpuSkinnedVertices はインポート時に保持される。空なら GPU スキニングは行えない。
    const auto* sourceBuffer = resources.Get(source);
    if (mesh.cpuSkinnedVertices.empty() || mesh.cpuSkinnedVertices.size() > UINT32_MAX
        || !sourceBuffer || sourceBuffer->GetContentVersion() == 0
        || sourceBuffer->GetStride() != sizeof(renderer::SkinnedVertex)
        || sourceBuffer->GetSize() < mesh.cpuSkinnedVertices.size() * sizeof(renderer::SkinnedVertex))
        return {};

    const SourceVertexCacheKey key{
        mesh.cpuSkinnedVertices.data(),
        static_cast<uint32_t>(mesh.cpuSkinnedVertices.size()),
        source, sourceBuffer->GetContentVersion()};
    g_usedSourceKeys.insert(key);
    const auto it = g_srcVertexCache.find(key);
    if (it != g_srcVertexCache.end()) {
        if (resources.Get(it->second)) return it->second;
        g_srcVertexCache.erase(it);
    }

    /// @note 元 mesh と同じ handle でも内容更新を持つため、authoritative な版付き入力を読む。
    std::vector<renderer::SkinnedVertex> currentVertices(mesh.cpuSkinnedVertices.size());
    if (!sourceBuffer->CopyData(0, currentVertices.size() * sizeof(renderer::SkinnedVertex), currentVertices.data())) return {};
    const auto handle = resources.CreateGpuLocalStructuredBuffer(
        currentVertices.data(),
        static_cast<uint32_t>(mesh.cpuSkinnedVertices.size()),
        static_cast<uint32_t>(sizeof(renderer::SkinnedVertex)));

    g_srcVertexCache[key] = handle;
    return handle;
}

/// @brief AcquireBonePalette — アニメーターのボーンパレットを今フレーム用に借りて転送する。
/// @note cbuffer ではなく StructuredBuffer を使う: 頂点あたり 4 回の動的インデックスアクセスが
/// @note 走り、128 要素の cbuffer 配列への動的アクセスは定数キャッシュの高速経路を外れやすい。
renderer::ResourceHandle<renderer::StructuredBufferTag> AcquireBonePalette(
    renderer::ResourceManager& resources,
    EntityID animatorEntity,
    const std::vector<math::Matrix4>& matrices)
{
    if (matrices.empty()) return {};
    if (const auto it = g_framePalettes.find(animatorEntity); it != g_framePalettes.end())
        return it->second;

    const size_t count  = matrices.size();
    const auto   handle = g_bonePalettePool.Acquire(
        resources, count, static_cast<uint32_t>(sizeof(math::Matrix4)));
    if (handle.IsValid())
        resources.Update(handle, matrices.data(), count * sizeof(math::Matrix4));
    g_framePalettes[animatorEntity] = handle;
    return handle;
}

} /// @note namespace

void ExecuteSkinningComputePass(RenderPassContext& ctx)
{
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    ctx.skinningRequests.clear();
    ctx.skinningExecuted = false;
    const auto invalidatePublication = [](SkinnedMeshRenderer& smr) {
        smr.gpuSkinnedThisFrame = false;
        smr.gpuSkinningFrame = (std::numeric_limits<uint64_t>::max)();
        smr.gpuSkinningVertexCount = 0;
        smr.gpuSkinningDispatchCount = 0;
        smr.gpuSkinningContent.clear();
    };
    if (!resources.Get(h.skinningComputeCS) || !resources.Get(h.skinningCB)) {
        /// @note Non-null typed handles can be stale. A missing program/CB must not expose an old pose as current deformation.
        for (auto& go : ctx.scene.GameObjects())
            if (auto* smr = go.GetComponent<SkinnedMeshRenderer>()) invalidatePublication(*smr);
        return;
    }
    struct PendingPublication {
        SkinnedMeshRenderer* component = nullptr;
        std::vector<std::pair<renderer::ResourceHandle<renderer::BufferTag>, uint64_t>> outputs;
    };
    std::vector<PendingPublication> pendingPublications;

    BeginSkinningCacheFrame(resources, Time::frameCount);
    /// @note EntityID は Scene 内だけで一意。別 Scene の同番号の Animator とパレットを共有しない。
    /// @note 計算済み Renderer は gpuSkinningFrame で再利用するため、既存出力を再 Dispatch しない。
    g_framePalettes.clear();
    auto& usedSourceKeys = g_usedSourceKeys;
    /// @note 各 submesh の Dispatch は別出力へ書き、相互依存しない。DX12 は UAV バリアを
    /// @note Dispatch ごとに発行せず、このバッチを閉じる時点の 1 回へ集約する。

    for (auto& go : ctx.scene.GameObjects()) {
        if (!go.activeInHierarchy()) continue;
        auto* smr = go.GetComponent<SkinnedMeshRenderer>();
        const bool rayDemand = ctx.experimentalRayTracingEnabled
            && (ctx.settings.modeRequest.mode != renderer::RenderMode::RASTER
                || renderer::IsRayDebugView(ctx.settings.viewMode));
        if (!smr || !smr->enabled || (!smr->lodVisible && !rayDemand) || !smr->model) continue;

        AnimatorComponent* anim = nullptr;
        EntityID animatorEntity = EntityID::INVALID;
        for (GameObject* current = &go; current; current = current->GetParent()) {
            if (auto* candidate = current->GetComponent<AnimatorComponent>()) {
                anim = candidate;
                animatorEntity = current->GetID();
                break;
            }
        }
        const auto* matrices = anim && !anim->boneMatrices.empty() ? &anim->boneMatrices
            : smr->model->skeleton && !smr->model->skeleton->referencePose.empty()
                ? &smr->model->skeleton->referencePose : nullptr;
        if (!matrices) {
            /// @note ポーズ未解決時に、レイ用として単位行列や未変形頂点を公開しない。
            smr->gpuSkinnedThisFrame = false;
            smr->gpuSkinningFrame = (std::numeric_limits<uint64_t>::max)();
            smr->gpuSkinningVertexCount = 0;
            smr->gpuSkinningDispatchCount = 0;
            smr->gpuSkinningContent.clear();
            continue;
        }

        /// @note モデルが差し替わったら、旧モデルの頂点数で確保したバッファを捨てる。
        if (smr->skinnedBufferModel != smr->model) {
            for (auto& handle : smr->skinnedVertexBuffers)
                if (handle.IsValid()) resources.Release(handle);
            smr->skinnedVertexBuffers.clear();
            smr->skinnedBufferModel = smr->model;
            smr->gpuSkinnedThisFrame = false;
            smr->gpuSkinningFrame = (std::numeric_limits<uint64_t>::max)();
            smr->gpuSkinningVertexCount = 0;
            smr->gpuSkinningDispatchCount = 0;
            smr->gpuSkinningContent.clear();
        }

        /// @note 出力バッファ配列は model->meshes と同じ添字で確保する。ローカルスロット数まで
        /// @note 詰めないのは、ShadowPass / ForwardPass も同じ添字で引くため。多少の空きスロット
        /// @note より、添字が 1 つに定まっていることを優先する。
        const size_t meshCount = smr->model->meshes.size();
        smr->skinnedVertexBuffers.resize(meshCount);

        const uint64_t frameStamp = Time::frameCount;
        std::vector<uint32_t> content;
        content.push_back(static_cast<uint32_t>(matrices->size()));
        for (const auto& matrix : *matrices)
            for (const auto& row : matrix.m)
                for (float value : row) content.push_back(std::bit_cast<uint32_t>(value));
        bool outputsReady = smr->gpuSkinnedThisFrame;
        for (size_t slot = 0; slot < smr->SubmeshCount(); ++slot) {
            const auto* mesh = smr->SubmeshMesh(slot);
            if (!mesh) { outputsReady = false; continue; }
            const auto source = smr->ResolveSlotVertexBuffer(slot, mesh->vertexBuffer);
            const auto* buffer = resources.Get(source);
            const uint64_t version = buffer ? buffer->GetContentVersion() : 0;
            content.push_back(smr->SubmeshAt(slot));
            content.push_back(source.id); content.push_back(source.gen);
            content.push_back(static_cast<uint32_t>(version)); content.push_back(static_cast<uint32_t>(version >> 32));
            content.push_back(static_cast<uint32_t>(mesh->cpuSkinnedVertices.size()));
            const auto* output = resources.Get(smr->ResolveSlotSkinnedVertexBuffer(slot));
            outputsReady &= output && output->GetStride() == sizeof(renderer::Vertex)
                && output->GetSize() >= mesh->cpuSkinnedVertices.size() * sizeof(renderer::Vertex)
                && output->GetContentVersion() != 0
                && output->GetBindlessUavIndex() != renderer::INVALID_BINDLESS_INDEX;
        }
        if (outputsReady && smr->gpuSkinningContent == content) {
            /// @note Scene/Game View の 2 回目以降は同じポーズと出力を共有する。
            /// @note キャッシュ寿命判定にはこのビューでも使用中の入力を記録しておく。
            const size_t reusedSlotCount = smr->SubmeshCount();
            for (size_t slot = 0; slot < reusedSlotCount; ++slot) {
                const uint32_t submeshIndex = smr->SubmeshAt(slot);
                if (submeshIndex == UINT32_MAX || submeshIndex >= meshCount) continue;
                const auto& meshPtr = smr->model->meshes[submeshIndex];
                if (!meshPtr) continue;
                const auto source = smr->ResolveSlotVertexBuffer(slot, meshPtr->vertexBuffer);
                usedSourceKeys.insert(SourceVertexCacheKey{
                    meshPtr->cpuSkinnedVertices.data(),
                    static_cast<uint32_t>(meshPtr->cpuSkinnedVertices.size()),
                    source, resources.Get(source) ? resources.Get(source)->GetContentVersion() : 0});
            }
            /// @note 最後に描画されたビューが Snapshot を上書きしても、当該フレームの実処理量を表示する。
            if (smr->gpuSkinningFrame != frameStamp) {
                smr->gpuSkinningVertexCount = 0;
                smr->gpuSkinningDispatchCount = 0;
                smr->gpuSkinningFrame = frameStamp;
            }
            ctx.statsSkinningVertexCount += smr->gpuSkinningVertexCount;
            ctx.statsSkinningDispatchCount += smr->gpuSkinningDispatchCount;
            continue;
        }

        const auto bonePalette = AcquireBonePalette(resources, anim ? animatorEntity : go.GetID(), *matrices);
        const auto* palette = resources.Get(bonePalette);
        if (!palette || palette->GetStride() != sizeof(math::Matrix4) || palette->GetElementCount() == 0
            || palette->GetSize() / sizeof(math::Matrix4) < palette->GetElementCount()) {
            smr->gpuSkinnedThisFrame = false;
            smr->gpuSkinningFrame = frameStamp;
            smr->gpuSkinningVertexCount = 0;
            smr->gpuSkinningDispatchCount = 0;
            continue;
        }

        bool allSkinned = smr->SubmeshCount() != 0;
        PendingPublication publication{smr, {}};
        uint64_t skinnedVertexCount = 0;
        uint32_t skinningDispatchCount = 0;
        /// @note この Renderer が実際に描く submesh だけをスキニングする。ノードごとに子 GameObject
        /// @note へ分けた構成では同じモデルを参照する Renderer が複数並ぶため、全員がモデル全体を
        /// @note 変形すると子の数だけ同じ計算を繰り返し、負荷が submesh 数倍になる。
        const size_t slotCount = smr->SubmeshCount();
        for (size_t slot = 0; slot < slotCount; ++slot) {
            const uint32_t submeshIndex = smr->SubmeshAt(slot);
            if (submeshIndex == UINT32_MAX || submeshIndex >= meshCount) { allSkinned = false; continue; }
            /// @note 以降の mi は model->meshes / 各バッファ配列と同じ添字。
            const size_t mi = static_cast<size_t>(submeshIndex);
            const auto& meshPtr = smr->model->meshes[mi];
            if (!meshPtr) { allSkinned = false; continue; }
            const auto source = smr->ResolveSlotVertexBuffer(slot, meshPtr->vertexBuffer);
            const auto srcVertices = EnsureSourceVertexBuffer(resources, *meshPtr, source);
            const uint32_t vertexCount = static_cast<uint32_t>(meshPtr->cpuSkinnedVertices.size());
            const auto* sourceVertices = resources.Get(srcVertices);
            if (vertexCount == 0 || !sourceVertices || sourceVertices->GetStride() != sizeof(renderer::SkinnedVertex)
                || sourceVertices->GetElementCount() < vertexCount
                || sourceVertices->GetSize() / sizeof(renderer::SkinnedVertex) < vertexCount) {
                allSkinned = false;
                continue;
            }

            /// @note ポーズが異なるため出力は renderer 別に所有し、現在の頂点数を収める容量だけを再利用する。
            auto& outBuffer = smr->skinnedVertexBuffers[mi];
            const size_t outputBytes = static_cast<size_t>(vertexCount) * sizeof(renderer::Vertex);
            const auto* output = resources.Get(outBuffer);
            /// @note 同じ model/source handle の topology growth でも旧容量へ書かない。解放は ResourceManager の GPU fence へ委ねる。
            if (output && (output->GetStride() != sizeof(renderer::Vertex) || output->GetSize() < outputBytes
                || output->GetBindlessUavIndex() == renderer::INVALID_BINDLESS_INDEX)) {
                resources.Release(outBuffer);
                outBuffer = {};
            } else if (!output) outBuffer = {};
            if (!outBuffer.IsValid()) {
                outBuffer = resources.CreateGpuWritableVertexBuffer(
                    outputBytes,
                    static_cast<uint32_t>(sizeof(renderer::Vertex)));
                /// @note バックエンド未対応ならここで無効ハンドルが返る。VS 経路へ落とす。
                if (!outBuffer.IsValid()) { allSkinned = false; break; }
            }
            output = resources.Get(outBuffer);
            if (!output || output->GetStride() != sizeof(renderer::Vertex)
                || output->GetSize() / sizeof(renderer::Vertex) < vertexCount
                || output->GetBindlessUavIndex() == renderer::INVALID_BINDLESS_INDEX) {
                allSkinned = false;
                continue;
            }

            ctx.skinningRequests.push_back({srcVertices, bonePalette, outBuffer, vertexCount});
            publication.outputs.emplace_back(outBuffer, output->GetContentVersion());

            skinnedVertexCount += vertexCount;
            ++skinningDispatchCount;
        }

        smr->gpuSkinnedThisFrame = allSkinned;
        smr->gpuSkinningContent = allSkinned ? std::move(content) : std::vector<uint32_t>{};
        if (allSkinned) pendingPublications.push_back(std::move(publication));
        smr->gpuSkinningFrame = frameStamp;
        smr->gpuSkinningVertexCount = skinnedVertexCount;
        smr->gpuSkinningDispatchCount = skinningDispatchCount;
        ctx.statsSkinningVertexCount += skinnedVertexCount;
        ctx.statsSkinningDispatchCount += skinningDispatchCount;
    }

    renderer::ExecuteSkinningRequests(ctx);
    /// @note Dispatch is a void API; only the backend's successful DIRECT write publication advances this version.
    for (const auto& publication : pendingPublications) {
        for (const auto& [handle, previousVersion] : publication.outputs) {
            const auto* output = resources.Get(handle);
            if (!output || output->GetContentVersion() <= previousVersion) {
                invalidatePublication(*publication.component);
                break;
            }
        }
    }
}

/// @brief ReleaseSkinningComputeCaches — シーン切り替え / リソースリセット時にキャッシュを捨てる。
/// @note キャッシュの GPU ハンドルはデバイス世代に属するため、デバイスリセット後に古い世代の
/// @note ハンドルを再利用しない。通常のシーン上の破棄は Execute 側で回収する。
void ReleaseSkinningComputeCaches()
{
    auto* resources = renderer::ResourceManager::Active();
    if (resources && resources == g_cacheResources && resources->GetResetVersion() == g_cacheResetVersion) {
        for (const auto& [key, handle] : g_srcVertexCache)
            if (resources->Get(handle)) resources->Release(handle);
        g_bonePalettePool.ReleaseAll(*resources);
    }
    g_bonePalettePool = renderer::DynamicStructuredBufferPool{};
    g_srcVertexCache.clear();
    g_framePalettes.clear();
    g_usedSourceKeys.clear();
    g_cacheUsageFrame = (std::numeric_limits<uint64_t>::max)();
    g_cacheResources = nullptr;
    g_cacheResetVersion = 0;
}


} /// @note namespace fbzz::scene
