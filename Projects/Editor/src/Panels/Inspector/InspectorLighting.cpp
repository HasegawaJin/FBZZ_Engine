/// @file    InspectorLighting.cpp
/// @brief   Light / Camera 系 Component の Inspector 描画。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#include "InspectorLighting.hpp"

namespace fbzz::editor {

namespace {

/// 名前なしレイヤーは既定で 26 個あり、空文字のままウィジェットへ渡すと
/// ImGui の ID が全部同じになる (空ラベル = 親と同じ ID)。表示名を必ず埋める。
std::string LayerDisplayName(const ProjectSettings& settings, int layer)
{
    const std::string& name = settings.game.layerNames[layer];
    return name.empty() ? ("User Layer " + std::to_string(layer)) : name;
}

} // namespace

void DrawLightingInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType)
{
    DrawComponentSection<scene::LightComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Light",
        [go](scene::LightComponent& lc, EditorContext& c) {
            /// @note 昼夜カーブは先頭の有効な SkyRenderer が持つ (RenderSystem の採用規則と同じ)。
            bool dayNightDriven = false;
            if (c.activeScene) {
                for (auto& other : c.activeScene->GameObjects()) {
                    auto* sky = other.GetComponent<scene::SkyRenderer>();
                    if (!sky || !sky->enabled) continue;
                    dayNightDriven = sky->dayNightEnabled;
                    break;
                }
            }
            DrawLightFields(*go, lc, dayNightDriven, c.projectRoot);
        });

    DrawComponentSection<scene::CameraComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Camera",
        [](scene::CameraComponent& cc, EditorContext& c) {
            ImGui::Checkbox("Is Main", &cc.isMain);
            ImGui::DragFloat("FOV", &cc.fovY, 0.5f, 1.0f, 170.0f);
            ImGui::DragFloat("Near", &cc.nearZ, 0.001f, 0.001f, 10.0f);
            ImGui::DragFloat("Far", &cc.farZ, 1.0f, 1.0f, 10000.0f);

            const char* clearItems[] = { "Solid Color", "Depth Only" };
            int clearMode = static_cast<int>(cc.clearMode);
            if (ImGui::Combo("Clear", &clearMode, clearItems, 2))
                cc.clearMode = static_cast<renderer::CameraClearMode>(clearMode);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Solid Color: fill with Background, reset depth.\n"
                    "Depth Only: keep whatever color is already there, reset depth only.\n"
                    "  For drawing ON TOP of an earlier camera (e.g. a first-person weapon\n"
                    "  that must never clip into walls).\n"
                    "  WARNING: this engine renders one camera per view, so a lone camera\n"
                    "  set to Depth Only will show last frame's image smeared.");
            }

            ImGui::BeginDisabled(cc.clearMode != renderer::CameraClearMode::SolidColor);
            widgets::ColorEdit4("Background", cc.backgroundColor);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Color left where nothing is drawn (the HDR target clear value).\n"
                    "Hidden by the sky in scenes that render one.\n"
                    "HDR: values above 1 are allowed and will feed bloom.\n"
                    "Unused when Clear is Depth Only.");
            }

            ImGui::SeparatorText("Culling");

            const char* maskLabel = cc.cullingMask == fbzz::Layer::Everything ? "Everything"
                                  : cc.cullingMask == fbzz::Layer::Nothing    ? "Nothing"
                                  : "Mixed...";
            const bool maskComboOpen = ImGui::BeginCombo("Culling Mask", maskLabel);
            /// @note ホバー判定はコンボを開く前に取る。BeginCombo が true を返すと「直前のアイテム」が
            ///       中身のチェックボックスへ移り、EndCombo 後の IsItemHovered() は本体を指さない。
            const bool maskHovered = ImGui::IsItemHovered();
            if (maskComboOpen) {
                bool all = cc.cullingMask == fbzz::Layer::Everything;
                if (ImGui::Checkbox("Everything", &all))
                    cc.cullingMask = all ? fbzz::Layer::Everything : fbzz::Layer::Nothing;
                ImGui::Separator();
                for (int i = 0; i < scene::kCullLayerCount; ++i) {
                    ImGui::PushID(i);
                    bool on = fbzz::Layer::Contains(cc.cullingMask, i);
                    if (ImGui::Checkbox(LayerDisplayName(c.projectSettings, i).c_str(), &on)) {
                        if (on) cc.cullingMask |=  fbzz::Layer::Mask(i);
                        else    cc.cullingMask &= ~fbzz::Layer::Mask(i);
                    }
                    ImGui::PopID();
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

            /// @note 余白は「バインドポーズ球からはみ出すモーション」への実務的な逃げ道。
            ///       Inspector からしか触れないと気付けないので、意図をツールチップに残す。
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

            /// @name 距離カリング
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

            /// @note 既定で畳む (32 行分)。距離カリングはレイヤー単位 (小物 30m・建物 500m 等) が
            ///       実用的な粒度で、オブジェクトごとの設定は運用に耐えない (Unity の layerCullDistances 相当)。
            if (ImGui::TreeNode("Layer Cull Distances")) {
                ImGui::TextDisabled("0 = use Max Draw Distance");
                if (ImGui::SmallButton("Clear All")) {
                    for (int i = 0; i < scene::kCullLayerCount; ++i)
                        cc.layerCullDistances[i] = 0.0f;
                }
                ImGui::BeginChild("##layercull", ImVec2(0.0f, 220.0f), true);
                for (int i = 0; i < scene::kCullLayerCount; ++i) {
                    ImGui::PushID(i);
                    ImGui::DragFloat(LayerDisplayName(c.projectSettings, i).c_str(),
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

            /// @name 極小オブジェクトカリング
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
