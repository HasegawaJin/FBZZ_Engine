/// @file    PlayModeController.cpp
/// @brief   Validated Play clones leave original Edit instances and their physics/runtime ownership intact.
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/Util/PlaySettingsBirthTable.hpp>
#include <Editor/Util/PlaySettingsAdoption.hpp>
#include <Editor/Util/PlayChangesAdoption.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/ProjectRuntime.hpp>
#include <Engine/Scene/ControlPointAdapters.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Core/Cursor.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Input/Input.hpp>
#include <Engine/Input/InputActionMap.hpp>
#include <Engine/Profiler/ScriptProfiler.hpp>
#include <algorithm>
#include <limits>
namespace fbzz::editor {
PlayModeController::PlayModeController() = default;
PlayModeController::~PlayModeController() { Cancel(); m_adoptionDrafts.clear(); }
void PlayModeController::Play(scene::Scene& scene, const std::string& sourcePath, renderer::ResourceManager* resources)
{
    if (m_state != PlayState::Editor || m_restorePending) return;
    m_diagnostic.clear();
    if (scene.IsExecutionParked()) { m_diagnostic = "Edit Scene is already parked by another Play session."; return; }
    scene::FlushWorldTransforms(scene);
    if (!scene::ValidateStaticControlPointBakes(scene)) {
        m_diagnostic = "制御点の静的ベイクが古いか不正です。ベイクを更新してからPlayしてください。";
        return;
    }
    auto createClone = scene::SceneSerializer::PreparePlayClone(scene, resources, sourcePath, m_diagnostic);
    if (!createClone) { FBZZ_LOG_ERROR("Play clone capture failed: %s", m_diagnostic.c_str()); return; }
    struct ParkGuard {
        scene::Scene& scene; bool transferred = false;
        ~ParkGuard() { if (!transferred) scene.SetExecutionParked(false); }
    } park{scene};
    scene.SetExecutionParked(true);
    auto& managed = scene::ManagedScriptRuntime::Instance();
    const uint64_t generation = managed.IsLoaded() ? managed.RetainAppliedGeneration() : 0;
    if (managed.IsLoaded() && !generation) { m_diagnostic = managed.LastDiagnostic(); return; }
    const auto rollbackGeneration = [&managed, generation] {
        if (!generation) return;
        (void)managed.ActivateRetainedGeneration(generation);
        managed.ReleaseRetainedGeneration(generation);
    };
    if (generation && !managed.ForkRetainedGenerationForPlay(generation)) {
        m_diagnostic = managed.LastDiagnostic(); rollbackGeneration(); return;
    }
    auto clone = createClone();
    if (!clone) { rollbackGeneration(); m_diagnostic = "Play clone construction failed; Edit objects are unchanged."; return; }
    scene::FlushWorldTransforms(*clone);
    if (!scene::ValidateStaticControlPointBakes(*clone)) {
        clone.reset(); rollbackGeneration();
        m_diagnostic = "Play用Sceneの制御点ベイクを検証できませんでした。";
        return;
    }
    /// @note Serialization callbacks run only on the detached clone, never on parked Edit instances.
    if (!scene::SceneSerializer::ValidatePlayClone(*clone, m_diagnostic)) {
        clone.reset(); rollbackGeneration();
        FBZZ_LOG_ERROR("Play clone validation failed: %s", m_diagnostic.c_str()); return;
    }
    std::shared_ptr<PlaySettingsBirthTable> births;
    if (generation) {
        births = std::make_shared<PlaySettingsBirthTable>();
        if (!births->Capture(scene, *clone, m_diagnostic)) { clone.reset(); rollbackGeneration(); return; }
    }
    auto changes = PlayChangesAdoptionState::Begin(scene, *clone, m_diagnostic);
    if (!changes) { clone.reset(); rollbackGeneration(); return; }
    m_changesAdoption = std::move(changes);
    m_adoptionBirths = std::move(births);
    m_editScene = &scene; m_playScene = std::move(clone);
    park.transferred = true;
    m_playRuntime = std::make_unique<scene::ProjectRuntime>(); m_playRuntime->BindExternalScene(m_playScene.get());
    m_temporaryUndo = std::make_unique<UndoStack>(); m_temporarySettingsChanged = false;
    m_parkedGeneration = generation; m_state = PlayState::Playing;
    scene::ScriptProfiler::SetExecutionMode(true); input::Input::Reset(); input::InputActionMap::SetEnabled(true);
}
void PlayModeController::Pause()
{
    if (m_state == PlayState::Playing) m_state = PlayState::Paused;
    else if (m_state == PlayState::Paused) m_state = PlayState::Playing;
}
bool PlayModeController::AddAdoptionDraft(std::shared_ptr<PlaySettingsAdoptionDraft> draft)
{
    if (!draft || m_adoptionIdsExhausted || m_adoptionDrafts.size() >= 256) return false;
    draft->id = m_nextAdoptionId;
    if (m_nextAdoptionId == std::numeric_limits<uint64_t>::max()) m_adoptionIdsExhausted = true;
    else ++m_nextAdoptionId;
    m_adoptionDrafts.push_back(std::move(draft)); return true;
}
std::shared_ptr<PlaySettingsAdoptionDraft> PlayModeController::FindAdoptionDraft(uint64_t id) const
{
    const auto found = std::find_if(m_adoptionDrafts.begin(), m_adoptionDrafts.end(), [id](const auto& draft) { return draft->id == id; });
    return found == m_adoptionDrafts.end() ? nullptr : *found;
}
void PlayModeController::DiscardAdoptionDraft(uint64_t id)
{
    std::erase_if(m_adoptionDrafts, [id](const auto& draft) { return draft->id == id; });
}
bool PlayModeController::CanEditTemporaryScene(const scene::Scene* scene) const
{
    return m_state != PlayState::Editor && !m_restorePending && m_playRuntime &&
        m_playRuntime->GetActiveScene() == scene && m_temporaryUndo && m_temporaryUndo->IsRecordingEnabled();
}
void PlayModeController::Stop(scene::Scene& scene)
{
    (void)scene;
    if (m_state == PlayState::Editor) return;
    core::Cursor::ResetForEditor(); input::InputActionMap::SetEnabled(false); m_restorePending = true;
}
bool PlayModeController::ApplyPendingRestore(scene::Scene& scene)
{
    if (!m_restorePending) return false;
    if (&scene != m_editScene) { m_diagnostic = "Stop target must be the parked original Edit Scene."; return false; }
    if (m_changesAdoption) m_changesAdoption->Finish(true);
    Cancel(); return true;
}
void PlayModeController::Cancel()
{
    if (m_state == PlayState::Editor && !m_playScene && !m_playRuntime && !m_parkedGeneration && !m_restorePending) return;
    if (m_changesAdoption) m_changesAdoption->Finish(false);
    /// @note Commands retain exact Play instances/generation tokens; release every command before callbacks and clone destruction.
    m_temporaryUndo.reset(); m_temporarySettingsChanged = false;
    if (m_clearTemporarySettings) m_clearTemporarySettings();
    m_clearTemporarySettings = {};
    if (m_playRuntime) m_playRuntime->Shutdown();
    m_playScene.reset(); m_playRuntime.reset();
    if (m_editScene) m_editScene->SetExecutionParked(false);
    m_state = PlayState::Editor; m_restorePending = false; m_stepRequested = false; m_editScene = nullptr;
    auto& managed = scene::ManagedScriptRuntime::Instance();
    if (m_parkedGeneration) {
        if (!managed.ActivateRetainedGeneration(m_parkedGeneration)) {
            m_diagnostic = "Original Edit objects restored; managed catalog activation failed: " + managed.LastDiagnostic();
            FBZZ_LOG_ERROR("%s", m_diagnostic.c_str());
        }
        managed.ReleaseRetainedGeneration(m_parkedGeneration); m_parkedGeneration = 0;
    }
    scene::ScriptProfiler::SetExecutionMode(false); input::Input::Reset(); core::Cursor::ResetForEditor();
}
} /// @note namespace fbzz::editor
