// FBZZ Engine
// InspectorTerrainWater.cpp | fbzz::editor
// Terrain / Water 系 Component の Inspector 描画
#include "InspectorTerrainWater.hpp"
#include <Engine/Scene/Components/TerrainDetailComponent.hpp>
#include <Engine/Scene/Components/FoliageComponent.hpp>

namespace fbzz::editor {

void DrawTerrainWaterInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType)
{
    DrawComponentSection<scene::TerrainComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Terrain",
        [go](scene::TerrainComponent& tc, EditorContext& ctx) {
            // ── 外部 Terrain Asset ─────────────────────────────────────────────
            ImGui::SeparatorText("Asset");
            {
                char pathBuf[512];
                std::snprintf(pathBuf, sizeof(pathBuf), "%s", tc.terrainAssetPath.c_str());
                if (ImGui::InputText("Asset Path", pathBuf, sizeof(pathBuf)))
                    tc.terrainAssetPath = NormalizeAssetPath(pathBuf);
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                        std::string path = NormalizeAssetPath(static_cast<const char*>(p->Data));
                        if (util::FileSystem::GetExtension(path) == ".terrain")
                            tc.terrainAssetPath = path;
                    }
                    ImGui::EndDragDropTarget();
                }

                if (tc.terrainAssetPath.empty()) {
                    if (ImGui::Button("Create Terrain Asset")) {
                        const std::string path = UniqueTerrainAssetPath(ctx, go ? go->name : "Terrain");
                        if (scene::TerrainAssetSerializer::Save(tc, TerrainAssetDiskPath(ctx, path))) {
                            tc.terrainAssetPath = path;
                            ctx.requestAssetBrowserRefresh = true;
                        }
                    }
                } else {
                    if (ImGui::Button("Save Asset")) {
                        scene::TerrainAssetSerializer::Save(tc, TerrainAssetDiskPath(ctx, tc.terrainAssetPath));
                        ctx.requestAssetBrowserRefresh = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Load Asset")) {
                        const bool enabled = tc.enabled;
                        const std::string path = tc.terrainAssetPath;
                        if (scene::TerrainAssetSerializer::Load(TerrainAssetDiskPath(ctx, path), tc)) {
                            tc.enabled = enabled;
                            tc.terrainAssetPath = path;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Unlink")) {
                        tc.terrainAssetPath.clear();
                    }
                }
            }

            // ── グリッド設定 ──────────────────────────────────────────────────
            ImGui::SeparatorText("Grid");
            if (ImGui::DragInt("Columns",    &tc.columns,   1.0f, 2, 4097))
                tc.heightDirty = true;
            if (ImGui::DragInt("Rows",       &tc.rows,      1.0f, 2, 4097))
                tc.heightDirty = true;
            if (ImGui::DragFloat("Cell Size",   &tc.cellSize,  0.01f, 0.01f, 100.0f))
                tc.heightDirty = true;
            if (ImGui::DragFloat("Max Height",  &tc.maxHeight, 0.5f, 0.5f, 1000.0f))
                tc.heightDirty = true;
            ImGui::DragInt("Chunk Size", &tc.chunkSize, 1.0f, 8, 256);

            // ── Layer Materials (.fzmat × 4) ──────────────────────────────────
            ImGui::SeparatorText("Layer Materials");
            static const char* kLayerNames[] = { "Layer 0", "Layer 1", "Layer 2", "Layer 3" };
            for (int li = 0; li < 4; ++li) {
                ImGui::PushID(li);
                ImGui::TextUnformatted(kLayerNames[li]);
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s", tc.layerMaterials[li].c_str());
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x
                    - (tc.layerMaterials[li].empty() ? 0.0f
                       : ImGui::CalcTextSize("Clear").x
                         + ImGui::GetStyle().FramePadding.x * 2.0f
                         + ImGui::GetStyle().ItemInnerSpacing.x));
                if (ImGui::InputText("##mat", buf, sizeof(buf))) {
                    tc.layerMaterials[li] = NormalizeAssetPath(buf);
                    tc.splatDirty = true;
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                        std::string dropped = NormalizeAssetPath(static_cast<const char*>(p->Data));
                        if (util::StringUtils::EndsWith(dropped, ".fzmat")) {
                            tc.layerMaterials[li] = dropped;
                            tc.splatDirty = true;
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                if (!tc.layerMaterials[li].empty()) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Clear")) {
                        tc.layerMaterials[li].clear();
                        tc.splatDirty = true;
                    }
                    auto matHandle = asset::AssetManager::LoadMaterial(tc.layerMaterials[li]);
                    if (auto* mat = asset::AssetManager::GetMaterial(matHandle)) {
                        {
                            const auto f = DrawTerrainLayerMaterialInspector(*mat);
                            if (f.textureDirty) tc.splatDirty         = true;
                            if (f.paramDirty)   tc.materialParamDirty = true;
                        }
                        if (ImGui::Button("Save .fzmat")) {
                            asset::SaveMaterialAssetToFile(
                                MaterialAssetDiskPath(ctx, tc.layerMaterials[li]), *mat);
                            ctx.requestAssetBrowserRefresh = true;
                        }
                    } else {
                        ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.25f, 1.0f),
                                           "Missing: %s", tc.layerMaterials[li].c_str());
                    }
                } else {
                    if (ImGui::Button("Use Default")) {
                        tc.layerMaterials[li] = DefaultTerrainLayerMaterialPath(li);
                        tc.splatDirty = true;
                    }
                }
                ImGui::PopID();
                ImGui::Spacing();
            }

            // ── コライダー ─────────────────────────────────────────────────────
            ImGui::SeparatorText("Collider");
            if (ImGui::Button("Rebuild Collider Now")) {
                tc.colliderDirty = true;
            }

            // ── デバッグ情報 ───────────────────────────────────────────────────
            ImGui::SeparatorText("Info");
            ImGui::Text("Vertices : %d", tc.columns * tc.rows);
            ImGui::Text("Triangles: %d", (tc.columns - 1) * (tc.rows - 1) * 2);
            ImGui::TextColored(tc.heightDirty   ? ImVec4{1,0.5f,0.2f,1} : ImVec4{0.5f,1,0.5f,1},
                               "Height Dirty : %s", tc.heightDirty   ? "Yes" : "No");
            ImGui::TextColored(tc.colliderDirty ? ImVec4{1,0.5f,0.2f,1} : ImVec4{0.5f,1,0.5f,1},
                               "Collider Dirty: %s", tc.colliderDirty ? "Yes" : "No");
        });

    DrawComponentSection<scene::WaterComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Water",
        [go](scene::WaterComponent& water, EditorContext&) {
            // ── Material (.fzmat) ─────────────────────────────────────────────
            ImGui::SeparatorText("Material (.fzmat)");
            char matBuf[256];
            std::snprintf(matBuf, sizeof(matBuf), "%s", water.materialPath.c_str());
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 4.0f);
            if (ImGui::InputText("##water_mat_path", matBuf, sizeof(matBuf))) {
                water.materialPath = NormalizeAssetPath(matBuf);
                water.texDirty = true;
                water.foamDirty = true;
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                    std::string dropped = NormalizeAssetPath(static_cast<const char*>(p->Data));
                    if (util::StringUtils::EndsWith(dropped, ".fzmat")) {
                        water.materialPath = dropped;
                        water.texDirty = true;
                        water.foamDirty = true;
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (water.materialPath.empty()) {
                ImGui::TextDisabled("(no material — visual params missing)");
                if (ImGui::Button("Use Default Water Material")) {
                    water.materialPath = DefaultWaterMaterialPath();
                    water.texDirty = true;
                    water.foamDirty = true;
                }
            }

            // ── Geometry ─────────────────────────────────────────────────────
            ImGui::Spacing();
            ImGui::SeparatorText("Geometry");
            int resX = static_cast<int>(water.resolutionX);
            int resZ = static_cast<int>(water.resolutionZ);
            if (ImGui::DragInt("Resolution X", &resX, 1.0f, 1, 512)) {
                water.resolutionX = static_cast<uint32_t>(resX);
                water.meshDirty = true;
                water.foamDirty = true;
            }
            if (ImGui::DragInt("Resolution Z", &resZ, 1.0f, 1, 512)) {
                water.resolutionZ = static_cast<uint32_t>(resZ);
                water.meshDirty = true;
                water.foamDirty = true;
            }
            if (ImGui::DragFloat("Extent X", &water.extentX, 0.5f, 0.1f, 10000.0f)) {
                water.meshDirty = true;
                water.foamDirty = true;
            }
            if (ImGui::DragFloat("Extent Z", &water.extentZ, 0.5f, 0.1f, 10000.0f)) {
                water.meshDirty = true;
                water.foamDirty = true;
            }
            {
                int chunks = static_cast<int>(water.chunkCount);
                if (ImGui::DragInt("Chunk Count", &chunks, 1.0f, 1, 64)) {
                    water.chunkCount = static_cast<uint32_t>(chunks < 1 ? 1 : chunks);
                    water.meshDirty = true;
                }
                ImGui::TextDisabled("(%d x %d chunks = %d draw calls)", chunks, chunks, chunks * chunks);
            }

            // ── Physics ───────────────────────────────────────────────────────
            ImGui::SeparatorText("Physics");
            if (ImGui::Button("Setup Buoyancy Volume")) {
                auto* box = go->GetComponent<scene::BoxColliderComponent>();
                if (!box) box = &go->AddComponent<scene::BoxColliderComponent>();
                box->enabled   = true;
                box->isTrigger = true;
                box->center    = { 0.0f, -2.5f, 0.0f };
                box->size      = { water.extentX, 5.0f, water.extentZ };

                auto* volume = go->GetComponent<scene::VolumeComponent>();
                if (!volume) volume = &go->AddComponent<scene::VolumeComponent>();
                volume->enabled  = true;
                volume->type     = physics::VolumeType::Buoyancy;
                volume->buoyancy = 15.0f;
                volume->drag     = 2.0f;
                volume->duration = -1.0f;
                volume->elapsed  = 0.0f;
            }
            ImGui::SameLine();
            if (ImGui::Button("Sync Collider Size")) {
                if (auto* box = go->GetComponent<scene::BoxColliderComponent>()) {
                    box->isTrigger = true;
                    box->size      = { water.extentX, box->size.y, water.extentZ };
                }
            }

            // ── Gerstner Waves ────────────────────────────────────────────────
            ImGui::SeparatorText("Gerstner Waves");
            ImGui::Checkbox("Enable Waves", &water.enableGerstnerWaves);
            for (int i = 0; i < static_cast<int>(water.waves.size()); ++i) {
                auto& wave = water.waves[static_cast<size_t>(i)];
                ImGui::PushID(i);
                if (ImGui::TreeNodeEx("Wave", ImGuiTreeNodeFlags_DefaultOpen, "Wave %d", i)) {
                    DragVec2("Direction", wave.direction, 0.01f, -1.0f, 1.0f);
                    ImGui::DragFloat("Amplitude",  &wave.amplitude,  0.01f, 0.0f,  100.0f);
                    ImGui::DragFloat("Wavelength", &wave.wavelength, 0.1f,  0.01f, 10000.0f);
                    ImGui::DragFloat("Steepness",  &wave.steepness,  0.01f, 0.0f,  1.0f);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
        });

    // ============================================================
    // TerrainDetailComponent — Detail レイヤー編集 UI
    // ============================================================

    DrawComponentSection<scene::TerrainDetailComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Terrain Detail",
    [go](scene::TerrainDetailComponent& tdc, EditorContext&) {

        // Bake ボタン: インスタンス配列を再生成する
        if (ImGui::Button("Bake All Layers")) {
            tdc.chunks.clear();
            tdc.needsBake = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%zu chunks cached", tdc.chunks.size());

        ImGui::Spacing();
        ImGui::SeparatorText("Layers");

        // レイヤー追加
        if (ImGui::Button("+ Add Mesh Layer")) {
            tdc.layers.push_back(scene::DetailLayer{});
            tdc.needsBake = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("+ Add Billboard")) {
            scene::DetailLayer l{};
            l.type = scene::DetailLayerType::Billboard;
            tdc.layers.push_back(std::move(l));
            tdc.needsBake = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("+ Add Grass")) {
            scene::DetailLayer l{};
            l.type = scene::DetailLayerType::Grass;
            tdc.layers.push_back(std::move(l));
            tdc.needsBake = true;
        }

        // レイヤーリスト
        static const char* kTypeNames[] = { "Mesh", "Billboard", "Grass" };
        int deleteIdx = -1;

        for (int i = 0; i < static_cast<int>(tdc.layers.size()); ++i) {
            auto& layer = tdc.layers[static_cast<size_t>(i)];
            ImGui::PushID(i);

            const char* typeName = kTypeNames[static_cast<int>(layer.type)];
            const bool open = ImGui::TreeNodeEx("##layer", ImGuiTreeNodeFlags_DefaultOpen,
                "[%d] %s", i, typeName);
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) deleteIdx = i;

            if (open) {
                // タイプ
                int typeInt = static_cast<int>(layer.type);
                if (ImGui::Combo("Type", &typeInt, kTypeNames, 3)) {
                    layer.type = static_cast<scene::DetailLayerType>(typeInt);
                    tdc.needsBake = true;
                }

                // アセット参照
                ImGui::SeparatorText("Assets");
                if (layer.type != scene::DetailLayerType::Billboard) {
                    char buf[256];
                    std::snprintf(buf, sizeof(buf), "%s", layer.meshPath.c_str());
                    if (ImGui::InputText("Mesh Path", buf, sizeof(buf))) {
                        layer.meshPath = buf;
                        tdc.needsBake  = true;
                    }
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                            layer.meshPath = static_cast<const char*>(p->Data);
                            tdc.needsBake  = true;
                        }
                        ImGui::EndDragDropTarget();
                    }
                }
                {
                    char buf[256];
                    std::snprintf(buf, sizeof(buf), "%s", layer.texturePath.c_str());
                    if (ImGui::InputText("Texture Path", buf, sizeof(buf)))
                        layer.texturePath = buf;
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH"))
                            layer.texturePath = static_cast<const char*>(p->Data);
                        ImGui::EndDragDropTarget();
                    }
                }
                {
                    char buf[256];
                    std::snprintf(buf, sizeof(buf), "%s", layer.densityMapPath.c_str());
                    if (ImGui::InputText("Density Map", buf, sizeof(buf))) {
                        layer.densityMapPath = buf;
                        tdc.needsBake        = true;
                    }
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                            layer.densityMapPath = static_cast<const char*>(p->Data);
                            tdc.needsBake        = true;
                        }
                        ImGui::EndDragDropTarget();
                    }
                }

                // 配置パラメータ
                ImGui::SeparatorText("Placement");
                if (ImGui::DragFloat("Density",       &layer.density,      0.05f, 0.0f, 10.0f))
                    tdc.needsBake = true;
                if (ImGui::DragFloat("Min Scale",     &layer.minScale,     0.01f, 0.0f,  5.0f))
                    tdc.needsBake = true;
                if (ImGui::DragFloat("Max Scale",     &layer.maxScale,     0.01f, 0.0f,  5.0f))
                    tdc.needsBake = true;
                if (ImGui::Checkbox("Random Y Rotation", &layer.randomYRotation))
                    tdc.needsBake = true;

                // 描画距離
                ImGui::SeparatorText("Draw Distance");
                ImGui::DragFloat("Draw Distance",  &layer.drawDistance,  1.0f, 0.0f, 500.0f);
                ImGui::DragFloat("Fade Start",     &layer.fadeStartDist, 1.0f, 0.0f, 500.0f);

                // Grass 専用
                if (layer.type == scene::DetailLayerType::Grass) {
                    ImGui::SeparatorText("Grass (Phase 4)");
                    if (ImGui::DragFloat("Blade Height",   &layer.bladeHeight,   0.01f, 0.0f, 5.0f))
                        tdc.needsBake = true;
                    if (ImGui::DragFloat("Blade Width",    &layer.bladeWidth,    0.001f,0.0f, 1.0f))
                        tdc.needsBake = true;
                    if (ImGui::DragInt  ("Blade Segments", &layer.bladeSegments, 1.0f,  1,   16))
                        tdc.needsBake = true;
                    ImGui::DragFloat("Wind Strength",  &layer.windStrength,  0.01f, 0.0f, 10.0f);
                    ImGui::DragFloat("Wind Frequency", &layer.windFrequency, 0.01f, 0.0f, 10.0f);
                }

                // インスタンス数の表示
                if (!tdc.chunks.empty()) {
                    size_t total = 0;
                    for (const auto& chunk : tdc.chunks) {
                        if (static_cast<size_t>(i) < chunk.instancesPerLayer.size())
                            total += chunk.instancesPerLayer[static_cast<size_t>(i)].size();
                    }
                    ImGui::TextDisabled("Instances: %zu", total);
                }

                ImGui::TreePop();
            }
            ImGui::PopID();
        }

        // 削除処理
        if (deleteIdx >= 0) {
            tdc.layers.erase(tdc.layers.begin() + deleteIdx);
            tdc.needsBake = true;
        }
    });

    DrawComponentSection<scene::FoliageComponent>(
        go, ctx, m_componentClipboard, m_componentClipboardType, "Foliage",
        [](scene::FoliageComponent& foliage, EditorContext&) {
            if (ImGui::Button("Bake Foliage")) {
                foliage.caches.clear();
                foliage.needsBake = foliage.needsBakeChildren = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%zu species", foliage.species.size());

            if (ImGui::Button("+ Add Species")) {
                foliage.species.emplace_back();
                foliage.needsBake = foliage.needsBakeChildren = true;
            }

            int deleteSpecies = -1;
            for (int speciesIndex = 0;
                 speciesIndex < static_cast<int>(foliage.species.size());
                 ++speciesIndex) {
                auto& species = foliage.species[static_cast<size_t>(speciesIndex)];
                ImGui::PushID(speciesIndex);

                const bool open = ImGui::TreeNodeEx(
                    "##FoliageSpecies", ImGuiTreeNodeFlags_DefaultOpen,
                    "[%d] %s", speciesIndex,
                    species.modelPath.empty() ? "(No Model)" : species.modelPath.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("X"))
                    deleteSpecies = speciesIndex;

                if (open) {
                    char modelPath[512];
                    std::snprintf(modelPath, sizeof(modelPath), "%s", species.modelPath.c_str());
                    if (ImGui::InputText("Model Path", modelPath, sizeof(modelPath))) {
                        species.modelPath = NormalizeAssetPath(modelPath);
                        foliage.needsBake = foliage.needsBakeChildren = true;
                    }
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload =
                                ImGui::AcceptDragDropPayload("ASSET_PATH")) {
                            species.modelPath = NormalizeAssetPath(
                                static_cast<const char*>(payload->Data));
                            foliage.needsBake = foliage.needsBakeChildren = true;
                        }
                        ImGui::EndDragDropTarget();
                    }

                    int placementMode =
                        species.placementMode == scene::FoliagePlacementMode::STAMP ? 1 : 0;
                    if (ImGui::Combo(
                            "Placement", &placementMode, "Procedural\0Stamp\0")) {
                        species.placementMode = placementMode == 1
                            ? scene::FoliagePlacementMode::STAMP
                            : scene::FoliagePlacementMode::PROCEDURAL;
                        foliage.needsBake = foliage.needsBakeChildren = true;
                    }
                    if (species.placementMode == scene::FoliagePlacementMode::STAMP) {
                        ImGui::TextDisabled("Stamped Instances: %zu", species.stamps.size());
                        if (ImGui::Button("Clear Stamps") && !species.stamps.empty()) {
                            species.stamps.clear();
                            foliage.needsBake = foliage.needsBakeChildren = true;
                        }
                    }

                    if (ImGui::DragFloat("Density / 100m2",
                                         &species.densityPer100SquareMeters,
                                         0.05f, 0.0f, 100.0f)) {
                        foliage.needsBake = true;
                    }
                    if (ImGui::DragFloat("Min Scale", &species.minScale,
                                         0.01f, 0.01f, 20.0f))
                        foliage.needsBake = foliage.needsBakeChildren = true;
                    if (ImGui::DragFloat("Max Scale", &species.maxScale,
                                         0.01f, 0.01f, 20.0f))
                        foliage.needsBake = foliage.needsBakeChildren = true;
                    ImGui::DragFloat("Draw Distance", &species.drawDistance,
                                     1.0f, 1.0f, 2000.0f);
                    int seed = static_cast<int>(species.seed);
                    if (ImGui::DragInt("Seed", &seed, 1.0f, 0)) {
                        species.seed = static_cast<uint32_t>(std::max(seed, 0));
                        foliage.needsBake = true;
                    }
                    if (ImGui::Checkbox("Random Y Rotation",
                                        &species.randomYRotation))
                        foliage.needsBake = true;

                    ImGui::SeparatorText("SubMesh Materials");
                    ImGui::TextDisabled("Index must match Model::meshes index.");
                    if (ImGui::SmallButton("+ Material Slot"))
                        species.subMeshMaterialPaths.emplace_back();
                    ImGui::SameLine();
                    if (ImGui::SmallButton("- Material Slot")
                        && !species.subMeshMaterialPaths.empty())
                        species.subMeshMaterialPaths.pop_back();

                    for (int materialIndex = 0;
                         materialIndex < static_cast<int>(
                             species.subMeshMaterialPaths.size());
                         ++materialIndex) {
                        ImGui::PushID(materialIndex);
                        auto& path =
                            species.subMeshMaterialPaths[static_cast<size_t>(materialIndex)];
                        char materialPath[512];
                        std::snprintf(materialPath, sizeof(materialPath), "%s", path.c_str());
                        char label[32];
                        std::snprintf(label, sizeof(label), "Material %d", materialIndex);
                        if (ImGui::InputText(label, materialPath, sizeof(materialPath)))
                            path = NormalizeAssetPath(materialPath);
                        if (ImGui::BeginDragDropTarget()) {
                            if (const ImGuiPayload* payload =
                                    ImGui::AcceptDragDropPayload("ASSET_PATH"))
                                path = NormalizeAssetPath(
                                    static_cast<const char*>(payload->Data));
                            ImGui::EndDragDropTarget();
                        }
                        ImGui::PopID();
                    }

                    if (static_cast<size_t>(speciesIndex) < foliage.caches.size())
                        ImGui::TextDisabled("Instances: %zu",
                            foliage.caches[static_cast<size_t>(speciesIndex)].instances.size());
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }

            if (deleteSpecies >= 0) {
                foliage.species.erase(foliage.species.begin() + deleteSpecies);
                foliage.caches.clear();
                foliage.needsBake = true;
            }
        });

}

} // namespace fbzz::editor
