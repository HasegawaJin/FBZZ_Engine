/// @file    InspectorTerrainWater.cpp
/// @brief   Terrain / Water 系 Component の Inspector 描画。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#include "InspectorTerrainWater.hpp"
#include <Engine/Scene/Components/TerrainGridComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <algorithm>
#include <string>

namespace fbzz::editor {

void DrawTerrainWaterInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType)
{
    DrawComponentSection<scene::TerrainComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Terrain",
        [go](scene::TerrainComponent& tc, EditorContext& ctx) {
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

            ImGui::SeparatorText("Grid");
            /// @note ドラッグ終了時だけ Resize() し、毎フレームのチャンク再構築と heightData のサイズ不整合を防ぐ。
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
            /// @note CellSize / MaxHeight はサイズ変化なし。ドラッグ終了時のみ再構築して FPS スパイクを防ぐ。
            ImGui::DragFloat("Cell Size",  &tc.cellSize,  0.01f, 0.01f, 100.0f);
            if (ImGui::IsItemDeactivatedAfterEdit()) tc.heightDirty = true;
            ImGui::DragFloat("Max Height", &tc.maxHeight, 0.5f,  0.5f,  1000.0f);
            if (ImGui::IsItemDeactivatedAfterEdit()) tc.heightDirty = true;
            ImGui::DragInt("Chunk Size", &tc.chunkSize, 1.0f, 8, 256);

            /// @note スプラットの番号付け替えと dirty 要求は TerrainComponent、Undo は前後スナップショットが持つ。
            /// @see Docs/design/terrain-layers.md
            ImGui::SeparatorText("Layer Materials");
            if (ImGui::SliderFloat("Height Blend Depth", &tc.heightBlendDepth, 0.01f, 1.0f, "%.2f"))
                tc.RequestMaterialRebuild();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("高さブレンドで高い層が境界を押し広げる幅。\n"
                                  "各層の .mat の Height Blend が 0 のときは効きません。");
            int pendingMoveFrom = -1;
            int pendingMoveTo   = -1;
            int pendingRemove   = -1;
            const int layerCount = tc.LayerCount();
            for (int li = 0; li < layerCount; ++li) {
                ImGui::PushID(li);
                const std::string layerLabel = "Layer " + std::to_string(li);
                if (widgets::AssetPathField(layerLabel.c_str(), tc.layerMaterials[li], ".mat", ctx.projectRoot))
                    tc.RequestSplatRebuild();

                ImGui::BeginDisabled(li == 0);
                if (ImGui::SmallButton("Up"))   { pendingMoveFrom = li; pendingMoveTo = li - 1; }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(li + 1 >= layerCount);
                if (ImGui::SmallButton("Down")) { pendingMoveFrom = li; pendingMoveTo = li + 1; }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(layerCount <= 1);
                if (ImGui::SmallButton("Remove")) pendingRemove = li;
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("層を削除します。この層の塗りは残りの層で分け直されます。");

                if (!tc.layerMaterials[li].empty()) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Clear")) {
                        tc.layerMaterials[li].clear();
                        tc.splatDirty = true;
                    }
                    auto matHandle = asset::AssetManager::Load<asset::MaterialAsset>(tc.layerMaterials[li]);
                    if (auto* mat = asset::AssetManager::Get<asset::MaterialAsset>(matHandle)) {
                        {
                            const auto f = DrawTerrainLayerMaterialInspector(*mat);
                            if (f.textureDirty) tc.splatDirty         = true;
                            if (f.paramDirty)   tc.materialParamDirty = true;
                            /// @note レイヤー .mat はシーン保存時に書き出すため、シーンを dirty にしない編集は保存されない。
                            if ((f.textureDirty || f.paramDirty) && ctx.markSceneDirty)
                                ctx.markSceneDirty();
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
                } else if (const char* defaultPath = DefaultTerrainLayerMaterialPath(li); defaultPath[0] != '\0') {
                    if (ImGui::Button("Use Default"))
                        tc.SetLayerMaterial(li, defaultPath);
                }
                ImGui::PopID();
                ImGui::Spacing();
            }
            /// @note ループ中に並びを変えると添字と PushID がずれるので、押された操作は描画後に 1 つだけ当てる。
            if (pendingRemove >= 0)
                tc.RemoveLayer(pendingRemove);
            else if (pendingMoveFrom >= 0)
                tc.MoveLayer(pendingMoveFrom, pendingMoveTo);

            ImGui::BeginDisabled(tc.LayerCount() >= scene::TERRAIN_MAX_LAYERS);
            if (ImGui::Button("Add Layer"))
                tc.AddLayer(DefaultTerrainLayerMaterialPath(tc.LayerCount()));
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("%d layers", tc.LayerCount());

            ImGui::SeparatorText("Holes");
            const size_t holeCount = tc.CountHoles();
            ImGui::Text("Hole Cells: %zu", holeCount);
            ImGui::SameLine();
            ImGui::BeginDisabled(holeCount == 0);
            if (ImGui::Button("Clear Holes")) {
                tc.holeData.clear();
                tc.RequestHoleRebuild();
            }
            ImGui::EndDisabled();

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

            ImGui::SeparatorText("Collider");
            if (ImGui::Button("Rebuild Collider Now")) {
                tc.colliderDirty = true;
            }

            ImGui::SeparatorText("Info");
            ImGui::Text("Vertices : %d", tc.columns * tc.rows);
            ImGui::Text("Triangles: %d", (tc.columns - 1) * (tc.rows - 1) * 2);
            ImGui::TextColored(tc.heightDirty   ? ImVec4{1,0.5f,0.2f,1} : ImVec4{0.5f,1,0.5f,1},
                               "Height Dirty : %s", tc.heightDirty   ? "Yes" : "No");
            ImGui::TextColored(tc.colliderDirty ? ImVec4{1,0.5f,0.2f,1} : ImVec4{0.5f,1,0.5f,1},
                               "Collider Dirty: %s", tc.colliderDirty ? "Yes" : "No");
        });

    DrawComponentSection<scene::WaterComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Water",
        [](scene::WaterComponent& water, EditorContext& ctx) {
            /// @note 色・波・環境風への反応・水流の正本は .mat にあり、その Inspector で編集する。
            ImGui::SeparatorText("Water Type (.mat)");
            {
                const std::string current = NormalizeAssetPath(water.materialPath);
                const auto presets = WaterMaterialPresets();
                const float spacing = ImGui::GetStyle().ItemSpacing.x;
                const float count = static_cast<float>(presets.size());
                const float buttonW = (ImGui::GetContentRegionAvail().x - spacing * (count - 1.0f)) / count;
                for (size_t i = 0; i < presets.size(); ++i) {
                    const WaterMaterialPreset& preset = presets[i];
                    if (i > 0) ImGui::SameLine();
                    const bool active = current == preset.path;
                    if (active)
                        ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(ThemeColor::Secondary));
                    if (ImGui::Button(preset.label, { buttonW, 0.0f }) && !active) {
                        water.materialPath = preset.path;
                        water.texDirty = true;
                        water.foamDirty = true;
                    }
                    if (active)
                        ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                        ImGui::SetTooltip("%s\n%s", preset.tooltip, preset.path);
                }
            }
            /// @note 変更時はテクスチャ・フォームキャッシュを無効化してレンダーパスに再ロードさせる。
            if (widgets::AssetPathField("Material (.mat)", water.materialPath, ".mat", ctx.projectRoot)) {
                water.texDirty = true;
                water.foamDirty = true;
            }
            if (water.materialPath.empty())
                ImGui::TextDisabled("(no material — built-in ocean waves, default look)");

            ImGui::Spacing();
            ImGui::SeparatorText("Geometry");
            ImGui::Checkbox("Camera Focused Grid", &water.cameraFocusedGrid);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("水面の範囲と頂点数を保ち、カメラ付近へ頂点を集中させます。\n"
                                  "近景のうねりを細かく描き、遠景は粗い格子で描きます。");
            ImGui::BeginDisabled(!water.cameraFocusedGrid);
            if (ImGui::DragFloat("Near Cell Size", &water.nearCellSize, 0.05f, 0.25f, 10.0f, "%.2f m"))
                water.nearCellSize = std::clamp(water.nearCellSize, 0.25f, 10.0f);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("カメラ付近の頂点間隔 [m]。小さいほど近景のうねりを細かく描きます。\n"
                                  "細かい範囲の外では、水面の端へ向けて間隔が滑らかに広がります。");
            ImGui::EndDisabled();
            int resX = static_cast<int>(water.resolutionX);
            int resZ = static_cast<int>(water.resolutionZ);
            if (ImGui::DragInt("Resolution X", &resX, 1.0f, 1, 512)) {
                resX = std::clamp(resX, 1, 512);
                water.resolutionX = static_cast<uint32_t>(resX);
                water.meshDirty = true;
                water.foamDirty = true;
            }
            if (ImGui::DragInt("Resolution Z", &resZ, 1.0f, 1, 512)) {
                resZ = std::clamp(resZ, 1, 512);
                water.resolutionZ = static_cast<uint32_t>(resZ);
                water.meshDirty = true;
                water.foamDirty = true;
            }
            if (ImGui::DragFloat("Extent X", &water.extentX, 0.5f, 0.1f, 10000.0f)) {
                water.extentX = std::clamp(water.extentX, 0.1f, 10000.0f);
                water.meshDirty = true;
                water.foamDirty = true;
            }
            if (ImGui::DragFloat("Extent Z", &water.extentZ, 0.5f, 0.1f, 10000.0f)) {
                water.extentZ = std::clamp(water.extentZ, 0.1f, 10000.0f);
                water.meshDirty = true;
                water.foamDirty = true;
            }
            {
                int chunks = static_cast<int>(water.chunkCount);
                if (ImGui::DragInt("Chunk Count", &chunks, 1.0f, 1, 64)) {
                    chunks = std::clamp(chunks, 1, 64);
                    water.chunkCount = static_cast<uint32_t>(chunks);
                    water.meshDirty = true;
                }
                ImGui::TextDisabled("(%d x %d chunks = %d draw calls)", chunks, chunks, chunks * chunks);
            }

            ImGui::SeparatorText("Waves");
            ImGui::Checkbox("Enable Waves", &water.enableGerstnerWaves);
            ImGui::BeginDisabled(!water.enableGerstnerWaves);
            if (ImGui::DragFloat("Amplitude Scale", &water.waveAmplitudeScale, 0.01f, 0.0f, 10.0f, "x%.2f"))
                water.waveAmplitudeScale = (std::max)(water.waveAmplitudeScale, 0.0f);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip(".mat の波の高さに掛ける倍率。\n"
                                  "同じ Ocean.mat を «凪の入り江» と «外洋» に使い分けるときに使います。");
            ImGui::EndDisabled();
            /// @note 実際に描かれている波 (.mat × 倍率 × 環境風)。風の効き具合をここで確かめる。
            for (size_t i = 0; i < water.waves.size(); ++i) {
                const scene::GerstnerWave& wave = water.waves[i];
                ImGui::TextDisabled("Wave %zu   A %.2f m   L %.1f m   Q %.2f",
                                    i, wave.amplitude, wave.wavelength, wave.steepness);
            }
            if (const float currentSpeed = water.current.Length(); currentSpeed > 1.0e-3f)
                ImGui::TextDisabled("Current  %.2f m/s", currentSpeed);

            ImGui::SeparatorText("Physics");
            ImGui::Checkbox("Buoyancy", &water.buoyancyEnabled);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("水面の範囲に入った RigidBody を浮かせます。\n"
                                  "トリガーコライダーや Volume を別に付ける必要はありません。");
            ImGui::BeginDisabled(!water.buoyancyEnabled);
            ImGui::DragFloat("Lift", &water.buoyancy, 0.1f, 0.0f, 200.0f, "%.1f m/s^2");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("完全に沈んだときの上向き加速度。重力 (9.8) を超えると浮きます。");
            ImGui::DragFloat("Drag", &water.waterDrag, 0.01f, 0.0f, 50.0f, "%.2f /s");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("水中での速度減衰。\n川では水流との速度差に掛かり、物体を流れに乗せます。");
            if (ImGui::DragFloat("Depth", &water.buoyancyDepth, 0.1f, 0.1f, 1000.0f, "%.1f m"))
                water.buoyancyDepth = (std::max)(water.buoyancyDepth, 0.1f);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("水面からこの深さまで浮力が届きます。");
            ImGui::EndDisabled();
            ImGui::Checkbox("Splash & Ripples", &water.splashEnabled);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("RigidBody が水面を通過したときに波紋としぶきを出し、\n"
                                  "水面をまたいで進む物体には航跡を引かせます。");
        });

    DrawComponentSection<scene::TerrainGridComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Terrain Grid",
        [](scene::TerrainGridComponent& tgc, EditorContext& ctx) {
            ImGui::SeparatorText("Grid");
            ImGui::Text("Grid Size: %d cols x %d rows  (%d cells total)",
                tgc.cellCountX, tgc.cellCountZ, tgc.cellCountX * tgc.cellCountZ);
            ImGui::TextDisabled("Edit grid dimensions in the Map Editor panel.");

            /// @note 新規セル追加と一括適用は同じ既定値を参照し、グリッド全体の地形サイズを揃える。
            ImGui::SeparatorText("Default Cell Settings");
            ImGui::TextDisabled("Applied when adding new cells and for batch operations.");

            /// @note ドラッグ終了時のみ更新し、一括適用に意図しない中間値が残るのを防ぐ。
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

}
