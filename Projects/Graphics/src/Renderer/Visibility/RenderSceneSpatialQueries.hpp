/// @file    RenderSceneSpatialQueries.hpp
/// @brief   ビュー別の視錐台から、不変のスナップショットを保持する描画候補列を得る。
/// @author  Hasegawa Jin
/// @date    2026-10-23
#pragma once
#include <Graphics/Api.hpp>

#include "RenderMeshSpatialIndex.hpp"
#include <Graphics/Scene/RenderScene.hpp>
#include <Core/Profiler/ProfileScope.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace fbzz::renderer { struct RenderPassContext; }
namespace fbzz::renderer::spatial_detail {

struct BvhDebugCaptureTicket {
    RenderBvhDebugRequest request;
    uint64_t revision = 0;
};

/// @brief ビュー別の BVH 診断要求と応答をスレッド間で同期する。
/// @note 保持するのは上限付きの不変な診断値。描画スナップショットや BVH 本体は所有しない。
class BvhDebugPublication {
public:
    bool SetRequest(const RenderBvhDebugRequest& request)
    {
        auto bounded = request;
        bounded.maxNodes = (std::min)(bounded.maxNodes, RENDER_BVH_DEBUG_MAX_NODES);
        if (bounded.sceneGeneration == 0) {
            bounded.enabled = false;
        }
        std::lock_guard lock(m_mutex);
        if (bounded == m_request) {
            return false;
        }
        m_request = bounded;
        ++m_revision;
        m_snapshot.reset();
        m_enabled.store(bounded.enabled, std::memory_order_release);
        return true;
    }

    [[nodiscard]] BvhDebugCaptureTicket Begin(uint64_t sceneGeneration, uint32_t viewOutputId = 0,
        uint32_t viewOutputGeneration = 0) const
    {
        if (!m_enabled.load(std::memory_order_acquire)) {
            return {};
        }
        std::lock_guard lock(m_mutex);
        const bool matchingView = m_request.viewOutputId == 0 || (m_request.viewOutputId == viewOutputId
            && m_request.viewOutputGeneration == viewOutputGeneration);
        return m_request.enabled && m_request.sceneGeneration == sceneGeneration && matchingView ? BvhDebugCaptureTicket{m_request, m_revision}
            : BvhDebugCaptureTicket{};
    }

    void Publish(const BvhDebugCaptureTicket& ticket, std::shared_ptr<const RenderBvhDebugSnapshot> snapshot)
    {
        if (!snapshot || ticket.revision == 0) {
            return;
        }
        std::lock_guard lock(m_mutex);
        if (!m_request.enabled || ticket.revision != m_revision || snapshot->requestRevision != ticket.revision
            || snapshot->sceneGeneration != m_request.sceneGeneration || snapshot->nodes.size() > m_request.maxNodes) {
            return;
        }
        if (m_request.viewOutputId != 0 && (snapshot->viewOutputId != m_request.viewOutputId
            || snapshot->viewOutputGeneration != m_request.viewOutputGeneration)) {
            return;
        }
        if (m_snapshot && snapshot->contextFrameStamp < m_snapshot->contextFrameStamp) {
            return;
        }
        m_snapshot = std::move(snapshot);
    }

    [[nodiscard]] std::shared_ptr<const RenderBvhDebugSnapshot> Snapshot() const
    {
        std::lock_guard lock(m_mutex);
        return m_snapshot;
    }

    void Invalidate()
    {
        std::lock_guard lock(m_mutex);
        m_request.enabled = false;
        ++m_revision;
        m_snapshot.reset();
        m_enabled.store(false, std::memory_order_release);
    }

private:
    mutable std::mutex m_mutex;
    std::atomic<bool> m_enabled{false};
    RenderBvhDebugRequest m_request;
    uint64_t m_revision = 1;
    std::shared_ptr<const RenderBvhDebugSnapshot> m_snapshot;
};

/// @brief 候補を参照先のスナップショットとともに保持する。
/// @note fullScan の場合は ordinals が空でも Objects() 全体が候補になる。
struct CandidateBatch {
    std::shared_ptr<const RenderScene> snapshot;
    std::vector<uint32_t> ordinals;
    UpdateDiagnostics update;
    QueryDiagnostics query;
    bool fullScan = true, refreshed = false, validated = false, validationMismatch = false;
    bool deltaApplied = false, geometryReused = false;

    [[nodiscard]] size_t Size() const { return fullScan ? (snapshot ? snapshot->Objects().size() : 0) : ordinals.size(); }
    /// @pre position は [0, Size()) の候補列添字。戻り値は snapshot->Objects() の添字。
    [[nodiscard]] uint32_t ObjectIndex(size_t position) const
    {
        return fullScan ? static_cast<uint32_t>(position) : ordinals[position];
    }
};

/// @brief シーンごとに形状境界を再利用するビュー横断キャッシュ。
/// @note インスタンスは 1 スレッドで使う。通常経路では thread_local の有限スロットに保持する。
/// @note 配列領域が再利用されるため、公開版は shared_ptr の所有者と公開番号・フレーム・シーン世代で照合する。
/// @note キャッシュの更新後も、返却済みの候補は各候補列が所有するスナップショットを指す。
class RenderSceneSpatialCache {
public:
    /// @param[in] padding 正のワールド境界半径に加える余白 [m]。負値は 0 として扱う。
    /// @param[in] frusta 候補を集める視錐台の和集合。各視錐台は呼び出し中だけ参照する。
    [[nodiscard]] CandidateBatch Collect(std::shared_ptr<const RenderScene> snapshot, float padding,
        std::span<const math::Frustum* const> frusta, const IndexOptions& options, bool validate = false,
        const RenderBvhDebugRequest* debugRequest = nullptr, RenderBvhDebugSnapshot* debugSnapshot = nullptr)
    {
        CandidateBatch batch;
        batch.snapshot = std::move(snapshot);
        auto* capture = debugRequest && debugRequest->enabled && debugSnapshot && batch.snapshot
            && debugRequest->sceneGeneration == batch.snapshot->sceneGeneration ? debugSnapshot : nullptr;
        if (capture) {
            capture->sceneGeneration = batch.snapshot->sceneGeneration;
            capture->snapshotSerial = batch.snapshot->snapshotSerial;
            capture->frameStamp = batch.snapshot->frameStamp;
            capture->shadowVolumeCount = static_cast<uint32_t>(frusta.size());
            capture->availability = RenderBvhDebugAvailability::LINEAR_MODE;
            capture->nodes.clear();
            capture->totalNodes = capture->unboundedSources = capture->visitedNodes = capture->testedLeaves = capture->candidates = 0;
            capture->examinedNodes = capture->selectionSourcesExamined = 0;
            capture->truncated = capture->selectedFound = capture->selectedLookupTruncated = false;
        }
        if (!batch.snapshot || options.mode == QueryMode::LINEAR || batch.snapshot->sceneGeneration == 0) {
            return batch;
        }
        auto& slot = SlotFor(batch.snapshot->sceneGeneration);
        const bool sameOwner = !slot.owner.owner_before(batch.snapshot) && !batch.snapshot.owner_before(slot.owner);
        const bool samePublication = batch.snapshot->snapshotSerial != 0 && sameOwner
            && slot.serial == batch.snapshot->snapshotSerial && slot.frame == batch.snapshot->frameStamp;
        const bool compatible = slot.padding == std::bit_cast<uint32_t>(padding) && SameOptions(slot.options, options);
        if (!samePublication || !compatible) {
            const auto objects = batch.snapshot->Objects();
            const auto* journal = batch.snapshot->geometry.SpatialUpdate();
            const bool knownJournal = options.updateMode == UpdateMode::STABLE && journal && journal->version != 0
                && journal->objectCount == objects.size();
            const bool matchingShape = compatible && slot.sources.size() == objects.size();
            batch.geometryReused = knownJournal && matchingShape && slot.geometryVersion == journal->version;
            if (!batch.geometryReused) {
                bool canApply = knownJournal && matchingShape && !journal->fullReplacement && journal->baseVersion != 0
                    && journal->version > journal->baseVersion && slot.geometryVersion == journal->baseVersion;
                if (canApply) {
                    for (uint32_t ordinal : journal->changedObjectOrdinals) {
                        if (ordinal >= objects.size()) { canApply = false; break; }
                    }
                }
                if (canApply) {
                    {
                        FBZZ_PROFILE_SCOPE("RenderMeshSpatial::SourceProjection");
                        for (uint32_t ordinal : journal->changedObjectOrdinals) {
                            slot.sources[ordinal] = Project(objects[ordinal], padding);
                        }
                    }
                    {
                        FBZZ_PROFILE_SCOPE("RenderMeshSpatial::WitnessAndUpdate");
                        batch.deltaApplied = slot.index.ApplyChanges(batch.snapshot->sceneGeneration, slot.sources,
                            journal->changedObjectOrdinals, options, slot.update);
                    }
                }
                if (!batch.deltaApplied) {
                    {
                        FBZZ_PROFILE_SCOPE("RenderMeshSpatial::SourceProjection");
                        slot.sources.clear();
                        slot.sources.reserve(objects.size());
                        for (const auto& object : objects) {
                            slot.sources.push_back(Project(object, padding));
                        }
                    }
                    {
                        FBZZ_PROFILE_SCOPE("RenderMeshSpatial::WitnessAndUpdate");
                        slot.update = slot.index.Refresh(batch.snapshot->sceneGeneration, slot.sources, options);
                    }
                }
                slot.update.retainedBytes += slot.sources.capacity() * sizeof(MeshSource);
                batch.refreshed = true;
            }
            slot.geometryVersion = knownJournal ? journal->version : 0;
            slot.owner = batch.snapshot;
            slot.serial = batch.snapshot->snapshotSerial;
            slot.frame = batch.snapshot->frameStamp;
            slot.padding = std::bit_cast<uint32_t>(padding);
            slot.options = options;
        }
        batch.update = slot.update;
        if (!batch.refreshed) {
            batch.update.changedBounds = batch.update.changedMembership = 0;
            batch.update.witnessedSources = 0;
            batch.update.rebuiltTrees = batch.update.refittedTrees = batch.update.refittedLeaves = 0;
            batch.update.refitSortedAncestors = batch.update.refitScannedNodes = 0;
        }
        {
            FBZZ_PROFILE_SCOPE("RenderMeshSpatial::QueryUnion");
            slot.index.QueryUnion(frusta, batch.ordinals, batch.query, options.mode, debugRequest, capture);
        }
        batch.fullScan = false;
        if (validate) {
            FBZZ_PROFILE_SCOPE("RenderMeshSpatial::ValidateUnion");
            QueryDiagnostics referenceDiagnostics;
            std::vector<uint32_t> reference;
            std::vector<MeshSource> currentSources;
            currentSources.reserve(batch.snapshot->Objects().size());
            for (const auto& object : batch.snapshot->Objects()) {
                currentSources.push_back(Project(object, padding));
            }
            RenderMeshSpatialIndex referenceIndex;
            auto referenceOptions = options;
            referenceOptions.mode = QueryMode::LINEAR;
            (void)referenceIndex.Refresh(batch.snapshot->sceneGeneration, currentSources, referenceOptions);
            referenceIndex.QueryUnion(frusta, reference, referenceDiagnostics, QueryMode::LINEAR);
            batch.validated = true;
            batch.validationMismatch = reference != batch.ordinals;
            if (batch.validationMismatch) {
                if (capture) {
                    capture->availability = RenderBvhDebugAvailability::VALIDATION_FAILED;
                    capture->nodes.clear();
                }
                batch.fullScan = true;
                batch.ordinals.clear();
                // 検証に失敗した形状の公開版は、後続の差分適用や同一版の再利用の基点にしない。
                slot.geometryVersion = 0;
                slot.owner.reset();
                slot.serial = 0;
            }
        }
        return batch;
    }

private:
    /// @note スナップショットは Component の寿命トークンを持たない。抽出側の差分記録が置換と確定した境界を記録する。
    static MeshSource Project(const RenderObject& object, float padding)
    {
        return {{object.sourceIndex, object.sourceGeneration, 0, object.skinned}, object.boundsCenter,
            object.boundsRadius > 0 ? object.boundsRadius + (std::max)(padding, 0.0f) : 0.0f, object.skinned};
    }

    struct Slot {
        uint64_t generation = 0, serial = 0, frame = 0;
        uint64_t geometryVersion = 0;
        uint32_t padding = 0;
        std::weak_ptr<const RenderScene> owner;
        IndexOptions options;
        RenderMeshSpatialIndex index;
        std::vector<MeshSource> sources;
        UpdateDiagnostics update;
    };

    Slot& SlotFor(uint64_t generation)
    {
        // 最新の公開版が退役したシーンのスロットは、他シーンの問い合わせ時に解放する。
        for (auto& slot : m_slots) {
            if (slot.generation != 0 && slot.generation != generation && slot.owner.expired()) {
                slot = {};
            }
        }
        for (auto& slot : m_slots) {
            if (slot.generation == generation) {
                return slot;
            }
        }
        for (auto& slot : m_slots) {
            if (slot.generation == 0) { slot.generation = generation; return slot; }
        }
        auto& slot = m_slots[m_nextSlot];
        m_nextSlot = (m_nextSlot + 1) % m_slots.size();
        slot = {};
        slot.generation = generation;
        return slot;
    }

    std::array<Slot, 4> m_slots;
    size_t m_nextSlot = 0;
};

/// @note 未設定・linear では境界変換と索引更新を省く。環境変数はプロセス内の初回呼び出しで確定する。
[[nodiscard]] FBZZ_GRAPHICS_API bool MeshSpatialQueriesEnabled();
/// @note 影の候補はライト視錐台の和集合で求める。カメラや反射面の可視判定とは独立する。
[[nodiscard]] FBZZ_GRAPHICS_API CandidateBatch CollectShadowCandidates(const RenderPassContext& context, uint32_t updateMask);

} // namespace fbzz::renderer::spatial_detail
