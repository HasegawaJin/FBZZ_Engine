// FBZZ Engine
// TerrainTool.cpp | fbzz::editor
// TerrainTool の実装: レイキャスト・ブラシアルゴリズム・ImGui UI
#include "TerrainTool.hpp"
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
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
#include <imgui.h>

namespace fbzz::editor {

namespace {

float TerrainMinWorldHeight(const scene::TerrainComponent& terrain)
{
    if (terrain.heightData.empty()) return 0.0f;
    const auto minIt = std::min_element(terrain.heightData.begin(), terrain.heightData.end());
    return *minIt * terrain.maxHeight;
}

} // namespace

// =============================================================================
// Update — メイン入力処理
// =============================================================================

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
    // ビューポート外ではレイキャストしない
    // 非アクティブ時はブラシ入力・プレビューをすべてスキップする
    if (!m_active || !viewportHovered) {
        m_isHovering = false;
        m_hitTerrain = nullptr;
        if (!m_strokeActive) return;
    }

    // レイキャストで地形ヒット判定
    math::Vector3     hitWorld;
    scene::GameObject* hitGO = nullptr;
    m_isHovering = RaycastTerrain(scene, camera, viewportMin, viewportSize, hitWorld, hitGO);

    if (m_isHovering) {
        m_hitPoint  = hitWorld;
        m_hitTerrain = hitGO;
    } else {
        m_hitTerrain = nullptr;
        // ヒットなしでもブラシ円は最後のヒット位置に残す（移動の遅延を自然に見せる）
    }

    // -- マウスボタンが押されている間だけ編集を適用 --
    const bool mouseHeld     = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool mouseReleased = ImGui::IsMouseReleased(ImGuiMouseButton_Left);

    if (mouseHeld && m_isHovering && m_hitTerrain) {
        auto* terrainComp = m_hitTerrain->GetComponent<scene::TerrainComponent>();
        assert(terrainComp && "ヒット判定したのに TerrainComponent がない — RaycastSingleTerrain のバグ");
        if (!m_strokeActive) {
            m_strokeEntity = m_hitTerrain->GetID();
            m_strokeInstanceId = m_hitTerrain->instanceId;
            m_strokeBefore = *terrainComp;
            m_strokeActive = true;
        }

        // ワールド座標 → テレインローカル座標に変換
        const scene::Transform& tf = m_hitTerrain->transform;
        const math::Vector3 hitLocal = {
            m_hitPoint.x - tf.position.x,
            m_hitPoint.y - tf.position.y,
            m_hitPoint.z - tf.position.z
        };

        // Flatten モード: 最初のクリックで基準高さを固定する
        if (m_mode == Mode::Sculpt && m_sculpt == SculptMode::Flatten && !m_flattenLocked) {
            m_flattenTarget = terrainComp->GetHeightAt(hitLocal.x, hitLocal.z);
            m_flattenLocked = true;
        }

        switch (m_mode) {
            case Mode::Sculpt:
                ApplySculpt(*terrainComp, hitLocal, dt);
                terrainComp->heightDirty = true;
                break;
            case Mode::Paint:
                // splatData が空なら layer0=255 で初期化してから塗る
                if (terrainComp->splatData.empty()) {
                    terrainComp->splatData.assign(
                        static_cast<size_t>(terrainComp->columns)
                      * static_cast<size_t>(terrainComp->rows) * 4u, 0u);
                    for (int i = 0; i < terrainComp->columns * terrainComp->rows; ++i)
                        terrainComp->splatData[static_cast<size_t>(i) * 4 + 0] = 255u;
                }
                ApplyPaint(*terrainComp, hitLocal, dt);
                terrainComp->splatDirty = true;
                break;
        }
        markDirty();
    }

    // マウスボタンを離したら Flatten の固定を解除する
    if (mouseReleased) {
        m_flattenLocked = false;
        if (m_strokeActive) {
            if (auto* go = scene.GetGameObject(m_strokeEntity)) {
                if (auto* terrain = go->GetComponent<scene::TerrainComponent>(); terrain && undoStack) {
                    const scene::TerrainComponent before = m_strokeBefore;
                    const scene::TerrainComponent after = *terrain;
                    scene::Scene* scenePtr = &scene;
                    const std::string instanceId = m_strokeInstanceId;
                    auto apply = [scenePtr, instanceId, markDirty](const scene::TerrainComponent& value) {
                        if (auto* target = scenePtr->FindByGuid(instanceId)) {
                            if (auto* component = target->GetComponent<scene::TerrainComponent>()) {
                                *component = value;
                                component->heightDirty = true;
                                component->splatDirty = true;
                                component->colliderDirty = true;
                                if (markDirty) markDirty();
                            }
                        }
                    };
                    undoStack->Push(std::make_unique<LambdaCommand>(
                        m_mode == Mode::Sculpt ? "Sculpt Terrain" : "Paint Terrain",
                        [apply, after]() { apply(after); },
                        [apply, before]() { apply(before); }));
                }
            }
            m_strokeActive = false;
        }
    }

    // ブラシ円をビューポートに投影描画
    if (m_isHovering)
        DrawBrushPreview(viewportMin, viewportSize, camera);
}

// =============================================================================
// レイキャスト
// =============================================================================

bool TerrainTool::RaycastTerrain(
    scene::Scene&           scene,
    const renderer::Camera& camera,
    const ImVec2&           viewportMin,
    const ImVec2&           viewportSize,
    math::Vector3&          outHitWorld,
    scene::GameObject*&     outGO) const
{
    // マウス位置 → NDC 変換
    ImVec2 mouse = ImGui::GetMousePos();
    const float ndcX = ((mouse.x - viewportMin.x) / viewportSize.x) * 2.0f - 1.0f;
    const float ndcY = 1.0f - ((mouse.y - viewportMin.y) / viewportSize.y) * 2.0f;

    // NDC → カメラレイ
    const math::Matrix4 invVP = math::Matrix4::Inverse(
        camera.GetProjectionMatrix() * camera.GetViewMatrix());
    const math::Ray ray = math::Ray::FromNDC(ndcX, ndcY, camera.m_position, invVP);

    // シーン内の全 TerrainComponent に対してレイキャスト
    float bestT = 1e30f;
    scene::GameObject* bestGO = nullptr;
    math::Vector3 bestLocalHit;

    for (scene::EntityID eid : scene.GetEntities<scene::TerrainComponent>()) {
        auto* tc = scene.GetComponent<scene::TerrainComponent>(eid);
        auto* go = scene.GetGameObject(eid);
        if (!tc || !go || !tc->enabled || tc->heightData.empty()) continue;

        math::Vector3 localHit;
        if (!RaycastSingleTerrain(ray, *tc, go->transform, localHit)) continue;

        // ヒット位置のワールド t を求めて最近傍を選ぶ
        const math::Vector3 hitWorld = {
            localHit.x + go->transform.position.x,
            localHit.y + go->transform.position.y,
            localHit.z + go->transform.position.z
        };
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

    outHitWorld = {
        bestLocalHit.x + bestGO->transform.position.x,
        bestLocalHit.y + bestGO->transform.position.y,
        bestLocalHit.z + bestGO->transform.position.z
    };
    outGO = bestGO;
    return true;
}

// DDA + 二分探法による単一地形へのレイキャスト
// 地形は Y 軸上向き・回転なし・スケール一様を前提とする。
bool TerrainTool::RaycastSingleTerrain(
    const math::Ray&               ray,
    const scene::TerrainComponent& terrain,
    const scene::Transform&        tf,
    math::Vector3&                 outLocalHit) const
{
    // テレインローカル空間でレイを表現する（回転なし前提なので平行移動のみ）
    const math::Vector3 rayOriginLocal = {
        ray.origin.x - tf.position.x,
        ray.origin.y - tf.position.y,
        ray.origin.z - tf.position.z
    };
    const math::Vector3& rayDir = ray.direction;

    // テレイン全体の AABB（ローカル空間）
    const float terrainW = static_cast<float>(terrain.columns - 1) * terrain.cellSize;
    const float terrainD = static_cast<float>(terrain.rows    - 1) * terrain.cellSize;
    const float minH     = std::min(0.0f, TerrainMinWorldHeight(terrain));
    const float maxH     = terrain.maxHeight;

    // AABB スラブテスト
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

    // DDA: cellSize ごとにステップして地形との交差セルを探す
    // WHY: ハイトマップは離散グリッドなので cellSize 単位のステップが最も効率的。
    //      距離ベースのレイマーチより少ないイテレーションで正確に交差セルを検出できる。
    const float stepT   = terrain.cellSize / std::max(std::abs(rayDir.x), std::abs(rayDir.z));
    const int   maxStep = static_cast<int>((tMax - tMin) / stepT) + 2;

    float prevT = tMin;
    float prevH = -1.0f; // 未使用の初期値
    bool  foundBracket = false;
    float bracketLo = tMin, bracketHi = tMin;

    for (int step = 0; step <= maxStep; ++step) {
        const float t = tMin + static_cast<float>(step) * stepT;
        const float tClamped = std::min(t, tMax);

        const math::Vector3 p = {
            rayOriginLocal.x + rayDir.x * tClamped,
            rayOriginLocal.y + rayDir.y * tClamped,
            rayOriginLocal.z + rayDir.z * tClamped
        };
        const float terrainH = terrain.GetHeightAt(p.x, p.z);

        if (step > 0) {
            // 前のステップではレイが地面より上、今は下 → 交差区間を発見
            if (prevH > 0.0f && (p.y <= terrainH) && (prevT < tMax)) {
                foundBracket = true;
                bracketLo    = prevT;
                bracketHi    = tClamped;
                break;
            }
        }
        prevT = tClamped;
        prevH = p.y - terrainH; // 正なら地面より上

        if (tClamped >= tMax) break;
    }

    if (!foundBracket) return false;

    // 二分探法（8 回）で交差点を精密化
    // WHY: DDA で見つけた区間は cellSize 精度なので、さらに二分で誤差を 1/256 に縮める。
    for (int i = 0; i < 8; ++i) {
        const float mid = (bracketLo + bracketHi) * 0.5f;
        const math::Vector3 p = {
            rayOriginLocal.x + rayDir.x * mid,
            rayOriginLocal.y + rayDir.y * mid,
            rayOriginLocal.z + rayDir.z * mid
        };
        if (p.y > terrain.GetHeightAt(p.x, p.z))
            bracketLo = mid;
        else
            bracketHi = mid;
    }

    const float tFinal = (bracketLo + bracketHi) * 0.5f;
    const math::Vector3 localHit = {
        rayOriginLocal.x + rayDir.x * tFinal,
        terrain.GetHeightAt(
            rayOriginLocal.x + rayDir.x * tFinal,
            rayOriginLocal.z + rayDir.z * tFinal),
        rayOriginLocal.z + rayDir.z * tFinal
    };
    outLocalHit = localHit;
    return true;
}

// =============================================================================
// フォールオフ計算
// =============================================================================

float TerrainTool::ComputeWeight(float dist) const
{
    const float r = m_brush.radius;
    if (dist >= r) return 0.0f;
    const float t = dist / r; // [0, 1)
    switch (m_brush.falloff) {
        case FalloffType::Linear:
            return 1.0f - t;
        case FalloffType::Smooth:
            // smoothstep: t²(3 - 2t)
            return 1.0f - t * t * (3.0f - 2.0f * t);
        case FalloffType::Gaussian:
            // exp(-3 * t²) → t=0 で 1、t=1 で exp(-3) ≒ 0.05
            return std::exp(-3.0f * t * t);
    }
    return 0.0f;
}

// =============================================================================
// Sculpt — 高さ彫刻
// =============================================================================

float TerrainTool::SampleAvg4(const scene::TerrainComponent& t, int x, int z) const
{
    auto h = [&](int xi, int zi) {
        xi = std::clamp(xi, 0, t.columns - 1);
        zi = std::clamp(zi, 0, t.rows    - 1);
        return t.heightData[static_cast<size_t>(zi) * static_cast<size_t>(t.columns)
                           + static_cast<size_t>(xi)];
    };
    return (h(x-1,z) + h(x+1,z) + h(x,z-1) + h(x,z+1)) * 0.25f;
}

void TerrainTool::ApplySculpt(
    scene::TerrainComponent& terrain,
    const math::Vector3&     hitLocal,
    float                    dt) const
{
    const int cx = static_cast<int>(hitLocal.x / terrain.cellSize);
    const int cz = static_cast<int>(hitLocal.z / terrain.cellSize);
    const int ri = static_cast<int>(m_brush.radius / terrain.cellSize) + 1;

    for (int z = cz - ri; z <= cz + ri; ++z) {
        if (z < 0 || z >= terrain.rows) continue;
        for (int x = cx - ri; x <= cx + ri; ++x) {
            if (x < 0 || x >= terrain.columns) continue;

            const float wx   = static_cast<float>(x) * terrain.cellSize;
            const float wz   = static_cast<float>(z) * terrain.cellSize;
            const float dx   = wx - hitLocal.x;
            const float dz   = wz - hitLocal.z;
            const float dist = std::sqrt(dx * dx + dz * dz);
            const float w    = ComputeWeight(dist);
            if (w <= 0.0f) continue;

            const size_t idx = static_cast<size_t>(z) * static_cast<size_t>(terrain.columns)
                             + static_cast<size_t>(x);
            float& h = terrain.heightData[idx];

            switch (m_sculpt) {
                case SculptMode::Raise:
                    h = std::clamp(h + m_brush.strength * w * dt, -1.0f, 1.0f);
                    break;
                case SculptMode::Lower:
                    // heightData=0 はフラットな基準面。Lower は負値を許可して地形を掘り下げる。
                    // WHY: 0 でクランプすると、平坦な Terrain から溝・川床・クレーターを作れない。
                    h = std::clamp(h - m_brush.strength * w * dt, -1.0f, 1.0f);
                    break;
                case SculptMode::Flatten: {
                    // m_flattenTarget は Update() で記録した基準高さ（ワールド単位）
                    const float targetNorm = m_flattenTarget / terrain.maxHeight;
                    h = math::Lerp(h, targetNorm, m_brush.strength * w * dt);
                    break;
                }
                case SculptMode::Smooth: {
                    const float avg = SampleAvg4(terrain, x, z);
                    h = math::Lerp(h, avg, m_brush.strength * w * dt);
                    break;
                }
                case SculptMode::Stamp:
                    // ブラシ中心が最高点になるよう、既存高さと weight の最大値を取る
                    // WHY: Stamp は「押し付け」なので既存の高い部分は下げない。
                    h = std::max(h, w);
                    break;
            }
        }
    }
}

// =============================================================================
// Paint — スプラットマップ塗布
// =============================================================================

void TerrainTool::ApplyPaint(
    scene::TerrainComponent& terrain,
    const math::Vector3&     hitLocal,
    float                    dt) const
{
    const int cx = static_cast<int>(hitLocal.x / terrain.cellSize);
    const int cz = static_cast<int>(hitLocal.z / terrain.cellSize);
    const int ri = static_cast<int>(m_brush.radius / terrain.cellSize) + 1;

    // 選択レイヤーを 0-3 の範囲にクランプする（fzmat で固定 4 層）
    const int layerIdx = static_cast<int>(std::min(m_paintLayer, 3u));

    for (int z = cz - ri; z <= cz + ri; ++z) {
        if (z < 0 || z >= terrain.rows) continue;
        for (int x = cx - ri; x <= cx + ri; ++x) {
            if (x < 0 || x >= terrain.columns) continue;

            const float wx   = static_cast<float>(x) * terrain.cellSize;
            const float wz   = static_cast<float>(z) * terrain.cellSize;
            const float dx   = wx - hitLocal.x;
            const float dz   = wz - hitLocal.z;
            const float dist = std::sqrt(dx * dx + dz * dz);
            const float w    = ComputeWeight(dist);
            if (w <= 0.0f) continue;

            const size_t base = (static_cast<size_t>(z) * static_cast<size_t>(terrain.columns)
                               + static_cast<size_t>(x)) * 4u;

            // uint8 → float に変換して計算
            float weights[4];
            for (int i = 0; i < 4; ++i)
                weights[i] = terrain.splatData[base + i] / 255.0f;

            // 選択レイヤーのウェイトを増やす
            const float delta = m_brush.strength * w * dt;
            weights[layerIdx] = std::min(1.0f, weights[layerIdx] + delta);

            // 合計が 1 を超えないよう他レイヤーを按分して下げる
            float excess = -1.0f;
            for (int i = 0; i < 4; ++i) excess += weights[i];

            if (excess > 0.0f) {
                float otherTotal = 0.0f;
                for (int i = 0; i < 4; ++i)
                    if (i != layerIdx) otherTotal += weights[i];
                if (otherTotal > 1e-4f) {
                    const float scale = (otherTotal - excess) / otherTotal;
                    for (int i = 0; i < 4; ++i)
                        if (i != layerIdx)
                            weights[i] = std::max(0.0f, weights[i] * scale);
                }
            }

            // float → uint8 に書き戻す（+0.5f で四捨五入）
            for (int i = 0; i < 4; ++i)
                terrain.splatData[base + i] = static_cast<uint8_t>(weights[i] * 255.0f + 0.5f);
        }
    }
}

// =============================================================================
// ブラシ円プレビュー（スクリーン空間投影）
// =============================================================================

void TerrainTool::DrawBrushPreview(
    const ImVec2&           viewportMin,
    const ImVec2&           viewportSize,
    const renderer::Camera& camera) const
{
    // ワールド座標を 2D スクリーン座標に変換するローカルラムダ
    const math::Matrix4 vp = camera.GetViewProjection();
    auto project = [&](const math::Vector3& p) -> ImVec2 {
        const math::Vector4 clip = vp * math::Vector4{ p.x, p.y, p.z, 1.0f };
        if (clip.w < 0.001f) return { -99999.f, -99999.f };
        const float ndcX = clip.x / clip.w;
        const float ndcY = clip.y / clip.w;
        return {
            viewportMin.x + (ndcX + 1.0f) * 0.5f * viewportSize.x,
            viewportMin.y + (1.0f - (ndcY + 1.0f) * 0.5f) * viewportSize.y
        };
    };

    // ブラシ半径のリング: 地形 XZ 平面上で 32 等分した点を投影して線分で結ぶ
    // WHY: DebugDraw は GPU コマンドなので ImGui DrawList と混在しづらい。
    //      ImGui DrawList の 2D ラインで代替する方が実装がシンプルで確実。
    // GetForegroundDrawList でウィンドウスタックの最前面に描画する。
    // GetWindowDrawList だとビューポート画像の裏に隠れる可能性がある。
    ImDrawList* dl      = ImGui::GetForegroundDrawList();
    const float r       = m_brush.radius;
    const int   segs    = 32;
    constexpr float kPi = 3.14159265f;
    const ImU32 col     = (m_mode == Mode::Sculpt)
                        ? IM_COL32(255, 220, 50,  220)  // Sculpt: 黄色
                        : IM_COL32(50,  200, 255, 220);  // Paint: 水色

    ImVec2 prev = project({
        m_hitPoint.x + r,
        m_hitPoint.y,
        m_hitPoint.z
    });
    for (int i = 1; i <= segs; ++i) {
        const float angle = static_cast<float>(i) / static_cast<float>(segs) * 2.0f * kPi;
        const ImVec2 cur = project({
            m_hitPoint.x + std::cos(angle) * r,
            m_hitPoint.y,
            m_hitPoint.z + std::sin(angle) * r
        });
        dl->AddLine(prev, cur, col, 1.5f);
        prev = cur;
    }
}

// =============================================================================
// ImGui UI
// =============================================================================

// =============================================================================
// DrawSculptContent / DrawPaintContent / DrawImportSection
// NatureTool のタブ内から呼ぶためのウィンドウなし描画メソッド
// =============================================================================

void TerrainTool::DrawSculptContent(
    scene::Scene& /*scene*/, UndoStack* /*undoStack*/, const std::function<void()>& /*markDirty*/)
{
    ImGui::TextDisabled("Brush Mode");
    const char* sculptLabels[] = { "Raise", "Lower", "Smooth", "Flatten", "Stamp" };
    for (int i = 0; i < 5; ++i) {
        const bool active = static_cast<int>(m_sculpt) == i;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 0.7f, 0.4f, 1.0f));
        if (ImGui::SmallButton(sculptLabels[i]))
            m_sculpt = static_cast<SculptMode>(i);
        if (active) ImGui::PopStyleColor();
        if (i < 4) ImGui::SameLine();
    }
}

void TerrainTool::DrawPaintContent(
    scene::Scene& /*scene*/, UndoStack* /*undoStack*/, const std::function<void()>& /*markDirty*/)
{
    ImGui::TextDisabled("Splat Layer");
    for (int i = 0; i < 4; ++i) {
        const bool active = static_cast<int>(m_paintLayer) == i;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));
        if (ImGui::Button(("Layer " + std::to_string(i)).c_str(), { -1.0f, 0.0f }))
            m_paintLayer = static_cast<uint32_t>(i);
        if (active) ImGui::PopStyleColor();
    }
}

void TerrainTool::DrawBrushSettings()
{
    ImGui::Spacing();
    ImGui::SeparatorText("Brush Settings");
    ImGui::SliderFloat("Radius", &m_brush.radius, 0.5f, 50.0f, "%.1f");
    ImGui::SliderFloat("Strength", &m_brush.strength, 0.001f, 1.0f, "%.3f");
    const char* falloffNames[] = { "Linear", "Smooth", "Gaussian" };
    int falloffIndex = static_cast<int>(m_brush.falloff);
    if (ImGui::Combo("Falloff", &falloffIndex, falloffNames, 3))
        m_brush.falloff = static_cast<FalloffType>(falloffIndex);
}

void TerrainTool::DrawImportSection(
    scene::Scene& scene, UndoStack* undoStack, const std::function<void()>& markDirty)
{
    if (!ImGui::CollapsingHeader("HeightMap Import")) return;

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
    ImGui::RadioButton("Unipolar  [0 → maxH]",    &reinterpret_cast<int&>(m_heightMapUnipolar), 1);
    ImGui::SameLine();
    ImGui::RadioButton("Bipolar [-maxH → +maxH]", &reinterpret_cast<int&>(m_heightMapUnipolar), 0);
    ImGui::Spacing();

    const bool canImport = m_heightMapPath[0] != '\0';
    if (!canImport) ImGui::BeginDisabled();
    if (ImGui::Button("Import into Terrain", { -1.0f, 0.0f })) {
        scene::TerrainComponent* target = nullptr;
        scene::GameObject* targetObject = nullptr;
        for (scene::EntityID eid : scene.GetEntities<scene::TerrainComponent>()) {
            auto* tc = scene.GetComponent<scene::TerrainComponent>(eid);
            if (tc && tc->enabled) { target = tc; targetObject = scene.GetGameObject(eid); break; }
        }
        if (!target) {
            m_heightMapStatus = "Error: No terrain in scene";
        } else {
            const scene::TerrainComponent before = *target;
            const bool ok = scene::LoadHeightMapFromFile(m_heightMapPath, *target, m_heightMapUnipolar);
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
        const bool isOk = (m_heightMapStatus == "OK");
        ImGui::TextColored(
            isOk ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
            "%s", m_heightMapStatus.c_str());
    }
}

// =============================================================================
// OnEditorGUI — スタンドアローン用ウィンドウ (既存互換)
// =============================================================================

void TerrainTool::OnEditorGUI(
    scene::Scene& scene,
    UndoStack* undoStack,
    const std::function<void()>& markDirty)
{
    bool hasTerrain = false;
    for (scene::EntityID eid : scene.GetEntities<scene::TerrainComponent>()) {
        if (auto* tc = scene.GetComponent<scene::TerrainComponent>(eid))
            if (tc->enabled) { hasTerrain = true; break; }
    }
    if (!hasTerrain) return;

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

    {
        ImGui::PushStyleColor(ImGuiCol_Button, m_active
            ? ImVec4(0.2f, 0.6f, 0.2f, 1.0f) : ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
        if (ImGui::Button(m_active ? "  Active  " : " Inactive ", { -1.0f, 0.0f }))
            m_active = !m_active;
        ImGui::PopStyleColor();
    }
    if (!m_active) ImGui::BeginDisabled();

    {
        const bool sculpt = m_mode == Mode::Sculpt, paint = m_mode == Mode::Paint;
        if (sculpt) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.6f, 0.3f, 1.0f));
        if (ImGui::Button("Sculpt", { 95.0f, 0.0f })) m_mode = Mode::Sculpt;
        if (sculpt) ImGui::PopStyleColor();
        ImGui::SameLine();
        if (paint) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.4f, 0.7f, 1.0f));
        if (ImGui::Button("Paint",  { 95.0f, 0.0f })) m_mode = Mode::Paint;
        if (paint) ImGui::PopStyleColor();
    }
    ImGui::Spacing();

    if (m_mode == Mode::Sculpt) DrawSculptContent(scene, undoStack, markDirty);
    else                         DrawPaintContent(scene, undoStack, markDirty);

    DrawBrushSettings();

    if (!m_active) ImGui::EndDisabled();
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    DrawImportSection(scene, undoStack, markDirty);
    ImGui::End();
}

} // namespace fbzz::editor
