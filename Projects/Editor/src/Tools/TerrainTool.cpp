/// @file    TerrainTool.cpp
/// @brief   TerrainTool の実装: レイキャスト・ストロークと Undo・ImGui UI。
/// @author  Hasegawa Jin
/// @date    2026-05-31
/// @see Docs/design/terrain-layers.md
#include "TerrainTool.hpp"
#include <Editor/Util/EditorTheme.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
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
#include <cstring>
#include <imgui.h>
#include <string>
#include <vector>

namespace fbzz::editor {

namespace {

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

} // namespace

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
        case Mode::Paint:  return "Layer " + std::to_string(m_paintLayer);
        case Mode::Hole:   return m_holeErase ? "Fill Hole" : "Cut Hole";
    }
    return {};
}

void TerrainTool::Update(
    scene::Scene&               scene,
    const renderer::Camera&     camera,
    float                       dt,
    bool                        viewportHovered,
    const ImVec2&               viewportMin,
    const ImVec2&               viewportSize,
    const std::function<void()>& markDirty,
    UndoStack*                   undoStack)
{
    /// @note ビューポート外・非アクティブではレイキャストしない。ただし進行中のストロークは離すまで追う。
    if (!m_active || !viewportHovered) {
        m_isHovering = false;
        m_hitTerrain = nullptr;
        if (!m_strokeActive && !m_rampDragging) return;
    }

    /// @note [ / ] で半径、Shift+[ / ] で強度。ホイールはカメラ操作と競合するため使わない。
    if (viewportHovered) {
        const bool  shift        = ImGui::GetIO().KeyShift;
        const float radiusStep   = std::max(0.5f, m_brush.radius * 0.1f);
        const float strengthStep = 0.02f;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, /*repeat=*/true)) {
            if (shift) m_brush.strength = std::clamp(m_brush.strength - strengthStep, 0.001f, 1.0f);
            else       m_brush.radius   = std::clamp(m_brush.radius   - radiusStep,   0.5f,  50.0f);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, /*repeat=*/true)) {
            if (shift) m_brush.strength = std::clamp(m_brush.strength + strengthStep, 0.001f, 1.0f);
            else       m_brush.radius   = std::clamp(m_brush.radius   + radiusStep,   0.5f,  50.0f);
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
    m_isHovering = RaycastTerrain(scene, camera, viewportMin, viewportSize, hitWorld, hitGO);
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

    if (m_mode == Mode::Sculpt && m_rampMode) {
        UpdateRamp(scene, mousePressed, mouseReleased || (!mouseHeld && m_rampDragging), markDirty, undoStack);
        if (m_rampDragging)
            DrawRampPreview(viewportMin, viewportSize, camera);
        else if (m_isHovering)
            DrawBrushPreview(viewportMin, viewportSize, camera);
        return;
    }

    if (mouseHeld && m_isHovering && m_hitTerrain) {
        auto* terrainComp = m_hitTerrain->GetComponent<scene::TerrainComponent>();
        assert(terrainComp && "ヒット判定したのに TerrainComponent がない — RaycastSingleTerrain のバグ");
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
                    ApplyTerrainSculpt(*tc, localN, m_brush, m_activeSculpt, m_flattenTarget, dt, m_strokeStep);
                    tc->heightDirty = true;
                }
                break;
            case Mode::Paint: {
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
        markDirty();
    }

    if (mouseReleased) {
        m_flattenLocked = false;
        if (m_strokeActive) {
            const char* label = m_mode == Mode::Sculpt ? "Sculpt Terrain"
                              : m_mode == Mode::Paint  ? "Paint Terrain"
                                                       : "Edit Terrain Holes";
            /// @note Paint は形状に影響しないのでコライダーを作り直さない。
            CommitStroke(scene, label, m_mode != Mode::Paint, markDirty, undoStack);
        }
    }

    if (m_isHovering)
        DrawBrushPreview(viewportMin, viewportSize, camera);
}

void TerrainTool::CaptureStrokeBefore(scene::GameObject& go, const scene::TerrainComponent& terrain)
{
    const auto found = std::find_if(
        m_strokeBeforeTerrains.begin(), m_strokeBeforeTerrains.end(),
        [&](const TerrainStrokeSnapshot& snapshot) { return snapshot.instanceId == go.instanceId; });
    if (found == m_strokeBeforeTerrains.end())
        m_strokeBeforeTerrains.push_back({ go.instanceId, terrain });
}

void TerrainTool::CommitStroke(scene::Scene& scene, const char* undoLabel, bool rebuildCollider,
                               const std::function<void()>& markDirty, UndoStack* undoStack)
{
    if (rebuildCollider) {
        for (const TerrainStrokeSnapshot& snapshot : m_strokeBeforeTerrains) {
            if (auto* target = scene.FindByGuid(snapshot.instanceId))
                if (auto* component = target->GetComponent<scene::TerrainComponent>())
                    component->colliderDirty = true;
        }
    }
    if (undoStack && !m_strokeBeforeTerrains.empty()) {
        std::vector<TerrainStrokeSnapshot> before = std::move(m_strokeBeforeTerrains);
        std::vector<TerrainStrokeSnapshot> after;
        after.reserve(before.size());
        for (const TerrainStrokeSnapshot& snapshot : before) {
            if (auto* target = scene.FindByGuid(snapshot.instanceId))
                if (auto* component = target->GetComponent<scene::TerrainComponent>())
                    after.push_back({ snapshot.instanceId, *component });
        }
        scene::Scene* scenePtr = &scene;
        auto apply = [scenePtr, markDirty](const std::vector<TerrainStrokeSnapshot>& values) {
            for (const TerrainStrokeSnapshot& snapshot : values) {
                if (auto* target = scenePtr->FindByGuid(snapshot.instanceId)) {
                    if (auto* component = target->GetComponent<scene::TerrainComponent>()) {
                        *component = snapshot.before;
                        component->heightDirty = true;
                        component->splatDirty = true;
                        component->colliderDirty = true;
                    }
                }
            }
            if (markDirty)
                markDirty();
        };
        undoStack->Push(std::make_unique<LambdaCommand>(
            undoLabel,
            [apply, after]() { apply(after); },
            [apply, before]() { apply(before); }));
    }
    m_strokeBeforeTerrains.clear();
    m_strokeActive = false;
}

void TerrainTool::UpdateRamp(scene::Scene& scene, bool mousePressed, bool mouseReleased,
                             const std::function<void()>& markDirty, UndoStack* undoStack)
{
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
        ApplyTerrainRamp(*tc, startLocal, endLocal, m_brush);
        tc->heightDirty = true;
        any = true;
    }
    if (!any)
        return;
    if (markDirty)
        markDirty();
    CommitStroke(scene, "Ramp Terrain", true, markDirty, undoStack);
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
    const float minH     = std::min(0.0f, TerrainToolMinWorldHeight(terrain));
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
    if (!slab(rayOriginLocal.y, rayDir.y, minH, maxH,     tMin, tMax)) return false;
    if (!slab(rayOriginLocal.z, rayDir.z, 0.0f, terrainD, tMin, tMax)) return false;
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
    const float r    = m_brush.radius;
    constexpr int kSegments = 64;
    ImU32 col = IM_COL32(255, 220, 50, 220);
    if (m_mode == Mode::Paint)
        col = IM_COL32(50, 200, 255, 220);
    else if (m_mode == Mode::Hole)
        col = (m_holeErase != ImGui::GetIO().KeyCtrl) ? IM_COL32(120, 255, 120, 220) : IM_COL32(255, 80, 80, 220);

    const math::Vector3 hitLocal = ToTerrainLocal(m_hitTerrain->transform, m_hitPoint);
    auto ringPoint = [&](float angle) {
        math::Vector3 localPoint = {
            hitLocal.x + std::cos(angle) * r,
            0.0f,
            hitLocal.z + std::sin(angle) * r
        };
        localPoint.y = terrain->GetHeightAt(localPoint.x, localPoint.z);
        return ToTerrainWorld(m_hitTerrain->transform, localPoint);
    };

    ImVec2 prev = TerrainToolProject(vp, viewportMin, viewportSize, ringPoint(0.0f));
    for (int i = 1; i <= kSegments; ++i) {
        const float angle = static_cast<float>(i) / static_cast<float>(kSegments) * math::TWO_PI;
        const ImVec2 cur = TerrainToolProject(vp, viewportMin, viewportSize, ringPoint(angle));
        dl->AddLine(prev, cur, col, 1.5f);
        prev = cur;
    }
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
    scene::Scene& /*scene*/, UndoStack* /*undoStack*/, const std::function<void()>& /*markDirty*/)
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

void TerrainTool::DrawPaintContent(
    scene::Scene& scene, UndoStack* /*undoStack*/, const std::function<void()>& /*markDirty*/)
{
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

void TerrainTool::DrawHoleContent(
    scene::Scene& scene, UndoStack* /*undoStack*/, const std::function<void()>& /*markDirty*/)
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

void TerrainTool::DrawBrushSettings()
{
    ImGui::Spacing();
    ImGui::SeparatorText("Brush Settings");
    ImGui::SliderFloat("Radius", &m_brush.radius, 0.5f, 50.0f, "%.1f");
    ImGui::SliderFloat("Strength", &m_brush.strength, 0.001f, 1.0f, "%.3f");
    ImGui::TextDisabled("[ / ] : Radius    Shift+[ / ] : Strength");
    const char* falloffNames[] = { "Linear", "Smooth", "Gaussian" };
    int falloffIndex = static_cast<int>(m_brush.falloff);
    if (ImGui::Combo("Falloff", &falloffIndex, falloffNames, 3))
        m_brush.falloff = static_cast<FalloffType>(falloffIndex);
}

void TerrainTool::DrawImportSection(
    scene::Scene& scene, UndoStack* undoStack, const std::function<void()>& markDirty)
{
    if (!ImGui::CollapsingHeader("HeightMap Import / Export")) return;

    ImGui::TextDisabled("File Path (PNG / TGA / DDS)");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##hmpath", m_heightMapPath, sizeof(m_heightMapPath));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            strncpy_s(m_heightMapPath, sizeof(m_heightMapPath),
                      static_cast<const char*>(payload->Data), _TRUNCATE);
            m_heightMapStatus.clear();
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::TextDisabled("(Asset Browser からドラッグ＆ドロップも可)");
    ImGui::RadioButton("Unipolar  [0 → maxH]",    &m_heightMapUnipolar, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Bipolar [-maxH → +maxH]", &m_heightMapUnipolar, 0);
    ImGui::Spacing();

    const bool canImport = m_heightMapPath[0] != '\0';
    if (!canImport) ImGui::BeginDisabled();
    if (ImGui::Button("Import into Terrain", { -1.0f, 0.0f })) {
        scene::GameObject* targetObject = nullptr;
        scene::TerrainComponent* target = TerrainToolFirstTerrain(scene, &targetObject);
        if (!target) {
            m_heightMapStatus = "Error: No terrain in scene";
        } else {
            const scene::TerrainComponent before = *target;
            const bool ok = scene::LoadHeightMapFromFile(m_heightMapPath, *target, m_heightMapUnipolar != 0);
            m_heightMapStatus = ok ? "OK" : "Error: Load failed";
            if (ok) {
                target->heightDirty = true; target->splatDirty = true; target->colliderDirty = true;
                if (markDirty) markDirty();
                if (undoStack && targetObject) {
                    const scene::TerrainComponent after = *target;
                    const std::string instanceId = targetObject->instanceId;
                    scene::Scene* scenePtr = &scene;
                    auto apply = [scenePtr, instanceId, markDirty](const scene::TerrainComponent& v) {
                        if (auto* go = scenePtr->FindByGuid(instanceId))
                            if (auto* comp = go->GetComponent<scene::TerrainComponent>()) {
                                *comp = v;
                                comp->heightDirty = true; comp->splatDirty = true; comp->colliderDirty = true;
                                if (markDirty) markDirty();
                            }
                    };
                    undoStack->Push(std::make_unique<LambdaCommand>(
                        "Import Terrain Heightmap",
                        [apply, after]()  { apply(after); },
                        [apply, before]() { apply(before); }));
                }
            }
        }
    }
    if (!canImport) ImGui::EndDisabled();
    if (!m_heightMapStatus.empty()) {
        const bool isOk = m_heightMapStatus.starts_with("OK");
        ImGui::TextColored(
            isOk ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
            "%s", m_heightMapStatus.c_str());
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Export Path (16-bit PNG)");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##hmexport", m_heightMapExport, sizeof(m_heightMapExport));
    const bool canExport = m_heightMapExport[0] != '\0';
    if (!canExport) ImGui::BeginDisabled();
    if (ImGui::Button("Export Heightmap", { -1.0f, 0.0f })) {
        const scene::TerrainComponent* target = TerrainToolFirstTerrain(scene, nullptr);
        std::string path = m_heightMapExport;
        /// @note 出力は常に PNG なので、拡張子が違えば付け足す (WIC は拡張子で形式を決めないが、読み戻す側が拡張子を見る)。
        if (!(path.ends_with(".png") || path.ends_with(".PNG")))
            path += ".png";
        if (!target)
            m_heightMapExportStatus = "Error: No terrain in scene";
        else if (scene::SaveHeightMapToFile(path, *target, m_heightMapUnipolar != 0))
            m_heightMapExportStatus = "OK: " + path;
        else
            m_heightMapExportStatus = "Error: Save failed";
    }
    if (!canExport) ImGui::EndDisabled();
    if (!m_heightMapExportStatus.empty()) {
        const bool isOk = m_heightMapExportStatus.starts_with("OK");
        ImGui::TextColored(
            isOk ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
            "%s", m_heightMapExportStatus.c_str());
    }
}

void TerrainTool::OnEditorGUI(
    scene::Scene& scene,
    UndoStack* undoStack,
    const std::function<void()>& markDirty)
{
    if (!TerrainToolFirstTerrain(scene, nullptr)) return;

    const ImGuiViewport* mainVP = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        { mainVP->WorkPos.x + mainVP->WorkSize.x - 230.0f,
          mainVP->WorkPos.y + mainVP->WorkSize.y - 10.0f },
        ImGuiCond_FirstUseEver, { 1.0f, 1.0f });
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::SetNextWindowSize({ 260.0f, 0.0f }, ImGuiCond_FirstUseEver);
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
        case Mode::Paint:  DrawPaintContent(scene, undoStack, markDirty);  break;
        case Mode::Hole:   DrawHoleContent(scene, undoStack, markDirty);   break;
    }

    DrawBrushSettings();

    if (!m_active) ImGui::EndDisabled();
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    DrawImportSection(scene, undoStack, markDirty);
    ImGui::End();
}

} // namespace fbzz::editor
