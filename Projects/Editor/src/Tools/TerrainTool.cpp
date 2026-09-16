/// @file    TerrainTool.cpp
/// @brief   TerrainTool の実装: レイキャスト・ブラシアルゴリズム・ImGui UI。
/// @author  Hasegawa Jin
/// @date    2026-05-31
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
#include <imgui.h>
#include <string>
#include <vector>

namespace fbzz::editor {

namespace {

float TerrainMinWorldHeight(const scene::TerrainComponent& terrain)
{
    if (terrain.heightData.empty()) return 0.0f;
    const auto minIt = std::min_element(terrain.heightData.begin(), terrain.heightData.end());
    return *minIt * terrain.maxHeight;
}

void EnsureSplatData(scene::TerrainComponent& terrain)
{
    const size_t expectedSize = static_cast<size_t>(terrain.columns)
                              * static_cast<size_t>(terrain.rows) * 4u;
    if (terrain.splatData.size() != expectedSize) {
        terrain.InitDefaultSplat();
        return;
    }

    for (int i = 0; i < terrain.columns * terrain.rows; ++i) {
        const size_t base = static_cast<size_t>(i) * 4u;
        const uint32_t sum = static_cast<uint32_t>(terrain.splatData[base + 0])
                           + static_cast<uint32_t>(terrain.splatData[base + 1])
                           + static_cast<uint32_t>(terrain.splatData[base + 2])
                           + static_cast<uint32_t>(terrain.splatData[base + 3]);
        if (sum == 0u) {
            terrain.splatData[base + 0] = 255u;
            terrain.splatData[base + 1] = 0u;
            terrain.splatData[base + 2] = 0u;
            terrain.splatData[base + 3] = 0u;
        }
    }
}

const char* LayerDisplayName(const std::string& path)
{
    if (path.empty())
        return "(empty)";

    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path.c_str() : path.c_str() + slash + 1;
}

// ToTerrainLocal / ToTerrainWorld / BrushOverlapsTerrainXZ は TerrainBrush.hpp が提供する
// (AI 側の terrain.sculpt / terrain.paint と同じ実体を使うため)。

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

    // ブラシサイズ・強度のホットキー調整。[ / ] で半径、Shift+[ / Shift+] で強度。
    // WHY: 多くの地形エディタ標準の操作で、パネルのスライダーへ視線を移さずブラシを連続調整できる。
    //      マウスホイールはビューポートのカメラ操作と競合するため、競合しないブラケットキーを使う。
    //      viewportHovered のときだけ拾うので、InputText 等にフォーカスがある場面では誤爆しない。
    if (viewportHovered) {
        const bool  shift        = ImGui::GetIO().KeyShift;
        const float radiusStep   = std::max(0.5f, m_brush.radius * 0.1f); // 大きいブラシほど粗く刻む
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

    // 修飾キーによる Sculpt サブモードの一時上書き (Unity Terrain 互換)。
    // Shift+drag = Smooth / Ctrl+drag = Lower。修飾を離せば選択中のサブモード(m_sculpt)に戻る。
    // WHY: 平滑化・掘り下げは頻繁に切り替えるため、パネルのラジオボタンへ視線を戻さず手元で操作できるようにする。
    //      Ctrl を優先し、Ctrl+Shift 同時押しは Lower とする。
    m_activeSculpt = m_sculpt;
    if (m_mode == Mode::Sculpt && viewportHovered) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl)       m_activeSculpt = SculptMode::Lower;
        else if (io.KeyShift) m_activeSculpt = SculptMode::Smooth;
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
            m_strokeBeforeTerrains.clear();
            m_strokeActive = true;
        }
        auto captureTerrainBefore = [&](scene::GameObject* go, scene::TerrainComponent* terrain) {
            if (!go || !terrain)
                return;
            const auto found = std::find_if(
                m_strokeBeforeTerrains.begin(),
                m_strokeBeforeTerrains.end(),
                [&](const TerrainStrokeSnapshot& snapshot) {
                    return snapshot.instanceId == go->instanceId;
                });
            if (found == m_strokeBeforeTerrains.end())
                m_strokeBeforeTerrains.push_back({ go->instanceId, *terrain });
        };
        captureTerrainBefore(m_hitTerrain, terrainComp);

        // 描画と同じ World Matrix の逆変換で Terrain ローカル座標へ変換する。
        const scene::Transform& tf = m_hitTerrain->transform;
        const math::Vector3 hitLocal = ToTerrainLocal(tf, m_hitPoint);

        // Flatten モード: 最初のクリックで基準高さを固定する
        if (m_mode == Mode::Sculpt && m_activeSculpt == SculptMode::Flatten && !m_flattenLocked) {
            m_flattenTarget = terrainComp->GetHeightAt(hitLocal.x, hitLocal.z);
            m_flattenLocked = true;
        }

        // ブラシ範囲にワールド空間で重なる「全 Terrain」を編集対象にする。
        // WHY: 各 Terrain は独立した heightData / splatData を持つため、ブラシ半径が境界を越えても
        //      カーソル下の 1 つだけを編集すると、隣の境界列が取り残されて段差・継ぎ目が残る。
        //      共有境界の頂点は隣接 Terrain 同士で同一のワールド点なので、各 Terrain のローカル空間へ
        //      ブラシ中心を変換して当てれば同一のデルタが入り、グリッド登録の有無に関係なく連続する。
        switch (m_mode) {
            case Mode::Sculpt:
                // colliderDirty はここでは立てない（毎フレーム BVH 再構築を避け、確定時に 1 回だけ）。
                for (scene::EntityID eid : scene.GetEntities<scene::TerrainComponent>()) {
                    auto* go = scene.GetGameObject(eid);
                    auto* tc = scene.GetComponent<scene::TerrainComponent>(eid);
                    if (!go || !tc || !tc->enabled || tc->heightData.empty())
                        continue;
                    const math::Vector3 localN = ToTerrainLocal(go->transform, m_hitPoint);
                    if (!BrushOverlapsTerrainXZ(*tc, localN, m_brush.radius))
                        continue;
                    captureTerrainBefore(go, tc);
                    ApplyTerrainSculpt(*tc, localN, m_brush, m_activeSculpt, m_flattenTarget, dt);
                    tc->heightDirty = true;
                }
                break;
            case Mode::Paint: {
                const int paintLayer = static_cast<int>(std::min(m_paintLayer, 3u));
                const std::string sourceMaterial = terrainComp->layerMaterials[paintLayer];
                for (scene::EntityID eid : scene.GetEntities<scene::TerrainComponent>()) {
                    auto* go = scene.GetGameObject(eid);
                    auto* tc = scene.GetComponent<scene::TerrainComponent>(eid);
                    if (!go || !tc || !tc->enabled || tc->heightData.empty())
                        continue;
                    const math::Vector3 localN = ToTerrainLocal(go->transform, m_hitPoint);
                    if (!BrushOverlapsTerrainXZ(*tc, localN, m_brush.radius))
                        continue;
                    // ヒットした Terrain は選択レイヤーをそのまま塗る。
                    // WHY: マテリアル解決を通すと、複数レイヤーが同じ material path を共有していたり
                    //      未割り当て(空)だった場合に選択レイヤーと違う層へ解決され、狙ったレイヤーを
                    //      塗れなくなる。隣接 Terrain だけは layerMaterials の並びが異なり得るため、
                    //      同じ material path のレイヤーを探して塗り、見つからなければスキップする。
                    int layer = paintLayer;
                    if (tc != terrainComp) {
                        layer = ResolvePaintLayerForTerrain(*tc, sourceMaterial, paintLayer);
                        if (layer < 0)
                            continue;
                    }
                    captureTerrainBefore(go, tc);
                    EnsureSplatData(*tc);
                    ApplyTerrainPaint(*tc, localN, m_brush, layer, dt);
                    tc->splatDirty = true;
                }
                break;
            }
        }
        markDirty();
    }

    // マウスボタンを離したら Flatten の固定を解除する
    if (mouseReleased) {
        m_flattenLocked = false;
        if (m_strokeActive) {
            // ストローク確定時に、このストロークで触れた全 Terrain のコライダーを一度だけ再構築する。
            // WHY: Sculpt は高さを変えるため物理形状の更新が要る。Paint(スプラット)は形状に影響しない
            //      ので colliderDirty は不要。押下中ではなくここで立てることで毎フレーム再構築を避ける。
            if (m_mode == Mode::Sculpt) {
                for (const TerrainStrokeSnapshot& snapshot : m_strokeBeforeTerrains) {
                    if (auto* target = scene.FindByGuid(snapshot.instanceId))
                        if (auto* component = target->GetComponent<scene::TerrainComponent>())
                            component->colliderDirty = true;
                }
            }
            if (undoStack && !m_strokeBeforeTerrains.empty()) {
                std::vector<TerrainStrokeSnapshot> before = m_strokeBeforeTerrains;
                std::vector<TerrainStrokeSnapshot> after;
                after.reserve(before.size());
                for (const TerrainStrokeSnapshot& snapshot : before) {
                    if (auto* target = scene.FindByGuid(snapshot.instanceId)) {
                        if (auto* component = target->GetComponent<scene::TerrainComponent>())
                            after.push_back({ snapshot.instanceId, *component });
                    }
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
                    m_mode == Mode::Sculpt ? "Sculpt Terrain" : "Paint Terrain",
                    [apply, after]() { apply(after); },
                    [apply, before]() { apply(before); }));
            }
            m_strokeBeforeTerrains.clear();
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

// DDA + 二分探法による単一地形へのレイキャスト
bool TerrainTool::RaycastSingleTerrain(
    const math::Ray&               ray,
    const scene::TerrainComponent& terrain,
    const scene::Transform&        tf,
    math::Vector3&                 outLocalHit) const
{
    // Terrain 描画と同じ World Matrix の逆変換でレイをローカル化する。
    // WHAT: 方向は w=0 で変換し、平行移動の影響を除外する。
    const math::Matrix4 invWorld = math::Matrix4::Inverse(tf.GetWorldMatrix());
    const math::Vector4 localOrigin =
        invWorld * math::Vector4{ ray.origin.x, ray.origin.y, ray.origin.z, 1.0f };
    const math::Vector4 localDirection =
        invWorld * math::Vector4{ ray.direction.x, ray.direction.y, ray.direction.z, 0.0f };
    const math::Vector3 rayOriginLocal = { localOrigin.x, localOrigin.y, localOrigin.z };
    const math::Vector3 rayDir = math::Vector3{
        localDirection.x, localDirection.y, localDirection.z
    }.Normalized();

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

    // ブラシ半径のリング: Terrain ローカル XZ 平面上で分割し、各点の地表高をサンプリングする。
    // WHY: 中心の高さだけで水平な円を描くと、斜面や凹凸で実際の編集範囲から浮いて見える。
    //      ApplyTerrainSculpt / ApplyTerrainPaint と同じローカル座標系を使うことで表示と編集範囲を一致させる。
    // WHY: DebugDraw は GPU コマンドなので ImGui DrawList と混在しづらい。
    //      ImGui DrawList の 2D ラインで代替する方が実装がシンプルで確実。
    // GetForegroundDrawList でウィンドウスタックの最前面に描画する。
    // GetWindowDrawList だとビューポート画像の裏に隠れる可能性がある。
    if (!m_hitTerrain)
        return;

    const auto* terrain = m_hitTerrain->GetComponent<scene::TerrainComponent>();
    if (!terrain || terrain->heightData.empty())
        return;

    ImDrawList* dl      = ImGui::GetForegroundDrawList();
    const float r       = m_brush.radius;
    const int   segs    = 64;
    constexpr float kPi = 3.14159265f;
    const ImU32 col     = (m_mode == Mode::Sculpt)
                        ? IM_COL32(255, 220, 50,  220)  // Sculpt: 黄色
                        : IM_COL32(50,  200, 255, 220);  // Paint: 水色

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

    ImVec2 prev = project(ringPoint(0.0f));
    for (int i = 1; i <= segs; ++i) {
        const float angle = static_cast<float>(i) / static_cast<float>(segs) * 2.0f * kPi;
        const ImVec2 cur = project(ringPoint(angle));
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
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(ThemeColor::Success));
        if (ImGui::SmallButton(sculptLabels[i]))
            m_sculpt = static_cast<SculptMode>(i);
        if (active) ImGui::PopStyleColor();
        if (i < 4) ImGui::SameLine();
    }
}

void TerrainTool::DrawPaintContent(
    scene::Scene& scene, UndoStack* /*undoStack*/, const std::function<void()>& /*markDirty*/)
{
    const scene::TerrainComponent* referenceTerrain = nullptr;
    if (m_hitTerrain)
        referenceTerrain = m_hitTerrain->GetComponent<scene::TerrainComponent>();
    if (!referenceTerrain) {
        for (scene::EntityID eid : scene.GetEntities<scene::TerrainComponent>()) {
            if (auto* terrain = scene.GetComponent<scene::TerrainComponent>(eid); terrain && terrain->enabled) {
                referenceTerrain = terrain;
                break;
            }
        }
    }

    ImGui::TextDisabled("Splat Layer");
    for (int i = 0; i < 4; ++i) {
        const bool active = static_cast<int>(m_paintLayer) == i;
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(ThemeColor::AccentActive));
        std::string label = "Layer " + std::to_string(i);
        if (referenceTerrain)
            label += "  " + std::string(LayerDisplayName(referenceTerrain->layerMaterials[i]));
        if (ImGui::Button(label.c_str(), { -1.0f, 0.0f }))
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
    ImGui::TextDisabled("[ / ] : Radius    Shift+[ / ] : Strength");
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
    if (sculpt) ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(ThemeColor::Success));
        if (ImGui::Button("Sculpt", { 95.0f, 0.0f })) m_mode = Mode::Sculpt;
        if (sculpt) ImGui::PopStyleColor();
        ImGui::SameLine();
    if (paint) ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Color(ThemeColor::AccentActive));
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
