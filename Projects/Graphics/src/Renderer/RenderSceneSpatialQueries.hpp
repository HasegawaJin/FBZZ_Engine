/// @file    RenderSceneSpatialQueries.hpp
/// @brief   所有snapshotと独立query volumeを結び、旧batchから独立した候補を返す。
/// @author  Hasegawa Jin
/// @date    2026-10-23
#pragma once

#include "RenderMeshSpatialIndex.hpp"
#include <Graphics/Renderer/RenderScene.hpp>
#include <Core/Profiler/ProfileScope.hpp>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace fbzz::renderer { struct RenderPassContext; }
namespace fbzz::renderer::spatial_detail {

struct BvhDebugCaptureTicket {
    RenderBvhDebugRequest request;
    uint64_t revision = 0;
};

/// @note This mailbox serializes only bounded immutable diagnostic publications; it never owns a RenderScene or a tree.
class BvhDebugPublication {
public:
    bool SetRequest(const RenderBvhDebugRequest& request)
    {
        auto bounded = request;
        bounded.maxNodes = (std::min)(bounded.maxNodes, RENDER_BVH_DEBUG_MAX_NODES);
        if (bounded.sceneGeneration == 0) bounded.enabled = false;
        std::lock_guard lock(m_mutex);
        if (bounded == m_request) return false;
        m_request = bounded;
        ++m_revision;
        m_snapshot.reset();
        m_enabled.store(bounded.enabled, std::memory_order_release);
        return true;
    }

    [[nodiscard]] BvhDebugCaptureTicket Begin(uint64_t sceneGeneration, uint32_t viewOutputId = 0,
        uint32_t viewOutputGeneration = 0) const
    {
        if (!m_enabled.load(std::memory_order_acquire)) return {};
        std::lock_guard lock(m_mutex);
        const bool matchingView = m_request.viewOutputId == 0 || (m_request.viewOutputId == viewOutputId
            && m_request.viewOutputGeneration == viewOutputGeneration);
        return m_request.enabled && m_request.sceneGeneration == sceneGeneration && matchingView ? BvhDebugCaptureTicket{m_request, m_revision}
            : BvhDebugCaptureTicket{};
    }

    void Publish(const BvhDebugCaptureTicket& ticket, std::shared_ptr<const RenderBvhDebugSnapshot> snapshot)
    {
        if (!snapshot || ticket.revision == 0) return;
        std::lock_guard lock(m_mutex);
        if (!m_request.enabled || ticket.revision != m_revision || snapshot->requestRevision != ticket.revision
            || snapshot->sceneGeneration != m_request.sceneGeneration || snapshot->nodes.size() > m_request.maxNodes) return;
        if (m_request.viewOutputId != 0 && (snapshot->viewOutputId != m_request.viewOutputId
            || snapshot->viewOutputGeneration != m_request.viewOutputGeneration)) return;
        if (m_snapshot && snapshot->contextFrameStamp < m_snapshot->contextFrameStamp) return;
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

struct CandidateBatch {
    std::shared_ptr<const RenderScene> snapshot;
    std::vector<uint32_t> ordinals;
    UpdateDiagnostics update;
    QueryDiagnostics query;
    bool fullScan = true, refreshed = false, validated = false, validationMismatch = false;
    bool deltaApplied = false, geometryReused = false;

    [[nodiscard]] size_t Size() const { return fullScan ? (snapshot ? snapshot->Objects().size() : 0) : ordinals.size(); }
    [[nodiscard]] uint32_t ObjectIndex(size_t position) const
    {
        return fullScan ? static_cast<uint32_t>(position) : ordinals[position];
    }
};

/// @note One producer thread owns each cache; production uses bounded thread_local storage and never stores Scene pointers.
/// @note The cache identifies publications by shared owner plus serial/frame/scene lifetime, never by a pooled array address alone.
/// @note Old batches own their snapshot and ordinal vector; refreshing any slot cannot mutate a previously returned batch.
class RenderSceneSpatialCache {
public:
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
        if (!batch.snapshot || options.mode == QueryMode::LINEAR || batch.snapshot->sceneGeneration == 0) return batch;
        auto& slot = SlotFor(batch.snapshot->sceneGeneration);
        const bool sameOwner = !slot.owner.owner_before(batch.snapshot) && !batch.snapshot.owner_before(slot.owner);
        const bool samePublication = batch.snapshot->snapshotSerial != 0 && sameOwner
            && slot.serial == batch.snapshot->snapshotSerial && slot.frame == batch.snapshot->frameStamp;
        const bool compatible = slot.padding == std::bit_cast<uint32_t>(padding) && SameOptions(slot.options, options);
        if (!samePublication || !compatible) {
            const auto objects = batch.snapshot->Objects();
            const auto& journal = batch.snapshot->spatialUpdate;
            const bool knownJournal = options.updateMode == UpdateMode::STABLE && journal && journal->version != 0
                && journal->objectCount == objects.size();
            const bool matchingShape = compatible && slot.sources.size() == objects.size();
            batch.geometryReused = knownJournal && matchingShape && slot.geometryVersion == journal->version;
            if (!batch.geometryReused) {
                bool canApply = knownJournal && matchingShape && !journal->fullReplacement && journal->baseVersion != 0
                    && journal->version > journal->baseVersion && slot.geometryVersion == journal->baseVersion;
                if (canApply) {
                    for (uint32_t ordinal : journal->changedObjectOrdinals)
                        if (ordinal >= objects.size()) { canApply = false; break; }
                }
                if (canApply) {
                    {
                        FBZZ_PROFILE_SCOPE("RenderMeshSpatial::SourceProjection");
                        for (uint32_t ordinal : journal->changedObjectOrdinals)
                            slot.sources[ordinal] = Project(objects[ordinal], padding);
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
                        for (const auto& object : objects) slot.sources.push_back(Project(object, padding));
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
            for (const auto& object : batch.snapshot->Objects()) currentSources.push_back(Project(object, padding));
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
                /// @note A rejected producer lineage cannot authorize a later delta or same-version reuse.
                slot.geometryVersion = 0;
                slot.owner.reset();
                slot.serial = 0;
            }
        }
        return batch;
    }

private:
    /// @note Snapshot values contain no Component lifetime token; producer journals cover replacement and finalized bounds before publication.
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
        /// @note A scene that no longer publishes snapshots retires on the next other-scene query; no callback or Scene reference is retained.
        for (auto& slot : m_slots)
            if (slot.generation != 0 && slot.generation != generation && slot.owner.expired()) slot = {};
        for (auto& slot : m_slots) if (slot.generation == generation) return slot;
        for (auto& slot : m_slots) if (slot.generation == 0) { slot.generation = generation; return slot; }
        auto& slot = m_slots[m_nextSlot];
        m_nextSlot = (m_nextSlot + 1) % m_slots.size();
        slot = {};
        slot.generation = generation;
        return slot;
    }

    std::array<Slot, 4> m_slots;
    size_t m_nextSlot = 0;
};

/// @note Unset/linear bypass all source projection and index maintenance; environment choices are captured once per process.
[[nodiscard]] bool MeshSpatialQueriesEnabled();
/// @note Shadow query owns the union of light frusta; camera visibility and reflection-face visibility are separate consumers.
[[nodiscard]] CandidateBatch CollectShadowCandidates(const RenderPassContext& context, int cascadeCount);

} /// @note namespace fbzz::renderer::spatial_detail
