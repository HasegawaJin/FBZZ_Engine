// FBZZ Engine
// InspectorRendering.cpp | fbzz::editor
// Rendering 系 Component の Inspector 描画
#include "InspectorRendering.hpp"

namespace fbzz::editor {

namespace {

// "Element N" ラベル列の幅。行が増えても .mat 名の開始位置を揃えるための固定値。
constexpr float kMaterialElementLabelWidth = 96.0f;

// Renderer が描く submesh 1 つぶんのマテリアル参照を 1 行で見せる (表示専用)。
// WHY: 参照先を「読む」ための行なので、編集ウィジェット (AssetPathField) は置かない。
//      代わりに行そのものをクリック対象にして、Asset Browser 側の .mat 実体へ飛べるようにする。
//      未割当・ファイル欠落・Visible OFF は色と注記で区別する — 一覧の役目は
//      「この Renderer が結局どのマテリアルで描かれるのか」を 1 画面で確定させること。
void DrawMaterialOverviewRow(size_t index, scene::MaterialComponent& mc)
{
    const scene::MaterialSlot& assigned = mc.RawSlotAt(index);
    scene::MaterialSlot&       drawn    = mc.SlotAt(index);
    drawn.EnsureMaterialAsset();

    // SlotAt は未割当スロットを主スロット (Element 0) へフォールバックさせる。
    // 「割り当てが無いのに描かれている」状態を隠さないよう、その旨を明示する。
    const bool usesFallback = assigned.materialPath.empty() && !drawn.materialPath.empty();
    const std::string& path = drawn.materialPath;

    ImGui::PushID(static_cast<int>(index));
    ImGui::TextDisabled("Element %zu", index);
    ImGui::SameLine(kMaterialElementLabelWidth);

    if (path.empty()) {
        ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning), "None (未割当)");
        ImGui::PopID();
        return;
    }

    const std::string name = util::FileSystem::PathToUtf8(
        util::FileSystem::PathFromUtf8(path).stem());
    const bool missing = !drawn.materialAsset.IsValid();
    const bool dimmed  = usesFallback || !assigned.visible;

    // WHY Selectable / Button を使わないか: それらは押下中 ImGui の ActiveID を握る。
    //      この一覧は DrawGenericUndoableComponentBody の内側で描かれ、あちらは
    //      「ActiveID が動いた = コンポーネントを編集した」とみなして Undo を積むため、
    //      ただ参照先を見に行っただけで中身の変わらない履歴が残ってしまう。
    //      テキスト + ホバー判定なら ActiveID に触れずにクリックだけを拾える。
    ImGui::TextColored(
        missing ? EditorTheme::Color(ThemeColor::Danger)
                : EditorTheme::Color(dimmed ? ThemeColor::TextMuted : ThemeColor::Text),
        "%s", name.c_str());
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        // 下線とカーソル変化だけで「押せる」ことを示す (行の地色は塗らない)。
        const ImVec2 rectMin = ImGui::GetItemRectMin();
        const ImVec2 rectMax = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(
            { rectMin.x, rectMax.y - 1.0f }, { rectMax.x, rectMax.y - 1.0f },
            EditorTheme::ColorU32(ThemeColor::Accent), 1.0f);
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    // シングルクリック = Asset Browser で位置を示すだけ、ダブルクリック = Inspector も移す。
    // 参照欄 (widgets::AssetPathField) と同じ操作感に揃える。
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        widgets::RequestAssetReveal(path, ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));

    if (hovered) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(path.c_str());
        if (missing)
            ImGui::TextColored(EditorTheme::Color(ThemeColor::Danger),
                               "ファイルが見つかりません");
        if (usesFallback)
            ImGui::TextDisabled("未割当のため Element 0 のマテリアルで描画されます");
        if (!assigned.visible)
            ImGui::TextDisabled("Visible = OFF — この submesh は描画されません");
        ImGui::Separator();
        ImGui::TextDisabled("Click: Asset Browser で表示 / Double-Click: 選択");
        ImGui::EndTooltip();
    }
    ImGui::PopID();
}

// Renderer が描く submesh すべてのマテリアル総一覧 (表示専用)。
// WHY Renderer 側に置くか: 「何枚のマテリアルで描かれるか」を決めているのはモデルの submesh 数、
//      つまり Renderer が持つ情報であって Material コンポーネントではない。スロット配列だけを
//      見ても submesh との対応が読めないため、全体像は Renderer に集約する。
//      割り当ての変更は Material コンポーネントに一本化し、ここでは編集させない。
void DrawRendererMaterialOverview(EditorContext& ctx, size_t submeshCount, bool skinned)
{
    scene::GameObject* go = ctx.GetSelectedGO();
    if (!go || submeshCount == 0) return;

    ImGui::SeparatorText("Materials");

    auto* mc = go->GetComponent<scene::MaterialComponent>();
    if (!mc) {
        ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning),
                           "! Material component required");
        if (ImGui::Button("Add Material"))
            go->AddComponent<scene::MaterialComponent>(CreateDefaultMaterialComponent(skinned));
        return;
    }

    ImGui::TextDisabled("Size  %zu", submeshCount);
    for (size_t i = 0; i < submeshCount; ++i)
        DrawMaterialOverviewRow(i, *mc);

    ImGui::TextDisabled("割り当ての変更は Material コンポーネントで行います");

    // スロット数と submesh 数がずれていても描画は主マテリアルへフォールバックするので
    // 壊れないが、submesh ごとに .mat を割り当てたい場合に備えて揃える手段を残す。
    if (mc->SlotCount() != submeshCount) {
        ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning),
                           "Material slots: %zu / %zu submesh", mc->SlotCount(), submeshCount);
        if (ImGui::Button("Match Material Slots")) {
            mc->ResizeSlots(submeshCount);
            if (ctx.markSceneDirty) ctx.markSceneDirty();
        }
    }
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

            // MeshRenderer は常に 1 メッシュしか描かないので一覧は 1 行だが、
            // SkinnedMeshRenderer と同じ場所・同じ見え方で参照先を確認できるようにする。
            DrawRendererMaterialOverview(ctx, mr.mesh ? 1u : 0u, false);
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

                DrawRendererMaterialOverview(ctx, meshCount, true);
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
