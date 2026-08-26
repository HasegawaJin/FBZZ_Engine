/// @file    NavigationPanel.cpp
/// @brief   NavMesh Surface 一覧・一括ベイク・ベイク診断・オーバーレイ設定の ImGui 実装。
/// @author  Hasegawa Jin
/// @date    2026-08-23
#include <Editor/Panels/NavigationPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/Selection.hpp>
#include <Editor/Util/Toast.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/NavMeshAgentComponent.hpp>
#include <Engine/Scene/Components/NavMeshModifierComponent.hpp>
#include <Engine/Scene/Components/NavMeshOffMeshLinkComponent.hpp>
#include <Engine/Scene/Components/NavMeshSurfaceComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Systems/NavMeshBakeSystem.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

namespace fbzz::editor {

namespace {

constexpr ImVec4 kWarnColor = { 1.00f, 0.72f, 0.20f, 1.00f };
constexpr ImVec4 kErrColor  = { 1.00f, 0.42f, 0.35f, 1.00f };
constexpr ImVec4 kOkColor   = { 0.45f, 0.90f, 0.55f, 1.00f };
constexpr ImVec4 kDimColor  = { 0.62f, 0.62f, 0.62f, 1.00f };

// ハッシュ再計算の間隔 [秒]。Terrain の heightData 全体を畳むので毎フレームは回さない。
constexpr double kStaleCheckInterval = 0.5;

const char* kDrawModeNames[] = { "Solid", "Transparent", "Areas", "Portals", "Voxels" };

// 描画モードごとの凡例。色の意味を絵の外に置かないと、Voxels の 5 色は当てずっぽうになる。
void DrawLegend(renderer::NavMeshDrawMode mode)
{
    struct Entry { ImVec4 color; const char* text; };
    static const Entry kSolid[] = {
        { { 0.30f, 0.62f, 0.98f, 1.0f }, "歩行可能面" },
        { { 1.00f, 0.58f, 0.12f, 1.0f }, "外周エッジ (NavMesh の縁・穴)" },
        { { 0.85f, 0.40f, 1.00f, 1.0f }, "Off-Mesh Link" },
    };
    static const Entry kAreas[] = {
        { { 0.16f, 0.52f, 0.95f, 1.0f }, "Area 0 (既定)" },
        { { 0.30f, 0.85f, 0.40f, 1.0f }, "Area 1" },
        { { 0.95f, 0.75f, 0.20f, 1.0f }, "Area 2" },
        { { 0.90f, 0.35f, 0.30f, 1.0f }, "Area 3" },
    };
    static const Entry kPortals[] = {
        { { 0.25f, 1.00f, 0.85f, 1.0f }, "Portal (隣接ポリゴンと共有する辺)" },
        { { 1.00f, 0.58f, 0.12f, 1.0f }, "外周エッジ (接続なし)" },
    };
    static const Entry kVoxels[] = {
        { { 0.20f, 0.75f, 0.35f, 1.0f }, "歩行可" },
        { { 0.95f, 0.60f, 0.10f, 1.0f }, "急斜面 (Max Slope 超え)" },
        { { 0.90f, 0.35f, 0.85f, 1.0f }, "段差 (Max Climb 超え)" },
        { { 0.92f, 0.18f, 0.18f, 1.0f }, "障害物 (NotWalkable Modifier)" },
        { { 0.25f, 0.55f, 0.95f, 1.0f }, "半径不足 (Agent Radius で削られた)" },
    };

    const Entry* entries = kSolid;
    int count = static_cast<int>(std::size(kSolid));
    switch (mode) {
    case renderer::NavMeshDrawMode::Areas:
        entries = kAreas; count = static_cast<int>(std::size(kAreas)); break;
    case renderer::NavMeshDrawMode::Portals:
        entries = kPortals; count = static_cast<int>(std::size(kPortals)); break;
    case renderer::NavMeshDrawMode::Voxels:
        entries = kVoxels; count = static_cast<int>(std::size(kVoxels)); break;
    default: break;
    }

    for (int i = 0; i < count; ++i) {
        ImGui::ColorButton("##legend", entries[i].color,
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                           ImVec2(12.0f, 12.0f));
        ImGui::SameLine();
        ImGui::TextColored(kDimColor, "%s", entries[i].text);
    }
}

const char* BakeStateLabel(const scene::NavMeshSurfaceComponent& surface, ImVec4& outColor)
{
    if (surface.bakeState == scene::NavMeshBakeState::Baking) { outColor = kWarnColor; return "Baking"; }
    if (surface.needsBake)                                    { outColor = kWarnColor; return "Queued"; }
    if (!surface.bakeStats.failReason.empty())                { outColor = kErrColor;  return "Failed"; }
    if (surface.navMesh.IsValid())                            { outColor = kOkColor;   return "Baked"; }
    outColor = kDimColor;
    return "Not baked";
}

// Agent Radius が Cell Size に対して小さすぎると侵食が 1 セルも起きない。
// Recast と同じく削り幅は整数セル単位なので、設定した値が黙って無視される。
bool ErosionIsInactive(const scene::NavMeshSurfaceComponent& surface)
{
    const float cellSize = std::max(0.1f, surface.cellSize);
    return static_cast<int>((std::max(0.0f, surface.agentRadius) / cellSize) * 2.0f) <= 0;
}

} // namespace

void NavigationPanel::OnRenderContent(EditorContext& ctx)
{
    if (!ctx.HasActiveScene()) {
        ImGui::TextColored(kDimColor, "シーンが開かれていません。");
        return;
    }

    DrawToolbar(ctx);
    ImGui::Separator();
    DrawOverlaySettings(ctx);
    ImGui::Separator();
    RefreshStaleCache(ctx);
    DrawSurfaceList(ctx);
    ImGui::Separator();
    DrawAgentDiagnostics(ctx);
}

void NavigationPanel::DrawToolbar(EditorContext& ctx)
{
    scene::Scene& scene = *ctx.activeScene;

    int surfaceCount = 0;
    int bakingCount  = 0;
    for (scene::EntityID eid : scene.GetEntities<scene::NavMeshSurfaceComponent>()) {
        auto* surface = scene.GetComponent<scene::NavMeshSurfaceComponent>(eid);
        if (!surface) continue;
        ++surfaceCount;
        if (surface->bakeState == scene::NavMeshBakeState::Baking || surface->needsBake)
            ++bakingCount;
    }

    ImGui::BeginDisabled(surfaceCount == 0 || bakingCount > 0);
    if (ImGui::Button("Bake All")) {
        int queued = 0;
        for (scene::EntityID eid : scene.GetEntities<scene::NavMeshSurfaceComponent>()) {
            auto* surface = scene.GetComponent<scene::NavMeshSurfaceComponent>(eid);
            if (!surface || !surface->enabled) continue;
            surface->needsBake = true;
            ++queued;
        }
        Toast::Info(std::to_string(queued) + " 個の NavMesh Surface をベイクします");
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("シーン内の有効な NavMesh Surface をすべて再ベイクします。");

    ImGui::SameLine();
    if (bakingCount > 0) ImGui::TextColored(kWarnColor, "%d / %d baking...", bakingCount, surfaceCount);
    else                 ImGui::TextColored(kDimColor, "%d surface(s)", surfaceCount);
}

void NavigationPanel::DrawOverlaySettings(EditorContext& ctx)
{
    auto& render = ctx.projectSettings.render;

    ImGui::Checkbox("Show NavMesh", &render.showNavMesh);
    ImGui::SameLine();
    ImGui::Checkbox("AI Sensors", &render.showNavSensors);

    ImGui::BeginDisabled(!render.showNavMesh);
    int mode = static_cast<int>(render.navMeshDrawMode);
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::Combo("Draw Mode", &mode, kDrawModeNames, static_cast<int>(std::size(kDrawModeNames))))
        render.navMeshDrawMode = static_cast<renderer::NavMeshDrawMode>(mode);
    ImGui::SetItemTooltip(
        "Solid: 面と外周を読む既定値\n"
        "Transparent: 地形との高さのズレを見る\n"
        "Areas: Modifier の areaType 塗り分けを見る\n"
        "Portals: ポリゴン同士の接続を見る\n"
        "Voxels: ベイクのセル判定そのものを見る (ベイク時の格子が残っている場合のみ)");

    ImGui::SetNextItemWidth(160.0f);
    ImGui::DragFloat("Draw Distance", &render.navMeshDrawDistance, 1.0f, 0.0f, 2000.0f, "%.0f m");
    ImGui::SetItemTooltip(
        "オーバーレイを描くカメラからの距離。0 で無制限。\n"
        "広いシーンで遠くまで描くと、遠景の細かいポリゴンが画面を埋めて\n"
        "手前の形が読めなくなります (描画コストもそのぶん増えます)。");

    if (render.navMeshDrawMode == renderer::NavMeshDrawMode::Voxels)
        ImGui::TextColored(kDimColor, "Voxels はカメラ周辺のみ描画します (最大 60 m)。");

    ImGui::Spacing();
    DrawLegend(render.navMeshDrawMode);
    ImGui::EndDisabled();
}

void NavigationPanel::RefreshStaleCache(EditorContext& ctx)
{
    const double now = ImGui::GetTime();
    if (m_lastHashTime >= 0.0 && now - m_lastHashTime < kStaleCheckInterval) return;
    m_lastHashTime = now;

    m_sourceHashes.clear();
    scene::Scene& scene = *ctx.activeScene;
    for (scene::EntityID eid : scene.GetEntities<scene::NavMeshSurfaceComponent>())
        m_sourceHashes[eid.index] = scene::HashNavMeshBakeSources(scene, eid);
}

void NavigationPanel::DrawSurfaceList(EditorContext& ctx)
{
    scene::Scene& scene = *ctx.activeScene;

    bool any = false;
    for (scene::EntityID eid : scene.GetEntities<scene::NavMeshSurfaceComponent>()) {
        auto* surface = scene.GetComponent<scene::NavMeshSurfaceComponent>(eid);
        auto* go      = scene.GetGameObject(eid);
        if (!surface || !go) continue;
        any = true;

        ImGui::PushID(static_cast<int>(eid.index));

        ImVec4 stateColor;
        const char* stateLabel = BakeStateLabel(*surface, stateColor);

        char header[192];
        std::snprintf(header, sizeof(header), "%s  (agentType %d)###surf",
                      go->name.c_str(), surface->agentTypeId);

        if (ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent();
            ImGui::TextColored(stateColor, "%s", stateLabel);

            const bool busy = surface->needsBake
                           || surface->bakeState == scene::NavMeshBakeState::Baking;
            ImGui::BeginDisabled(busy);
            if (ImGui::Button("Bake")) surface->needsBake = true;
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Select")) {
                SelectEntity(ctx, eid);
                ctx.requestFocusOnSelected = true;
            }
            ImGui::SameLine();
            ImGui::Checkbox("Enabled", &surface->enabled);

            if (surface->bakeState == scene::NavMeshBakeState::Baking) {
                char label[32];
                std::snprintf(label, sizeof(label), "%d%%",
                              static_cast<int>(surface->bakeProgress * 100.0f));
                ImGui::ProgressBar(surface->bakeProgress, ImVec2(-1.0f, 0.0f), label);
            }

            const auto hashIt = m_sourceHashes.find(eid.index);
            const bool stale  = !busy && surface->navMesh.IsValid()
                             && hashIt != m_sourceHashes.end()
                             && hashIt->second != surface->bakedSourceHash;
            if (stale) {
                ImGui::TextColored(kWarnColor,
                    "\xe2\x9a\xa0 ベイク後に地形か Modifier か設定が変わっています。再ベイクしてください");
            }
            if (!surface->bakeStats.failReason.empty()) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(kErrColor, "%s", surface->bakeStats.failReason.c_str());
                ImGui::PopTextWrapPos();
            }

            ImGui::Spacing();
            bool changed = false;
            static constexpr const char* kCollectNames[] = { "This Object", "Volume" };
            int collectIdx = static_cast<int>(surface->collectObjects);
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::Combo("Collect Objects", &collectIdx, kCollectNames, 2)) {
                surface->collectObjects = static_cast<scene::NavMeshCollectObjects>(collectIdx);
                changed = true;
            }
            if (surface->collectObjects == scene::NavMeshCollectObjects::Volume)
                changed |= widgets::DragVec3("Size", surface->size, 0.5f);

            ImGui::SetNextItemWidth(160.0f);
            changed |= ImGui::DragFloat("Cell Size", &surface->cellSize, 0.05f, 0.1f, 10.0f, "%.2f m");
            ImGui::SetNextItemWidth(160.0f);
            changed |= ImGui::DragFloat("Max Slope", &surface->maxSlopeAngleDeg, 0.5f, 0.0f, 89.0f, "%.1f deg");
            ImGui::SetNextItemWidth(160.0f);
            changed |= ImGui::DragFloat("Agent Radius", &surface->agentRadius, 0.05f, 0.0f, 5.0f, "%.2f m");
            ImGui::SetItemTooltip("歩行可能面をこの幅だけ内側へ削ります (Recast の walkableRadius)。");
            ImGui::SetNextItemWidth(160.0f);
            changed |= ImGui::DragFloat("Agent Height", &surface->agentHeight, 0.05f, 0.1f, 10.0f, "%.2f m");
            ImGui::SetNextItemWidth(160.0f);
            changed |= ImGui::DragFloat("Max Climb", &surface->maxClimb, 0.02f, 0.0f, 5.0f, "%.2f m");
            ImGui::SetItemTooltip(
                "Walkable Modifier の縁でこの高さを超える段差があるセルを歩行不可にします\n"
                "(Recast の walkableClimb)。0 にすると崖の上下が地続きになります。");
            ImGui::SetNextItemWidth(160.0f);
            changed |= ImGui::DragInt("Agent Type ID", &surface->agentTypeId, 1, 0, 31);

            if (ErosionIsInactive(*surface) && surface->agentRadius > 0.0f) {
                ImGui::TextColored(kWarnColor,
                    "\xe2\x9a\xa0 Agent Radius %.2f m は Cell Size %.2f m に対して小さく、侵食が起きません "
                    "(Cell Size < %.2f m が必要)",
                    surface->agentRadius, surface->cellSize, surface->agentRadius * 2.0f);
            }

            if (changed && ctx.markSceneDirty) ctx.markSceneDirty();

            const scene::NavMeshBakeStats& stats = surface->bakeStats;
            if (stats.cellsX > 0) {
                ImGui::Spacing();
                ImGui::TextColored(kDimColor,
                    "%d polys / %.0f m\xc2\xb2 / %.2f s   grid %d x %d",
                    stats.polygonCount, stats.areaSquareMeters, stats.bakeSeconds,
                    stats.cellsX, stats.cellsZ);
                const int total = stats.cellsX * stats.cellsZ;
                const auto pct = [total](int n) {
                    return total > 0 ? 100.0f * static_cast<float>(n) / static_cast<float>(total) : 0.0f;
                };
                ImGui::TextColored(kDimColor,
                    "walkable %.0f%% / steep %.0f%% / step %.0f%% / blocked %.0f%% / eroded %.0f%%",
                    pct(stats.walkableCells), pct(stats.steepCells), pct(stats.stepCells),
                    pct(stats.obstructedCells), pct(stats.erodedCells));
                if (!surface->bakeDebug.IsValid())
                    ImGui::TextColored(kDimColor,
                        "格子が大きいため Voxels 表示は使えません (Cell Size を上げてください)");
            }

            ImGui::Unindent();
        }
        ImGui::PopID();
    }

    if (!any) {
        ImGui::TextColored(kWarnColor, "NavMesh Surface がありません。");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kDimColor,
            "Terrain を持つ GameObject に NavMesh Surface を追加するか、床の GameObject へ "
            "Walkable の NavMesh Modifier を付けてください。");
        ImGui::PopTextWrapPos();
    }
}

void NavigationPanel::DrawAgentDiagnostics(EditorContext& ctx)
{
    scene::Scene& scene = *ctx.activeScene;

    std::vector<int> surfaceTypes;
    for (scene::EntityID eid : scene.GetEntities<scene::NavMeshSurfaceComponent>()) {
        auto* surface = scene.GetComponent<scene::NavMeshSurfaceComponent>(eid);
        if (surface && surface->enabled && surface->navMesh.IsValid())
            surfaceTypes.push_back(surface->agentTypeId);
    }

    int agentCount = 0;
    std::vector<int> orphanTypes;
    for (scene::EntityID eid : scene.GetEntities<scene::NavMeshAgentComponent>()) {
        auto* agent = scene.GetComponent<scene::NavMeshAgentComponent>(eid);
        if (!agent) continue;
        ++agentCount;
        if (std::find(surfaceTypes.begin(), surfaceTypes.end(), agent->agentTypeId) != surfaceTypes.end())
            continue;
        if (std::find(orphanTypes.begin(), orphanTypes.end(), agent->agentTypeId) == orphanTypes.end())
            orphanTypes.push_back(agent->agentTypeId);
    }

    int modifierCount = 0;
    for (scene::EntityID eid : scene.GetEntities<scene::NavMeshModifierComponent>()) {
        if (scene.GetComponent<scene::NavMeshModifierComponent>(eid)) ++modifierCount;
    }
    int linkCount = 0;
    for (scene::EntityID eid : scene.GetEntities<scene::NavMeshOffMeshLinkComponent>()) {
        if (scene.GetComponent<scene::NavMeshOffMeshLinkComponent>(eid)) ++linkCount;
    }

    ImGui::TextColored(kDimColor, "Agents %d   Modifiers %d   Off-Mesh Links %d",
                       agentCount, modifierCount, linkCount);

    // WHY ここで警告するか: agentTypeId が食い違うと A* は静かに経路なしを返す。
    //     Agent 側にも Surface 側にもエラーは出ないので、両方を並べて見るこの画面が唯一の気付き所。
    for (int type : orphanTypes) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kWarnColor,
            "\xe2\x9a\xa0 agentTypeId=%d の Agent に対応するベイク済み Surface がありません。"
            "この Agent は経路を引けません。", type);
        ImGui::PopTextWrapPos();
    }
}

} // namespace fbzz::editor
