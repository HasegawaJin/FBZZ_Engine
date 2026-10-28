/// @file    ManagedWorldFamilyServicesTests.cpp
/// @brief   COUNT境界のrow birthとTerrain/Water/VFX/Sequenceの値検証・実native操作を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/WaterInteraction.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Components/VFXComponent.hpp>
#include <Engine/Scene/Components/SequencePlayerComponent.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/SequenceAsset.hpp>
#include "../../../Engine/src/Scene/Managed/ManagedWorldFamilyServices.hpp"
#include "../../../Engine/src/Scene/Managed/ManagedServicesBridge.hpp"
#include "../../../Engine/src/Scene/Managed/ManagedObjectControls.hpp"
#include <array>
#include <bit>
#include <charconv>
#include <limits>
namespace fbzz::tests {
using Status = scene::ManagedBridgeStatus;
using Op = scene::ManagedBridgeOp;
using Kind = scene::ManagedNativeComponentKind;
using Controls = scene::managed_detail::ManagedObjectControls;
class ManagedWorldFamilyServicesTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp(); m_previousPlay = scene::Script::IsInPlayMode(); scene::Script::SetInPlayMode(true);
        m_object = &m_scene.CreateGameObject("World families"); m_view.SetContext(&m_scene, m_object);
    }
    void TearDown() override
    {
        m_view.SetContext(nullptr, nullptr); m_scene.Clear(); scene::Script::SetInPlayMode(m_previousPlay); EngineFixture::TearDown();
    }
    Status Run(Op op, uint32_t selector, bool read, int64_t integer = 0,
        std::span<const float> values = {}, std::span<const std::string> text = {}, bool fixed = false, scene::GameObject* related = nullptr)
    {
        scene::ManagedBridgeRequest request; request.op = op; request.index = selector; request.flags = read ? 1 : 0;
        return scene::managed_detail::RunWorldFamilyServices(m_scene, m_view, *m_object, related,
            request, integer, values, text, m_result, fixed);
    }
    static int64_t Grid(int32_t x, int32_t z)
    { return std::bit_cast<int64_t>((static_cast<uint64_t>(static_cast<uint32_t>(z)) << 32) | static_cast<uint32_t>(x)); }
    scene::Scene m_scene;
    scene::GameObject* m_object = nullptr;
    scene::Script m_view;
    scene::managed_detail::ManagedServiceResult m_result;
    bool m_previousPlay = false;
};
TEST_F(ManagedWorldFamilyServicesTest, PendingRemovalAndReaddUseExactBirthsBeyondOld32BitBoundary)
{
    for (const auto kind : std::array<Kind, 7>{Kind::VFX, Kind::SEQUENCE, Kind::TERRAIN, Kind::WATER,
        Kind::LINE_RENDERER, Kind::MOTION_WARP, Kind::FLOW_FIELD}) {
        bool exists = false; uint64_t first = 0, pending = 999, replacement = 0;
        const auto index = static_cast<uint32_t>(kind);
        ASSERT_EQ(Controls::Component(*m_object, index, Op::COMPONENT_ADD, exists, &first), Status::OK);
        ASSERT_TRUE(exists); ASSERT_NE(first, 0U);
        ASSERT_EQ(Controls::Component(*m_object, index, Op::COMPONENT_REMOVE, exists), Status::OK);
        EXPECT_TRUE(Controls::IsPendingComponentRemoval(*m_object, index));
        ASSERT_EQ(Controls::Component(*m_object, index, Op::COMPONENT_READ, exists, &pending), Status::OK);
        EXPECT_FALSE(exists); EXPECT_EQ(pending, 0U);
        EXPECT_EQ(Controls::Component(*m_object, index, Op::COMPONENT_ADD, exists), Status::INVALID_ARGUMENT);
        Controls::FlushScriptRemovals(m_scene);
        EXPECT_FALSE(Controls::IsPendingComponentRemoval(*m_object, index));
        ASSERT_EQ(Controls::Component(*m_object, index, Op::COMPONENT_ADD, exists, &replacement), Status::OK);
        EXPECT_TRUE(exists); EXPECT_NE(replacement, 0U); EXPECT_NE(replacement, first);
    }
    for (const uint32_t invalid : std::array<uint32_t, 3>{7, static_cast<uint32_t>(Kind::COUNT), UINT32_MAX}) {
        bool exists = true; uint64_t lifetime = 999;
        EXPECT_EQ(Controls::Component(*m_object, invalid, Op::COMPONENT_READ, exists, &lifetime), Status::UNSUPPORTED);
        EXPECT_FALSE(exists); EXPECT_EQ(lifetime, 0U);
        EXPECT_FALSE(Controls::IsPendingComponentRemoval(*m_object, invalid));
    }
}
TEST_F(ManagedWorldFamilyServicesTest, TerrainEditsRequestGeometryRebuildAndRejectWrongScopeWithoutChanges)
{
    auto& terrain = m_object->AddComponent<scene::TerrainComponent>();
    terrain.columns = terrain.rows = 3; terrain.maxHeight = 10; terrain.InitFlat();
    terrain.heightDirty = terrain.colliderDirty = false;
    const std::array<float, 1> height{4};
    EXPECT_EQ(Run(Op::SERVICE_TERRAIN, 9, false, Grid(1, 1), height), Status::INVALID_SCOPE);
    EXPECT_FALSE(terrain.heightDirty); EXPECT_NEAR(terrain.heightData[4], 0, testkit::kTolerance);
    ASSERT_EQ(Run(Op::SERVICE_TERRAIN, 9, false, Grid(1, 1), height, {}, true), Status::OK);
    EXPECT_EQ(m_result.integer, 1); EXPECT_TRUE(terrain.heightDirty); EXPECT_TRUE(terrain.colliderDirty);
    EXPECT_NEAR(terrain.heightData[4], 0.4f, testkit::kTolerance);
    terrain.heightDirty = terrain.colliderDirty = false;
    const std::array<float, 1> hole{1};
    ASSERT_EQ(Run(Op::SERVICE_TERRAIN, 14, false, Grid(0, 0), hole, {}, true), Status::OK);
    EXPECT_TRUE(terrain.IsHoleCell(0, 0)); EXPECT_TRUE(terrain.heightDirty); EXPECT_TRUE(terrain.colliderDirty);
    terrain.heightDirty = terrain.colliderDirty = false;
    ASSERT_EQ(Run(Op::SERVICE_TERRAIN, 14, false, Grid(0, 0), hole, {}, true), Status::OK);
    EXPECT_FALSE(terrain.heightDirty); EXPECT_FALSE(terrain.colliderDirty);
    const auto original = terrain.heightData;
    const std::array<float, 1> invalid{std::numeric_limits<float>::quiet_NaN()};
    EXPECT_EQ(Run(Op::SERVICE_TERRAIN, 9, false, Grid(1, 1), invalid, {}, true), Status::INVALID_ARGUMENT);
    EXPECT_EQ(terrain.heightData, original);
    EXPECT_EQ(Run(Op::SERVICE_TERRAIN, 9, true, Grid(1, 1), height, {}, true), Status::INVALID_ARGUMENT);
}
TEST_F(ManagedWorldFamilyServicesTest, CaveDirectionalQueriesTransferFloorCeilingMissAndRejectMalformedPackets)
{
    auto& terrain = m_object->AddComponent<scene::TerrainComponent>();
    terrain.columns = terrain.rows = 2; terrain.cellSize = 2; terrain.InitFlat();
    ASSERT_TRUE(terrain.SetHoleCell(0, 0, true));
    ASSERT_TRUE(terrain.SetSurfaceGeometry(
        {{0,-6,0},{0,-6,2},{2,-6,0},{2,-6,2},{0,-2,0},{2,-2,0},{0,-2,2},{2,-2,2}},
        {0,1,2,2,1,3,4,5,6,6,5,7}));
    const std::array<float, 4> reference{.5f,-4,.5f,10};
    scene::Script::SetInPlayMode(false);
    ASSERT_EQ(Run(Op::SERVICE_TERRAIN, 16, true, 0, reference), Status::OK);
    ASSERT_EQ(m_result.values.size(), 6U); EXPECT_EQ(m_result.integer, 1);
    EXPECT_FLOAT_EQ(m_result.values[1], -6); EXPECT_GT(m_result.values[4], .99f);
    ASSERT_EQ(Run(Op::SERVICE_TERRAIN, 17, true, 0, reference), Status::OK);
    ASSERT_EQ(m_result.values.size(), 6U); EXPECT_EQ(m_result.integer, 1);
    EXPECT_FLOAT_EQ(m_result.values[1], -2); EXPECT_LT(m_result.values[4], -.99f);
    auto shortRange = reference; shortRange[3] = 1;
    ASSERT_EQ(Run(Op::SERVICE_TERRAIN, 16, true, 0, shortRange), Status::OK);
    EXPECT_EQ(m_result.integer, 0); EXPECT_TRUE(m_result.values.empty());
    m_result.integer = 77; m_result.values = {88};
    auto invalid = reference; invalid[3] = -1;
    EXPECT_EQ(Run(Op::SERVICE_TERRAIN, 16, true, 0, invalid), Status::INVALID_ARGUMENT);
    invalid = reference; invalid[1] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(Run(Op::SERVICE_TERRAIN, 17, true, 0, invalid), Status::INVALID_ARGUMENT);
    EXPECT_EQ(Run(Op::SERVICE_TERRAIN, 16, false, 0, reference), Status::INVALID_ARGUMENT);
    EXPECT_EQ(Run(Op::SERVICE_TERRAIN, 16, true, 1, reference), Status::INVALID_ARGUMENT);
    EXPECT_EQ(m_result.integer, 77); ASSERT_EQ(m_result.values.size(), 1U); EXPECT_FLOAT_EQ(m_result.values[0], 88);
    bool exists = false;
    ASSERT_EQ(Controls::Component(*m_object, static_cast<uint32_t>(Kind::TERRAIN), Op::COMPONENT_REMOVE, exists), Status::OK);
    EXPECT_EQ(Run(Op::SERVICE_TERRAIN, 16, true, 0, reference), Status::NOT_FOUND);
}
TEST_F(ManagedWorldFamilyServicesTest, WaterQueriesReuseCpuWaveEvaluationAndPlaybackMutationRequiresPlay)
{
    auto& water = m_object->AddComponent<scene::WaterComponent>();
    water.enableGerstnerWaves = false; water.current = {2, -3};
    m_object->transform.worldPosition = {10, 7, 20};
    const std::array<float, 3> sample{11, 22, 0.5f};
    ASSERT_EQ(Run(Op::SERVICE_WATER, 8, true), Status::OK);
    ASSERT_EQ(m_result.values.size(), 2U);
    EXPECT_NEAR(m_result.values[0], 2, testkit::kTolerance);
    EXPECT_NEAR(m_result.values[1], -3, testkit::kTolerance);
    ASSERT_EQ(Run(Op::SERVICE_WATER, 9, true, 0, sample), Status::OK);
    ASSERT_EQ(m_result.values.size(), 1U); EXPECT_NEAR(m_result.values[0], 7, testkit::kTolerance);
    const std::array<float, 4> ripple{11, 7, 22, 0.5f};
    scene::Script::SetInPlayMode(false);
    EXPECT_EQ(Run(Op::SERVICE_WATER, 11, false, 0, ripple), Status::INVALID_SCOPE);
    EXPECT_TRUE(water.ripples.empty());
    scene::Script::SetInPlayMode(true);
    ASSERT_EQ(Run(Op::SERVICE_WATER, 11, false, 0, ripple), Status::OK);
    EXPECT_EQ(water.ripples.size(), 1U);
}
TEST_F(ManagedWorldFamilyServicesTest, WaterInteractionSelectorQueuesOnlyValidatedPlayValuesWithoutStructuralEdits)
{
    auto& water = m_object->AddComponent<scene::WaterComponent>();
    water.enableGerstnerWaves = false;
    water.buoyancyDepth = 10;
    scene::FlushWorldTransforms(m_scene);
    scene::PrepareWaterSceneSurfaces(m_scene);
    const std::array<float, 9> bubble{0, -1, 0, 0, 0.3f, 0, 0.2f, 0.5f, 2};
    const auto count = m_scene.GameObjectCount();
    scene::Script::SetInPlayMode(false);
    EXPECT_EQ(Run(Op::SERVICE_WATER, 13, false, 1, bubble), Status::INVALID_SCOPE);
    EXPECT_TRUE(water.interactionRuntime.pending.empty());
    scene::Script::SetInPlayMode(true);
    ASSERT_EQ(Run(Op::SERVICE_WATER, 13, false, 1, bubble), Status::OK);
    EXPECT_EQ(m_result.integer, 1);
    EXPECT_EQ(m_scene.GameObjectCount(), count);
    ASSERT_EQ(water.interactionRuntime.pending.size(), 1u);
    EXPECT_EQ(water.interactionRuntime.pending.front().kind, scene::WaterInteractionKind::BUBBLES);
    auto invalid = bubble;
    invalid[6] = -1;
    EXPECT_EQ(Run(Op::SERVICE_WATER, 13, false, 1, invalid), Status::INVALID_ARGUMENT);
    EXPECT_EQ(Run(Op::SERVICE_WATER, 13, false, 1000000, bubble), Status::INVALID_ARGUMENT);
    EXPECT_EQ(water.interactionRuntime.pending.size(), 1u);
}
TEST_F(ManagedWorldFamilyServicesTest, ExplicitWaterInteractionReturnsFalseForExcludedOrDisabledLocations)
{
    auto& water = m_object->AddComponent<scene::WaterComponent>();
    water.enableGerstnerWaves = false;
    scene::FlushWorldTransforms(m_scene);
    scene::PrepareWaterSceneSurfaces(m_scene);
    const std::array<float, 9> outside{10000, -1, 0, 0, 0, 0, 0.2f, 0.5f, 2};
    ASSERT_EQ(Run(Op::SERVICE_WATER, 13, false, 2, outside), Status::OK);
    EXPECT_EQ(m_result.integer, 0);
    water.interactionSettings.enabled = false;
    const std::array<float, 9> inside{0, -1, 0, 0, 0, 0, 0.2f, 0.5f, 2};
    ASSERT_EQ(Run(Op::SERVICE_WATER, 13, false, 2, inside), Status::OK);
    EXPECT_EQ(m_result.integer, 0);
    EXPECT_TRUE(water.interactionRuntime.pending.empty());
}
TEST_F(ManagedWorldFamilyServicesTest, VfxCommandsKeepActualPlaybackAndTriggerSemantics)
{
    auto& vfx = m_object->AddComponent<scene::VFXComponent>();
    vfx.time = 2; vfx.resolvedDuration = 4;
    ASSERT_EQ(Run(Op::SERVICE_VFX, 7, false, 1), Status::OK);
    EXPECT_TRUE(vfx.playing); EXPECT_TRUE(vfx.restartRequested); EXPECT_NEAR(vfx.time, 0, testkit::kTolerance);
    const std::array<std::string, 1> trigger{"Hit"};
    ASSERT_EQ(Run(Op::SERVICE_VFX, 11, false, 0, {}, trigger), Status::OK);
    ASSERT_EQ(vfx.pendingTriggers.size(), 1U); EXPECT_EQ(vfx.pendingTriggers[0], "Hit");
    ASSERT_EQ(Run(Op::SERVICE_VFX, 6, true), Status::OK);
    EXPECT_NEAR(m_result.values[0], 4, testkit::kTolerance);
    EXPECT_EQ(Run(Op::SERVICE_VFX, 10, true), Status::INVALID_ARGUMENT); EXPECT_TRUE(vfx.playing);
    ASSERT_EQ(Run(Op::SERVICE_VFX, 10, false), Status::OK); EXPECT_FALSE(vfx.playing);
}
TEST_F(ManagedWorldFamilyServicesTest, SequenceQueuesLifecycleAndPreservesDoublePrecisionAndExactBinding)
{
    auto& sequence = m_object->AddComponent<scene::SequencePlayerComponent>();
    auto& bound = m_scene.CreateGameObject("Bound");
    const std::array<std::string, 1> key{"Actor"}, seconds{"1000000000000.125"};
    ASSERT_EQ(Run(Op::SERVICE_SEQUENCE, 13, false, 0, {}, key, false, &bound), Status::OK);
    ASSERT_EQ(sequence.bindings.size(), 1U); EXPECT_EQ(sequence.bindings[0].target.id, bound.GetID());
    ASSERT_EQ(Run(Op::SERVICE_SEQUENCE, 8, false), Status::OK); EXPECT_TRUE(sequence.pendingPlay); EXPECT_FALSE(sequence.playing);
    ASSERT_EQ(Run(Op::SERVICE_SEQUENCE, 12, false, 0, {}, seconds), Status::OK);
    EXPECT_EQ(sequence.pendingSeek, 1000000000000.125);
    sequence.time = sequence.pendingSeek;
    ASSERT_EQ(Run(Op::SERVICE_SEQUENCE, 6, true), Status::OK);
    ASSERT_EQ(m_result.text.size(), 1U); double value = 0;
    const auto parsed = std::from_chars(m_result.text[0].data(), m_result.text[0].data() + m_result.text[0].size(), value);
    ASSERT_EQ(parsed.ec, std::errc{}); EXPECT_EQ(value, 1000000000000.125);
    ASSERT_EQ(Run(Op::SERVICE_SEQUENCE, 9, false), Status::OK); EXPECT_TRUE(sequence.pendingStop); EXPECT_FALSE(sequence.pendingPlay);
    ASSERT_EQ(Run(Op::SERVICE_SEQUENCE, 13, false, 0, {}, key), Status::OK); EXPECT_FALSE(sequence.bindings[0].target.IsValid());
    sequence.sequencePath = "Assets/NotImported.sequence";
    const auto before = asset::AssetStore<asset::SequenceAsset>::Get().slots.size();
    EXPECT_EQ(Run(Op::SERVICE_SEQUENCE, 7, true), Status::NOT_FOUND);
    EXPECT_EQ(asset::AssetStore<asset::SequenceAsset>::Get().slots.size(), before);
}
} /// @note namespace fbzz::tests
