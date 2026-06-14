// FBZZ Engine
// InspectorMaterial.cpp | fbzz::editor
// Material Component の Inspector 描画
#include "InspectorMaterial.hpp"

namespace fbzz::editor {

void DrawMaterialInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType)
{
    DrawComponentSection<scene::MaterialComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Material",
        [](scene::MaterialComponent& mc, EditorContext& ctx) {

            auto loadMaterialAsset = [&]() {
                mc.materialAsset = mc.materialPath.empty()
                    ? renderer::ResourceHandle<renderer::MaterialAssetTag>{}
                    : asset::AssetManager::LoadMaterial(mc.materialPath);
                mc.material.reset();
            };

            // ── Rendering ───────────────────────────────────────────────────
            ImGui::SeparatorText("Material Asset");
            {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s", mc.materialPath.c_str());
                if (ImGui::InputText("Material (.fzmat)", buf, sizeof(buf))) {
                    mc.materialPath = NormalizeAssetPath(buf);
                    loadMaterialAsset();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                        const std::string dropped = NormalizeAssetPath(static_cast<const char*>(p->Data));
                        if (util::StringUtils::EndsWith(dropped, ".fzmat")) {
                            mc.materialPath = dropped;
                            loadMaterialAsset();
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                if (!mc.materialPath.empty() && !mc.materialAsset.IsValid())
                    ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.25f, 1.0f), "Missing: %s", mc.materialPath.c_str());
            }

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
                char shaderBuf[512];
                std::snprintf(shaderBuf, sizeof(shaderBuf), "%s", mat.shaderPath.c_str());
                if (ImGui::InputText("Shader", shaderBuf, sizeof(shaderBuf))) {
                    mat.shaderPath = NormalizeAssetPath(shaderBuf);
                    mc.material.reset();
                    materialDirty = true;
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                        const std::string dropped = NormalizeAssetPath(static_cast<const char*>(p->Data));
                        if (util::StringUtils::EndsWith(dropped, ".hlsl")) {
                            mat.shaderPath = dropped;
                            mc.material.reset();
                            materialDirty = true;
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                static constexpr const char* kBlendNames[] = { "Opaque", "AlphaBlend", "Additive" };
                int blendIndex = 0;
                if (mat.blendMode == renderer::BlendMode::ALPHA_BLEND) blendIndex = 1;
                else if (mat.blendMode == renderer::BlendMode::ADDITIVE) blendIndex = 2;
                if (ImGui::Combo("Blend", &blendIndex, kBlendNames, 3)) {
                    mat.blendMode = blendIndex == 1 ? renderer::BlendMode::ALPHA_BLEND
                        : blendIndex == 2 ? renderer::BlendMode::ADDITIVE
                        : renderer::BlendMode::OPAQUE_BLEND;
                    materialDirty = true;
                }
                materialDirty |= ImGui::Checkbox("Double Sided", &mat.doubleSided);
                materialDirty |= ImGui::DragInt("Render Queue", &mat.renderQueue, 1.0f, 0, 5000);
                {
                    static constexpr const char* kRenderPathNames[] = { "Auto", "Deferred", "Forward" };
                    int rpIndex = static_cast<int>(mat.renderPath);
                    if (ImGui::Combo("Render Path", &rpIndex, kRenderPathNames, 3)) {
                        mat.renderPath = static_cast<asset::RenderPath>(rpIndex);
                        materialDirty = true;
                    }
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

            const bool isTerrainMaterial = mat.shaderPath == "Assets/Shaders/Terrain/Terrain.hlsl"
                || mc.materialPath.find("/Terrain/") != std::string::npos;
            const bool isWaterMaterial = mat.shaderPath == "Assets/Shaders/Water/Water.hlsl"
                || mc.materialPath.find("/Water/") != std::string::npos;

            if (isTerrainMaterial) {
                materialDirty |= DrawTerrainMaterialInspector(mat);
            } else if (isWaterMaterial) {
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
                    char texBuf[512];
                    std::snprintf(texBuf, sizeof(texBuf), "%s", path.c_str());
                    if (ImGui::InputText(slot, texBuf, sizeof(texBuf))) {
                        path = NormalizeAssetPath(texBuf);
                        materialDirty = true;
                    }
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                            path = NormalizeAssetPath(static_cast<const char*>(p->Data));
                            materialDirty = true;
                        }
                        ImGui::EndDragDropTarget();
                    }
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
                        materialDirty |= ImGui::DragFloat2(name.c_str(), values.data(), 0.01f);
                    } else if (values.size() == 3) {
                        materialDirty |= ImGui::DragFloat3(name.c_str(), values.data(), 0.01f);
                    } else if (values.size() == 4) {
                        const bool looksLikeColor = name.find("color") != std::string::npos
                            || name.find("Color") != std::string::npos
                            || name.find("albedo") != std::string::npos;
                        materialDirty |= looksLikeColor
                            ? ImGui::ColorEdit4(name.c_str(), values.data())
                            : ImGui::DragFloat4(name.c_str(), values.data(), 0.01f);
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
                if (ctx.activeScene) {
                    const std::string changedPath = NormalizeAssetPath(mc.materialPath);
                    for (auto [terrain] : ctx.activeScene->View<scene::TerrainComponent>()) {
                        if (NormalizeAssetPath(terrain.materialPath) == changedPath)
                            terrain.splatDirty = true;
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
                auto apply = [handle, markDirty](const asset::MaterialAsset& value) {
                    if (auto* target = asset::AssetManager::GetMaterial(handle)) {
                        *target = value;
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
            if (ImGui::Button("Save .fzmat")) {
                if (asset::SaveMaterialAssetToFile(MaterialAssetDiskPath(ctx, mc.materialPath), mat))
                    ctx.requestAssetBrowserRefresh = true;
            }
            if (!canSave)
                ImGui::EndDisabled();

            // ── Shader ──────────────────────────────────────────────────────

            // ── Textures ────────────────────────────────────────────────────

            // ── Parameters (Descriptor 駆動) ────────────────────────────────
        });

}


} // namespace fbzz::editor
