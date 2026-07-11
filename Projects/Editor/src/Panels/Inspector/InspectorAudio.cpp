// FBZZ Engine
// InspectorAudio.cpp | fbzz::editor
// Audio 系 Component の Inspector 描画
#include "InspectorAudio.hpp"

namespace fbzz::editor {

void DrawAudioInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType)
{
    DrawComponentSection<scene::AudioSourceComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Audio Source",
        [](scene::AudioSourceComponent& asc, EditorContext& ctx) {
            ImGui::Checkbox("Play On Awake", &asc.playOnAwake);
            ImGui::Checkbox("Loop", &asc.loop);
            widgets::AssetPathField("Clip Path", asc.clipPath, ".wav,.ogg,.mp3", ctx.projectRoot);
            ImGui::SliderFloat("Volume", &asc.volume, 0.0f, 1.0f);
        });

}


} // namespace fbzz::editor
