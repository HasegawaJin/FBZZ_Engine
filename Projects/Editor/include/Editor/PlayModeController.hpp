/// @file    PlayModeController.hpp
/// @brief   Separate Play ownership parks original Edit objects, runtime and Undo identities.
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <memory>
#include <string>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>
namespace fbzz::scene { class Scene; class ProjectRuntime; }
namespace fbzz::renderer { class ResourceManager; }
namespace fbzz::editor {
class UndoStack;
class PlaySettingsBirthTable;
class PlayChangesAdoptionState;
struct PlaySettingsAdoptionDraft;
enum class PlayState { Editor, Playing, Paused };
class PlayModeController {
public:
    PlayModeController();
    ~PlayModeController();
    /// @note Builds and validates a separate clone before switching state; failures leave original Edit instances unchanged.
    void Play(scene::Scene& scene, const std::string& sourcePath = {}, renderer::ResourceManager* resources = nullptr);
    void Pause();
    void Stop(scene::Scene& scene);
    /// @note Safe-point disposal of Play ownership; the original Scene is never deserialized or recreated.
    bool ApplyPendingRestore(scene::Scene& scene);
    /// @note Shutdown/project switch can dispose Play ownership before unloading script modules.
    void Cancel();
    void RequestStep() { if (m_state == PlayState::Paused) m_stepRequested = true; }
    bool ConsumeStep() { if (!m_stepRequested) return false; m_stepRequested = false; return true; }
    PlayState GetState() const { return m_state; }
    bool IsPlaying() const { return m_state == PlayState::Playing; }
    bool IsPaused() const { return m_state == PlayState::Paused; }
    bool IsInEditor() const { return m_state == PlayState::Editor; }
    bool HasPendingRestore() const { return m_restorePending; }
    scene::Scene* PlayScene() const { return m_playScene.get(); }
    scene::ProjectRuntime* PlayRuntime() const { return m_playRuntime.get(); }
    scene::Scene* ParkedEditScene() const { return m_editScene; }
    uint64_t ParkedGeneration() const { return m_parkedGeneration; }
    const std::string& Diagnostic() const { return m_diagnostic; }
    UndoStack* TemporaryUndo() const { return m_temporaryUndo.get(); }
    bool CanEditTemporaryScene(const scene::Scene* scene) const;
    void MarkTemporarySettingsChanged() { m_temporarySettingsChanged = true; }
    /// @note Tracks authoring activity; arbitrary callback field writes require explicit observation/capture.
    bool TemporarySettingsChanged() const { return m_temporarySettingsChanged; }
    void SetTemporarySettingsCleanup(std::function<void()> cleanup) { m_clearTemporarySettings = std::move(cleanup); }
    std::shared_ptr<const PlaySettingsBirthTable> AdoptionBirths() const { return m_adoptionBirths; }
    std::shared_ptr<PlayChangesAdoptionState> ChangesAdoption() const { return m_changesAdoption; }
    const std::vector<std::shared_ptr<PlaySettingsAdoptionDraft>>& AdoptionDrafts() const { return m_adoptionDrafts; }
    bool AddAdoptionDraft(std::shared_ptr<PlaySettingsAdoptionDraft> draft);
    std::shared_ptr<PlaySettingsAdoptionDraft> FindAdoptionDraft(uint64_t id) const;
    void DiscardAdoptionDraft(uint64_t id);
private:
    PlayState m_state = PlayState::Editor;
    scene::Scene* m_editScene = nullptr;
    std::unique_ptr<scene::Scene> m_playScene;
    std::unique_ptr<scene::ProjectRuntime> m_playRuntime;
    std::unique_ptr<UndoStack> m_temporaryUndo;
    bool m_temporarySettingsChanged = false;
    std::function<void()> m_clearTemporarySettings;
    std::shared_ptr<PlaySettingsBirthTable> m_adoptionBirths;
    std::shared_ptr<PlayChangesAdoptionState> m_changesAdoption;
    std::vector<std::shared_ptr<PlaySettingsAdoptionDraft>> m_adoptionDrafts;
    uint64_t m_nextAdoptionId = 1;
    bool m_adoptionIdsExhausted = false;
    uint64_t m_parkedGeneration = 0;
    std::string m_diagnostic;
    bool m_restorePending = false;
    bool m_stepRequested = false;
};
} /// @note namespace fbzz::editor
