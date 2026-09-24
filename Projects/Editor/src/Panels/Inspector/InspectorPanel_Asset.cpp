/// @file    InspectorPanel_Asset.cpp
/// @brief   Asset Browser から選択したファイル用 Inspector。
/// @author  Hasegawa Jin
/// @date    2026-06-07
/// @brief Undo 記録可否の判定 (CanRecordEditorUndo) など、Inspector 共通ヘルパーを使う。
#include "InspectorCommon.hpp"
#include <Editor/Panels/InspectorPanel.hpp>
#include <Editor/Panels/AnimationGraphInspector.hpp>
#include <Editor/Panels/AnimationMaskPreview.hpp>
#include <Editor/Panels/AnimationPreview.hpp>
#include <Editor/Panels/MaterialPreview.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Import/FbxMetaSerializer.hpp>
#include <Editor/Import/ImportSettingsSchema.hpp>
#include <Editor/Util/AnimatorMaskAudit.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/FluidInspector.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/MaterialInspectorWidgets.hpp>
#include <Editor/Util/ParticleMaterialInspector.hpp>
#include <Editor/Util/PostProcessInspectorWidgets.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/Util/VfxAssetSummary.hpp>
#include <Editor/ImGuiReflector.hpp>
#include <Engine/Asset/AnimationClip.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Audio/AudioManager.hpp>
#include <Engine/Audio/Synth.hpp>
#include <Engine/Core/Application.hpp>
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
#include <Engine/Asset/VectorFieldFile.hpp>
#include <Engine/Asset/PostProcessProfile.hpp>
#include <Engine/Asset/SynthAsset.hpp>
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
#include <cstring>
#include <cmath>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <utility>
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
        (sourceExt == ".png" && !asset::IsVectorFieldPng(assetPath)) || sourceExt == ".jpg" || sourceExt == ".jpeg" ||
        sourceExt == ".dds" || sourceExt == ".tga" || sourceExt == ".bmp" ||
        sourceExt == ".hdr" || sourceExt == ".exr";

    /// @note Unity と同様に、Project で原本を選択したまま隣接 .meta の Import Settings を描画する。
    /// @note 非表示 sidecar を選び直す操作や別ウィンドウを挟まず、全 raw 形式で同じ編集体験にする。
    const std::string absPath =
        (isModelSource || isTextureSource) ? assetPath + ".meta" : assetPath;
    const std::string filename = util::FileSystem::GetFilename(assetPath);
    const std::string ext =
        util::StringUtils::ToLower(util::FileSystem::GetExtension(absPath));

    /// @note deselect 自動保存: 前回の .mat が dirty のまま別アセットへ移動したときに保存する。
    /// @note Save 押し忘れを解消しつつ mid-drag 中の大量書き込みを避けるため、選択が外れたタイミング
    /// @note (本関数が別パスで呼ばれた瞬間) に保存する。
    if (!m_inspectedAssetPath.empty() &&
        m_inspectedAssetPath != absPath &&
        util::StringUtils::ToLower(util::FileSystem::GetExtension(m_inspectedAssetPath)) == ".mat" &&
        AssetDirtyRegistry::IsDirty(m_inspectedAssetPath))
    {
        auto* prevMat = asset::AssetManager::Get<asset::MaterialAsset>(m_inspectedMat);
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
            m_inspectedMat = asset::AssetManager::Load<asset::MaterialAsset>(relPath);
        }

        if (!m_inspectedMat.IsValid()) {
            ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, "Failed to load .mat");
            return;
        }

        auto* matPtr = asset::AssetManager::Get<asset::MaterialAsset>(m_inspectedMat);
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
            materialDirty |= ImGui::DragInt  ("Depth Bias",       &mat.depthBias,      0.25f, -1024, 1024);
            materialDirty |= ImGui::DragFloat("Depth Bias Slope", &mat.depthBiasSlope, 0.01f, -8.0f, 8.0f);
            ImGui::SetItemTooltip("Positive values pull this material toward the camera (fixes Z-fighting on coplanar faces)");
            {
                /// @note 通常の Forward / Deferred はプロジェクト設定で決まる。
                /// @note ここでは専用レンダーパスの用途だけを指定する。
                /// @note LAYOUT: asset::RenderPath と同じ並びにすること。欠けた用途は
                /// @note Combo を触った瞬間に別の値へ化ける。
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
            /// @note Sprite のコマを受けるのはメッシュ描画 (Auto) の albedo だけ
            /// @note (理由は InspectorMaterial.cpp の同じ場所)。
            const bool spriteAlbedo = mat.renderPath == asset::RenderPath::Auto;
            auto drawTexSlot = [&](const char* slot) {
                std::string& path = mat.textures[slot];
                const char* filter = spriteAlbedo && std::strcmp(slot, "albedo") == 0
                    ? widgets::kSpriteAssetFilter : widgets::kTextureAssetFilter;
                if (widgets::TextureSlotField(slot, path, filter, ctx.projectRoot))
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
                    if (!var.IsWritable() || var.varType != renderer::ShaderVarType::Float
                            || var.elements || var.varClass == renderer::ShaderVarClass::Matrix
                            || mat.integerParams.contains(var.name)) {
                            materialDirty |= DrawReflectedMaterialParam(mat, var);
                            continue;
                        }
                    const size_t n = var.columns > 0 ? var.columns : 1;
                    std::vector<float>& values = mat.params[var.name];
                    if (values.size() != n)
                        values = defaultParamValues(var.name, n);
                    drawParam(var.name, values);
                }
            }
            for (const auto& [name, values] : mat.integerParams) {
                    if (desc && desc->FindVar(name)) continue;
                    ImGui::TextDisabled("%s: reflection unavailable (%zu values)", name.c_str(), values.size());
                }
                for (auto& [name, values] : mat.params) {
                if (desc && desc->FindVar(name)) continue;
                drawParam(name, values);
            }
        }

        /// @note パーティクルの «見た目» はこの .mat が正本。ParticleEmitter からは編集できない。
        if (mat.renderPath == asset::RenderPath::Particle)
            materialDirty |= DrawParticleMaterialInspector(mat, ctx.projectRoot, ctx.resources, ctx.imguiRenderer);

        /// @note AssetBrowser のサムネイルを値変更のたびに追従させる (保存待ちにしない)。
        if (materialDirty)
            ctx.BumpMaterialPreviewRevision(relPath);

        if (materialDirty && ctx.activeScene) {
            for (auto [mc] : ctx.activeScene->View<scene::MaterialComponent>()) {
                if (NormalizeAssetPath(mc.materialPath) == relPath)
                    mc.material.reset();
            }
            for (auto [terrain] : ctx.activeScene->View<scene::TerrainComponent>()) {
                for (const auto& lm : terrain.layerMaterials)
                    if (NormalizeAssetPath(lm) == relPath) {
                        terrain.RequestSplatRebuild();
                        terrain.RequestMaterialRebuild();
                        break;
                    }
            }
            for (auto [water] : ctx.activeScene->View<scene::WaterComponent>()) {
                if (NormalizeAssetPath(water.materialPath) == relPath) {
                    water.texDirty = true;
                    water.foamDirty = true;
                }
            }
        }

        /// @note dirty になったら Registry に登録 (deselect 時 or Save All で一括保存できるようにする)
        if (materialDirty) {
            const std::string capturedPath   = absPath;
            const std::string capturedDisplay = NormalizeAssetPath(absPath);
            auto capturedHandle = m_inspectedMat;
            AssetDirtyRegistry::Register(
                capturedPath, capturedDisplay, "MAT",
                [capturedPath, capturedHandle]() {
                    auto* m = asset::AssetManager::Get<asset::MaterialAsset>(capturedHandle);
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
            auto persistMaterial = [handle, capturedPath, capturedDisplay](
                                       const asset::MaterialAsset& value) {
                if (asset::SaveMaterialAssetToFile(capturedPath, value)) {
                    AssetDirtyRegistry::MarkClean(capturedPath);
                    return;
                }
                AssetDirtyRegistry::Register(
                    capturedPath, capturedDisplay, "MAT",
                    [handle, capturedPath]() {
                        const auto* material = asset::AssetManager::Get<asset::MaterialAsset>(handle);
                        return material && asset::SaveMaterialAssetToFile(capturedPath, *material);
                    });
            };
            auto apply = [context, handle, capturedDisplay, persistMaterial](
                             const asset::MaterialAsset& value) {
                auto* target = asset::AssetManager::Get<asset::MaterialAsset>(handle);
                if (!target) return;
                *target = value;
                /// @note Save ボタンを廃止した代わりに、Undo/Redo が確定した瞬間に即ディスクへ書く。
                /// @note メモリ上の値と .mat が食い違うと、Undo で戻したつもりが再読み込みで元に戻る
                /// @note (見た目だけの Undo) 事故が起きる。書き込みはウィジェット確定時の 1 回だけ。
                persistMaterial(*target);
                if (context->activeScene) {
                    for (auto [component] : context->activeScene->View<scene::MaterialComponent>()) {
                        if (NormalizeAssetPath(component.materialPath) == capturedDisplay)
                            component.material.reset();
                    }
                    for (auto [terrain] : context->activeScene->View<scene::TerrainComponent>()) {
                        for (const auto& lm : terrain.layerMaterials)
                            if (NormalizeAssetPath(lm) == capturedDisplay) {
                                terrain.RequestSplatRebuild();
                                terrain.RequestMaterialRebuild();
                                break;
                            }
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
            /// @note UndoStack::Push は記録するだけで Do() を呼ばない (mat は既にウィジェットで
            /// @note 直接編集済みのため)。よってここで確定時点の保存を明示的に行う。apply() 内の
            /// @note 保存は Undo/Redo 実行時にのみ効く。
            persistMaterial(after);
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

        /// @note Save ボタンは廃止 (Unity ライク: 編集を確定した瞬間に自動保存し、Undo/Redo で
        /// @note 巻き戻せる)。ウィジェットがアクティブな間 (ドラッグ中など) だけ "Modified" を
        /// @note 表示し、フォーカスが外れて pushMaterialCommand が確定すると同時に消える。
        ImGui::Separator();
        const bool isMaterialDirtyNow = AssetDirtyRegistry::IsDirty(absPath);
        if (isMaterialDirtyNow) {
            ImGui::TextColored({1.0f, 0.8f, 0.2f, 1.0f}, "Modified");
        } else {
            ImGui::TextDisabled("Saved");
        }

        /// @note AnimationPreview と同じく、編集項目を確認した後の Inspector 最下部へ置く。
        /// @note 先頭固定だと Shader / Params / Textures が長い材質ほどプレビューが画面を占有し、
        /// @note 編集対象へ到達しにくくなる。
        ImGui::SeparatorText("Preview");
        m_materialPreview.Draw(ctx, mat, 240.0f);
    } else if (ext == ".vfx") {
        /// @note .vfx の再生面は Prefab 編集モード (Docs/design/vfx-prefab.md §8.2)。
        /// @note ここが受け持つのは «開く前に中身の見当を付ける» ところまで。
        DrawVfxAssetInspector(ctx, absPath);
    } else if (ext == ".animcontroller") {
        if (DrawAnimationGraphAssetInspector(ctx)) {
            /// @note Unity と同じく、State / Transition 詳細の直下でクリップと遷移ブレンドを確認できる。
            ImGui::SeparatorText("Preview");
            DrawAnimationPreviewWidget(ctx, 240.0f);
        } else {
            ImGui::TextDisabled("Select a State or Transition in Animation Graph.");
            ImGui::Spacing();
            ImGui::TextDisabled("The controller can be edited without selecting a Hierarchy object.");
        }
        /// @note 未保存表示は他のアセット型と同じ規約に揃える。absPath でなく editorPath で判定するのは、
        /// @note Inspector が表示中の .animcontroller と Animation Graph が開いているドキュメントは
        /// @note 別物でありうるため (ブラウザーで別コントローラーを選んでも編集対象は切り替わらない)。
        /// @note dirty なのは「開いている方」なので、そちらと一致するときだけ Modified を出す。
        const bool isThisControllerOpen =
            NormalizeAssetPath(ctx.animationControllerEditorPath) == NormalizeAssetPath(absPath);
        ImGui::Separator();
        if (isThisControllerOpen && ctx.animationControllerDirty) {
            ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning),
                               "Modified - Ctrl+S or Save Controller in Animation Graph.");
        } else if (isThisControllerOpen) {
            ImGui::TextDisabled("Saved");
        } else {
            ImGui::TextDisabled("Not open. Double-click to edit in Animation Graph.");
        }
    } else if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb") {
        namespace fs = std::filesystem;
        const fs::path    p         = util::FileSystem::PathFromUtf8(absPath);
        const std::string stem      = util::FileSystem::PathToUtf8(p.stem());
        /// @note 物理 .fzasset は Library/Baked に隔離されているため、論理パスの解決を通して確認する。
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

            /// @note クリップを持つスキンモデルはその場で再生確認できるようにする。
            if (const asset::Model* previewModel = asset::AssetManager::LoadAndGet<asset::Model>(absPath);
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
        /// @name .model バイナリ
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

            /// @name スケルトン情報
            if (m->IsSkinned() && m->skeleton) {
                ImGui::SeparatorText("Skeleton");
                ImGui::LabelText("Nodes", "%zu", m->skeleton->nodes.size());
            }

            /// @name LOD / サブメッシュ一覧
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

            /// @name マテリアルスロット
            if (!m->materialSlotNames.empty()) {
                ImGui::SeparatorText("Material Slots");
                for (const auto& slotName : m->materialSlotNames)
                    ImGui::TextDisabled("  %s  ->  (unbound)", slotName.c_str());
            }

            /// @note クリップを持つスキンモデルはその場で再生確認できるようにする。
            if (const asset::Model* previewModel = asset::AssetManager::LoadAndGet<asset::Model>(absPath);
                previewModel && previewModel->skeleton && !previewModel->clips.empty()) {
                ImGui::SeparatorText("Animation Preview");
                DrawAnimationPreviewWidget(ctx, 240.0f);
            }
        }

        /// @name Reimport
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
        /// @name .anim バイナリ
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
            /// @note Loop Time は .anim へ焼かれた値。.anim は再インポートで上書きされる生成物のため
            /// @note ここでは編集させず、原本の FBX 側で行う設定であることだけ明示する。
            ImGui::LabelText("Loop Time", "%s", clip->loop ? "On" : "Off");
            ImGui::TextDisabled("Edit in the source FBX > Animation.");

            /// @name Root Motion
            /// @note "Yes / No" だけだと No のとき専用ノード不在か候補未検出かを切り分けられないため、
            /// @note 解決済みノード名と自動検出の候補を並べて提示する。
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

            /// @name Events テーブル
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

        /// @name Reimport
        ImGui::Spacing();
        ImGui::Separator();
        {
            namespace fs = std::filesystem;
            const fs::path animPath  = util::FileSystem::PathFromUtf8(absPath);
            const fs::path parentDir = animPath.parent_path();
            /// @note "Model@Clip.anim" → "Model" が FBX stem
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

        /// @name Preview
        /// @note 同じフォルダー階層のモデル (.fbx / .fzasset) を自動解決してクリップを再生する。
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
            static bool s_modelOptionsLoaded = false;
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
                s_modelOptionsLoaded = FbxMetaSerializer::LoadOptions(sourcePath, s_modelOptions);
                s_modelMetaWriteTime = currentModelMetaWriteTime;
            }

            bool dirty = false;
            ImGui::SeparatorText("Source");
            ImGui::TextUnformatted(sourcePath.c_str());

            ImGui::SeparatorText("Model Import Settings");
            if (!s_modelOptionsLoaded)
                ImGui::TextDisabled("No saved model settings; using defaults.");
            static constexpr const char* kSourceDccNames[] = { "Auto Detect", "Maya / FBX SDK", "Blender" };
            int sourceDccIdx = static_cast<int>(s_modelOptions.sourceDcc);
            if (ImGui::Combo("Source DCC", &sourceDccIdx, kSourceDccNames, 3)) {
                s_modelOptions.sourceDcc = static_cast<FbxSourceDcc>(sourceDccIdx);
                dirty = true;
            }

            ImGui::SetNextItemWidth(140.0f);
            dirty |= ImGui::DragFloat("Unit Scale", &s_modelOptions.unitScaleMultiplier,
                                      0.01f, 0.001f, 100.0f, "%.3fx");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("FBX の自動単位変換後に掛ける倍率。");

            static constexpr const char* kUpAxisNames[] = { "Auto", "Y Up", "Z Up" };
            int upAxisIdx = static_cast<int>(s_modelOptions.upAxis);
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::Combo("Source Up Axis", &upAxisIdx, kUpAxisNames, 3)) {
                s_modelOptions.upAxis = static_cast<FbxUpAxis>(upAxisIdx);
                dirty = true;
            }

            dirty |= ImGui::Checkbox("Generate Normals", &s_modelOptions.generateNormals);
            ImGui::SameLine();
            dirty |= ImGui::Checkbox("Generate Tangents", &s_modelOptions.generateTangents);

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

            /// @name Animation (Unity の Model Import Settings > Animation 相当)
            /// @note .fbx.meta には既定から外れた設定しか書かないため、meta だけではクリップの
            /// @note 全体像が出ない。原本を軽量スキャンして名前を並べ、meta の値を重ねて表示する。
            ImGui::SeparatorText("Animation");
            {
                /// @note スキャン結果は FBX ごとにキャッシュする。毎フレーム開くと重い。
                static std::string   s_scannedFbxPath;
                static FbxScanResult s_scan;
                if (s_scannedFbxPath != sourcePath) {
                    s_scannedFbxPath = sourcePath;
                    s_scan = FbxImportTool::Scan(sourcePath);
                }

                if (!s_scan.valid || s_scan.animNames.empty()) {
                    ImGui::TextDisabled("No animation clips in this FBX.");
                } else {
                    ImGui::TextDisabled(
                        "Loop Time bakes into the .anim on reimport.");
                    if (ImGui::BeginTable("##ClipImportSettings", 5,
                                          ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                        ImGui::TableSetupColumn("Clip", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Start", ImGuiTableColumnFlags_WidthFixed, 64.0f);
                        ImGui::TableSetupColumn("End", ImGuiTableColumnFlags_WidthFixed, 64.0f);
                        ImGui::TableSetupColumn("Loop Time", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                        ImGui::TableHeadersRow();

                        for (const std::string& clipName : s_scan.animNames) {
                            ImGui::PushID(clipName.c_str());
                            const auto findClip = [&]() {
                                return std::find_if(
                                    s_modelOptions.clipSettings.begin(),
                                    s_modelOptions.clipSettings.end(),
                                    [&clipName](const AnimationClipImportSettings& settings) {
                                        return settings.name == clipName;
                                    });
                            };
                            const auto currentIt = findClip();
                            AnimationClipImportSettings current = currentIt != s_modelOptions.clipSettings.end()
                                ? *currentIt : AnimationClipImportSettings{ .name = clipName };
                            const auto updateClip = [&](AnimationClipImportSettings next) {
                                const bool isDefault = !next.loop && next.startFrame == 0.0 &&
                                    next.endFrame < 0.0 && next.outputName.empty();
                                auto it = findClip();
                                if (isDefault) {
                                    if (it != s_modelOptions.clipSettings.end())
                                        s_modelOptions.clipSettings.erase(it);
                                } else if (it != s_modelOptions.clipSettings.end()) {
                                    *it = std::move(next);
                                } else {
                                    s_modelOptions.clipSettings.push_back(std::move(next));
                                }
                                dirty = true;
                            };
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(clipName.c_str());

                            ImGui::TableNextColumn();
                            char outputNameBuffer[256] = {};
                            std::snprintf(outputNameBuffer, sizeof(outputNameBuffer), "%s",
                                          current.outputName.empty() ? clipName.c_str() : current.outputName.c_str());
                            if (ImGui::InputText("##name", outputNameBuffer, sizeof(outputNameBuffer))) {
                                current.outputName = std::string(outputNameBuffer) == clipName
                                    ? std::string{} : std::string(outputNameBuffer);
                                updateClip(current);
                            }

                            ImGui::TableNextColumn();
                            float startFrame = static_cast<float>(current.startFrame);
                            if (ImGui::DragFloat("##start", &startFrame, 0.25f, 0.0f, 100000.0f, "%.1f")) {
                                current.startFrame = std::max(0.0f, startFrame);
                                updateClip(current);
                            }

                            ImGui::TableNextColumn();
                            float endFrame = static_cast<float>(current.endFrame);
                            if (ImGui::DragFloat("##end", &endFrame, 0.25f, -1.0f, 100000.0f, "%.1f")) {
                                current.endFrame = endFrame < 0.0f ? -1.0 : endFrame;
                                updateClip(current);
                            }

                            ImGui::TableNextColumn();
                            bool loop = current.loop;
                            if (ImGui::Checkbox("##loop", &loop)) {
                                current.loop = loop;
                                updateClip(current);
                            }
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                }

                /// @note Root Motion の抽出元を Inspector から直接指定する。自動判定だけでは
                /// @note リグ固有の移動ノードを拾えないため、.anim 生成前の原本設定として .meta に保存する。
                ImGui::Spacing();
                char rootMotionBuffer[256] = {};
                std::snprintf(rootMotionBuffer, sizeof(rootMotionBuffer), "%s",
                              s_modelOptions.rootMotionNodeName.c_str());
                if (ImGui::InputTextWithHint("Root Motion Node", "Auto Detect",
                                             rootMotionBuffer, sizeof(rootMotionBuffer))) {
                    s_modelOptions.rootMotionNodeName = rootMotionBuffer;
                    dirty = true;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    ImGui::SetTooltip("空欄は自動判定。指定したノードから Root Motion を抽出します。");

                ImGui::SeparatorText("Sub Assets");
                ImGui::TextDisabled("空欄は全て選択。チェックを外すと選択対象を限定します。");
                auto drawSelectionList = [&dirty](const char* label,
                                                   const std::vector<std::string>& allNames,
                                                   std::vector<std::string>& selectedNames)
                {
                    if (allNames.empty()) return;
                    ImGui::PushID(label);
                    ImGui::TextDisabled("%s", label);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("All")) {
                        /// @note 空の配列はインポーター上で「全選択」を意味する。
                        selectedNames.clear();
                        dirty = true;
                    }
                    ImGui::BeginChild("##selection_list", { 0.0f, 120.0f }, true);
                    for (const std::string& name : allNames) {
                        const bool allSelected = selectedNames.empty();
                        const bool selected = allSelected ||
                            std::find(selectedNames.begin(), selectedNames.end(), name)
                                != selectedNames.end();
                        bool checked = selected;
                        /// @note FBX のメッシュ名は重複しうる ("Cube" が何個も並ぶ)。
                        ImGui::PushID(&name);
                        const bool toggled = ImGui::Checkbox(name.c_str(), &checked);
                        ImGui::PopID();
                        if (toggled) {
                            if (checked) {
                                if (!allSelected &&
                                    std::find(selectedNames.begin(), selectedNames.end(), name)
                                        == selectedNames.end())
                                    selectedNames.push_back(name);
                            } else {
                                if (allSelected)
                                    selectedNames = allNames;
                                selectedNames.erase(
                                    std::remove(selectedNames.begin(), selectedNames.end(), name),
                                    selectedNames.end());
                            }
                            dirty = true;
                        }
                    }
                    ImGui::EndChild();
                    ImGui::PopID();
                };
                drawSelectionList("Meshes", s_scan.meshNames, s_modelOptions.selectedMeshNames);
                drawSelectionList("Animations", s_scan.animNames, s_modelOptions.selectedAnimNames);
            }

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
                    s_modelOptionsLoaded = true;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Revert")) {
                s_modelOptions = {};
                s_modelOptionsLoaded = FbxMetaSerializer::LoadOptions(sourcePath, s_modelOptions);
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
        /// @name .tex descriptor
        ImGui::TextDisabled("Type: Texture Import Settings");
        ImGui::Spacing();

        /// @note TexDescSerializer で読み込んで設定を表示・編集できるようにする
        static asset::TextureAsset s_texAsset;
        static std::string         s_texPath;
        static std::filesystem::file_time_type s_texWriteTime{};
        std::error_code texTimeError;
        const auto currentTexWriteTime = std::filesystem::last_write_time(absPath, texTimeError);
        const bool externallyUpdated = !texTimeError
            && currentTexWriteTime != s_texWriteTime
            && !AssetDirtyRegistry::IsDirty(absPath);
        /// @note 「今フレームでディスクから読み直したか」。Sanitize はこのタイミングだけ走らせる
        /// @note (毎フレーム正すと、値を触っている最中に書き戻してしまう)。
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

        /// @note 元画像の拡張子で分類し、この分類で意味を持つ項目だけを描く。全項目を無条件に
        /// @note 並べると .hdr に sRGB/BC7 のような効きようがない設定が出る。表示の絞り込みと
        /// @note 保存値の整合 (Sanitize) を同じ判定表から導く。
        const ImportCategory texCategory = CategoryForExtension(
            util::StringUtils::ToLower(util::FileSystem::GetExtension(sourcePath)));
        if (texJustLoaded) SanitizeTextureSettings(texCategory, s);
        const TextureFieldMask texMask = TextureFieldsFor(texCategory, s);

        ImGui::TextDisabled("Category: %s", ImportCategoryLabel(texCategory));

        /// @note 画像選択から Sprite Editor までを 1 クリックにする (Unity の Sprite Editor 導線と同様)。
        /// @note Texture Type の変更と .meta 作成を別々に要求せず、前処理を安全にまとめて行う。
        /// @note Sprite として扱えない分類 (.hdr / .exr / .dds) では導線ごと出さない。押しても
        /// @note Sanitize で Sprite 型が戻されるだけで、何も起きないボタンになるため。
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
            /// @note デフォルトを再適用
            s = asset::DefaultSettingsForType(s.type);
            SanitizeTextureSettings(texCategory, s);
            dirty = true;
        }
        /// @note sRGB はガンマ付きカラーにしか意味がない。法線・データマスク・HDR では出さない。
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

        /// @name Compression
        /// @note .dds は既にブロック圧縮済みで再エンコード経路を持たないため、セクションごと出さない。
        if (texMask.compression) {
            ImGui::SeparatorText("Compression");
            /// @note 選択肢は型ごとに絞る (法線に BC1、HDR に BC7 等は破綻するため出さない)。
            if (DrawCompressionCombo("Format", texCategory, s.type, s.compression))
                dirty = true;
            static constexpr const char* kQualNames[] = { "Fast", "Normal", "High" };
            int qualIdx = static_cast<int>(s.compressionQuality);
            if (ImGui::Combo("Quality", &qualIdx, kQualNames, 3)) {
                s.compressionQuality = static_cast<asset::CompQuality>(qualIdx);
                dirty = true;
            }
        }

        /// @name Mipmaps
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
            /// @note 縮小で崩れた法線の再正規化。法線マップ以外では効果がない。
            if (texMask.normalizeMips)
                dirty |= ImGui::Checkbox("Normalize Mipmaps", &s.normalizeMipmaps);
        }

        /// @name Normal Map
        if (texMask.flipGreen) {
            ImGui::SeparatorText("Normal Map");
            dirty |= ImGui::Checkbox("Flip Green (OpenGL)", &s.flipGreen);
        }

        /// @name Sampling
        /// @note サンプラー状態は .dds でも実行時に効くので、圧縮を出さない分類でも表示する。
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

        /// @name Resolution
        if (texMask.maxSize) {
            ImGui::SeparatorText("Resolution");
            int maxSize = static_cast<int>(s.maxSize);
            if (ImGui::InputInt("Max Size", &maxSize)) {
                s.maxSize = static_cast<uint32_t>(std::max(1, maxSize));
                dirty = true;
            }
        }

        /// @name Alpha
        /// @note 法線・データマスクのアルファはチャンネルとして使うだけで、合成方式の概念がない。
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
        ImGui::SeparatorText("Preview");
        m_texturePreview.Draw(ctx, sourcePath, 240.0f);
    } else if ((ext == ".png" && !asset::IsVectorFieldPng(absPath)) || ext == ".jpg" || ext == ".jpeg" ||
               ext == ".dds" || ext == ".tga" || ext == ".bmp" ||
               ext == ".hdr" || ext == ".exr") {
        ImGui::TextDisabled("Type: Texture (%s)", ext.c_str());
        ImGui::Spacing();

        /// @note 元画像の隣に "<画像>.meta" サイドカーが存在するか確認 (二重拡張子)
        const std::string texDescPath = absPath + ".meta";
        const bool hasTexDesc = util::FileSystem::Exists(
            util::FileSystem::PathFromUtf8(texDescPath));

        if (hasTexDesc) {
            ImGui::TextColored({0.3f, 0.9f, 0.3f, 1.0f}, ".meta sidecar: found");
            ImGui::TextDisabled("%s", util::FileSystem::GetFilename(texDescPath).c_str());
            ImGui::Spacing();
            if (ImGui::Button("Open .meta Inspector"))
                SelectAsset(ctx, texDescPath);
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
                /// @note GuessTextureType でデフォルト設定を生成して "<画像>.meta" を書き出す
                const asset::TextureType guessedType =
                    asset::GuessTextureType(util::FileSystem::GetFilename(absPath));
                asset::TextureAsset newAsset;
                newAsset.settings   = asset::DefaultSettingsForType(guessedType);
                asset::TexDescSerializer ser;
                if (ser.Save(newAsset, texDescPath)) {
                    SelectAsset(ctx, texDescPath);
                    ctx.requestAssetBrowserRefresh = true;
                }
            }
            ImGui::Spacing();
            ImGui::TextDisabled(
                "Or drag directly to a material slot to use with default settings.");
        }
    } else if (ext == ".mask") {
        /// @name Avatar Mask (.mask)
        /// @note 上半身だけ / 下半身だけの制御はこのアセットが入口。ボーンパスを手書きさせず、
        /// @note Humanoid プリセットで一括生成し、必要なら個別に weight と blendDepth を詰める。
        ImGui::TextDisabled("Type: Avatar Mask");
        ImGui::Spacing();

        static asset::AvatarMaskAsset s_mask;
        static std::string            s_maskPath;
        static int                    s_maskAnchorNode = -1;
        static std::string            s_maskSelectedPath;
        static std::string            s_maskAnchorSourcePath;
        static std::string            s_maskPresetPath;
        static std::string            s_maskPresetMessage;
        static std::filesystem::file_time_type s_maskWriteTime{};
        std::error_code maskTimeError;
        const auto currentMaskWriteTime = std::filesystem::last_write_time(absPath, maskTimeError);
        const bool maskExternallyUpdated = !maskTimeError
            && currentMaskWriteTime != s_maskWriteTime
            && !AssetDirtyRegistry::IsDirty(absPath);
        if (s_maskPath != absPath || maskExternallyUpdated) {
            s_maskPath = absPath;
            s_maskAnchorNode = -1;
            s_maskSelectedPath.clear();
            ClearAnimationMaskPreviewSelection();
            s_mask = asset::AvatarMaskAsset{};
            if (!asset::LoadAvatarMaskAsset(absPath, s_mask))
                s_mask.name = util::FileSystem::GetFilename(absPath);
            s_maskWriteTime = currentMaskWriteTime;
        }
        std::string previewSelectionPath;
        if (ConsumeAnimationMaskPreviewSelection(previewSelectionPath))
            s_maskSelectedPath = std::move(previewSelectionPath);
        bool maskDirty = false;

        /// @note .mask も他の Inspector アセットと同じく、1 回のウィジェット操作を 1 件の Undo
        /// @note コマンドとして記録する。毎フレーム積むと Weight 入力中に中間値が大量に残り、
        /// @note Apply 前の編集を Undo/Redo したとき表示と保存先が食い違う。
        struct MaskUndoTracker {
            std::string            path;
            asset::AvatarMaskAsset before;
            ImGuiID                activeId = 0;
            bool                   active = false;
            bool                   changed = false;
        };
        static MaskUndoTracker s_maskUndo;
        const bool canRecordMaskUndo = CanRecordEditorUndo(ctx);
        const asset::AvatarMaskAsset maskBeforeDraw = s_mask;
        const ImGuiID maskActiveBefore = ImGui::GetActiveID();
        const asset::Model* maskModel = s_mask.skeletonSourcePath.empty()
            ? nullptr : asset::AssetManager::LoadAndGet<asset::Model>(s_mask.skeletonSourcePath);
        const asset::Skeleton* maskSkeleton =
            (maskModel && maskModel->skeleton) ? maskModel->skeleton.get() : nullptr;
        if (s_maskAnchorSourcePath != s_mask.skeletonSourcePath) {
            s_maskAnchorSourcePath = s_mask.skeletonSourcePath;
            s_maskAnchorNode = -1;
        }
        const auto sourceSignature = [](const std::string& sourcePath) {
            if (sourcePath.empty()) return std::string{};
            const std::string resolved = asset::AssetManager::ResolveAssetPath(sourcePath);
            if (resolved.empty()) return std::string{};
            std::error_code error;
            const std::filesystem::path path = util::FileSystem::PathFromUtf8(resolved);
            if (!std::filesystem::exists(path, error) || error) return std::string{};
            const auto writeTime = std::filesystem::last_write_time(path, error);
            if (error) return std::string{};
            const uintmax_t fileSize = std::filesystem::file_size(path, error);
            if (error) return std::string{};
            return std::to_string(writeTime.time_since_epoch().count()) +
                   ":" + std::to_string(fileSize);
        };
        const std::string currentSourceSignature = sourceSignature(s_mask.skeletonSourcePath);
        const bool sourceSignatureMissing =
            !s_mask.skeletonSourcePath.empty() && s_mask.skeletonSourceSignature.empty();
        const bool sourceChanged =
            !sourceSignatureMissing && !currentSourceSignature.empty() &&
            currentSourceSignature != s_mask.skeletonSourceSignature;
        const auto prepareMaskForSave = [&]() {
            /// @note UI で skeletonSourcePath 自体が変更された場合も、描画開始時の古い署名を
            /// @note 保存しないように、その時点のパスから再計算する。
            /// @note 重複した Bone パスは Advanced Rules の直接編集で発生し得るため、保存前に
            /// @note 正規化して「削除したのに同じルールが残る」状態も防ぐ。
            asset::CompressAvatarMaskEntries(s_mask);
            s_mask.skeletonSourceSignature = sourceSignature(s_mask.skeletonSourcePath);
        };

        enum class MaskRuleState { Inherit, Include, Exclude, Mixed };

        const auto findEntry = [&](std::string_view path) -> asset::AvatarMaskEntry* {
            for (auto& entry : s_mask.entries)
                if (entry.bonePath == path) return &entry;
            return nullptr;
        };
        const auto findEntryIndex = [&](std::string_view path) -> int {
            for (int i = 0; i < static_cast<int>(s_mask.entries.size()); ++i)
                if (s_mask.entries[static_cast<size_t>(i)].bonePath == path) return i;
            return -1;
        };
        const auto ruleState = [&](std::string_view path) {
            const auto* entry = findEntry(path);
            if (!entry) return MaskRuleState::Inherit;
            return entry->weight > 0.001f ? MaskRuleState::Include : MaskRuleState::Exclude;
        };
        const auto setRule = [&](std::string path, MaskRuleState state, float weight = 1.0f) {
            const int index = findEntryIndex(path);
            if (state == MaskRuleState::Inherit) {
                if (index >= 0) s_mask.entries.erase(s_mask.entries.begin() + index);
                return;
            }
            weight = std::isfinite(weight) ? std::clamp(weight, 0.0f, 1.0f) : 0.0f;
            if (index < 0) {
                asset::AvatarMaskEntry entry;
                entry.bonePath = std::move(path);
                entry.weight = weight;
                entry.includeChildren = true;
                s_mask.entries.push_back(std::move(entry));
                return;
            }
            auto& entry = s_mask.entries[static_cast<size_t>(index)];
            entry.weight = weight;
            entry.includeChildren = true;
        };
        const auto cycleState = [](MaskRuleState state) {
            switch (state) {
            case MaskRuleState::Inherit: return MaskRuleState::Include;
            case MaskRuleState::Include: return MaskRuleState::Exclude;
            case MaskRuleState::Exclude: return MaskRuleState::Inherit;
            case MaskRuleState::Mixed: return MaskRuleState::Include;
            }
            return MaskRuleState::Inherit;
        };
        const auto nodePath = [&](int nodeIndex) {
            return asset::BuildSkeletonNodePath(*maskSkeleton, nodeIndex);
        };
        const auto setNodeRule = [&](int nodeIndex, MaskRuleState state) {
            if (!maskSkeleton) return;
            const std::string path = nodePath(nodeIndex);
            if (path.empty()) return;
            setRule(path, state);
        };
        const auto clearBranchEntries = [&](int nodeIndex) {
            if (!maskSkeleton) return;
            const std::string rootPath = nodePath(nodeIndex);
            if (rootPath.empty()) return;
            s_mask.entries.erase(
                std::remove_if(s_mask.entries.begin(), s_mask.entries.end(),
                    [&rootPath](const asset::AvatarMaskEntry& entry) {
                        if (entry.bonePath == rootPath) return true;
                        return entry.bonePath.size() > rootPath.size()
                            && entry.bonePath.compare(0, rootPath.size(), rootPath) == 0
                            && entry.bonePath[rootPath.size()] == '/';
                    }),
                s_mask.entries.end());
        };
        const auto applyBranchRule = [&](int nodeIndex, MaskRuleState state) {
            /// @note いったん配下の明示ルールを消してから親へ設定する。Include 済みの親に子の Exclude が
            /// @note 残ると表示が Mixed のままになり、Include → Exclude → Inherit の循環が成立しない。
            clearBranchEntries(nodeIndex);
            if (state != MaskRuleState::Inherit)
                setNodeRule(nodeIndex, state);
        };
        const auto lowerCopy = [](std::string value) {
            return util::StringUtils::ToLower(value);
        };
        const auto addNameRule = [&](const std::string& name) {
            if (!maskSkeleton) {
                setRule(name, MaskRuleState::Include);
                return;
            }
            for (int i = 0; i < static_cast<int>(maskSkeleton->nodes.size()); ++i) {
                const auto& node = maskSkeleton->nodes[static_cast<size_t>(i)];
                if (lowerCopy(node.name) == lowerCopy(name)) {
                    setNodeRule(i, MaskRuleState::Include);
                    return;
                }
            }
            setRule(name, MaskRuleState::Include);
        };
        const auto addBodyPartRules = [&](asset::HumanoidBodyPart part) {
            bool added = false;
            if (maskSkeleton) {
                for (int i = 0; i < static_cast<int>(maskSkeleton->nodes.size()); ++i) {
                    const auto& node = maskSkeleton->nodes[static_cast<size_t>(i)];
                    if (!asset::BoneNameMatchesBodyPart(node.name, part)) continue;
                    setNodeRule(i, MaskRuleState::Include);
                    added = true;
                }
            }
            if (!added) {
                const auto& patterns = asset::HumanoidBonePatterns(part);
                setRule(patterns.empty() ? asset::HumanoidBodyPartName(part) : patterns.front(),
                        MaskRuleState::Include);
            }
        };
        const auto replaceWithPreset = [&](const char* preset) {
            s_mask.entries.clear();
            s_mask.defaultInclude = false;
            if (std::strcmp(preset, "Upper Body") == 0) {
                addNameRule("Spine");
                addBodyPartRules(asset::HumanoidBodyPart::Head);
                addBodyPartRules(asset::HumanoidBodyPart::LeftArm);
                addBodyPartRules(asset::HumanoidBodyPart::RightArm);
                addBodyPartRules(asset::HumanoidBodyPart::LeftHand);
                addBodyPartRules(asset::HumanoidBodyPart::RightHand);
            } else if (std::strcmp(preset, "Lower Body") == 0) {
                addNameRule("Hips");
                addBodyPartRules(asset::HumanoidBodyPart::LeftLeg);
                addBodyPartRules(asset::HumanoidBodyPart::RightLeg);
            } else if (std::strcmp(preset, "Arms Only") == 0) {
                addBodyPartRules(asset::HumanoidBodyPart::LeftArm);
                addBodyPartRules(asset::HumanoidBodyPart::RightArm);
            } else if (std::strcmp(preset, "Hands Only") == 0) {
                addBodyPartRules(asset::HumanoidBodyPart::LeftHand);
                addBodyPartRules(asset::HumanoidBodyPart::RightHand);
            } else if (std::strcmp(preset, "Facial") == 0) {
                addBodyPartRules(asset::HumanoidBodyPart::Head);
            } else if (std::strcmp(preset, "Root Motion Only") == 0) {
                addBodyPartRules(asset::HumanoidBodyPart::Root);
            }
            asset::CompressAvatarMaskEntries(s_mask);
            maskDirty = true;
        };
        const auto mirrorPath = [](std::string path) {
            const auto replaceAll = [](std::string& value,
                                        std::string_view from,
                                        std::string_view to) {
                size_t position = 0;
                while ((position = value.find(from, position)) != std::string::npos) {
                    value.replace(position, from.size(), to);
                    position += to.size();
                }
            };
            replaceAll(path, "Left", "__FBZZ_RIGHT__");
            replaceAll(path, "Right", "Left");
            replaceAll(path, "__FBZZ_RIGHT__", "Right");
            replaceAll(path, "left", "__fbzz_right__");
            replaceAll(path, "right", "left");
            replaceAll(path, "__fbzz_right__", "right");
            replaceAll(path, "_L", "__FBZZ_R__");
            replaceAll(path, "_R", "_L");
            replaceAll(path, "__FBZZ_R__", "_R");
            replaceAll(path, "_l", "__fbzz_r__");
            replaceAll(path, "_r", "_l");
            replaceAll(path, "__fbzz_r__", "_r");
            replaceAll(path, "L_", "__FBZZ_R_PREFIX__");
            replaceAll(path, "R_", "L_");
            replaceAll(path, "__FBZZ_R_PREFIX__", "R_");
            replaceAll(path, "l_", "__fbzz_r_prefix__");
            replaceAll(path, "r_", "l_");
            replaceAll(path, "__fbzz_r_prefix__", "r_");
            return path;
        };
        const auto mirrorMask = [&]() {
            std::vector<asset::AvatarMaskEntry> mirrored;
            mirrored.reserve(s_mask.entries.size());
            for (const auto& entry : s_mask.entries) {
                auto copy = entry;
                copy.bonePath = mirrorPath(copy.bonePath);
                mirrored.push_back(std::move(copy));
            }
            s_mask.entries = std::move(mirrored);
            asset::CompressAvatarMaskEntries(s_mask);
            maskDirty = true;
        };

        ImGui::SeparatorText("Mask");
        /// @note defaultInclude を切り替えると「列挙したボーンだけ有効」と「列挙したボーンだけ無効」が
        /// @note 反転する。上半身マスクは前者、指だけ抜くマスクは後者が書きやすい。
        if (ImGui::Checkbox("Default Include (未列挙のボーンも有効にする)", &s_mask.defaultInclude))
            maskDirty = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::SetTooltip(
                "OFF: 下の一覧に無いボーンは weight 0 (上半身だけ有効、などに使う)\n"
                "ON : 下の一覧に無いボーンは weight 1 (指だけ除外、などに使う)");
        }

        /// @name Humanoid プリセット
        /// @note FBX を指定すると同じスケルトンをツリーで編集できる。AssetPathField は
        /// @note ピッカー・拡張子検証・Asset Browser からの D&D を共通で提供する。
        ImGui::SeparatorText("Skeleton Source");
        if (widgets::AssetPathField("Skeleton Source", s_mask.skeletonSourcePath,
                                     ".fbx", ctx.projectRoot)) {
            s_mask.skeletonSourceSignature.clear();
            maskDirty = true;
        }
        if (sourceChanged) {
            ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning),
                               "元FBXが変更されています。ボーン構成とマスクルールを確認してください。");
            ImGui::SameLine();
            if (ImGui::SmallButton("Accept Current FBX")) {
                s_mask.skeletonSourceSignature = currentSourceSignature;
                maskDirty = true;
            }
        } else if (sourceSignatureMissing && !s_mask.skeletonSourcePath.empty()) {
            ImGui::TextDisabled("FBX変更検知の基準は未保存です。Applyで現在のFBXを基準にします。");
        }

        ImGui::TextDisabled("FBXを指定すると階層ツリーが表示されます。ツリーのボーンをドラッグして範囲へ追加できます。");
        ImGui::SeparatorText("Presets");
        constexpr const char* kPresets[] = {
            "Upper Body", "Lower Body", "Arms Only", "Hands Only", "Facial", "Root Motion Only" };
        for (int i = 0; i < static_cast<int>(std::size(kPresets)); ++i) {
            if (i > 0) ImGui::SameLine();
            if (ImGui::Button(kPresets[i])) replaceWithPreset(kPresets[i]);
        }

        ImGui::SeparatorText("Saved Presets");
        widgets::AssetPathField("Preset File", s_maskPresetPath,
                                ".maskpreset,.mask", ctx.projectRoot);
        ImGui::SameLine();
        if (ImGui::Button("Load Preset") && !s_maskPresetPath.empty()) {
            asset::AvatarMaskAsset loadedPreset;
            const std::string presetLoadPath = asset::AssetManager::ResolveAssetPath(s_maskPresetPath);
            if (asset::LoadAvatarMaskAsset(
                    presetLoadPath.empty() ? s_maskPresetPath : presetLoadPath, loadedPreset)) {
                const std::string currentSource = s_mask.skeletonSourcePath;
                const std::string currentSignature = currentSourceSignature;
                s_mask = std::move(loadedPreset);
                if (!currentSource.empty()) {
                    s_mask.skeletonSourcePath = currentSource;
                    s_mask.skeletonSourceSignature = currentSignature;
                }
                s_maskPresetMessage = "Preset loaded";
                maskDirty = true;
            } else {
                s_maskPresetMessage = "Preset load failed";
            }
        }
        if (!s_maskPresetMessage.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", s_maskPresetMessage.c_str());
        }

        ImGui::SeparatorText("Body Parts");
        for (int p = 0; p < static_cast<int>(asset::HumanoidBodyPart::Count); ++p) {
            const auto part = static_cast<asset::HumanoidBodyPart>(p);
            ImGui::PushID(p);
            if (ImGui::SmallButton("+")) {
                addBodyPartRules(part);
                asset::CompressAvatarMaskEntries(s_mask);
                maskDirty = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("-")) {
                const auto& patterns = asset::HumanoidBonePatterns(part);
                setRule(patterns.empty() ? asset::HumanoidBodyPartName(part) : patterns.front(),
                        MaskRuleState::Exclude, 0.0f);
                maskDirty = true;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(asset::HumanoidBodyPartName(part));
            ImGui::PopID();
            if (p % 2 == 0 && p + 1 < static_cast<int>(asset::HumanoidBodyPart::Count))
                ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.5f);
        }

        ImGui::Spacing();
        if (ImGui::Button("Mirror Left / Right")) mirrorMask();
        ImGui::SameLine();
        if (ImGui::Button("All Include")) {
            s_mask.defaultInclude = true;
            maskDirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("All Exclude")) {
            s_mask.defaultInclude = false;
            s_mask.entries.clear();
            maskDirty = true;
        }

        /// @name エントリ一覧
        ImGui::SeparatorText("Hierarchy");
        auto drawDropZone = [&](const char* label, MaskRuleState state) {
            ImGui::Button(label, { ImGui::GetContentRegionAvail().x, 30.0f });
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload("FBZZ_MASK_BONE_PATH")) {
                    const size_t size = payload->DataSize > 0
                        ? static_cast<size_t>(payload->DataSize - 1) : 0;
                    setRule(std::string(static_cast<const char*>(payload->Data), size), state);
                    maskDirty = true;
                }
                ImGui::EndDragDropTarget();
            }
        };
        drawDropZone("Drop bone here to Include", MaskRuleState::Include);
        drawDropZone("Drop bone here to Exclude", MaskRuleState::Exclude);

        if (maskSkeleton) {
            static char s_maskSearch[128] = {};
            static bool s_maskChangedOnly = false;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
            ImGui::InputTextWithHint("##mask_search", "Search bone name or path",
                                     s_maskSearch, sizeof(s_maskSearch));
            ImGui::SameLine();
            ImGui::Checkbox("Changed only", &s_maskChangedOnly);

            std::vector<int> flattenedNodes;
            std::vector<bool> visited(maskSkeleton->nodes.size(), false);
            const std::function<void(int)> collectNodes = [&](int nodeIndex) {
                if (nodeIndex < 0 || nodeIndex >= static_cast<int>(maskSkeleton->nodes.size())
                    || visited[static_cast<size_t>(nodeIndex)]) return;
                visited[static_cast<size_t>(nodeIndex)] = true;
                flattenedNodes.push_back(nodeIndex);
                for (const int child : maskSkeleton->nodes[static_cast<size_t>(nodeIndex)].children)
                    collectNodes(child);
            };
            for (int i = 0; i < static_cast<int>(maskSkeleton->nodes.size()); ++i)
                if (maskSkeleton->nodes[static_cast<size_t>(i)].parentIndex < 0) collectNodes(i);
            for (int i = 0; i < static_cast<int>(maskSkeleton->nodes.size()); ++i)
                collectNodes(i);

            int missingRuleCount = 0;
            for (const auto& entry : s_mask.entries) {
                if (entry.bonePath.find('/') == std::string::npos) continue;
                bool found = false;
                for (int nodeIndex : flattenedNodes) {
                    if (nodePath(nodeIndex) == entry.bonePath) {
                        found = true;
                        break;
                    }
                }
                if (!found) ++missingRuleCount;
            }
            if (sourceChanged && missingRuleCount > 0) {
                ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning),
                                   "%d mask rule(s) no longer exist in this FBX.", missingRuleCount);
            }

            const auto ruleLabel = [](MaskRuleState state) {
                switch (state) {
                case MaskRuleState::Include: return "✓";
                case MaskRuleState::Exclude: return "×";
                case MaskRuleState::Mixed: return "~";
                case MaskRuleState::Inherit: return "－";
                }
                return "－";
            };
            const auto ruleName = [](MaskRuleState state) {
                switch (state) {
                case MaskRuleState::Include: return "Included";
                case MaskRuleState::Exclude: return "Excluded";
                case MaskRuleState::Mixed: return "Mixed";
                case MaskRuleState::Inherit: return "Inherited";
                }
                return "Inherited";
            };
            const auto mixColor = [](ImVec4 a, ImVec4 b, float amount) {
                const float t = std::clamp(amount, 0.0f, 1.0f);
                return ImVec4(
                    a.x + (b.x - a.x) * t,
                    a.y + (b.y - a.y) * t,
                    a.z + (b.z - a.z) * t,
                    a.w + (b.w - a.w) * t);
            };
            const auto weightColor = [&](float weight) {
                /// @note 0.0 = 危険色、0.5 = 注意色、1.0 = 有効色の信号機配色にする。薄い灰色だと
                /// @note 「除外」と「未設定」を見分けにくく、メッシュ Preview の色とも対応しなかった。
                if (weight < 0.5f) {
                    return mixColor(EditorTheme::Color(ThemeColor::Danger),
                                    EditorTheme::Color(ThemeColor::Warning), weight * 2.0f);
                }
                return mixColor(EditorTheme::Color(ThemeColor::Warning),
                                EditorTheme::Color(ThemeColor::Success),
                                (weight - 0.5f) * 2.0f);
            };
            const auto stateColor = [&](MaskRuleState state, float effectiveWeight) {
                switch (state) {
                case MaskRuleState::Include:
                    return EditorTheme::Color(ThemeColor::Success);
                case MaskRuleState::Exclude:
                    return EditorTheme::Color(ThemeColor::Danger);
                case MaskRuleState::Mixed:
                    return EditorTheme::Color(ThemeColor::Warning);
                case MaskRuleState::Inherit:
                    /// @note 親の Include を継承して効いているノードは青、未適用はミュート。
                    return effectiveWeight > 0.001f
                        ? EditorTheme::Color(ThemeColor::Accent)
                        : EditorTheme::Color(ThemeColor::TextMuted);
                }
                return EditorTheme::Color(ThemeColor::TextMuted);
            };
            const std::string searchText = lowerCopy(s_maskSearch);
            const std::function<bool(int)> nodeVisible = [&](int nodeIndex) {
                if (nodeIndex < 0 || nodeIndex >= static_cast<int>(maskSkeleton->nodes.size()))
                    return false;
                const auto& node = maskSkeleton->nodes[static_cast<size_t>(nodeIndex)];
                const std::string path = nodePath(nodeIndex);
                const std::string lowerName = lowerCopy(node.name);
                const bool matchesSearch = searchText.empty()
                    || lowerName.find(searchText) != std::string::npos
                    || lowerCopy(path).find(searchText) != std::string::npos;
                const bool isChanged = ruleState(path) != MaskRuleState::Inherit;
                if ((matchesSearch && (!s_maskChangedOnly || isChanged))) return true;
                for (const int child : node.children)
                    if (nodeVisible(child)) return true;
                return false;
            };
            struct BranchWeightFlags {
                bool hasIncluded = false;
                bool hasExcluded = false;
            };
            std::vector<BranchWeightFlags> branchWeightCache(maskSkeleton->nodes.size());
            std::vector<bool> branchWeightCached(maskSkeleton->nodes.size(), false);
            const std::function<BranchWeightFlags(int)> collectBranchWeights =
                [&](int nodeIndex) {
                    BranchWeightFlags result;
                    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(maskSkeleton->nodes.size()))
                        return result;
                    if (branchWeightCached[static_cast<size_t>(nodeIndex)])
                        return branchWeightCache[static_cast<size_t>(nodeIndex)];
                    const auto& branchNode = maskSkeleton->nodes[static_cast<size_t>(nodeIndex)];
                    const std::string branchPath = nodePath(nodeIndex);
                    const float branchWeight = asset::EvaluateAvatarMaskWeight(
                        s_mask, branchPath, branchNode.name);
                    result.hasIncluded = branchWeight > 0.001f;
                    result.hasExcluded = !result.hasIncluded;
                    for (const int child : branchNode.children) {
                        const BranchWeightFlags childFlags = collectBranchWeights(child);
                        result.hasIncluded = result.hasIncluded || childFlags.hasIncluded;
                        result.hasExcluded = result.hasExcluded || childFlags.hasExcluded;
                    }
                    branchWeightCache[static_cast<size_t>(nodeIndex)] = result;
                    branchWeightCached[static_cast<size_t>(nodeIndex)] = true;
                    return result;
                };
            const auto displayState = [&](int nodeIndex) {
                const BranchWeightFlags branch = collectBranchWeights(nodeIndex);
                if (branch.hasIncluded && branch.hasExcluded)
                    return MaskRuleState::Mixed;
                return ruleState(nodePath(nodeIndex));
            };
            const auto nextRule = [&](MaskRuleState state, int nodeIndex) {
                const MaskRuleState next = cycleState(state);
                if (ImGui::GetIO().KeyShift && s_maskAnchorNode >= 0) {
                    const auto anchor = std::find(flattenedNodes.begin(), flattenedNodes.end(),
                                                  s_maskAnchorNode);
                    const auto current = std::find(flattenedNodes.begin(), flattenedNodes.end(),
                                                   nodeIndex);
                    if (anchor != flattenedNodes.end() && current != flattenedNodes.end()) {
                        const auto first = std::min(anchor, current);
                        const auto last = std::max(anchor, current);
                        for (auto it = first; it != last + 1; ++it)
                            setNodeRule(*it, next);
                        return;
                    }
                }
                applyBranchRule(nodeIndex, next);
            };
            const std::function<void(int, int)> drawNode = [&](int nodeIndex, int depth) {
                if (!nodeVisible(nodeIndex)) return;
                const auto& node = maskSkeleton->nodes[static_cast<size_t>(nodeIndex)];
                const std::string path = nodePath(nodeIndex);
                const MaskRuleState state = displayState(nodeIndex);
                const float effectiveWeight =
                    asset::EvaluateAvatarMaskWeight(s_mask, path, node.name);
                const std::string lowerPath = lowerCopy(path);
                const bool matchesSearch = !searchText.empty()
                    && (lowerCopy(node.name).find(searchText) != std::string::npos
                        || lowerPath.find(searchText) != std::string::npos);
                /// @note SpanAvailWidth はテーブルの固定操作列まで TreeNode の矩形に含めるため、
                /// @note 長いノード名が Weight 入力欄の上へ描画される。ノード列のクリップに任せる。
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow;
                if (node.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
                if (!searchText.empty() || s_maskChangedOnly)
                    flags |= ImGuiTreeNodeFlags_DefaultOpen;
                if (matchesSearch || s_maskSelectedPath == path)
                    flags |= ImGuiTreeNodeFlags_Selected;
                /// @note TreePush はテーブル全体の横幅を狭めて固定操作列へ侵入するため使わず、
                /// @note Bone 列の中だけへ深さを描画する。これで深い階層でも Weight 列を守る。
                flags |= ImGuiTreeNodeFlags_NoTreePushOnOpen;

                const ImVec4 nodeColor = stateColor(state, effectiveWeight);
                bool open = false;
                bool clicked = false;
                ImGui::PushID(path.c_str());
                if (ImGui::BeginTable("##mask_node_row", 4,
                                      ImGuiTableFlags_SizingStretchProp |
                                      ImGuiTableFlags_NoSavedSettings)) {
                    ImGui::TableSetupColumn("##mask_node_label", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("##mask_node_state",
                                            ImGuiTableColumnFlags_WidthFixed, 38.0f);
                    ImGui::TableSetupColumn("##mask_node_weight",
                                             ImGuiTableColumnFlags_WidthFixed, 104.0f);
                    ImGui::TableSetupColumn("##mask_node_state_name",
                                             ImGuiTableColumnFlags_WidthFixed, 96.0f);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const float rowStartX = ImGui::GetCursorPosX();
                    ImGui::SetCursorPosX(rowStartX +
                                         depth * ImGui::GetStyle().IndentSpacing);
                    ImGui::PushStyleColor(ImGuiCol_Text, nodeColor);
                    open = ImGui::TreeNodeEx(path.c_str(), flags, "%s", node.name.c_str());
                    ImGui::PopStyleColor();
                    clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
                    if (ImGui::BeginPopupContextItem()) {
                        if (ImGui::MenuItem("Include branch")) {
                            applyBranchRule(nodeIndex, MaskRuleState::Include);
                            maskDirty = true;
                        }
                        if (ImGui::MenuItem("Exclude branch")) {
                            applyBranchRule(nodeIndex, MaskRuleState::Exclude);
                            maskDirty = true;
                        }
                        if (ImGui::MenuItem("Reset branch")) {
                            applyBranchRule(nodeIndex, MaskRuleState::Inherit);
                            maskDirty = true;
                        }
                        ImGui::Separator();
                        ImGui::TextDisabled("Path: %s", path.c_str());
                        ImGui::EndPopup();
                    }

                    ImGui::TableNextColumn();
                    ImVec4 statusButtonColor = nodeColor;
                    statusButtonColor.w = 0.22f;
                    ImVec4 statusButtonHover = nodeColor;
                    statusButtonHover.w = 0.38f;
                    ImVec4 statusButtonActive = nodeColor;
                    statusButtonActive.w = 0.55f;
                    ImGui::PushStyleColor(ImGuiCol_Button, statusButtonColor);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, statusButtonHover);
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, statusButtonActive);
                    if (ImGui::Button(ruleLabel(state), ImVec2(-FLT_MIN, 0.0f))) {
                        nextRule(state, nodeIndex);
                        s_maskAnchorNode = nodeIndex;
                        maskDirty = true;
                    }
                    ImGui::PopStyleColor(3);
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                        ImGui::SetTooltip("%s\n%s", ruleName(state), path.c_str());

                    ImGui::TableNextColumn();
                    float editedWeight = effectiveWeight;
                    ImGui::PushStyleColor(ImGuiCol_Text, weightColor(effectiveWeight));
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    const bool weightEdited = ImGui::InputFloat(
                        "##weight", &editedWeight, 0.01f, 0.1f, "%.2f");
                    if (weightEdited) {
                        /// @note ImGui の直接入力・ステップ操作・貼り付けのどの経路でも、
                        /// @note NaN や上限超過を .mask へ渡さず 0..1 に確定させる。
                        editedWeight = std::isfinite(editedWeight)
                            ? std::clamp(editedWeight, 0.0f, 1.0f)
                            : effectiveWeight;
                        setRule(path,
                                editedWeight > 0.001f
                                    ? MaskRuleState::Include : MaskRuleState::Exclude,
                                editedWeight);
                        maskDirty = true;
                        s_maskSelectedPath = path;
                        SetAnimationMaskPreviewSelection(path);
                    }
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                        ImGui::SetTooltip("Weight を直接入力 (0.00〜1.00)\nInherited の値を変更すると明示ルールを作成");

                    ImGui::TableNextColumn();
                    ImGui::TextColored(nodeColor, "%s", ruleName(state));
                    /// @note 同じ骨に複数エントリが当たると、勝つのは specificity が最大の 1 件だけ。
                    /// @note 残りは一度も効かないまま .mask に残り続けるため、ここで見えるようにする。
                    if (const auto matches =
                            asset::MatchAvatarMaskEntries(s_mask, path, node.name);
                        matches.size() > 1) {
                        ImGui::SameLine(0.0f, 6.0f);
                        ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning),
                                           "x%d", static_cast<int>(matches.size()));
                        if (ImGui::IsItemHovered()) {
                            std::string tooltip = std::to_string(matches.size())
                                + " 件のエントリが一致します (上が勝ち):";
                            for (std::size_t m = 0; m < matches.size(); ++m) {
                                const auto& match = matches[m];
                                const auto& matched =
                                    s_mask.entries[static_cast<size_t>(match.entryIndex)];
                                char line[320];
                                std::snprintf(line, sizeof(line),
                                    "\n%s %s  w %.2f depth %d  → %.2f",
                                    m == 0 ? "->" : "  ",
                                    matched.bonePath.c_str(), matched.weight,
                                    matched.blendDepth, match.weight);
                                tooltip += line;
                            }
                            if (matches.size() > 1)
                                tooltip += "\n\n勝たないエントリは一度も効きません。";
                            ImGui::SetTooltip("%s", tooltip.c_str());
                        }
                    }
                    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                        ImGui::SetDragDropPayload("FBZZ_MASK_BONE_PATH",
                                                  path.c_str(), path.size() + 1);
                        ImGui::TextUnformatted(path.c_str());
                        ImGui::EndDragDropSource();
                    }
                    ImGui::EndTable();
                }
                if (open) {
                    for (const int child : node.children)
                        drawNode(child, depth + 1);
                }
                ImGui::PopID();
                if (clicked) {
                    s_maskSelectedPath = path;
                    SetAnimationMaskPreviewSelection(path);
                }
            };

            /// @note 1 本も拾えていないマスクは、レイヤーへ割り当てても何も動かさない。
            /// @note 「設定はしてあるのに効かない」は画面から判別できないので、ここで名指しする。
            if (!s_mask.entries.empty() && !s_mask.defaultInclude &&
                maskaudit::CountMaskedBones(*maskSkeleton, s_mask) == 0) {
                ImGui::TextColored(EditorTheme::Color(ThemeColor::Danger),
                    "このマスクはスケルトンのどのボーンにも一致しません。"
                    "割り当てたレイヤーは何も動かしません (ボーン名/パスを確認)。");
            }

            ImGui::TextColored(EditorTheme::Color(ThemeColor::Success), "✓ Include");
            ImGui::SameLine();
            ImGui::TextColored(EditorTheme::Color(ThemeColor::Danger), "× Exclude");
            ImGui::SameLine();
            ImGui::TextColored(EditorTheme::Color(ThemeColor::TextMuted), "－ Inherit");
            ImGui::SameLine();
            ImGui::TextColored(EditorTheme::Color(ThemeColor::Warning), "~ Mixed");
            ImGui::SameLine();
            ImGui::TextDisabled("| Weight: 0 red → 1 green | Shift+クリックで範囲選択");
            ImGui::BeginChild("##mask_hierarchy", { 0.0f, 300.0f }, true);
            for (int i = 0; i < static_cast<int>(maskSkeleton->nodes.size()); ++i)
                if (maskSkeleton->nodes[static_cast<size_t>(i)].parentIndex < 0)
                    drawNode(i, 0);
            ImGui::EndChild();
        } else if (!s_mask.skeletonSourcePath.empty()) {
            ImGui::TextDisabled("スケルトンを読み込めませんでした。");
        } else {
            ImGui::TextDisabled("Skeleton Source に FBX を指定してください。");
        }

        ImGui::SeparatorText("Advanced Rules");
        int removeEntry = -1;
        if (ImGui::BeginTable("##mask_advanced_rules", 6,
                              ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_NoSavedSettings)) {
            ImGui::TableSetupColumn("Bone", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Weight", ImGuiTableColumnFlags_WidthFixed, 104.0f);
            ImGui::TableSetupColumn("Depth", ImGuiTableColumnFlags_WidthFixed, 94.0f);
            ImGui::TableSetupColumn("Ramp", ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableSetupColumn("Children", ImGuiTableColumnFlags_WidthFixed, 78.0f);
            ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, 28.0f);
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(s_mask.entries.size()); ++i) {
                auto& entry = s_mask.entries[static_cast<size_t>(i)];
                ImGui::PushID(i);
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                char boneBuffer[256];
                std::snprintf(boneBuffer, sizeof(boneBuffer), "%s", entry.bonePath.c_str());
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputText("##bone", boneBuffer, sizeof(boneBuffer))) {
                    entry.bonePath = boneBuffer;
                    maskDirty = true;
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    ImGui::SetTooltip("ボーン名 (\"Spine1\") またはパス (\"Hips/Spine/Spine1\")");

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::SliderFloat("##w", &entry.weight, 0.0f, 1.0f, "w %.2f"))
                    maskDirty = true;

                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::DragInt("##depth", &entry.blendDepth, 0.1f, 0, 8, "depth %d"))
                    maskDirty = true;
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip(
                        "0 なら配下一律。1 以上でこのボーンから下へ段階的に weight を立ち上げ、\n"
                        "上半身/下半身の境界でポーズが折れるのを防ぎます。\n"
                        "ランプは「指定したボーン自身」から始まるため、depth を上げるほど\n"
                        "起点の骨は weight に届かなくなります (右の Ramp 列が実効値)。");
                }

                /// @name ランプの実効値
                /// @note RampedWeight = weight×(depth+1)/(blendDepth+1)。「Chest に weight 1.0 /
                /// @note depth 2」は実効 0.33 になるが、この数字がどこにも出ていなかったため、
                /// @note 編集した場所へそのまま出す。
                ImGui::TableNextColumn();
                {
                    const std::vector<float> ramp =
                        maskaudit::BlendDepthRamp(entry.weight, entry.blendDepth);
                    std::string rampText;
                    for (std::size_t r = 0; r < ramp.size(); ++r) {
                        char value[16];
                        std::snprintf(value, sizeof(value), "%.2f", ramp[r]);
                        if (r > 0) rampText += " > ";
                        rampText += value;
                    }
                    const bool rootFallsShort =
                        !ramp.empty() && ramp.front() < entry.weight - 0.001f;
                    ImGui::TextColored(rootFallsShort
                            ? EditorTheme::Color(ThemeColor::Warning)
                            : EditorTheme::Color(ThemeColor::TextMuted),
                        "%s", rampText.c_str());
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                        if (rootFallsShort) {
                            ImGui::SetTooltip(
                                "起点の %s は %.2f までしか効きません "
                                "(Base Layer が %.0f%% 残ります)。\n"
                                "weight どおりに効かせたいなら depth を 0 にしてください。",
                                entry.bonePath.empty() ? "(bone)" : entry.bonePath.c_str(),
                                ramp.front(), (1.0f - ramp.front()) * 100.0f);
                        } else {
                            ImGui::SetTooltip("起点から順に、この実効ウェイトで効きます。");
                        }
                    }
                }

                ImGui::TableNextColumn();
                if (ImGui::Checkbox("##children", &entry.includeChildren))
                    maskDirty = true;

                ImGui::TableNextColumn();
                if (ImGui::SmallButton("x")) removeEntry = i;
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (removeEntry >= 0) {
            s_mask.entries.erase(s_mask.entries.begin() + removeEntry);
            maskDirty = true;
        }
        if (ImGui::Button("Add Bone Entry")) {
            s_mask.entries.push_back(asset::AvatarMaskEntry{});
            maskDirty = true;
        }

        /// @name スケルトンのボーンツリー (実効ウェイトのプレビュー)

        /// @name 保存
        const ImGuiID maskActiveAfter = ImGui::GetActiveID();
        auto pushMaskCommand = [&](const asset::AvatarMaskAsset& before,
                                   const asset::AvatarMaskAsset& after) {
            if (!ctx.undoStack || before == after) return;
            prepareMaskForSave();
            const asset::AvatarMaskAsset normalizedAfter = s_mask;
            if (before == normalizedAfter) return;

            EditorContext* context = &ctx;
            const std::string capturedPath = absPath;
            asset::AvatarMaskAsset* liveMask = &s_mask;
            std::string* livePath = &s_maskPath;
            auto* liveWriteTime = &s_maskWriteTime;
            auto apply = [context, capturedPath, liveMask, livePath, liveWriteTime](
                             const asset::AvatarMaskAsset& value) {
                /// @note 別の .mask を選択中に古い履歴を実行しても、現在表示中の値を壊さない。
                if (*livePath == capturedPath)
                    *liveMask = value;
                if (!asset::SaveAvatarMaskAsset(capturedPath, value)) return;

                if (*livePath == capturedPath) {
                    std::error_code error;
                    *liveWriteTime = std::filesystem::last_write_time(
                        capturedPath, error);
                }
                AssetDirtyRegistry::MarkClean(capturedPath);
                context->requestAssetBrowserRefresh = true;
            };
            ctx.undoStack->Push(std::make_unique<LambdaCommand>(
                "Edit Avatar Mask",
                [apply, normalizedAfter]() { apply(normalizedAfter); },
                [apply, before]() { apply(before); }));
        };

        if (!canRecordMaskUndo) {
            s_maskUndo.active = false;
            s_maskUndo.changed = false;
        } else {
            /// @note アセットを切り替えたときに、前の .mask の操作と現在の操作を混ぜない。
            if (s_maskUndo.active && s_maskUndo.path != absPath)
                s_maskUndo.active = false;

            if (!s_maskUndo.active) {
                if (maskActiveAfter != 0 && maskActiveAfter != maskActiveBefore) {
                    s_maskUndo.path = absPath;
                    s_maskUndo.before = maskBeforeDraw;
                    s_maskUndo.activeId = maskActiveAfter;
                    s_maskUndo.active = true;
                    s_maskUndo.changed = maskDirty;
                } else if (maskDirty) {
                    /// @note プリセット適用、ノードの一括変更、追加/削除などの即時操作。
                    pushMaskCommand(maskBeforeDraw, s_mask);
                }
            } else if (maskActiveAfter == s_maskUndo.activeId) {
                s_maskUndo.changed |= maskDirty;
            } else {
                /// @note 入力を離したフレームで、操作全体の before/after を 1 件に確定する。
                s_maskUndo.changed |= maskDirty;
                if (s_maskUndo.changed)
                    pushMaskCommand(s_maskUndo.before, s_mask);
                s_maskUndo.active = false;
                s_maskUndo.changed = false;
                if (maskActiveAfter != 0) {
                    s_maskUndo.path = absPath;
                    s_maskUndo.before = s_mask;
                    s_maskUndo.activeId = maskActiveAfter;
                    s_maskUndo.active = true;
                    s_maskUndo.changed = maskDirty;
                }
            }
        }

        ImGui::Spacing();
        const bool isMaskDirty = AssetDirtyRegistry::IsDirty(absPath) || maskDirty;
        if (maskDirty) {
            prepareMaskForSave();
            AssetDirtyRegistry::Register(
                absPath, util::FileSystem::GetFilename(absPath), "MASK",
                [path = absPath, maskCopy = s_mask, livePath = &s_maskPath,
                 liveWriteTime = &s_maskWriteTime]() {
                    const bool saved = asset::SaveAvatarMaskAsset(path, maskCopy);
                    if (saved && *livePath == path) {
                        std::error_code error;
                        *liveWriteTime = std::filesystem::last_write_time(path, error);
                    }
                    return saved;
                });
        }
        if (ImGui::Button("Apply") ||
            (isMaskDirty && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))) {
            prepareMaskForSave();
            if (asset::SaveAvatarMaskAsset(absPath, s_mask)) {
                std::error_code error;
                s_maskWriteTime = std::filesystem::last_write_time(absPath, error);
                AssetDirtyRegistry::MarkClean(absPath);
                ctx.requestAssetBrowserRefresh = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert")) {
            s_mask = asset::AvatarMaskAsset{};
            (void)asset::LoadAvatarMaskAsset(absPath, s_mask);
            std::error_code error;
            s_maskWriteTime = std::filesystem::last_write_time(absPath, error);
            s_maskUndo.active = false;
            s_maskUndo.changed = false;
            AssetDirtyRegistry::MarkClean(absPath);
        }
        if (isMaskDirty) {
            ImGui::SameLine();
            ImGui::TextColored({ 1.0f, 0.8f, 0.2f, 1.0f }, "Modified");
        }

        /// @note .mask 選択時は Inspector の最下部で、現在の編集状態をそのまま可視化する。
        /// @note 別 Preview ウィンドウへ切り替えずに、木構造の変更結果とメッシュの色を同じ視線で
        /// @note 確認できるようにする。保存前の s_mask を渡すため即時反映される。
        ImGui::SeparatorText("Animation Mask Preview");
        ImGui::TextDisabled("FBX mesh is colored by effective mask weight.");
        ImGui::TextColored(ImVec4(0.90f, 0.20f, 0.20f, 1.0f), "0.0 Excluded");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.90f, 0.85f, 0.20f, 1.0f), "0.5 Blended");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.20f, 0.90f, 0.35f, 1.0f), "1.0 Included");

        static AnimationMaskPreview s_animationMaskPreview;
        const float previewHeight =
            (std::max)(ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing(), 180.0f);
        if (!s_animationMaskPreview.DrawPreview(ctx, absPath, s_mask, previewHeight))
            ImGui::TextDisabled("FBX と .mask を指定するとプレビューできます。");
    } else if (ext == ".terrain") {
        /// @name .terrain アセット
        /// @note オンディスク形式は scene::TerrainAssetSerializer が読み書きする TOML。SceneSerializer /
        /// @note ランタイムも同じ経路でロードするため Inspector もこれを使う。旧 asset::FzTerrainSerializer は
        /// @note バイナリ "FZTN" 形式で、TOML を読めず保存するとファイルを破壊する。
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

            ImGui::SeparatorText("Holes");
            ImGui::LabelText("Hole Cells", "%zu / %zu", ta.CountHoles(), ta.CellCount());

            ImGui::SeparatorText("Layer Materials");
            ImGui::LabelText("Layers", "%d", ta.LayerCount());
            if (ImGui::SliderFloat("Height Blend Depth", &ta.heightBlendDepth, 0.01f, 1.0f, "%.2f"))
                terrainDirty = true;
            int pendingRemove = -1;
            for (int li = 0; li < ta.LayerCount(); ++li) {
                ImGui::PushID(li);
                const std::string layerLabel = "Layer " + std::to_string(li);
                if (widgets::AssetPathField(layerLabel.c_str(), ta.layerMaterials[static_cast<size_t>(li)], ".mat", ctx.projectRoot))
                    terrainDirty = true;
                ImGui::BeginDisabled(ta.LayerCount() <= 1);
                if (ImGui::SmallButton("Remove")) pendingRemove = li;
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            /// @note 添字と PushID がずれないよう、削除はループの外で 1 つだけ当てる。
            if (pendingRemove >= 0 && ta.RemoveLayer(pendingRemove))
                terrainDirty = true;
            ImGui::BeginDisabled(ta.LayerCount() >= scene::TERRAIN_MAX_LAYERS);
            if (ImGui::Button("Add Layer") && ta.AddLayer(DefaultTerrainLayerMaterialPath(ta.LayerCount())) >= 0)
                terrainDirty = true;
            ImGui::EndDisabled();

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
        /// @name DataAsset (純共有 ScriptableObject)
        const std::string relPath = NormalizeAssetPath(absPath);
        asset::DataAsset* data = asset::DataAssetRegistry::Resolve(relPath);
        if (!data) {
            ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f},
                "Failed to load .fzdata (型が未登録か、パース失敗)");
            return;
        }

        ImGui::TextDisabled("Type: %s", data->GetTypeName());
        ImGui::Separator();

        /// @note 編集前の状態を控える。DataAsset は多態基底で値コピーできないため、.mat のような
        /// @note 構造体まるごとのコピーでなく TOML 直列化を控えに使う。どのウィジェットが掴まれるかは
        /// @note 描画前には分からないため毎フレーム取るが、選択中の 1 アセットに対する文字列 1 本ぶんで済む。
        const bool canRecordFzDataUndo = CanRecordEditorUndo(ctx);
        const std::string fzdataBeforeDraw = canRecordFzDataUndo
            ? asset::DataAssetRegistry::Snapshot(relPath)
            : std::string{};
        const ImGuiID fzdataActiveBefore = ImGui::GetActiveID();

        /// @note 各オーバーライドのパラメーターは共通リフレクタで描く。Reflect() が既にレンジと
        /// @note カラーヒントを持ち、ImGuiReflector がそれをプロパティ行・カラーピッカー・ファイル
        /// @note スロットへ翻訳するため、効果ごとの専用 UI が要らず効果の追加は Engine 側だけで閉じる。
        ImGuiReflector reflector;
        reflector.m_projectRoot = ctx.projectRoot;

        bool editedByCustomUi = false;
        if (auto* profile = dynamic_cast<asset::PostProcessProfile*>(data)) {
            /// @note PostProcessProfile だけは専用 UI を使う。中身は「効果のリスト」で、追加・削除・
            /// @note 並べ替え・一時無効化という配列固有の操作は汎用のフィールド列挙では表現できない。
            /// @note 編集対象は Registry の共有実体そのものなので、変更は全参照へ即反映される。
            /// @note レンダー設定を渡すと「このパイプラインでは効かない」効果に警告が出る。
            const PostProcessInspectorResult inspectorResult =
                DrawVolumeOverrideListInspector(*profile, reflector,
                                                &ctx.projectSettings.render);
            editedByCustomUi = inspectorResult.changed;
        } else {
            /// @note Inspector の共通リフレクタでフィールドを描画する (スクリプトと同じ UI)。
            data->Reflect(reflector);
        }

        /// @note 自動保存: 値が編集され、操作 (ドラッグ/入力) が終わった瞬間にディスクへ書き戻す
        /// @note (ScriptableObject 的な「いじったら保存されている」体験)。連続ドラッグ中の大量書き込みを
        /// @note 避けるため、アクティブ操作が無くなったフレームでだけ保存する。
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

        /// @name Undo 記録
        /// @note .fzdata は自動保存でディスクへ即書き戻すため、Undo が無いと値を壊しても戻せない。
        /// @note 記録の粒度は .mat と同じ「ウィジェットを掴んでから離すまで = 1 操作」。
        /// @note フレーム単位で積むとドラッグ 1 回が数十件の中間値で履歴を埋めてしまう。
        struct FzDataUndoTracker {
            std::string path;      ///< @brief どの .fzdata に対する記録か
            std::string before;    ///< @brief 掴んだ直前のスナップショット
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
                /// @note 復元した値はディスクへも書き戻す。.fzdata は編集確定ごとに自動保存されるため、
                /// @note メモリだけ戻すと次のロードやホットリロードで巻き戻る (.mat と同じ規則へ揃える)。
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
            /// @note 選択が別の .fzdata へ移ったら記録途中の操作は捨てる
            /// @note (別アセットの値で before/after が混ざるのを防ぐ)。
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
                    /// @note 掴まずに 1 フレームで確定した編集 (オーバーライドの追加・削除など)。
                    /// @note 掴み→離しの経路に乗らないので、その場で 1 操作として積む。
                    pushFzDataCommand(fzdataBeforeDraw,
                                      asset::DataAssetRegistry::Snapshot(relPath));
                }
            } else if (fzdataActiveAfter == fzdataUndo.activeId) {
                /// @note ドラッグ継続中
                fzdataUndo.changed |= editedThisFrame;
            } else {
                /// @note 離したフレーム。ボタンは「離した瞬間」に効くので、この 1 回ぶんも拾う。
                fzdataUndo.changed |= editedThisFrame;
                if (fzdataUndo.changed)
                    pushFzDataCommand(fzdataUndo.before,
                                      asset::DataAssetRegistry::Snapshot(relPath));
                fzdataUndo.active  = false;
                fzdataUndo.changed = false;
                /// @note 離した直後に別ウィジェットを掴んでいたら、そこから記録し直す。
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
        /// @note 保険の手動保存 (自動保存があるので通常は不要)。
        if (ImGui::Button("Save .fzdata"))
            asset::DataAssetRegistry::Save(relPath);
        ImGui::SameLine();
        ImGui::TextDisabled(s_fzdataDirty && s_fzdataDirtyPath == relPath
                            ? "Saving on release..." : "Auto-saved");
    } else if (ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac") {
        /// @note 録音素材の試聴。テクスチャにプレビューがあって音だけ無いと、どんな音かは
        /// @note ゲームを走らせるまで分からず探す手間が桁違いになる。
        static std::string        s_audioPath;
        static std::vector<float> s_audioWave;
        static uint32_t           s_audioVoice = 0;
        static bool               s_audioLoop  = false;
        static bool               s_audioDecoded = false;
        static float              s_audioSeconds = 0.0f;
        static std::string        s_audioFormat;

        const std::string relPath = NormalizeAssetPath(absPath);
        auto* manager = core::Application::Get().GetAudioManager();

        if (s_audioPath != absPath) {
            /// @note 別のアセットへ移ったら試聴も止める。裏で鳴り続けると、どのファイルの
            /// @note 音を聞いているのか分からなくなる。
            if (manager && s_audioVoice != 0) manager->StopVoice(s_audioVoice);
            s_audioVoice = 0;
            s_audioPath  = absPath;
            s_audioWave.clear();
            s_audioDecoded = false;
            s_audioSeconds = 0.0f;
            s_audioFormat.clear();
        }

        std::error_code sizeError;
        const auto fileBytes =
            std::filesystem::file_size(util::FileSystem::PathFromUtf8(absPath), sizeError);
        const uintmax_t sizeOnDisk = sizeError ? 0 : fileBytes;
        ImGui::Text("File: %.1f KB", static_cast<double>(sizeOnDisk) / 1024.0);

        /// @note 大きいファイルを自動デコードしないのは、デコード結果が AudioManager のキャッシュに
        /// @note 載り Shutdown まで解放されないため。数分の BGM フォルダを見て回るだけで数百 MB 積み上がる。
        constexpr uintmax_t kAutoDecodeLimit = 2u * 1024u * 1024u;
        const bool wantDecode = s_audioDecoded || sizeOnDisk <= kAutoDecodeLimit;

        if (!manager) {
            ImGui::TextDisabled("オーディオデバイスが初期化されていません");
        } else {
            if (!s_audioDecoded && !wantDecode) {
                if (ImGui::Button("Load Preview"))
                    /// @note 次のフレームで復号する
                    s_audioDecoded = true;
                ImGui::SameLine();
                ImGui::TextDisabled("(2 MB 超のためクリックで読み込み)");
            }

            if (wantDecode && s_audioWave.empty()) {
                const auto clip = manager->AcquireClip(relPath);
                audio::AudioManager::ClipInfo info;
                if (clip != 0 && manager->DescribeClip(clip, info)) {
                    s_audioDecoded = true;
                    s_audioSeconds = info.durationSeconds;

                    char formatText[96];
                    std::snprintf(formatText, sizeof(formatText), "%u Hz  %u ch  %u bit",
                                  info.fmt.sampleRate, info.fmt.channels, info.fmt.bitsPerSample);
                    s_audioFormat = formatText;

                    /// @note 16bit 以外は波形描画を諦める (再生はできる)。
                    if (info.fmt.bitsPerSample == 16 && info.fmt.channels > 0) {
                        const size_t frames =
                            info.bytes / (2u * static_cast<size_t>(info.fmt.channels));
                        const size_t buckets = (std::min)(static_cast<size_t>(512), frames);
                        s_audioWave.assign((std::max)(buckets, static_cast<size_t>(1)), 0.0f);
                        const size_t stride = static_cast<size_t>(info.fmt.channels) * 2u;
                        for (size_t i = 0; i < buckets; ++i) {
                            const size_t begin = i * frames / buckets;
                            const size_t end = (std::max)(begin + 1, (i + 1) * frames / buckets);
                            int peak = 0;
                            int peakMagnitude = 0;
                            for (size_t f = begin; f < end && f < frames; ++f) {
                                /// @note 先頭チャンネルのみ。左右差より「どこで鳴っているか」が知りたい。
                                const size_t at = f * stride;
                                const auto lo = static_cast<uint16_t>(info.pcm[at]);
                                const auto hi = static_cast<uint16_t>(info.pcm[at + 1]);
                                const int value = static_cast<int16_t>(
                                    static_cast<uint16_t>(lo | (hi << 8)));
                                const int magnitude = value < 0 ? -value : value;
                                if (magnitude > peakMagnitude) {
                                    peakMagnitude = magnitude;
                                    peak = value;
                                }
                            }
                            s_audioWave[i] = static_cast<float>(peak) / 32768.0f;
                        }
                    }
                } else if (wantDecode) {
                    ImGui::TextColored({ 1.0f, 0.4f, 0.4f, 1.0f }, "デコードできません");
                }
            }

            if (s_audioDecoded) {
                ImGui::Text("%s  /  %.2f s", s_audioFormat.c_str(), s_audioSeconds);
                if (!s_audioWave.empty()) {
                    ImGui::PlotLines("##audiowave", s_audioWave.data(),
                                     static_cast<int>(s_audioWave.size()), 0, nullptr,
                                     -1.0f, 1.0f, ImVec2(-1.0f, 60.0f));
                }

                const bool playing = s_audioVoice != 0 && manager->IsVoicePlaying(s_audioVoice);
                if (!playing) s_audioVoice = 0;

                if (ImGui::Button(playing ? "Stop" : "Play")) {
                    if (playing) {
                        manager->StopVoice(s_audioVoice);
                        s_audioVoice = 0;
                    } else {
                        s_audioVoice = manager->PlayVoice(relPath, s_audioLoop,
                                                          manager->FindBus("UI"));
                    }
                }
                ImGui::SameLine();
                if (ImGui::Checkbox("Loop", &s_audioLoop) && s_audioVoice != 0) {
                    /// @note ループ指定は voice 生成時にしか渡せないため、鳴らし直す。
                    manager->StopVoice(s_audioVoice);
                    s_audioVoice = manager->PlayVoice(relPath, s_audioLoop,
                                                      manager->FindBus("UI"));
                }
            }
        }
    } else if (ext == ".synth") {
        /// @note ここでは編集させない。SFX Editor が同じファイルの内容をメモリに持って編集するため、
        /// @note 両方から書けると片方の未保存の変更をもう片方が黙って上書きする。ここは「今どんな音か」
        /// @note を確かめ、必要なら編集面へ移るための面に絞る。
        static std::string       s_synthPath;
        static asset::SynthAsset s_synth;
        static bool              s_synthValid = false;
        if (s_synthPath != absPath) {
            s_synthPath  = absPath;
            s_synthValid = asset::LoadSynthAssetFromFile(absPath, s_synth);
        }
        if (!s_synthValid) {
            ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, "Failed to load .synth");
            return;
        }

        const audio::SynthSpec& spec = s_synth.spec;
        ImGui::Text("Preset : %s",
                    s_synth.presetName.empty() ? "(custom)" : s_synth.presetName.c_str());
        ImGui::Text("Wave   : %s", audio::WaveToString(spec.wave));
        ImGui::Text("Length : %.3f s", spec.attack + spec.sustain + spec.decay);
        ImGui::Text("Pitch  : %.0f Hz (slide %+.2f oct/s)", spec.startFrequency, spec.slide);
        ImGui::Separator();

        if (ImGui::Button("Play")) {
            if (auto* manager = core::Application::Get().GetAudioManager()) {
                const auto clip = manager->AcquireGeneratedClip(spec);
                if (clip != 0) {
                    (void)manager->PlayClipVoice(clip, false, manager->FindBus("UI"));
                    manager->ReleaseClip(clip);
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Open in SFX Editor")) {
            SelectAsset(ctx, absPath);
            ctx.requestOpenSfxEditor = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reload")) s_synthPath.clear();
    } else if (ext == ".physmat") {
        /// @name PhysicsMaterial (共有物理マテリアル)
        /// @note AssetManager 上の実体を直接編集する。ColliderComponent::ResolvePhysicsMaterial() が
        /// @note 毎フレーム同じキャッシュから値を引くため、書き換えれば参照している全コライダーの
        /// @note 物性が再ロードもシーン再生も挟まずその場で変わる (アセットを共有にした本来の狙い)。
        const std::string relPath = NormalizeAssetPath(absPath);
        const auto handle = asset::AssetManager::Load<asset::PhysicsMaterialAsset>(relPath);
        auto* physicsMaterial = asset::AssetManager::Get<asset::PhysicsMaterialAsset>(handle);
        if (!physicsMaterial) {
            ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, "Failed to load .physmat");
            return;
        }

        /// @note 掴む直前の値を毎フレーム控える (.fzdata と同じ理由: どのウィジェットが
        /// @note 掴まれるかは描画前に分からない)。中身は float 数個なのでコピーは無視できる。
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

        /// @note フィールドは共通リフレクタで描く (Inspector と TOML 出力で同じ Reflect を共有)。
        ImGuiReflector reflector;
        reflector.m_projectRoot = ctx.projectRoot;
        physicsMaterial->Reflect(reflector);

        ImGui::Spacing();
        ImGui::TextDisabled("合成規則が異なる材質同士では、優先度の高い側が採用される");
        ImGui::TextDisabled("(Average < Geometric Mean < Minimum < Multiply < Maximum)");

        /// @note 自動保存 (.fzdata と同じ規則: 操作が終わったフレームで書き戻す)。
        const bool editedThisFrame =
            (GImGui && GImGui->ActiveIdHasBeenEditedThisFrame) || editedByPreset;
        static bool        s_physmatDirty = false;
        static std::string s_physmatDirtyPath;
        if (editedThisFrame) {
            s_physmatDirty     = true;
            s_physmatDirtyPath = relPath;
            /// @note プリセット以外の手編集はプリセット由来の表示を外す。
            if (!editedByPreset) physicsMaterial->presetName.clear();
        }
        if (s_physmatDirty && s_physmatDirtyPath == relPath && !ImGui::IsAnyItemActive()) {
            if (asset::SavePhysicsMaterialAssetToFile(absPath, *physicsMaterial))
                ctx.requestAssetBrowserRefresh = true;
            s_physmatDirty = false;
        }

        /// @note Undo: 掴んでから離すまでを 1 操作として積む (.mat / .fzdata と同じ粒度)。
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
                /// @note メモリだけ戻すと次のロードで巻き戻るため、ディスクへも書き戻す。
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
                    /// @note プリセット適用は掴み→離しの経路に乗らないため、その場で 1 操作にする。
                    pushPhysMatCommand(beforeDraw, *physicsMaterial);
                }
            } else if (activeAfter != physmatUndo.activeId) {
                /// @note 掴んでいたウィジェットから手が離れた = 1 操作の終わり。
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
    } else if (ext == ".fluid") {
        DrawFluidAssetInspector(absPath, ctx.resources, ctx.imguiRenderer, &ctx.requestVolumeFlipbookFluid);
    } else if (ext == ".fga" || asset::IsVectorFieldPng(absPath)) {
        /// @name VectorField (焼いた速度場)
        /// @note 焼き直しは «レシピ → グリッド» の一方通行で、元のレシピはファイルに残らない
        /// @note (残すと «ファイルの中身とレシピのどちらが正か» が生まれる)。ここはあくまで
        /// @note «この設定で焼き直す» ボタンで、開くたびに既定のレシピが表示される。
        const std::string relPath = NormalizeAssetPath(absPath);
        const auto handle = asset::AssetManager::Load<fluid::VectorFieldAsset>(relPath);
        auto* field = asset::AssetManager::Get<fluid::VectorFieldAsset>(handle);
        if (!field) {
            ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, "Failed to load %s", ext.c_str());
            return;
        }

        ImGui::Text("Grid: %u x %u x %u  (%zu vectors)",
                    field->sizeX, field->sizeY, field->sizeZ, field->data.size());
        ImGui::Text("Bounds: [%.2f %.2f %.2f] .. [%.2f %.2f %.2f]",
                    field->boundsMin.x, field->boundsMin.y, field->boundsMin.z,
                    field->boundsMax.x, field->boundsMax.y, field->boundsMax.z);
        ImGui::Text("Max magnitude: %.3f", field->maxMagnitude);
        /// @note 取り込みで固定解像度へ揃えるので、焼いた解像度と表示は一致しないことがある。
        ImGui::TextDisabled("取り込み時に %u³ へ揃えられます (GPU アトラスのタイル寸法)",
                            fluid::kVelocityFieldTileResolution);

        ImGui::Spacing();
        ImGui::SeparatorText("Re-bake");

        static const char* kRecipeLabels[] = { "Curl", "Vortex", "Tornado", "Sphere", "Turbulence" };
        static int   s_recipe     = 0;
        static int   s_resolution = 32;
        static float s_extents[3] = { 5.0f, 5.0f, 5.0f };
        static int   s_seed       = 1;
        static float s_strength   = 1.0f;

        ImGui::Combo("Recipe", &s_recipe, kRecipeLabels, IM_ARRAYSIZE(kRecipeLabels));
        ImGui::SliderInt("Resolution", &s_resolution, 8, 64);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("1 辺のテクセル数。64 で 262144 ベクトルになります。\n"
                              "粒子の乱流としては 32 前後で十分で、細かくしても絵には出ません。");
        ImGui::DragFloat3("Extents [m]", s_extents, 0.05f, 0.01f, 1000.0f);
        ImGui::DragInt("Seed", &s_seed, 1, 1, 100000);
        ImGui::DragFloat("Strength", &s_strength, 0.01f, -100.0f, 100.0f);

        if (ImGui::Button("Bake and Save")) {
            fluid::VectorFieldAsset baked;
            fluid::BakeVectorField(static_cast<fluid::VectorFieldRecipe>(s_recipe),
                                   static_cast<uint32_t>(s_resolution),
                                   { s_extents[0], s_extents[1], s_extents[2] },
                                   static_cast<uint32_t>(s_seed), s_strength, baked);
            /// @note 拡張子が .fga でも書き出しは PNG 形式。読み込みだけが両対応で、
            /// @note «Unreal の形式で書き戻す» 用途は無い。
            std::string savePath = absPath;
            if (ext == ".fga") savePath = absPath.substr(0, absPath.size() - 4) + "_Velocity.png";
            if (asset::SaveVectorField(savePath, baked)) {
                /// @note ストア上の実体を差し替える。GPU テクスチャは古い物が残るが、
                /// @note ここで手放すと同じフレームに描いているパスが無効ハンドルを引く。
                /// @note 差し替えの正しい手順はプロジェクトの再読み込み。
                asset::AssetManager::Unload<fluid::VectorFieldAsset>(
                    NormalizeAssetPath(savePath));
                asset::AssetManager::BumpAssetGeneration();
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("焼き直すと、この場を参照する全エミッターの流れが変わります。");
    } else {
        ImGui::TextDisabled("Type: %s", ext.c_str());
        ImGui::Spacing();
        ImGui::TextDisabled("Drag from Asset Browser to assign to a field.");
    }

}

} /// @note namespace fbzz::editor
