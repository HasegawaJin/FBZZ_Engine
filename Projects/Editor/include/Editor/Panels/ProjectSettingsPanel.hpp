// FBZZ Engine
// ProjectSettingsPanel.hpp | fbzz::editor
// タグ・レイヤー名の編集パネル
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <cstdint>

namespace fbzz { struct ProjectSettings; }
namespace fbzz::renderer { struct RenderSettings; }

namespace fbzz::editor {

class ProjectSettingsPanel final : public IPanel {
public:
    enum class Section {
        Application,
        Import,
        Render,
        PostProcess,
        Physics,
        Audio,
        Screen,
        Tags,
        Layers
    };

    const char* GetWindowName()        const override { return "Project Settings"; }
    bool        GetDefaultVisibility() const override { return false; }

protected:
    void OnRenderContent(EditorContext& ctx) override;

private:
    void DrawSidebar();
    void DrawSection(EditorContext& ctx);
    void DrawApplication(ProjectSettings& settings);
    void DrawImport(EditorContext& ctx);
    void DrawRender(renderer::RenderSettings& render);
    void DrawPostProcess(renderer::RenderSettings& render);
    void DrawPhysics(ProjectSettings& settings);
    void DrawAudio(ProjectSettings& settings);
    void DrawScreen(ProjectSettings& settings);
    void DrawTags(ProjectSettings& settings);
    void DrawLayers(ProjectSettings& settings);

    Section m_currentSection = Section::Render;
    char m_newTag[64] = {};
    std::uint64_t m_editGeneration = 0;
};

} // namespace fbzz::editor
