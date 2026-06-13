// FBZZ Engine
// Inspector/InspectorPanel_Asset.cpp | fbzz::editor
// Asset Browser から選択したファイル用 Inspector
#include <Editor/Panels/InspectorPanel.hpp>
#include <Editor/Panels/AnimationGraphInspector.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/MaterialInspectorWidgets.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
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
#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace fbzz::editor {

void InspectorPanel::DrawAssetInspector(EditorContext& ctx, const std::string& assetPath)
{
    const std::string absPath = assetPath;
    const std::string filename = util::FileSystem::GetFilename(absPath);
    const std::string ext      = util::StringUtils::ToLower(util::FileSystem::GetExtension(absPath));

    ImGui::TextUnformatted(filename.c_str());
    ImGui::TextDisabled("%s", absPath.c_str());
    ImGui::Separator();

    if (ext == ".fzmat") {
        const std::string relPath = NormalizeAssetPath(absPath);
        if (m_inspectedAssetPath != absPath || !m_inspectedMat.IsValid()) {
            m_inspectedAssetPath = absPath;
            m_inspectedMat = asset::AssetManager::LoadMaterial(relPath);
        }

        if (!m_inspectedMat.IsValid()) {
            ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, "Failed to load .fzmat");
            return;
        }

        auto* matPtr = asset::AssetManager::GetMaterial(m_inspectedMat);
        if (!matPtr) {
            ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, "Failed to load .fzmat");
            return;
        }

        asset::MaterialAsset& mat = *matPtr;
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
                materialDirty = true;
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                    const std::string dropped = NormalizeAssetPath(static_cast<const char*>(p->Data));
                    if (util::StringUtils::EndsWith(dropped, ".hlsl")) {
                        mat.shaderPath = dropped;
                        materialDirty = true;
                    }
                }
                ImGui::EndDragDropTarget();
            }

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
            materialDirty |= DrawTerrainMaterialInspector(mat);
        } else if (isWaterMaterial) {
            materialDirty |= DrawWaterMaterialInspector(mat);
        } else {
            ImGui::SeparatorText("Textures");
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
                if (NormalizeAssetPath(terrain.materialPath) == relPath)
                    terrain.splatDirty = true;
            }
            for (auto [water] : ctx.activeScene->View<scene::WaterComponent>()) {
                if (NormalizeAssetPath(water.materialPath) == relPath) {
                    water.texDirty = true;
                    water.foamDirty = true;
                }
            }
        }

        ImGui::Separator();
        if (ImGui::Button("Save .fzmat")) {
            if (asset::SaveMaterialAssetToFile(absPath, mat))
                ctx.requestAssetBrowserRefresh = true;
        }
        if (materialDirty) {
            ImGui::SameLine();
            ImGui::TextColored({1.0f, 0.8f, 0.2f, 1.0f}, "Modified");
        }
    } else if (ext == ".fbzzanimcontroller") {
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
    } else {
        ImGui::TextDisabled("Type: %s", ext.c_str());
        ImGui::Spacing();
        ImGui::TextDisabled("Drag from Asset Browser to assign to a field.");
    }
}

} // namespace fbzz::editor
