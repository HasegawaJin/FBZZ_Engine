/// @file    PlayChangesAdoptionTests.cpp
/// @brief   Native and generated Play adoption preserves Edit identities and checked all-or-nothing history.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/PlayChangesAdoption.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <initializer_list>
namespace fbzz::tests {
namespace {
class PlayChangesAdoptionTests : public testkit::EditorFixture {
protected:
    void SetUp() override {
        EditorFixture::SetUp(); AttachScene(); Context().playMode = &m_play;
        Context().operators = &m_registry; Context().undoStack = &m_editUndo;
        Context().markSceneDirty = [this]() { ++m_dirty; };
        editor::RegisterPlayChangesAdoptionOperators(m_registry);
    }
    void TearDown() override {
        m_play.Cancel(); Context().activeScene = AttachedScene(); Context().playMode = nullptr;
        Context().operators = nullptr; Context().undoStack = nullptr; Context().markSceneDirty = {};
        EditorFixture::TearDown();
    }
    void Start() {
        m_play.Play(*AttachedScene()); ASSERT_FALSE(m_play.IsInEditor()) << m_play.Diagnostic();
        Context().activeScene = m_play.PlayScene(); Context().undoStack = m_play.TemporaryUndo();
    }
    void Stop() {
        m_play.Stop(*AttachedScene()); ASSERT_TRUE(m_play.ApplyPendingRestore(*AttachedScene()));
        Context().activeScene = AttachedScene(); Context().undoStack = &m_editUndo;
    }
    editor::OpData Selection(const std::string& guid, std::initializer_list<const char*> paths) {
        editor::OpData selection = editor::OpData::MakeObject(), targets = editor::OpData::MakeArray();
        editor::OpData target = editor::OpData::MakeObject(), chosen = editor::OpData::MakeArray();
        for (const char* path : paths) chosen.Push(path);
        target.Set("ownerGuid", guid); target.Set("paths", chosen); targets.Push(target); selection.Set("targets", targets); return selection;
    }
    editor::OpResult Capture(const editor::OpData& selection) {
        editor::OpArgs args; args.Set("selection", selection); return editor::InvokeOperator(Context(), "scene.play_changes.capture", args);
    }
    editor::OpData Generated(const std::string& guid) {
        editor::OpData selection = editor::OpData::MakeObject(), roots = editor::OpData::MakeArray();
        roots.Push(guid); selection.Set("generatedRoots", roots); return selection;
    }
    editor::OpResult Invoke(const char* action, const std::string& id) {
        editor::OpArgs args; args.Set("adoptionId", id); return editor::InvokeOperator(Context(), std::string("scene.play_changes.") + action, args);
    }
    std::string Id(const editor::OpResult& result) { return result.data.Find("adoptionId")->AsString(); }
    editor::PlayModeController m_play;
    editor::OperatorRegistry m_registry;
    editor::UndoStack m_editUndo;
    int m_dirty = 0;
};
TEST_F(PlayChangesAdoptionTests, SelectedNativeFieldsPreserveOriginalObjectsAndUnselectedComponents) {
    auto& original = AttachedScene()->CreateGameObject("Authoring");
    const auto id = original.GetID(); const auto guid = original.instanceId; auto* address = &original;
    original.AddComponent<scene::LightComponent>().intensity = 4.0f;
    Start(); auto* source = m_play.PlayScene()->FindByGuid(guid);
    source->transform.position = {2.0f, 3.0f, 4.0f}; source->GetComponent<scene::LightComponent>()->intensity = 90.0f;
    m_play.Pause(); ASSERT_TRUE(m_play.IsPaused());
    const auto captured = Capture(Selection(guid, {"Transform.position"})); ASSERT_TRUE(captured.ok) << captured.message;
    const auto draft = Id(captured); source->transform.position.x = 99.0f;
    Stop(); EXPECT_EQ(AttachedScene()->GetGameObject(id), address); EXPECT_FLOAT_EQ(original.transform.position.x, 0.0f);
    ASSERT_TRUE(Invoke("prepare", draft).ok); ASSERT_TRUE(Invoke("apply", draft).ok);
    EXPECT_EQ(AttachedScene()->GetGameObject(id), address); EXPECT_FLOAT_EQ(original.transform.position.x, 2.0f);
    EXPECT_FLOAT_EQ(original.GetComponent<scene::LightComponent>()->intensity, 4.0f); EXPECT_EQ(m_editUndo.GetHistorySize(), 1u);
    ASSERT_TRUE(m_editUndo.Undo()); EXPECT_FLOAT_EQ(original.transform.position.x, 0.0f);
    ASSERT_TRUE(m_editUndo.Redo()); EXPECT_FLOAT_EQ(original.transform.position.x, 2.0f); EXPECT_EQ(&original, address);
}
TEST_F(PlayChangesAdoptionTests, GeneratedForestPreservesParentsReferencesAndPersistenceAcrossUndoRedo) {
    auto& anchor = AttachedScene()->CreateGameObject("Anchor"); const auto anchorGuid = anchor.instanceId;
    Start(); auto& runtime = *m_play.PlayScene();
    auto& root = runtime.CreateGameObject("GeneratedRoot"); root.runtimeGenerated = true;
    auto& child = runtime.CreateGameObject("GeneratedTarget"); child.AddComponent<scene::TargetPointComponent>();
    auto& sibling = runtime.CreateGameObject("GeneratedSibling");
    ASSERT_TRUE(root.SetParent(runtime.FindByGuid(anchorGuid))); ASSERT_TRUE(child.SetParent(&root));
    ASSERT_TRUE(sibling.SetParent(&root));
    root.transform.position = {2.0f, 0.0f, 0.0f}; child.transform.position = {0.0f, 3.0f, 0.0f};
    const auto rootGuid = root.instanceId, childGuid = child.instanceId;
    root.AddComponent<scene::TransformConstraintComponent>().target = {child.GetID(), childGuid, runtime.GetRenderSceneGeneration()};
    const auto captured = Capture(Generated(rootGuid)); ASSERT_TRUE(captured.ok) << captured.message; const auto draft = Id(captured);
    Stop(); EXPECT_EQ(AttachedScene()->FindByGuid(rootGuid), nullptr);
    ASSERT_TRUE(Invoke("prepare", draft).ok); ASSERT_TRUE(Invoke("apply", draft).ok);
    auto* adopted = AttachedScene()->FindByGuid(rootGuid); auto* target = AttachedScene()->FindByGuid(childGuid);
    ASSERT_NE(adopted, nullptr); ASSERT_NE(target, nullptr); EXPECT_FALSE(adopted->runtimeGenerated);
    EXPECT_EQ(adopted->GetParent(), &anchor); EXPECT_EQ(target->GetParent(), adopted);
    EXPECT_EQ(adopted->GetChild(0), target); EXPECT_EQ(adopted->GetChild(1)->name, "GeneratedSibling");
    EXPECT_FLOAT_EQ(adopted->transform.position.x, 2.0f); EXPECT_FLOAT_EQ(target->transform.position.y, 3.0f);
    EXPECT_EQ(adopted->GetComponent<scene::TransformConstraintComponent>()->target.Resolve(*AttachedScene()), target);
    const auto firstBirth = adopted->GetID(); const auto saved = scene::SceneSerializer::SaveToText(*AttachedScene());
    EXPECT_NE(saved.find(rootGuid), std::string::npos); EXPECT_NE(saved.find(childGuid), std::string::npos);
    auto reloaded = scene::SceneSerializer::LoadDataFromText(saved, {}); ASSERT_NE(reloaded, nullptr);
    auto* loaded = reloaded->FindByGuid(rootGuid); ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->GetComponent<scene::TransformConstraintComponent>()->target.Resolve(*reloaded), reloaded->FindByGuid(childGuid));
    ASSERT_TRUE(m_editUndo.Undo()); EXPECT_EQ(AttachedScene()->FindByGuid(rootGuid), nullptr); EXPECT_EQ(AttachedScene()->FindByGuid(childGuid), nullptr);
    ASSERT_TRUE(m_editUndo.Redo()); adopted = AttachedScene()->FindByGuid(rootGuid); ASSERT_NE(adopted, nullptr);
    EXPECT_NE(adopted->GetID(), firstBirth); EXPECT_EQ(adopted->GetComponent<scene::TransformConstraintComponent>()->target.Resolve(*AttachedScene()), AttachedScene()->FindByGuid(childGuid));
}
TEST_F(PlayChangesAdoptionTests, GeneratedUnsupportedComponentRejectsTheWholeCapture) {
    AttachedScene()->CreateGameObject("Initial"); Start();
    auto& root = m_play.PlayScene()->CreateGameObject("Unsupported"); root.AddComponent<scene::BoxColliderComponent>();
    const auto result = Capture(Generated(root.instanceId)); EXPECT_FALSE(result.ok); EXPECT_EQ(result.errorCode, "UNSUPPORTED_GENERATED");
    EXPECT_NE(result.message.find("BoxColliderComponent"), std::string::npos); Stop(); EXPECT_EQ(AttachedScene()->GameObjectCount(), 1u); EXPECT_EQ(m_dirty, 0);
}
TEST_F(PlayChangesAdoptionTests, UnselectedGeneratedReferenceRequiresExplicitGraphSelection) {
    AttachedScene()->CreateGameObject("Initial"); Start(); auto& runtime = *m_play.PlayScene();
    auto& root = runtime.CreateGameObject("Root"); auto& target = runtime.CreateGameObject("OtherRoot");
    root.AddComponent<scene::TransformConstraintComponent>().target = {target.GetID(), target.instanceId, runtime.GetRenderSceneGeneration()};
    EXPECT_FALSE(Capture(Generated(root.instanceId)).ok);
    auto selection = Generated(root.instanceId); auto roots = *selection.Find("generatedRoots"); roots.Push(target.instanceId); selection.Set("generatedRoots", roots);
    EXPECT_TRUE(Capture(selection).ok);
}
TEST_F(PlayChangesAdoptionTests, UnsupportedNativePathAndReaddedComponentAreRejected) {
    auto& owner = AttachedScene()->CreateGameObject("Camera"); const auto guid = owner.instanceId; owner.AddComponent<scene::CameraComponent>(); Start();
    EXPECT_EQ(Capture(Selection(guid, {"CameraComponent.aspectRatio"})).errorCode, "UNSUPPORTED_PATH");
    auto* source = m_play.PlayScene()->FindByGuid(guid); source->RemoveComponent<scene::CameraComponent>(); source->AddComponent<scene::CameraComponent>();
    EXPECT_EQ(Capture(Selection(guid, {"CameraComponent.fovY"})).errorCode, "STALE_COMPONENT");
}
TEST_F(PlayChangesAdoptionTests, StopWithoutCaptureAndCancellationNeverAdoptRuntimeValues) {
    auto& owner = AttachedScene()->CreateGameObject("Original"); const auto guid = owner.instanceId;
    Start(); m_play.PlayScene()->FindByGuid(guid)->transform.position.x = 8.0f;
    Stop(); EXPECT_FLOAT_EQ(owner.transform.position.x, 0.0f); EXPECT_FALSE(m_editUndo.CanUndo());
    Start(); m_play.PlayScene()->FindByGuid(guid)->transform.position.x = 12.0f;
    const auto captured = Capture(Selection(guid, {"Transform.position"})); ASSERT_TRUE(captured.ok); const auto draft = Id(captured);
    m_play.Cancel(); Context().activeScene = AttachedScene(); Context().undoStack = &m_editUndo;
    EXPECT_FALSE(Invoke("prepare", draft).ok); EXPECT_FLOAT_EQ(owner.transform.position.x, 0.0f); EXPECT_EQ(m_dirty, 0);
}
TEST_F(PlayChangesAdoptionTests, DeletedOwnerAndReloadedSceneCannotReceiveAnOldDraft) {
    auto& owner = AttachedScene()->CreateGameObject("Original"); const auto guid = owner.instanceId; const auto id = owner.GetID();
    Start(); m_play.PlayScene()->FindByGuid(guid)->transform.position.x = 5.0f;
    const auto captured = Capture(Selection(guid, {"Transform.position"})); ASSERT_TRUE(captured.ok); const auto draft = Id(captured);
    Stop(); ASSERT_TRUE(AttachedScene()->DestroyGameObject(id));
    auto& replacement = AttachedScene()->CreateGameObject("Replacement"); replacement.instanceId = guid;
    EXPECT_FALSE(Invoke("prepare", draft).ok); EXPECT_FLOAT_EQ(replacement.transform.position.x, 0.0f);
    AttachedScene()->Clear(); AttachedScene()->CreateGameObject("OtherScene"); EXPECT_FALSE(Invoke("prepare", draft).ok); EXPECT_EQ(m_dirty, 0);
}
TEST_F(PlayChangesAdoptionTests, NameAndIdConflictsLeaveEverySelectedTargetUnchanged) {
    auto& owner = AttachedScene()->CreateGameObject("Original"); const auto guid = owner.instanceId;
    Start(); m_play.PlayScene()->FindByGuid(guid)->transform.position.x = 6.0f;
    auto& generated = m_play.PlayScene()->CreateGameObject("Collision"); const auto generatedGuid = generated.instanceId;
    auto selection = Selection(guid, {"Transform.position"}); selection.Set("generatedRoots", *Generated(generatedGuid).Find("generatedRoots"));
    const auto captured = Capture(selection); ASSERT_TRUE(captured.ok); const auto draft = Id(captured); Stop();
    auto& collision = AttachedScene()->CreateGameObject("Collision"); EXPECT_FALSE(Invoke("prepare", draft).ok);
    EXPECT_FLOAT_EQ(owner.transform.position.x, 0.0f); collision.name = "DifferentName"; collision.instanceId = generatedGuid;
    EXPECT_FALSE(Invoke("prepare", draft).ok); EXPECT_FLOAT_EQ(owner.transform.position.x, 0.0f); EXPECT_EQ(m_editUndo.GetHistorySize(), 0u); EXPECT_EQ(m_dirty, 0);
}
TEST_F(PlayChangesAdoptionTests, StalePreparedHistoryRequiresFreshReviewAndUndoConflictKeepsCursor) {
    auto& owner = AttachedScene()->CreateGameObject("Original"); const auto guid = owner.instanceId;
    Start(); m_play.PlayScene()->FindByGuid(guid)->transform.position.x = 6.0f;
    const auto captured = Capture(Selection(guid, {"Transform.position"})); ASSERT_TRUE(captured.ok); const auto draft = Id(captured); Stop();
    ASSERT_TRUE(Invoke("prepare", draft).ok);
    ASSERT_TRUE(m_editUndo.Push(std::make_unique<editor::LambdaCommand>("Unrelated history", [] {}, [] {})));
    EXPECT_EQ(Invoke("apply", draft).errorCode, "STALE_PREVIEW"); EXPECT_FLOAT_EQ(owner.transform.position.x, 0.0f);
    ASSERT_TRUE(Invoke("prepare", draft).ok); ASSERT_TRUE(Invoke("apply", draft).ok);
    const auto cursor = m_editUndo.GetCursor(), revision = m_editUndo.GetRevision(); owner.transform.position.x = 7.0f;
    EXPECT_FALSE(m_editUndo.Undo()); EXPECT_EQ(m_editUndo.GetCursor(), cursor); EXPECT_EQ(m_editUndo.GetRevision(), revision); EXPECT_FLOAT_EQ(owner.transform.position.x, 7.0f);
}
TEST_F(PlayChangesAdoptionTests, GeneratedUndoRefusesToDestroyNewUnownedChildren) {
    AttachedScene()->CreateGameObject("Initial"); Start(); auto& generated = m_play.PlayScene()->CreateGameObject("Generated"); const auto guid = generated.instanceId;
    const auto captured = Capture(Generated(guid)); ASSERT_TRUE(captured.ok); const auto draft = Id(captured); Stop();
    ASSERT_TRUE(Invoke("prepare", draft).ok); ASSERT_TRUE(Invoke("apply", draft).ok);
    auto* adopted = AttachedScene()->FindByGuid(guid); auto& added = AttachedScene()->CreateGameObject("LaterChild"); ASSERT_TRUE(added.SetParent(adopted));
    const auto cursor = m_editUndo.GetCursor(); EXPECT_FALSE(m_editUndo.Undo()); EXPECT_EQ(m_editUndo.GetCursor(), cursor); EXPECT_EQ(added.GetParent(), adopted);
}
TEST_F(PlayChangesAdoptionTests, DraftIdsDoNotAliasLaterPlaySessionsAndDiscardDoesNotMutateEdit) {
    auto& owner = AttachedScene()->CreateGameObject("Original"); const auto guid = owner.instanceId;
    Start(); m_play.PlayScene()->FindByGuid(guid)->transform.position.x = 1.0f;
    const auto first = Capture(Selection(guid, {"Transform.position"})); ASSERT_TRUE(first.ok); const auto firstId = Id(first); Stop();
    ASSERT_TRUE(Invoke("discard", firstId).ok); EXPECT_FALSE(Invoke("review", firstId).ok); EXPECT_EQ(m_dirty, 0);
    Start(); m_play.PlayScene()->FindByGuid(guid)->transform.position.x = 2.0f;
    const auto second = Capture(Selection(guid, {"Transform.position"})); ASSERT_TRUE(second.ok); EXPECT_NE(Id(second), firstId);
    EXPECT_FALSE(Invoke("review", firstId).ok); Stop(); EXPECT_FLOAT_EQ(owner.transform.position.x, 0.0f);
}
TEST_F(PlayChangesAdoptionTests, GeneratedRootGroupsAppendAfterExistingSiblingsInPlayOrderAcrossRedo) {
    auto& anchor = AttachedScene()->CreateGameObject("Anchor"); const auto anchorGuid = anchor.instanceId;
    auto& existing = AttachedScene()->CreateGameObject("ExistingChild"); ASSERT_TRUE(existing.SetParent(&anchor));
    Start(); auto& runtime = *m_play.PlayScene();
    auto& firstRoot = runtime.CreateGameObject("FirstRoot"); firstRoot.instanceId = "00000000-0000-4000-8000-000000000004";
    auto& secondRoot = runtime.CreateGameObject("SecondRoot"); secondRoot.instanceId = "00000000-0000-4000-8000-000000000003";
    auto& firstChild = runtime.CreateGameObject("FirstChild"); firstChild.instanceId = "00000000-0000-4000-8000-000000000002";
    auto& secondChild = runtime.CreateGameObject("SecondChild"); secondChild.instanceId = "00000000-0000-4000-8000-000000000001";
    ASSERT_TRUE(firstChild.SetParent(runtime.FindByGuid(anchorGuid))); ASSERT_TRUE(secondChild.SetParent(runtime.FindByGuid(anchorGuid)));
    auto selection = Generated(secondChild.instanceId); auto roots = *selection.Find("generatedRoots");
    roots.Push(secondRoot.instanceId); roots.Push(firstChild.instanceId); roots.Push(firstRoot.instanceId); selection.Set("generatedRoots", roots);
    const auto captured = Capture(selection); ASSERT_TRUE(captured.ok) << captured.message; const auto draft = Id(captured); Stop();
    ASSERT_TRUE(Invoke("prepare", draft).ok); const auto applied = Invoke("apply", draft); ASSERT_TRUE(applied.ok) << applied.message;
    const auto verifyOrder = [&]() {
        const auto sceneRoots = AttachedScene()->GetRootGameObjects(); ASSERT_EQ(sceneRoots.size(), 3u);
        EXPECT_EQ(sceneRoots[0], &anchor); EXPECT_EQ(sceneRoots[1]->name, "FirstRoot"); EXPECT_EQ(sceneRoots[2]->name, "SecondRoot");
        ASSERT_EQ(anchor.GetChildCount(), 3); EXPECT_EQ(anchor.GetChild(0), &existing);
        EXPECT_EQ(anchor.GetChild(1)->name, "FirstChild"); EXPECT_EQ(anchor.GetChild(2)->name, "SecondChild");
    };
    verifyOrder(); ASSERT_TRUE(m_editUndo.Undo()) << m_editUndo.LastDiagnostic();
    ASSERT_EQ(anchor.GetChildCount(), 1); EXPECT_EQ(anchor.GetChild(0), &existing);
    ASSERT_TRUE(m_editUndo.Redo()) << m_editUndo.LastDiagnostic(); verifyOrder();
}
TEST_F(PlayChangesAdoptionTests, GeneratedUndoRejectsNewOutsideEntityAndCustomCodecReferencesWithoutAdvancingHistory) {
    auto& outside = AttachedScene()->CreateGameObject("Outside"); outside.AddComponent<scene::NavMeshAgentComponent>();
    Start(); auto& generated = m_play.PlayScene()->CreateGameObject("Generated"); const auto guid = generated.instanceId;
    const auto captured = Capture(Generated(guid)); ASSERT_TRUE(captured.ok); const auto draft = Id(captured); Stop();
    ASSERT_TRUE(Invoke("prepare", draft).ok); ASSERT_TRUE(Invoke("apply", draft).ok); auto* adopted = AttachedScene()->FindByGuid(guid); ASSERT_NE(adopted, nullptr);
    auto* agent = outside.GetComponent<scene::NavMeshAgentComponent>(); agent->target = {adopted->GetID()};
    const auto originalReference = agent->target;
    const auto cursor = m_editUndo.GetCursor(), revision = m_editUndo.GetRevision(); const auto dirty = m_dirty;
    ASSERT_FALSE(m_editUndo.Undo()); EXPECT_NE(m_editUndo.LastDiagnostic().find("INCOMING_REFERENCE"), std::string::npos);
    EXPECT_EQ(m_editUndo.GetCursor(), cursor); EXPECT_EQ(m_editUndo.GetRevision(), revision); EXPECT_EQ(m_dirty, dirty);
    EXPECT_EQ(agent->target.id, originalReference.id); EXPECT_EQ(agent->target.guid, originalReference.guid); EXPECT_EQ(agent->target.sceneGeneration, originalReference.sceneGeneration);
    auto resolvedReference = agent->target; EXPECT_EQ(resolvedReference.Resolve(*AttachedScene()), adopted); EXPECT_EQ(AttachedScene()->FindByGuid(guid), adopted);
    agent->target = {};
    auto& lod = outside.AddComponent<scene::LODGroupComponent>(); lod.levels.push_back({});
    lod.levels.back().renderers.push_back({{}, adopted->GetID()});
    ASSERT_FALSE(m_editUndo.Undo()); EXPECT_NE(m_editUndo.LastDiagnostic().find("INCOMING_REFERENCE"), std::string::npos);
    EXPECT_EQ(m_editUndo.GetCursor(), cursor); EXPECT_EQ(m_editUndo.GetRevision(), revision); EXPECT_EQ(m_dirty, dirty);
    EXPECT_TRUE(lod.levels.back().renderers.front().instanceId.empty()); EXPECT_EQ(lod.levels.back().renderers.front().entity, adopted->GetID());
    lod.levels.clear(); ASSERT_TRUE(m_editUndo.Undo()) << m_editUndo.LastDiagnostic(); EXPECT_EQ(AttachedScene()->FindByGuid(guid), nullptr);
}
TEST_F(PlayChangesAdoptionTests, GeneratedUndoAllowsOnlyTheExactOwnedReferenceLeafBeingRestored) {
    auto& owner = AttachedScene()->CreateGameObject("Original"); owner.AddComponent<scene::TransformConstraintComponent>(); const auto ownerGuid = owner.instanceId;
    Start(); auto& generated = m_play.PlayScene()->CreateGameObject("Generated"); const auto guid = generated.instanceId;
    m_play.PlayScene()->FindByGuid(ownerGuid)->GetComponent<scene::TransformConstraintComponent>()->target = {generated.GetID(), guid, m_play.PlayScene()->GetRenderSceneGeneration()};
    auto selection = Selection(ownerGuid, {"TransformConstraintComponent.target"}); selection.Set("generatedRoots", *Generated(guid).Find("generatedRoots"));
    const auto captured = Capture(selection); ASSERT_TRUE(captured.ok) << captured.message; const auto draft = Id(captured); Stop();
    ASSERT_TRUE(Invoke("prepare", draft).ok); ASSERT_TRUE(Invoke("apply", draft).ok); auto* adopted = AttachedScene()->FindByGuid(guid); ASSERT_NE(adopted, nullptr);
    auto& agent = owner.AddComponent<scene::NavMeshAgentComponent>(); agent.target = {adopted->GetID(), guid, AttachedScene()->GetRenderSceneGeneration()};
    const auto cursor = m_editUndo.GetCursor(); EXPECT_FALSE(m_editUndo.Undo()); EXPECT_EQ(m_editUndo.GetCursor(), cursor);
    agent.target = {}; ASSERT_TRUE(m_editUndo.Undo()) << m_editUndo.LastDiagnostic();
    EXPECT_FALSE(owner.GetComponent<scene::TransformConstraintComponent>()->target.IsAssigned()); EXPECT_EQ(AttachedScene()->FindByGuid(guid), nullptr);
    ASSERT_TRUE(m_editUndo.Redo()) << m_editUndo.LastDiagnostic();
    EXPECT_EQ(owner.GetComponent<scene::TransformConstraintComponent>()->target.Resolve(*AttachedScene()), AttachedScene()->FindByGuid(guid));
}
TEST_F(PlayChangesAdoptionTests, GeneratedUndoFailsClosedWhenPassiveReferenceProofOmitsAnOutsideOwner) {
    AttachedScene()->CreateGameObject("Initial"); Start(); auto& generated = m_play.PlayScene()->CreateGameObject("Generated"); const auto guid = generated.instanceId;
    const auto captured = Capture(Generated(guid)); ASSERT_TRUE(captured.ok); const auto draft = Id(captured); Stop();
    ASSERT_TRUE(Invoke("prepare", draft).ok); ASSERT_TRUE(Invoke("apply", draft).ok);
    auto& outside = AttachedScene()->CreateGameObject("UninspectableRuntimeOwner"); outside.runtimeGenerated = true;
    const auto cursor = m_editUndo.GetCursor(), revision = m_editUndo.GetRevision();
    EXPECT_FALSE(m_editUndo.Undo()); EXPECT_NE(m_editUndo.LastDiagnostic().find("REFERENCE_PROOF_UNAVAILABLE"), std::string::npos);
    EXPECT_EQ(m_editUndo.GetCursor(), cursor); EXPECT_EQ(m_editUndo.GetRevision(), revision); ASSERT_NE(AttachedScene()->FindByGuid(guid), nullptr);
    outside.runtimeGenerated = false; ASSERT_TRUE(m_editUndo.Undo()) << m_editUndo.LastDiagnostic();
}
} /// @note namespace
} /// @note namespace fbzz::tests
