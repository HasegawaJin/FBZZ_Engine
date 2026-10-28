/// @file    InspectorTerrainWater.cpp
/// @brief   Terrain / Water 系 Component の Inspector 描画。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#include "InspectorTerrainWater.hpp"
#include "../../Util/MaterialAuthoringEditing.hpp"
#include <Engine/Scene/Components/TerrainGridComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <algorithm>
#include <string>

namespace fbzz::editor {

namespace {

/// @note Terrain の配列は最初の変更直前だけ保存し、ドラッグ終了まで同じ Undo 操作にまとめる。
class TerrainInspectorEdit {
public:
    TerrainInspectorEdit(scene::GameObject& object, EditorContext& context, scene::TerrainComponent& terrain)
        : m_object(object), m_context(context), m_terrain(terrain)
    {
        auto& state = State();
        const uint64_t generation = context.activeScene ? context.activeScene->GetRenderSceneGeneration() : 0;
        if (!CanRecordEditorUndo(context) || state.scene != context.activeScene
            || state.sceneGeneration != generation || state.id != object.GetID()
            || state.undo != context.undoStack) state.before.reset();
        state.scene = context.activeScene;
        state.sceneGeneration = generation;
        state.id = object.GetID();
        state.undo = context.undoStack;
    }
    ~TerrainInspectorEdit()
    {
        auto& state = State();
        if (state.before && (state.activeId == 0 || state.activeId != ImGui::GetActiveID())) Commit();
    }
    template<typename ChangeFn>
    void Change(ChangeFn change)
    {
        auto& state = State();
        const ImGuiID activeId = ImGui::GetActiveID();
        if (state.before && state.activeId != activeId) Commit();
        if (!state.before && CanRecordEditorUndo(m_context)) {
            state.before = std::make_unique<scene::TerrainComponent>(m_terrain);
            state.activeId = activeId;
        }
        change();
    }
private:
    struct EditState {
        scene::Scene* scene = nullptr;
        uint64_t sceneGeneration = 0;
        scene::EntityID id;
        UndoStack* undo = nullptr;
        ImGuiID activeId = 0;
        std::unique_ptr<scene::TerrainComponent> before;
    };
    static EditState& State() { static EditState state; return state; }
    void Commit()
    {
        auto& state = State();
        if (!state.before) return;
        if (state.before->heightData != m_terrain.heightData
            || !ComponentUndoCompare<scene::TerrainComponent>::UserValuesEqual(*state.before, m_terrain)) {
            PushComponentValueCommand(m_object, m_context, "Change Terrain", *state.before, m_terrain);
            if (m_context.markSceneDirty) m_context.markSceneDirty();
        }
        state.before.reset();
    }
    scene::GameObject& m_object;
    EditorContext& m_context;
    scene::TerrainComponent& m_terrain;
};

} /// @note namespace

void DrawTerrainWaterInspectors(scene::GameObject* go, EditorContext& ctx, std::any& m_componentClipboard, const std::type_info*& m_componentClipboardType)
{
    DrawComponentSection<scene::TerrainComponent>(go, ctx, m_componentClipboard, m_componentClipboardType, "Terrain",
        [go](scene::TerrainComponent& tc, EditorContext& ctx) {
            TerrainInspectorEdit edit(*go, ctx, tc);
            ImGui::SeparatorText("Asset");
            {
                std::string assetPath = tc.terrainAssetPath;
                if (widgets::AssetPathField("Asset Path", assetPath, ".terrain", ctx.projectRoot))
                    edit.Change([&] { tc.terrainAssetPath = std::move(assetPath); });

                if (tc.terrainAssetPath.empty()) {
                    if (ImGui::Button("Create Terrain Asset")) {
                        const std::string path = UniqueTerrainAssetPath(ctx, go ? go->name : "Terrain");
                        if (scene::TerrainAssetSerializer::Save(tc, TerrainAssetDiskPath(ctx, path))) {
                            edit.Change([&] { tc.terrainAssetPath = path; });
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
                        edit.Change([&] {
                            if (scene::TerrainAssetSerializer::Load(TerrainAssetDiskPath(ctx, path), tc)) {
                                tc.enabled = enabled;
                                tc.terrainAssetPath = path;
                            }
                        });
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Unlink")) {
                        edit.Change([&] { tc.terrainAssetPath.clear(); });
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
                        edit.Change([&] { tc.Resize(s_pendingCols, tc.rows); });
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
                        edit.Change([&] { tc.Resize(tc.columns, s_pendingRows); });
                    tc.heightDirty = tc.splatDirty = tc.colliderDirty = true;
                    s_pendingRows = -1;
                }
            }
            /// @note CellSize / MaxHeight はサイズ変化なし。ドラッグ終了時のみ再構築して FPS スパイクを防ぐ。
            float cellSize = tc.cellSize;
            if (ImGui::DragFloat("Cell Size", &cellSize, 0.01f, 0.01f, 100.0f))
                edit.Change([&] { tc.cellSize = cellSize; });
            if (ImGui::IsItemDeactivatedAfterEdit()) tc.heightDirty = true;
            float maxHeight = tc.maxHeight;
            if (ImGui::DragFloat("Max Height", &maxHeight, 0.5f, 0.5f, 1000.0f))
                edit.Change([&] { tc.maxHeight = maxHeight; });
            if (ImGui::IsItemDeactivatedAfterEdit()) tc.heightDirty = true;
            int chunkSize = tc.chunkSize;
            if (ImGui::DragInt("Chunk Size", &chunkSize, 1.0f, 8, 256))
                edit.Change([&] { tc.chunkSize = chunkSize; });

            /// @note スプラットの番号付け替えと dirty 要求は TerrainComponent、Undo は前後スナップショットが持つ。
            /// @see Docs/design/terrain-layers.md
            ImGui::SeparatorText("Layer Materials");
            float heightBlendDepth = tc.heightBlendDepth;
            if (ImGui::SliderFloat("Height Blend Depth", &heightBlendDepth, 0.01f, 1.0f, "%.2f"))
                edit.Change([&] { tc.heightBlendDepth = heightBlendDepth; tc.RequestMaterialRebuild(); });
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
                std::string materialPath = tc.layerMaterials[li];
                if (widgets::AssetPathField(layerLabel.c_str(), materialPath, ".mat", ctx.projectRoot))
                    edit.Change([&] { tc.layerMaterials[li] = std::move(materialPath); tc.RequestSplatRebuild(); });

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
                        edit.Change([&] { tc.layerMaterials[li].clear(); tc.splatDirty = true; });
                    }
                    const std::string capturedMaterialPath = tc.layerMaterials[li];
                    scene::Scene* capturedScene = ctx.activeScene;
                    const uint64_t capturedGeneration = capturedScene ? capturedScene->GetRenderSceneGeneration() : 0;
                    const std::string capturedOwner = go->instanceId;
                    const auto resolveTerrain = [&]() -> scene::TerrainComponent* {
                        if (!capturedScene || ctx.activeScene != capturedScene
                            || capturedScene->GetRenderSceneGeneration() != capturedGeneration) return nullptr;
                        auto* owner = capturedScene->FindByGuid(capturedOwner);
                        return owner ? owner->GetComponent<scene::TerrainComponent>() : nullptr;
                    };
                    const auto matHandle = asset::AssetManager::Load<asset::MaterialAsset>(capturedMaterialPath);
                    if (auto materialEdit = material_edit_detail::BeginAssetEdit(matHandle)) {
                        const auto beforeTextures = materialEdit->Draft().textures;
                        const auto flags = DrawTerrainLayerMaterialInspector(materialEdit->Draft());
                        const bool textureChanged = beforeTextures != materialEdit->Draft().textures;
                        const auto result = material_edit_detail::FinishAssetEdit(matHandle, *materialEdit);
                        if (result == material_edit_detail::CommitResult::COMMITTED) {
                            if (auto* liveTerrain = resolveTerrain()) {
                                if (flags.textureDirty || textureChanged) liveTerrain->splatDirty = true;
                                liveTerrain->materialParamDirty = true;
                            }
                            if (ctx.markSceneDirty) ctx.markSceneDirty();
                        }
                        ImGui::BeginDisabled(result == material_edit_detail::CommitResult::CONFLICT);
                        if (ImGui::Button("Save .mat")) {
                            if (material_edit_detail::SaveAssetSnapshot(MaterialAssetDiskPath(ctx, capturedMaterialPath), matHandle))
                                ctx.requestAssetBrowserRefresh = true;
                        }
                        ImGui::EndDisabled();
                    } else {
                        ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.25f, 1.0f),
                                           "Missing: %s", capturedMaterialPath.c_str());
                    }
                } else if (const char* defaultPath = DefaultTerrainLayerMaterialPath(li); defaultPath[0] != '\0') {
                    if (ImGui::Button("Use Default"))
                        edit.Change([&] { tc.SetLayerMaterial(li, defaultPath); });
                }
                ImGui::PopID();
                ImGui::Spacing();
            }
            /// @note ループ中に並びを変えると添字と PushID がずれるので、押された操作は描画後に 1 つだけ当てる。
            if (pendingRemove >= 0)
                edit.Change([&] { tc.RemoveLayer(pendingRemove); });
            else if (pendingMoveFrom >= 0)
                edit.Change([&] { tc.MoveLayer(pendingMoveFrom, pendingMoveTo); });

            ImGui::BeginDisabled(tc.LayerCount() >= scene::TERRAIN_MAX_LAYERS);
            if (ImGui::Button("Add Layer"))
                edit.Change([&] { tc.AddLayer(DefaultTerrainLayerMaterialPath(tc.LayerCount())); });
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("%d layers", tc.LayerCount());

            ImGui::SeparatorText("Holes");
            const size_t holeCount = tc.CountHoles();
            ImGui::Text("Hole Cells: %zu", holeCount);
            ImGui::SameLine();
            ImGui::BeginDisabled(holeCount == 0);
            if (ImGui::Button("Clear Holes")) {
                edit.Change([&] { tc.holeData.clear(); tc.RequestHoleRebuild(); });
            }
            ImGui::EndDisabled();

            ImGui::SeparatorText("Heightmap");
            if (tc.heightData.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.1f, 1.0f), "Not initialized");
                ImGui::SameLine();
            }
            if (ImGui::Button("Initialize Flat")) {
                edit.Change([&] {
                    tc.InitFlat(0.0f);
                    tc.heightDirty = tc.splatDirty = tc.colliderDirty = true;
                });
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
        [go](scene::WaterComponent& water, EditorContext& ctx) {
            if (water.zoneOwner.IsValid()) {
                const auto* owner = go && go->GetScene() ? go->GetScene()->GetGameObject(water.zoneOwner) : nullptr;
                ImGui::TextDisabled("Water Zone: %s", owner ? owner->name.c_str() : "(missing)");
                ImGui::TextWrapped("接続中の波・材質・浮力係数はZoneの親から使います。このWaterの設定はZoneを解除すると戻ります。");
            } else if (water.zoneSnapshot) {
                ImGui::TextDisabled("Connected water surface");
                if (scene::WaterSurfaceGeometry(water)->meshQualityLimited)
                    ImGui::TextDisabled("共有水面の細かさが上限に達しています。");
            }
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
            ImGui::SeparatorText("Surface Shape");
            static constexpr const char* SHAPE_NAMES[] = {"Rectangle", "Spline Lake", "Spline River"};
            int surfaceShape = static_cast<int>(water.surfaceShape);
            const auto initializeSpline = [&water]() {
                water.splinePoints.clear();
                if (water.surfaceShape == scene::WaterSurfaceShape::SPLINE_LAKE) {
                    water.splinePoints = {
                        {1, {-10.0f, 0.0f, -10.0f}, 6.0f, 2.0f, 0.0f},
                        {2, { 10.0f, 0.0f, -10.0f}, 6.0f, 2.0f, 0.0f},
                        {3, { 10.0f, 0.0f,  10.0f}, 6.0f, 2.0f, 0.0f},
                        {4, {-10.0f, 0.0f,  10.0f}, 6.0f, 2.0f, 0.0f}};
                } else if (water.surfaceShape == scene::WaterSurfaceShape::SPLINE_RIVER) {
                    water.splinePoints = {
                        {1, {-10.0f,  0.0f, 0.0f}, 6.0f, 2.0f, 1.0f},
                        {2, {  0.0f, -0.5f, 2.0f}, 6.0f, 2.0f, 1.0f},
                        {3, { 10.0f, -1.0f, 0.0f}, 6.0f, 2.0f, 1.0f}};
                }
                water.meshDirty = water.foamDirty = true;
            };
            if (ImGui::Combo("Shape", &surfaceShape, SHAPE_NAMES, 3)) {
                water.surfaceShape = static_cast<scene::WaterSurfaceShape>(surfaceShape);
                if (water.pointSourceMode == scene::ControlPointSourceMode::INLINE && water.splinePoints.empty()) initializeSpline();
                water.meshDirty = water.foamDirty = true;
            }
            const bool splineShape = water.surfaceShape != scene::WaterSurfaceShape::RECTANGLE;
            if (splineShape) {
                int samples = static_cast<int>(water.splineSamplesPerSegment);
                if (ImGui::SliderInt("Curve Detail", &samples, 1, 64)) {
                    water.splineSamplesPerSegment = static_cast<uint32_t>(samples);
                    water.meshDirty = water.foamDirty = true;
                }
                if (ImGui::DragFloat("Surface Detail Size", &water.splineMeshCellSize, 0.05f, 0.05f, 100.0f, "%.2f m"))
                    water.meshDirty = true;
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    ImGui::SetTooltip("水面内部の頂点間隔。小さくすると波を細かく描きます。\n物理が評価する波の高さは変わりません。");
                if (water.pointSourceMode == scene::ControlPointSourceMode::INLINE) {
                if (ImGui::Button("Reset Shape")) initializeSpline();
                if (water.surfaceShape == scene::WaterSurfaceShape::SPLINE_LAKE && !water.splinePoints.empty()) {
                    float lakeHeight = water.splinePoints.front().localPosition.y;
                    if (ImGui::DragFloat("Surface Height", &lakeHeight, 0.1f)) {
                        for (auto& point : water.splinePoints) point.localPosition.y = lakeHeight;
                        water.meshDirty = water.foamDirty = true;
                    }
                }
                int removePoint = -1;
                for (size_t i = 0; i < water.splinePoints.size(); ++i) {
                    auto& point = water.splinePoints[i];
                    ImGui::PushID(static_cast<int>(i));
                    const auto label = "Point " + std::to_string(i + 1);
                    if (ImGui::TreeNode(label.c_str())) {
                        bool changed = false;
                        if (water.surfaceShape == scene::WaterSurfaceShape::SPLINE_LAKE) {
                            float position[2] = {point.localPosition.x, point.localPosition.z};
                            if (ImGui::DragFloat2("Position XZ", position, 0.1f)) {
                                point.localPosition.x = position[0]; point.localPosition.z = position[1];
                                changed = true;
                            }
                        } else {
                            changed |= ImGui::DragFloat3("Position", &point.localPosition.x, 0.1f);
                            changed |= ImGui::DragFloat("Width", &point.width, 0.1f, 0.01f, 10000.0f, "%.2f m");
                        }
                        changed |= ImGui::DragFloat("Depth", &point.depth, 0.1f, 0.0f, 10000.0f, "%.2f m");
                        changed |= ImGui::DragFloat("Flow Speed", &point.flowSpeed, 0.05f, -1000.0f, 1000.0f, "%.2f m/s");
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                            ImGui::SetTooltip("正の値で点の並び方向へ流れ、負の値で逆向きに流れます。");
                        if (ImGui::SmallButton("Remove Point")) removePoint = static_cast<int>(i);
                        if (changed) water.meshDirty = water.foamDirty = true;
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
                if (removePoint >= 0) {
                    water.splinePoints.erase(water.splinePoints.begin() + removePoint);
                    water.meshDirty = water.foamDirty = true;
                }
                ImGui::BeginDisabled(water.splinePoints.size() >= 256);
                if (ImGui::Button("Add Point")) {
                    scene::WaterSplinePoint point;
                    if (!water.splinePoints.empty()) point = water.splinePoints.back();
                    point.id = 1;
                    while (std::any_of(water.splinePoints.begin(), water.splinePoints.end(),
                        [&point](const scene::WaterSplinePoint& other) { return other.id == point.id; })) ++point.id;
                    point.localPosition.x += 5.0f;
                    if (water.splinePoints.empty()) { point.width = 6.0f; point.depth = 2.0f; }
                    water.splinePoints.push_back(point);
                    water.meshDirty = water.foamDirty = true;
                }
                ImGui::EndDisabled();
                }
                if (!water.splineSnapshot)
                    ImGui::TextColored({1.0f, 0.65f, 0.25f, 1.0f}, "形状を確認してください。点の不足や交差があると水面を作れません。");
                else if (water.splineSnapshot->meshQualityLimited)
                    ImGui::TextDisabled("水面の細かさが上限に達しています。");
            }
            ImGui::SeparatorText("Geometry");
            ImGui::BeginDisabled(scene::UsesWaterSurfaceGeometry(water) || water.zoneOwner.IsValid());
            if (ImGui::Checkbox("Adaptive Mesh", &water.adaptivePatchLod)) water.meshDirty = true;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("カメラと画面上の大きさに合わせて、水面の細かさを変えます。\n"
                                  "波と浮力の設定は描画の細かさに影響されません。");
            if (water.adaptivePatchLod || water.farOceanEnabled) {
                int patchResolution = static_cast<int>(water.lodPatchResolution);
                int maxDepth = static_cast<int>(water.lodMaxDepth);
                int maxPatches = static_cast<int>(water.lodMaxPatches);
                if (ImGui::SliderInt("Patch Resolution", &patchResolution, 2, 64)) {
                    water.lodPatchResolution = 2;
                    while (water.lodPatchResolution < static_cast<uint32_t>(patchResolution)) water.lodPatchResolution *= 2;
                    water.meshDirty = true;
                }
                if (ImGui::SliderInt("Detail Levels", &maxDepth, 0, 10)) water.lodMaxDepth = static_cast<uint32_t>(maxDepth);
                if (ImGui::SliderInt("Patch Budget", &maxPatches, 1, 4096)) water.lodMaxPatches = static_cast<uint32_t>(maxPatches);
                ImGui::DragFloat("Screen Cell Width", &water.lodTargetPixels, 0.25f, 1.0f, 64.0f, "%.1f px");
                ImGui::DragFloat("Near Detail Range", &water.lodNearDistance, 1.0f, 0.0f, 10000.0f, "%.1f m");
                ImGui::DragFloat("Near Detail Size", &water.lodNearCellSize, 0.05f, 0.25f, 100.0f, "%.2f m");
                ImGui::Checkbox("Smooth LOD", &water.lodMorphEnabled);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    ImGui::SetTooltip("格子の切り替えを滑らかにし、カメラ移動時の波形の変化を抑えます。");
                ImGui::BeginDisabled(!water.lodMorphEnabled);
                ImGui::DragFloat("LOD Transition Width", &water.lodMorphTransitionWidth, 0.01f, 0.05f, 1.0f, "%.2f");
                ImGui::EndDisabled();
            }
            ImGui::BeginDisabled(water.adaptivePatchLod || water.farOceanEnabled);
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
            ImGui::EndDisabled();
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
            ImGui::BeginDisabled(water.adaptivePatchLod || water.farOceanEnabled);
            {
                int chunks = static_cast<int>(water.chunkCount);
                if (ImGui::DragInt("Chunk Count", &chunks, 1.0f, 1, 64)) {
                    chunks = std::clamp(chunks, 1, 64);
                    water.chunkCount = static_cast<uint32_t>(chunks);
                    water.meshDirty = true;
                }
                ImGui::TextDisabled("(%d x %d chunks = %d draw calls)", chunks, chunks, chunks * chunks);
            }
            ImGui::EndDisabled();
            ImGui::SeparatorText("Far Ocean");
            if (ImGui::Checkbox("Extend Ocean To Horizon", &water.farOceanEnabled)) water.meshDirty = true;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("海面と水中領域をカメラの遠方まで延長します。\n"
                    "浮力・水中判定も水平に広がります。水深と除外領域は維持します。\n"
                    "湖や川では必要なときだけ有効にしてください。近景の細かさも自動調整します。");
            ImGui::BeginDisabled(!water.farOceanEnabled);
            ImGui::DragFloat("Far Coverage Margin", &water.farOceanCoverageScale, 0.01f, 1.0f, 2.0f, "x%.2f");
            ImGui::DragFloat("Outer Ripple Fade", &water.farOceanRippleFadeDistance, 0.1f, 0.1f, 1000.0f, "%.1f m");
            ImGui::EndDisabled();

            ImGui::EndDisabled();
            ImGui::SeparatorText("Waves");
            ComponentImGuiReflector waveRefUi;
            waveRefUi.m_projectRoot = ctx.projectRoot;
            waveRefUi.Field("Wave Asset", water.wavesAssetRef);
            if (waveRefUi.m_changed && ctx.markSceneDirty) ctx.markSceneDirty();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("WaterWavesAssetを共有して波を生成します。\n未設定なら材質に保存した4本の波を使います。");
            ImGui::Checkbox("Enable Waves", &water.enableGerstnerWaves);
            ImGui::BeginDisabled(!water.enableGerstnerWaves);
            if (ImGui::DragFloat("Amplitude Scale", &water.waveAmplitudeScale, 0.01f, 0.0f, 10.0f, "x%.2f"))
                water.waveAmplitudeScale = (std::max)(water.waveAmplitudeScale, 0.0f);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("波Assetまたは材質の波の高さに掛ける倍率。\n"
                                  "同じ Ocean.mat を «凪の入り江» と «外洋» に使い分けるときに使います。");
            ImGui::EndDisabled();
            ImGui::DragFloat("Shallow Wave Depth", &water.waveAttenuationDepth, 0.05f, 0.0f, 10000.0f, "%.2f m");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("この深さより浅い場所で波を弱めます。\n0で無効。描画と浮力に同じ減衰を使います。");
            /// @note 実際に描かれている波 (.mat × 倍率 × 環境風)。風の効き具合をここで確かめる。
            for (size_t i = 0; i < (std::min)(static_cast<size_t>(water.waveCount), water.waves.size()); ++i) {
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
            ImGui::EndDisabled();
            if (ImGui::DragFloat("Water Depth", &water.buoyancyDepth, 0.1f, 0.1f, 1000.0f, "%.1f m"))
                water.buoyancyDepth = (std::max)(water.buoyancyDepth, 0.1f);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("水面からこの深さまでを水中として扱います。\n"
                                  "浮力が有効な場合、その作用範囲にも使います。");
            ImGui::Checkbox("Splash & Ripples", &water.splashEnabled);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("RigidBody が水面を通過したときに波紋としぶきを出し、\n"
                                  "水面をまたいで進む物体には航跡を引かせます。");

            ImGui::SeparatorText("Water Interaction");
            auto& interaction = water.interactionSettings;
            ImGui::Checkbox("Enable Interaction Effects", &interaction.enabled);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("泳ぐ剛体から泡を、床との接触から砂の濁りを発生させます。\n"
                    "Water Exclusionの中では発生しません。Zoneでは統合元のWaterで設定します。");
            ImGui::BeginDisabled(!interaction.enabled);
            ImGui::Checkbox("Surface Foam", &interaction.foamEnabled);
            ImGui::Checkbox("Swimming Bubbles", &interaction.bubblesEnabled);
            ImGui::Checkbox("Contact Sediment", &interaction.sedimentEnabled);
            if (ImGui::TreeNode("Motion Effects")) {
                ImGui::DragFloat("Minimum Swimming Speed", &interaction.minimumMotionSpeed,
                    0.05f, 0.01f, 100.0f, "%.2f m/s");
                ImGui::DragFloat("Motion Spawn Interval", &interaction.motionInterval,
                    0.01f, 0.01f, 10.0f, "%.2f s");
                ImGui::DragFloat("Surface Foam Band", &interaction.surfaceBand,
                    0.01f, 0.01f, 10.0f, "%.2f m");
                ImGui::DragFloat("Bubble Lifetime", &interaction.bubbleLifetime,
                    0.05f, 0.01f, 60.0f, "%.2f s");
                widgets::AssetPathField("Bubble Material", interaction.bubbleMaterialPath, ".mat", ctx.projectRoot);
                ImGui::TreePop();
            }
            if (ImGui::TreeNode("Sediment Effects")) {
                ImGui::DragFloat("Minimum Contact Speed", &interaction.sedimentMinimumSpeed,
                    0.05f, 0.01f, 100.0f, "%.2f m/s");
                ImGui::DragFloat("Sediment Spawn Interval", &interaction.sedimentInterval,
                    0.01f, 0.01f, 10.0f, "%.2f s");
                ImGui::DragFloat("Sediment Lifetime", &interaction.sedimentLifetime,
                    0.05f, 0.01f, 60.0f, "%.2f s");
                ImGui::DragFloat3("Sediment Scattering", &interaction.sedimentScattering.x,
                    0.001f, 0.0f, 1000.0f, "%.3f /m");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    ImGui::SetTooltip("濁りによるRGBの光の散乱量です。大きくすると砂の雲が濃くなります。");
                widgets::AssetPathField("Sediment Material", interaction.sedimentMaterialPath, ".mat", ctx.projectRoot);
                ImGui::TreePop();
            }
            ImGui::EndDisabled();
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
