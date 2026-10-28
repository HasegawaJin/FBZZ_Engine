/// @file    RenderSceneSpatialQueriesTests.cpp
/// @brief   snapshot世代・pool再利用・旧view保持と候補batchの独立所有を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-23
#include <TestKit/TestKit.hpp>
#include "../../../Graphics/src/Renderer/RenderSceneSpatialQueries.hpp"
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <array>
#include <memory>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {
using namespace renderer::spatial_detail;

std::shared_ptr<renderer::RenderScene> Snapshot(uint64_t scene, uint64_t serial, float xOffset = 0)
{
    auto snapshot = std::make_shared<renderer::RenderScene>();
    snapshot->sceneGeneration = scene;
    snapshot->snapshotSerial = serial;
    snapshot->frameStamp = serial;
    for (uint32_t ordinal = 0; ordinal < 128; ++ordinal) {
        renderer::RenderObject object;
        object.sourceIndex = ordinal;
        object.sourceGeneration = 3;
        object.boundsCenter = {static_cast<float>(static_cast<int>(ordinal % 16) - 8) * 8 + xOffset,
            static_cast<float>(static_cast<int>(ordinal / 16) - 4) * 7, 30};
        object.boundsRadius = 1;
        snapshot->objects.push_back(object);
    }
    return snapshot;
}

void Journal(const std::shared_ptr<renderer::RenderScene>& snapshot, uint64_t version, uint64_t base,
    bool fullReplacement, std::vector<uint32_t> changed = {})
{
    auto journal = std::make_shared<renderer::RenderSpatialUpdate>();
    journal->version = version;
    journal->baseVersion = base;
    journal->objectCount = snapshot->Objects().size();
    journal->fullReplacement = fullReplacement;
    journal->changedObjectOrdinals = std::move(changed);
    snapshot->spatialUpdate = std::move(journal);
}

class RenderSceneSpatialQueriesTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        testkit::Fixture::SetUp();
        options.mode = QueryMode::BVH;
        view = math::Frustum::FromViewProjection(math::Matrix4::Perspective(math::ToRad(70.0f), 1, 0.1f, 100)
            * math::Matrix4::LookAt({0, 0, 0}, {0, 0, 1}, math::Vector3::UP));
    }
    CandidateBatch Collect(RenderSceneSpatialCache& cache, const std::shared_ptr<const renderer::RenderScene>& snapshot,
        float padding = 0)
    {
        const std::array<const math::Frustum*, 1> volumes{&view};
        return cache.Collect(snapshot, padding, volumes, options, true);
    }
    IndexOptions options;
    math::Frustum view;
};
} /// @note namespace

TEST_F(RenderSceneSpatialQueriesTest, LinearDefaultSkipsProjectionAndIndexMaintenance)
{
    auto snapshot = Snapshot(1, 1);
    RenderSceneSpatialCache cache;
    options.mode = QueryMode::LINEAR;
    const auto batch = Collect(cache, snapshot);
    EXPECT_TRUE(batch.fullScan);
    EXPECT_FALSE(batch.refreshed);
    EXPECT_FALSE(batch.validated);
    EXPECT_EQ(batch.update.sources, 0u);
    EXPECT_EQ(batch.Size(), snapshot->Objects().size());
    EXPECT_EQ(batch.ObjectIndex(127), 127u);
}

TEST_F(RenderSceneSpatialQueriesTest, OptInNullSnapshotReturnsLegacyFallbackWithoutIndexAccess)
{
    RenderSceneSpatialCache cache;
    const auto batch = Collect(cache, {});
    EXPECT_TRUE(batch.fullScan);
    EXPECT_FALSE(batch.snapshot);
    EXPECT_FALSE(batch.refreshed);
    EXPECT_FALSE(batch.validated);
    EXPECT_EQ(batch.Size(), 0u);
    EXPECT_EQ(batch.update.sources, 0u);
}

TEST_F(RenderSceneSpatialQueriesTest, LaterSnapshotAndOldSnapshotReentryKeepIndependentBatches)
{
    auto oldSnapshot = Snapshot(2, 10);
    RenderSceneSpatialCache cache;
    auto previous = Collect(cache, oldSnapshot);
    ASSERT_FALSE(previous.validationMismatch);
    const auto saved = previous.ordinals;
    auto current = Collect(cache, Snapshot(2, 11, 10000));
    EXPECT_TRUE(current.refreshed);
    EXPECT_TRUE(current.ordinals.empty());
    EXPECT_EQ(previous.ordinals, saved);
    EXPECT_EQ(previous.snapshot, oldSnapshot);
    const auto reentered = Collect(cache, oldSnapshot);
    EXPECT_TRUE(reentered.refreshed);
    EXPECT_EQ(reentered.ordinals, saved);
    EXPECT_FALSE(reentered.validationMismatch);
}

TEST_F(RenderSceneSpatialQueriesTest, SamePublicationSharesIndexButPaddingIsAnIndependentWitness)
{
    auto snapshot = Snapshot(3, 20);
    RenderSceneSpatialCache cache;
    const auto first = Collect(cache, snapshot);
    const auto second = Collect(cache, snapshot);
    EXPECT_TRUE(first.refreshed);
    EXPECT_FALSE(second.refreshed);
    EXPECT_EQ(second.update.changedBounds, 0u);
    EXPECT_EQ(second.update.refittedTrees + second.update.rebuiltTrees, 0u);
    EXPECT_EQ(first.ordinals, second.ordinals);
    const auto padded = Collect(cache, snapshot, 20);
    EXPECT_TRUE(padded.refreshed);
    EXPECT_GE(padded.ordinals.size(), first.ordinals.size());
    EXPECT_FALSE(padded.validationMismatch);
}

TEST_F(RenderSceneSpatialQueriesTest, PooledPublicationWithSameOwnerRequiresNewSerialWitness)
{
    auto pooled = Snapshot(4, 30);
    RenderSceneSpatialCache cache;
    {
        const auto initial = Collect(cache, pooled);
        EXPECT_FALSE(initial.ordinals.empty());
    }
    /// @note The prior batch is retired before the producer reuses its publication storage.
    pooled->snapshotSerial = 31;
    pooled->frameStamp = 31;
    for (auto& object : pooled->objects) object.boundsCenter.x += 10000;
    const auto current = Collect(cache, pooled);
    EXPECT_TRUE(current.refreshed);
    EXPECT_TRUE(current.ordinals.empty());
    EXPECT_FALSE(current.validationMismatch);
}

TEST_F(RenderSceneSpatialQueriesTest, MoreThanFourLiveScenesEvictOnlyPrivateValues)
{
    RenderSceneSpatialCache cache;
    std::vector<CandidateBatch> retained;
    std::vector<std::vector<uint32_t>> saved;
    for (uint64_t scene = 5; scene < 12; ++scene) {
        retained.push_back(Collect(cache, Snapshot(scene, scene * 10)));
        saved.push_back(retained.back().ordinals);
    }
    for (size_t index = 0; index < retained.size(); ++index) {
        const auto rebuilt = Collect(cache, retained[index].snapshot);
        EXPECT_EQ(retained[index].ordinals, saved[index]);
        EXPECT_EQ(rebuilt.ordinals, saved[index]);
        EXPECT_FALSE(rebuilt.validationMismatch);
    }
}

TEST_F(RenderSceneSpatialQueriesTest, IndependentCacheOwnersNeverShareMutableTrees)
{
    RenderSceneSpatialCache firstOwner, secondOwner;
    auto snapshot = Snapshot(12, 120);
    const auto first = Collect(firstOwner, snapshot);
    const auto second = Collect(secondOwner, snapshot);
    EXPECT_TRUE(first.refreshed);
    EXPECT_TRUE(second.refreshed);
    EXPECT_EQ(first.ordinals, second.ordinals);
    (void)Collect(firstOwner, Snapshot(12, 121, 10000));
    const auto unchanged = Collect(secondOwner, snapshot);
    EXPECT_FALSE(unchanged.refreshed);
    EXPECT_EQ(unchanged.ordinals, second.ordinals);
}

TEST_F(RenderSceneSpatialQueriesTest, StableJournalUpdatesOnlyTheChangedCurrentOrdinal)
{
    options.updateMode = UpdateMode::STABLE;
    auto initial = Snapshot(13, 130);
    Journal(initial, 100, 0, true);
    RenderSceneSpatialCache cache;
    const auto previous = Collect(cache, initial);
    auto next = Snapshot(13, 131);
    next->objects[72].boundsCenter.x = 10000;
    Journal(next, 101, 100, false, {72});

    const auto current = Collect(cache, next);

    EXPECT_TRUE(current.deltaApplied);
    EXPECT_TRUE(current.refreshed);
    EXPECT_EQ(current.update.witnessedSources, 1u);
    EXPECT_EQ(current.update.changedBounds, 1u);
    EXPECT_EQ(current.update.refittedLeaves, 1u);
    EXPECT_EQ(current.update.rebuiltTrees, 0u);
    EXPECT_FALSE(current.validationMismatch);
    EXPECT_NE(std::find(previous.ordinals.begin(), previous.ordinals.end(), 72u), previous.ordinals.end());
    EXPECT_EQ(std::find(current.ordinals.begin(), current.ordinals.end(), 72u), current.ordinals.end());
    EXPECT_EQ(previous.snapshot, initial);
}

TEST_F(RenderSceneSpatialQueriesTest, EmptyJournalAndSharedGeometryVersionReuseNeedNoFullWitness)
{
    options.updateMode = UpdateMode::STABLE;
    auto initial = Snapshot(14, 140);
    Journal(initial, 200, 0, true);
    RenderSceneSpatialCache cache;
    const auto first = Collect(cache, initial);
    auto next = Snapshot(14, 141);
    Journal(next, 201, 200, false);

    const auto emptyDelta = Collect(cache, next);
    auto materialOnly = Snapshot(14, 142);
    materialOnly->spatialUpdate = next->spatialUpdate;
    const auto shared = Collect(cache, materialOnly);

    EXPECT_TRUE(emptyDelta.deltaApplied);
    EXPECT_EQ(emptyDelta.update.witnessedSources, 0u);
    EXPECT_EQ(emptyDelta.update.refittedTrees + emptyDelta.update.rebuiltTrees, 0u);
    EXPECT_TRUE(shared.geometryReused);
    EXPECT_FALSE(shared.refreshed);
    EXPECT_EQ(shared.update.witnessedSources, 0u);
    EXPECT_EQ(shared.ordinals, first.ordinals);
    EXPECT_FALSE(shared.validationMismatch);
}

TEST_F(RenderSceneSpatialQueriesTest, JournalGapsReplacementAndUnknownVersionRequireFullCurrentWitness)
{
    options.updateMode = UpdateMode::STABLE;
    auto initial = Snapshot(15, 150);
    Journal(initial, 300, 0, true);
    RenderSceneSpatialCache cache;
    (void)Collect(cache, initial);
    auto gap = Snapshot(15, 151, 10000);
    Journal(gap, 302, 301, false, {72});

    const auto gapped = Collect(cache, gap);

    EXPECT_FALSE(gapped.deltaApplied);
    EXPECT_EQ(gapped.update.witnessedSources, gap->Objects().size());
    EXPECT_TRUE(gapped.ordinals.empty());
    EXPECT_FALSE(gapped.validationMismatch);
    auto replacement = Snapshot(15, 152);
    Journal(replacement, 303, 0, true);
    const auto full = Collect(cache, replacement);
    EXPECT_FALSE(full.deltaApplied);
    EXPECT_EQ(full.update.witnessedSources, replacement->Objects().size());
    auto unknown = Snapshot(15, 153, 10000);
    Journal(unknown, 0, 303, false, {72});
    const auto zero = Collect(cache, unknown);
    EXPECT_FALSE(zero.deltaApplied);
    EXPECT_FALSE(zero.geometryReused);
    EXPECT_EQ(zero.update.witnessedSources, unknown->Objects().size());
    EXPECT_FALSE(zero.validationMismatch);
}

TEST_F(RenderSceneSpatialQueriesTest, InvalidJournalOrdinalAndClassificationChangesFallBackToFullWitness)
{
    options.updateMode = UpdateMode::STABLE;
    auto initial = Snapshot(16, 160);
    Journal(initial, 400, 0, true);
    RenderSceneSpatialCache cache;
    (void)Collect(cache, initial);
    auto invalid = Snapshot(16, 161);
    invalid->objects[72].boundsCenter.x = 10000;
    Journal(invalid, 401, 400, false, {72, 999});

    const auto rejected = Collect(cache, invalid);

    EXPECT_FALSE(rejected.deltaApplied);
    EXPECT_EQ(rejected.update.witnessedSources, invalid->Objects().size());
    EXPECT_FALSE(rejected.validationMismatch);
    auto unbounded = Snapshot(16, 162);
    unbounded->objects[72].boundsRadius = 0;
    Journal(unbounded, 402, 401, false, {72});
    const auto unknownBounds = Collect(cache, unbounded);
    EXPECT_FALSE(unknownBounds.deltaApplied);
    EXPECT_EQ(unknownBounds.update.witnessedSources, unbounded->Objects().size());
    EXPECT_FALSE(unknownBounds.validationMismatch);
    auto skinned = Snapshot(16, 163);
    skinned->objects[72].skinned = true;
    Journal(skinned, 403, 402, false, {72});
    const auto changedClass = Collect(cache, skinned);
    EXPECT_FALSE(changedClass.deltaApplied);
    EXPECT_GT(changedClass.update.rebuiltTrees, 0u);
    EXPECT_FALSE(changedClass.validationMismatch);
}

TEST_F(RenderSceneSpatialQueriesTest, PaddingOptionsAndSceneLifetimeInvalidateJournalReuse)
{
    options.updateMode = UpdateMode::STABLE;
    auto initial = Snapshot(17, 170);
    Journal(initial, 500, 0, true);
    RenderSceneSpatialCache cache;
    (void)Collect(cache, initial);

    const auto padded = Collect(cache, initial, 20);
    options.denseUpdate = DenseUpdate::REFIT;
    const auto changedOptions = Collect(cache, initial, 20);
    auto otherScene = Snapshot(18, 180, 10000);
    otherScene->spatialUpdate = initial->spatialUpdate;
    const auto changedScene = Collect(cache, otherScene, 20);

    EXPECT_TRUE(padded.refreshed);
    EXPECT_FALSE(padded.geometryReused);
    EXPECT_EQ(padded.update.witnessedSources, initial->Objects().size());
    EXPECT_TRUE(changedOptions.refreshed);
    EXPECT_FALSE(changedOptions.geometryReused);
    EXPECT_EQ(changedOptions.update.witnessedSources, initial->Objects().size());
    EXPECT_FALSE(changedScene.geometryReused);
    EXPECT_EQ(changedScene.update.witnessedSources, otherScene->Objects().size());
    EXPECT_FALSE(changedScene.validationMismatch);
}

TEST_F(RenderSceneSpatialQueriesTest, LegacyUpdateModeIgnoresProducerJournalReuse)
{
    auto initial = Snapshot(19, 190);
    Journal(initial, 600, 0, true);
    RenderSceneSpatialCache cache;
    (void)Collect(cache, initial);
    auto next = Snapshot(19, 191, 10000);
    next->spatialUpdate = initial->spatialUpdate;

    const auto current = Collect(cache, next);

    EXPECT_FALSE(current.geometryReused);
    EXPECT_FALSE(current.deltaApplied);
    EXPECT_EQ(current.update.witnessedSources, next->Objects().size());
    EXPECT_TRUE(current.ordinals.empty());
    EXPECT_FALSE(current.validationMismatch);
}

TEST_F(RenderSceneSpatialQueriesTest, ValidationRejectsOmittedCurrentBoundsAndRetiresTheJournalLineage)
{
    options.updateMode = UpdateMode::STABLE;
    auto initial = Snapshot(20, 200);
    Journal(initial, 700, 0, true);
    RenderSceneSpatialCache cache;
    (void)Collect(cache, initial);
    auto omitted = Snapshot(20, 201);
    omitted->objects[72].boundsCenter.x = 10000;
    Journal(omitted, 701, 700, false);

    const auto rejected = Collect(cache, omitted);
    const auto recovered = Collect(cache, omitted);

    EXPECT_TRUE(rejected.deltaApplied);
    EXPECT_TRUE(rejected.validationMismatch);
    EXPECT_TRUE(rejected.fullScan);
    EXPECT_TRUE(rejected.ordinals.empty());
    EXPECT_TRUE(recovered.refreshed);
    EXPECT_FALSE(recovered.deltaApplied);
    EXPECT_FALSE(recovered.geometryReused);
    EXPECT_EQ(recovered.update.witnessedSources, omitted->Objects().size());
    EXPECT_FALSE(recovered.validationMismatch);
}

TEST_F(RenderSceneSpatialQueriesTest, ValidationUsesCurrentSnapshotWhenGeometryVersionIsReused)
{
    options.updateMode = UpdateMode::STABLE;
    auto initial = Snapshot(21, 210);
    Journal(initial, 800, 0, true);
    RenderSceneSpatialCache cache;
    (void)Collect(cache, initial);
    auto invalidReuse = Snapshot(21, 211, 10000);
    invalidReuse->spatialUpdate = initial->spatialUpdate;

    const auto rejected = Collect(cache, invalidReuse);

    EXPECT_TRUE(rejected.geometryReused);
    EXPECT_TRUE(rejected.validationMismatch);
    EXPECT_TRUE(rejected.fullScan);
    EXPECT_TRUE(rejected.ordinals.empty());
}

TEST_F(RenderSceneSpatialQueriesTest, DebugSnapshotOwnsOnlyBoundedValuesAcrossSceneDeletionAndReplacement)
{
    auto initial = Snapshot(22, 220);
    std::weak_ptr<const renderer::RenderScene> originalOwner = initial;
    RenderSceneSpatialCache cache;
    const std::array<const math::Frustum*, 1> volumes{&view};
    renderer::RenderBvhDebugRequest request;
    request.enabled = true;
    request.sceneGeneration = 22;
    request.maxNodes = 7;
    renderer::RenderBvhDebugSnapshot previous;
    {
        const auto batch = cache.Collect(initial, 0, volumes, options, true, &request, &previous);
        EXPECT_FALSE(batch.validationMismatch);
    }
    ASSERT_EQ(previous.nodes.size(), 7u);
    const auto savedLow = previous.nodes[0].low;
    initial.reset();

    EXPECT_TRUE(originalOwner.expired());
    renderer::RenderBvhDebugSnapshot current;
    request.sceneGeneration = 23;
    const auto batch = cache.Collect(Snapshot(23, 230, 10000), 0, volumes, options, true, &request, &current);

    EXPECT_FALSE(batch.validationMismatch);
    EXPECT_EQ(previous.sceneGeneration, 22u);
    EXPECT_EQ(previous.snapshotSerial, 220u);
    EXPECT_EQ(previous.frameStamp, 220u);
    EXPECT_EQ(previous.shadowVolumeCount, 1u);
    EXPECT_VEC3_NEAR(previous.nodes[0].low, savedLow, 0.0001f);
    EXPECT_EQ(current.sceneGeneration, 23u);
    EXPECT_EQ(current.snapshotSerial, 230u);
    ASSERT_FALSE(current.nodes.empty());
    EXPECT_GT(current.nodes[0].low.x, previous.nodes[0].low.x + 9000);
}

TEST_F(RenderSceneSpatialQueriesTest, DebugCaptureOffAndWrongSceneDoNotTouchTheDiagnosticOutput)
{
    RenderSceneSpatialCache cache;
    const std::array<const math::Frustum*, 1> volumes{&view};
    renderer::RenderBvhDebugRequest request;
    request.sceneGeneration = 24;
    renderer::RenderBvhDebugSnapshot capture;
    capture.totalNodes = 777;
    capture.nodes.push_back({});
    const auto* retained = capture.nodes.data();

    (void)cache.Collect(Snapshot(24, 240), 0, volumes, options, false, &request, &capture);
    request.enabled = true;
    (void)cache.Collect(Snapshot(25, 250), 0, volumes, options, false, &request, &capture);

    EXPECT_EQ(capture.totalNodes, 777u);
    EXPECT_EQ(capture.nodes.size(), 1u);
    EXPECT_EQ(capture.nodes.data(), retained);
}

TEST_F(RenderSceneSpatialQueriesTest, ValidationFailureRetiresCapturedHierarchy)
{
    options.updateMode = UpdateMode::STABLE;
    auto initial = Snapshot(26, 260);
    Journal(initial, 900, 0, true);
    RenderSceneSpatialCache cache;
    (void)Collect(cache, initial);
    auto invalidReuse = Snapshot(26, 261, 10000);
    invalidReuse->spatialUpdate = initial->spatialUpdate;
    const std::array<const math::Frustum*, 1> volumes{&view};
    renderer::RenderBvhDebugRequest request;
    request.enabled = true;
    request.sceneGeneration = 26;
    renderer::RenderBvhDebugSnapshot capture;

    const auto batch = cache.Collect(invalidReuse, 0, volumes, options, true, &request, &capture);

    EXPECT_TRUE(batch.validationMismatch);
    EXPECT_EQ(capture.availability, renderer::RenderBvhDebugAvailability::VALIDATION_FAILED);
    EXPECT_TRUE(capture.nodes.empty());
}

TEST_F(RenderSceneSpatialQueriesTest, DebugMailboxInvalidationRejectsInFlightAndDifferentScenePublications)
{
    BvhDebugPublication mailbox;
    EXPECT_EQ(mailbox.Begin(27).revision, 0u);
    EXPECT_FALSE(mailbox.Snapshot());
    renderer::RenderBvhDebugRequest request;
    request.enabled = true;
    request.sceneGeneration = 27;
    request.maxNodes = 2;
    ASSERT_TRUE(mailbox.SetRequest(request));
    const auto oldTicket = mailbox.Begin(27);
    auto previous = std::make_shared<renderer::RenderBvhDebugSnapshot>();
    previous->requestRevision = oldTicket.revision;
    previous->sceneGeneration = 27;
    previous->contextFrameStamp = 100;
    previous->nodes.resize(2);
    mailbox.Publish(oldTicket, previous);
    const auto retained = mailbox.Snapshot();
    ASSERT_TRUE(retained);

    mailbox.Invalidate();
    mailbox.Publish(oldTicket, previous);

    EXPECT_FALSE(mailbox.Snapshot());
    EXPECT_EQ(mailbox.Begin(27).revision, 0u);
    EXPECT_EQ(retained->nodes.size(), 2u);
    request.sceneGeneration = 28;
    ASSERT_TRUE(mailbox.SetRequest(request));
    EXPECT_EQ(mailbox.Begin(27).revision, 0u);
    mailbox.Publish(oldTicket, previous);
    EXPECT_FALSE(mailbox.Snapshot());
    const auto currentTicket = mailbox.Begin(28);
    auto current = std::make_shared<renderer::RenderBvhDebugSnapshot>();
    current->requestRevision = currentTicket.revision;
    current->sceneGeneration = 28;
    current->contextFrameStamp = 101;
    mailbox.Publish(currentTicket, current);
    ASSERT_TRUE(mailbox.Snapshot());
    EXPECT_EQ(mailbox.Snapshot()->sceneGeneration, 28u);
    EXPECT_EQ(retained->sceneGeneration, 27u);
}

TEST_F(RenderSceneSpatialQueriesTest, DebugMailboxRejectsOverBudgetOlderAndRetiredSelectionCaptures)
{
    BvhDebugPublication mailbox;
    renderer::RenderBvhDebugRequest request;
    request.enabled = true;
    request.sceneGeneration = 29;
    request.maxNodes = 1;
    ASSERT_TRUE(mailbox.SetRequest(request));
    const auto ticket = mailbox.Begin(29);
    auto overBudget = std::make_shared<renderer::RenderBvhDebugSnapshot>();
    overBudget->requestRevision = ticket.revision;
    overBudget->sceneGeneration = 29;
    overBudget->contextFrameStamp = 200;
    overBudget->nodes.resize(2);
    mailbox.Publish(ticket, overBudget);
    EXPECT_FALSE(mailbox.Snapshot());
    auto current = std::make_shared<renderer::RenderBvhDebugSnapshot>();
    current->requestRevision = ticket.revision;
    current->sceneGeneration = 29;
    current->contextFrameStamp = 201;
    current->nodes.resize(1);
    mailbox.Publish(ticket, current);
    ASSERT_TRUE(mailbox.Snapshot());

    overBudget->nodes.resize(1);
    mailbox.Publish(ticket, overBudget);
    EXPECT_EQ(mailbox.Snapshot()->contextFrameStamp, 201u);
    request.sourceIndex = 5;
    request.sourceGeneration = 3;
    ASSERT_TRUE(mailbox.SetRequest(request));
    mailbox.Publish(ticket, current);
    EXPECT_FALSE(mailbox.Snapshot());
    EXPECT_NE(mailbox.Begin(29).revision, ticket.revision);
    request.enabled = false;
    ASSERT_TRUE(mailbox.SetRequest(request));
    EXPECT_EQ(mailbox.Begin(29).revision, 0u);
    EXPECT_FALSE(mailbox.Snapshot());
}

TEST_F(RenderSceneSpatialQueriesTest, DebugMailboxAcceptsOnlyTheRequestedViewAndOutputGeneration)
{
    BvhDebugPublication mailbox;
    renderer::RenderBvhDebugRequest request;
    request.enabled = true;
    request.sceneGeneration = 30;
    request.viewOutputId = 17;
    request.viewOutputGeneration = 4;
    ASSERT_TRUE(mailbox.SetRequest(request));

    EXPECT_EQ(mailbox.Begin(30, 18, 4).revision, 0u);
    EXPECT_EQ(mailbox.Begin(30, 17, 3).revision, 0u);
    const auto ticket = mailbox.Begin(30, 17, 4);
    ASSERT_NE(ticket.revision, 0u);
    auto snapshot = std::make_shared<renderer::RenderBvhDebugSnapshot>();
    snapshot->requestRevision = ticket.revision;
    snapshot->sceneGeneration = 30;
    snapshot->viewOutputId = 18;
    snapshot->viewOutputGeneration = 4;
    mailbox.Publish(ticket, snapshot);
    EXPECT_FALSE(mailbox.Snapshot());
    snapshot->viewOutputId = 17;
    snapshot->viewOutputGeneration = 3;
    mailbox.Publish(ticket, snapshot);
    EXPECT_FALSE(mailbox.Snapshot());
    snapshot->viewOutputGeneration = 4;
    mailbox.Publish(ticket, snapshot);
    ASSERT_TRUE(mailbox.Snapshot());
    EXPECT_EQ(mailbox.Snapshot()->viewOutputId, 17u);
    EXPECT_EQ(mailbox.Snapshot()->viewOutputGeneration, 4u);
}

} /// @note namespace fbzz::tests
