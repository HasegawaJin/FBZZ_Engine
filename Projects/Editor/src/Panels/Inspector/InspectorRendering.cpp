// FBZZ Engine
// InspectorRendering.cpp | fbzz::editor
// Rendering 系 Component の Inspector 描画
#include "InspectorRendering.hpp"

namespace fbzz::editor {

void DrawRenderingInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType)
{
    DrawComponentSection<scene::MeshRenderer>(go, ctx, m_componentClipboard, m_componentClipboardType, "Mesh Renderer",
        [](scene::MeshRenderer& mr, EditorContext& ctx) {
            static constexpr const char* kPrimitiveNames[] = {
                "Custom", "Cube", "Sphere", "Plane", "Quad", "Cylinder", "Cone", "Torus", "Capsule"
            };
            static constexpr const char* kPrimitivePaths[] = {
                "", "primitive:cube", "primitive:sphere", "primitive:plane",
                "primitive:quad", "primitive:cylinder", "primitive:cone", "primitive:torus", "primitive:capsule"
            };
            constexpr int kPrimitiveCount = 9;

            int sel = 0;
            for (int i = 1; i < kPrimitiveCount; ++i)
                if (mr.meshPath == kPrimitivePaths[i]) { sel = i; break; }

            if (ImGui::Combo("Mesh", &sel, kPrimitiveNames, kPrimitiveCount)) {
                if (sel > 0) {
                    mr.meshPath = kPrimitivePaths[sel];
                    if (auto* res = renderer::ResourceManager::Active()) {
                        switch (sel) {
                        case 1: mr.mesh = renderer::PrimitiveMesh::Cube(*res);         break;
                        case 2: mr.mesh = renderer::PrimitiveMesh::Sphere(*res);       break;
                        case 3: mr.mesh = renderer::PrimitiveMesh::Plane(*res);        break;
                        case 4: mr.mesh = renderer::PrimitiveMesh::Quad(*res);         break;
                        case 5: mr.mesh = renderer::PrimitiveMesh::Cylinder(*res);     break;
                        case 6: mr.mesh = renderer::PrimitiveMesh::Cone(*res);         break;
                        case 7: mr.mesh = renderer::PrimitiveMesh::Torus(*res);        break;
                        case 8: mr.mesh = renderer::PrimitiveMesh::Capsule(*res);      break;
                        default: break;
                        }
                    }
                } else {
                    mr.meshPath.clear();
                    mr.mesh = nullptr;
                }
            }

            if (sel == 0) {
                widgets::AssetPathFieldWithLoad("Mesh Path", mr.meshPath, ".fbx", ctx.projectRoot,
                    [&mr]() {
                        if (!mr.meshPath.empty())
                            if (auto* model = asset::AssetManager::LoadModel(mr.meshPath))
                                if (!model->meshes.empty())
                                    mr.mesh = model->meshes[0].get();
                    });
            }

            ImGui::Checkbox("Cast Shadows", &mr.castShadows);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "ShadowPass はシーンを光源視点でもう一度描く。\n"
                    "影が絵に出ないオブジェクト (小物・天井裏・遠景) を外すと、\n"
                    "見た目を変えずにシャドウ描画量をそのぶん減らせる。");
        });

    DrawComponentSection<scene::SkinnedMeshRenderer>(go, ctx, m_componentClipboard, m_componentClipboardType, "Skinned Mesh Renderer",
        [](scene::SkinnedMeshRenderer& smr, EditorContext& ctx) {
            widgets::AssetPathFieldWithLoad("Model", smr.modelPath, ".fbx", ctx.projectRoot,
                [&smr]() {
                    smr.model = nullptr;
                    if (!smr.modelPath.empty())
                        smr.model = asset::AssetManager::LoadModel(smr.modelPath);
                });

            if (smr.model) {
                // WHY: submesh の指定 UI は持たない。1 GameObject = モデル全体を描き、
                //      submesh ごとの見た目は Material コンポーネントのスロットで決める。
                const size_t meshCount = smr.model->meshes.size();
                ImGui::TextDisabled("%zu submesh(es) | %s skeleton",
                    meshCount, smr.model->skeleton ? "has" : "no");

                auto* selected = ctx.GetSelectedGO();
                auto* mat = selected ? selected->GetComponent<scene::MaterialComponent>() : nullptr;
                if (!mat) {
                    ImGui::TextColored({ 1.0f, 0.8f, 0.2f, 1.0f }, "! Material component required");
                    if (ImGui::Button("Add Material") && selected)
                        selected->AddComponent<scene::MaterialComponent>(CreateDefaultMaterialComponent(true));
                } else if (mat->SlotCount() != meshCount) {
                    // スロット数と submesh 数がずれていても描画は主マテリアルへフォールバック
                    // するので壊れないが、submesh ごとに .mat を割り当てたい場合に備えて促す。
                    ImGui::TextColored({ 1.0f, 0.8f, 0.2f, 1.0f },
                        "Material slots: %zu / %zu submesh", mat->SlotCount(), meshCount);
                    if (ImGui::Button("Match Material Slots")) {
                        mat->ResizeSlots(meshCount);
                        if (ctx.markSceneDirty) ctx.markSceneDirty();
                    }
                }
            }

            ImGui::Checkbox("Cast Shadows", &smr.castShadows);
        });

    DrawComponentSection<scene::LODGroupComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "LOD Group",
        [](scene::LODGroupComponent& group, EditorContext& ctx) {
            ImGui::DragFloat("Size", &group.size, 0.05f, 0.001f, 100000.0f);
            ImGui::Checkbox("Cull Below Last LOD", &group.cullBelowLastLevel);

            int removeLevel = -1;
            for (size_t levelIndex = 0; levelIndex < group.levels.size(); ++levelIndex) {
                auto& level = group.levels[levelIndex];
                ImGui::PushID(static_cast<int>(levelIndex));
                const std::string label = "LOD " + std::to_string(levelIndex);
                if (ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    widgets::RangeField("Screen Relative Height", level.screenRelativeHeight, 0.0f, 1.0f);

                    int removeRenderer = -1;
                    for (size_t rendererIndex = 0; rendererIndex < level.renderers.size(); ++rendererIndex) {
                        auto& reference = level.renderers[rendererIndex];
                        const scene::GameObject* rendererGo = ctx.activeScene
                            ? ctx.activeScene->FindByGuid(reference.instanceId) : nullptr;
                        ImGui::PushID(static_cast<int>(rendererIndex));
                        ImGui::TextUnformatted(rendererGo ? rendererGo->name.c_str() : "Missing Renderer");
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove")) removeRenderer = static_cast<int>(rendererIndex);
                        ImGui::PopID();
                    }
                    if (removeRenderer >= 0) {
                        level.renderers.erase(level.renderers.begin() + removeRenderer);
                        if (ctx.markSceneDirty) ctx.markSceneDirty();
                    }

                    ImGui::Button("Drop Renderer Here", { -1.0f, 0.0f });
                    if (scene::GameObject* dropped = AcceptHierarchyDrop(ctx.activeScene)) {
                        if (dropped->GetComponent<scene::MeshRenderer>() ||
                            dropped->GetComponent<scene::SkinnedMeshRenderer>()) {
                            scene::LODRendererReference reference{};
                            reference.instanceId = dropped->instanceId;
                            reference.entity = dropped->GetID();
                            level.renderers.push_back(std::move(reference));
                            if (ctx.markSceneDirty) ctx.markSceneDirty();
                        }
                    }

                    if (ImGui::SmallButton("Remove LOD")) removeLevel = static_cast<int>(levelIndex);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            if (removeLevel >= 0) {
                group.levels.erase(group.levels.begin() + removeLevel);
                if (ctx.markSceneDirty) ctx.markSceneDirty();
            }

            if (ImGui::Button("Add LOD")) {
                scene::LODLevel level{};
                level.screenRelativeHeight = group.levels.empty()
                    ? 0.5f : group.levels.back().screenRelativeHeight * 0.5f;
                group.levels.push_back(std::move(level));
                if (ctx.markSceneDirty) ctx.markSceneDirty();
            }
        });

}


} // namespace fbzz::editor
