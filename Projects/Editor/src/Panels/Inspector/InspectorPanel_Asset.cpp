// FBZZ Engine
// Inspector/InspectorPanel_Asset.cpp | fbzz::editor
// Asset Browser から選択したファイル用 Inspector
#include <Editor/Panels/InspectorPanel.hpp>
#include <Editor/Panels/AnimationGraphInspector.hpp>
#include <Editor/EditorContext.hpp>
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
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/ModelAsset.hpp>
#include <Engine/Asset/PostProcessAsset.hpp>
#include <Engine/Asset/TerrainAsset.hpp>
#include <Engine/Asset/TexDescSerializer.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Renderer/ShaderDescriptor.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fbzz::editor {

void InspectorPanel::DrawAssetInspector(EditorContext& ctx, const std::string& assetPath)
{
    const std::string absPath = assetPath;
    const std::string filename = util::FileSystem::GetFilename(absPath);
    const std::string ext      = util::StringUtils::ToLower(util::FileSystem::GetExtension(absPath));

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
    ImGui::TextDisabled("%s", absPath.c_str());
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

            static constexpr const char* kBlendNames[] = { "Opaque", "AlphaBlend", "Additive" };
            int blendIndex = 0;
            if      (mat.blendMode == renderer::BlendMode::ALPHA_BLEND) blendIndex = 1;
            else if (mat.blendMode == renderer::BlendMode::ADDITIVE)    blendIndex = 2;
            if (ImGui::Combo("Blend", &blendIndex, kBlendNames, 3)) {
                mat.blendMode = blendIndex == 1 ? renderer::BlendMode::ALPHA_BLEND
                              : blendIndex == 2 ? renderer::BlendMode::ADDITIVE
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
                if (widgets::AssetPathField(slot, path, ".fztex,.png,.dds", ctx.projectRoot))
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
                AssetDirtyRegistry::Register(
                    capturedPath, capturedDisplay, "MAT",
                    [capturedPath, handle]() {
                        auto* material = asset::AssetManager::GetMaterial(handle);
                        return material && asset::SaveMaterialAssetToFile(capturedPath, *material);
                    });
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
                context->requestAssetBrowserRefresh = true;
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
        if (ImGui::Button("Save .mat")) {
            if (asset::SaveMaterialAssetToFile(absPath, mat)) {
                AssetDirtyRegistry::MarkClean(absPath);
                ctx.requestAssetBrowserRefresh = true;
            }
        }
        const bool isMaterialDirtyNow = AssetDirtyRegistry::IsDirty(absPath);
        if (isMaterialDirtyNow) {
            ImGui::SameLine();
            ImGui::TextColored({1.0f, 0.8f, 0.2f, 1.0f}, "Modified");
        }
    } else if (ext == ".animcontroller") {
        if (!DrawAnimationGraphAssetInspector(ctx)) {
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
        const bool imported = util::FileSystem::Exists(p.parent_path() / p.stem() / (stem + ".fzasset"));

        ImGui::TextDisabled("Type: 3D Model Source (%s)", ext.c_str());
        ImGui::Spacing();

        if (imported) {
            ImGui::TextColored({ 0.3f, 0.9f, 0.3f, 1.0f }, "Status: Imported");
            ImGui::Spacing();
            if (ImGui::Button("Reimport..."))
                ctx.requestOpenImportModal = absPath;
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
            ImGui::LabelText("Root Motion",  "%s",    clip->hasRootMotion ? "Yes" : "No");

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
    } else if (ext == ".tex") {
        // ── .tex descriptor ──────────────────────────────────────────────
        ImGui::TextDisabled("Type: Texture Descriptor");
        ImGui::Spacing();

        // TexDescSerializer で読み込んで設定を表示・編集できるようにする
        static asset::TextureAsset s_texAsset;
        static std::string         s_texPath;
        if (s_texPath != absPath) {
            s_texPath = absPath;
            asset::TexDescSerializer ser;
            ser.Load(absPath, s_texAsset);
        }
        bool dirty = false;

        ImGui::SeparatorText("Source");
        ImGui::TextUnformatted(s_texAsset.sourcePath.c_str());

        ImGui::SeparatorText("Import Settings");
        auto& s = s_texAsset.settings;

        static constexpr const char* kTypeNames[] = { "Color", "Normal", "Data", "HDR", "UI" };
        int typeIdx = static_cast<int>(s.type);
        if (ImGui::Combo("Type", &typeIdx, kTypeNames, 5)) {
            s.type = static_cast<asset::TextureType>(typeIdx);
            s = asset::DefaultSettingsForType(s.type); // デフォルトを再適用
            dirty = true;
        }
        // ── Type / sRGB ───────────────────────────────────────────────
        dirty |= ImGui::Checkbox("sRGB",             &s.srgb);

        // ── Compression ───────────────────────────────────────────────
        ImGui::SeparatorText("Compression");
        static constexpr const char* kCompNames[] = {
            "Auto", "BC1", "BC3", "BC4", "BC5", "BC6H", "BC7", "None"
        };
        int compIdx = static_cast<int>(s.compression);
        if (ImGui::Combo("Format", &compIdx, kCompNames, 8)) {
            s.compression = static_cast<asset::TextureCompression>(compIdx);
            dirty = true;
        }
        {
            static constexpr const char* kQualNames[] = { "Fast", "Normal", "High" };
            int qualIdx = static_cast<int>(s.compressionQuality);
            if (ImGui::Combo("Quality", &qualIdx, kQualNames, 3)) {
                s.compressionQuality = static_cast<asset::CompQuality>(qualIdx);
                dirty = true;
            }
        }

        // ── Mipmaps ───────────────────────────────────────────────────
        ImGui::SeparatorText("Mipmaps");
        dirty |= ImGui::Checkbox("Mipmaps",           &s.mipmaps);
        if (s.mipmaps) {
            static constexpr const char* kMipFilterNames[] = { "Box", "Kaiser", "Lanczos" };
            int mipFIdx = static_cast<int>(s.mipFilter);
            if (ImGui::Combo("Mip Filter", &mipFIdx, kMipFilterNames, 3)) {
                s.mipFilter = static_cast<asset::MipFilter>(mipFIdx);
                dirty = true;
            }
            dirty |= ImGui::DragFloat("Mip Sharpen", &s.mipSharpen, 0.01f, 0.0f, 4.0f);
            dirty |= ImGui::DragFloat("Mip Bias",    &s.mipBias,    0.01f, -4.0f, 4.0f);
            dirty |= ImGui::Checkbox("Normalize Mipmaps", &s.normalizeMipmaps);
        }

        // ── Normal Map ────────────────────────────────────────────────
        if (s.type == asset::TextureType::Normal) {
            ImGui::SeparatorText("Normal Map");
            dirty |= ImGui::Checkbox("Flip Green (OpenGL)", &s.flipGreen);
        }

        // ── Sampling ─────────────────────────────────────────────────
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

        // ── Resolution ────────────────────────────────────────────────
        ImGui::SeparatorText("Resolution");
        int maxSize = static_cast<int>(s.maxSize);
        if (ImGui::InputInt("Max Size", &maxSize)) {
            s.maxSize = static_cast<uint32_t>(std::max(1, maxSize));
            dirty = true;
        }

        // ── Alpha ─────────────────────────────────────────────────────
        ImGui::SeparatorText("Alpha");
        {
            static constexpr const char* kAlphaNames[] = { "Straight", "Premultiplied", "None" };
            int alphaIdx = static_cast<int>(s.alphaMode);
            if (ImGui::Combo("Alpha Mode", &alphaIdx, kAlphaNames, 3)) {
                s.alphaMode = static_cast<asset::AlphaMode>(alphaIdx);
                dirty = true;
            }
        }
        dirty |= ImGui::Checkbox("Alpha Dither", &s.alphaDither);

        ImGui::Spacing();
        const bool isTxDirty = AssetDirtyRegistry::IsDirty(absPath) || dirty;
        if (dirty) {
            AssetDirtyRegistry::Register(
                absPath,
                util::FileSystem::GetFilename(absPath),
                "TEX",
                [path = absPath]() {
                    asset::TexDescSerializer ser;
                    return ser.Save(s_texAsset, path);
                });
        }

        if (ImGui::Button("Apply & Save") || (isTxDirty && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))) {
            asset::TexDescSerializer ser;
            if (ser.Save(s_texAsset, absPath))
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

        // .tex が存在するか確認
        const std::filesystem::path imgPath = util::FileSystem::PathFromUtf8(absPath);
        const std::string texDescPath = util::FileSystem::PathToUtf8(
            imgPath.parent_path() /
            (util::FileSystem::PathToUtf8(imgPath.stem()) + ".tex"));
        const bool hasTexDesc = util::FileSystem::Exists(
            util::FileSystem::PathFromUtf8(texDescPath));

        if (hasTexDesc) {
            ImGui::TextColored({0.3f, 0.9f, 0.3f, 1.0f}, ".tex descriptor: found");
            ImGui::TextDisabled("%s", util::FileSystem::GetFilename(texDescPath).c_str());
            ImGui::Spacing();
            if (ImGui::Button("Open .tex Inspector"))
                ctx.selectedAssetPath = texDescPath;
        } else {
            ImGui::TextColored({1.0f, 0.8f, 0.2f, 1.0f},
                "No .tex descriptor for this image.");
            ImGui::Spacing();
            ImGui::TextWrapped(
                "Creating a .tex lets you control sRGB, compression, "
                "mipmaps and other import settings.");
            ImGui::Spacing();
            if (ImGui::Button("Create .tex...")) {
                // GuessTextureType でデフォルト設定を生成して .tex を書き出す
                const asset::TextureType guessedType =
                    asset::GuessTextureType(util::FileSystem::GetFilename(absPath));
                asset::TextureAsset newAsset;
                newAsset.sourcePath = util::FileSystem::GetFilename(absPath);
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
    } else if (ext == ".terrain") {
        // ── .terrain バイナリ ─────────────────────────────────────────────
        ImGui::TextDisabled("Type: Terrain Asset");
        ImGui::Spacing();

        static asset::TerrainAsset s_terrainAsset;
        static std::string         s_terrainPath;
        static bool                s_terrainLoaded = false;
        bool terrainDirty = false;

        if (s_terrainPath != absPath) {
            s_terrainPath   = absPath;
            s_terrainLoaded = false;
            asset::FzTerrainSerializer ser;
            s_terrainLoaded = ser.Load(absPath, s_terrainAsset);
        }

        if (!s_terrainLoaded) {
            ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, "Load failed");
        } else {
            auto& ta = s_terrainAsset;

            ImGui::SeparatorText("Geometry");
            ImGui::LabelText("Columns",    "%u", ta.columns);
            ImGui::LabelText("Rows",       "%u", ta.rows);
            ImGui::LabelText("Cell Size",  "%.2f m", ta.cellSize);
            ImGui::LabelText("Max Height", "%.2f m", ta.maxHeight);
            ImGui::LabelText("Chunk Size", "%u", ta.chunkSize);
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
            for (int li = 0; li < static_cast<int>(ta.layerMaterialPaths.size()); ++li) {
                ImGui::PushID(li);
                const std::string layerLabel = "Layer " + std::to_string(li);
                if (widgets::AssetPathField(layerLabel.c_str(), ta.layerMaterialPaths[li], ".mat", ctx.projectRoot))
                    terrainDirty = true;
                ImGui::PopID();
            }

            if (terrainDirty) {
                AssetDirtyRegistry::Register(
                    absPath,
                    util::FileSystem::GetFilename(absPath),
                    "TERRAIN",
                    [path = absPath]() {
                        asset::FzTerrainSerializer ser;
                        return ser.Save(s_terrainAsset, path);
                    });
            }

            ImGui::Spacing();
            const bool isTerrainDirty = AssetDirtyRegistry::IsDirty(absPath) || terrainDirty;
            if (ImGui::Button("Save .terrain") ||
                (isTerrainDirty && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))) {
                asset::FzTerrainSerializer ser;
                if (ser.Save(ta, absPath))
                    AssetDirtyRegistry::MarkClean(absPath);
            }
            if (isTerrainDirty) {
                ImGui::SameLine();
                ImGui::TextColored({1.0f, 0.8f, 0.2f, 1.0f}, "Modified");
            }
        }
    } else if (ext == ".fzpp") {
        ImGui::TextDisabled("Type: Post Process Profile");
        ImGui::Spacing();

        static renderer::PostProcessSettings s_ppSettings;
        static std::string                   s_ppPath;
        bool ppDirty = false;

        if (s_ppPath != absPath) {
            s_ppPath     = absPath;
            s_ppSettings = renderer::PostProcessSettings{};
            asset::LoadPostProcessAssetFromFile(absPath, s_ppSettings);
        }

        auto& pp = s_ppSettings;
        const PostProcessInspectorResult inspectorResult = DrawPostProcessInspector(pp);
        ppDirty |= inspectorResult.changed;

        // アクティブシーンへの適用
        ImGui::Spacing();
        ImGui::Separator();
        if (ctx.activeScene) {
            if (ImGui::Button("Apply to Scene"))
                ctx.activeScene->GetRuntimePostProcessSettings() = pp;
            ImGui::SameLine();
            if (ImGui::Button("Clear Scene Override"))
                ctx.activeScene->ClearRuntimePostProcessSettings();
        }

        if (ppDirty) {
            AssetDirtyRegistry::Register(
                absPath,
                util::FileSystem::GetFilename(absPath),
                "FZPP",
                [path = absPath]() {
                    return asset::SavePostProcessAssetToFile(path, s_ppSettings);
                });
        }

        ImGui::Spacing();
        const bool isPPDirty = AssetDirtyRegistry::IsDirty(absPath) || ppDirty;
        if (ImGui::Button("Save .fzpp") ||
            (isPPDirty && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))) {
            if (asset::SavePostProcessAssetToFile(absPath, pp))
                AssetDirtyRegistry::MarkClean(absPath);
        }
        if (isPPDirty) {
            ImGui::SameLine();
            ImGui::TextColored({1.0f, 0.8f, 0.2f, 1.0f}, "Modified");
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

        // Inspector の共通リフレクタでフィールドを描画する (スクリプトと同じ UI)。
        // 編集対象は Registry がキャッシュする共有実体そのものなので、変更は全参照へ即反映される。
        ImGuiReflector reflector;
        data->Reflect(reflector);

        // 自動保存: 値が編集され、かつ操作 (ドラッグ/入力) が終わった瞬間にディスクへ書き戻す。
        // WHY: ScriptableObject 的な「いじったら保存されている」体験にする。連続ドラッグ中の
        //      大量書き込みは避けたいので、アクティブ操作が無くなったフレームでだけ保存する。
        const bool editedThisFrame = GImGui && GImGui->ActiveIdHasBeenEditedThisFrame;
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

        ImGui::Spacing();
        ImGui::Separator();
        // 保険の手動保存 (自動保存があるので通常は不要)。
        if (ImGui::Button("Save .fzdata"))
            asset::DataAssetRegistry::Save(relPath);
        ImGui::SameLine();
        ImGui::TextDisabled(s_fzdataDirty && s_fzdataDirtyPath == relPath
                            ? "Saving on release..." : "Auto-saved");
    } else {
        ImGui::TextDisabled("Type: %s", ext.c_str());
        ImGui::Spacing();
        ImGui::TextDisabled("Drag from Asset Browser to assign to a field.");
    }
}

} // namespace fbzz::editor
