/// @file    InspectorRendering.cpp
/// @brief   Rendering 系 Component の Inspector 描画。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#include "InspectorRendering.hpp"

namespace fbzz::editor {

namespace {

// マテリアルの状況を 1 行で示す。割り当てと編集は Material コンポーネント側。
//
// WHY 一覧をここへ置かないか (重要):
//   以前は Renderer が「閲覧専用の Element 一覧」を、Material が「編集可能な
//   Element 一覧」を別々に出していた。同じ情報が 2 か所に並ぶため、どちらを触れば
//   よいのか、なぜ Element 0 だけ扱いが違うのかが読めない UI になっていた。
//
//   Unity は Renderer に materials 配列を出すが、あれは Unity に Material
//   コンポーネントが存在しないからである。このエンジンではスロット配列の実体を
//   MaterialComponent が持っているため、配置だけ真似ると「Renderer のセクションで
//   編集しているのに、Undo トラッカーが見ているのは MaterialComponent ではない」
//   というズレが生まれ、回避コードが必要になる。編集 UI はデータの持ち主へ置く。
//
//   ここに残すのは、Material コンポーネントが必要なのに無い場合の導線だけ。
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

            DrawRendererMaterialStatus(ctx, mr.mesh ? 1u : 0u, false);
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
                // WHY: submesh の指定 UI は持たない。担当 submesh は FBX のノード構造から
                //      配置時に決まる構造的な事実で、ユーザーが手で打つ値ではない
                //      (SkinnedMeshRenderer::submeshIndices のコメント参照)。
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

}


} // namespace fbzz::editor
