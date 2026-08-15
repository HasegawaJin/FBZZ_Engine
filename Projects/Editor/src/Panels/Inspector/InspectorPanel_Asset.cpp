// FBZZ Engine
// Inspector/InspectorPanel_Asset.cpp | fbzz::editor
// Asset Browser から選択したファイル用 Inspector
// Undo 記録可否の判定 (CanRecordEditorUndo) など、Inspector 共通ヘルパーを使う。
#include "InspectorCommon.hpp"
#include <Editor/Panels/InspectorPanel.hpp>
#include <Editor/Panels/AnimationGraphInspector.hpp>
#include <Editor/Panels/AnimationPreviewPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Import/FbxMetaSerializer.hpp>
#include <Editor/Import/ImportSettingsSchema.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/MaterialInspectorWidgets.hpp>
#include <Editor/Util/PostProcessInspectorWidgets.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/ImGuiReflector.hpp>
#include <Engine/Asset/AnimationClip.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/DataAsset.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Asset/FzTerrainSerializer.hpp>
#include <Engine/Scene/TerrainAssetSerializer.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/AvatarMaskAsset.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Asset/Skeleton.hpp>
#include <Engine/Asset/ModelAsset.hpp>
#include <Engine/Asset/PhysicsMaterialAsset.hpp>
#include <Engine/Asset/PostProcessProfile.hpp>
#include <Engine/Asset/TerrainAsset.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/ITexture.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Renderer/ShaderDescriptor.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <Engine/Util/Uuid.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace fbzz::editor {

void InspectorPanel::DrawAssetInspector(EditorContext& ctx, const std::string& assetPath)
{
    const std::string sourceExt =
        util::StringUtils::ToLower(util::FileSystem::GetExtension(assetPath));
    const bool isModelSource =
        sourceExt == ".fbx" || sourceExt == ".obj" ||
        sourceExt == ".gltf" || sourceExt == ".glb";
    const bool isTextureSource =
        sourceExt == ".png" || sourceExt == ".jpg" || sourceExt == ".jpeg" ||
        sourceExt == ".dds" || sourceExt == ".tga" || sourceExt == ".bmp" ||
        sourceExt == ".hdr" || sourceExt == ".exr";

    // Unity と同様に、Project で原本を選択したまま隣接 .meta の Import Settings を描画する。
    // WHY: 非表示 sidecar を選び直す操作や別ウィンドウを挟まず、全 raw 形式で同じ編集体験にする。
    const std::string absPath =
        (isModelSource || isTextureSource) ? assetPath + ".meta" : assetPath;
    const std::string filename = util::FileSystem::GetFilename(assetPath);
    const std::string ext =
        util::StringUtils::ToLower(util::FileSystem::GetExtension(absPath));

    // deselect 自動保存: 前回の .mat が dirty のまま別アセットへ移動したとき保存する。
    // WHY: 「Save ボタンを押し忘れる」問題を解消しつつ、mid-drag 中の大量書き込みを避けるため
    //      選択が外れたタイミング (= 本関数が別パスで呼ばれた瞬間) に保存する。
    if (!m_inspectedAssetPath.empty() &&
        m_inspectedAssetPath != absPath &&
        util::StringUtils::ToLower(util::FileSystem::GetExtension(m_inspectedAssetPath)) == ".mat" &&
        AssetDirtyRegistry::IsDirty(m_inspectedAssetPath))
    {
        auto* prevMat = asset::AssetManager::GetMaterial(m_inspectedMat);
        if (prevMat && asset::SaveMaterialAssetToFile(m_inspectedAssetPath, *prevMat)) {
            AssetDirtyRegistry::MarkClean(m_inspectedAssetPath);
            ctx.requestAssetBrowserRefresh = true;
        }
    }

    ImGui::TextUnformatted(filename.c_str());
    ImGui::TextDisabled("%s", assetPath.c_str());
    ImGui::Separator();

    if (ext == ".mat") {
        const std::string relPath = NormalizeAssetPath(absPath);
        if (m_inspectedAssetPath != absPath || !m_inspectedMat.IsValid()) {
            m_inspectedAssetPath = absPath;
            m_inspectedMat = asset::AssetManager::LoadMaterial(relPath);
        }

        if (!m_inspectedMat.IsValid()) {
            ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, "Failed to load .mat");
            return;
        }

        auto* matPtr = asset::AssetManager::GetMaterial(m_inspectedMat);
        if (!matPtr) {
            ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, "Failed to load .mat");
            return;
        }

        asset::MaterialAsset& mat = *matPtr;
        struct MaterialUndoTracker {
            ImGuiID activeId = 0;
            asset::MaterialAsset before;
            bool active = false;
            bool changed = false;
        };
        static MaterialUndoTracker undo;
        const bool canRecordUndo =
            ctx.undoStack != nullptr && ctx.undoStack->IsRecordingEnabled();
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
            if (widgets::AssetPathField("Shader", mat.shaderPath, ".hlsl", ctx.projectRoot))
                materialDirty = true;

            static constexpr const char* kBlendNames[] = {
                "Opaque", "AlphaBlend", "Additive", "Premultiplied" };
            int blendIndex = 0;
            if      (mat.blendMode == renderer::BlendMode::ALPHA_BLEND)   blendIndex = 1;
            else if (mat.blendMode == renderer::BlendMode::ADDITIVE)      blendIndex = 2;
            else if (mat.blendMode == renderer::BlendMode::PREMULTIPLIED) blendIndex = 3;
            if (ImGui::Combo("Blend", &blendIndex, kBlendNames, 4)) {
                mat.blendMode = blendIndex == 1 ? renderer::BlendMode::ALPHA_BLEND
                              : blendIndex == 2 ? renderer::BlendMode::ADDITIVE
                              : blendIndex == 3 ? renderer::BlendMode::PREMULTIPLIED
                              : renderer::BlendMode::OPAQUE_BLEND;
                materialDirty = true;
            }
            materialDirty |= ImGui::Checkbox("Double Sided",  &mat.doubleSided);
            materialDirty |= ImGui::DragInt  ("Render Queue", &mat.renderQueue, 1.0f, 0, 5000);
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
            || relPath.find("/Terrain/") != std::string::npos;
        const bool isWaterMaterial = mat.shaderPath == "Assets/Shaders/Water/Water.hlsl"
            || relPath.find("/Water/") != std::string::npos;

        if (isTerrainMaterial) {
            const auto tf = DrawTerrainLayerMaterialInspector(mat);
            materialDirty |= (tf.textureDirty || tf.paramDirty);
        } else if (isWaterMaterial) {
            materialDirty |= DrawWaterMaterialInspector(mat);
        } else {
            ImGui::SeparatorText("Textures");
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
                for (size_t i = 0; i < 5; ++i)
                    drawTexSlot(kCanonicalSlots[i]);
            }

            auto drawParam = [&](const std::string& name, std::vector<float>& values) {
                if (values.empty()) return;
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
            auto defaultParamValues = [](const std::string& name, size_t n) {
                std::vector<float> v(n, 0.0f);
                if (name == "albedo" && n >= 4)
                    v[0] = v[1] = v[2] = v[3] = 1.0f;
                else if ((name == "emissiveColor" || name == "rimColor") && n >= 3)
                    v[0] = v[1] = v[2] = 1.0f;
                else if (name == "uvTiling" && n >= 2)
                    v[0] = v[1] = 1.0f;
                else if (name == "roughness")
                    v[0] = 0.65f;
                else if (name == "normalStrength" || name == "occlusionStrength" || name == "rimIntensity")
                    v[0] = 1.0f;
                else if (name == "alphaCutoff")
                    v[0] = 0.5f;
                else if (name == "rimPower")
                    v[0] = 3.0f;
                return v;
            };

            ImGui::SeparatorText("Params");
            if (desc) {
                for (const auto& var : desc->vars) {
                    if (var.varType != renderer::ShaderVarType::Float) continue;
                    const size_t n = var.columns > 0 ? var.columns : 1;
                    std::vector<float>& values = mat.params[var.name];
                    if (values.size() != n)
                        values = defaultParamValues(var.name, n);
                    drawParam(var.name, values);
                }
            }
            for (auto& [name, values] : mat.params) {
                if (desc && desc->FindVar(name)) continue;
                drawParam(name, values);
            }
        }

        // AssetBrowser のサムネイルを値変更のたびに追従させる (保存待ちにしない)。
        if (materialDirty)
            ctx.BumpMaterialPreviewRevision(relPath);

        if (materialDirty && ctx.activeScene) {
            for (auto [mc] : ctx.activeScene->View<scene::MaterialComponent>()) {
                if (NormalizeAssetPath(mc.materialPath) == relPath)
                    mc.material.reset();
            }
            for (auto [terrain] : ctx.activeScene->View<scene::TerrainComponent>()) {
                for (const auto& lm : terrain.layerMaterials)
                    if (NormalizeAssetPath(lm) == relPath) { terrain.splatDirty = true; break; }
            }
            for (auto [water] : ctx.activeScene->View<scene::WaterComponent>()) {
                if (NormalizeAssetPath(water.materialPath) == relPath) {
                    water.texDirty = true;
                    water.foamDirty = true;
                }
            }
        }

        // dirty になったら Registry に登録 (deselect 時 or Save All で一括保存できるようにする)
        if (materialDirty) {
            const std::string capturedPath   = absPath;
            const std::string capturedDisplay = NormalizeAssetPath(absPath);
            auto capturedHandle = m_inspectedMat;
            AssetDirtyRegistry::Register(
                capturedPath, capturedDisplay, "MAT",
                [capturedPath, capturedHandle]() {
                    auto* m = asset::AssetManager::GetMaterial(capturedHandle);
                    return m && asset::SaveMaterialAssetToFile(capturedPath, *m);
                });
        }

        const ImGuiID activeAfter = ImGui::GetActiveID();
        auto pushMaterialCommand = [&](const asset::MaterialAsset& before,
                                       const asset::MaterialAsset& after) {
            if (!ctx.undoStack) return;
            EditorContext* context = &ctx;
            const auto handle = m_inspectedMat;
            const std::string capturedPath = absPath;
            const std::string capturedDisplay = relPath;
            auto apply = [context, handle, capturedPath, capturedDisplay](
                             const asset::MaterialAsset& value) {
                auto* target = asset::AssetManager::GetMaterial(handle);
                if (!target) return;
                *target = value;
                // Save ボタンを廃止した代わりに、Undo/Redo が確定した瞬間に即ディスクへ書く。
                // WHY: メモリ上の値と .mat ファイルが常に一致していないと、Undo で戻した
                //      つもりが再読み込み後に元に戻ってしまう (=見た目だけの Undo) という
                //      事故が起きる。書き込み自体はここ (ウィジェット確定時) 1 回だけなので、
                //      ドラッグ中の連続フレーム書き込みにはならない。
                asset::SaveMaterialAssetToFile(capturedPath, *target);
                AssetDirtyRegistry::MarkClean(capturedPath);
                if (context->activeScene) {
                    for (auto [component] : context->activeScene->View<scene::MaterialComponent>()) {
                        if (NormalizeAssetPath(component.materialPath) == capturedDisplay)
                            component.material.reset();
                    }
                    for (auto [terrain] : context->activeScene->View<scene::TerrainComponent>()) {
                        for (const auto& lm : terrain.layerMaterials)
                            if (NormalizeAssetPath(lm) == capturedDisplay) { terrain.splatDirty = true; break; }
                    }
                    for (auto [water] : context->activeScene->View<scene::WaterComponent>()) {
                        if (NormalizeAssetPath(water.materialPath) == capturedDisplay) {
                            water.texDirty = true;
                            water.foamDirty = true;
                        }
                    }
                }
                context->BumpMaterialPreviewRevision(capturedDisplay);
                context->requestAssetBrowserRefresh = true;
            };
            ctx.undoStack->Push(std::make_unique<LambdaCommand>(
                "Edit Material Asset",
                [apply, after]() { apply(after); },
                [apply, before]() { apply(before); }));
            // UndoStack::Push は記録するだけで Do() を呼ばない (mat は既にウィジェットで
            // 直接編集済みのため)。よってここで確定時点の保存を明示的に行う。apply() 内の
            // 保存は Undo/Redo 実行時にのみ効く。
            asset::SaveMaterialAssetToFile(capturedPath, after);
            AssetDirtyRegistry::MarkClean(capturedPath);
            context->requestAssetBrowserRefresh = true;
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

        // Save ボタンは廃止 (Unity ライク: 編集を確定した瞬間に自動保存し、Undo/Redo で
        // 巻き戻せる)。ウィジェットがアクティブな間 (ドラッグ中など) だけ "Modified" を
        // 表示し、フォーカスが外れて pushMaterialCommand が確定すると同時に消える。
        ImGui::Separator();
        const bool isMaterialDirtyNow = AssetDirtyRegistry::IsDirty(absPath);
        if (isMaterialDirtyNow) {
            ImGui::TextColored({1.0f, 0.8f, 0.2f, 1.0f}, "Modified");
        } else {
            ImGui::TextDisabled("Saved");
        }
    } else if (ext == ".animcontroller") {
        if (DrawAnimationGraphAssetInspector(ctx)) {
            // Unity と同じく、State / Transition 詳細の直下でクリップと遷移ブレンドを確認できる。
            ImGui::SeparatorText("Preview");
            DrawAnimationPreviewWidget(ctx, 240.0f);
        } else {
            ImGui::TextDisabled("Select a State or Transition in Animation Graph.");
            ImGui::Spacing();
            ImGui::TextDisabled("The controller can be edited without selecting a Hierarchy object.");
        }
        if (ctx.animationControllerDirty) {
            ImGui::Spacing();
            ImGui::TextColored(
                {1.0f, 0.8f, 0.2f, 1.0f},
                "Modified - use Save Controller in Animation Graph.");
        }
    } else if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb") {
        namespace fs = std::filesystem;
        const fs::path    p         = util::FileSystem::PathFromUtf8(absPath);
        const std::string stem      = util::FileSystem::PathToUtf8(p.stem());
        // 物理 .fzasset は Library/Baked に隔離されているため、論理パスの解決を通して確認する。
        const bool imported = util::FileSystem::Exists(
            asset::AssetManager::ResolveAssetPath(util::FileSystem::PathToUtf8(
                p.parent_path() / p.stem() / (stem + ".fzasset"))));

        ImGui::TextDisabled("Type: 3D Model Source (%s)", ext.c_str());
        ImGui::Spacing();

        if (imported) {
            ImGui::TextColored({ 0.3f, 0.9f, 0.3f, 1.0f }, "Status: Imported");
            ImGui::Spacing();
            if (ImGui::Button("Reimport..."))
                ctx.requestOpenImportModal = absPath;

            const auto handle = asset::AssetManager::Load<asset::ModelAsset>(absPath);
            if (const auto* m = asset::AssetManager::Get(handle)) {
                ImGui::SeparatorText("Info");
                ImGui::LabelText("LODs",      "%u",  m->LodCount());
                ImGui::LabelText("Skinned",   "%s",  m->IsSkinned() ? "Yes" : "No");
                ImGui::LabelText("Mat Slots", "%zu", m->materialSlotNames.size());
                if (m->IsSkinned() && m->skeleton)
                    ImGui::LabelText("Skeleton Nodes", "%zu", m->skeleton->nodes.size());
            }

            // クリップを持つスキンモデルはその場で再生確認できるようにする。
            if (const asset::Model* previewModel = asset::AssetManager::LoadModel(absPath);
                previewModel && previewModel->skeleton && !previewModel->clips.empty()) {
                ImGui::SeparatorText("Animation Preview");
                DrawAnimationPreviewWidget(ctx, 240.0f);
            }
        } else {
            ImGui::TextColored({ 1.0f, 0.6f, 0.1f, 1.0f }, "Status: Not Imported");
            ImGui::Spacing();
            if (ImGui::Button("Import..."))
                ctx.requestOpenImportModal = absPath;
        }
    } else if (ext == ".fzasset") {
        // ── .model バイナリ ──────────────────────────────────────────────
        ImGui::TextDisabled("Type: FBZZ Model Asset");
        ImGui::Spacing();

        const auto handle = asset::AssetManager::Load<asset::ModelAsset>(absPath);
        if (!handle.IsValid()) {
            ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, "Load failed");
        } else if (const auto* m = asset::AssetManager::Get(handle)) {
            ImGui::SeparatorText("Info");
            ImGui::LabelText("LODs",      "%u",  m->LodCount());
            ImGui::LabelText("Skinned",   "%s",  m->IsSkinned() ? "Yes" : "No");
            ImGui::LabelText("Mat Slots", "%zu", m->materialSlotNames.size());

            // ── スケルトン情報 ─────────────────────────────────────────
            if (m->IsSkinned() && m->skeleton) {
                ImGui::SeparatorText("Skeleton");
                ImGui::LabelText("Nodes", "%zu", m->skeleton->nodes.size());
            }

            // ── LOD / サブメッシュ一覧 ─────────────────────────────────
            ImGui::SeparatorText("Meshes");
            for (uint32_t lodIdx = 0; lodIdx < m->LodCount(); ++lodIdx) {
                const auto& lod = m->lods[lodIdx];
                const std::string lodLabel =
                    lodIdx == 0
                    ? std::string("LOD 0  (primary)")
                    : std::string("LOD ") + std::to_string(lodIdx)
                      + "  (screen < " + std::to_string(lod.screenSizeThreshold).substr(0, 4) + ")";
                if (ImGui::TreeNodeEx(lodLabel.c_str(),
                        lodIdx == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
                    for (int si = 0; si < (int)lod.submeshes.size(); ++si) {
                        const auto& sm = lod.submeshes[si];
                        const char* slotName =
                            sm.materialSlotIndex < m->materialSlotNames.size()
                            ? m->materialSlotNames[sm.materialSlotIndex].c_str()
                            : "?";
                        const uint32_t verts = sm.mesh ? sm.mesh->vertexCount : 0;
                        const uint32_t tris  = sm.mesh ? sm.mesh->indexCount / 3 : 0;
                        ImGui::TextDisabled("[%d]", si);
                        ImGui::SameLine();
                        ImGui::Text("%s", slotName);
                        ImGui::SameLine();
                        ImGui::TextDisabled("%u verts  %u tris", verts, tris);
                    }
                    ImGui::TreePop();
                }
            }

            // ── マテリアルスロット ─────────────────────────────────────
            if (!m->materialSlotNames.empty()) {
                ImGui::SeparatorText("Material Slots");
                for (const auto& slotName : m->materialSlotNames)
                    ImGui::TextDisabled("  %s  ->  (unbound)", slotName.c_str());
            }

            // クリップを持つスキンモデルはその場で再生確認できるようにする。
            if (const asset::Model* previewModel = asset::AssetManager::LoadModel(absPath);
                previewModel && previewModel->skeleton && !previewModel->clips.empty()) {
                ImGui::SeparatorText("Animation Preview");
                DrawAnimationPreviewWidget(ctx, 240.0f);
            }
        }

        // ── Reimport ───────────────────────────────────────────────────
        ImGui::Spacing();
        ImGui::Separator();
        {
            namespace fs = std::filesystem;
            const fs::path modelPath = util::FileSystem::PathFromUtf8(absPath);
            const std::string stem   = util::FileSystem::PathToUtf8(modelPath.stem());
            const fs::path parentDir = modelPath.parent_path();
            std::string fbxCandidate;
            for (const auto* fbxExt : {".fbx", ".FBX", ".obj", ".OBJ", ".gltf", ".GLTF", ".glb", ".GLB"}) {
                const fs::path c = parentDir / (stem + fbxExt);
                if (util::FileSystem::Exists(c)) { fbxCandidate = util::FileSystem::PathToUtf8(c); break; }
                const fs::path cp = parentDir.parent_path() / (stem + fbxExt);
                if (util::FileSystem::Exists(cp)) { fbxCandidate = util::FileSystem::PathToUtf8(cp); break; }
            }
            if (fbxCandidate.empty()) {
                ImGui::TextDisabled("Source: (not found)");
            } else {
                const std::string label =
                    "Reimport from " + util::FileSystem::GetFilename(fbxCandidate);
                if (ImGui::Button(label.c_str()))
                    ctx.requestOpenImportModal = fbxCandidate;
            }
        }
    } else if (ext == ".anim") {
        // ── .anim バイナリ ───────────────────────────────────────────────
        ImGui::TextDisabled("Type: Animation Clip");
        ImGui::Spacing();

        const auto handle = asset::AssetManager::Load<asset::AnimationClip>(absPath);
        if (!handle.IsValid()) {
            ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, "Load failed");
        } else if (const auto* clip = asset::AssetManager::Get(handle)) {
            ImGui::SeparatorText("Info");
            ImGui::LabelText("Name",         "%s",    clip->name.c_str());
            ImGui::LabelText("Duration",     "%.3f s", clip->GetDurationSeconds());
            ImGui::LabelText("FPS",          "%.1f",  clip->frameRate);
            ImGui::LabelText("Tracks",       "%zu",   clip->tracks.size());
            ImGui::LabelText("Loop",         "%s",    clip->loop ? "Yes" : "No");

            // ── Root Motion ────────────────────────────────────────────
            // WHY: 以前は "Yes / No" しか出しておらず、No のとき「専用ノードが
            //      無いのか、名前が候補に入っていないのか」を切り分けられなかった。
            //      解決済みノード名と自動検出の候補を並べて提示する。
            ImGui::LabelText("Root Motion", "%s",
                             clip->hasRootMotion ? "Yes (clip defined)" : "No");
            if (clip->hasRootMotion) {
                ImGui::LabelText("  Node", "%s",
                                 clip->rootMotionNodeName.empty()
                                     ? "(unnamed)"
                                     : clip->rootMotionNodeName.c_str());
                ImGui::LabelText("  Axes", "XZ:%s  Y:%s  Rot:%s",
                                 clip->rootMotionApplyXZ ? "on" : "off",
                                 clip->rootMotionApplyY ? "on" : "off",
                                 clip->rootMotionApplyRotation ? "on" : "off");
            } else {
                const uint32_t candidate = asset::AutoDetectRootMotionTrackIndex(*clip);
                if (candidate < clip->tracks.size()) {
                    ImGui::LabelText("  Candidate", "%s",
                                     clip->tracks[candidate].nodeName.c_str());
                    ImGui::TextDisabled(
                        "Animator の Root Motion Source を Auto Detect にすると使えます。");
                } else {
                    ImGui::TextDisabled(
                        "候補ノードなし。Animator で Node Name を明示指定してください。");
                }
            }

            // ── Events テーブル ────────────────────────────────────────
            if (!clip->events.empty()) {
                ImGui::SeparatorText("Events");
                if (ImGui::BeginTable("##anim_events", 4,
                        ImGuiTableFlags_BordersOuter |
                        ImGuiTableFlags_BordersInnerV |
                        ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("Time",  ImGuiTableColumnFlags_WidthFixed,  52.0f);
                    ImGui::TableSetupColumn("Name",  ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Int",   ImGuiTableColumnFlags_WidthFixed,  36.0f);
                    ImGui::TableSetupColumn("Float", ImGuiTableColumnFlags_WidthFixed,  52.0f);
                    ImGui::TableHeadersRow();
                    for (const auto& ev : clip->events) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("%.3f", ev.time);
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(ev.name.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", ev.intParam);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.2f", ev.floatParam);
                    }
                    ImGui::EndTable();
                }
            } else {
                ImGui::TextDisabled("(no events)");
            }
        }

        // ── Reimport ───────────────────────────────────────────────────
        ImGui::Spacing();
        ImGui::Separator();
        {
            namespace fs = std::filesystem;
            const fs::path animPath  = util::FileSystem::PathFromUtf8(absPath);
            const fs::path parentDir = animPath.parent_path();
            // "Model@Clip.anim" → "Model" が FBX stem
            const std::string stem = util::FileSystem::PathToUtf8(animPath.stem());
            const std::string baseStem = stem.find('@') != std::string::npos
                ? stem.substr(0, stem.find('@')) : stem;
            std::string fbxCandidate;
            for (const auto* fbxExt : {".fbx", ".FBX", ".obj", ".OBJ", ".gltf", ".GLTF", ".glb", ".GLB"}) {
                const fs::path c = parentDir / (baseStem + fbxExt);
                if (util::FileSystem::Exists(c)) { fbxCandidate = util::FileSystem::PathToUtf8(c); break; }
                const fs::path cp = parentDir.parent_path() / (baseStem + fbxExt);
                if (util::FileSystem::Exists(cp)) { fbxCandidate = util::FileSystem::PathToUtf8(cp); break; }
            }
            if (fbxCandidate.empty()) {
                ImGui::TextDisabled("Source: (not found)");
            } else {
                const std::string label =
                    "Reimport from " + util::FileSystem::GetFilename(fbxCandidate);
                if (ImGui::Button(label.c_str()))
                    ctx.requestOpenImportModal = fbxCandidate;
            }
        }

        // ── Preview ────────────────────────────────────────────────────
        // 同じフォルダー階層のモデル (.fbx / .fzasset) を自動解決してクリップを再生する。
        ImGui::SeparatorText("Preview");
        if (!DrawAnimationPreviewWidget(ctx, 240.0f))
            ImGui::TextDisabled("No companion model found for this clip.");
    } else if (ext == ".meta") {
        const std::string sourcePath = absPath.size() > 5 ? absPath.substr(0, absPath.size() - 5) : std::string{};
        const std::string sourceExt = util::StringUtils::ToLower(util::FileSystem::GetExtension(sourcePath));
        if (sourceExt == ".fbx" || sourceExt == ".obj" ||
            sourceExt == ".gltf" || sourceExt == ".glb") {
            ImGui::TextDisabled("Type: Model Import Settings");
            ImGui::Spacing();

            static FbxImportOptions s_modelOptions;
            static std::string      s_modelMetaPath;
            static std::filesystem::file_time_type s_modelMetaWriteTime{};
            std::error_code modelMetaTimeError;
            const auto currentModelMetaWriteTime =
                std::filesystem::last_write_time(absPath, modelMetaTimeError);
            const bool modelMetaExternallyUpdated =
                !modelMetaTimeError &&
                currentModelMetaWriteTime != s_modelMetaWriteTime &&
                !AssetDirtyRegistry::IsDirty(absPath);
            if (s_modelMetaPath != absPath || modelMetaExternallyUpdated) {
                s_modelMetaPath = absPath;
                s_modelOptions = {};
                FbxMetaSerializer::LoadOptions(sourcePath, s_modelOptions);
                s_modelMetaWriteTime = currentModelMetaWriteTime;
            }

            bool dirty = false;
            ImGui::SeparatorText("Source");
            ImGui::TextUnformatted(sourcePath.c_str());

            ImGui::SeparatorText("Model Import Settings");
            static constexpr const char* kSourceDccNames[] = { "Auto Detect", "Maya / FBX SDK", "Blender" };
            int sourceDccIdx = static_cast<int>(s_modelOptions.sourceDcc);
            if (ImGui::Combo("Source DCC", &sourceDccIdx, kSourceDccNames, 3)) {
                s_modelOptions.sourceDcc = static_cast<FbxSourceDcc>(sourceDccIdx);
                dirty = true;
            }

            dirty |= ImGui::Checkbox("Auto-generate texture .meta", &s_modelOptions.generateTexDescriptors);

            static constexpr const char* kConventionNames[] = { "DirectX", "OpenGL" };
            int conventionIdx = static_cast<int>(s_modelOptions.normalMapConvention);
            if (ImGui::Combo("Normal Map Convention", &conventionIdx, kConventionNames, 2)) {
                s_modelOptions.normalMapConvention = static_cast<NormalMapConvention>(conventionIdx);
                dirty = true;
            }

            static constexpr const char* kCompressionNames[] = {
                "Auto", "BC1", "BC3", "BC4", "BC5", "BC6H", "BC7", "None"
            };
            int compressionIdx = static_cast<int>(s_modelOptions.defaultCompression);
            if (ImGui::Combo("Default Texture Compression", &compressionIdx, kCompressionNames, 8)) {
                s_modelOptions.defaultCompression = static_cast<asset::TextureCompression>(compressionIdx);
                dirty = true;
            }

            ImGui::SeparatorText("Sub Assets");
            ImGui::LabelText("Selected Meshes", "%zu", s_modelOptions.selectedMeshNames.size());
            ImGui::LabelText("Selected Animations", "%zu", s_modelOptions.selectedAnimNames.size());
            ImGui::TextDisabled("Mesh / animation selection is edited from Model Import Settings.");

            if (dirty) {
                AssetDirtyRegistry::Register(
                    absPath,
                    util::FileSystem::GetFilename(absPath),
                    "MODEL META",
                    [sourcePath, options = s_modelOptions]() {
                        return FbxMetaSerializer::SaveOptions(sourcePath, options);
                    });
            }

            const bool isModelMetaDirty = AssetDirtyRegistry::IsDirty(absPath) || dirty;
            if (ImGui::Button("Apply") ||
                (isModelMetaDirty && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))) {
                if (FbxMetaSerializer::SaveOptions(sourcePath, s_modelOptions)) {
                    AssetDirtyRegistry::MarkClean(absPath);
                    ctx.requestAssetBrowserRefresh = true;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Revert")) {
                s_modelOptions = {};
                FbxMetaSerializer::LoadOptions(sourcePath, s_modelOptions);
                AssetDirtyRegistry::MarkClean(absPath);
            }
            ImGui::SameLine();
            if (ImGui::Button("Reimport..."))
                ctx.requestOpenImportModal = sourcePath;

            if (isModelMetaDirty) {
                ImGui::SameLine();
                ImGui::TextColored({1.0f, 0.8f, 0.2f, 1.0f}, "Modified");
            }
            return;
        }
        // ── .tex descriptor ──────────────────────────────────────────────
        ImGui::TextDisabled("Type: Texture Import Settings");
        ImGui::Spacing();

        // TexDescSerializer で読み込んで設定を表示・編集できるようにする
        static asset::TextureAsset s_texAsset;
        static std::string         s_texPath;
        static std::filesystem::file_time_type s_texWriteTime{};
        std::error_code texTimeError;
        const auto currentTexWriteTime = std::filesystem::last_write_time(absPath, texTimeError);
        const bool externallyUpdated = !texTimeError
            && currentTexWriteTime != s_texWriteTime
            && !AssetDirtyRegistry::IsDirty(absPath);
        // 「今フレームでディスクから読み直したか」。Sanitize はこのタイミングだけ走らせる。
        // WHY: 毎フレーム正すと、ユーザーが値を触っている最中に書き戻してしまう。
        bool texJustLoaded = false;
        if (s_texPath != absPath || externallyUpdated) {
            s_texPath = absPath;
            s_texAsset = {};
            asset::TexDescSerializer ser;
            if (!ser.Load(absPath, s_texAsset)) {
                s_texAsset.settings = asset::DefaultSettingsForType(
                    asset::GuessTextureType(util::FileSystem::GetFilename(sourcePath)));
            }
            s_texAsset.sourcePath = sourcePath;
            s_texWriteTime = currentTexWriteTime;
            texJustLoaded = true;
        }
        bool dirty = false;

        ImGui::SeparatorText("Source");
        ImGui::TextUnformatted(s_texAsset.sourcePath.c_str());

        ImGui::SeparatorText("Import Settings");
        auto& s = s_texAsset.settings;

        // 元画像の拡張子で分類し、この分類で意味を持つ項目だけを描く。
        // WHY: 以前は全項目を無条件に並べていたため、.hdr に sRGB / BC7、.dds に
        //      Mipmap 生成や Max Size といった「効きようがない設定」が出ていた。
        //      表示の絞り込みと保存値の整合 (Sanitize) を同じ判定表から導く。
        const ImportCategory texCategory = CategoryForExtension(
            util::StringUtils::ToLower(util::FileSystem::GetExtension(sourcePath)));
        if (texJustLoaded) SanitizeTextureSettings(texCategory, s);
        const TextureFieldMask texMask = TextureFieldsFor(texCategory, s);

        ImGui::TextDisabled("Category: %s", ImportCategoryLabel(texCategory));

        // 画像選択から Sprite Editor までを 1 クリックにする。
        // WHY: Unity の Sprite Editor 導線と同様に、Texture Type の変更と .meta 作成を
        //      ユーザーへ別々に要求せず、必要な前処理を安全にまとめて行うため。
        // Sprite として扱えない分類 (.hdr / .exr / .dds) では導線ごと出さない。
        // WHY: 押しても Sanitize で Sprite 型が戻されるだけで、何も起きないボタンになる。
        if (ctx.openSpriteEditor && IsTextureTypeAllowed(texCategory, asset::TextureType::Sprite)) {
            const bool hasPendingInspectorEdits = AssetDirtyRegistry::IsDirty(absPath);
            if (hasPendingInspectorEdits) ImGui::BeginDisabled();
            const char* spriteEditorLabel = s.type == asset::TextureType::Sprite
                ? "Open Sprite Editor"
                : "Convert to Sprite & Open";
            if (ImGui::Button(spriteEditorLabel)) {
                if (s.type != asset::TextureType::Sprite)
                    s = asset::DefaultSettingsForType(asset::TextureType::Sprite);
                if (s.sprites.empty()) {
                    asset::SpriteRect sprite;
                    sprite.id = util::GenerateUUID();
                    sprite.name = util::FileSystem::PathToUtf8(
                        util::FileSystem::PathFromUtf8(sourcePath).stem());
                    s.sprites.push_back(std::move(sprite));
                }
                asset::TexDescSerializer serializer;
                if (serializer.Save(s_texAsset, absPath)) {
                    AssetDirtyRegistry::MarkClean(absPath);
                    ctx.requestAssetBrowserRefresh = true;
                    ctx.openSpriteEditor(absPath);
                }
            }
            if (hasPendingInspectorEdits) ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip(hasPendingInspectorEdits
                    ? "Apply or Revert the current Import Settings first"
                    : "Create Sprite import settings and open Sprite Editor");
            }
            ImGui::Spacing();
        }

        if (DrawTextureTypeCombo("Type", texCategory, s.type)) {
            s = asset::DefaultSettingsForType(s.type); // デフォルトを再適用
            SanitizeTextureSettings(texCategory, s);
            dirty = true;
        }
        // sRGB はガンマ付きカラーにしか意味がない。法線・データマスク・HDR では出さない。
        if (texMask.srgb)
            dirty |= ImGui::Checkbox("sRGB", &s.srgb);

        if (texMask.sprite) {
            ImGui::SeparatorText("Sprite");
            static constexpr const char* kSpriteModeNames[] = { "Single", "Multiple" };
            int spriteMode = static_cast<int>(s.spriteMode);
            if (ImGui::Combo("Sprite Mode", &spriteMode, kSpriteModeNames, 2)) {
                s.spriteMode = static_cast<asset::SpriteMode>(spriteMode);
                dirty = true;
            }
            dirty |= ImGui::DragFloat(
                "Pixels Per Unit", &s.pixelsPerUnit, 1.0f, 0.001f, 10000.0f, "%.1f");

            const std::string imagePath =
                absPath.size() > 5 ? absPath.substr(0, absPath.size() - 5) : std::string{};
            const std::string baseName = util::FileSystem::PathToUtf8(
                util::FileSystem::PathFromUtf8(imagePath).stem());
            if (s.sprites.empty()) {
                asset::SpriteRect sprite;
                sprite.id = util::GenerateUUID();
                sprite.name = baseName.empty() ? "Sprite" : baseName;
                s.sprites.push_back(std::move(sprite));
                dirty = true;
            }
            if (s.spriteMode == asset::SpriteMode::Single && s.sprites.size() > 1) {
                s.sprites.resize(1);
                dirty = true;
            }

            if (s.spriteMode == asset::SpriteMode::Multiple) {
                static int s_sliceColumns = 1;
                static int s_sliceRows = 1;
                ImGui::SeparatorText("Slice");
                ImGui::SetNextItemWidth(90.0f);
                ImGui::InputInt("Columns", &s_sliceColumns);
                ImGui::SetNextItemWidth(90.0f);
                ImGui::InputInt("Rows", &s_sliceRows);
                s_sliceColumns = std::clamp(s_sliceColumns, 1, 64);
                s_sliceRows = std::clamp(s_sliceRows, 1, 64);

                uint32_t textureWidth = 0;
                uint32_t textureHeight = 0;
                if (ctx.resources != nullptr) {
                    const auto textureHandle = ctx.resources->LoadTexture(imagePath);
                    if (const renderer::ITexture* texture = ctx.resources->Get(textureHandle)) {
                        textureWidth = texture->GetWidth();
                        textureHeight = texture->GetHeight();
                    }
                }
                const bool canSlice = textureWidth > 0 && textureHeight > 0;
                if (!canSlice) ImGui::BeginDisabled();
                if (ImGui::Button("Slice Grid")) {
                    const int sliceColumns = std::min(
                        s_sliceColumns, static_cast<int>(textureWidth));
                    const int sliceRows = std::min(
                        s_sliceRows, static_cast<int>(textureHeight));
                    s.sprites.clear();
                    const uint32_t cellWidth =
                        textureWidth / static_cast<uint32_t>(sliceColumns);
                    const uint32_t cellHeight =
                        textureHeight / static_cast<uint32_t>(sliceRows);
                    for (int row = 0; row < sliceRows; ++row) {
                        for (int column = 0; column < sliceColumns; ++column) {
                            asset::SpriteRect sprite;
                            sprite.id = util::GenerateUUID();
                            sprite.name = baseName + "_" + std::to_string(
                                row * sliceColumns + column);
                            sprite.x = static_cast<uint32_t>(column) * cellWidth;
                            sprite.y = static_cast<uint32_t>(row) * cellHeight;
                            sprite.width = column == sliceColumns - 1
                                ? textureWidth - sprite.x : cellWidth;
                            sprite.height = row == sliceRows - 1
                                ? textureHeight - sprite.y : cellHeight;
                            s.sprites.push_back(std::move(sprite));
                        }
                    }
                    dirty = true;
                }
                if (!canSlice) ImGui::EndDisabled();
                if (!canSlice) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("Texture preview must be loadable");
                }
                ImGui::SameLine();
                if (ImGui::Button("Add Sprite")) {
                    asset::SpriteRect sprite;
                    sprite.id = util::GenerateUUID();
                    sprite.name = baseName + "_" + std::to_string(s.sprites.size());
                    s.sprites.push_back(std::move(sprite));
                    dirty = true;
                }
            }

            ImGui::SeparatorText("Sprite Rects");
            int removeIndex = -1;
            for (int i = 0; i < static_cast<int>(s.sprites.size()); ++i) {
                asset::SpriteRect& sprite = s.sprites[static_cast<size_t>(i)];
                ImGui::PushID(i);
                const std::string header = sprite.name.empty()
                    ? "Sprite " + std::to_string(i) : sprite.name;
                if (ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    char nameBuffer[256];
                    std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", sprite.name.c_str());
                    const std::string nameBeforeEdit = sprite.name;
                    const bool spriteNameChanged =
                        ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer));
                    if (spriteNameChanged) {
                        sprite.name = nameBuffer;
                        dirty = true;
                    }
                    if (s.spriteMode == asset::SpriteMode::Multiple) {
                        int rect[4] = {
                            static_cast<int>(sprite.x), static_cast<int>(sprite.y),
                            static_cast<int>(sprite.width), static_cast<int>(sprite.height)
                        };
                        if (ImGui::InputInt4("Rect (x,y,w,h)", rect)) {
                            sprite.x = static_cast<uint32_t>(std::max(0, rect[0]));
                            sprite.y = static_cast<uint32_t>(std::max(0, rect[1]));
                            sprite.width = static_cast<uint32_t>(std::max(0, rect[2]));
                            sprite.height = static_cast<uint32_t>(std::max(0, rect[3]));
                            dirty = true;
                        }
                    } else {
                        ImGui::TextDisabled("Rect: Full Texture");
                    }
                    float pivot[2] = { sprite.pivotX, sprite.pivotY };
                    if (widgets::DragAxes("Pivot", pivot, 2, 0.01f, 0.0f, 1.0f)) {
                        sprite.pivotX = std::clamp(pivot[0], 0.0f, 1.0f);
                        sprite.pivotY = std::clamp(pivot[1], 0.0f, 1.0f);
                        dirty = true;
                    }
                    float border[4] = {
                        sprite.borderLeft, sprite.borderTop,
                        sprite.borderRight, sprite.borderBottom
                    };
                    if (ImGui::DragFloat4("Border (L,T,R,B)", border, 1.0f, 0.0f, 16384.0f)) {
                        sprite.borderLeft = std::max(0.0f, border[0]);
                        sprite.borderTop = std::max(0.0f, border[1]);
                        sprite.borderRight = std::max(0.0f, border[2]);
                        sprite.borderBottom = std::max(0.0f, border[3]);
                        dirty = true;
                    }
                    if (s.spriteMode == asset::SpriteMode::Multiple
                        && ImGui::Button("Remove")) {
                        removeIndex = i;
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            if (removeIndex >= 0) {
                s.sprites.erase(s.sprites.begin() + removeIndex);
                dirty = true;
            }
        }

        // ── Compression ───────────────────────────────────────────────
        // .dds は既にブロック圧縮済みで再エンコード経路を持たないため、セクションごと出さない。
        if (texMask.compression) {
            ImGui::SeparatorText("Compression");
            // 選択肢は型ごとに絞る (法線に BC1、HDR に BC7 等は破綻するため出さない)。
            if (DrawCompressionCombo("Format", texCategory, s.type, s.compression))
                dirty = true;
            static constexpr const char* kQualNames[] = { "Fast", "Normal", "High" };
            int qualIdx = static_cast<int>(s.compressionQuality);
            if (ImGui::Combo("Quality", &qualIdx, kQualNames, 3)) {
                s.compressionQuality = static_cast<asset::CompQuality>(qualIdx);
                dirty = true;
            }
        }

        // ── Mipmaps ───────────────────────────────────────────────────
        if (texMask.mipmaps) {
            ImGui::SeparatorText("Mipmaps");
            dirty |= ImGui::Checkbox("Mipmaps", &s.mipmaps);
            if (texMask.mipDetail) {
                static constexpr const char* kMipFilterNames[] = { "Box", "Kaiser", "Lanczos" };
                int mipFIdx = static_cast<int>(s.mipFilter);
                if (ImGui::Combo("Mip Filter", &mipFIdx, kMipFilterNames, 3)) {
                    s.mipFilter = static_cast<asset::MipFilter>(mipFIdx);
                    dirty = true;
                }
                dirty |= ImGui::DragFloat("Mip Sharpen", &s.mipSharpen, 0.01f, 0.0f, 4.0f);
                dirty |= ImGui::DragFloat("Mip Bias",    &s.mipBias,    0.01f, -4.0f, 4.0f);
            }
            // 縮小で崩れた法線の再正規化。法線マップ以外では効果がない。
            if (texMask.normalizeMips)
                dirty |= ImGui::Checkbox("Normalize Mipmaps", &s.normalizeMipmaps);
        }

        // ── Normal Map ────────────────────────────────────────────────
        if (texMask.flipGreen) {
            ImGui::SeparatorText("Normal Map");
            dirty |= ImGui::Checkbox("Flip Green (OpenGL)", &s.flipGreen);
        }

        // ── Sampling ─────────────────────────────────────────────────
        // サンプラー状態は .dds でも実行時に効くので、圧縮を出さない分類でも表示する。
        if (texMask.sampling) {
            ImGui::SeparatorText("Sampling");
            static constexpr const char* kWrapNames[] = { "Repeat", "Clamp", "Mirror", "Border" };
            int wrapU = static_cast<int>(s.wrapU);
            int wrapV = static_cast<int>(s.wrapV);
            if (ImGui::Combo("Wrap U", &wrapU, kWrapNames, 4)) { s.wrapU = static_cast<asset::TextureWrap>(wrapU); dirty = true; }
            if (ImGui::Combo("Wrap V", &wrapV, kWrapNames, 4)) { s.wrapV = static_cast<asset::TextureWrap>(wrapV); dirty = true; }
            int anisoLv = static_cast<int>(s.anisoLevel);
            if (ImGui::SliderInt("Aniso Level", &anisoLv, 1, 16)) {
                s.anisoLevel = static_cast<uint32_t>(anisoLv);
                dirty = true;
            }
        }

        // ── Resolution ────────────────────────────────────────────────
        if (texMask.maxSize) {
            ImGui::SeparatorText("Resolution");
            int maxSize = static_cast<int>(s.maxSize);
            if (ImGui::InputInt("Max Size", &maxSize)) {
                s.maxSize = static_cast<uint32_t>(std::max(1, maxSize));
                dirty = true;
            }
        }

        // ── Alpha ─────────────────────────────────────────────────────
        // 法線・データマスクのアルファはチャンネルとして使うだけで、合成方式の概念がない。
        if (texMask.alpha) {
            ImGui::SeparatorText("Alpha");
            static constexpr const char* kAlphaNames[] = { "Straight", "Premultiplied", "None" };
            int alphaIdx = static_cast<int>(s.alphaMode);
            if (ImGui::Combo("Alpha Mode", &alphaIdx, kAlphaNames, 3)) {
                s.alphaMode = static_cast<asset::AlphaMode>(alphaIdx);
                dirty = true;
            }
            dirty |= ImGui::Checkbox("Alpha Dither", &s.alphaDither);
        }

        ImGui::Spacing();
        const bool isTxDirty = AssetDirtyRegistry::IsDirty(absPath) || dirty;
        if (dirty) {
            AssetDirtyRegistry::Register(
                absPath,
                util::FileSystem::GetFilename(absPath),
                "TEX",
                [path = absPath, textureAsset = s_texAsset]() {
                    asset::TexDescSerializer ser;
                    return ser.Save(textureAsset, path);
                });
        }

        if (ImGui::Button("Apply") || (isTxDirty && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))) {
            asset::TexDescSerializer ser;
            if (ser.Save(s_texAsset, absPath)) {
                AssetDirtyRegistry::MarkClean(absPath);
                ctx.requestAssetBrowserRefresh = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert")) {
            asset::TexDescSerializer ser;
            s_texAsset = {};
            if (!ser.Load(absPath, s_texAsset)) {
                s_texAsset.settings = asset::DefaultSettingsForType(
                    asset::GuessTextureType(util::FileSystem::GetFilename(sourcePath)));
            }
            s_texAsset.sourcePath = sourcePath;
            AssetDirtyRegistry::MarkClean(absPath);
        }
        if (isTxDirty) {
            ImGui::SameLine();
            ImGui::TextColored({1.0f, 0.8f, 0.2f, 1.0f}, "Modified");
        }
    } else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
               ext == ".dds" || ext == ".tga" || ext == ".bmp" ||
               ext == ".hdr" || ext == ".exr") {
        ImGui::TextDisabled("Type: Texture (%s)", ext.c_str());
        ImGui::Spacing();

        // 元画像の隣に "<画像>.meta" サイドカーが存在するか確認 (二重拡張子)
        const std::string texDescPath = absPath + ".meta";
        const bool hasTexDesc = util::FileSystem::Exists(
            util::FileSystem::PathFromUtf8(texDescPath));

        if (hasTexDesc) {
            ImGui::TextColored({0.3f, 0.9f, 0.3f, 1.0f}, ".meta sidecar: found");
            ImGui::TextDisabled("%s", util::FileSystem::GetFilename(texDescPath).c_str());
            ImGui::Spacing();
            if (ImGui::Button("Open .meta Inspector"))
                ctx.selectedAssetPath = texDescPath;
            static std::string s_cachedSpriteMetaPath;
            static std::filesystem::file_time_type s_cachedSpriteMetaTime{};
            static bool s_cachedIsSprite = false;
            std::error_code spriteMetaTimeError;
            const auto spriteMetaTime = std::filesystem::last_write_time(
                texDescPath, spriteMetaTimeError);
            if (s_cachedSpriteMetaPath != texDescPath
                || (!spriteMetaTimeError && s_cachedSpriteMetaTime != spriteMetaTime)) {
                s_cachedSpriteMetaPath = texDescPath;
                s_cachedSpriteMetaTime = spriteMetaTime;
                asset::TextureAsset textureAsset;
                asset::TexDescSerializer serializer;
                s_cachedIsSprite = serializer.Load(texDescPath, textureAsset)
                    && textureAsset.settings.type == asset::TextureType::Sprite;
            }
            if (s_cachedIsSprite && ctx.openSpriteEditor) {
                ImGui::SameLine();
                if (ImGui::Button("Open Sprite Editor"))
                    ctx.openSpriteEditor(texDescPath);
            }
        } else {
            ImGui::TextColored({1.0f, 0.8f, 0.2f, 1.0f},
                "No .meta sidecar for this image.");
            ImGui::Spacing();
            ImGui::TextWrapped(
                "Creating a .meta lets you control sRGB, compression, "
                "mipmaps and other import settings.");
            ImGui::Spacing();
            if (ImGui::Button("Create .meta...")) {
                // GuessTextureType でデフォルト設定を生成して "<画像>.meta" を書き出す
                const asset::TextureType guessedType =
                    asset::GuessTextureType(util::FileSystem::GetFilename(absPath));
                asset::TextureAsset newAsset;
                newAsset.settings   = asset::DefaultSettingsForType(guessedType);
                asset::TexDescSerializer ser;
                if (ser.Save(newAsset, texDescPath)) {
                    ctx.selectedAssetPath     = texDescPath;
                    ctx.requestAssetBrowserRefresh = true;
                }
            }
            ImGui::Spacing();
            ImGui::TextDisabled(
                "Or drag directly to a material slot to use with default settings.");
        }
    } else if (ext == ".mask") {
        // ── Avatar Mask (.mask) ───────────────────────────────────────────
        // WHY: 上半身だけ / 下半身だけの制御はこのアセットが入口。ボーンパスを手書きさせず、
        //      Humanoid プリセットで一括生成し、必要なら個別に weight と blendDepth を詰める。
        ImGui::TextDisabled("Type: Avatar Mask");
        ImGui::Spacing();

        static asset::AvatarMaskAsset s_mask;
        static std::string            s_maskPath;
        static std::filesystem::file_time_type s_maskWriteTime{};
        std::error_code maskTimeError;
        const auto currentMaskWriteTime = std::filesystem::last_write_time(absPath, maskTimeError);
        const bool maskExternallyUpdated = !maskTimeError
            && currentMaskWriteTime != s_maskWriteTime
            && !AssetDirtyRegistry::IsDirty(absPath);
        if (s_maskPath != absPath || maskExternallyUpdated) {
            s_maskPath = absPath;
            s_mask = asset::AvatarMaskAsset{};
            if (!asset::LoadAvatarMaskAsset(absPath, s_mask))
                s_mask.name = util::FileSystem::GetFilename(absPath);
            s_maskWriteTime = currentMaskWriteTime;
        }
        bool maskDirty = false;

        ImGui::SeparatorText("Mask");
        // defaultInclude を切り替えると「列挙したボーンだけ有効」と「列挙したボーンだけ無効」が
        // 反転する。上半身マスクは前者、指だけ抜くマスクは後者が書きやすい。
        if (ImGui::Checkbox("Default Include (未列挙のボーンも有効にする)", &s_mask.defaultInclude))
            maskDirty = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip(
                "OFF: 下の一覧に無いボーンは weight 0 (上半身だけ有効、などに使う)\n"
                "ON : 下の一覧に無いボーンは weight 1 (指だけ除外、などに使う)");
        }

        // ── Humanoid プリセット ───────────────────────────────────────────
        // 参照スケルトンがあればボーン名から実体を拾い、無ければ体パーツ名を
        // そのままボーン名エントリとして置く (ボーン名部分一致でランタイムが解決する)。
        ImGui::SeparatorText("Humanoid Presets");
        if (widgets::AssetPathField("Skeleton Source", s_mask.skeletonSourcePath,
                                    ".fbx", ctx.projectRoot))
            maskDirty = true;

        const asset::Model* maskModel = s_mask.skeletonSourcePath.empty()
            ? nullptr : asset::AssetManager::LoadModel(s_mask.skeletonSourcePath);
        const asset::Skeleton* maskSkeleton =
            (maskModel && maskModel->skeleton) ? maskModel->skeleton.get() : nullptr;

        // 指定パーツに属するボーンを weight で登録する。
        const auto applyBodyPart = [&](asset::HumanoidBodyPart part, float weight) {
            const auto upsert = [&](const std::string& bone) {
                for (auto& e : s_mask.entries) {
                    if (e.bonePath == bone) { e.weight = weight; return; }
                }
                asset::AvatarMaskEntry entry;
                entry.bonePath = bone;
                entry.weight = weight;
                entry.includeChildren = false; // パーツ単位で個別に列挙するため子孫は含めない
                s_mask.entries.push_back(std::move(entry));
            };
            if (maskSkeleton) {
                for (const auto& node : maskSkeleton->nodes)
                    if (asset::BoneNameMatchesBodyPart(node.name, part)) upsert(node.name);
            } else {
                // スケルトン未指定でも最低限使えるよう、パーツ名をそのまま登録する。
                upsert(asset::HumanoidBonePatterns(part).empty()
                       ? std::string(asset::HumanoidBodyPartName(part))
                       : asset::HumanoidBonePatterns(part).front());
            }
            maskDirty = true;
        };

        for (int p = 0; p < static_cast<int>(asset::HumanoidBodyPart::Count); ++p) {
            const auto part = static_cast<asset::HumanoidBodyPart>(p);
            ImGui::PushID(p);
            if (ImGui::SmallButton("+")) applyBodyPart(part, 1.0f);
            ImGui::SameLine();
            if (ImGui::SmallButton("-")) applyBodyPart(part, 0.0f);
            ImGui::SameLine();
            ImGui::TextUnformatted(asset::HumanoidBodyPartName(part));
            ImGui::PopID();
            if (p % 2 == 0 && p + 1 < static_cast<int>(asset::HumanoidBodyPart::Count))
                ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.5f);
        }

        // 上半身 / 下半身は最頻出なのでワンボタンで用意する。
        ImGui::Spacing();
        if (ImGui::Button("Upper Body Preset")) {
            s_mask.entries.clear();
            s_mask.defaultInclude = false;
            // Spine から下へ 3 階層かけて立ち上げ、腰の継ぎ目を目立たなくする。
            asset::AvatarMaskEntry spine;
            spine.bonePath = "Spine";
            spine.weight = 1.0f;
            spine.includeChildren = true;
            spine.blendDepth = 3;
            s_mask.entries.push_back(std::move(spine));
            maskDirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Lower Body Preset")) {
            s_mask.entries.clear();
            s_mask.defaultInclude = false;
            for (const char* bone : { "Hips", "LeftUpLeg", "RightUpLeg" }) {
                asset::AvatarMaskEntry entry;
                entry.bonePath = bone;
                entry.weight = 1.0f;
                entry.includeChildren = true;
                s_mask.entries.push_back(std::move(entry));
            }
            // 上半身側を明示的に 0 にして、Hips 配下の背骨が巻き込まれないようにする。
            asset::AvatarMaskEntry spine;
            spine.bonePath = "Spine";
            spine.weight = 0.0f;
            spine.includeChildren = true;
            s_mask.entries.push_back(std::move(spine));
            maskDirty = true;
        }

        // ── エントリ一覧 ──────────────────────────────────────────────────
        ImGui::SeparatorText("Bones");
        int removeEntry = -1;
        for (int i = 0; i < static_cast<int>(s_mask.entries.size()); ++i) {
            auto& entry = s_mask.entries[static_cast<size_t>(i)];
            ImGui::PushID(i);

            char boneBuffer[256];
            std::snprintf(boneBuffer, sizeof(boneBuffer), "%s", entry.bonePath.c_str());
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
            if (ImGui::InputText("##bone", boneBuffer, sizeof(boneBuffer))) {
                entry.bonePath = boneBuffer;
                maskDirty = true;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("ボーン名 (\"Spine1\") またはパス (\"Hips/Spine/Spine1\")");

            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::SliderFloat("##w", &entry.weight, 0.0f, 1.0f, "w %.2f"))
                maskDirty = true;

            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::DragInt("##depth", &entry.blendDepth, 0.1f, 0, 8, "depth %d"))
                maskDirty = true;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip(
                    "0 なら配下一律。1 以上でこのボーンから下へ段階的に weight を立ち上げ、\n"
                    "上半身/下半身の境界でポーズが折れるのを防ぎます。");
            }

            ImGui::SameLine();
            if (ImGui::Checkbox("children", &entry.includeChildren)) maskDirty = true;
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) removeEntry = i;

            ImGui::PopID();
        }
        if (removeEntry >= 0) {
            s_mask.entries.erase(s_mask.entries.begin() + removeEntry);
            maskDirty = true;
        }
        if (ImGui::Button("Add Bone Entry")) {
            s_mask.entries.push_back(asset::AvatarMaskEntry{});
            maskDirty = true;
        }

        // ── スケルトンのボーンツリー (実効ウェイトのプレビュー) ─────────────
        if (maskSkeleton) {
            ImGui::SeparatorText("Skeleton Preview");
            ImGui::TextDisabled("各ボーンにこのマスクを適用したときの実効ウェイトです。");
            ImGui::BeginChild("##mask_bone_tree", { 0.0f, 220.0f }, true);
            for (const auto& node : maskSkeleton->nodes) {
                const float w = asset::EvaluateAvatarMaskWeight(s_mask, node.name, node.name);
                // 効いているボーンだけ色を付け、無効ボーンは沈める。
                const ImVec4 color = w > 0.001f
                    ? ImVec4(0.55f + 0.45f * w, 0.85f, 0.55f, 1.0f)
                    : ImVec4(0.45f, 0.45f, 0.45f, 1.0f);
                ImGui::TextColored(color, "%-32s  %.2f", node.name.c_str(), w);
            }
            ImGui::EndChild();
        } else if (!s_mask.skeletonSourcePath.empty()) {
            ImGui::TextDisabled("スケルトンを読み込めませんでした。");
        }

        // ── 保存 ─────────────────────────────────────────────────────────
        ImGui::Spacing();
        const bool isMaskDirty = AssetDirtyRegistry::IsDirty(absPath) || maskDirty;
        if (maskDirty) {
            AssetDirtyRegistry::Register(
                absPath, util::FileSystem::GetFilename(absPath), "MASK",
                [path = absPath, maskCopy = s_mask]() {
                    return asset::SaveAvatarMaskAsset(path, maskCopy);
                });
        }
        if (ImGui::Button("Apply") ||
            (isMaskDirty && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))) {
            if (asset::SaveAvatarMaskAsset(absPath, s_mask)) {
                AssetDirtyRegistry::MarkClean(absPath);
                ctx.requestAssetBrowserRefresh = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert")) {
            s_mask = asset::AvatarMaskAsset{};
            (void)asset::LoadAvatarMaskAsset(absPath, s_mask);
            AssetDirtyRegistry::MarkClean(absPath);
        }
        if (isMaskDirty) {
            ImGui::SameLine();
            ImGui::TextColored({ 1.0f, 0.8f, 0.2f, 1.0f }, "Modified");
        }
    } else if (ext == ".terrain") {
        // ── .terrain アセット ─────────────────────────────────────────────
        // WHY: 実際のオンディスク形式は scene::TerrainAssetSerializer が読み書きする TOML。
        //      SceneSerializer / ランタイムもこのシリアライザ経由でロードするため、
        //      Inspector も同じ TerrainAssetSerializer を使って読み書きを一致させる。
        //      （旧 asset::FzTerrainSerializer はバイナリ "FZTN" 形式で、TOML の .terrain を
        //        読めず "Load failed"、保存すると TOML ファイルをバイナリに破壊していた。）
        ImGui::TextDisabled("Type: Terrain Asset");
        ImGui::Spacing();

        static scene::TerrainComponent s_terrain;
        static std::string             s_terrainPath;
        static bool                    s_terrainLoaded = false;
        bool terrainDirty = false;

        if (s_terrainPath != absPath) {
            s_terrainPath   = absPath;
            s_terrain       = scene::TerrainComponent{};
            s_terrainLoaded = scene::TerrainAssetSerializer::Load(absPath, s_terrain);
        }

        if (!s_terrainLoaded) {
            ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, "Load failed");
        } else {
            auto& ta = s_terrain;

            ImGui::SeparatorText("Geometry");
            ImGui::LabelText("Columns",    "%d", ta.columns);
            ImGui::LabelText("Rows",       "%d", ta.rows);
            ImGui::LabelText("Cell Size",  "%.2f m", ta.cellSize);
            ImGui::LabelText("Max Height", "%.2f m", ta.maxHeight);
            ImGui::LabelText("Chunk Size", "%d", ta.chunkSize);
            const float worldW = static_cast<float>(ta.columns - 1) * ta.cellSize;
            const float worldH = static_cast<float>(ta.rows    - 1) * ta.cellSize;
            ImGui::LabelText("World Size", "%.0f m × %.0f m", worldW, worldH);

            if (!ta.heightData.empty()) {
                ImGui::SeparatorText("Height Data");
                float hMin =  1e9f, hMax = -1e9f;
                for (float v : ta.heightData) {
                    if (v < hMin) hMin = v;
                    if (v > hMax) hMax = v;
                }
                const float hRange = ta.maxHeight;
                ImGui::LabelText("Points", "%zu", ta.heightData.size());
                ImGui::LabelText("Min",    "%.2f m", hMin * hRange);
                ImGui::LabelText("Max",    "%.2f m", hMax * hRange);
            }

            ImGui::SeparatorText("Layer Materials");
            for (int li = 0; li < static_cast<int>(ta.layerMaterials.size()); ++li) {
                ImGui::PushID(li);
                const std::string layerLabel = "Layer " + std::to_string(li);
                if (widgets::AssetPathField(layerLabel.c_str(), ta.layerMaterials[li], ".mat", ctx.projectRoot))
                    terrainDirty = true;
                ImGui::PopID();
            }

            if (terrainDirty) {
                AssetDirtyRegistry::Register(
                    absPath,
                    util::FileSystem::GetFilename(absPath),
                    "TERRAIN",
                    [path = absPath]() {
                        return scene::TerrainAssetSerializer::Save(s_terrain, path);
                    });
            }

            ImGui::Spacing();
            const bool isTerrainDirty = AssetDirtyRegistry::IsDirty(absPath) || terrainDirty;
            if (ImGui::Button("Save .terrain") ||
                (isTerrainDirty && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))) {
                if (scene::TerrainAssetSerializer::Save(ta, absPath))
                    AssetDirtyRegistry::MarkClean(absPath);
            }
            if (isTerrainDirty) {
                ImGui::SameLine();
                ImGui::TextColored({1.0f, 0.8f, 0.2f, 1.0f}, "Modified");
            }
        }
    } else if (ext == ".fzdata") {
        // ── DataAsset (純共有 ScriptableObject) ───────────────────────────
        const std::string relPath = NormalizeAssetPath(absPath);
        asset::DataAsset* data = asset::DataAssetRegistry::Resolve(relPath);
        if (!data) {
            ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f},
                "Failed to load .fzdata (型が未登録か、パース失敗)");
            return;
        }

        ImGui::TextDisabled("Type: %s", data->GetTypeName());
        ImGui::Separator();

        // 編集前の状態を控える。DataAsset は多態基底で値コピーできないため、
        // .mat のような「構造体まるごとのコピー」ではなく TOML 直列化を控えに使う。
        // WHY 毎フレーム取るか: どのウィジェットが掴まれるかは描画前には分からず、
        //     掴まれた瞬間に「その直前の値」が必要になる。文字列 1 本ぶんの
        //     コストで、選択中の 1 アセットに対してだけ走る。
        const bool canRecordFzDataUndo = CanRecordEditorUndo(ctx);
        const std::string fzdataBeforeDraw = canRecordFzDataUndo
            ? asset::DataAssetRegistry::Snapshot(relPath)
            : std::string{};
        const ImGuiID fzdataActiveBefore = ImGui::GetActiveID();

        // 各オーバーライドのパラメーターは共通リフレクタで描く。
        // WHY 共通のものを使うか: Reflect() が既にレンジとカラーヒントを持っており、
        //     ImGuiReflector はそれをプロパティ行・カラーピッカー・ファイルスロットへ
        //     翻訳する。効果ごとの専用 UI を書かずに済み、効果の追加は Engine 側だけで閉じる。
        ImGuiReflector reflector;
        reflector.m_projectRoot = ctx.projectRoot;

        bool editedByCustomUi = false;
        if (auto* profile = dynamic_cast<asset::PostProcessProfile*>(data)) {
            // PostProcessProfile だけは専用 UI を使う。
            // WHY 汎用リフレクタに任せないか: 中身は「効果のリスト」で、
            //     追加・削除・並べ替え・一時無効化という配列固有の操作が要る。
            //     汎用のフィールド列挙ではそれらを表現できない。
            //     編集対象は Registry の共有実体そのものなので、変更は全参照へ即反映される。
            const PostProcessInspectorResult inspectorResult =
                DrawVolumeOverrideListInspector(*profile, reflector);
            editedByCustomUi = inspectorResult.changed;
        } else {
            // Inspector の共通リフレクタでフィールドを描画する (スクリプトと同じ UI)。
            data->Reflect(reflector);
        }

        // 自動保存: 値が編集され、かつ操作 (ドラッグ/入力) が終わった瞬間にディスクへ書き戻す。
        // WHY: ScriptableObject 的な「いじったら保存されている」体験にする。連続ドラッグ中の
        //      大量書き込みは避けたいので、アクティブ操作が無くなったフレームでだけ保存する。
        const bool editedThisFrame =
            (GImGui && GImGui->ActiveIdHasBeenEditedThisFrame) || editedByCustomUi;
        static bool        s_fzdataDirty = false;
        static std::string s_fzdataDirtyPath;
        if (editedThisFrame) {
            s_fzdataDirty     = true;
            s_fzdataDirtyPath = relPath;
        }
        if (s_fzdataDirty && s_fzdataDirtyPath == relPath && !ImGui::IsAnyItemActive()) {
            if (asset::DataAssetRegistry::Save(relPath))
                ctx.requestAssetBrowserRefresh = true;
            s_fzdataDirty = false;
        }

        // ── Undo 記録 ────────────────────────────────────────────────────────
        // WHY: .fzdata だけ Undo が一切効かず、しかも自動保存でディスクへ即書き戻すため、
        //      値を壊すと戻す手段が無かった (Ctrl+Z を押しても無関係な履歴が戻るだけ)。
        //      記録の粒度は .mat と同じ「ウィジェットを掴んでから離すまで = 1 操作」。
        //      フレーム単位で積むとドラッグ 1 回が数十件の中間値で履歴を埋めてしまう。
        struct FzDataUndoTracker {
            std::string path;      // どの .fzdata に対する記録か
            std::string before;    // 掴んだ直前のスナップショット
            ImGuiID     activeId = 0;
            bool        active   = false;
            bool        changed  = false;
        };
        static FzDataUndoTracker fzdataUndo;
        const ImGuiID fzdataActiveAfter = ImGui::GetActiveID();

        auto pushFzDataCommand = [&ctx, &relPath](const std::string& before,
                                                  const std::string& after) {
            if (!ctx.undoStack || before.empty() || before == after) return;
            EditorContext*    context      = &ctx;
            const std::string capturedPath = relPath;
            auto apply = [context, capturedPath](const std::string& snapshot) {
                if (!asset::DataAssetRegistry::RestoreSnapshot(capturedPath, snapshot)) return;
                // 復元した値はディスクへも書き戻す。
                // WHY: .fzdata は編集確定ごとに自動保存される。メモリだけ戻すと
                //      次のロードやホットリロードで巻き戻り、「Undo したのに直っていない」
                //      という一番たちの悪い壊れ方になる (.mat と同じ規則へ揃える)。
                asset::DataAssetRegistry::Save(capturedPath);
                context->requestAssetBrowserRefresh = true;
            };
            ctx.undoStack->Push(std::make_unique<LambdaCommand>(
                "Edit Data Asset",
                [apply, after]()  { apply(after); },
                [apply, before]() { apply(before); }));
        };

        if (!canRecordFzDataUndo) {
            fzdataUndo.active = false;
        } else {
            // 選択が別の .fzdata へ移ったら記録途中の操作は捨てる
            // (別アセットの値で before/after が混ざるのを防ぐ)。
            if (fzdataUndo.active && fzdataUndo.path != relPath)
                fzdataUndo.active = false;

            if (!fzdataUndo.active) {
                if (fzdataActiveAfter != 0 && fzdataActiveAfter != fzdataActiveBefore) {
                    fzdataUndo.path     = relPath;
                    fzdataUndo.before   = fzdataBeforeDraw;
                    fzdataUndo.activeId = fzdataActiveAfter;
                    fzdataUndo.active   = true;
                    fzdataUndo.changed  = editedThisFrame;
                } else if (editedThisFrame) {
                    // 掴まずに 1 フレームで確定した編集 (オーバーライドの追加・削除など)。
                    // 掴み→離しの経路に乗らないので、その場で 1 操作として積む。
                    pushFzDataCommand(fzdataBeforeDraw,
                                      asset::DataAssetRegistry::Snapshot(relPath));
                }
            } else if (fzdataActiveAfter == fzdataUndo.activeId) {
                fzdataUndo.changed |= editedThisFrame;   // ドラッグ継続中
            } else {
                // 離したフレーム。ボタンは「離した瞬間」に効くので、この 1 回ぶんも拾う。
                fzdataUndo.changed |= editedThisFrame;
                if (fzdataUndo.changed)
                    pushFzDataCommand(fzdataUndo.before,
                                      asset::DataAssetRegistry::Snapshot(relPath));
                fzdataUndo.active  = false;
                fzdataUndo.changed = false;
                // 離した直後に別ウィジェットを掴んでいたら、そこから記録し直す。
                if (fzdataActiveAfter != 0) {
                    fzdataUndo.path     = relPath;
                    fzdataUndo.before   = asset::DataAssetRegistry::Snapshot(relPath);
                    fzdataUndo.activeId = fzdataActiveAfter;
                    fzdataUndo.active   = true;
                }
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        // 保険の手動保存 (自動保存があるので通常は不要)。
        if (ImGui::Button("Save .fzdata"))
            asset::DataAssetRegistry::Save(relPath);
        ImGui::SameLine();
        ImGui::TextDisabled(s_fzdataDirty && s_fzdataDirtyPath == relPath
                            ? "Saving on release..." : "Auto-saved");
    } else if (ext == ".physmat") {
        // ── PhysicsMaterial (共有物理マテリアル) ──────────────────────────
        // WHY AssetManager 上の実体を直接編集するか:
        //     ColliderComponent::ResolvePhysicsMaterial() が毎フレーム同じキャッシュから
        //     値を引いている。ここを書き換えれば、参照している全コライダーの物性が
        //     再ロードもシーン再生も挟まずにその場で変わる (アセットを共有にした本来の狙い)。
        const std::string relPath = NormalizeAssetPath(absPath);
        const auto handle = asset::AssetManager::Load<asset::PhysicsMaterialAsset>(relPath);
        auto* physicsMaterial = asset::AssetManager::Get<asset::PhysicsMaterialAsset>(handle);
        if (!physicsMaterial) {
            ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, "Failed to load .physmat");
            return;
        }

        // 掴む直前の値を毎フレーム控える (.fzdata と同じ理由: どのウィジェットが
        // 掴まれるかは描画前に分からない)。中身は float 数個なのでコピーは無視できる。
        const bool canRecordUndo = CanRecordEditorUndo(ctx);
        const asset::PhysicsMaterialAsset beforeDraw = *physicsMaterial;
        const ImGuiID activeBefore = ImGui::GetActiveID();

        bool editedByPreset = false;
        if (ImGui::BeginCombo("Preset", physicsMaterial->presetName.empty()
                                            ? "(custom)"
                                            : physicsMaterial->presetName.c_str())) {
            for (int i = 0; i < physics::PhysicsMaterial::PRESET_COUNT; ++i) {
                const char* name = physics::PhysicsMaterial::PresetName(i);
                if (!ImGui::Selectable(name)) continue;
                if (const auto* preset = physics::PhysicsMaterial::PresetAt(i)) {
                    physicsMaterial->material   = *preset;
                    physicsMaterial->presetName = name;
                    editedByPreset = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::Separator();

        // フィールドは共通リフレクタで描く (Inspector と TOML 出力で同じ Reflect を共有)。
        ImGuiReflector reflector;
        reflector.m_projectRoot = ctx.projectRoot;
        physicsMaterial->Reflect(reflector);

        ImGui::Spacing();
        ImGui::TextDisabled("合成規則が異なる材質同士では、優先度の高い側が採用される");
        ImGui::TextDisabled("(Average < Geometric Mean < Minimum < Multiply < Maximum)");

        // 自動保存 (.fzdata と同じ規則: 操作が終わったフレームで書き戻す)。
        const bool editedThisFrame =
            (GImGui && GImGui->ActiveIdHasBeenEditedThisFrame) || editedByPreset;
        static bool        s_physmatDirty = false;
        static std::string s_physmatDirtyPath;
        if (editedThisFrame) {
            s_physmatDirty     = true;
            s_physmatDirtyPath = relPath;
            // プリセット以外の手編集はプリセット由来の表示を外す。
            if (!editedByPreset) physicsMaterial->presetName.clear();
        }
        if (s_physmatDirty && s_physmatDirtyPath == relPath && !ImGui::IsAnyItemActive()) {
            if (asset::SavePhysicsMaterialAssetToFile(absPath, *physicsMaterial))
                ctx.requestAssetBrowserRefresh = true;
            s_physmatDirty = false;
        }

        // Undo: 掴んでから離すまでを 1 操作として積む (.mat / .fzdata と同じ粒度)。
        struct PhysMatUndoTracker {
            std::string                  path;
            asset::PhysicsMaterialAsset  before;
            ImGuiID                      activeId = 0;
            bool                         active   = false;
        };
        static PhysMatUndoTracker physmatUndo;
        const ImGuiID activeAfter = ImGui::GetActiveID();

        auto pushPhysMatCommand = [&ctx, &relPath, &absPath](
            const asset::PhysicsMaterialAsset& before,
            const asset::PhysicsMaterialAsset& after) {
            if (!ctx.undoStack) return;
            EditorContext*    context     = &ctx;
            const std::string capturedRel = relPath;
            const std::string capturedAbs = absPath;
            auto apply = [context, capturedRel, capturedAbs](const asset::PhysicsMaterialAsset& value) {
                const auto h = asset::AssetManager::Load<asset::PhysicsMaterialAsset>(capturedRel);
                auto* target = asset::AssetManager::Get<asset::PhysicsMaterialAsset>(h);
                if (!target) return;
                *target = value;
                // メモリだけ戻すと次のロードで巻き戻るため、ディスクへも書き戻す。
                (void)asset::SavePhysicsMaterialAssetToFile(capturedAbs, value);
                context->requestAssetBrowserRefresh = true;
            };
            ctx.undoStack->Push(std::make_unique<LambdaCommand>(
                "Edit Physics Material",
                [apply, after]()  { apply(after); },
                [apply, before]() { apply(before); }));
        };

        if (!canRecordUndo) {
            physmatUndo.active = false;
        } else {
            if (physmatUndo.active && physmatUndo.path != relPath)
                physmatUndo.active = false;

            if (!physmatUndo.active) {
                if (activeAfter != 0 && activeAfter != activeBefore) {
                    physmatUndo.path     = relPath;
                    physmatUndo.before   = beforeDraw;
                    physmatUndo.activeId = activeAfter;
                    physmatUndo.active   = true;
                } else if (editedByPreset) {
                    // プリセット適用は掴み→離しの経路に乗らないため、その場で 1 操作にする。
                    pushPhysMatCommand(beforeDraw, *physicsMaterial);
                }
            } else if (activeAfter != physmatUndo.activeId) {
                // 掴んでいたウィジェットから手が離れた = 1 操作の終わり。
                pushPhysMatCommand(physmatUndo.before, *physicsMaterial);
                physmatUndo.active = false;
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("Save .physmat"))
            (void)asset::SavePhysicsMaterialAssetToFile(absPath, *physicsMaterial);
        ImGui::SameLine();
        ImGui::TextDisabled(s_physmatDirty && s_physmatDirtyPath == relPath
                            ? "Saving on release..." : "Auto-saved");
    } else {
        ImGui::TextDisabled("Type: %s", ext.c_str());
        ImGui::Spacing();
        ImGui::TextDisabled("Drag from Asset Browser to assign to a field.");
    }
}

} // namespace fbzz::editor
