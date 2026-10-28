/// @file    PrefabStructureMergeTests.cpp
/// @brief   Structural Prefab merge, conflict rejection, persistence and checked Undo regression cases.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Util/PrefabSerializer.hpp>
#include <Editor/Util/PrefabStructureMerge.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Scene/Components/CameraRigComponents.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/ScriptFactory.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <sstream>

namespace fbzz::tests {
namespace {
toml::table Object(const char* id, const char* parent = "", double value = 1.0)
{
    return toml::table{{"instanceId", id}, {"name", id}, {"parentInstanceId", parent},
        {"parent", ""}, {"Settings", toml::table{{"value", value}}}};
}

toml::table Document(std::initializer_list<toml::table> objects)
{
    toml::array array;
    for (const auto& object : objects) array.push_back(object);
    return toml::table{{"gameobjects", std::move(array)}};
}

const toml::table* FindObject(const toml::table& document, const std::string& id)
{
    const auto* objects = document["gameobjects"].as_array();
    if (!objects) return nullptr;
    for (const auto& object : *objects)
        if (const auto* table = object.as_table(); table && (*table)["instanceId"].value_or(std::string{}) == id) return table;
    return nullptr;
}

std::string Snapshot(scene::Scene& scene)
{
    std::string reason;
    return scene::SceneSerializer::CapturePassiveClone(scene, reason);
}
} /// @note namespace

TEST(PrefabStructureMerge, TemplateAddRemoveReparentAndLocalOverridesMergeTogether)
{
    const auto before = Document({Object("root"), Object("a", "root"), Object("b", "root"), Object("c", "root")});
    const auto local = Document({Object("root"), Object("a", "root", 9.0), Object("b", "root"), Object("c", "root"), Object("local", "a")});
    const auto incoming = Document({Object("root"), Object("a", "root", 3.0), Object("b", "a"), Object("new", "root")});
    const auto result = editor::MergePrefabStructure(before, local, incoming);
    ASSERT_TRUE(result.CanApply());
    ASSERT_NE(FindObject(result.document, "a"), nullptr);
    EXPECT_DOUBLE_EQ((*FindObject(result.document, "a"))["Settings"]["value"].value_or(0.0), 9.0);
    EXPECT_EQ((*FindObject(result.document, "b"))["parentInstanceId"].value_or(std::string{}), "a");
    EXPECT_EQ(FindObject(result.document, "c"), nullptr);
    EXPECT_NE(FindObject(result.document, "new"), nullptr);
    EXPECT_NE(FindObject(result.document, "local"), nullptr);
}

TEST(PrefabStructureMerge, CompatibleLocalDeletionAndReparentArePreserved)
{
    const auto before = Document({Object("root"), Object("a", "root"), Object("b", "root"), Object("c", "root")});
    const auto local = Document({Object("root"), Object("a", "root"), Object("b", "a")});
    const auto incoming = Document({Object("root", "", 2.0), Object("a", "root"), Object("b", "root"), Object("c", "root")});
    const auto result = editor::MergePrefabStructure(before, local, incoming);
    ASSERT_TRUE(result.CanApply());
    EXPECT_EQ(FindObject(result.document, "c"), nullptr);
    EXPECT_EQ((*FindObject(result.document, "b"))["parentInstanceId"].value_or(std::string{}), "a");
}

TEST(PrefabStructureMerge, DeleteVersusLocalEditAndLocalDeleteVersusTemplateEditConflict)
{
    const auto before = Document({Object("root"), Object("a", "root")});
    const auto edited = Document({Object("root"), Object("a", "root", 4.0)});
    const auto deleted = Document({Object("root")});
    const auto first = editor::MergePrefabStructure(before, edited, deleted);
    const auto second = editor::MergePrefabStructure(before, deleted, edited);
    EXPECT_FALSE(first.CanApply());
    EXPECT_FALSE(second.CanApply());
    ASSERT_FALSE(first.conflicts.empty());
    EXPECT_EQ(first.conflicts.front().sourceId, "a");
}

TEST(PrefabStructureMerge, DivergentReparentsAndCombinedCyclesConflict)
{
    const auto before = Document({Object("root"), Object("a", "root"), Object("b", "root"), Object("c", "root")});
    const auto local = Document({Object("root"), Object("a", "b"), Object("b", "root"), Object("c", "root")});
    const auto different = Document({Object("root"), Object("a", "c"), Object("b", "root"), Object("c", "root")});
    const auto cycle = Document({Object("root"), Object("a", "root"), Object("b", "a"), Object("c", "root")});
    EXPECT_FALSE(editor::MergePrefabStructure(before, local, different).CanApply());
    EXPECT_FALSE(editor::MergePrefabStructure(before, local, cycle).CanApply());
}

TEST(PrefabStructureMerge, LocalChildUnderDeletedParentIsNotSilentlyDeletedOrReparented)
{
    const auto before = Document({Object("root"), Object("a", "root")});
    const auto local = Document({Object("root"), Object("a", "root"), Object("local", "a")});
    const auto result = editor::MergePrefabStructure(before, local, Document({Object("root")}));
    EXPECT_FALSE(result.CanApply());
    EXPECT_FALSE(result.conflicts.empty());
}

TEST(PrefabStructureMerge, InvalidIdentityMissingParentNestedBoundaryAndRemovedReferenceConflict)
{
    const auto before = Document({Object("root"), Object("a", "root")});
    EXPECT_FALSE(editor::MergePrefabStructure(before, before, Document({Object("root"), Object("root")})).CanApply());
    EXPECT_FALSE(editor::MergePrefabStructure(before, before, Document({Object("root"), Object("a", "missing")})).CanApply());
    auto noId = Object("");
    EXPECT_FALSE(editor::MergePrefabStructure(before, before, Document({noId})).CanApply());
    auto nested = Object("a", "root");
    nested.insert("prefabAssetPath", "Nested.prefab");
    EXPECT_FALSE(editor::MergePrefabStructure(before, before, Document({Object("root"), nested})).CanApply());
    auto referenceRoot = Object("root");
    referenceRoot.insert("Link", toml::table{{"target", "a"}});
    const auto linked = Document({referenceRoot, Object("a", "root")});
    EXPECT_FALSE(editor::MergePrefabStructure(linked, linked, Document({referenceRoot})).CanApply());
}

class PrefabStructureMergeTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        asset::AssetDatabase::Init(File("Assets").generic_string());
    }
    void TearDown() override
    {
        asset::AssetDatabase::Shutdown();
        EditorFixture::TearDown();
    }
    std::string Path() const { return File("Assets/Tiny.prefab").generic_string(); }
    bool Save(scene::Scene& source, scene::EntityID root)
    {
        return editor::PrefabSerializer::SaveSelection(source, {root}, Path());
    }
    scene::GameObject* SourceObject(scene::Scene& scene, const std::string& sourceId)
    {
        for (auto& object : scene.GameObjects()) if (object.prefabSourceId == sourceId) return &object;
        return nullptr;
    }
};

TEST_F(PrefabStructureMergeTest, NoOpRetainsSiblingOrderAndIncomingAdditionsAppendInTemplateOrder)
{
    scene::Scene source;
    auto& root = source.CreateGameObject("Root");
    root.instanceId = "root";
    auto& z = source.CreateGameObject("Z");
    z.instanceId = "z";
    z.SetParent(root);
    auto& a = source.CreateGameObject("A");
    a.instanceId = "a";
    a.SetParent(root);
    ASSERT_TRUE(Save(source, root.GetID()));
    auto& destination = AttachScene();
    std::vector<scene::EntityID> roots;
    ASSERT_TRUE(editor::PrefabSerializer::Instantiate(destination, Path(), roots));
    const auto rootGuid = destination.GetGameObject(roots.front())->instanceId;
    editor::PrefabStructureMergePlan plan;
    std::string reason;
    ASSERT_TRUE(editor::PreviewPrefabStructureMerge(destination, roots.front(), ProjectRoot().generic_string(), plan));
    ASSERT_TRUE(editor::ApplyPrefabStructureMerge(destination, plan, roots, reason)) << reason;
    auto* instance = destination.FindByGuid(rootGuid);
    ASSERT_EQ(instance->GetChildCount(), 2);
    EXPECT_EQ(instance->GetChild(0)->prefabSourceId, "z");
    EXPECT_EQ(instance->GetChild(1)->prefabSourceId, "a");
    auto& n = source.CreateGameObject("N");
    n.instanceId = "n";
    n.SetParent(root);
    auto& m = source.CreateGameObject("M");
    m.instanceId = "m";
    m.SetParent(root);
    ASSERT_TRUE(Save(source, root.GetID()));
    ASSERT_TRUE(editor::PreviewPrefabStructureMerge(destination, instance->GetID(), ProjectRoot().generic_string(), plan));
    ASSERT_TRUE(editor::ApplyPrefabStructureMerge(destination, plan, roots, reason)) << reason;
    instance = destination.FindByGuid(rootGuid);
    ASSERT_EQ(instance->GetChildCount(), 4);
    EXPECT_EQ(instance->GetChild(0)->prefabSourceId, "z");
    EXPECT_EQ(instance->GetChild(1)->prefabSourceId, "a");
    EXPECT_EQ(instance->GetChild(2)->prefabSourceId, "n");
    EXPECT_EQ(instance->GetChild(3)->prefabSourceId, "m");
}

TEST_F(PrefabStructureMergeTest, NonRestorableUnrelatedMeshRejectsPreviewAndApplyWithoutChangingOriginals)
{
    scene::Scene source;
    auto& root = source.CreateGameObject("Root");
    ASSERT_TRUE(Save(source, root.GetID()));
    auto& destination = AttachScene();
    std::vector<scene::EntityID> roots;
    ASSERT_TRUE(editor::PrefabSerializer::Instantiate(destination, Path(), roots));
    editor::PrefabStructureMergePlan plan;
    ASSERT_TRUE(editor::PreviewPrefabStructureMerge(destination, roots.front(), ProjectRoot().generic_string(), plan));
    auto& unrelated = destination.CreateGameObject("UnsavedMesh");
    renderer::Mesh cpuOnlyMesh;
    auto& mesh = unrelated.AddComponent<scene::MeshRenderer>();
    mesh.mesh = &cpuOnlyMesh;
    struct ResetMeshPointer {
        scene::MeshRenderer& renderer;
        ~ResetMeshPointer() { renderer.mesh = nullptr; }
    } resetMeshPointer{mesh};
    const auto rootGuid = destination.GetGameObject(roots.front())->instanceId;
    auto* rootAddress = destination.FindByGuid(rootGuid);
    auto* unrelatedAddress = &unrelated;
    const auto before = Snapshot(destination);
    std::string reason;
    ASSERT_FALSE(editor::ApplyPrefabStructureMerge(destination, plan, roots, reason));
    EXPECT_NE(reason.find("no meshPath"), std::string::npos);
    editor::PrefabStructureMergePlan rejected;
    EXPECT_FALSE(editor::PreviewPrefabStructureMerge(destination, rootAddress->GetID(), ProjectRoot().generic_string(), rejected));
    ASSERT_FALSE(rejected.merge.conflicts.empty());
    EXPECT_NE(rejected.merge.conflicts.front().reason.find("no meshPath"), std::string::npos);
    EXPECT_EQ(Snapshot(destination), before);
    EXPECT_EQ(destination.FindByGuid(rootGuid), rootAddress);
    EXPECT_EQ(destination.FindByGuid(unrelated.instanceId), unrelatedAddress);
    EXPECT_EQ(mesh.mesh, &cpuOnlyMesh);
    mesh.mesh = nullptr;
}

namespace {
class PrefabRestoreMutationScript final : public scene::Script {
public:
    inline static bool rewriteAfterLoad = false;
    int setting = 7;
    const char* GetTypeName() const override { return "PrefabRestoreMutationScript"; }
    void Reflect(scene::IReflector& reflector) override { reflector.Field("setting", setting); }
    void OnAfterDeserialize() override { if (rewriteAfterLoad) setting = 99; }
};
} /// @note namespace

class PrefabRestoreCallbackTest : public PrefabStructureMergeTest {
protected:
    void SetUp() override
    {
        PrefabStructureMergeTest::SetUp();
        ASSERT_TRUE(scene::ScriptFactory::RegisteredTypeNames().empty()) << "Callback fixture requires an isolated factory registry";
        m_registered = scene::ScriptFactory::RegisterManaged("PrefabRestoreMutationScript",
            [](const std::string&) { return std::make_unique<PrefabRestoreMutationScript>(); });
        ASSERT_TRUE(m_registered);
        PrefabRestoreMutationScript::rewriteAfterLoad = false;
    }
    void TearDown() override
    {
        PrefabRestoreMutationScript::rewriteAfterLoad = false;
        PrefabStructureMergeTest::TearDown();
        if (m_registered) scene::ScriptFactory::UnregisterManaged();
    }
    PrefabRestoreMutationScript* AddExternalCallback(scene::Scene& destination)
    {
        auto& owner = destination.CreateGameObject("UnrelatedScript");
        scene::ScriptEntry entry;
        auto script = std::make_unique<PrefabRestoreMutationScript>();
        auto* address = script.get();
        entry.script = std::move(script);
        owner.AddComponent<scene::ScriptComponent>().scripts.push_back(std::move(entry));
        return address;
    }
    bool m_registered = false;
};

TEST_F(PrefabRestoreCallbackTest, ChangedRecoveryCallbackRejectsInitialApplyWithLiveSceneUntouched)
{
    scene::Scene source;
    auto& root = source.CreateGameObject("Root");
    ASSERT_TRUE(Save(source, root.GetID()));
    auto& destination = AttachScene();
    std::vector<scene::EntityID> roots;
    ASSERT_TRUE(editor::PrefabSerializer::Instantiate(destination, Path(), roots));
    auto* externalScript = AddExternalCallback(destination);
    auto* rootAddress = destination.GetGameObject(roots.front());
    const auto rootGuid = rootAddress->instanceId;
    editor::PrefabStructureMergePlan plan;
    ASSERT_TRUE(editor::PreviewPrefabStructureMerge(destination, roots.front(), ProjectRoot().generic_string(), plan));
    const auto before = Snapshot(destination);
    editor::UndoStack undo;
    Context().undoStack = &undo;
    PrefabRestoreMutationScript::rewriteAfterLoad = true;
    EXPECT_FALSE(undo.Execute(editor::MakePrefabStructureMergeCommand(Context(), plan)));
    EXPECT_NE(undo.LastDiagnostic().find("differs from the requested"), std::string::npos);
    EXPECT_EQ(undo.GetHistorySize(), 0u);
    EXPECT_EQ(destination.FindByGuid(rootGuid), rootAddress);
    EXPECT_EQ(externalScript->setting, 7);
    EXPECT_EQ(Snapshot(destination), before);
    Context().undoStack = nullptr;
}

TEST_F(PrefabRestoreCallbackTest, ChangedRestoreCallbackRejectsUndoAndRetainsLiveSceneAndHistory)
{
    scene::Scene source;
    auto& root = source.CreateGameObject("Root");
    ASSERT_TRUE(Save(source, root.GetID()));
    auto& destination = AttachScene();
    std::vector<scene::EntityID> roots;
    ASSERT_TRUE(editor::PrefabSerializer::Instantiate(destination, Path(), roots));
    auto* externalScript = AddExternalCallback(destination);
    editor::PrefabStructureMergePlan plan;
    ASSERT_TRUE(editor::PreviewPrefabStructureMerge(destination, roots.front(), ProjectRoot().generic_string(), plan));
    editor::UndoStack undo;
    Context().undoStack = &undo;
    ASSERT_TRUE(undo.Execute(editor::MakePrefabStructureMergeCommand(Context(), plan))) << undo.LastDiagnostic();
    auto* rootAddress = destination.FindByGuid(plan.rootGuid);
    const auto after = Snapshot(destination);
    const auto cursor = undo.GetCursor(), revision = undo.GetRevision();
    PrefabRestoreMutationScript::rewriteAfterLoad = true;
    EXPECT_FALSE(undo.Undo());
    EXPECT_NE(undo.LastDiagnostic().find("differs from the requested"), std::string::npos);
    EXPECT_EQ(undo.GetCursor(), cursor);
    EXPECT_EQ(undo.GetRevision(), revision);
    EXPECT_EQ(destination.FindByGuid(plan.rootGuid), rootAddress);
    EXPECT_EQ(externalScript->setting, 7);
    EXPECT_EQ(Snapshot(destination), after);
    Context().undoStack = nullptr;
}

TEST_F(PrefabStructureMergeTest, AdditionReparentLocalReferenceSaveLoadAndRepeatedApplyKeepStableGuids)
{
    scene::Scene source;
    auto& root = source.CreateGameObject("Root");
    auto& a = source.CreateGameObject("A");
    auto& b = source.CreateGameObject("B");
    a.SetParent(root);
    b.SetParent(root);
    const auto rootId = root.GetID();
    const auto aSource = a.instanceId, bSource = b.instanceId;
    ASSERT_TRUE(Save(source, rootId));
    auto& destination = AttachScene();
    std::vector<scene::EntityID> roots;
    ASSERT_TRUE(editor::PrefabSerializer::Instantiate(destination, Path(), roots));
    const auto rootGuid = destination.GetGameObject(roots.front())->instanceId;
    auto* instanceA = SourceObject(destination, aSource);
    ASSERT_NE(instanceA, nullptr);
    instanceA->tag = "local override";
    auto& local = destination.CreateGameObject("Local");
    local.SetParent(*instanceA);
    const auto localGuid = local.instanceId, aGuid = instanceA->instanceId;
    auto& follow = local.AddComponent<scene::CameraFollowComponent>();
    follow.target.id = instanceA->GetID();
    follow.target.guid = aGuid;
    b.SetParent(a);
    auto& added = source.CreateGameObject("Added");
    added.SetParent(root);
    const auto addedSource = added.instanceId;
    ASSERT_TRUE(Save(source, rootId));
    editor::PrefabStructureMergePlan plan;
    ASSERT_TRUE(editor::PreviewPrefabStructureMerge(destination, roots.front(), ProjectRoot().generic_string(), plan));
    std::string reason;
    ASSERT_TRUE(editor::ApplyPrefabStructureMerge(destination, plan, roots, reason)) << reason;
    ASSERT_NE(destination.FindByGuid(localGuid), nullptr);
    EXPECT_TRUE(destination.FindByGuid(localGuid)->prefabSourceId.empty());
    EXPECT_EQ(destination.FindByGuid(aGuid)->tag, "local override");
    ASSERT_NE(SourceObject(destination, bSource), nullptr);
    EXPECT_EQ(SourceObject(destination, bSource)->GetParent()->instanceId, aGuid);
    ASSERT_NE(SourceObject(destination, addedSource), nullptr);
    const auto addedGuid = SourceObject(destination, addedSource)->instanceId;
    const auto* restoredFollow = destination.FindByGuid(localGuid)->GetComponent<scene::CameraFollowComponent>();
    ASSERT_NE(restoredFollow, nullptr);
    EXPECT_EQ(restoredFollow->target.guid, aGuid);
    EXPECT_EQ(restoredFollow->target.id, destination.FindByGuid(aGuid)->GetID());
    ASSERT_TRUE(editor::PreviewPrefabStructureMerge(destination, roots.front(), ProjectRoot().generic_string(), plan));
    ASSERT_TRUE(editor::ApplyPrefabStructureMerge(destination, plan, roots, reason)) << reason;
    EXPECT_EQ(SourceObject(destination, addedSource)->instanceId, addedGuid);
    auto restored = scene::SceneSerializer::LoadDataFromText(Snapshot(destination), Path());
    ASSERT_NE(restored, nullptr);
    ASSERT_NE(restored->FindByGuid(rootGuid), nullptr);
    ASSERT_NE(restored->FindByGuid(localGuid), nullptr);
    EXPECT_TRUE(restored->FindByGuid(localGuid)->prefabSourceId.empty());
    EXPECT_FALSE(restored->FindByGuid(rootGuid)->prefabSourceSnapshot.empty());
    EXPECT_EQ(restored->FindByGuid(localGuid)->GetComponent<scene::CameraFollowComponent>()->target.guid, aGuid);
}

TEST_F(PrefabStructureMergeTest, CompatibleLocalDeletePersistsAcrossTwoTemplateRevisions)
{
    scene::Scene source;
    auto& root = source.CreateGameObject("Root");
    auto& child = source.CreateGameObject("Child");
    child.SetParent(root);
    const auto rootId = root.GetID();
    const auto childSource = child.instanceId;
    ASSERT_TRUE(Save(source, rootId));
    auto& destination = AttachScene();
    std::vector<scene::EntityID> roots;
    ASSERT_TRUE(editor::PrefabSerializer::Instantiate(destination, Path(), roots));
    const auto rootGuid = destination.GetGameObject(roots.front())->instanceId;
    destination.DestroyGameObject(SourceObject(destination, childSource)->GetID());
    for (const char* label : {"First", "Second"}) {
        auto& added = source.CreateGameObject(label);
        added.SetParent(root);
        ASSERT_TRUE(Save(source, rootId));
        const auto before = Snapshot(destination);
        auto* oldRoot = destination.FindByGuid(rootGuid);
        ASSERT_EQ(editor::PrefabSerializer::PropagateToInstances(destination, Path(), {}, ProjectRoot().generic_string()), 0);
        EXPECT_EQ(destination.FindByGuid(rootGuid), oldRoot);
        EXPECT_EQ(Snapshot(destination), before);
        editor::PrefabStructureMergePlan plan;
        ASSERT_TRUE(editor::PreviewPrefabStructureMerge(destination, oldRoot->GetID(), ProjectRoot().generic_string(), plan));
        std::string reason;
        ASSERT_TRUE(editor::ApplyPrefabStructureMerge(destination, plan, roots, reason)) << reason;
        EXPECT_EQ(SourceObject(destination, childSource), nullptr);
        EXPECT_NE(SourceObject(destination, added.instanceId), nullptr);
    }
}

TEST_F(PrefabStructureMergeTest, AutomaticRefusalPreservesLocalTopologyReferencesAndHistoryBeforeExplicitApply)
{
    scene::Scene source;
    auto& root = source.CreateGameObject("Root");
    root.tag = "baseline";
    source.CreateGameObject("A").SetParent(root);
    source.CreateGameObject("B").SetParent(root);
    source.CreateGameObject("RemovedLocally").SetParent(root);
    ASSERT_TRUE(Save(source, root.GetID()));
    auto& destination = AttachScene();
    std::vector<scene::EntityID> roots;
    ASSERT_TRUE(editor::PrefabSerializer::Instantiate(destination, Path(), roots));
    auto* instance = destination.GetGameObject(roots.front());
    const auto rootId = instance->GetID();
    const auto rootGuid = instance->instanceId;
    const auto oldBaseline = instance->prefabSourceSnapshot;
    auto* a = instance->GetChild(0);
    auto* b = instance->GetChild(1);
    const auto aGuid = a->instanceId, bGuid = b->instanceId;
    const auto deletedGuid = instance->GetChild(2)->instanceId;
    destination.DestroyGameObject(instance->GetChild(2)->GetID());
    b->SetParent(*a);
    auto& local = destination.CreateGameObject("Local");
    local.SetParent(*b);
    const auto localGuid = local.instanceId;
    auto& external = destination.CreateGameObject("External");
    const auto externalGuid = external.instanceId;
    auto& follow = external.AddComponent<scene::CameraFollowComponent>();
    follow.target.id = local.GetID();
    follow.target.guid = localGuid;
    ASSERT_TRUE(editor::PrefabSerializer::Instantiate(destination, Path(), roots));
    auto* author = destination.GetGameObject(roots.front());
    const auto authorId = author->GetID();
    author->tag = "incoming";
    auto& incoming = destination.CreateGameObject("Incoming");
    incoming.SetParent(*author);
    editor::UndoStack undo;
    const auto oldTag = a->tag;
    ASSERT_TRUE(undo.Execute(std::make_unique<editor::LambdaCommand>("Local property",
        [&destination, aGuid] { destination.FindByGuid(aGuid)->tag = "local override"; },
        [&destination, aGuid, oldTag] { destination.FindByGuid(aGuid)->tag = oldTag; })));
    const auto cursor = undo.GetCursor(), revision = undo.GetRevision();

    ASSERT_EQ(editor::PrefabSerializer::ApplyAndPropagate(destination, authorId, ProjectRoot().generic_string()), 0);
    EXPECT_EQ(destination.GetGameObject(rootId), instance);
    EXPECT_EQ(instance->prefabSourceSnapshot, oldBaseline);
    const auto incomingSource = incoming.prefabSourceId;
    ASSERT_FALSE(incomingSource.empty());
    const auto before = Snapshot(destination);
    ASSERT_EQ(editor::PrefabSerializer::PropagateToInstances(destination, Path(), authorId, ProjectRoot().generic_string()), 0);
    std::vector<scene::EntityID> refreshed;
    ASSERT_FALSE(editor::PrefabSerializer::RefreshInstanceKeepingOverrides(destination, rootId, refreshed, ProjectRoot().generic_string()));
    EXPECT_TRUE(refreshed.empty());
    editor::PrefabOverrideSet overrides;
    ASSERT_TRUE(editor::ComputePrefabOverrides(destination, rootId, ProjectRoot().generic_string(), overrides));
    ASSERT_TRUE(overrides.hasStructuralOverrides);
    ASSERT_FALSE(editor::PrefabSerializer::Revert(destination, rootId, refreshed, ProjectRoot().generic_string(), &overrides));
    EXPECT_EQ(Snapshot(destination), before);
    EXPECT_EQ(destination.GetGameObject(rootId), instance);
    EXPECT_EQ(destination.FindByGuid(aGuid), a);
    EXPECT_EQ(destination.FindByGuid(bGuid), b);
    EXPECT_EQ(destination.FindByGuid(localGuid), &local);
    EXPECT_EQ(b->GetParent(), a);
    EXPECT_EQ(local.GetParent(), b);
    EXPECT_EQ(follow.target.Resolve(destination), &local);
    EXPECT_EQ(instance->tag, "baseline");
    EXPECT_EQ(undo.GetHistorySize(), 1u);
    EXPECT_EQ(undo.GetCursor(), cursor);
    EXPECT_EQ(undo.GetRevision(), revision);

    editor::PrefabStructureMergePlan plan;
    ASSERT_TRUE(editor::PreviewPrefabStructureMerge(destination, rootId, ProjectRoot().generic_string(), plan));
    ASSERT_TRUE(undo.Execute(editor::MakePrefabStructureMergeCommand(Context(), plan))) << undo.LastDiagnostic();
    EXPECT_EQ(undo.GetHistorySize(), 2u);
    auto* merged = destination.FindByGuid(rootGuid);
    ASSERT_NE(merged, nullptr);
    EXPECT_EQ(merged->tag, "incoming");
    EXPECT_EQ(destination.FindByGuid(aGuid)->tag, "local override");
    EXPECT_EQ(destination.FindByGuid(bGuid)->GetParent(), destination.FindByGuid(aGuid));
    EXPECT_EQ(destination.FindByGuid(localGuid)->GetParent(), destination.FindByGuid(bGuid));
    EXPECT_EQ(destination.FindByGuid(deletedGuid), nullptr);
    ASSERT_EQ(merged->GetChildCount(), 2);
    EXPECT_EQ(merged->GetChild(1)->prefabSourceId, incomingSource);
    EXPECT_EQ(destination.FindByGuid(externalGuid)->GetComponent<scene::CameraFollowComponent>()->target.Resolve(destination),
        destination.FindByGuid(localGuid));
    const auto after = Snapshot(destination);
    ASSERT_TRUE(undo.Undo()) << undo.LastDiagnostic();
    EXPECT_EQ(Snapshot(destination), before);
    EXPECT_EQ(undo.GetCursor(), cursor);
    ASSERT_TRUE(undo.Redo()) << undo.LastDiagnostic();
    EXPECT_EQ(Snapshot(destination), after);
}

TEST_F(PrefabStructureMergeTest, AutomaticPropagationRetainsIncomingTopologyUpdatesForUnchangedLocalHierarchy)
{
    scene::Scene source;
    auto& root = source.CreateGameObject("Root");
    auto& a = source.CreateGameObject("A");
    a.SetParent(root);
    auto& b = source.CreateGameObject("B");
    b.SetParent(root);
    auto& removed = source.CreateGameObject("Removed");
    removed.SetParent(root);
    const auto aSource = a.instanceId, bSource = b.instanceId, removedSource = removed.instanceId;
    ASSERT_TRUE(Save(source, root.GetID()));
    auto& destination = AttachScene();
    std::vector<scene::EntityID> roots;
    ASSERT_TRUE(editor::PrefabSerializer::Instantiate(destination, Path(), roots));
    const auto rootGuid = destination.GetGameObject(roots.front())->instanceId;
    SourceObject(destination, aSource)->tag = "local property";
    b.SetParent(a);
    source.DestroyGameObject(removed.GetID());
    auto& added = source.CreateGameObject("Added");
    added.SetParent(root);
    const auto addedSource = added.instanceId;
    root.tag = "incoming";
    ASSERT_TRUE(Save(source, root.GetID()));

    ASSERT_EQ(editor::PrefabSerializer::PropagateToInstances(destination, Path(), {}, ProjectRoot().generic_string()), 1);

    ASSERT_NE(destination.FindByGuid(rootGuid), nullptr);
    EXPECT_EQ(destination.FindByGuid(rootGuid)->tag, "incoming");
    ASSERT_NE(SourceObject(destination, aSource), nullptr);
    EXPECT_EQ(SourceObject(destination, aSource)->tag, "local property");
    ASSERT_NE(SourceObject(destination, bSource), nullptr);
    EXPECT_EQ(SourceObject(destination, bSource)->GetParent(), SourceObject(destination, aSource));
    EXPECT_EQ(SourceObject(destination, removedSource), nullptr);
    ASSERT_NE(SourceObject(destination, addedSource), nullptr);
    EXPECT_EQ(SourceObject(destination, addedSource)->GetParent(), destination.FindByGuid(rootGuid));
}

TEST_F(PrefabStructureMergeTest, ExplicitApplyUndoRedoRestoreDeletedObjectAndExternalReference)
{
    scene::Scene source;
    auto& root = source.CreateGameObject("Root");
    auto& child = source.CreateGameObject("Child");
    child.SetParent(root);
    const auto childSource = child.instanceId, rootSource = root.instanceId;
    ASSERT_TRUE(Save(source, root.GetID()));
    auto& destination = AttachScene();
    std::vector<scene::EntityID> roots;
    ASSERT_TRUE(editor::PrefabSerializer::Instantiate(destination, Path(), roots));
    const auto rootGuid = destination.GetGameObject(roots.front())->instanceId;
    const auto childGuid = SourceObject(destination, childSource)->instanceId;
    auto& external = destination.CreateGameObject("External");
    const auto externalGuid = external.instanceId;
    external.AddComponent<scene::CameraFollowComponent>().target.id = SourceObject(destination, childSource)->GetID();
    external.GetComponent<scene::CameraFollowComponent>()->target.guid = childGuid;
    source.DestroyGameObject(child.GetID());
    ASSERT_TRUE(Save(source, source.FindByGuid(rootSource)->GetID()));
    editor::PrefabStructureMergePlan plan;
    ASSERT_TRUE(editor::PreviewPrefabStructureMerge(destination, roots.front(), ProjectRoot().generic_string(), plan));
    editor::UndoStack undo;
    Context().undoStack = &undo;
    ASSERT_TRUE(undo.Execute(editor::MakePrefabStructureMergeCommand(Context(), plan))) << undo.LastDiagnostic();
    EXPECT_EQ(destination.FindByGuid(childGuid), nullptr);
    EXPECT_FALSE(destination.FindByGuid(externalGuid)->GetComponent<scene::CameraFollowComponent>()->target.id.IsValid());
    ASSERT_TRUE(undo.Undo()) << undo.LastDiagnostic();
    ASSERT_NE(destination.FindByGuid(childGuid), nullptr);
    EXPECT_EQ(destination.FindByGuid(externalGuid)->GetComponent<scene::CameraFollowComponent>()->target.id,
        destination.FindByGuid(childGuid)->GetID());
    ASSERT_TRUE(undo.Redo()) << undo.LastDiagnostic();
    EXPECT_EQ(destination.FindByGuid(childGuid), nullptr);
    ASSERT_NE(destination.FindByGuid(rootGuid), nullptr);
    destination.FindByGuid(externalGuid)->tag = "unrelated edit";
    const auto cursor = undo.GetCursor();
    EXPECT_FALSE(undo.Undo());
    EXPECT_EQ(undo.GetCursor(), cursor);
    EXPECT_EQ(destination.FindByGuid(externalGuid)->tag, "unrelated edit");
    Context().undoStack = nullptr;
}

TEST_F(PrefabStructureMergeTest, StaleTemplateAndSceneReviewsFailBeforeMutation)
{
    scene::Scene source;
    auto& root = source.CreateGameObject("Root");
    ASSERT_TRUE(Save(source, root.GetID()));
    auto& destination = AttachScene();
    std::vector<scene::EntityID> roots;
    ASSERT_TRUE(editor::PrefabSerializer::Instantiate(destination, Path(), roots));
    editor::PrefabStructureMergePlan plan;
    ASSERT_TRUE(editor::PreviewPrefabStructureMerge(destination, roots.front(), ProjectRoot().generic_string(), plan));
    source.CreateGameObject("UnrelatedSourceRoot");
    root.tag = "changed template";
    ASSERT_TRUE(Save(source, root.GetID()));
    const auto before = Snapshot(destination);
    std::string reason;
    EXPECT_FALSE(editor::ApplyPrefabStructureMerge(destination, plan, roots, reason));
    EXPECT_EQ(Snapshot(destination), before);
    auto* instance = destination.FindByGuid(plan.rootGuid);
    ASSERT_NE(instance, nullptr);
    ASSERT_TRUE(editor::PreviewPrefabStructureMerge(destination, instance->GetID(), ProjectRoot().generic_string(), plan));
    instance->tag = "changed instance";
    const auto edited = Snapshot(destination);
    EXPECT_FALSE(editor::ApplyPrefabStructureMerge(destination, plan, roots, reason));
    EXPECT_EQ(Snapshot(destination), edited);
}
} /// @note namespace fbzz::tests
