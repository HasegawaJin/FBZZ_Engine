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
                widgets::AssetPathFieldWithLoad("Mesh Path", mr.meshPath, ".fzasset", ctx.projectRoot,
                    [&mr]() {
                        if (!mr.meshPath.empty())
                            if (auto* model = asset::AssetManager::LoadModel(mr.meshPath))
                                if (!model->meshes.empty())
                                    mr.mesh = model->meshes[0].get();
                    });
            }
        });

    DrawComponentSection<scene::SkinnedMeshRenderer>(go, ctx, m_componentClipboard, m_componentClipboardType, "Skinned Mesh Renderer",
        [](scene::SkinnedMeshRenderer& smr, EditorContext& ctx) {
            widgets::AssetPathFieldWithLoad("Model", smr.modelPath, ".fzasset", ctx.projectRoot,
                [&smr]() {
                    smr.model = nullptr;
                    if (!smr.modelPath.empty())
                        smr.model = asset::AssetManager::LoadModel(smr.modelPath);
                });

            if (smr.model) {
                const int meshCount = static_cast<int>(smr.model->meshes.size());
                ImGui::DragInt("Mesh Index", &smr.meshIndex, 1.0f, 0, std::max(0, meshCount - 1));
                ImGui::TextDisabled("%d mesh(es) | %s skeleton",
                    meshCount, smr.model->skeleton ? "has" : "no");

                if (!ctx.GetSelectedGO()->GetComponent<scene::MaterialComponent>()) {
                    ImGui::TextColored({ 1.0f, 0.8f, 0.2f, 1.0f }, "! Material component required");
                    if (ImGui::Button("Add Material")) {
                        if (auto* go2 = ctx.GetSelectedGO())
                            go2->AddComponent<scene::MaterialComponent>(CreateDefaultMaterialComponent(true));
                    }
                }
            }
        });

}


} // namespace fbzz::editor
