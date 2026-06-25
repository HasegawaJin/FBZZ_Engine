// FBZZ Engine
// InspectorTerrainWater.cpp | fbzz::editor
// Terrain / Water 系 Component の Inspector 描画
#include "InspectorTerrainWater.hpp"
#include <Engine/Scene/Components/TerrainDetailComponent.hpp>
#include <Engine/Scene/Components/FoliageComponent.hpp>
#include <Engine/Scene/Components/TerrainGridComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>

namespace fbzz::editor {

void DrawTerrainWaterInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType)
{
    DrawComponentSection<scene::TerrainComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Terrain",
        [go](scene::TerrainComponent& tc, EditorContext& ctx) {
            // ── 外部 Terrain Asset ─────────────────────────────────────────────
            ImGui::SeparatorText("Asset");
            {
                widgets::AssetPathField("Asset Path", tc.terrainAssetPath, ".terrain", ctx.projectRoot);

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
            // Columns / Rows はドラッグ中に毎フレーム heightDirty を立てると
            // チャンク全再構築が連続発生して FPS スパイクになる。
            // さらに heightData のサイズが columns*rows と一致しなくなり assert が火を吹く。
            // そのため IsItemDeactivatedAfterEdit でドラッグ終了時のみ Resize() を呼ぶ。
            static int s_pendingCols = -1;
            static int s_pendingRows = -1;
            {
                int cols = (s_pendingCols >= 2) ? s_pendingCols : tc.columns;
                ImGui::DragInt("Columns", &cols, 1.0f, 2, 4097);
                if (ImGui::IsItemActive())                s_pendingCols = cols;
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    if (s_pendingCols >= 2 && s_pendingCols != tc.columns)
                        tc.Resize(s_pendingCols, tc.rows);
                    tc.heightDirty = tc.splatDirty = tc.colliderDirty = true;
                    s_pendingCols = -1;
                }
            }
            {
                int rows = (s_pendingRows >= 2) ? s_pendingRows : tc.rows;
                ImGui::DragInt("Rows", &rows, 1.0f, 2, 4097);
                if (ImGui::IsItemActive())                s_pendingRows = rows;
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    if (s_pendingRows >= 2 && s_pendingRows != tc.rows)
                        tc.Resize(tc.columns, s_pendingRows);
                    tc.heightDirty = tc.splatDirty = tc.colliderDirty = true;
                    s_pendingRows = -1;
                }
            }
            // CellSize / MaxHeight はサイズ変化なし。ドラッグ終了時のみ再構築して FPS スパイクを防ぐ。
            ImGui::DragFloat("Cell Size",  &tc.cellSize,  0.01f, 0.01f, 100.0f);
            if (ImGui::IsItemDeactivatedAfterEdit()) tc.heightDirty = true;
            ImGui::DragFloat("Max Height", &tc.maxHeight, 0.5f,  0.5f,  1000.0f);
            if (ImGui::IsItemDeactivatedAfterEdit()) tc.heightDirty = true;
            ImGui::DragInt("Chunk Size", &tc.chunkSize, 1.0f, 8, 256);

            // ── Layer Materials (.mat × 4) ──────────────────────────────────
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
                        if (util::StringUtils::EndsWith(dropped, ".mat")) {
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
                        if (ImGui::Button("Save .mat")) {
                            (void)asset::SaveMaterialAssetToFile(
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

            // ── ハイトマップ初期化 ────────────────────────────────────────────────
            ImGui::SeparatorText("Heightmap");
            if (tc.heightData.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.1f, 1.0f), "Not initialized");
                ImGui::SameLine();
            }
            if (ImGui::Button("Initialize Flat")) {
                tc.InitFlat(0.0f);
                tc.heightDirty  = true;
                tc.splatDirty   = true;
                tc.colliderDirty = true;
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
        [go](scene::WaterComponent& water, EditorContext& ctx) {
            // ── Material (.mat) ─────────────────────────────────────────────
            ImGui::SeparatorText("Material (.mat)");
            // 変更時はテクスチャ・フォームキャッシュを無効化してレンダーパスに再ロードさせる。
            if (widgets::AssetPathField("Material (.mat)", water.materialPath, ".mat", ctx.projectRoot)) {
                water.texDirty = true;
                water.foamDirty = true;
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
    [go](scene::TerrainDetailComponent& tdc, EditorContext& ctx) {

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
                    if (widgets::AssetPathField("Mesh Path", layer.meshPath, ".fbx,.fzmodel", ctx.projectRoot))
                        tdc.needsBake = true;
                }
                widgets::AssetPathField("Texture Path", layer.texturePath, ".fztex,.png,.dds", ctx.projectRoot);
                if (widgets::AssetPathField("Density Map", layer.densityMapPath, ".png,.fztex", ctx.projectRoot))
                    tdc.needsBake = true;

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
        [](scene::FoliageComponent& foliage, EditorContext& ctx) {
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
                    if (widgets::AssetPathField("Model Path", species.modelPath, ".fbx,.fzmodel", ctx.projectRoot))
                        foliage.needsBake = foliage.needsBakeChildren = true;

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
                        char label[32];
                        std::snprintf(label, sizeof(label), "Material %d", materialIndex);
                        widgets::AssetPathField(label, path, ".mat", ctx.projectRoot);
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

    // TerrainGridComponent — グリッド全体の管理設定
    DrawComponentSection<scene::TerrainGridComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Terrain Grid",
        [](scene::TerrainGridComponent& tgc, EditorContext& ctx) {
            // グリッドサイズは MapEditorPanel のグリッドビューで変更する
            ImGui::SeparatorText("Grid");
            ImGui::Text("Grid Size: %d cols x %d rows  (%d cells total)",
                tgc.cellCountX, tgc.cellCountZ, tgc.cellCountX * tgc.cellCountZ);
            ImGui::TextDisabled("Edit grid dimensions in the Map Editor panel.");

            // 新規セル・一括適用のデフォルト設定
            // WHY: セル個別に同じ設定を繰り返すのを避け、グリッド単位で一貫した地形サイズを保つ。
            //      新規セル追加時と "Apply to All Cells" の両方がここを参照する。
            ImGui::SeparatorText("Default Cell Settings");
            ImGui::TextDisabled("Applied when adding new cells and for batch operations.");

            // Columns / Rows はドラッグ終了時のみ更新
            // WHY: ドラッグ中毎フレーム更新すると Apply 時に意図しない中間値が残る可能性がある
            static int s_pendingCols = -1;
            static int s_pendingRows = -1;
            {
                int cols = (s_pendingCols >= 2) ? s_pendingCols : tgc.defaultColumns;
                ImGui::DragInt("Default Columns", &cols, 1.0f, 2, 4097);
                if (ImGui::IsItemActive())               s_pendingCols = cols;
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    tgc.defaultColumns = std::max(2, s_pendingCols);
                    s_pendingCols = -1;
                }
            }
            {
                int rows = (s_pendingRows >= 2) ? s_pendingRows : tgc.defaultRows;
                ImGui::DragInt("Default Rows",    &rows, 1.0f, 2, 4097);
                if (ImGui::IsItemActive())               s_pendingRows = rows;
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    tgc.defaultRows = std::max(2, s_pendingRows);
                    s_pendingRows = -1;
                }
            }
            ImGui::DragFloat("Default Cell Size",  &tgc.defaultCellSize,  0.01f, 0.01f, 100.0f);
            ImGui::DragInt  ("Default Chunk Size", &tgc.defaultChunkSize, 1.0f,  8,     256);
            ImGui::TextDisabled("World size per cell: %.1f m",
                static_cast<float>(tgc.defaultColumns - 1) * tgc.defaultCellSize);

            // 既存セルへの一括適用
            ImGui::Spacing();
            ImGui::SeparatorText("Batch Operations");
            if (ImGui::Button("Apply Defaults to All Cells")) {
                if (ctx.activeScene) {
                    for (const scene::EntityID id : tgc.cells) {
                        if (!ctx.activeScene->IsValid(id)) continue;
                        auto* tc = ctx.activeScene->GetComponent<scene::TerrainComponent>(id);
                        if (!tc) continue;
                        if (tc->columns != tgc.defaultColumns || tc->rows != tgc.defaultRows)
                            tc->Resize(tgc.defaultColumns, tgc.defaultRows);
                        tc->cellSize   = tgc.defaultCellSize;
                        tc->chunkSize  = tgc.defaultChunkSize;
                        tc->heightDirty   = true;
                        tc->colliderDirty = true;
                    }
                    ctx.markSceneDirty();
                }
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip(
                    "Resize all existing cells to %d x %d\n"
                    "CellSize = %.2f, ChunkSize = %d\n"
                    "WARNING: height data on resized cells will be discarded.",
                    tgc.defaultColumns, tgc.defaultRows,
                    tgc.defaultCellSize, tgc.defaultChunkSize);
        });

}

} // namespace fbzz::editor
