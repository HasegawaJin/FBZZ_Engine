/// @file    SceneRendererIndexTests.cpp
/// @brief   Renderer 候補の所有順、所属変更と Scene 世代・再入を公開境界から検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-16
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Core/Profiler/Profiler.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {

using RendererSet = scene::Scene::RendererEntitySet;

class RendererDestroyObserver final : public scene::Script {
public:
    explicit RendererDestroyObserver(std::function<void()> callback) : m_callback(std::move(callback)) {}
    void OnDestroy() override { m_callback(); }
private:
    std::function<void()> m_callback;
};

struct RestoreRendererIndexProfiler {
    bool m_enabled = profiler::Profiler::IsEnabled();
    profiler::PerformanceCaptureFilter m_filter = profiler::Profiler::GetRequestedCaptureFilter();
    ~RestoreRendererIndexProfiler()
    {
        profiler::Profiler::SetEnabled(m_enabled);
        profiler::Profiler::RequestCaptureFilter(std::move(m_filter));
    }
};

} /// @note namespace

class SceneRendererIndexTest : public testkit::EngineFixture {
protected:
    scene::Scene m_scene;

    std::vector<scene::EntityID> Ids(scene::Scene& scene, RendererSet set = RendererSet::GEOMETRY)
    {
        std::vector<scene::EntityID> ids;
        scene.CopyRendererEntities(ids, set);
        return ids;
    }

    void ExpectCurrent(scene::Scene& scene)
    {
        for (const auto set : {RendererSet::GEOMETRY, RendererSet::SKINNED, RendererSet::GEOMETRY_AND_TERRAIN}) {
            std::vector<scene::EntityID> expected;
            for (auto& object : scene.GameObjects()) {
                const bool included = scene.HasComponent<scene::SkinnedMeshRenderer>(object.GetID())
                    || (set != RendererSet::SKINNED && scene.HasComponent<scene::MeshRenderer>(object.GetID()))
                    || (set == RendererSet::GEOMETRY_AND_TERRAIN && scene.HasComponent<scene::TerrainComponent>(object.GetID()));
                if (included) expected.push_back(object.GetID());
            }
            EXPECT_EQ(Ids(scene, set), expected);
        }
    }

    size_t Rebuilds(const std::function<void()>& action)
    {
        profiler::Profiler::BeginFrame();
        action();
        profiler::Profiler::EndFrame();
        const auto& records = profiler::Profiler::GetLastFrameRecords();
        return static_cast<size_t>(std::count_if(records.begin(), records.end(), [](const auto& record) {
            return std::string_view(record.name) == "Scene::RendererIndexRebuild";
        }));
    }

    std::array<size_t, 3> Paths(const std::function<void()>& action)
    {
        profiler::Profiler::BeginFrame();
        action();
        profiler::Profiler::EndFrame();
        std::array<size_t, 3> counts{};
        for (const auto& record : profiler::Profiler::GetLastFrameRecords()) {
            const std::string_view name = record.name;
            if (name == "Scene::RendererIndexLegacy") ++counts[0];
            else if (name == "Scene::RendererIndexRevisionRefresh") ++counts[1];
            else if (name == "Scene::RendererIndexRevisionReuse") ++counts[2];
        }
        return counts;
    }
};

TEST_F(SceneRendererIndexTest, LateRegistrationDeduplicatesKindsAndIncludesDisabledInactiveAndEmptyRenderers)
{
    EXPECT_TRUE(Ids(m_scene).empty());
    auto& first = m_scene.CreateGameObject("First");
    for (int index = 0; index < 128; ++index) m_scene.CreateGameObject("Bone");
    auto& mixed = m_scene.CreateGameObject("Mixed");
    mixed.AddComponent<scene::SkinnedMeshRenderer>();
    mixed.AddComponent<scene::MeshRenderer>().enabled = false;
    mixed.AddComponent<scene::TerrainComponent>();
    mixed.SetActive(false);
    auto& terrain = m_scene.CreateGameObject("Terrain");
    terrain.AddComponent<scene::TerrainComponent>();
    first.AddComponent<scene::MeshRenderer>();
    EXPECT_EQ(Ids(m_scene), (std::vector<scene::EntityID>{first.GetID(), mixed.GetID()}));
    EXPECT_EQ(Ids(m_scene, RendererSet::SKINNED), (std::vector<scene::EntityID>{mixed.GetID()}));
    EXPECT_EQ(Ids(m_scene, RendererSet::GEOMETRY_AND_TERRAIN),
        (std::vector<scene::EntityID>{first.GetID(), mixed.GetID(), terrain.GetID()}));
    ExpectCurrent(m_scene);
}

TEST_F(SceneRendererIndexTest, MembershipCopiesDenseRemovalAndGenerationReuseInvalidateEveryDependentSet)
{
    auto& source = m_scene.CreateGameObject("Source");
    source.AddComponent<scene::MeshRenderer>();
    source.AddComponent<scene::SkinnedMeshRenderer>();
    source.AddComponent<scene::TerrainComponent>();
    auto& duplicate = m_scene.CreateGameObject("Duplicate");
    auto& passive = m_scene.CreateGameObject("Passive");
    ExpectCurrent(m_scene);
    m_scene.DuplicateComponents(source.GetID(), duplicate.GetID());
    ExpectCurrent(m_scene);
    m_scene.DuplicateComponentsPassive(source.GetID(), passive.GetID());
    ExpectCurrent(m_scene);
    scene::Scene other;
    auto& external = other.CreateGameObject("External");
    external.AddComponent<scene::MeshRenderer>();
    auto& copied = m_scene.CreateGameObject("Copied");
    m_scene.CopyComponentsFrom(other, external.GetID(), copied.GetID());
    auto& copiedPassive = m_scene.CreateGameObject("CopiedPassive");
    m_scene.CopyComponentsFromPassive(other, external.GetID(), copiedPassive.GetID());
    ExpectCurrent(m_scene);
    const auto saved = Ids(m_scene);
    const auto retired = source.GetID();
    source.RemoveComponent<scene::MeshRenderer>();
    EXPECT_EQ(Ids(m_scene).front(), retired);
    source.RemoveComponent<scene::SkinnedMeshRenderer>();
    ExpectCurrent(m_scene);
    scene::GameObject::Destroy(source);
    EXPECT_NE(m_scene.GetGameObject(retired), nullptr);
    ExpectCurrent(m_scene);
    m_scene.FlushDestroyQueue(0.0f);
    auto& born = m_scene.CreateGameObject("Born");
    EXPECT_EQ(born.GetID().index, retired.index);
    EXPECT_NE(born.GetID().generation, retired.generation);
    born.AddComponent<scene::SkinnedMeshRenderer>();
    ExpectCurrent(m_scene);
    EXPECT_EQ(saved.front(), retired);
    EXPECT_NE(Ids(m_scene).back(), retired);
}

TEST_F(SceneRendererIndexTest, OwnershipMovesAndSiblingReordersAreVisibleWithoutMembershipChanges)
{
    auto& root = m_scene.CreateGameObject("Root");
    auto& first = m_scene.CreateGameObject("First");
    auto& second = m_scene.CreateGameObject("Second");
    auto& last = m_scene.CreateGameObject("Last");
    first.AddComponent<scene::MeshRenderer>();
    second.AddComponent<scene::SkinnedMeshRenderer>();
    last.AddComponent<scene::TerrainComponent>();
    ASSERT_TRUE(first.SetParent(&root));
    ASSERT_TRUE(second.SetParent(&root));
    ExpectCurrent(m_scene);
    ASSERT_TRUE(second.SetSiblingIndex(0));
    ExpectCurrent(m_scene);
    ASSERT_TRUE(m_scene.MoveGameObjectToIndex(last.GetID(), 0));
    ExpectCurrent(m_scene);
    ASSERT_TRUE(m_scene.MoveGameObject(first.GetID(), -1));
    ExpectCurrent(m_scene);
    ASSERT_TRUE(m_scene.SetRootSiblingIndex(last.GetID(), 1));
    ExpectCurrent(m_scene);
}

TEST_F(SceneRendererIndexTest, ClearMoveAssignmentAndSidecarEvictionNeverReuseAnotherScenesIds)
{
    auto& original = m_scene.CreateGameObject("Original");
    original.AddComponent<scene::MeshRenderer>();
    const auto saved = Ids(m_scene);
    const auto generation = m_scene.GetRenderSceneGeneration();
    m_scene.Clear();
    auto& replacement = m_scene.CreateGameObject("Replacement");
    EXPECT_EQ(replacement.GetID(), saved.front());
    replacement.AddComponent<scene::TerrainComponent>();
    EXPECT_NE(m_scene.GetRenderSceneGeneration(), generation);
    ExpectCurrent(m_scene);
    scene::Scene moved(std::move(m_scene));
    ExpectCurrent(m_scene);
    ExpectCurrent(moved);
    m_scene = std::move(moved);
    ExpectCurrent(moved);
    ExpectCurrent(m_scene);
    std::vector<std::unique_ptr<scene::Scene>> scenes;
    for (int index = 0; index < 8; ++index) {
        auto& scene = *scenes.emplace_back(std::make_unique<scene::Scene>());
        auto& object = scene.CreateGameObject("Other");
        if (index % 2 == 0) object.AddComponent<scene::MeshRenderer>();
        else object.AddComponent<scene::SkinnedMeshRenderer>();
        ExpectCurrent(scene);
    }
    for (auto& scene : scenes) ExpectCurrent(*scene);
    ExpectCurrent(m_scene);
    EXPECT_EQ(saved.size(), 1u);
}

TEST_F(SceneRendererIndexTest, DestroyCallbackObservesSparseOwnersAndCanMutateAndQueryReentrantly)
{
    auto& first = m_scene.CreateGameObject("First");
    auto& second = m_scene.CreateGameObject("Second");
    auto& third = m_scene.CreateGameObject("Third");
    for (auto* object : {&first, &second, &third}) object->AddComponent<scene::MeshRenderer>();
    const auto saved = Ids(m_scene);
    const auto firstId = first.GetID();
    const auto secondId = second.GetID();
    const auto thirdId = third.GetID();
    int calls = 0;
    auto& entry = second.AddComponent<scene::ScriptComponent>().scripts.emplace_back();
    entry.script = std::make_unique<RendererDestroyObserver>([&] {
        ++calls;
        EXPECT_EQ(Ids(m_scene), (std::vector<scene::EntityID>{secondId, thirdId}));
        auto& born = m_scene.CreateGameObject("Born");
        EXPECT_EQ(born.GetID().index, firstId.index);
        born.AddComponent<scene::SkinnedMeshRenderer>();
        ASSERT_TRUE(m_scene.MoveGameObjectToIndex(born.GetID(), 0));
        ExpectCurrent(m_scene);
        ASSERT_TRUE(m_scene.DestroyGameObject(thirdId));
        ExpectCurrent(m_scene);
    });
    entry.script->SetContext(&m_scene, &second);
    entry.m_awoken = true;
    entry.m_started = true;
    scene::GameObject::Destroy(first);
    scene::GameObject::Destroy(second);
    m_scene.FlushDestroyQueue(0.0f);
    EXPECT_EQ(calls, 1);
    ExpectCurrent(m_scene);
    EXPECT_EQ(saved, (std::vector<scene::EntityID>{firstId, secondId, thirdId}));
}

TEST_F(SceneRendererIndexTest, ProfileRebuildOccursOnlyWhenTheRequestedMembershipOrOrderChanges)
{
    RestoreRendererIndexProfiler restore;
    profiler::Profiler::SetEnabled(false);
    ASSERT_TRUE(profiler::Profiler::RequestCaptureFilter({}));
    profiler::Profiler::SetEnabled(true);
    auto& first = m_scene.CreateGameObject("First");
    auto& second = m_scene.CreateGameObject("Second");
    first.AddComponent<scene::MeshRenderer>();
    second.AddComponent<scene::SkinnedMeshRenderer>();
    EXPECT_EQ(Rebuilds([&] { Ids(m_scene); Ids(m_scene); }), 1u);
    first.SetActive(false);
    first.GetComponent<scene::MeshRenderer>()->enabled = false;
    first.transform.worldPosition = {1, 2, 3};
    EXPECT_EQ(Rebuilds([&] { Ids(m_scene); }), 0u);
    ASSERT_TRUE(m_scene.MoveGameObjectToIndex(second.GetID(), 0));
    EXPECT_EQ(Rebuilds([&] { Ids(m_scene); Ids(m_scene); }), 1u);
    second.RemoveComponent<scene::SkinnedMeshRenderer>();
    EXPECT_EQ(Rebuilds([&] { Ids(m_scene); Ids(m_scene); }), 1u);
}

TEST_F(SceneRendererIndexTest, ProfileRecordsOneSelectedPathAndSkipsUnchangedStructureAfterNonRendererAppend)
{
    RestoreRendererIndexProfiler restore;
    profiler::Profiler::SetEnabled(false);
    ASSERT_TRUE(profiler::Profiler::RequestCaptureFilter({}));
    profiler::Profiler::SetEnabled(true);
    m_scene.CreateGameObject("Renderer").AddComponent<scene::MeshRenderer>();

    const auto paths = Paths([&] {
        Ids(m_scene);
        Ids(m_scene);
        m_scene.CreateGameObject("Bone");
        Ids(m_scene);
    });

    EXPECT_EQ(paths[0] + paths[1] + paths[2], 3u);
    if (paths[0] != 0) EXPECT_EQ(paths, (std::array<size_t, 3>{3, 0, 0}));
    else EXPECT_EQ(paths, (std::array<size_t, 3>{0, 1, 2}));
    ExpectCurrent(m_scene);
}

TEST_F(SceneRendererIndexTest, NonRendererCompactionRefreshesSkinnedOwnersBeforeOtherSetsAreQueried)
{
    RestoreRendererIndexProfiler restore;
    profiler::Profiler::SetEnabled(false);
    ASSERT_TRUE(profiler::Profiler::RequestCaptureFilter({}));
    profiler::Profiler::SetEnabled(true);
    const auto bone = m_scene.CreateGameObject("Bone").GetID();
    auto& mesh = m_scene.CreateGameObject("Mesh");
    mesh.AddComponent<scene::MeshRenderer>();
    auto& skinned = m_scene.CreateGameObject("Skinned");
    skinned.AddComponent<scene::SkinnedMeshRenderer>();
    const auto saved = Ids(m_scene, RendererSet::SKINNED);

    ASSERT_TRUE(m_scene.DestroyGameObject(bone));
    const auto paths = Paths([&] {
        EXPECT_EQ(Ids(m_scene, RendererSet::SKINNED), saved);
        EXPECT_EQ(Ids(m_scene, RendererSet::SKINNED), saved);
    });

    if (paths[0] != 0) EXPECT_EQ(paths, (std::array<size_t, 3>{2, 0, 0}));
    else EXPECT_EQ(paths, (std::array<size_t, 3>{0, 1, 1}));
    ASSERT_TRUE(m_scene.MoveGameObjectToIndex(skinned.GetID(), 0));
    ExpectCurrent(m_scene);
}

TEST_F(SceneRendererIndexTest, DeferredQueriesAndEqualCountsKeepEveryRendererSetCurrent)
{
    auto& first = m_scene.CreateGameObject("First");
    auto& second = m_scene.CreateGameObject("Second");
    auto& third = m_scene.CreateGameObject("Third");
    first.AddComponent<scene::MeshRenderer>();
    first.AddComponent<scene::SkinnedMeshRenderer>();
    second.AddComponent<scene::TerrainComponent>();
    third.AddComponent<scene::MeshRenderer>();
    ExpectCurrent(m_scene);

    first.RemoveComponent<scene::SkinnedMeshRenderer>();
    third.AddComponent<scene::SkinnedMeshRenderer>();
    EXPECT_EQ(Ids(m_scene, RendererSet::SKINNED), (std::vector<scene::EntityID>{third.GetID()}));
    second.RemoveComponent<scene::TerrainComponent>();
    first.AddComponent<scene::TerrainComponent>();
    EXPECT_EQ(Ids(m_scene), (std::vector<scene::EntityID>{first.GetID(), third.GetID()}));
    ExpectCurrent(m_scene);

    const auto saved = Ids(m_scene, RendererSet::GEOMETRY_AND_TERRAIN);
    first.RemoveComponent<scene::MeshRenderer>();
    first.AddComponent<scene::MeshRenderer>();
    EXPECT_EQ(Ids(m_scene, RendererSet::GEOMETRY_AND_TERRAIN), saved);
    ExpectCurrent(m_scene);
}

TEST_F(SceneRendererIndexTest, SerializedSnapshotRestorationRetiresPrimedMembershipAndOwnerOrder)
{
    auto& first = m_scene.CreateGameObject("First");
    auto& second = m_scene.CreateGameObject("Second");
    first.AddComponent<scene::MeshRenderer>();
    second.AddComponent<scene::SkinnedMeshRenderer>();
    ASSERT_TRUE(second.SetParent(&first));
    ExpectCurrent(m_scene);
    const auto snapshot = scene::SceneSerializer::SaveToText(m_scene);
    ASSERT_FALSE(snapshot.empty());

    first.RemoveComponent<scene::MeshRenderer>();
    second.RemoveComponent<scene::SkinnedMeshRenderer>();
    second.AddComponent<scene::MeshRenderer>();
    ASSERT_TRUE(m_scene.MoveGameObjectToIndex(second.GetID(), 0));
    ExpectCurrent(m_scene);
    auto restored = scene::SceneSerializer::LoadDataFromText(snapshot, {});
    ASSERT_NE(restored, nullptr);
    m_scene = std::move(*restored);

    ExpectCurrent(m_scene);
    ASSERT_NE(m_scene.Find("First"), nullptr);
    ASSERT_NE(m_scene.Find("Second"), nullptr);
    EXPECT_EQ(Ids(m_scene), (std::vector<scene::EntityID>{m_scene.Find("First")->GetID(), m_scene.Find("Second")->GetID()}));
    EXPECT_EQ(Ids(m_scene, RendererSet::SKINNED), (std::vector<scene::EntityID>{m_scene.Find("Second")->GetID()}));
}

} /// @note namespace fbzz::tests
