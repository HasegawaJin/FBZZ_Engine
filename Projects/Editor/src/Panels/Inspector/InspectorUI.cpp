// FBZZ Engine
// InspectorUI.cpp | fbzz::editor
// UI 系 Component の Inspector 描画
#include "InspectorUI.hpp"

namespace fbzz::editor {

void DrawUIInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType)
{
    DrawComponentSection<scene::UICanvas>(go, ctx, m_componentClipboard, m_componentClipboardType, "UI Canvas",
        [go](scene::UICanvas& canvas, EditorContext& ctx) {
            if (go->GetParent())
                ImGui::TextDisabled("Only root GameObjects are rendered as canvases.");
            ImGui::DragFloat("Canvas Width",  &canvas.canvasWidth,  1.0f, 1.0f, 16384.0f);
            ImGui::DragFloat("Canvas Height", &canvas.canvasHeight, 1.0f, 1.0f, 16384.0f);
            ImGui::DragInt("Sort Order", &canvas.sortOrder);
            static constexpr const char* kModeNames[] = {
                "Screen Space Overlay",
                "World Space",
                "Screen Space Camera"
            };
            int modeIdx = static_cast<int>(canvas.renderMode);
            if (ImGui::Combo("Render Mode", &modeIdx, kModeNames, 3))
                canvas.renderMode = static_cast<scene::UIRenderMode>(modeIdx);

            if (canvas.renderMode != scene::UIRenderMode::WorldSpace) {
                static constexpr const char* kScaleNames[] = {
                    "Constant Pixel Size",
                    "Scale With Screen Size"
                };
                int scaleIdx = static_cast<int>(canvas.scaleMode);
                if (ImGui::Combo("Scale Mode", &scaleIdx, kScaleNames, 2))
                    canvas.scaleMode = static_cast<scene::UICanvasScaleMode>(scaleIdx);
                if (canvas.scaleMode == scene::UICanvasScaleMode::ScaleWithScreenSize) {
                    ImGui::DragFloat("Reference Width",  &canvas.referenceWidth,  1.0f, 1.0f, 16384.0f);
                    ImGui::DragFloat("Reference Height", &canvas.referenceHeight, 1.0f, 1.0f, 16384.0f);
                    ImGui::SliderFloat("Match Width/Height", &canvas.matchWidthOrHeight, 0.0f, 1.0f);
                }
                if (ImGui::Button("Set Active UI Canvas"))
                    ctx.activeUICanvas = go->GetID();
            }

            if (canvas.renderMode == scene::UIRenderMode::WorldSpace)
                ImGui::DragFloat("World Scale", &canvas.worldScale, 0.0001f, 0.00001f, 1.0f, "%.5f");
        });

    DrawComponentSection<scene::UIImage>(go, ctx, m_componentClipboard, m_componentClipboardType, "UI Image",
        [](scene::UIImage& image, EditorContext&) {
            // 位置・サイズは Transform で管理 (上の Transform セクションを参照)
            float color[4] = { image.color.x, image.color.y, image.color.z, image.color.w };
            if (ImGui::ColorEdit4("Color", color))
                image.color = { color[0], color[1], color[2], color[3] };
            DragVec2("UV Min", image.uvMin, 0.01f, 0.0f, 1.0f);
            DragVec2("UV Max", image.uvMax, 0.01f, 0.0f, 1.0f);
            char texBuf[512];
            std::snprintf(texBuf, sizeof(texBuf), "%s", image.texturePath.c_str());
            if (ImGui::InputText("Texture Path", texBuf, sizeof(texBuf)))
                image.texturePath = NormalizeAssetPath(texBuf);
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                    image.texturePath = NormalizeAssetPath(static_cast<const char*>(p->Data));
                }
                ImGui::EndDragDropTarget();
            }
        });

    DrawComponentSection<scene::UIButton>(go, ctx, m_componentClipboard, m_componentClipboardType, "UI Button",
        [](scene::UIButton& button, EditorContext&) {
            ImGui::Checkbox("Interactable", &button.isInteractable);
            float normal[4] = { button.normalColor.x, button.normalColor.y, button.normalColor.z, button.normalColor.w };
            if (ImGui::ColorEdit4("Normal Color", normal))
                button.normalColor = { normal[0], normal[1], normal[2], normal[3] };
            float hover[4] = { button.hoverColor.x, button.hoverColor.y, button.hoverColor.z, button.hoverColor.w };
            if (ImGui::ColorEdit4("Hover Color", hover))
                button.hoverColor = { hover[0], hover[1], hover[2], hover[3] };
            float pressed[4] = { button.pressedColor.x, button.pressedColor.y, button.pressedColor.z, button.pressedColor.w };
            if (ImGui::ColorEdit4("Pressed Color", pressed))
                button.pressedColor = { pressed[0], pressed[1], pressed[2], pressed[3] };
            static constexpr const char* kStateNames[] = { "Normal", "Hovered", "Pressed" };
            widgets::ReadOnlyText("State", kStateNames[static_cast<int>(button.state)]);
        });

    DrawComponentSection<scene::UIText>(go, ctx, m_componentClipboard, m_componentClipboardType, "UI Text",
        [](scene::UIText& text, EditorContext&) {
            // 位置は Transform で管理
            char textBuf[512];
            std::snprintf(textBuf, sizeof(textBuf), "%s", text.text.c_str());
            if (ImGui::InputText("Text", textBuf, sizeof(textBuf)))
                text.text = textBuf;
            ImGui::DragFloat("Font Size",      &text.fontSize,      1.0f, 1.0f, 512.0f);
            ImGui::DragFloat("Letter Spacing", &text.letterSpacing, 0.1f, 0.0f, 128.0f);
            float color[4] = { text.color.x, text.color.y, text.color.z, text.color.w };
            if (ImGui::ColorEdit4("Color", color))
                text.color = { color[0], color[1], color[2], color[3] };

            // Font Path: gen_font_atlas.py で生成したアトラスのベースパス (拡張子なし)。
            // 例: Assets/Fonts/Kenney/Future → Future.png + Future.fnt を参照する。
            // 空欄 = 内蔵 5x7 SDF フォント (後方互換)。
            // .png または .fnt ファイルをドロップすると拡張子を除いたパスを自動セットする。
            ImGui::TextUnformatted("Font Path");
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Base path for a generated TTF atlas, without extension.\n"
                                  "Example: Assets/Fonts/Kenney/Future\n"
                                  "Empty = built-in SDF font\n"
                                  "Drop a .png or .fnt file to assign it");
            char fontBuf[512];
            std::snprintf(fontBuf, sizeof(fontBuf), "%s", text.fontPath.c_str());
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("##fontPath", fontBuf, sizeof(fontBuf)))
                text.fontPath = fontBuf;
            // D&D: .png / .fnt をドロップしたとき拡張子を除いてベースパスにセットする。
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                    std::string dropped = NormalizeAssetPath(static_cast<const char*>(p->Data));
                    // .png / .fnt どちらをドロップしても拡張子を取り除く
                    const std::string ext4 = dropped.size() >= 4
                        ? dropped.substr(dropped.size() - 4) : "";
                    if (ext4 == ".png" || ext4 == ".fnt")
                        dropped = dropped.substr(0, dropped.size() - 4);
                    text.fontPath = dropped;
                }
                ImGui::EndDragDropTarget();
            }
        });

    DrawComponentSection<scene::UILayoutGroup>(go, ctx, m_componentClipboard, m_componentClipboardType, "UI Layout Group",
        [](scene::UILayoutGroup& layout, EditorContext&) {
            static constexpr const char* kAxisNames[] = { "Horizontal", "Vertical" };
            int axisIdx = static_cast<int>(layout.axis);
            if (ImGui::Combo("Axis", &axisIdx, kAxisNames, 2))
                layout.axis = static_cast<scene::UILayoutAxis>(axisIdx);
            ImGui::DragFloat("Spacing", &layout.spacing, 1.0f, 0.0f, 1024.0f);
            ImGui::DragFloat("Pad Left",   &layout.paddingLeft,   1.0f, 0.0f, 512.0f);
            ImGui::DragFloat("Pad Right",  &layout.paddingRight,  1.0f, 0.0f, 512.0f);
            ImGui::DragFloat("Pad Top",    &layout.paddingTop,    1.0f, 0.0f, 512.0f);
            ImGui::DragFloat("Pad Bottom", &layout.paddingBottom, 1.0f, 0.0f, 512.0f);
            ImGui::Checkbox("Reverse Order", &layout.reverseOrder);
        });

    DrawComponentSection<scene::UIAnimator>(go, ctx, m_componentClipboard, m_componentClipboardType, "UI Animator",
        [](scene::UIAnimator& anim, EditorContext&) {
            if (ImGui::TreeNodeEx("Color Tween", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox("Active##ct", &anim.colorTween.active);
                float from[4] = { anim.colorTween.from.x, anim.colorTween.from.y, anim.colorTween.from.z, anim.colorTween.from.w };
                if (ImGui::ColorEdit4("From##ct", from)) anim.colorTween.from = { from[0], from[1], from[2], from[3] };
                float to[4] = { anim.colorTween.to.x, anim.colorTween.to.y, anim.colorTween.to.z, anim.colorTween.to.w };
                if (ImGui::ColorEdit4("To##ct", to)) anim.colorTween.to = { to[0], to[1], to[2], to[3] };
                ImGui::DragFloat("Duration##ct", &anim.colorTween.duration, 0.05f, 0.01f, 60.0f);
                ImGui::Checkbox("Loop##ct",     &anim.colorTween.loop);
                ImGui::Checkbox("Ping Pong##ct",&anim.colorTween.pingPong);
                ImGui::TreePop();
            }
            if (ImGui::TreeNodeEx("Position Tween", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox("Active##pt", &anim.positionTween.active);
                DragVec2("From##pt", anim.positionTween.from, 1.0f);
                DragVec2("To##pt",   anim.positionTween.to,   1.0f);
                ImGui::DragFloat("Duration##pt", &anim.positionTween.duration, 0.05f, 0.01f, 60.0f);
                ImGui::Checkbox("Loop##pt",     &anim.positionTween.loop);
                ImGui::Checkbox("Ping Pong##pt",&anim.positionTween.pingPong);
                ImGui::TreePop();
            }
        });

}


} // namespace fbzz::editor
