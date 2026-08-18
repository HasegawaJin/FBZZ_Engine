// FBZZ Engine
// InspectorLighting.cpp | fbzz::editor
// Light / Camera 系 Component の Inspector 描画
#include "InspectorLighting.hpp"

namespace fbzz::editor {

void DrawLightingInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType)
{
    DrawComponentSection<scene::LightComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Light",
        [go](scene::LightComponent& lc, EditorContext&) {
            DrawLightFields(*go, lc);
        });

    DrawComponentSection<scene::CameraComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Camera",
        [](scene::CameraComponent& cc, EditorContext& c) {
            ImGui::Checkbox("Is Main", &cc.isMain);
            ImGui::DragFloat("FOV", &cc.fovY, 0.5f, 1.0f, 170.0f);
            ImGui::DragFloat("Near", &cc.nearZ, 0.001f, 0.001f, 10.0f);
            ImGui::DragFloat("Far", &cc.farZ, 1.0f, 1.0f, 10000.0f);
            ImGui::SeparatorText("Culling");

            const char* maskLabel = cc.cullingMask == fbzz::Layer::Everything ? "Everything"
                                  : cc.cullingMask == fbzz::Layer::Nothing    ? "Nothing"
                                  : "Mixed...";
            const bool maskComboOpen = ImGui::BeginCombo("Culling Mask", maskLabel);
            // ホバー判定はコンボを開く前に取る。
            // WHY: BeginCombo が true を返した後は「直前のアイテム」が中身のチェックボックスへ
            //      移るため、EndCombo の後に IsItemHovered() を呼んでも本体を指さない。
            const bool maskHovered = ImGui::IsItemHovered();
            if (maskComboOpen) {
                bool all = cc.cullingMask == fbzz::Layer::Everything;
                if (ImGui::Checkbox("Everything", &all))
                    cc.cullingMask = all ? fbzz::Layer::Everything : fbzz::Layer::Nothing;
                ImGui::Separator();
                for (int i = 0; i < scene::kCullLayerCount; ++i) {
                    bool on = fbzz::Layer::Contains(cc.cullingMask, i);
                    if (ImGui::Checkbox(c.projectSettings.game.layerNames[i].c_str(), &on)) {
                        if (on) cc.cullingMask |=  fbzz::Layer::Mask(i);
                        else    cc.cullingMask &= ~fbzz::Layer::Mask(i);
                    }
                }
                ImGui::EndCombo();
            }
            if (maskHovered)
                ImGui::SetTooltip("Which layers this camera renders.");

            ImGui::Checkbox("Frustum Culling", &cc.frustumCulling);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Skip objects whose bounding sphere is outside the view frustum.\n"
                    "Turn OFF to check whether a missing object was culled or failed for\n"
                    "another reason (material / lodVisible / disabled renderer).\n"
                    "Applies to meshes, skinned meshes, terrain and water chunks.");
            }

            ImGui::Checkbox("Occlusion Culling", &cc.occlusionCulling);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "CPU software occlusion culling (128x72 linear depth buffer).\n"
                    "Occluders are approximated by bounding spheres, so the test is kept\n"
                    "conservative: it never removes a visible object, but it also rejects\n"
                    "far fewer than a real occluder-mesh rasterizer would.\n"
                    "Check the actual counts in Analysis > Rendering.");
            }

            // 余白は「バインドポーズ球からはみ出すモーション」への実務的な逃げ道。
            // Inspector からしか触れないと気付けないので、意図をツールチップに残す。
            ImGui::DragFloat("Culling Bounds Padding", &cc.cullingBoundsPadding,
                             0.05f, 0.0f, 50.0f, "%.2f m");
            if (cc.cullingBoundsPadding < 0.0f) cc.cullingBoundsPadding = 0.0f;
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Extra radius added to every bounding sphere before culling.\n"
                    "Skinned mesh bounds come from the bind pose, so poses that reach far\n"
                    "out (big swings, long weapons) can pop out at the screen edge.\n"
                    "0 = no padding. 0.5-2 m is usually enough for humanoid characters.");
            }
            if (cc.cullingBoundsPadding <= 0.0f)
                ImGui::TextDisabled("0 = use raw bounds");

            ImGui::Spacing();

            // ---- 距離カリング ----
            ImGui::DragFloat("Max Draw Distance", &cc.maxDrawDistance,
                             1.0f, 0.0f, 100000.0f, "%.0f m");
            if (cc.maxDrawDistance < 0.0f) cc.maxDrawDistance = 0.0f;
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Stop drawing objects past this distance. 0 = draw up to Far.\n"
                    "Kept separate from Far on purpose: shrinking Far changes how depth\n"
                    "precision is distributed and moves Z-fighting and shadow quality with it.\n"
                    "This only skips draws, the projection matrix is untouched.");
            }
            if (cc.maxDrawDistance > cc.farZ) {
                ImGui::TextColored({ 1.0f, 0.75f, 0.35f, 1.0f },
                                   "Beyond Far (%.0f m) - the frustum culls first.", cc.farZ);
            }

            int distanceMode = cc.cullDistanceSpherical ? 0 : 1;
            if (ImGui::Combo("Distance Mode", &distanceMode, "Spherical\0Planar (depth)\0"))
                cc.cullDistanceSpherical = (distanceMode == 0);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Spherical: distance from the camera position. Turning the camera never\n"
                    "changes which objects are inside the range.\n"
                    "Planar: distance along the view direction. Objects near the screen edge\n"
                    "survive further out, but panning the camera can pop them in and out.");
            }

            // レイヤー別の距離は 32 行あるので既定で畳んでおく。
            // WHY 個別に持てるようにするか: 「小物は 30m、建物は 500m」が距離カリングの実用形で、
            //      オブジェクトごとに設定して回るのは現実的でない (Unity の layerCullDistances 相当)。
            if (ImGui::TreeNode("Layer Cull Distances")) {
                ImGui::TextDisabled("0 = use Max Draw Distance");
                if (ImGui::SmallButton("Clear All")) {
                    for (int i = 0; i < scene::kCullLayerCount; ++i)
                        cc.layerCullDistances[i] = 0.0f;
                }
                ImGui::BeginChild("##layercull", ImVec2(0.0f, 220.0f), true);
                for (int i = 0; i < scene::kCullLayerCount; ++i) {
                    ImGui::PushID(i);
                    ImGui::DragFloat(c.projectSettings.game.layerNames[i].c_str(),
                                     &cc.layerCullDistances[i],
                                     1.0f, 0.0f, 100000.0f,
                                     cc.layerCullDistances[i] > 0.0f ? "%.0f m" : "default");
                    if (cc.layerCullDistances[i] < 0.0f) cc.layerCullDistances[i] = 0.0f;
                    ImGui::PopID();
                }
                ImGui::EndChild();
                ImGui::TreePop();
            }

            ImGui::Spacing();

            // ---- 極小オブジェクトカリング ----
            ImGui::DragFloat("Small Object Culling", &cc.smallObjectScreenHeight,
                             0.0005f, 0.0f, 0.5f, "%.4f");
            if (cc.smallObjectScreenHeight < 0.0f) cc.smallObjectScreenHeight = 0.0f;
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Skip objects whose bounding sphere covers less than this fraction of the\n"
                    "screen height. Same unit as LOD Group's Screen Relative Height.\n"
                    "LOD Groups only affect the objects you set them up on; this is the\n"
                    "scene-wide safety net that stops un-LODed assets from issuing draw calls\n"
                    "for sub-pixel geometry in the distance.\n"
                    "0 = disabled. 0.002-0.01 is a usual starting range.");
            }
            if (cc.smallObjectScreenHeight > 0.0f) {
                ImGui::TextDisabled("= %.2f%% of screen height",
                                    cc.smallObjectScreenHeight * 100.0f);
            } else {
                ImGui::TextDisabled("0 = disabled");
            }
        });

}


} // namespace fbzz::editor
