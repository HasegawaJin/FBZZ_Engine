/// @file    InspectorRendering.cpp
/// @brief   Rendering 系 Component の Inspector 描画。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#include "InspectorRendering.hpp"

#include <Engine/Scene/Components/ProceduralMeshComponent.hpp>

namespace fbzz::editor {

namespace {

/// @brief マテリアルの状況を 1 行で示す。割り当てと編集は Material コンポーネント側。
/// @note 閲覧専用の一覧をここにも出すと、編集可能な Material 側の一覧と重複し、どちらを
///       触ればよいか読めなくなる。スロット配列の実体は MaterialComponent が持つため、
///       Unity の Renderer.materials は真似ない。ここは Material コンポーネントが無い場合の導線だけ。
void DrawRendererMaterialStatus(EditorContext& ctx, size_t submeshCount, bool skinned)
{
    scene::GameObject* go = ctx.GetSelectedGO();
    if (!go || submeshCount == 0) return;

    auto* mc = go->GetComponent<scene::MaterialComponent>();
    if (!mc) {
        ImGui::SeparatorText("Materials");
        ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning),
                           "! Material component required");
        if (ImGui::Button("Add Material"))
            go->AddComponent<scene::MaterialComponent>(CreateDefaultMaterialComponent(skinned));
        return;
    }

    ImGui::TextDisabled("%zu material slot(s) — Material コンポーネントで割り当てます",
                        mc->SlotCount());
}

} // namespace

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
                            if (auto* model = asset::AssetManager::LoadAndGet<asset::Model>(mr.meshPath))
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

            DrawRendererMaterialStatus(ctx, mr.mesh ? 1u : 0u, false);
        });

    DrawComponentSection<scene::SkinnedMeshRenderer>(go, ctx, m_componentClipboard, m_componentClipboardType, "Skinned Mesh Renderer",
        [](scene::SkinnedMeshRenderer& smr, EditorContext& ctx) {
            widgets::AssetPathFieldWithLoad("Model", smr.modelPath, ".fbx", ctx.projectRoot,
                [&smr]() {
                    smr.model = nullptr;
                    if (!smr.modelPath.empty())
                        smr.model = asset::AssetManager::LoadAndGet<asset::Model>(smr.modelPath);
                });

            if (smr.model) {
                /// @note submesh の指定 UI は持たない。担当は FBX のノード構造から配置時に決まる (submeshIndices 参照)。
                const size_t meshCount = smr.SubmeshCount();
                ImGui::TextDisabled("%zu submesh(es) of %zu | %s skeleton",
                    meshCount, smr.model->meshes.size(),
                    smr.model->skeleton ? "has" : "no");

                DrawRendererMaterialStatus(ctx, meshCount, true);
            }

            ImGui::Checkbox("Cast Shadows", &smr.castShadows);
        });

    DrawComponentSection<scene::LODGroupComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "LOD Group",
        [](scene::LODGroupComponent& group, EditorContext& ctx) {
            ImGui::DragFloat("Size", &group.size, 0.05f, 0.001f, 100000.0f);
            ImGui::Checkbox("Cull Below Last LOD", &group.cullBelowLastLevel);
            ImGui::DragFloat("Fade Duration", &group.fadeDuration, 0.01f, 0.0f, 2.0f, "%.2f s");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("LOD を入れ替えるときのディザクロスフェード時間。0 で即差し替え");

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

    /// @note 形の正本はスクリプト (mesh.Apply) で保存もされないので、中身は読み取り専用で出す。
    DrawComponentSection<scene::ProceduralMeshComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Procedural Mesh",
        [](scene::ProceduralMeshComponent& procedural, EditorContext&) {
            const uint32_t vertexCount = procedural.builder.VertexCount();
            const uint32_t indexCount  = procedural.builder.IndexCount();
            ImGui::Text("Vertices: %u   Indices: %u   Triangles: %u",
                        vertexCount, indexCount, indexCount / 3u);
            ImGui::Text("GPU Mesh: %s", procedural.runtimeMesh.HasMesh() ? "Uploaded" : "Not uploaded");
            ImGui::Text("Material: %s",
                        procedural.materialPath.empty() ? "(left to Material component)"
                                                        : procedural.materialPath.c_str());
            ImGui::TextDisabled("Built by script (mesh.Apply) at runtime. Not saved with the scene.");
        });
}


} // namespace fbzz::editor
