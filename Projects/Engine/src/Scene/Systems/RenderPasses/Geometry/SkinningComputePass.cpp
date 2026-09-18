/// @file    SkinningComputePass.cpp
/// @brief   コンピュートスキニング — ボーン変形を 1 フレームに 1 回だけ計算して共有する。
/// @author  Hasegawa Jin
/// @date    2026-08-14
///
/// スキニングは各マテリアルの VS 内で毎回計算されていた。ここで 1 回だけ変形し、静的メッシュと
/// 同じ頂点レイアウト (`renderer::Vertex`) へ書き出すことで後続パスは「ただの静的メッシュ」として
/// 扱える。次の場合は従来の VS スキニングへフォールバックする: Animator 未評価、モーフ有効、
/// バックエンドが GPU 書き込み頂点バッファ非対応。
#include "GeometryPasses.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Renderer/Mesh.hpp"
#include "Engine/Renderer/ComputeCall.hpp"
#include "Engine/Renderer/DynamicBufferPool.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Scene/Components/MaterialComponent.hpp"
#include "Engine/Scene/Components/AnimatorComponent.hpp"
#include "Engine/Scene/Components/SkinnedMeshRenderer.hpp"
#include <Math/Matrix4.hpp>
#include <cstdint>
#include <functional>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::scene {

namespace {

/// CS のスレッドグループ幅。SkinningCompute.cs.hlsl の SKINNING_GROUP_SIZE と一致させること。
constexpr uint32_t kSkinningGroupSize = 64u;

/// StructuredBuffer は密詰め (cbuffer のような 16 バイト整列パディングが入らない) なので、
/// HLSL 側の struct と C++ 側でオフセットが 1 対 1 に対応する。
/// ここがずれると頂点が明後日の方向へ飛ぶ形で壊れるため、サイズで固定しておく。
static_assert(sizeof(renderer::SkinnedVertex) == 76,
    "SkinSrcVertex in SkinningCompute.cs.hlsl must match renderer::SkinnedVertex (76 bytes)");
static_assert(sizeof(renderer::Vertex) == 60,
    "SkinnedOutVertex in SkinningCompute.cs.hlsl must match renderer::Vertex (60 bytes)");

/// SkinningConstants (b0) — CS 側と一致させること。
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
        return result;
    }
};

/// 入力頂点の StructuredBuffer は Mesh 単位で共有する (ポーズに依存しない生データのため)。同じ
/// モデルを何体出しても入力は 1 本で足りる。Mesh の破棄・再生成でアドレスが再利用されても古い
/// 頂点バッファを掴まないよう、CPU 配列と元 GPU バッファの世代を値としてキーにする。
std::unordered_map<SourceVertexCacheKey,
                   renderer::ResourceHandle<renderer::StructuredBufferTag>,
                   SourceVertexCacheKeyHash> g_srcVertexCache;

/// ボーンパレットはポーズが毎フレーム変わるので、Animator ごとに 1 本持たずフレームごとに借りる。
/// DX12 の読み取り専用 StructuredBuffer は Upload ヒープへの直 memcpy のため、1 本を使い回すと
/// GPU がまだ実行中の前フレームのスキニングが今フレームのポーズを読んでしまう (詳細は
/// `DynamicBufferPool.hpp`)。
renderer::DynamicStructuredBufferPool g_bonePalettePool;

/// このフレームに借りたパレット。Animator の下に Renderer が何本あっても転送は 1 回で済ませる。
/// キーは EntityID: ComponentArray::Remove は末尾要素を swap するため AnimatorComponent* は
/// 安定した識別子ではなく、EntityID なら generation を含むので削除後の再利用も区別できる。
std::unordered_map<EntityID,
                   renderer::ResourceHandle<renderer::StructuredBufferTag>,
                   EntityIDHash> g_framePalettes;

/// 使用集合はビュー単位ではなくエンジンフレーム全体で累積する。Scene View にだけ見えるメッシュを
/// Game View 側の判定末尾で「未使用」と誤判定すると、次フレームに immutable 入力 SRV を
/// 再アップロードすることになる。
std::unordered_set<SourceVertexCacheKey, SourceVertexCacheKeyHash> g_usedSourceKeys;
uint64_t g_cacheUsageFrame = (std::numeric_limits<uint64_t>::max)();

/// BeginSkinningCacheFrame — 前フレームの全ビューで未使用だったキャッシュだけを回収する。
void BeginSkinningCacheFrame(renderer::ResourceManager& resources, uint64_t frameStamp)
{
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

/// EnsureSourceVertexBuffer — スキニング入力の StructuredBuffer を Mesh 単位で用意する。
renderer::ResourceHandle<renderer::StructuredBufferTag> EnsureSourceVertexBuffer(
    renderer::ResourceManager& resources, const renderer::Mesh& mesh)
{
    /// @note cpuSkinnedVertices はインポート時に保持される。空なら GPU スキニングは行えない。
    if (mesh.cpuSkinnedVertices.empty())
        return {};

    const SourceVertexCacheKey key{
        mesh.cpuSkinnedVertices.data(),
        static_cast<uint32_t>(mesh.cpuSkinnedVertices.size()),
        mesh.vertexBuffer};
    const auto it = g_srcVertexCache.find(key);
    if (it != g_srcVertexCache.end()) {
        return it->second;
    }

    const auto handle = resources.CreateGpuLocalStructuredBuffer(
        mesh.cpuSkinnedVertices.data(),
        static_cast<uint32_t>(mesh.cpuSkinnedVertices.size()),
        static_cast<uint32_t>(sizeof(renderer::SkinnedVertex)));

    g_srcVertexCache[key] = handle;
    return handle;
}

/// @brief AcquireBonePalette — アニメーターのボーンパレットを今フレーム用に借りて転送する。
/// @note cbuffer ではなく StructuredBuffer を使う: 頂点あたり 4 回の動的インデックスアクセスが
///       走り、128 要素の cbuffer 配列への動的アクセスは定数キャッシュの高速経路を外れやすい。
renderer::ResourceHandle<renderer::StructuredBufferTag> AcquireBonePalette(
    renderer::ResourceManager& resources,
    EntityID animatorEntity,
    const AnimatorComponent& anim)
{
    if (anim.boneMatrices.empty()) return {};
    if (const auto it = g_framePalettes.find(animatorEntity); it != g_framePalettes.end())
        return it->second;

    const size_t count  = anim.boneMatrices.size();
    const auto   handle = g_bonePalettePool.Acquire(
        resources, count, static_cast<uint32_t>(sizeof(math::Matrix4)));
    if (handle.IsValid())
        resources.Update(handle, anim.boneMatrices.data(), count * sizeof(math::Matrix4));
    g_framePalettes[animatorEntity] = handle;
    return handle;
}

} // namespace

void ExecuteSkinningComputePass(RenderPassContext& ctx)
{
    auto& resources = ctx.resources;
    auto& h         = ctx.handles;

    if (!h.skinningComputeCS.IsValid()) return;

    BeginSkinningCacheFrame(resources, Time::frameCount);
    auto& usedSourceKeys = g_usedSourceKeys;
    /// @note 各 submesh の Dispatch は別出力へ書き、相互依存しない。DX12 は UAV バリアを
    ///       Dispatch ごとに発行せず、このバッチを閉じる時点の 1 回へ集約する。
    ctx.renderer.BeginComputeBatch();

    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* smr = go.GetComponent<SkinnedMeshRenderer>();
        if (!smr || !smr->enabled || !smr->lodVisible || !smr->model) continue;

        AnimatorComponent* anim = nullptr;
        EntityID animatorEntity = EntityID::INVALID;
        for (GameObject* current = &go; current; current = current->GetParent()) {
            if (auto* candidate = current->GetComponent<AnimatorComponent>()) {
                anim = candidate;
                animatorEntity = current->GetID();
                break;
            }
        }
        if (!anim || anim->boneMatrices.empty()) {
            /// @note パレット未評価。bind pose のままなら VS 経路で十分。
            smr->gpuSkinnedThisFrame = false;
            smr->gpuSkinningFrame = (std::numeric_limits<uint64_t>::max)();
            smr->gpuSkinningVertexCount = 0;
            smr->gpuSkinningDispatchCount = 0;
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
        }

        /// @note 出力バッファ配列は model->meshes と同じ添字で確保する。ローカルスロット数まで
        ///       詰めないのは、ShadowPass / ForwardPass も同じ添字で引くため。多少の空きスロット
        ///       より、添字が 1 つに定まっていることを優先する。
        const size_t meshCount = smr->model->meshes.size();
        smr->skinnedVertexBuffers.resize(meshCount);

        const uint64_t frameStamp = Time::frameCount;
        if (smr->gpuSkinningFrame == frameStamp) {
            /// @note Scene/Game View の 2 回目以降は同じポーズと出力を共有する。
            ///       キャッシュ寿命判定にはこのビューでも使用中の入力を記録しておく。
            const size_t reusedSlotCount = smr->SubmeshCount();
            for (size_t slot = 0; slot < reusedSlotCount; ++slot) {
                const uint32_t submeshIndex = smr->SubmeshAt(slot);
                if (submeshIndex == UINT32_MAX || submeshIndex >= meshCount) continue;
                const auto& meshPtr = smr->model->meshes[submeshIndex];
                if (!meshPtr) continue;
                usedSourceKeys.insert(SourceVertexCacheKey{
                    meshPtr->cpuSkinnedVertices.data(),
                    static_cast<uint32_t>(meshPtr->cpuSkinnedVertices.size()),
                    meshPtr->vertexBuffer});
            }
            /// @note 最後に描画されたビューが Snapshot を上書きしても、当該フレームの実処理量を表示する。
            ctx.statsSkinningVertexCount += smr->gpuSkinningVertexCount;
            ctx.statsSkinningDispatchCount += smr->gpuSkinningDispatchCount;
            continue;
        }

        const auto bonePalette = AcquireBonePalette(resources, animatorEntity, *anim);
        if (!bonePalette.IsValid()) {
            smr->gpuSkinnedThisFrame = false;
            smr->gpuSkinningFrame = frameStamp;
            smr->gpuSkinningVertexCount = 0;
            smr->gpuSkinningDispatchCount = 0;
            continue;
        }

        bool anySkinned = false;
        uint64_t skinnedVertexCount = 0;
        uint32_t skinningDispatchCount = 0;
        /// @note この Renderer が実際に描く submesh だけをスキニングする。ノードごとに子 GameObject
        ///       へ分けた構成では同じモデルを参照する Renderer が複数並ぶため、全員がモデル全体を
        ///       変形すると子の数だけ同じ計算を繰り返し、負荷が submesh 数倍になる。
        const size_t slotCount = smr->SubmeshCount();
        for (size_t slot = 0; slot < slotCount; ++slot) {
            const uint32_t submeshIndex = smr->SubmeshAt(slot);
            if (submeshIndex == UINT32_MAX || submeshIndex >= meshCount) continue;
            /// @note 以降の mi は model->meshes / 各バッファ配列と同じ添字。
            const size_t mi = static_cast<size_t>(submeshIndex);
            const auto& meshPtr = smr->model->meshes[mi];
            if (!meshPtr) continue;
            usedSourceKeys.insert(SourceVertexCacheKey{
                meshPtr->cpuSkinnedVertices.data(),
                static_cast<uint32_t>(meshPtr->cpuSkinnedVertices.size()),
                meshPtr->vertexBuffer});

            /// @note モーフが実際に適用されている submesh だけ従来の VS 経路へ落とす。入力
            ///       StructuredBuffer は Mesh 単位で共有するため、インスタンス固有のモーフ差分を
            ///       混ぜると他インスタンスまで巻き込む。
            /// @note morphVertexBuffers はモーフを持たないモデルでも meshCount ぶん resize される
            ///       (`AnimatorSystem::UpdateMorphVertexBuffers`)。空判定では全スキンドメッシュが
            ///       この経路から外れるため、ハンドルが有効かどうかで見る。
            if (mi < smr->morphVertexBuffers.size() && smr->morphVertexBuffers[mi].IsValid())
                continue;

            const auto srcVertices = EnsureSourceVertexBuffer(resources, *meshPtr);
            if (!srcVertices.IsValid()) continue;

            const uint32_t vertexCount = static_cast<uint32_t>(meshPtr->cpuSkinnedVertices.size());
            if (vertexCount == 0) continue;

            /// @note 出力バッファはレンダラーインスタンスごと (ポーズが違うため)。初回のみ確保。
            auto& outBuffer = smr->skinnedVertexBuffers[mi];
            if (!outBuffer.IsValid()) {
                outBuffer = resources.CreateGpuWritableVertexBuffer(
                    static_cast<size_t>(vertexCount) * sizeof(renderer::Vertex),
                    static_cast<uint32_t>(sizeof(renderer::Vertex)));
                /// @note バックエンド未対応ならここで無効ハンドルが返る。VS 経路へ落とす。
                if (!outBuffer.IsValid()) break;
            }

            SkinningCB cbData{};
            cbData.vertexCount = vertexCount;
            resources.Update(h.skinningCB, &cbData, sizeof(SkinningCB));

            renderer::ComputeCall call;
            call.shader             = h.skinningComputeCS;
            call.constantBuffers[0] = h.skinningCB;
            /// @note t14: 入力頂点
            call.srvBuffers[14]     = srcVertices;
            /// @note t15: ボーンパレット
            call.srvBuffers[15]     = bonePalette;
            /// @note u4:  出力頂点
            call.uavVertexBuffer    = outBuffer;
            call.dispatchX          = (vertexCount + kSkinningGroupSize - 1) / kSkinningGroupSize;
            call.dispatchY          = 1;
            call.dispatchZ          = 1;
            ctx.renderer.Dispatch(call, resources);

            anySkinned = true;
            skinnedVertexCount += vertexCount;
            ++skinningDispatchCount;
        }

        smr->gpuSkinnedThisFrame = anySkinned;
        smr->gpuSkinningFrame = frameStamp;
        smr->gpuSkinningVertexCount = skinnedVertexCount;
        smr->gpuSkinningDispatchCount = skinningDispatchCount;
        ctx.statsSkinningVertexCount += skinnedVertexCount;
        ctx.statsSkinningDispatchCount += skinningDispatchCount;
    }

    ctx.renderer.EndComputeBatch();
}

/// @brief ReleaseSkinningComputeCaches — シーン切り替え / リソースリセット時にキャッシュを捨てる。
/// @note キャッシュの GPU ハンドルはデバイス世代に属するため、デバイスリセット後に古い世代の
///       ハンドルを再利用しない。通常のシーン上の破棄は Execute 側で回収する。
void ReleaseSkinningComputeCaches()
{
    g_srcVertexCache.clear();
    g_framePalettes.clear();
    g_usedSourceKeys.clear();
    g_cacheUsageFrame = (std::numeric_limits<uint64_t>::max)();
}


void SkinningComputePass::Execute(PassResources&, RenderPassContext& ctx)
{
    ExecuteSkinningComputePass(ctx);
}
} // namespace fbzz::scene
