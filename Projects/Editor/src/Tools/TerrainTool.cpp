/// @file    TerrainTool.cpp
/// @brief   TerrainTool の実装: レイキャスト・ストロークと Undo・ImGui UI。
/// @author  Hasegawa Jin
/// @date    2026-05-31
/// @see Docs/design/terrain-layers.md
#include "TerrainTool.hpp"
#include "TerrainAuthoring.hpp"
#include "TerrainScatter.hpp"
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/Util/AssetSearch.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/FiberComponent.hpp>
#include <Engine/Scene/Components/InstancedMeshComponent.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/TerrainPaintAsset.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Scene/Components/TerrainGridComponent.hpp>
#include <Engine/Scene/TerrainHeightMapLoader.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Math/Ray.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <imgui.h>
#include <string>
#include <vector>

namespace fbzz::editor {

namespace {

bool IsTerrainPaintReference(const std::string& reference)
{
    return util::FileSystem::GetExtension(asset::AssetManager::ResolveAssetPath(reference)) == ".terrainpaint";
}

ImU32 TerrainPaintColor(size_t index)
{
    constexpr ImU32 COLORS[] = {
        IM_COL32(97, 220, 162, 255), IM_COL32(251, 174, 97, 255), IM_COL32(102, 195, 249, 255),
        IM_COL32(235, 126, 186, 255), IM_COL32(217, 210, 104, 255), IM_COL32(172, 145, 254, 255),
        IM_COL32(251, 117, 103, 255), IM_COL32(97, 220, 218, 255), IM_COL32(195, 235, 129, 255),
        IM_COL32(240, 160, 218, 255), IM_COL32(152, 166, 245, 255), IM_COL32(245, 202, 139, 255),
        IM_COL32(98, 184, 144, 255), IM_COL32(242, 232, 204, 255), IM_COL32(104, 161, 220, 255),
        IM_COL32(223, 158, 132, 255)
    };
    static_assert(std::size(COLORS) == TERRAIN_SCATTER_PALETTE_LIMIT);
    return COLORS[index % std::size(COLORS)];
}

std::string TerrainToolObjectLabel(const scene::GameObject& object)
{
    return object.name + " [" + object.instanceId.substr(0, 8) + "]";
}

void TerrainToolPathField(const char* id, char* path, size_t capacity)
{
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText(id, path, capacity);
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
            strncpy_s(path, capacity, static_cast<const char*>(payload->Data), _TRUNCATE);
        ImGui::EndDragDropTarget();
    }
}

void TerrainToolStatus(const std::string& status)
{
    if (status.empty()) return;
    const bool ok = status.starts_with("OK");
    ImGui::PushStyleColor(ImGuiCol_Text, ok ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
    ImGui::TextWrapped("%s", status.c_str());
    ImGui::PopStyleColor();
}

std::string TerrainToolOpStatus(const OpResult& result)
{
    return std::string(result.ok ? "OK: " : "Error: ") + result.message;
}

void TerrainToolLayerCombo(const char* label, const scene::TerrainComponent* terrain, int& layer, bool allowAll = false)
{
    std::string preview = layer < 0 ? "All layers" : "Layer " + std::to_string(layer);
    if (terrain && layer >= 0 && layer < terrain->LayerCount())
        preview += "  " + terrain->layerMaterials[static_cast<size_t>(layer)];
    if (!ImGui::BeginCombo(label, preview.c_str())) return;
    if (allowAll && ImGui::Selectable("All layers", layer < 0)) layer = -1;
    if (terrain) {
        for (int i = 0; i < terrain->LayerCount(); ++i) {
            const std::string name = "Layer " + std::to_string(i) + "  " + terrain->layerMaterials[static_cast<size_t>(i)];
            if (ImGui::Selectable(name.c_str(), layer == i)) layer = i;
        }
    }
    ImGui::EndCombo();
}

float TerrainToolMinWorldHeight(const scene::TerrainComponent& terrain)
{
    if (terrain.heightData.empty()) return 0.0f;
    const auto minIt = std::min_element(terrain.heightData.begin(), terrain.heightData.end());
    return *minIt * terrain.maxHeight;
}

const char* TerrainToolLayerDisplayName(const std::string& path)
{
    if (path.empty())
        return "(empty)";

    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path.c_str() : path.c_str() + slash + 1;
}

/// @return シーン内の最初の有効な地形。無ければ null。
scene::TerrainComponent* TerrainToolFirstTerrain(scene::Scene& scene, scene::GameObject** outObject)
{
    for (scene::EntityID eid : scene.GetEntities<scene::TerrainComponent>()) {
        auto* tc = scene.GetComponent<scene::TerrainComponent>(eid);
        if (tc && tc->enabled) {
            if (outObject) *outObject = scene.GetGameObject(eid);
            return tc;
        }
    }
    return nullptr;
}

/// @brief ワールド座標をビューポートのスクリーン座標へ投影する。カメラの後ろは画面外の値を返す。
ImVec2 TerrainToolProject(const math::Matrix4& viewProjection, const ImVec2& viewportMin,
                          const ImVec2& viewportSize, const math::Vector3& p)
{
    const math::Vector4 clip = viewProjection * math::Vector4{ p.x, p.y, p.z, 1.0f };
    if (clip.w < 0.001f) return { -99999.0f, -99999.0f };
    const float ndcX = clip.x / clip.w;
    const float ndcY = clip.y / clip.w;
    return {
        viewportMin.x + (ndcX + 1.0f) * 0.5f * viewportSize.x,
        viewportMin.y + (1.0f - (ndcY + 1.0f) * 0.5f) * viewportSize.y
    };
}

/// @brief 横幅に収まる限り同じ行へ並べる SmallButton。
/// @return 押されたか。
bool TerrainToolFlowButton(const char* label, bool active, float rightEdge, bool first, ThemeColor activeColor)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float width = ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
    if (!first && ImGui::GetItemRectMax().x + style.ItemSpacing.x + width < rightEdge)
        ImGui::SameLine();
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(activeColor));
    const bool pressed = ImGui::SmallButton(label);
    if (active) ImGui::PopStyleColor();
    return pressed;
}

} /// @note namespace

const char* TerrainTool::SculptModeLabel(SculptMode mode)
{
    switch (mode) {
        case SculptMode::Raise:            return "Raise";
        case SculptMode::Lower:            return "Lower";
        case SculptMode::Smooth:           return "Smooth";
        case SculptMode::Flatten:          return "Flatten";
        case SculptMode::Stamp:            return "Stamp";
        case SculptMode::Noise:            return "Noise";
        case SculptMode::ThermalErosion:   return "Thermal";
        case SculptMode::HydraulicErosion: return "Hydraulic";
        case SculptMode::Terrace:          return "Terrace";
    }
    return "?";
}

std::string TerrainTool::StatusLabel() const
{
    switch (m_mode) {
        case Mode::Sculpt: return m_rampMode ? "Ramp" : SculptModeLabel(m_sculpt);
        case Mode::Paint:
            if (m_paintTarget == PaintTarget::Grass) return "Grass";
            if (m_paintTarget == PaintTarget::Instances) return "Placement Brush";
            if (m_paintTarget == PaintTarget::HeightMap) return "HeightMap Stamp";
            return "Layer " + std::to_string(m_paintLayer);
        case Mode::Hole:   return m_holeErase ? "Fill Hole" : "Cut Hole";
    }
    return {};
}

void TerrainTool::Update(
    EditorContext&              context,
    const renderer::Camera&     camera,
    float                       dt,
    bool                        viewportHovered,
    const ImVec2&               viewportMin,
    const ImVec2&               viewportSize)
{
    if (!context.activeScene) return;
    auto& scene = *context.activeScene;
    const auto& markDirty = context.markSceneDirty;
    const auto generation = scene.GetRenderSceneGeneration();
    if (m_strokeSceneGeneration != generation) {
        m_strokeSceneGeneration = generation;
        m_strokeBeforeTerrains.clear();
        m_scatterStroke.clear();
        m_paintPreviewTerrainId.clear();
        m_strokeActive = m_rampDragging = m_flattenLocked = false;
        m_hitTerrain = nullptr;
        m_instanceOwner.clear();m_instanceId=0;m_instanceDragging=false;m_paintArea.clear();
    }
    /// @note ビューポート外・非アクティブではレイキャストしない。ただし進行中のストロークは離すまで追う。
    const bool scatterPaint = m_mode == Mode::Paint && m_paintTarget == PaintTarget::Instances;
    const bool heightStamp = m_mode == Mode::Paint && m_paintTarget == PaintTarget::HeightMap;
    if (!scatterPaint || !m_active) {
        m_scatterStroke.clear();
        m_paintPreviewTerrainId.clear();
    }
    if(m_active&&scatterPaint&&m_instanceMode==1){UpdateInstanceSelection(context,camera,viewportMin,viewportSize,viewportHovered);return;}
    if (!m_active || !viewportHovered) {
        m_isHovering = false;
        m_hitTerrain = nullptr;
        if (!m_strokeActive && !m_rampDragging && m_scatterStroke.empty()
            && !(scatterPaint && m_showPaintPreview && !m_paintPreviewTerrainId.empty())) return;
    }

    /// @note [ / ] で半径、Shift+[ / ] で強度。ホイールはカメラ操作と競合するため使わない。
    if (viewportHovered && heightStamp) {
        const float factor = ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true) ? 0.9f
            : ImGui::IsKeyPressed(ImGuiKey_RightBracket, true) ? 1.1f : 1.0f;
        if (ImGui::GetIO().KeyShift)
            m_heightStamp.strength = std::clamp(m_heightStamp.strength + (factor - 1.0f) * 0.2f, 0.0f, 1.0f);
        else {
            m_heightStamp.width = std::clamp(m_heightStamp.width * factor, 0.01f, 100000.0f);
            m_heightStamp.depth = std::clamp(m_heightStamp.depth * factor, 0.01f, 100000.0f);
        }
    } else if (viewportHovered) {
        const float radiusLimit = BrushRadiusLimit();
        m_brush.radius = std::min(m_brush.radius, radiusLimit);
        const bool  shift        = ImGui::GetIO().KeyShift;
        const float radiusStep   = std::max(0.5f, m_brush.radius * 0.1f);
        const float strengthStep = 0.02f;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true)) {
            if (shift) m_brush.strength = std::clamp(m_brush.strength - strengthStep, 0.001f, 1.0f);
            else       m_brush.radius   = std::clamp(m_brush.radius   - radiusStep,   0.5f, radiusLimit);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, true)) {
            if (shift) m_brush.strength = std::clamp(m_brush.strength + strengthStep, 0.001f, 1.0f);
            else       m_brush.radius   = std::clamp(m_brush.radius   + radiusStep,   0.5f, radiusLimit);
        }
    }

    /// @note 修飾キーによる一時上書き (Unity Terrain 互換)。Ctrl を優先し、Ctrl+Shift は Lower。Ramp 中は上書きしない。
    const ImGuiIO& io = ImGui::GetIO();
    m_activeSculpt = m_sculpt;
    if (m_mode == Mode::Sculpt && !m_rampMode && viewportHovered) {
        if (io.KeyCtrl)       m_activeSculpt = SculptMode::Lower;
        else if (io.KeyShift) m_activeSculpt = SculptMode::Smooth;
    }

    math::Vector3     hitWorld;
    scene::GameObject* hitGO = nullptr;
    m_isHovering = viewportHovered && m_active
        && RaycastTerrain(scene, camera, viewportMin, viewportSize, hitWorld, hitGO);
    if (m_isHovering) {
        m_hitPoint   = hitWorld;
        m_hitTerrain = hitGO;
    } else {
        /// @note ヒットなしでもブラシ円は最後のヒット位置に残す (移動の遅延を自然に見せる)。
        m_hitTerrain = nullptr;
    }

    const bool mousePressed  = viewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool mouseHeld     = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool mouseReleased = ImGui::IsMouseReleased(ImGuiMouseButton_Left);

    if (heightStamp) {
        UpdateHeightMapStamp(context, mousePressed);
        if (m_isHovering && m_hitTerrain && m_hitTerrain->instanceId == m_heightMapTargetId)
            DrawHeightMapStampPreview(viewportMin, viewportSize, camera);
        return;
    }

    if (scatterPaint) {
        if(m_instanceMode==2){UpdatePaintArea(context,camera,viewportMin,viewportSize,mousePressed,viewportHovered);UpdateScatterPreview(context,viewportHovered);DrawPaintDiagnostics(camera,viewportMin,viewportSize);return;}
        UpdateScatterStroke(context, mousePressed, mouseHeld);
        UpdateScatterPreview(context, viewportHovered);
        DrawPaintDiagnostics(camera,viewportMin,viewportSize);
        if (!m_paintPalette.empty() && m_hitTerrain && m_hitTerrain->instanceId == m_heightMapTargetId)
            DrawBrushPreview(viewportMin, viewportSize, camera);
        return;
    }

    if (m_mode == Mode::Sculpt && m_rampMode) {
        UpdateRamp(context, mousePressed, mouseReleased || (!mouseHeld && m_rampDragging));
        if (m_rampDragging)
            DrawRampPreview(viewportMin, viewportSize, camera);
        else if (m_isHovering)
            DrawBrushPreview(viewportMin, viewportSize, camera);
        return;
    }

    const bool grassPaint = m_mode == Mode::Paint && m_paintTarget == PaintTarget::Grass;
    const auto* fiber = grassPaint && m_hitTerrain && m_hitTerrain->instanceId == m_heightMapTargetId
        ? m_hitTerrain->GetComponent<scene::FiberComponent>() : nullptr;
    if (mouseHeld && m_isHovering && m_hitTerrain
        && (!grassPaint || (fiber && fiber->m_enabled && fiber->m_terrainLayer >= 0))) {
        auto* terrainComp = m_hitTerrain->GetComponent<scene::TerrainComponent>();
        assert(terrainComp && "ヒット判定したのに TerrainComponent がない — RaycastSingleTerrain のバグ");
        const int grassLayer = fiber ? ((m_grassErase != io.KeyCtrl) ? m_grassEraseLayer : fiber->m_terrainLayer) : -1;
        const bool validGrassLayer = !grassPaint || (grassLayer >= 0 && grassLayer < terrainComp->LayerCount()
            && (!(m_grassErase != io.KeyCtrl) || grassLayer != fiber->m_terrainLayer));
        if (!validGrassLayer) return;
        if (!m_strokeActive) {
            m_strokeBeforeTerrains.clear();
            m_strokeActive = true;
            m_strokeStep   = 0;
        }
        CaptureStrokeBefore(*m_hitTerrain, *terrainComp);

        const math::Vector3 hitLocal = ToTerrainLocal(m_hitTerrain->transform, m_hitPoint);

        /// @note Flatten は最初のクリックで基準高さを固定する。
        if (m_mode == Mode::Sculpt && m_activeSculpt == SculptMode::Flatten && !m_flattenLocked) {
            m_flattenTarget = terrainComp->GetHeightAt(hitLocal.x, hitLocal.z);
            m_flattenLocked = true;
        }

        /// @note ブラシ範囲に重なる全 Terrain を編集する。カーソル下の 1 つだけだと境界に段差・継ぎ目が残る。
        /// @note 共有境界の頂点は同一のワールド点なので、各 Terrain のローカルへブラシ中心を変換すれば同じデルタが入る。
        switch (m_mode) {
            case Mode::Sculpt:
                /// @note colliderDirty は確定時に 1 回だけ立てる (毎フレーム BVH 再構築を避ける)。
                for (scene::EntityID eid : scene.GetEntities<scene::TerrainComponent>()) {
                    auto* go = scene.GetGameObject(eid);
                    auto* tc = scene.GetComponent<scene::TerrainComponent>(eid);
                    if (!go || !tc || !tc->enabled || tc->heightData.empty())
                        continue;
                    const math::Vector3 localN = ToTerrainLocal(go->transform, m_hitPoint);
                    if (!BrushOverlapsTerrainXZ(*tc, localN, m_brush.radius))
                        continue;
                    CaptureStrokeBefore(*go, *tc);
                    tc->RequestHeightPreview(
                        ApplyTerrainSculpt(*tc, localN, m_brush, m_activeSculpt, m_flattenTarget, dt, m_strokeStep));
                }
                break;
            case Mode::Paint: {
                if (grassPaint) {
                    ApplyTerrainPaint(*terrainComp, hitLocal, m_brush, grassLayer, dt);
                    terrainComp->splatDirty = true;
                    break;
                }
                const int layerCount = std::max(terrainComp->LayerCount(), 1);
                const int paintLayer = std::min(static_cast<int>(m_paintLayer), layerCount - 1);
                const std::string sourceMaterial = paintLayer < terrainComp->LayerCount()
                    ? terrainComp->layerMaterials[static_cast<size_t>(paintLayer)]
                    : std::string();
                for (scene::EntityID eid : scene.GetEntities<scene::TerrainComponent>()) {
                    auto* go = scene.GetGameObject(eid);
                    auto* tc = scene.GetComponent<scene::TerrainComponent>(eid);
                    if (!go || !tc || !tc->enabled || tc->heightData.empty())
                        continue;
                    const math::Vector3 localN = ToTerrainLocal(go->transform, m_hitPoint);
                    if (!BrushOverlapsTerrainXZ(*tc, localN, m_brush.radius))
                        continue;
                    /// @note ヒットした Terrain は選択層をそのまま塗る。隣接 Terrain は層の並びが違い得るので material path で解決する。
                    int layer = paintLayer;
                    if (tc != terrainComp) {
                        layer = ResolvePaintLayerForTerrain(*tc, sourceMaterial, paintLayer);
                        if (layer < 0)
                            continue;
                    }
                    CaptureStrokeBefore(*go, *tc);
                    ApplyTerrainPaint(*tc, localN, m_brush, layer, dt);
                    tc->splatDirty = true;
                }
                break;
            }
            case Mode::Hole: {
                /// @note Ctrl+drag で Cut / Fill を一時的に反転する。
                const bool fill = m_holeErase != io.KeyCtrl;
                for (scene::EntityID eid : scene.GetEntities<scene::TerrainComponent>()) {
                    auto* go = scene.GetGameObject(eid);
                    auto* tc = scene.GetComponent<scene::TerrainComponent>(eid);
                    if (!go || !tc || !tc->enabled || tc->heightData.empty())
                        continue;
                    const math::Vector3 localN = ToTerrainLocal(go->transform, m_hitPoint);
                    if (!BrushOverlapsTerrainXZ(*tc, localN, m_brush.radius))
                        continue;
                    CaptureStrokeBefore(*go, *tc);
                    /// @note 穴は三角形の有無なのでメッシュを作り直す。コライダーは確定時に 1 回。
                    if (ApplyTerrainHole(*tc, localN, m_brush, !fill))
                        tc->heightDirty = true;
                }
                break;
            }
        }
        ++m_strokeStep;
        if (markDirty) markDirty();
    }

    if (mouseReleased || (!mouseHeld && m_strokeActive)) {
        m_flattenLocked = false;
        if (m_strokeActive) {
            const char* label = m_mode == Mode::Sculpt ? "Sculpt Terrain"
                              : m_mode == Mode::Paint  ? (grassPaint ? "Paint Terrain Grass" : "Paint Terrain")
                                                       : "Edit Terrain Holes";
            /// @note Paint は形状に影響しないのでコライダーを作り直さない。
            CommitStroke(context, label, m_mode != Mode::Paint);
        }
    }

    if (m_isHovering)
        DrawBrushPreview(viewportMin, viewportSize, camera);
}

void TerrainTool::SetPaintPalette(std::vector<std::string> sources, bool focusWindow)
{
    std::vector<PaintSource> palette;
    for (auto& reference : sources) {
        const auto guid = asset::AssetDatabase::GuidFromRef(reference);
        const auto previous = std::find_if(m_paintPalette.begin(), m_paintPalette.end(), [&](const PaintSource& source) {
            return asset::AssetDatabase::GuidFromRef(source.reference) == guid;
        });
        palette.push_back({std::move(reference), previous == m_paintPalette.end() ? 100.0f : previous->weight});
    }
    m_paintPalette = std::move(palette);
    m_nextPaintPreviewTime = 0.0;
    m_paintPreview.clear();
    m_paintPreviewInputs.clear();
    m_paintPreviewTotal = 0;
    m_paintDiagnostics={};
    m_paintPreviewError.clear();
    m_focusPaintPalette = focusWindow;
    m_scatterStroke.clear();
    m_scatterStatus.clear();
    m_scatterErase = false;
    m_paintTarget = PaintTarget::Instances;
    m_mode = Mode::Paint;
    m_active = true;
}

void TerrainTool::UpdateScatterStroke(EditorContext& context, bool pressed, bool held)
{
    auto& scene = *context.activeScene;
    const bool overTerrain = m_hitTerrain && m_hitTerrain->instanceId == m_heightMapTargetId;
    if (pressed && overTerrain && !m_paintPalette.empty()) {
        m_scatterStroke = {m_hitPoint};
        m_scatterStrokeTerrainId = m_heightMapTargetId;
        m_scatterTerrainLifetime = scene.GetComponentLifetime<scene::TerrainComponent>(m_hitTerrain->GetID());
        m_scatterStrokeRadius = m_brush.radius;
        m_scatterStrokeErase = m_scatterErase != ImGui::GetIO().KeyCtrl;
        m_scatterStrokeContinuous = true;
    }
    if (m_scatterStroke.empty()) return;
    const auto* terrain = scene.FindByGuid(m_scatterStrokeTerrainId);
    if (!terrain || m_scatterStrokeTerrainId != m_heightMapTargetId
        || scene.GetComponentLifetime<scene::TerrainComponent>(terrain->GetID()) != m_scatterTerrainLifetime) {
        m_scatterStroke.clear();
        return;
    }
    if (held) {
        if (overTerrain) {
            const auto last = m_scatterStroke.back();
            const auto delta = m_hitPoint - last;
            const float distance = delta.Length();
            const float spacing = m_scatterStrokeRadius * 0.5f;
            const float steps = m_scatterStrokeContinuous ? std::floor(distance / spacing) : 1.0f;
            if (steps > static_cast<float>(TERRAIN_SCATTER_STROKE_LIMIT - m_scatterStroke.size())) {
                m_scatterStatus = "Error: Stroke is too long. Use a shorter drag or a larger brush.";
                m_scatterStroke.clear();
                return;
            }
            for (int i = 1; i <= static_cast<int>(steps); ++i)
                m_scatterStroke.push_back(m_scatterStrokeContinuous ? last + delta * (static_cast<float>(i) * spacing / distance) : m_hitPoint);
        }
        m_scatterStrokeContinuous = overTerrain;
        return;
    }
    auto args = ScatterStrokeArguments(m_scatterStrokeTerrainId, m_scatterStroke, m_scatterStrokeRadius, m_scatterStrokeErase);
    m_scatterStroke.clear();
    m_scatterStatus = TerrainToolOpStatus(InvokeOperator(context, "terrain.paint_instances", args));
    m_nextPaintPreviewTime = 0.0;
}

OpArgs TerrainTool::ScatterStrokeArguments(const std::string& terrainId,
    std::span<const math::Vector3> centers, float radius, bool erase) const
{
    auto args = ScatterArguments(terrainId, {});
    auto sources = OpData::MakeArray();
    auto weights = OpData::MakeArray();
    for (const auto& source : m_paintPalette) {
        sources.Push(OpData(source.reference));
        weights.Push(OpData(source.weight));
    }
    args.Set("sources", std::move(sources));
    args.Set("weights", std::move(weights));
    if (std::any_of(m_paintPalette.begin(), m_paintPalette.end(), [](const PaintSource& source) {
        return IsTerrainPaintReference(source.reference);
    })) {
        args.Set("minSlope", 0.0f); args.Set("maxSlope", 90.0f);
    }
    auto stroke = OpData::MakeArray();
    for (const auto& point : centers) stroke.Push(OpData(point));
    args.Set("stroke", std::move(stroke));
    args.Set("brushRadius", radius);
    args.Set("erase", erase);
    args.Set("avoidWater", m_paintAvoidWater);
    args.Set("avoidPaths", m_paintAvoidPaths);
    args.Set("excludedLayer", m_paintExcludedLayer);
    args.Set("minimumDistance", m_paintMinimumDistance);
    args.Set("feather",m_paintFeather);args.Set("polygon",m_instanceMode==2);
    auto excluded=OpData::MakeArray();for(const auto& ref:m_paintExcludedMaterials)excluded.Push(OpData(ref));args.Set("excludedMaterials",std::move(excluded));
    return args;
}

void TerrainTool::UpdateScatterPreview(EditorContext& context, bool viewportHovered)
{
    const bool dragging = !m_scatterStroke.empty();
    const bool erase = dragging ? m_scatterStrokeErase : (m_scatterErase != ImGui::GetIO().KeyCtrl);
    if (viewportHovered) {
        if (m_hitTerrain && m_hitTerrain->instanceId == m_heightMapTargetId) {
            m_paintPreviewTerrainId = m_heightMapTargetId;
            m_paintPreviewCenter = m_hitPoint;
        } else {
            m_paintPreviewTerrainId.clear();
        }
    }
    if(m_instanceMode==2&&m_paintArea.size()>=3)m_paintPreviewTerrainId=m_heightMapTargetId;
    if (!m_showPaintPreview || m_paintPalette.empty() || m_paintPreviewTerrainId.empty()
        || m_paintPreviewTerrainId != m_heightMapTargetId || (m_instanceMode==2&&m_paintArea.size()<3)) {
        m_paintPreview.clear();
        m_paintPreviewInputs.clear();
        m_paintPreviewTotal = 0;
        m_paintDiagnostics={};
        m_paintPreviewError.clear();
        m_nextPaintPreviewTime = 0.0;
        return;
    }
    const double now = ImGui::GetTime();
    if (now < m_nextPaintPreviewTime) return;
    /// @note シーン側の占有格子の照会は描画フレームごとに繰り返さない。
    m_nextPaintPreviewTime = now + 0.1;
    const auto centers = m_instanceMode==2 ? std::span<const math::Vector3>(m_paintArea) : dragging ? std::span<const math::Vector3>(m_scatterStroke)
        : std::span<const math::Vector3>(&m_paintPreviewCenter, 1);
    const auto args = ScatterStrokeArguments(m_paintPreviewTerrainId, centers,
        dragging ? m_scatterStrokeRadius : m_brush.radius, erase);
    (void)BuildTerrainPaintPreview(context, args, m_paintPreview, m_paintPreviewTotal, m_paintPreviewError, &m_paintDiagnostics);
    m_paintPreviewInputs.clear();
    m_paintPreviewGeneration = context.activeScene->GetRenderSceneGeneration();
    std::vector<std::shared_ptr<renderer::InstanceSetData>> data;
    for (const auto& point : m_paintPreview) {
        size_t group = 0;
        for (; group < m_paintPreviewInputs.size(); ++group)
            if (m_paintPreviewInputs[group].definition == point.definition
                && std::memcmp(&m_paintPreviewInputs[group].world, &point.world, sizeof(point.world)) == 0) break;
        if (group == m_paintPreviewInputs.size()) {
            data.push_back(std::make_shared<renderer::InstanceSetData>());
            m_paintPreviewInputs.push_back({point.definition, data.back(), point.world, group + 1, erase});
        }
        auto placement = point.placement;
        placement.id = static_cast<uint32_t>(data[group]->placements.size() + 1);
        data[group]->placements.push_back(placement);
    }
}

std::span<const scene::RenderInstancePreview> TerrainTool::PaintPreviewInputs(const scene::Scene& scene) const
{
    if (!m_active || m_mode != Mode::Paint || m_paintTarget != PaintTarget::Instances || (!m_showPaintPreview&&m_instanceMode!=1)
        || m_paintPreviewGeneration != scene.GetRenderSceneGeneration()) return {};
    return m_paintPreviewInputs;
}

void TerrainTool::CaptureStrokeBefore(scene::GameObject& go, const scene::TerrainComponent& terrain)
{
    const auto found = std::find_if(
        m_strokeBeforeTerrains.begin(), m_strokeBeforeTerrains.end(),
        [&](const TerrainStrokeSnapshot& snapshot) { return snapshot.instanceId == go.instanceId; });
    if (found == m_strokeBeforeTerrains.end())
        m_strokeBeforeTerrains.push_back({ go.instanceId, terrain,
            go.GetScene()->GetComponentLifetime<scene::TerrainComponent>(go.GetID()) });
}

void TerrainTool::CommitStroke(EditorContext& context, const char* undoLabel, bool rebuildCollider)
{
    auto& scene = *context.activeScene;
    if (rebuildCollider) {
        for (const TerrainStrokeSnapshot& snapshot : m_strokeBeforeTerrains) {
            if (auto* target = scene.FindByGuid(snapshot.instanceId))
                if (auto* component = target->GetComponent<scene::TerrainComponent>())
                    component->colliderDirty = true;
        }
    }
    if (context.undoStack && !m_strokeBeforeTerrains.empty()) {
        std::vector<TerrainStrokeSnapshot> before = std::move(m_strokeBeforeTerrains);
        std::vector<TerrainStrokeSnapshot> after;
        after.reserve(before.size());
        for (const TerrainStrokeSnapshot& snapshot : before) {
            if (auto* target = scene.FindByGuid(snapshot.instanceId))
                if (auto* component = target->GetComponent<scene::TerrainComponent>())
                    after.push_back({ snapshot.instanceId, *component, snapshot.lifetime });
        }
        struct StrokeHistory {
            std::vector<TerrainStrokeSnapshot> before;
            std::vector<TerrainStrokeSnapshot> after;
        };
        const auto history = std::make_shared<StrokeHistory>(std::move(before), std::move(after));
        auto* editorContext = &context;
        const auto generation = scene.GetRenderSceneGeneration();
        auto apply = [editorContext, generation, rebuildCollider](const std::vector<TerrainStrokeSnapshot>& values,
            const std::vector<TerrainStrokeSnapshot>& expected, std::string& reason) {
            auto* currentScene = editorContext->activeScene;
            if (!currentScene || currentScene->GetRenderSceneGeneration() != generation) {
                reason = "地形を編集したシーンが変わりました。";
                return false;
            }
            if (values.size() != expected.size()) {
                reason = "ストローク中に対象地形が変わりました。";
                return false;
            }
            /// @note 複数地形にまたがるストロークは、全対象の寿命を確認してから一括で復元する。
            for (size_t index = 0; index < values.size(); ++index) {
                const auto& snapshot = values[index];
                auto* target = currentScene->FindByGuid(snapshot.instanceId);
                if (!target || !target->GetComponent<scene::TerrainComponent>()
                    || currentScene->GetComponentLifetime<scene::TerrainComponent>(target->GetID()) != snapshot.lifetime) {
                    reason = "ストロークの対象地形が削除または置換されています。";
                    return false;
                }
                if (snapshot.instanceId != expected[index].instanceId
                    || !TerrainAuthoringValuesEqual(*target->GetComponent<scene::TerrainComponent>(), expected[index].before)) {
                    reason = "ストロークの対象地形が別の操作で変更されています。";
                    return false;
                }
            }
            for (const TerrainStrokeSnapshot& snapshot : values) {
                auto* target = currentScene->FindByGuid(snapshot.instanceId);
                ApplyTerrainAuthoringSnapshot(*target->GetComponent<scene::TerrainComponent>(), snapshot.before, rebuildCollider);
            }
            if (editorContext->markSceneDirty) editorContext->markSceneDirty();
            reason.clear();
            return true;
        };
        context.undoStack->Push(std::make_unique<CheckedLambdaCommand>(
            undoLabel,
            [apply, history](std::string& reason) { return apply(history->after, history->before, reason); },
            [apply, history](std::string& reason) { return apply(history->before, history->after, reason); }));
    }
    m_strokeBeforeTerrains.clear();
    m_strokeActive = false;
}

void TerrainTool::UpdateRamp(EditorContext& context, bool mousePressed, bool mouseReleased)
{
    auto& scene = *context.activeScene;
    const auto& markDirty = context.markSceneDirty;
    if (mousePressed && m_isHovering && !m_rampDragging) {
        m_rampDragging   = true;
        m_rampStartWorld = m_hitPoint;
        m_rampEndWorld   = m_hitPoint;
    }
    if (m_rampDragging && m_isHovering)
        m_rampEndWorld = m_hitPoint;
    if (!m_rampDragging || !mouseReleased)
        return;

    m_rampDragging = false;
    m_strokeBeforeTerrains.clear();

    /// @note 線分全体を半径刻みで標本化して重なりを判定する。両端と中点だけだと、長い坂が途中で横切る Terrain を取りこぼす。
    const float dxW = m_rampEndWorld.x - m_rampStartWorld.x;
    const float dzW = m_rampEndWorld.z - m_rampStartWorld.z;
    const float lengthW = std::sqrt(dxW * dxW + dzW * dzW);
    const int samples = std::clamp(static_cast<int>(lengthW / std::max(m_brush.radius, 0.5f)) + 2, 3, 256);

    bool any = false;
    for (scene::EntityID eid : scene.GetEntities<scene::TerrainComponent>()) {
        auto* go = scene.GetGameObject(eid);
        auto* tc = scene.GetComponent<scene::TerrainComponent>(eid);
        if (!go || !tc || !tc->enabled || tc->heightData.empty())
            continue;
        const math::Vector3 startLocal = ToTerrainLocal(go->transform, m_rampStartWorld);
        const math::Vector3 endLocal   = ToTerrainLocal(go->transform, m_rampEndWorld);
        bool overlaps = false;
        for (int i = 0; i < samples && !overlaps; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(samples - 1);
            const math::Vector3 p{
                math::Lerp(startLocal.x, endLocal.x, t), 0.0f, math::Lerp(startLocal.z, endLocal.z, t) };
            overlaps = BrushOverlapsTerrainXZ(*tc, p, m_brush.radius);
        }
        if (!overlaps)
            continue;
        CaptureStrokeBefore(*go, *tc);
        tc->RequestHeightPreview(ApplyTerrainRamp(*tc, startLocal, endLocal, m_brush));
        any = true;
    }
    if (!any)
        return;
    if (markDirty)
        markDirty();
    CommitStroke(context, "Ramp Terrain", true);
}

bool TerrainTool::RaycastTerrain(
    scene::Scene&           scene,
    const renderer::Camera& camera,
    const ImVec2&           viewportMin,
    const ImVec2&           viewportSize,
    math::Vector3&          outHitWorld,
    scene::GameObject*&     outGO) const
{
    const ImVec2 mouse = ImGui::GetMousePos();
    const float ndcX = ((mouse.x - viewportMin.x) / viewportSize.x) * 2.0f - 1.0f;
    const float ndcY = 1.0f - ((mouse.y - viewportMin.y) / viewportSize.y) * 2.0f;

    const math::Matrix4 invVP = math::Matrix4::Inverse(
        camera.GetProjectionMatrix() * camera.GetViewMatrix());
    const math::Ray ray = math::Ray::FromNDC(ndcX, ndcY, camera.m_position, invVP);

    float bestT = 1e30f;
    scene::GameObject* bestGO = nullptr;
    math::Vector3 bestLocalHit;

    for (scene::EntityID eid : scene.GetEntities<scene::TerrainComponent>()) {
        auto* tc = scene.GetComponent<scene::TerrainComponent>(eid);
        auto* go = scene.GetGameObject(eid);
        if (!tc || !go || !tc->enabled || tc->heightData.empty()) continue;

        math::Vector3 localHit;
        if (!RaycastSingleTerrain(ray, *tc, go->transform, localHit)) continue;

        /// @note 地形ごとにローカル空間が違うので、ワールドへ戻した距離で最近傍を選ぶ。
        const math::Vector3 hitWorld = ToTerrainWorld(go->transform, localHit);
        const math::Vector3 toHit = {
            hitWorld.x - ray.origin.x,
            hitWorld.y - ray.origin.y,
            hitWorld.z - ray.origin.z
        };
        const float t = math::Vector3::Dot(toHit, ray.direction);
        if (t < bestT) {
            bestT        = t;
            bestGO       = go;
            bestLocalHit = localHit;
        }
    }

    if (!bestGO) return false;

    outHitWorld = ToTerrainWorld(bestGO->transform, bestLocalHit);
    outGO = bestGO;
    return true;
}

bool TerrainTool::RaycastSingleTerrain(
    const math::Ray&               ray,
    const scene::TerrainComponent& terrain,
    const scene::Transform&        tf,
    math::Vector3&                 outLocalHit) const
{
    /// @note 描画と同じ World Matrix の逆変換でレイをローカル化する。方向は w=0 で平行移動を除く。
    const math::Matrix4 invWorld = math::Matrix4::Inverse(tf.GetWorldMatrix());
    const math::Vector4 localOrigin =
        invWorld * math::Vector4{ ray.origin.x, ray.origin.y, ray.origin.z, 1.0f };
    const math::Vector4 localDirection =
        invWorld * math::Vector4{ ray.direction.x, ray.direction.y, ray.direction.z, 0.0f };
    const math::Vector3 rayOriginLocal = { localOrigin.x, localOrigin.y, localOrigin.z };
    const math::Vector3 rayDir = math::Vector3{
        localDirection.x, localDirection.y, localDirection.z
    }.Normalized();

    const float terrainW = static_cast<float>(terrain.columns - 1) * terrain.cellSize;
    const float terrainD = static_cast<float>(terrain.rows    - 1) * terrain.cellSize;
    const float maxH     = terrain.maxHeight;

    auto slab = [](float origin, float dir, float lo, float hi, float& tMin, float& tMax) {
        if (std::abs(dir) < 1e-6f) {
            if (origin < lo || origin > hi) return false;
        } else {
            float t0 = (lo - origin) / dir;
            float t1 = (hi - origin) / dir;
            if (t0 > t1) std::swap(t0, t1);
            tMin = std::max(tMin, t0);
            tMax = std::min(tMax, t1);
        }
        return tMin <= tMax;
    };

    float tMin = 0.0f, tMax = 1e30f;
    if (!slab(rayOriginLocal.x, rayDir.x, 0.0f, terrainW, tMin, tMax)) return false;
    if (!slab(rayOriginLocal.z, rayDir.z, 0.0f, terrainD, tMin, tMax)) return false;
    const float minH = std::min(0.0f, TerrainToolMinWorldHeight(terrain));
    if (!slab(rayOriginLocal.y, rayDir.y, minH, maxH, tMin, tMax)) return false;
    if (tMax <= 0.0f) return false;
    tMin = std::max(tMin, 0.0f);

    auto pointAt = [&](float t) {
        return math::Vector3{
            rayOriginLocal.x + rayDir.x * t,
            rayOriginLocal.y + rayDir.y * t,
            rayOriginLocal.z + rayDir.z * t };
    };

    /// @note DDA: 格子が離散なので cellSize 刻みで交差セルを探すのが最少の反復になる。
    const float horizontal = std::max(std::abs(rayDir.x), std::abs(rayDir.z));
    const float stepT   = horizontal > 1e-6f ? terrain.cellSize / horizontal : (tMax - tMin);
    const int   maxStep = static_cast<int>((tMax - tMin) / std::max(stepT, 1e-6f)) + 2;

    float prevT = tMin;
    float prevH = -1.0f;

    for (int step = 0; step <= maxStep; ++step) {
        const float tClamped = std::min(tMin + static_cast<float>(step) * stepT, tMax);
        const math::Vector3 p = pointAt(tClamped);
        const float terrainH = terrain.GetHeightAt(p.x, p.z);

        /// @note 前の標本が地面より上、今が下 → 交差区間。
        if (step > 0 && prevH > 0.0f && p.y <= terrainH && prevT < tMax) {
            /// @note 二分探索 8 回で cellSize 精度の区間を 1/256 に縮める。
            float lo = prevT;
            float hi = tClamped;
            for (int i = 0; i < 8; ++i) {
                const float mid = (lo + hi) * 0.5f;
                const math::Vector3 q = pointAt(mid);
                if (q.y > terrain.GetHeightAt(q.x, q.z)) lo = mid;
                else                                     hi = mid;
            }
            const math::Vector3 q = pointAt((lo + hi) * 0.5f);
            /// @note 穴のセルは面が無いので貫通させ、次の交差を探し続ける。
            if (!terrain.IsHoleAtLocal(q.x, q.z)) {
                outLocalHit = { q.x, terrain.GetHeightAt(q.x, q.z), q.z };
                return true;
            }
        }
        prevT = tClamped;
        prevH = p.y - terrainH;

        if (tClamped >= tMax) break;
    }
    return false;
}

void TerrainTool::DrawBrushPreview(
    const ImVec2&           viewportMin,
    const ImVec2&           viewportSize,
    const renderer::Camera& camera) const
{
    /// @note リングは地形ローカル XZ で分割し各点の地表高を引く。中心の高さの水平円だと斜面で編集範囲から浮いて見える。
    /// @note GetForegroundDrawList を使う。GetWindowDrawList だとビューポート画像の裏に隠れうる。
    if (!m_hitTerrain)
        return;

    const auto* terrain = m_hitTerrain->GetComponent<scene::TerrainComponent>();
    if (!terrain || terrain->heightData.empty())
        return;

    const math::Matrix4 vp = camera.GetViewProjection();
    ImDrawList* dl   = ImGui::GetForegroundDrawList();
    const bool scatter = m_mode == Mode::Paint && m_paintTarget == PaintTarget::Instances;
    const float r = scatter && !m_scatterStroke.empty() ? m_scatterStrokeRadius : m_brush.radius;
    const auto worldScale = m_hitTerrain->transform.worldScale;
    const float radiusX = scatter ? r / std::max(std::abs(worldScale.x), 0.00001f) : r;
    const float radiusZ = scatter ? r / std::max(std::abs(worldScale.z), 0.00001f) : r;
    constexpr int kSegments = 64;
    ImU32 col = IM_COL32(255, 220, 50, 220);
    if (m_mode == Mode::Paint)
        col = IM_COL32(50, 200, 255, 220);
    const bool scatterErase = m_scatterStroke.empty() ? (m_scatterErase != ImGui::GetIO().KeyCtrl) : m_scatterStrokeErase;
    if (scatter)
        col = scatterErase ? IM_COL32(255, 112, 105, 245) : IM_COL32(136, 232, 145, 245);
    else if (m_mode == Mode::Hole)
        col = (m_holeErase != ImGui::GetIO().KeyCtrl) ? IM_COL32(120, 255, 120, 220) : IM_COL32(255, 80, 80, 220);

    const math::Vector3 hitLocal = ToTerrainLocal(m_hitTerrain->transform, m_hitPoint);
    auto ringPoint = [&](float angle) {
        math::Vector3 localPoint = {
            hitLocal.x + std::cos(angle) * radiusX,
            0.0f,
            hitLocal.z + std::sin(angle) * radiusZ
        };
        localPoint.y = terrain->GetHeightAt(localPoint.x, localPoint.z);
        return ToTerrainWorld(m_hitTerrain->transform, localPoint);
    };

    dl->PushClipRect(viewportMin, {viewportMin.x + viewportSize.x, viewportMin.y + viewportSize.y}, true);
    ImVec2 prev = TerrainToolProject(vp, viewportMin, viewportSize, ringPoint(0.0f));
    for (int i = 1; i <= kSegments; ++i) {
        const float angle = static_cast<float>(i) / static_cast<float>(kSegments) * math::TWO_PI;
        const ImVec2 cur = TerrainToolProject(vp, viewportMin, viewportSize, ringPoint(angle));
        dl->AddLine(prev, cur, col, 1.5f);
        prev = cur;
    }
    if (scatter) {
        const auto mouse = ImGui::GetMousePos();
        char label[48];
        std::snprintf(label, sizeof(label), "%s  /  %.1f m", scatterErase ? "Erase" : "Add", r);
        const auto size = ImGui::CalcTextSize(label);
        const ImVec2 labelMin{
            std::max(viewportMin.x + 4.0f, std::min(mouse.x + 16.0f, viewportMin.x + viewportSize.x - size.x - 18.0f)),
            std::max(viewportMin.y + 4.0f, std::min(mouse.y + 16.0f, viewportMin.y + viewportSize.y - size.y - 14.0f))};
        const ImVec2 labelMax{labelMin.x + size.x + 12.0f, labelMin.y + size.y + 8.0f};
        dl->AddRectFilled(labelMin, labelMax, IM_COL32(20, 23, 25, 235), 4.0f);
        dl->AddText({labelMin.x + 6.0f, labelMin.y + 4.0f}, col, label);
    }
    dl->PopClipRect();
}

void TerrainTool::DrawRampPreview(
    const ImVec2&           viewportMin,
    const ImVec2&           viewportSize,
    const renderer::Camera& camera) const
{
    const math::Matrix4 vp = camera.GetViewProjection();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    constexpr ImU32 kCenter = IM_COL32(255, 160, 40, 240);
    constexpr ImU32 kEdge   = IM_COL32(255, 160, 40, 120);

    const math::Vector3& a = m_rampStartWorld;
    const math::Vector3& b = m_rampEndWorld;
    const float dx = b.x - a.x;
    const float dz = b.z - a.z;
    const float len = std::sqrt(dx * dx + dz * dz);
    /// @note 坂の幅 (radius) を線分に垂直な 2 本で示す。長さ 0 のあいだは向きが無いので中心線だけ描く。
    const float px = len > 1e-4f ? -dz / len * m_brush.radius : 0.0f;
    const float pz = len > 1e-4f ?  dx / len * m_brush.radius : 0.0f;

    auto project = [&](const math::Vector3& p) { return TerrainToolProject(vp, viewportMin, viewportSize, p); };
    dl->AddLine(project(a), project(b), kCenter, 2.5f);
    if (len > 1e-4f) {
        dl->AddLine(project({ a.x + px, a.y, a.z + pz }), project({ b.x + px, b.y, b.z + pz }), kEdge, 1.5f);
        dl->AddLine(project({ a.x - px, a.y, a.z - pz }), project({ b.x - px, b.y, b.z - pz }), kEdge, 1.5f);
    }
    dl->AddCircleFilled(project(a), 5.0f, kCenter);
    dl->AddCircle(project(b), 6.0f, kCenter, 0, 2.0f);
}

void TerrainTool::DrawSculptContent(
    scene::Scene&, UndoStack*, const std::function<void()>&)
{
    ImGui::TextDisabled("Brush Mode");
    const float rightEdge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    constexpr int kSculptModeCount = static_cast<int>(SculptMode::Terrace) + 1;
    for (int i = 0; i < kSculptModeCount; ++i) {
        const auto mode = static_cast<SculptMode>(i);
        if (TerrainToolFlowButton(SculptModeLabel(mode), !m_rampMode && m_sculpt == mode, rightEdge, i == 0,
                                  ThemeColor::Success))
            SetSculptMode(mode);
    }
    if (TerrainToolFlowButton("Ramp", m_rampMode, rightEdge, false, ThemeColor::Success))
        m_rampMode = true;

    ImGui::Spacing();
    if (m_rampMode) {
        ImGui::TextWrapped("Click to set the start, drag to the end, release to build the ramp. "
                           "Strength 1 matches the slope exactly.");
        return;
    }
    switch (m_sculpt) {
        case SculptMode::Noise:
            ImGui::SliderFloat("Noise Scale [m]", &m_brush.noiseScale, 0.5f, 200.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderInt("Octaves", &m_brush.noiseOctaves, 1, 8);
            ImGui::InputScalar("Seed", ImGuiDataType_U32, &m_brush.seed);
            ImGui::SameLine();
            if (ImGui::SmallButton("Next"))
                ++m_brush.seed;
            break;
        case SculptMode::ThermalErosion:
            ImGui::SliderFloat("Talus [deg]", &m_brush.talusDegrees, 5.0f, 85.0f, "%.0f");
            ImGui::TextDisabled("Slopes steeper than the talus angle crumble.");
            break;
        case SculptMode::HydraulicErosion:
            ImGui::SliderInt("Droplets", &m_brush.erosionDroplets, 1, 512);
            ImGui::InputScalar("Seed", ImGuiDataType_U32, &m_brush.seed);
            break;
        case SculptMode::Terrace:
            ImGui::SliderFloat("Step [m]", &m_brush.terraceStep, 0.1f, 50.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Sharpness", &m_brush.terraceSharpness, 0.0f, 1.0f, "%.2f");
            break;
        default:
            ImGui::TextDisabled("Shift+drag: Smooth   Ctrl+drag: Lower");
            break;
    }
}

void TerrainTool::DrawPaintContent(EditorContext& context)
{
    auto& scene = *context.activeScene;
    constexpr const char* targets[] = {"Ground Materials", "Fiber Grass Layer", "Objects", "HeightMap Stamp"};
    int target = static_cast<int>(m_paintTarget);
    if (ImGui::Combo("Paint", &target, targets, IM_ARRAYSIZE(targets))) {
        if (m_strokeActive) CommitStroke(context, "Paint Terrain", false);
        m_paintTarget = static_cast<PaintTarget>(target);
    }
    if (m_paintTarget != PaintTarget::Materials) {
        auto* terrain = DrawHeightMapTarget(scene);
        if (m_paintTarget == PaintTarget::HeightMap) DrawHeightMapPaint();
        else if (m_paintTarget == PaintTarget::Grass) DrawGrassPaint(context, terrain);
        else DrawInstancePaint(context, terrain);
        return;
    }
    const scene::TerrainComponent* referenceTerrain = nullptr;
    if (m_hitTerrain)
        referenceTerrain = m_hitTerrain->GetComponent<scene::TerrainComponent>();
    if (!referenceTerrain)
        referenceTerrain = TerrainToolFirstTerrain(scene, nullptr);

    ImGui::TextDisabled("Splat Layer");
    if (!referenceTerrain) {
        ImGui::TextDisabled("No terrain in scene.");
        return;
    }

    /// @note 層 0 枚の地形も «白い既定層» 1 枚として塗れる。
    const int layerCount = std::max(referenceTerrain->LayerCount(), 1);
    m_paintLayer = std::min(m_paintLayer, static_cast<uint32_t>(layerCount - 1));

    constexpr int kVisibleRows = 8;
    const float rowHeight = ImGui::GetFrameHeightWithSpacing();
    const float listHeight = rowHeight * static_cast<float>(std::min(layerCount, kVisibleRows)) + 4.0f;
    ImGui::BeginChild("##TerrainPaintLayers", { 0.0f, listHeight }, false);
    for (int i = 0; i < layerCount; ++i) {
        const bool active = static_cast<int>(m_paintLayer) == i;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(ThemeColor::AccentActive));
        std::string label = "Layer " + std::to_string(i) + "  ";
        label += i < referenceTerrain->LayerCount()
            ? TerrainToolLayerDisplayName(referenceTerrain->layerMaterials[static_cast<size_t>(i)])
            : "(default)";
        label += "##paintLayer" + std::to_string(i);
        if (ImGui::Button(label.c_str(), { -1.0f, 0.0f }))
            m_paintLayer = static_cast<uint32_t>(i);
        if (active) ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Add or reorder layers in the Terrain Inspector.");
}

void TerrainTool::DrawHeightMapPaint()
{
    ImGui::SeparatorText("HeightMap Stamp");
    ImGui::TextDisabled("Image (PNG / TGA / DDS)");
    TerrainToolPathField("##HeightStampPath", m_heightStampPath, sizeof(m_heightStampPath));
    ImGui::TextWrapped("Drop a height image here, then click the selected terrain to paste once. The bright edge marks the top of the image.");
    ImGui::DragFloat("Width [m]", &m_heightStamp.width, 0.5f, 0.01f, 100000.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::DragFloat("Depth [m]", &m_heightStamp.depth, 0.5f, 0.01f, 100000.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SliderFloat("Rotation [deg]", &m_heightStamp.rotationDegrees, -180.0f, 180.0f, "%.1f");
    ImGui::DragFloat("Height [m]", &m_heightStamp.heightScale, 0.25f, -100000.0f, 100000.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::DragFloat("Height Offset [m]", &m_heightStamp.heightOffset, 0.25f, -100000.0f, 100000.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::Checkbox("Black = Zero (off: gray = zero)", &m_heightStampUnipolar);
    constexpr const char* modes[] = {"Add", "Replace", "Maximum", "Minimum"};
    int blend = static_cast<int>(m_heightStamp.blend);
    if (ImGui::Combo("Blend", &blend, modes, IM_ARRAYSIZE(modes)))
        m_heightStamp.blend = static_cast<TerrainHeightStampBlend>(blend);
    if (m_heightStamp.blend != TerrainHeightStampBlend::ADD)
        ImGui::Checkbox("Anchor to Clicked Height", &m_heightStampAnchor);
    ImGui::SliderFloat("Stamp Strength", &m_heightStamp.strength, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Edge Fade", &m_heightStamp.edgeFade, 0.0f, 0.5f, "%.2f");
    ImGui::TextWrapped("Dimensions and height follow the terrain's axes and scale. The inner outline marks where the edge fade ends.");
    ImGui::TextDisabled("[ / ] : Size    Shift+[ / ] : Strength");
    TerrainToolStatus(m_heightStampStatus);
}

void TerrainTool::UpdateHeightMapStamp(EditorContext& context, bool pressed)
{
    if (!pressed || !m_isHovering || !m_hitTerrain || m_hitTerrain->instanceId != m_heightMapTargetId) return;
    OpArgs args;
    args.Set("node", m_hitTerrain->instanceId);
    args.Set("path", std::string(m_heightStampPath));
    args.Set("center", m_hitPoint);
    args.Set("width", m_heightStamp.width);
    args.Set("depth", m_heightStamp.depth);
    args.Set("rotation", m_heightStamp.rotationDegrees);
    args.Set("heightScale", m_heightStamp.heightScale);
    float offset = m_heightStamp.heightOffset;
    if (m_heightStampAnchor && m_heightStamp.blend != TerrainHeightStampBlend::ADD)
        offset += ToTerrainLocal(m_hitTerrain->transform, m_hitPoint).y;
    args.Set("heightOffset", offset);
    args.Set("strength", m_heightStamp.strength);
    args.Set("edgeFade", m_heightStamp.edgeFade);
    args.Set("unipolar", m_heightStampUnipolar);
    constexpr const char* modes[] = {"add", "replace", "max", "min"};
    args.Set("blend", std::string(modes[static_cast<int>(m_heightStamp.blend)]));
    m_heightStampStatus = TerrainToolOpStatus(InvokeOperator(context, "terrain.heightmap.stamp", args));
}

void TerrainTool::DrawHeightMapStampPreview(const ImVec2& viewportMin, const ImVec2& viewportSize,
    const renderer::Camera& camera) const
{
    if (!m_hitTerrain || m_heightStampPath[0] == '\0') return;
    const auto* terrain = m_hitTerrain->GetComponent<scene::TerrainComponent>();
    if (!terrain) return;
    auto settings = m_heightStamp;
    settings.centerLocal = ToTerrainLocal(m_hitTerrain->transform, m_hitPoint);
    if (!IsValidTerrainHeightStamp(*terrain, settings)) return;
    const float angle = std::remainder(settings.rotationDegrees, 360.0f) * (math::PI / 180.0f);
    const float cosine = std::cos(angle), sine = std::sin(angle);
    const auto viewProjection = camera.GetViewProjection();
    auto* draw = ImGui::GetForegroundDrawList();
    draw->PushClipRect(viewportMin, {viewportMin.x + viewportSize.x, viewportMin.y + viewportSize.y}, true);
    const auto point = [&](float u, float v) {
        const float x = (u - 0.5f) * settings.width, z = (v - 0.5f) * settings.depth;
        math::Vector3 local{settings.centerLocal.x + cosine * x + sine * z, 0.0f,
            settings.centerLocal.z - sine * x + cosine * z};
        local.y = terrain->GetHeightAt(local.x, local.z);
        return local;
    };
    const auto inside = [&](const math::Vector3& local) {
        return local.x >= 0.0f && local.x <= (terrain->columns - 1) * terrain->cellSize
            && local.z >= 0.0f && local.z <= (terrain->rows - 1) * terrain->cellSize;
    };
    const auto outline = [&](float margin, bool inner) {
        const float corners[][2] = {{margin, margin}, {1.0f - margin, margin},
            {1.0f - margin, 1.0f - margin}, {margin, 1.0f - margin}};
        for (int side = 0; side < 4; ++side) {
            const auto& a = corners[side];
            const auto& b = corners[(side + 1) % 4];
            auto previous = point(a[0], a[1]);
            for (int segment = 1; segment <= 32; ++segment) {
                const float t = static_cast<float>(segment) / 32.0f;
                const auto current = point(std::lerp(a[0], b[0], t), std::lerp(a[1], b[1], t));
                if (inside(previous) && inside(current)) {
                    const auto from = TerrainToolProject(viewProjection, viewportMin, viewportSize, ToTerrainWorld(m_hitTerrain->transform, previous));
                    const auto to = TerrainToolProject(viewProjection, viewportMin, viewportSize, ToTerrainWorld(m_hitTerrain->transform, current));
                    const ImU32 color = inner ? IM_COL32(96, 200, 255, 120)
                        : side == 0 ? IM_COL32(240, 225, 100, 255) : IM_COL32(96, 200, 255, 240);
                    draw->AddLine(from, to, color, inner ? 1.0f : 2.0f);
                }
                previous = current;
            }
        }
    };
    outline(0.0f, false);
    if (settings.edgeFade > 0.0f && settings.edgeFade < 0.5f) outline(settings.edgeFade, true);
    draw->AddCircle(TerrainToolProject(viewProjection, viewportMin, viewportSize, m_hitPoint), 4.0f, IM_COL32(240, 225, 100, 255));
    draw->PopClipRect();
}

void TerrainTool::DrawGrassPaint(EditorContext& context, scene::GameObject* object)
{
    if (!object) return;
    const auto* terrain = object->GetComponent<scene::TerrainComponent>();
    if (!terrain || terrain->LayerCount() == 0) {
        ImGui::TextWrapped("Add a terrain material layer in the Inspector to paint grass.");
        return;
    }
    const auto* fiber = object->GetComponent<scene::FiberComponent>();
    if (!fiber || !fiber->m_enabled || fiber->m_terrainLayer < 0 || fiber->m_terrainLayer >= terrain->LayerCount()) {
        m_biomeGrassLayer = std::clamp(m_biomeGrassLayer, 0, terrain->LayerCount() - 1);
        TerrainToolLayerCombo("Grass Layer", terrain, m_biomeGrassLayer);
        TerrainToolPathField("##paintGrassMaterial", m_grassMaterial, sizeof(m_grassMaterial));
        OpArgs args;
        args.Set("node", object->instanceId);
        args.Set("terrainLayer", m_biomeGrassLayer);
        args.Set("material", std::string(m_grassMaterial));
        args.Set("density", m_grassDensity);
        args.Set("bladeWidth", m_grassBladeWidth);
        args.Set("lodFar", m_grassDistance);
        ImGui::BeginDisabled(!CanInvokeOperator(context, "terrain.grass.setup", args));
        if (ImGui::Button("Enable Grass Painting"))
            m_grassStatus = TerrainToolOpStatus(InvokeOperator(context, "terrain.grass.setup", args));
        ImGui::EndDisabled();
    } else {
        const int layer = fiber->m_terrainLayer;
        ImGui::Text("Grass layer %d  %s", layer, TerrainToolLayerDisplayName(terrain->layerMaterials[layer]));
        if (ImGui::RadioButton("Grow", !m_grassErase)) m_grassErase = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("Erase", m_grassErase)) m_grassErase = true;
        TerrainToolLayerCombo("Erase Into", terrain, m_grassEraseLayer);
        ImGui::TextWrapped("Grass follows its painted layer. Choose another layer to erase into. Ctrl+drag inverts Grow / Erase.");
    }
    TerrainToolStatus(m_grassStatus);
}

void TerrainTool::DrawHoleContent(
    scene::Scene& scene, UndoStack*, const std::function<void()>&)
{
    ImGui::TextDisabled("Hole Brush");
    if (ImGui::RadioButton("Cut", !m_holeErase)) m_holeErase = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Fill", m_holeErase)) m_holeErase = true;
    ImGui::TextDisabled("Ctrl+drag: invert. Cells whose centre is inside the brush change.");

    const scene::TerrainComponent* referenceTerrain = nullptr;
    if (m_hitTerrain)
        referenceTerrain = m_hitTerrain->GetComponent<scene::TerrainComponent>();
    if (!referenceTerrain)
        referenceTerrain = TerrainToolFirstTerrain(scene, nullptr);
    if (referenceTerrain)
        ImGui::Text("Hole cells: %zu / %zu", referenceTerrain->CountHoles(), referenceTerrain->CellCount());
}

float TerrainTool::BrushRadiusLimit() const
{
    return m_mode == Mode::Paint ? TERRAIN_PAINT_MAX_BRUSH_RADIUS : 50.0f;
}

void TerrainTool::DrawBrushSettings()
{
    if (m_mode == Mode::Paint && m_paintTarget == PaintTarget::HeightMap) return;
    ImGui::Spacing();
    ImGui::SeparatorText("Brush Settings");
    const ImGuiSliderFlags radiusFlags = ImGuiSliderFlags_AlwaysClamp
        | (m_mode == Mode::Paint ? ImGuiSliderFlags_Logarithmic : ImGuiSliderFlags_None);
    ImGui::SliderFloat("Radius", &m_brush.radius, 0.5f, BrushRadiusLimit(), "%.1f", radiusFlags);
    if (m_mode == Mode::Paint && m_paintTarget == PaintTarget::Instances) {
        ImGui::TextDisabled("[ / ] : Radius in meters");
        return;
    }
    ImGui::SliderFloat("Strength", &m_brush.strength, 0.001f, 1.0f, "%.3f");
    ImGui::TextDisabled("[ / ] : Radius    Shift+[ / ] : Strength");
    const char* falloffNames[] = { "Linear", "Smooth", "Gaussian" };
    int falloffIndex = static_cast<int>(m_brush.falloff);
    if (ImGui::Combo("Falloff", &falloffIndex, falloffNames, 3))
        m_brush.falloff = static_cast<FalloffType>(falloffIndex);
}

scene::GameObject* TerrainTool::DrawHeightMapTarget(scene::Scene& scene)
{
    const auto generation = scene.GetRenderSceneGeneration();
    if (m_heightMapSceneGeneration != generation) {
        m_heightMapSceneGeneration = generation;
        m_heightMapTargetId.clear();
        m_heightMapStatus.clear();
        m_heightMapExportStatus.clear();
        m_heightStampStatus.clear();
        m_biomeStatus.clear();
        m_grassStatus.clear();
        m_layerMaskStatus.clear();
        m_scatterStatus.clear();
        m_scatterTargetId.clear();
    }

    scene::GameObject* targetObject = nullptr;
    scene::GameObject* soleTerrain = nullptr;
    size_t terrainCount = 0;
    for (scene::EntityID id : scene.GetEntities<scene::TerrainComponent>()) {
        auto* object = scene.GetGameObject(id);
        const auto* terrain = scene.GetComponent<scene::TerrainComponent>(id);
        if (!object || !terrain || !terrain->enabled) continue;
        soleTerrain = object;
        ++terrainCount;
        if (object->instanceId == m_heightMapTargetId) targetObject = object;
    }
    if (m_heightMapTargetId.empty() && terrainCount == 1) {
        targetObject = soleTerrain;
        m_heightMapTargetId = targetObject->instanceId;
    }

    const std::string preview = targetObject ? TerrainToolObjectLabel(*targetObject)
        : m_heightMapTargetId.empty() ? "Select terrain..." : "Target unavailable";
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##HeightMapTarget", preview.c_str())) {
        for (scene::EntityID id : scene.GetEntities<scene::TerrainComponent>()) {
            auto* object = scene.GetGameObject(id);
            const auto* terrain = scene.GetComponent<scene::TerrainComponent>(id);
            if (!object || !terrain || !terrain->enabled) continue;
            const bool selected = object->instanceId == m_heightMapTargetId;
            ImGui::PushID(object->instanceId.c_str());
            if (ImGui::Selectable(TerrainToolObjectLabel(*object).c_str(), selected)) {
                if(m_heightMapTargetId!=object->instanceId)m_paintArea.clear();
                m_heightMapTargetId = object->instanceId;
                targetObject = object;
                m_heightMapStatus.clear();
                m_heightMapExportStatus.clear();
                m_heightStampStatus.clear();
                m_biomeStatus.clear();
                m_grassStatus.clear();
                m_layerMaskStatus.clear();
                m_scatterStatus.clear();
            }
            if (selected) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (!targetObject) {
        ImGui::TextWrapped(terrainCount == 0 ? "No enabled terrain in this scene."
            : "Choose the terrain to edit.");
        return nullptr;
    }

    const auto* terrain = targetObject->GetComponent<scene::TerrainComponent>();
    ImGui::Text("Resolution: %d x %d", terrain->columns, terrain->rows);
    ImGui::Text("Cell Size: %.3f m   Max Height: %.1f m", terrain->cellSize, terrain->maxHeight);
    ImGui::Text("Local Size: %.1f x %.1f m", (terrain->columns - 1) * terrain->cellSize,
        (terrain->rows - 1) * terrain->cellSize);
    return targetObject;
}

void TerrainTool::DrawImportSection(EditorContext& context, scene::GameObject* targetObject)
{
    if (!ImGui::CollapsingHeader("1. HeightMap Import / Export")) return;
    scene::TerrainComponent* target = targetObject
        ? targetObject->GetComponent<scene::TerrainComponent>() : nullptr;

    ImGui::TextDisabled("File Path (PNG / TGA / DDS)");
    TerrainToolPathField("##hmpath", m_heightMapPath, sizeof(m_heightMapPath));
    ImGui::TextDisabled("Drop an image from the Asset Browser.");
    OpArgs starter;
    starter.Set("node", targetObject ? targetObject->instanceId : std::string{});
    if (m_heightMapPath[0] != '\0') starter.Set("path", std::string(m_heightMapPath));
    ImGui::BeginDisabled(!targetObject || !CanInvokeOperator(context, "terrain.alpine.setup", starter));
    if (ImGui::Button("Alpine Starter (replace terrain)", { -1.0f, 0.0f }))
        m_heightMapStatus = TerrainToolOpStatus(InvokeOperator(context, "terrain.alpine.setup", starter));
    ImGui::EndDisabled();
    ImGui::TextWrapped("Starter: 513 x 513, 4 m cells, 640 m height and grass / rock / snow materials. An empty path uses AlpineRange_Height16.png. Undo restores the terrain.");
    ImGui::RadioButton("Unipolar  [0 → maxH]",    &m_heightMapUnipolar, 1);
    ImGui::RadioButton("Bipolar [-maxH → +maxH]", &m_heightMapUnipolar, 0);
    ImGui::Checkbox("Set terrain size on import", &m_heightMapResize);
    if (m_heightMapResize) {
        ImGui::DragInt("Columns", &m_heightMapColumns, 1.0f, 2, 4097, "%d", ImGuiSliderFlags_AlwaysClamp);
        ImGui::DragInt("Rows", &m_heightMapRows, 1.0f, 2, 4097, "%d", ImGuiSliderFlags_AlwaysClamp);
        ImGui::DragFloat("Cell Size [m]", &m_heightMapCellSize, 0.1f, 0.01f, 100.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::DragFloat("Max Height [m]", &m_heightMapMaxHeight, 1.0f, 0.5f, 10000.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
    }
    ImGui::TextWrapped("The image is resampled to the terrain resolution. Import replaces heights; Undo restores them.");
    ImGui::Spacing();

    OpArgs args;
    args.Set("node", targetObject ? targetObject->instanceId : std::string{});
    args.Set("path", std::string(m_heightMapPath));
    args.Set("unipolar", m_heightMapUnipolar != 0);
    if (m_heightMapResize) {
        args.Set("columns", m_heightMapColumns);
        args.Set("rows", m_heightMapRows);
        args.Set("cellSize", m_heightMapCellSize);
        args.Set("maxHeight", m_heightMapMaxHeight);
    }
    const bool canImport = target && m_heightMapPath[0] != '\0' && CanInvokeOperator(context, "terrain.heightmap.import", args);
    if (!canImport) ImGui::BeginDisabled();
    if (ImGui::Button("Import into Terrain", { -1.0f, 0.0f })) {
        m_heightMapStatus = TerrainToolOpStatus(InvokeOperator(context, "terrain.heightmap.import", args));
    }
    if (!canImport) ImGui::EndDisabled();
    TerrainToolStatus(m_heightMapStatus);

    ImGui::Spacing();
    ImGui::TextDisabled("Export Path (16-bit PNG)");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##hmexport", m_heightMapExport, sizeof(m_heightMapExport));
    const bool canExport = target && m_heightMapExport[0] != '\0';
    if (!canExport) ImGui::BeginDisabled();
    if (ImGui::Button("Export Heightmap", { -1.0f, 0.0f })) {
        std::string path = m_heightMapExport;
        /// @note 出力は常に PNG なので、拡張子が違えば付け足す (WIC は拡張子で形式を決めないが、読み戻す側が拡張子を見る)。
        if (!(path.ends_with(".png") || path.ends_with(".PNG")))
            path += ".png";
        if (!target)
            m_heightMapExportStatus = "Error: Select a terrain";
        else if (scene::SaveHeightMapToFile(path, *target, m_heightMapUnipolar != 0))
            m_heightMapExportStatus = "OK: " + path;
        else
            m_heightMapExportStatus = "Error: Save failed";
    }
    if (!canExport) ImGui::EndDisabled();
    TerrainToolStatus(m_heightMapExportStatus);
}

void TerrainTool::DrawBiomeSection(EditorContext& context, scene::GameObject* targetObject)
{
    const auto* terrain = targetObject ? targetObject->GetComponent<scene::TerrainComponent>() : nullptr;
    if (ImGui::CollapsingHeader("2. Grass / Rock / Snow")) {
        ImGui::PushID("Biome");
        ImGui::TextWrapped("Bake height and slope rules into editable terrain layers. Paint can refine the result, and grass can follow the grass layer.");
        TerrainToolLayerCombo("Grass Layer", terrain, m_biomeGrassLayer);
        TerrainToolLayerCombo("Rock Layer", terrain, m_biomeRockLayer);
        TerrainToolLayerCombo("Snow Layer", terrain, m_biomeSnowLayer);
        ImGui::DragFloat("Snow Height [world m]", &m_biomeSnowHeight, 1.0f);
        ImGui::DragFloat("Snow Fade [m]", &m_biomeSnowFade, 1.0f, 0.01f, 10000.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SliderFloat("Rock Slope [deg]", &m_biomeRockSlope, 0.0f, 90.0f);
        ImGui::SliderFloat("Slope Fade [deg]", &m_biomeSlopeFade, 0.01f, 90.0f);
        if (terrain && terrain->LayerCount() < 3)
            ImGui::TextWrapped("Assign three terrain materials in the Inspector before baking.");
        OpArgs args;
        args.Set("node", targetObject ? targetObject->instanceId : std::string{});
        args.Set("grassLayer", m_biomeGrassLayer);
        args.Set("rockLayer", m_biomeRockLayer);
        args.Set("snowLayer", m_biomeSnowLayer);
        args.Set("snowHeight", m_biomeSnowHeight);
        args.Set("snowFade", m_biomeSnowFade);
        args.Set("rockSlope", m_biomeRockSlope);
        args.Set("slopeFade", m_biomeSlopeFade);
        ImGui::BeginDisabled(!targetObject || !CanInvokeOperator(context, "terrain.biome.bake", args));
        if (ImGui::Button("Bake Terrain Layers", { -1.0f, 0.0f }))
            m_biomeStatus = TerrainToolOpStatus(InvokeOperator(context, "terrain.biome.bake", args));
        ImGui::EndDisabled();
        TerrainToolStatus(m_biomeStatus);
        ImGui::SeparatorText("Grass on the Grass Layer");
        ImGui::TextDisabled("Grass material (.mat)");
        TerrainToolPathField("##grassMaterial", m_grassMaterial, sizeof(m_grassMaterial));
        ImGui::SliderFloat("Grass Density [/local m2]", &m_grassDensity, 0.001f, 8.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
        ImGui::DragFloat("Grass Width [local m]", &m_grassBladeWidth, 0.001f, 0.001f, 0.5f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::DragFloat("Grass Distance [world m]", &m_grassDistance, 1.0f, 1.0f, 2000.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::TextWrapped("Grass is created around each view. Display distance and density control the local budget (up to 500,000 candidates per view).");
        OpArgs grass;
        grass.Set("node", targetObject ? targetObject->instanceId : std::string{});
        grass.Set("material", std::string(m_grassMaterial));
        grass.Set("terrainLayer", m_biomeGrassLayer);
        grass.Set("density", m_grassDensity);
        grass.Set("bladeWidth", m_grassBladeWidth);
        grass.Set("lodFar", m_grassDistance);
        ImGui::BeginDisabled(!targetObject || !CanInvokeOperator(context, "terrain.grass.setup", grass));
        if (ImGui::Button("Enable Grass on Layer", { -1.0f, 0.0f })) {
            const auto result = InvokeOperator(context, "terrain.grass.setup", grass);
            m_grassStatus = TerrainToolOpStatus(result);
            if (result.ok)
                if (const auto* count = result.data.Find("candidateWindowUpperBound"))
                    m_grassStatus += " (up to " + std::to_string(count->AsInt()) + " candidates per view)";
        }
        ImGui::EndDisabled();
        TerrainToolStatus(m_grassStatus);
        ImGui::PopID();
    }
    if (ImGui::CollapsingHeader("Layer Mask PNG")) {
        ImGui::PushID("LayerMask");
        ImGui::TextWrapped("White paints the chosen layer; black keeps existing weights. Use a forest or meadow mask to control placement.");
        TerrainToolPathField("##maskPath", m_layerMaskPath, sizeof(m_layerMaskPath));
        TerrainToolLayerCombo("Layer", terrain, m_layerMaskLayer);
        ImGui::SliderFloat("Strength", &m_layerMaskStrength, 0.0f, 1.0f);
        ImGui::Checkbox("Invert", &m_layerMaskInvert);
        OpArgs args;
        args.Set("node", targetObject ? targetObject->instanceId : std::string{});
        args.Set("path", std::string(m_layerMaskPath));
        args.Set("layer", m_layerMaskLayer);
        args.Set("strength", m_layerMaskStrength);
        args.Set("invert", m_layerMaskInvert);
        ImGui::BeginDisabled(!targetObject || m_layerMaskPath[0] == '\0'
            || !CanInvokeOperator(context, "terrain.layer.import_mask", args));
        if (ImGui::Button("Import Layer Mask", { -1.0f, 0.0f }))
            m_layerMaskStatus = TerrainToolOpStatus(InvokeOperator(context, "terrain.layer.import_mask", args));
        ImGui::EndDisabled();
        TerrainToolStatus(m_layerMaskStatus);
        ImGui::PopID();
    }
}

void TerrainTool::DrawPaintAssetInspector(EditorContext& context, const std::string& path)
{
    m_paintLibrary.DrawInspector(context, path);
}

void TerrainTool::OpenPaintAssetCreation(const std::string& directory)
{
    m_active = true;
    m_mode = Mode::Paint; m_paintTarget = PaintTarget::Instances;
    m_paintLibrary.OpenCreate(directory);
    m_focusPaintPalette = true;
}

TerrainPaintMix TerrainTool::CapturePaintMix(const std::string& name) const
{
    TerrainPaintMix mix;
    mix.name = name; mix.radius = m_brush.radius; mix.avoidWater = m_paintAvoidWater; mix.excludedLayer = m_paintExcludedLayer;
    mix.avoidPaths = m_paintAvoidPaths;mix.excludedMaterials=m_paintExcludedMaterials;mix.feather=m_paintFeather;
    for (const auto& source : m_paintPalette) mix.sources.push_back({source.reference, source.weight});
    auto& p = mix.settings;
    p.spacing = m_scatterSpacing; p.density = m_scatterDensity; p.seed = static_cast<uint32_t>(m_scatterSeed);
    p.minHeight = m_scatterMinHeight; p.maxHeight = m_scatterMaxHeight;
    p.minSlope = m_scatterMinSlope; p.maxSlope = m_scatterMaxSlope; p.layer = m_scatterLayer;
    p.layerThreshold = m_scatterLayerThreshold; p.minScale = m_scatterMinScale; p.maxScale = m_scatterMaxScale;
    p.minYaw = m_scatterMinYaw; p.maxYaw = m_scatterMaxYaw;
    p.alignToNormal = m_scatterOrientation == 1; p.alignDownslope = m_scatterOrientation == 2;
    p.downslopeJitter = m_scatterDownslopeJitter; p.embedDepth = m_scatterEmbedDepth;
    p.maxInstances = static_cast<uint32_t>(m_scatterMaxInstances); p.minimumDistance = m_paintMinimumDistance;
    return mix;
}

void TerrainTool::ApplyPaintMix(const TerrainPaintMix& mix)
{
    std::vector<std::string> sources;
    for (const auto& source : mix.sources) sources.push_back(source.reference);
    SetPaintPalette(std::move(sources), false);
    for (size_t i = 0; i < m_paintPalette.size(); ++i) m_paintPalette[i].weight = mix.sources[i].weight;
    m_paintMixName = mix.name; m_brush.radius = mix.radius; m_paintAvoidWater = mix.avoidWater; m_paintExcludedLayer = mix.excludedLayer;
    m_paintAvoidPaths = mix.avoidPaths;m_paintExcludedMaterials=mix.excludedMaterials;m_paintFeather=mix.feather;
    const auto& p = mix.settings;
    m_scatterSpacing = p.spacing; m_scatterDensity = p.density; m_scatterSeed = static_cast<int>(p.seed);
    m_scatterMinHeight = p.minHeight; m_scatterMaxHeight = p.maxHeight;
    m_scatterMinSlope = p.minSlope; m_scatterMaxSlope = p.maxSlope; m_scatterLayer = p.layer;
    m_scatterLayerThreshold = p.layerThreshold; m_scatterMinScale = p.minScale; m_scatterMaxScale = p.maxScale;
    m_scatterMinYaw = p.minYaw; m_scatterMaxYaw = p.maxYaw;
    m_scatterOrientation = p.alignDownslope ? 2 : p.alignToNormal ? 1 : 0;
    m_scatterDownslopeJitter = p.downslopeJitter; m_scatterEmbedDepth = p.embedDepth;
    m_scatterMaxInstances = static_cast<int>(p.maxInstances); m_paintMinimumDistance = p.minimumDistance;
    m_nextPaintPreviewTime = 0.0;
}

void TerrainTool::DrawInstancePaint(EditorContext& context, scene::GameObject* terrain)
{
    DrawInstanceWorkspace(context,terrain);
    std::vector<std::string> selected, next;
    for (const auto& source : m_paintPalette) selected.push_back(source.reference);
    if(ImGui::CollapsingHeader("Add / change assets",m_paintPalette.empty()?ImGuiTreeNodeFlags_DefaultOpen:0))
        if (m_paintLibrary.Draw(context, selected, next)) SetPaintPalette(std::move(next), false);
    ImGui::SeparatorText("Saved Mixes");
    AssetSearch::SetProjectRoot(context.projectRoot);
    std::string useMix;
    if (ImGui::BeginCombo("Load Mix", m_paintMixName.c_str())) {
        for (const auto& entry : AssetSearch::Entries()) {
            if (entry.extension != ".terrainmix") continue;
            TerrainPaintMix mix;
            if(!LoadTerrainPaintMix(entry.absolutePath,mix))continue;
            std::vector<std::string> refs;std::vector<float> weights;
            for(const auto& source:mix.sources){refs.push_back(source.reference);weights.push_back(source.weight);}
            ImGui::PushID(entry.absolutePath.c_str());
            if(void* image=m_paintLibrary.MixThumbnail(context,entry.absolutePath,refs,weights))ImGui::Image(widgets::ToImTextureID(image),{64,64});
            else ImGui::Dummy({64,64});
            ImGui::SameLine();
            if (ImGui::Selectable(mix.name.c_str(), m_paintMixPath == entry.relativePath,0,{0,64})) useMix = entry.relativePath;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (!useMix.empty()) {
        OpArgs args; args.Set("path", useMix);
        const auto result = InvokeOperator(context, "terrain.paint_mix.use", args);
        m_scatterStatus = TerrainToolOpStatus(result);
        if (result.ok) m_paintMixPath = useMix;
    }
    widgets::InputString("Mix Name", m_paintMixName);
    ImGui::BeginDisabled(m_paintPalette.empty() || m_paintMixName.empty());
    if (ImGui::Button("Save Mix")) {
        std::string filename = m_paintMixName;
        for (char& c : filename) if (static_cast<unsigned char>(c) < 32 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos) c = '_';
        m_paintMixPath = "Assets/Terrain/Paint/Mixes/" + filename + ".terrainmix";
        OpArgs args; args.Set("path", m_paintMixPath); args.Set("name", m_paintMixName);
        m_scatterStatus = TerrainToolOpStatus(InvokeOperator(context, "terrain.paint_mix.save", args));
    }
    ImGui::EndDisabled();
    ImGui::SeparatorText("Placement Palette");
    if (m_paintPalette.empty()) {
        ImGui::TextWrapped("Choose a thumbnail above, or create a Paint asset from a mesh and material.");
        return;
    }
    float totalWeight=0;for(const auto& source:m_paintPalette)totalWeight+=source.weight;
    size_t removeIndex=m_paintPalette.size();
    ImGui::TextDisabled("Weight controls the mix. Counts also depend on each asset's spacing.");
    if(ImGui::BeginChild("##ActivePaintAssets",{0,std::min(260.0f,static_cast<float>(m_paintPalette.size())*88.0f)},true)) {
        for(size_t index=0;index<m_paintPalette.size();++index){
            auto& source=m_paintPalette[index];const auto* paint=IsTerrainPaintReference(source.reference)?asset::AssetManager::Get(asset::AssetManager::Load<asset::TerrainPaintAsset>(source.reference)):nullptr;
            const auto path=asset::AssetManager::ResolveAssetPath(source.reference);
            const std::string name=paint?paint->name:util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(path).stem());
            ImGui::PushID(static_cast<int>(index));
            if(ImGui::BeginTable("##SourceCard",2,ImGuiTableFlags_SizingStretchProp)){
                ImGui::TableSetupColumn("Preview",ImGuiTableColumnFlags_WidthFixed,64);ImGui::TableSetupColumn("Settings");ImGui::TableNextRow();ImGui::TableNextColumn();
                if(void* image=m_paintLibrary.Thumbnail(context,source.reference))ImGui::Image(widgets::ToImTextureID(image),{64,64});else ImGui::Dummy({64,64});
                ImGui::TableNextColumn();ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(TerrainPaintColor(index)),"%s",name.c_str());
                ImGui::SameLine();if(ImGui::SmallButton("x##RemovePaintSource"))removeIndex=index;
                ImGui::SetNextItemWidth(-1);
                if(ImGui::SliderFloat("##weight",&source.weight,0,100,"Weight %.0f",ImGuiSliderFlags_AlwaysClamp))m_nextPaintPreviewTime=0;
                ImGui::TextDisabled("Mix %.0f%%    Preview %zu",totalWeight>0?source.weight/totalWeight*100:0,m_paintDiagnostics.sources[index]);
                ImGui::EndTable();
            }
            ImGui::PopID();
        }
    }ImGui::EndChild();
    if (removeIndex < m_paintPalette.size()) {
        m_paintPalette.erase(m_paintPalette.begin() + static_cast<std::ptrdiff_t>(removeIndex));
        m_scatterStroke.clear();
        m_paintPreview.clear();
        m_paintPreviewInputs.clear();
        m_paintPreviewTotal = 0;
        m_paintPreviewError.clear();
        m_nextPaintPreviewTime = 0.0;
    }
    if (ImGui::Checkbox("Live Preview", &m_showPaintPreview)) m_nextPaintPreviewTime = 0.0;
    ImGui::SameLine();
    if (ImGui::SmallButton("Equal Weights")) {
        for (auto& source : m_paintPalette) source.weight = 100.0f;
        m_nextPaintPreviewTime = 0.0;
    }
    if (m_showPaintPreview && !m_paintPreviewTerrainId.empty()) {
        if (!m_paintPreviewError.empty()) TerrainToolStatus("Error: " + m_paintPreviewError);
        else if (m_paintPreviewTotal > m_paintPreview.size())
            ImGui::TextDisabled("Preview: %zu placements (%zu models shown)", m_paintPreviewTotal, m_paintPreview.size());
        else ImGui::TextDisabled("Preview: %zu placements", m_paintPreviewTotal);
    }
    ImGui::Spacing();
    if (ImGui::RadioButton("Add", !m_scatterErase)) m_scatterErase = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Erase", m_scatterErase)) m_scatterErase = true;
    if(m_instanceMode==0)ImGui::TextWrapped(terrain ? "Drag on the terrain. Ctrl+drag switches Add / Erase."
                               : "Choose a terrain to begin painting.");
    const bool native = std::any_of(m_paintPalette.begin(), m_paintPalette.end(), [](const PaintSource& source) {
        return IsTerrainPaintReference(source.reference);
    });
    if (!native) ImGui::DragFloat("Spacing [m]", &m_scatterSpacing, 0.1f, 0.25f, 10000.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SliderFloat(native ? "Density Multiplier" : "Density", &m_scatterDensity, 0.0f, 1.0f);
    if(ImGui::CollapsingHeader("Placement boundaries")) {
        ImGui::Checkbox("Avoid Water", &m_paintAvoidWater);ImGui::Checkbox("Avoid Paths", &m_paintAvoidPaths);
        ImGui::SetItemTooltip("Named Path / Road / Trail layers are detected. Choose any additional ground materials below.");
        ImGui::DragFloat("Minimum Separation [m]", &m_paintMinimumDistance, 0.1f, 0.0f, 1000.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SliderFloat("Soft edge",&m_paintFeather,0,1,"%.2f");
        const auto* ground=terrain?terrain->GetComponent<scene::TerrainComponent>():nullptr;
        if(ground&&m_paintExcludedLayer>=0&&m_paintExcludedLayer<ground->LayerCount()){
            m_paintExcludedMaterials.push_back(ground->layerMaterials[m_paintExcludedLayer]);m_paintExcludedLayer=-1;
        }
        ImGui::TextDisabled("Avoid these ground materials:");
        for(int i=0;ground&&i<ground->LayerCount();++i){
            const auto& reference=ground->layerMaterials[i];if(reference.empty())continue;
            auto found=std::find(m_paintExcludedMaterials.begin(),m_paintExcludedMaterials.end(),reference);bool checked=found!=m_paintExcludedMaterials.end();
            ImGui::PushID(i);
            if(void* image=m_paintLibrary.GroundThumbnail(context,reference)){ImGui::Image(widgets::ToImTextureID(image),{32,32});ImGui::SameLine();}
            const auto path=asset::AssetManager::ResolveAssetPath(reference);const auto name=util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(path).stem());
            if(ImGui::Checkbox(name.empty()?reference.c_str():name.c_str(),&checked)){
                if(checked)m_paintExcludedMaterials.push_back(reference);else if(found!=m_paintExcludedMaterials.end())m_paintExcludedMaterials.erase(found);m_nextPaintPreviewTime=0;
            }
            ImGui::PopID();
        }
        ImGui::TextWrapped("Water / path clearance is set per asset in Edit Paint Asset.");
    }
    ImGui::Checkbox("Show placement reasons",&m_paintShowDiagnostics);
    DrawPaintDiagnosticSummary();
    if (native) {
        ImGui::TextWrapped("Each Paint asset supplies its spacing, scale, orientation and slope rules.");
        if (ImGui::CollapsingHeader("Area Filters")) {
            ImGui::DragInt("Instance Limit",&m_scatterMaxInstances,100,1,1000000,"%d",ImGuiSliderFlags_AlwaysClamp);
            ImGui::DragFloatRange2("Height [world m]", &m_scatterMinHeight, &m_scatterMaxHeight, 1.0f, -100000.0f, 100000.0f);
            ImGui::DragInt("Seed", &m_scatterSeed, 1.0f, 0, (std::numeric_limits<int>::max)(), "%d", ImGuiSliderFlags_AlwaysClamp);
        }
    } else if (ImGui::CollapsingHeader("Placement Rules")) DrawScatterSettings(terrain);
    DrawInstanceAdjustments(context,terrain);
    TerrainToolStatus(m_scatterStatus);
}

void TerrainTool::DrawScatterSection(EditorContext& context, scene::GameObject* terrainObject)
{
    if (!ImGui::CollapsingHeader("3. Trees / Rocks Placement")) return;
    ImGui::PushID("Scatter");
    auto& scene = *context.activeScene;
    scene::GameObject* targetObject = scene.FindByGuid(m_scatterTargetId);
    const std::string preview = targetObject ? TerrainToolObjectLabel(*targetObject) : "Select placement object...";
    if (ImGui::BeginCombo("Placement Object", preview.c_str())) {
        for (auto& object : scene.GameObjects()) {
            ImGui::PushID(object.instanceId.c_str());
            if (ImGui::Selectable(TerrainToolObjectLabel(object).c_str(), object.instanceId == m_scatterTargetId)) {
                m_scatterTargetId = object.instanceId;
                targetObject = &object;
                m_scatterStatus.clear();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    auto* selected = context.GetSelectedGO();
    ImGui::BeginDisabled(selected == nullptr);
    if (ImGui::SmallButton("Use Selected Object")) {
        targetObject = selected;
        m_scatterTargetId = selected->instanceId;
        m_scatterStatus.clear();
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Model template (.terrainpaint / .meshinstances)");
    TerrainToolPathField("##scatterSource", m_scatterSource, sizeof(m_scatterSource));
    if (targetObject) {
        const auto* instances = targetObject->GetComponent<scene::InstancedMeshComponent>();
        if (instances && !instances->m_instanceSetRef.path.empty())
            ImGui::TextWrapped("Current template: %s", instances->m_instanceSetRef.path.c_str());
    }
    ImGui::TextWrapped("Choose a model template, or leave it blank to reuse this object's current template. Baking replaces this object's placements and supports Undo.");
    ImGui::DragFloat("Spacing [world m]", &m_scatterSpacing, 0.1f, 0.25f, 10000.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SliderFloat("Density", &m_scatterDensity, 0.0f, 1.0f);
    DrawScatterSettings(terrainObject);
    ImGui::DragInt("Placements per 128 m Cell", &m_scatterMaxPlacementsPerCell, 1.0f, 0, 1000000,
        "%d", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("0 leaves the number of placements per cell unlimited.");
    auto args = ScatterArguments(terrainObject ? terrainObject->instanceId : std::string{},
        targetObject ? targetObject->instanceId : std::string{});
    args.Set("maxPlacementsPerCell", m_scatterMaxPlacementsPerCell);
    ImGui::BeginDisabled(!terrainObject || !targetObject || !CanInvokeOperator(context, "terrain.scatter", args));
    if (ImGui::Button("Bake Placements", {-1.0f, 0.0f}))
        m_scatterStatus = TerrainToolOpStatus(InvokeOperator(context, "terrain.scatter", args));
    ImGui::EndDisabled();
    TerrainToolStatus(m_scatterStatus);
    ImGui::PopID();
}

void TerrainTool::DrawScatterSettings(scene::GameObject* terrainObject)
{
    ImGui::DragInt("Seed", &m_scatterSeed, 1.0f, 0, (std::numeric_limits<int>::max)(), "%d", ImGuiSliderFlags_AlwaysClamp);
    ImGui::DragFloatRange2("Height [world m]", &m_scatterMinHeight, &m_scatterMaxHeight, 1.0f, -100000.0f, 100000.0f);
    ImGui::DragFloatRange2("Slope [deg]", &m_scatterMinSlope, &m_scatterMaxSlope, 0.5f, 0.0f, 90.0f);
    const auto* terrain = terrainObject ? terrainObject->GetComponent<scene::TerrainComponent>() : nullptr;
    TerrainToolLayerCombo("Placement Layer", terrain, m_scatterLayer, true);
    if (m_scatterLayer >= 0) ImGui::SliderFloat("Layer Threshold", &m_scatterLayerThreshold, 0.0f, 1.0f);
    ImGui::DragFloatRange2("Scale", &m_scatterMinScale, &m_scatterMaxScale, 0.01f, 0.01f, 100.0f);
    static constexpr const char* ORIENTATIONS[] = {"Upright", "Surface Normal", "Downslope"};
    ImGui::Combo("Orientation", &m_scatterOrientation, ORIENTATIONS, IM_ARRAYSIZE(ORIENTATIONS));
    if (m_scatterOrientation == 2)
        ImGui::DragFloat("Downslope Jitter [deg]", &m_scatterDownslopeJitter, 0.5f, 0.0f, 180.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::DragFloatRange2("Yaw [deg]", &m_scatterMinYaw, &m_scatterMaxYaw, 1.0f, -360.0f, 360.0f);
    if (m_scatterOrientation == 2) ImGui::SetItemTooltip("Yaw is used on nearly flat ground.");
    ImGui::DragFloat("Embed Depth [world m]", &m_scatterEmbedDepth, 0.01f, 0.0f, 100.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::DragInt("Instance Limit", &m_scatterMaxInstances, 100.0f, 1, 1000000, "%d", ImGuiSliderFlags_AlwaysClamp);

}

OpArgs TerrainTool::ScatterArguments(const std::string& terrainId, const std::string& targetId) const
{
    OpArgs args;
    args.Set("terrain", terrainId);
    if (!targetId.empty()) {
        args.Set("target", targetId);
        if (m_scatterSource[0] != '\0') args.Set("source", std::string(m_scatterSource));
    }
    args.Set("spacing", m_scatterSpacing);
    args.Set("density", m_scatterDensity);
    args.Set("seed", m_scatterSeed);
    args.Set("minHeight", m_scatterMinHeight);
    args.Set("maxHeight", m_scatterMaxHeight);
    args.Set("minSlope", m_scatterMinSlope);
    args.Set("maxSlope", m_scatterMaxSlope);
    args.Set("layer", m_scatterLayer);
    args.Set("layerThreshold", m_scatterLayerThreshold);
    args.Set("minScale", m_scatterMinScale);
    args.Set("maxScale", m_scatterMaxScale);
    args.Set("minYaw", m_scatterMinYaw);
    args.Set("maxYaw", m_scatterMaxYaw);
    args.Set("alignToNormal", m_scatterOrientation == 1);
    args.Set("alignDownslope", m_scatterOrientation == 2);
    args.Set("downslopeJitter", m_scatterDownslopeJitter);
    args.Set("embedDepth", m_scatterEmbedDepth);
    args.Set("maxInstances", m_scatterMaxInstances);
    return args;
}

void TerrainTool::DrawAuthoringSection(EditorContext& context)
{
    if (!context.activeScene || !ImGui::CollapsingHeader("Terrain Authoring", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::PushID("TerrainAuthoring");
    ImGui::TextDisabled("Target Terrain");
    auto* targetObject = DrawHeightMapTarget(*context.activeScene);
    DrawImportSection(context, targetObject);
    DrawBiomeSection(context, targetObject);
    DrawScatterSection(context, targetObject);
    ImGui::PopID();
}

void TerrainTool::OnEditorGUI(EditorContext& context)
{
    if (!context.activeScene) return;
    auto& scene = *context.activeScene;
    auto* undoStack = context.undoStack;
    const auto& markDirty = context.markSceneDirty;

    const ImGuiViewport* mainVP = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        { mainVP->WorkPos.x + mainVP->WorkSize.x - 230.0f,
          mainVP->WorkPos.y + mainVP->WorkSize.y - 10.0f },
        ImGuiCond_FirstUseEver, { 1.0f, 1.0f });
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::SetNextWindowSize({ 260.0f, 0.0f }, ImGuiCond_FirstUseEver);
    if (m_focusPaintPalette) {
        ImGui::SetNextWindowFocus();
        m_focusPaintPalette = false;
    }
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;
    const char* windowTitle = m_active ? "Terrain Tool###TerrainTool" : "Terrain Tool [OFF]###TerrainTool";
    if (!ImGui::Begin(windowTitle, nullptr, kFlags)) { ImGui::End(); return; }

    ImGui::PushStyleColor(ImGuiCol_Button, m_active
        ? ImVec4(0.2f, 0.6f, 0.2f, 1.0f) : ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
    if (ImGui::Button(m_active ? "  Active  " : " Inactive ", { -1.0f, 0.0f }))
        m_active = !m_active;
    ImGui::PopStyleColor();
    if (!m_active) ImGui::BeginDisabled();

    {
        struct ModeButton { Mode mode; const char* label; ThemeColor color; };
        constexpr ModeButton kModes[] = {
            { Mode::Sculpt, "Sculpt", ThemeColor::Success },
            { Mode::Paint,  "Paint",  ThemeColor::AccentActive },
            { Mode::Hole,   "Hole",   ThemeColor::Secondary },
        };
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float width = (ImGui::GetContentRegionAvail().x - spacing * 2.0f) / 3.0f;
        for (size_t i = 0; i < std::size(kModes); ++i) {
            if (i > 0) ImGui::SameLine();
            const bool active = m_mode == kModes[i].mode;
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(kModes[i].color));
            if (ImGui::Button(kModes[i].label, { width, 0.0f })) m_mode = kModes[i].mode;
            if (active) ImGui::PopStyleColor();
        }
    }
    ImGui::Spacing();

    switch (m_mode) {
        case Mode::Sculpt: DrawSculptContent(scene, undoStack, markDirty); break;
        case Mode::Paint:  DrawPaintContent(context); break;
        case Mode::Hole:   DrawHoleContent(scene, undoStack, markDirty);   break;
    }

    DrawBrushSettings();

    if (!m_active) ImGui::EndDisabled();
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    DrawAuthoringSection(context);
    ImGui::End();
}

} /// @note namespace fbzz::editor
