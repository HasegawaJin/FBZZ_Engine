// FBZZ Engine
// DetailTool.cpp | fbzz::editor
// Detail ペイントツールの実装
#include "DetailTool.hpp"
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/TerrainDetailComponent.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Math/Ray.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <imgui.h>

namespace fbzz::editor {

// 密度マップの 1 テクセルあたりのワールドサイズ [m]
static constexpr float kDensityTexelSize = 2.0f;
static constexpr float kChunkSize        = 16.0f;
static constexpr float kPi               = 3.14159265f;

// =============================================================================
// Update
// =============================================================================

void DetailTool::Update(
    scene::Scene&             scene,
    const renderer::Camera&   camera,
    bool                      viewportHovered,
    const ImVec2&             viewportMin,
    const ImVec2&             viewportSize,
    const std::function<void()>& markDirty)
{
    // チャンクデバッグ表示は常時 (アクティブ/非アクティブ問わず)
    if (m_showChunkBounds || m_showCounts)
        DrawChunkDebug(scene, viewportMin, viewportSize, camera);

    // ドラッグ中に Terrain 全体を毎フレーム再 Bake すると編集操作が停止するため、
    // 密度変更をストローク終了時にまとめて一度だけ Bake する。
    if (m_strokeDirty && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (auto* detail = scene.GetComponent<scene::TerrainDetailComponent>(m_strokeEntity))
            detail->needsBake = true;
        m_strokeDirty  = false;
        m_strokeEntity = {};
        markDirty();
    }

    if (!m_active || !viewportHovered) {
        m_isHovering = false;
        return;
    }

    // ブラシ半径・強度のホットキー調整（TerrainTool と統一）。
    // [ / ] で半径、Shift+[ / ] で強度。viewportHovered のときだけ拾うので入力欄では誤爆しない。
    {
        const bool  shift        = ImGui::GetIO().KeyShift;
        const float radiusStep   = std::max(0.5f, m_brush.radius * 0.1f);
        const float strengthStep = 0.05f;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, /*repeat=*/true)) {
            if (shift) m_brush.strength = std::clamp(m_brush.strength - strengthStep, 0.01f, 1.0f);
            else       m_brush.radius   = std::clamp(m_brush.radius   - radiusStep,   0.5f, 30.0f);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, /*repeat=*/true)) {
            if (shift) m_brush.strength = std::clamp(m_brush.strength + strengthStep, 0.01f, 1.0f);
            else       m_brush.radius   = std::clamp(m_brush.radius   + radiusStep,   0.5f, 30.0f);
        }
    }

    // レイキャスト
    math::Vector3   hitWorld;
    scene::EntityID hitEntity;
    m_isHovering = RaycastTerrain(scene, camera, viewportMin, viewportSize, hitWorld, hitEntity);

    if (m_isHovering) {
        m_hitPoint  = hitWorld;
        m_hitEntity = hitEntity;
    }

    if (m_isHovering)
        DrawBrushPreview(viewportMin, viewportSize, camera);

    // マウスホールドで密度を変更する
    const bool mouseHeld = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (mouseHeld && m_isHovering) {
        auto* detail  = scene.GetComponent<scene::TerrainDetailComponent>(m_hitEntity);
        auto* terrain = scene.GetComponent<scene::TerrainComponent>(m_hitEntity);
        auto* go      = scene.GetGameObject(m_hitEntity);
        if (detail && terrain && go
            && m_layerIndex >= 0
            && m_layerIndex < static_cast<int>(detail->layers.size()))
        {
            const float dt = ImGui::GetIO().DeltaTime;
            if (ApplyBrush(*detail, *terrain, go->transform, hitWorld, dt)) {
                m_strokeDirty  = true;
                m_strokeEntity = m_hitEntity;
            }
        }
    }
}

// =============================================================================
// レイキャスト
// =============================================================================

bool DetailTool::RaycastTerrain(
    scene::Scene&           scene,
    const renderer::Camera& camera,
    const ImVec2&           viewportMin,
    const ImVec2&           viewportSize,
    math::Vector3&          outHitWorld,
    scene::EntityID&        outEntity) const
{
    const ImVec2 mouse = ImGui::GetMousePos();
    const float ndcX = ((mouse.x - viewportMin.x) / viewportSize.x) * 2.0f - 1.0f;
    const float ndcY = 1.0f - ((mouse.y - viewportMin.y) / viewportSize.y) * 2.0f;

    const math::Matrix4 invVP = math::Matrix4::Inverse(
        camera.GetProjectionMatrix() * camera.GetViewMatrix());
    const math::Ray ray = math::Ray::FromNDC(ndcX, ndcY, camera.m_position, invVP);

    float bestT = 1e30f;
    scene::EntityID bestEID{};
    math::Vector3   bestHit;

    for (scene::EntityID eid : scene.GetEntities<scene::TerrainComponent>()) {
        auto* tc = scene.GetComponent<scene::TerrainComponent>(eid);
        auto* go = scene.GetGameObject(eid);
        if (!tc || !go || !tc->enabled || tc->heightData.empty()) continue;

        math::Vector3 localHit;
        if (!RaycastSingleTerrain(ray, *tc, go->transform, localHit)) continue;

        const math::Vector4 transformed =
            go->transform.GetWorldMatrix() * math::Vector4{ localHit.x, localHit.y, localHit.z, 1.0f };
        const math::Vector3 hitW = { transformed.x, transformed.y, transformed.z };
        const math::Vector3 toHit = {
            hitW.x - ray.origin.x,
            hitW.y - ray.origin.y,
            hitW.z - ray.origin.z
        };
        const float t = math::Vector3::Dot(toHit, ray.direction);
        if (t < bestT) {
            bestT   = t;
            bestEID = eid;
            bestHit = hitW;
        }
    }

    if (!scene.IsValid(bestEID)) return false;
    outHitWorld = bestHit;
    outEntity   = bestEID;
    return true;
}

bool DetailTool::RaycastSingleTerrain(
    const math::Ray&               ray,
    const scene::TerrainComponent& terrain,
    const scene::Transform&        tf,
    math::Vector3&                 outLocalHit) const
{
    // Terrain 描画・MeshCollider と同じ World Matrix の逆変換でレイをローカル化する。
    const math::Matrix4 invWorld = math::Matrix4::Inverse(tf.GetWorldMatrix());
    const math::Vector4 localOrigin4 =
        invWorld * math::Vector4{ ray.origin.x, ray.origin.y, ray.origin.z, 1.0f };
    const math::Vector4 localDir4 =
        invWorld * math::Vector4{ ray.direction.x, ray.direction.y, ray.direction.z, 0.0f };
    const math::Vector3 rayOriginLocal = { localOrigin4.x, localOrigin4.y, localOrigin4.z };
    const math::Vector3 rayDir = math::Vector3{
        localDir4.x, localDir4.y, localDir4.z
    }.Normalized();

    const float terrainW = static_cast<float>(terrain.columns - 1) * terrain.cellSize;
    const float terrainD = static_cast<float>(terrain.rows    - 1) * terrain.cellSize;
    const float minH     = -terrain.maxHeight;
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

    const float stepT   = terrain.cellSize / std::max(std::abs(rayDir.x), std::abs(rayDir.z));
    const int   maxStep = static_cast<int>((tMax - tMin) / stepT) + 2;

    float prevT = tMin, prevH = -1.0f;
    bool foundBracket = false;
    float bracketLo = tMin, bracketHi = tMin;

    for (int step = 0; step <= maxStep; ++step) {
        const float t = std::min(tMin + static_cast<float>(step) * stepT, tMax);
        const math::Vector3 p = {
            rayOriginLocal.x + rayDir.x * t,
            rayOriginLocal.y + rayDir.y * t,
            rayOriginLocal.z + rayDir.z * t
        };
        const float terrainH = terrain.GetHeightAt(p.x, p.z);

        if (step > 0 && prevH > 0.0f && p.y <= terrainH && prevT < tMax) {
            foundBracket = true;
            bracketLo    = prevT;
            bracketHi    = t;
            break;
        }
        prevT = t;
        prevH = p.y - terrainH;
        if (t >= tMax) break;
    }

    if (!foundBracket) return false;

    for (int i = 0; i < 8; ++i) {
        const float mid = (bracketLo + bracketHi) * 0.5f;
        const math::Vector3 p = {
            rayOriginLocal.x + rayDir.x * mid,
            rayOriginLocal.y + rayDir.y * mid,
            rayOriginLocal.z + rayDir.z * mid
        };
        if (p.y > terrain.GetHeightAt(p.x, p.z)) bracketLo = mid;
        else                                       bracketHi = mid;
    }

    const float tFinal = (bracketLo + bracketHi) * 0.5f;
    outLocalHit = {
        rayOriginLocal.x + rayDir.x * tFinal,
        terrain.GetHeightAt(rayOriginLocal.x + rayDir.x * tFinal,
                            rayOriginLocal.z + rayDir.z * tFinal),
        rayOriginLocal.z + rayDir.z * tFinal
    };
    return true;
}

// =============================================================================
// 密度マップ操作
// =============================================================================

void DetailTool::EnsureDensityMap(
    scene::TerrainDetailComponent& detail,
    const scene::TerrainComponent& terrain,
    int layerIdx) const
{
    if (layerIdx >= static_cast<int>(detail.densityMaps.size()))
        detail.densityMaps.resize(static_cast<size_t>(layerIdx) + 1);

    auto& dm = detail.densityMaps[static_cast<size_t>(layerIdx)];
    if (dm.IsValid()) return;

    const float terrainW = static_cast<float>(terrain.columns - 1) * terrain.cellSize;
    const float terrainD = static_cast<float>(terrain.rows    - 1) * terrain.cellSize;
    dm.width  = std::max(1, static_cast<int>(std::ceil(terrainW / kDensityTexelSize)));
    dm.height = std::max(1, static_cast<int>(std::ceil(terrainD / kDensityTexelSize)));
    // WHY: Paint/Erase の選択状態に関係なく、新規レイヤーは配置数 0 から開始する。
    dm.data.assign(
        static_cast<size_t>(dm.width) * static_cast<size_t>(dm.height), 0.0f);
}

bool DetailTool::ApplyBrush(
    scene::TerrainDetailComponent& detail,
    const scene::TerrainComponent& terrain,
    const scene::Transform&        tf,
    const math::Vector3&           hitWorld,
    float                          dt)
{
    EnsureDensityMap(detail, terrain, m_layerIndex);

    auto& dm = detail.densityMaps[static_cast<size_t>(m_layerIndex)];
    const float terrainW = static_cast<float>(terrain.columns - 1) * terrain.cellSize;
    const float terrainD = static_cast<float>(terrain.rows    - 1) * terrain.cellSize;

    // ブラシ範囲をテクセルに変換
    const math::Matrix4 invWorld = math::Matrix4::Inverse(tf.GetWorldMatrix());
    const math::Vector4 localHit =
        invWorld * math::Vector4{ hitWorld.x, hitWorld.y, hitWorld.z, 1.0f };
    const float localX = localHit.x;
    const float localZ = localHit.z;
    const float hitU   = localX / terrainW;
    const float hitV   = localZ / terrainD;

    const float texelW = terrainW / static_cast<float>(dm.width);
    const float texelH = terrainD / static_cast<float>(dm.height);

    const int rX = static_cast<int>(std::ceil(m_brush.radius / texelW));
    const int rZ = static_cast<int>(std::ceil(m_brush.radius / texelH));

    const int centerX = static_cast<int>(hitU * dm.width);
    const int centerZ = static_cast<int>(hitV * dm.height);

    const float delta = (m_mode == Mode::Paint ? 1.0f : -1.0f)
                      * m_brush.strength * dt;
    bool changed = false;

    for (int dz = -rZ; dz <= rZ; ++dz)
    for (int dx = -rX; dx <= rX; ++dx)
    {
        const int tx = centerX + dx;
        const int tz = centerZ + dz;
        if (tx < 0 || tx >= dm.width || tz < 0 || tz >= dm.height) continue;

        // テクセル中心のワールド距離でフォールオフ計算 (smoothstep)
        const float wx = (static_cast<float>(tx) + 0.5f) * texelW;
        const float wz = (static_cast<float>(tz) + 0.5f) * texelH;
        const float dist = std::sqrt(
            (wx - localX) * (wx - localX) + (wz - localZ) * (wz - localZ));
        if (dist >= m_brush.radius) continue;

        const float t = dist / m_brush.radius;
        const float weight = 1.0f - t * t * (3.0f - 2.0f * t); // smoothstep

        float& val = dm.data[static_cast<size_t>(tz) * static_cast<size_t>(dm.width) + static_cast<size_t>(tx)];
        const float newValue = std::max(0.0f, std::min(1.0f, val + delta * weight));
        if (newValue != val) {
            val = newValue;
            changed = true;
        }
    }

    return changed;
}

// =============================================================================
// ビューポートビジュアル
// =============================================================================

void DetailTool::DrawBrushPreview(
    const ImVec2&           viewportMin,
    const ImVec2&           viewportSize,
    const renderer::Camera& camera) const
{
    const math::Matrix4 vp = camera.GetViewProjection();
    auto project = [&](const math::Vector3& p) -> ImVec2 {
        const math::Vector4 clip = vp * math::Vector4{ p.x, p.y, p.z, 1.0f };
        if (clip.w < 0.001f) return { -99999.f, -99999.f };
        const float nx = clip.x / clip.w;
        const float ny = clip.y / clip.w;
        return {
            viewportMin.x + (nx + 1.0f) * 0.5f * viewportSize.x,
            viewportMin.y + (1.0f - (ny + 1.0f) * 0.5f) * viewportSize.y
        };
    };

    ImDrawList* dl   = ImGui::GetForegroundDrawList();
    const float r    = m_brush.radius;
    const int   segs = 32;
    const ImU32 col  = (m_mode == Mode::Paint)
                     ? IM_COL32(60, 220, 80, 230)   // Paint: 緑
                     : IM_COL32(220, 80, 60, 230);   // Erase: 赤

    ImVec2 prev = project({ m_hitPoint.x + r, m_hitPoint.y, m_hitPoint.z });
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

void DetailTool::DrawChunkDebug(
    scene::Scene&           scene,
    const ImVec2&           viewportMin,
    const ImVec2&           viewportSize,
    const renderer::Camera& camera) const
{
    if (!m_showChunkBounds && !m_showCounts) return;

    const math::Matrix4 vp = camera.GetViewProjection();
    auto project = [&](const math::Vector3& p) -> ImVec2 {
        const math::Vector4 clip = vp * math::Vector4{ p.x, p.y, p.z, 1.0f };
        if (clip.w < 0.001f) return { -99999.f, -99999.f };
        const float nx = clip.x / clip.w;
        const float ny = clip.y / clip.w;
        return {
            viewportMin.x + (nx + 1.0f) * 0.5f * viewportSize.x,
            viewportMin.y + (1.0f - (ny + 1.0f) * 0.5f) * viewportSize.y
        };
    };

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    for (scene::EntityID eid : scene.GetEntities<scene::TerrainDetailComponent>()) {
        auto* detail  = scene.GetComponent<scene::TerrainDetailComponent>(eid);
        auto* terrain = scene.GetComponent<scene::TerrainComponent>(eid);
        auto* go      = scene.GetGameObject(eid);
        if (!detail || !terrain || !go || !detail->enabled) continue;

        const math::Vector3 origin = {
            go->transform.position.x,
            go->transform.position.y,
            go->transform.position.z
        };
        const float terrainH = terrain->maxHeight;

        for (const auto& chunk : detail->chunks) {
            const float x0 = origin.x + static_cast<float>(chunk.chunkX) * kChunkSize;
            const float z0 = origin.z + static_cast<float>(chunk.chunkZ) * kChunkSize;
            const float x1 = x0 + kChunkSize;
            const float z1 = z0 + kChunkSize;
            const float yMid = origin.y + terrainH * 0.5f;

            // チャンク総インスタンス数を集計
            int totalInst = 0;
            for (const auto& v : chunk.instancesPerLayer)     totalInst += static_cast<int>(v.size());
            for (const auto& v : chunk.grassInstancesPerLayer) totalInst += static_cast<int>(v.size());

            // カメラが近い場合だけ描画する (遠景はノイズになる)
            const float camX = camera.m_position.x;
            const float camZ = camera.m_position.z;
            const float dx   = (x0 + x1) * 0.5f - camX;
            const float dz   = (z0 + z1) * 0.5f - camZ;
            const float dist = std::sqrt(dx * dx + dz * dz);
            if (dist > 120.0f) continue;

            if (m_showChunkBounds) {
                const ImU32 boxCol = IM_COL32(80, 200, 255, 100);
                const ImVec2 p00 = project({ x0, yMid, z0 });
                const ImVec2 p10 = project({ x1, yMid, z0 });
                const ImVec2 p11 = project({ x1, yMid, z1 });
                const ImVec2 p01 = project({ x0, yMid, z1 });
                dl->AddLine(p00, p10, boxCol, 1.0f);
                dl->AddLine(p10, p11, boxCol, 1.0f);
                dl->AddLine(p11, p01, boxCol, 1.0f);
                dl->AddLine(p01, p00, boxCol, 1.0f);
            }

            if (m_showCounts && totalInst > 0) {
                const ImVec2 labelPos = project({
                    (x0 + x1) * 0.5f, yMid + 0.2f, (z0 + z1) * 0.5f
                });
                if (labelPos.x > viewportMin.x && labelPos.x < viewportMin.x + viewportSize.x
                 && labelPos.y > viewportMin.y && labelPos.y < viewportMin.y + viewportSize.y)
                {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%d", totalInst);
                    dl->AddText(labelPos, IM_COL32(255, 255, 80, 200), buf);
                }
            }
        }
    }
}

// =============================================================================
// ImGui UI
// =============================================================================

void DetailTool::OnEditorGUI(
    scene::Scene& scene,
    const std::function<void()>& markDirty)
{
    const ImGuiViewport* mainVP = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        { mainVP->WorkPos.x + mainVP->WorkSize.x - 230.0f,
          mainVP->WorkPos.y + 10.0f },
        ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::SetNextWindowSize({ 260.0f, 0.0f }, ImGuiCond_FirstUseEver);

    const char* title = m_active ? "Detail Tool###DetailTool" : "Detail Tool [OFF]###DetailTool";
    if (!ImGui::Begin(title, nullptr, ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::End();
        return;
    }

    // ON/OFF
    {
        ImGui::PushStyleColor(ImGuiCol_Button,
            m_active ? ImVec4(0.2f, 0.6f, 0.2f, 1.0f) : ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
        if (ImGui::Button(m_active ? "  Active  " : " Inactive ", { -1.0f, 0.0f }))
            m_active = !m_active;
        ImGui::PopStyleColor();
    }

    if (!m_active) ImGui::BeginDisabled();
    DrawContent(scene, markDirty);
    if (!m_active) ImGui::EndDisabled();

    ImGui::End();
}

void DetailTool::DrawContent(
    scene::Scene& scene,
    const std::function<void()>& markDirty)
{
    // コンポーネント確認
    scene::TerrainDetailComponent* detail = nullptr;
    for (scene::EntityID eid : scene.GetEntities<scene::TerrainDetailComponent>()) {
        if (auto* d = scene.GetComponent<scene::TerrainDetailComponent>(eid))
            if (d->enabled) { detail = d; break; }
    }
    if (!detail) {
        ImGui::TextDisabled("No TerrainDetailComponent in scene.");
        ImGui::TextDisabled("Add it to a Terrain entity via Inspector.");
        return;
    }
    if (detail->layers.empty()) {
        ImGui::TextDisabled("TerrainDetailComponent has no layers.");
        ImGui::TextDisabled("Add layers in Inspector.");
        return;
    }

    // ─── レイヤー選択 ───
    ImGui::SeparatorText("Layers");

    const int layerCount = static_cast<int>(detail->layers.size());
    for (int li = 0; li < layerCount; ++li) {
        const auto& layer    = detail->layers[static_cast<size_t>(li)];
        const bool  selected = (m_layerIndex == li);

        const char* typeName = "Mesh";
        if (layer.type == scene::DetailLayerType::Billboard) typeName = "Bill";
        if (layer.type == scene::DetailLayerType::Grass)     typeName = "Grass";

        const std::string& assetPath = layer.meshPath.empty() ? layer.texturePath : layer.meshPath;
        const std::string  shortName = assetPath.empty()
            ? "(no asset)"
            : assetPath.substr(assetPath.find_last_of("/\\") + 1);

        char btnLabel[128];
        std::snprintf(btnLabel, sizeof(btnLabel), "[%s]  %s###DL_%d", typeName, shortName.c_str(), li);

        if (selected)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.55f, 0.75f, 1.0f));
        if (ImGui::Button(btnLabel, { -1.0f, 0.0f }))
            m_layerIndex = li;
        if (selected)
            ImGui::PopStyleColor();

        // 選択中レイヤーのインライン詳細
        if (selected) {
            ImGui::Indent(10.0f);

            const bool hasDM = li < static_cast<int>(detail->densityMaps.size())
                            && detail->densityMaps[static_cast<size_t>(li)].IsValid();
            if (hasDM) {
                const auto& dm = detail->densityMaps[static_cast<size_t>(li)];
                ImGui::TextDisabled("Density: %dx%d", dm.width, dm.height);
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear")) {
                    detail->densityMaps[static_cast<size_t>(li)] = {};
                    detail->needsBake = true;
                    markDirty();
                }
            } else {
                ImGui::TextDisabled("Density: empty");
            }

            int total = 0;
            for (const auto& chunk : detail->chunks) {
                if (li < static_cast<int>(chunk.instancesPerLayer.size()))
                    total += static_cast<int>(chunk.instancesPerLayer[static_cast<size_t>(li)].size());
                if (li < static_cast<int>(chunk.grassInstancesPerLayer.size()))
                    total += static_cast<int>(chunk.grassInstancesPerLayer[static_cast<size_t>(li)].size());
            }
            if (total > 0)
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%d instances", total);
            else
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "0 instances");

            // Mesh パス警告
            if (layer.type == scene::DetailLayerType::Mesh) {
                if (layer.meshPath.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.2f, 1.0f));
                    ImGui::TextWrapped("! Mesh Path not set.");
                    ImGui::PopStyleColor();
                } else if (!util::FileSystem::Exists(
                               asset::AssetManager::ResolveAssetPath(layer.meshPath))) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.25f, 0.2f, 1.0f));
                    ImGui::TextWrapped("! Not found: %s", layer.meshPath.c_str());
                    ImGui::PopStyleColor();
                }
            }

            ImGui::Unindent(10.0f);
        }
    }

    // ─── Paint / Erase ───
    ImGui::Spacing();
    ImGui::SeparatorText("Mode");
    {
        const float halfW  = (ImGui::GetContentRegionAvail().x - 4.0f) * 0.5f;
        const bool  isPaint = (m_mode == Mode::Paint);
        if (isPaint)  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
        if (ImGui::Button("Paint", { halfW, 0.0f })) m_mode = Mode::Paint;
        if (isPaint)  ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, 4.0f);
        if (!isPaint) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.25f, 0.25f, 1.0f));
        if (ImGui::Button("Erase", { -1.0f, 0.0f })) m_mode = Mode::Erase;
        if (!isPaint) ImGui::PopStyleColor();
    }

    // ─── ブラシ設定 ───
    ImGui::Spacing();
    ImGui::SeparatorText("Brush");
    ImGui::SliderFloat("Radius",   &m_brush.radius,   0.5f, 30.0f, "%.1f m");
    ImGui::SliderFloat("Strength", &m_brush.strength, 0.01f, 1.0f, "%.2f");
    ImGui::TextDisabled("[ / ] : Radius   Shift+[ / ] : Strength");

    // ─── Bake ───
    ImGui::Spacing();
    if (detail->needsBake) {
        const float t = fmodf(static_cast<float>(ImGui::GetTime()) * 0.7f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.30f, 0.75f, 0.40f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg,       ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
        ImGui::ProgressBar(t, ImVec2(-1.0f, 0.0f), "Baking...");
        ImGui::PopStyleColor(2);
    }
    if (ImGui::Button("Bake All Layers", { -1.0f, 0.0f })) {
        detail->needsBake = true;
        markDirty();
    }

    // ─── Debug (折りたたみ) ───
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Debug")) {
        ImGui::Checkbox("Chunk Bounds", &m_showChunkBounds);
        ImGui::SameLine();
        ImGui::Checkbox("Counts", &m_showCounts);
        ImGui::TextDisabled("Total chunks: %d", static_cast<int>(detail->chunks.size()));

        if (m_isHovering && scene.IsValid(m_hitEntity)) {
            if (!scene.GetComponent<scene::TerrainDetailComponent>(m_hitEntity)) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.2f, 1.0f));
                ImGui::TextWrapped("! Terrain under cursor has no TerrainDetailComponent.");
                ImGui::PopStyleColor();
            }
        } else if (!m_isHovering) {
            ImGui::TextDisabled("Hover over terrain to paint.");
        }
    }
}

} // namespace fbzz::editor
