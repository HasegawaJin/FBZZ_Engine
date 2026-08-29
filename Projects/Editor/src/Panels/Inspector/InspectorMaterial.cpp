/// @file    InspectorMaterial.cpp
/// @brief   Material Component の Inspector 描画。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#include "InspectorMaterial.hpp"

namespace fbzz::editor {

namespace {

// マテリアルスロット 1 つぶんの .mat 編集 UI (shader / textures / params / 保存)。
//
// WHY MaterialComponent ではなく MaterialSlot を受けるか:
//   以前この中身はコンポーネントのスロット 0 専用で、Element 1 以降は参照欄しか
//   出せなかった。「Element 3 の色を変えたい」ために .mat を Asset Browser から
//   探し直す必要があり、それが Renderer 側と Material 側で一覧が二重化していた
//   原因でもある。スロットを引数にすれば、どの Element でもその場で展開できる。
void DrawMaterialSlotBody(scene::MaterialSlot& mc, EditorContext& ctx)
{
            auto* matPtr = asset::AssetManager::GetMaterial(mc.materialAsset);
            if (!matPtr)
                return;

            asset::MaterialAsset& mat = *matPtr;
            struct MaterialUndoTracker {
                ImGuiID activeId = 0;
                asset::MaterialAsset before;
                bool active = false;
                bool changed = false;
            };
            static MaterialUndoTracker undo;
            const bool canRecordUndo = CanRecordEditorUndo(ctx);
            asset::MaterialAsset materialBeforeDraw;
            if (canRecordUndo)
                materialBeforeDraw = mat;
            else
                undo.active = false;
            const ImGuiID activeBefore = ImGui::GetActiveID();
            bool materialDirty = false;
            const renderer::ShaderDescriptor* desc = nullptr;
            if (auto* resources = renderer::ResourceManager::Active()) {
                const std::string shaderPath = mat.shaderPath.empty()
                    ? "Assets/Shaders/Material/Surface/Fallback.hlsl"
                    : mat.shaderPath;
                if (auto shader = resources->LoadShader(shaderPath); shader.IsValid()) {
                    if (auto* loadedShader = resources->Get(shader))
                        desc = &loadedShader->GetDescriptor();
                }
            }

            ImGui::SeparatorText("State");
            {
                if (widgets::AssetPathField("Shader", mat.shaderPath,
                                            ".hlsl", ctx.projectRoot)) {
                    mc.material.reset();
                    materialDirty = true;
                }

                static constexpr const char* kBlendNames[] = {
                    "Opaque", "AlphaBlend", "Additive", "Premultiplied" };
                int blendIndex = 0;
                if (mat.blendMode == renderer::BlendMode::ALPHA_BLEND) blendIndex = 1;
                else if (mat.blendMode == renderer::BlendMode::ADDITIVE) blendIndex = 2;
                else if (mat.blendMode == renderer::BlendMode::PREMULTIPLIED) blendIndex = 3;
                if (ImGui::Combo("Blend", &blendIndex, kBlendNames, 4)) {
                    mat.blendMode = blendIndex == 1 ? renderer::BlendMode::ALPHA_BLEND
                        : blendIndex == 2 ? renderer::BlendMode::ADDITIVE
                        : blendIndex == 3 ? renderer::BlendMode::PREMULTIPLIED
                        : renderer::BlendMode::OPAQUE_BLEND;
                    materialDirty = true;
                }
                materialDirty |= ImGui::Checkbox("Double Sided", &mat.doubleSided);
                materialDirty |= ImGui::DragInt("Render Queue", &mat.renderQueue, 1.0f, 0, 5000);
                {
                    // 通常の Forward / Deferred はプロジェクト設定で決まる。
                    // ここでは専用レンダーパスの用途だけを指定する。
                    // LAYOUT: asset::RenderPath と同じ並びにすること。欠けた用途は
                    //         Combo を触った瞬間に別の値へ化ける。
                    static constexpr const char* kMaterialUsageNames[] = {
                        "Auto", "Particle", "Trail", "UI", "Decal" };
                    int usageIndex = static_cast<int>(mat.renderPath);
                    if (ImGui::Combo("Material Usage", &usageIndex, kMaterialUsageNames,
                                     IM_ARRAYSIZE(kMaterialUsageNames))) {
                        mat.renderPath = static_cast<asset::RenderPath>(usageIndex);
                        materialDirty = true;
                    }
                    ImGui::TextDisabled("Forward / Deferred: Project Settings");
                }
                {
                    static constexpr const char* kMeshTypeNames[] = { "Any", "Surface", "Skinned" };
                    int mtIndex = static_cast<int>(mat.meshType);
                    if (ImGui::Combo("Mesh Type", &mtIndex, kMeshTypeNames, 3)) {
                        mat.meshType = static_cast<asset::MeshType>(mtIndex);
                        materialDirty = true;
                    }
                }
            }

            const bool isWaterMaterial = mat.shaderPath == "Assets/Shaders/Water/Water.hlsl"
                || mc.materialPath.find("/Water/") != std::string::npos;

            if (isWaterMaterial) {
                materialDirty |= DrawWaterMaterialInspector(mat);
            } else {
                ImGui::SeparatorText("Textures");
                // WHY: fzmat のキーは GeometryPassHelpers の kTextureSlotNames と一致させる必要がある。
                //      ShaderDescriptor の tex.name は HLSL 変数名 ("texAlbedo") であり、
                //      kTextureSlotNames ("albedo") と異なるため tex.slot でインデックスして変換する。
                //      t5-t7 はカスタムシェーダー用の汎用スロット ("tex5"/"tex6"/"tex7") として開放。
                static constexpr std::array<const char*, 8> kCanonicalSlots = {
                    "albedo", "normal", "metallic", "emissive", "ao", "tex5", "tex6", "tex7"
                };
                auto drawTexSlot = [&](const char* slot) {
                    std::string& path = mat.textures[slot];
                    if (widgets::AssetPathField(slot, path, widgets::kTextureAssetFilter, ctx.projectRoot))
                        materialDirty = true;
                };
                if (desc && !desc->textures.empty()) {
                    for (const auto& tex : desc->textures)
                        if (tex.slot < kCanonicalSlots.size())
                            drawTexSlot(kCanonicalSlots[tex.slot]);
                } else {
                    // シェーダー未取得時は標準 5 スロットのみ表示。
                    for (size_t i = 0; i < 5; ++i)
                        drawTexSlot(kCanonicalSlots[i]);
                }
                auto drawParam = [&](const std::string& name, std::vector<float>& values) {
                    if (values.empty())
                        return;

                    ImGui::PushID(name.c_str());
                    if (values.size() == 1) {
                        materialDirty |= ImGui::DragFloat(name.c_str(), values.data(), 0.01f);
                    } else if (values.size() == 2) {
                        materialDirty |= widgets::DragAxes(name.c_str(), values.data(), 2, 0.01f);
                    } else if (values.size() == 3) {
                        materialDirty |= widgets::DragAxes(name.c_str(), values.data(), 3, 0.01f);
                    } else if (values.size() == 4) {
                        const bool looksLikeColor = name.find("color") != std::string::npos
                            || name.find("Color") != std::string::npos
                            || name.find("albedo") != std::string::npos;
                        materialDirty |= looksLikeColor
                            ? ImGui::ColorEdit4(name.c_str(), values.data())
                            : widgets::DragAxes(name.c_str(), values.data(), 4, 0.01f);
                    } else {
                        for (size_t i = 0; i < values.size(); ++i) {
                            ImGui::PushID(static_cast<int>(i));
                            materialDirty |= ImGui::DragFloat("Value", &values[i], 0.01f);
                            ImGui::PopID();
                        }
                    }
                    ImGui::PopID();
                };
                auto defaultParamValues = [](const std::string& name, size_t componentCount) {
                    std::vector<float> values(componentCount, 0.0f);
                    if (name == "albedo" && componentCount >= 4) {
                        values[0] = values[1] = values[2] = values[3] = 1.0f;
                    } else if ((name == "emissiveColor" || name == "rimColor") && componentCount >= 3) {
                        values[0] = values[1] = values[2] = 1.0f;
                    } else if (name == "uvTiling" && componentCount >= 2) {
                        values[0] = values[1] = 1.0f;
                    } else if (name == "roughness") {
                        values[0] = 0.65f;
                    } else if (name == "normalStrength" || name == "occlusionStrength" || name == "rimIntensity") {
                        values[0] = 1.0f;
                    } else if (name == "alphaCutoff") {
                        values[0] = 0.5f;
                    } else if (name == "rimPower") {
                        values[0] = 3.0f;
                    }
                    return values;
                };

                ImGui::SeparatorText("Params");
                if (desc) {
                    for (const auto& var : desc->vars) {
                        if (var.varType != renderer::ShaderVarType::Float)
                            continue;
                        const size_t componentCount = var.columns > 0 ? var.columns : 1;
                        std::vector<float>& values = mat.params[var.name];
                        if (values.size() != componentCount)
                            values = defaultParamValues(var.name, componentCount);
                        drawParam(var.name, values);
                    }
                }
                for (auto& [name, values] : mat.params) {
                    if (desc && desc->FindVar(name))
                        continue;
                    drawParam(name, values);
                }
            }

            if (materialDirty) {
                mc.material.reset();
                // AssetBrowser のサムネイルを値変更のたびに追従させる (保存待ちにしない)。
                ctx.BumpMaterialPreviewRevision(NormalizeAssetPath(mc.materialPath));
                if (ctx.activeScene) {
                    const std::string changedPath = NormalizeAssetPath(mc.materialPath);
                    for (auto [terrain] : ctx.activeScene->View<scene::TerrainComponent>()) {
                        for (const auto& lm : terrain.layerMaterials)
                            if (NormalizeAssetPath(lm) == changedPath) { terrain.splatDirty = true; break; }
                    }
                    for (auto [water] : ctx.activeScene->View<scene::WaterComponent>()) {
                        if (NormalizeAssetPath(water.materialPath) == changedPath) {
                            water.texDirty = true;
                            water.foamDirty = true;
                        }
                    }
                }
            }

            const ImGuiID activeAfter = ImGui::GetActiveID();
            auto pushMaterialCommand = [&](const asset::MaterialAsset& before,
                                           const asset::MaterialAsset& after) {
                if (!ctx.undoStack) return;
                const auto handle = mc.materialAsset;
                const auto markDirty = ctx.markSceneDirty;
                EditorContext* context = &ctx;
                const std::string relPath = NormalizeAssetPath(mc.materialPath);
                auto apply = [handle, markDirty, context, relPath](const asset::MaterialAsset& value) {
                    if (auto* target = asset::AssetManager::GetMaterial(handle)) {
                        *target = value;
                        context->BumpMaterialPreviewRevision(relPath);
                        if (markDirty) markDirty();
                    }
                };
                ctx.undoStack->Push(std::make_unique<LambdaCommand>(
                    "Edit Material Asset",
                    [apply, after]() { apply(after); },
                    [apply, before]() { apply(before); }));
            };
            if (!canRecordUndo) {
                undo.active = false;
                undo.changed = false;
            } else if (!undo.active && activeAfter != 0 && activeAfter != activeBefore) {
                undo.activeId = activeAfter;
                undo.before = materialBeforeDraw;
                undo.active = true;
                undo.changed = materialDirty;
            } else if (undo.active && activeAfter == undo.activeId) {
                undo.changed |= materialDirty;
            } else if (undo.active && activeAfter != undo.activeId) {
                if (undo.changed) pushMaterialCommand(undo.before, mat);
                undo.active = false;
                undo.changed = false;
            } else if (!undo.active && materialDirty && activeAfter == 0) {
                pushMaterialCommand(materialBeforeDraw, mat);
            }

            ImGui::Separator();
            const bool canSave = !mc.materialPath.empty();
            if (!canSave)
                ImGui::BeginDisabled();
            if (ImGui::Button("Save .mat")) {
                if (asset::SaveMaterialAssetToFile(MaterialAssetDiskPath(ctx, mc.materialPath), mat))
                    ctx.requestAssetBrowserRefresh = true;
            }
            if (!canSave)
                ImGui::EndDisabled();
}

// マテリアル配列の 1 行。D&D / クリックで辿る / 中身のインライン編集をここで完結させる。
//
// WHY 番号入力を出さないか:
//   スロット番号は submesh との対応そのもので、ユーザーが決める値ではない。
//   行がその submesh の置き場になっていれば、.mat をその行へ落とすだけで割り当てが済む。
//   widgets::AssetPathField は D&D 受理・クリックで Asset Browser へ ping・
//   ダブルクリックで Inspector へ移動をすべて内包しているため、それを使うだけでよい。
void DrawMaterialElementRow(scene::MaterialSlot& slot,
                            size_t slotIndex,
                            bool isFallbackTarget,
                            EditorContext& ctx)
{
    ImGui::PushID(static_cast<int>(slotIndex));

    const std::string label = "Element " + std::to_string(slotIndex);
    if (widgets::AssetPathField(label.c_str(), slot.materialPath, ".mat", ctx.projectRoot)) {
        // AssetPathField は materialPath を直接書き換えるため、解決済みハンドルと
        // GPU キャッシュをここで捨てて再解決させる。
        slot.materialAsset = {};
        slot.material.reset();
        slot.propertyValidationCache.clear();
        slot.propertyValidationDescriptor = nullptr;
        slot.EnsureMaterialAsset();
        if (ctx.markSceneDirty) ctx.markSceneDirty();
    }

    if (!slot.materialPath.empty() && !slot.materialAsset.IsValid()) {
        ImGui::TextColored(EditorTheme::Color(ThemeColor::Danger),
                           "Missing: %s", slot.materialPath.c_str());
    } else if (slot.materialPath.empty() && isFallbackTarget) {
        // SlotAt() は未割当スロットを Element 0 へフォールバックさせる。
        // 「割り当てが無いのに描かれている」状態を隠さない。
        ImGui::TextDisabled("未割当 — Element 0 のマテリアルで描画されます");
    }

    if (ImGui::Checkbox("Visible", &slot.visible)) {
        if (ctx.markSceneDirty) ctx.markSceneDirty();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("この submesh を描画するか");

    // 中身のインライン編集。Unity がマテリアルごとの折りたたみを出すのと同じ。
    // WHY 折りたたみにするか: 全スロットを常時展開すると、submesh が 5 枚あるだけで
    //     Inspector が数百行になり、割り当ての一覧という本来の役割が埋もれる。
    if (slot.materialAsset.IsValid()) {
        if (ImGui::TreeNode("Edit")) {
            DrawMaterialSlotBody(slot, ctx);
            ImGui::TreePop();
        }
    }

    ImGui::Separator();
    ImGui::PopID();
}

// この GameObject の Renderer が描く submesh 数。Renderer が無ければ 0。
size_t RendererSubmeshCount(scene::GameObject& go)
{
    if (const auto* smr = go.GetComponent<scene::SkinnedMeshRenderer>())
        return smr->SubmeshCount();
    if (const auto* mr = go.GetComponent<scene::MeshRenderer>())
        return mr->mesh ? 1u : 0u;
    return 0u;
}

} // namespace

void DrawMaterialInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType)
{
    DrawComponentSection<scene::MaterialComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Material",
        [](scene::MaterialComponent& mc, EditorContext& ctx) {
            scene::GameObject* owner = ctx.GetSelectedGO();
            const size_t submeshCount = owner ? RendererSubmeshCount(*owner) : 0u;

            // 行数を submesh 数へ自動追従させる。
            // WHY 手動の Add Slot / Match Submesh Count を無くしたか:
            //   スロット数は submesh 数と一致していなければ意味がなく、ずれた状態は
            //   フォールバック描画という分かりにくい挙動を生むだけだった。
            //   必要な数だけが常に並ぶ形にする。
            if (submeshCount > 0 && mc.SlotCount() != submeshCount)
                mc.ResizeSlots(submeshCount);

            // Renderer を持たない GameObject (VFX の Mesh 出力・デカール等) は
            // submesh の概念が無いので、単一マテリアルとして 1 行だけ出す。
            if (submeshCount == 0) {
                ImGui::SeparatorText("Material");
                DrawMaterialElementRow(mc, 0, false, ctx);
                return;
            }

            ImGui::SeparatorText("Materials");
            ImGui::TextDisabled("Size  %zu", mc.SlotCount());
            for (size_t i = 0; i < mc.SlotCount(); ++i) {
                // Element 0 自身はフォールバック先なので注記の対象にしない。
                DrawMaterialElementRow(mc.RawSlotAt(i), i, i > 0, ctx);
            }
        });
}


} // namespace fbzz::editor
