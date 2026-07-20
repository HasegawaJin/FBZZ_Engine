// FBZZ Engine
// VFXEditorApp.hpp | fbzz::editor
// 独立VFX制作Applicationのライフサイクルと専用Preview World
#pragma once

#include <Editor/EditorContext.hpp>
#include <Editor/Panels/VFXEditorPanel.hpp>
#include <Engine/Core/IModule.hpp>
#include <Engine/Renderer/DebugCamera.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/SceneManager.hpp>
#include <Physics/World.hpp>
#include <memory>
#include <string>

namespace fbzz::core { class Window; }
namespace fbzz::renderer { class IImGuiRenderer; class IRenderer; class ResourceManager; }
namespace fbzz::editor::ai { class EditorBusDispatcher; class NamedPipeServer; }

namespace fbzz::editor {

// VFXアセット編集だけを担い、ゲームSceneを一切ロードしない独立Application。
// WHY: Preview Entity、Undo、選択をEditor Sceneからプロセス境界で隔離し、誤配置を構造的に防止する。
class VFXEditorApp final : public core::IModule {
public:
    VFXEditorApp() = default;
    ~VFXEditorApp() override;

    [[nodiscard]] bool Init(renderer::IRenderer& renderer,
                            renderer::IImGuiRenderer& imguiRenderer,
                            renderer::ResourceManager& resources,
                            core::Window& window,
                            const std::string& projectRoot,
                            const std::string& projectSettingsPath,
                            const std::string& initialAssetPath);

    [[nodiscard]] bool OnInit() override;
    void OnUpdate(float dt) override;
    void OnLateUpdate(float dt) override;
    void OnRender() override;
    void OnShutdown() override;

private:
    void OpenGraphDialog();
    [[nodiscard]] bool ConfirmDocumentSwitch();
    std::string HandleIpcRequest(const std::string& request);
    void ResizePreviewIfNeeded();
    void RenderPreview();
    void RenderAiPreview();

    renderer::IRenderer* m_renderer = nullptr;
    renderer::IImGuiRenderer* m_imguiRenderer = nullptr;
    renderer::ResourceManager* m_resources = nullptr;
    core::Window* m_window = nullptr;
    EditorContext m_context;
    EditorContext m_aiContext;
    VFXEditorPanel m_panel;
    std::unique_ptr<scene::Scene> m_previewScene;
    scene::SceneManager m_previewSceneManager;
    physics::World m_previewPhysicsWorld;
    std::unique_ptr<scene::Scene> m_aiPreviewScene;
    scene::SceneManager m_aiPreviewSceneManager;
    physics::World m_aiPreviewPhysicsWorld;
    renderer::DebugCamera m_previewCamera;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_previewRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_aiPreviewRT;
    std::string m_imguiIniPath;
    std::unique_ptr<ai::EditorBusDispatcher> m_aiDispatcher;
    std::unique_ptr<ai::NamedPipeServer> m_aiPipeServer;
};

} // namespace fbzz::editor
