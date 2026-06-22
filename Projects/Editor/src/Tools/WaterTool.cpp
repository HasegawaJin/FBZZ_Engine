// FBZZ Engine
// WaterTool.cpp | fbzz::editor
// WaterTool の実装: ビューポート可視化・アセット管理・波エディタ UI
#include "WaterTool.hpp"
#include <Editor/Util/UndoStack.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/TerrainWaterDefaults.hpp>
#include <imgui_internal.h>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector4.hpp>
#include <Math/MathUtils.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <imgui.h>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::editor {

namespace {

// WHAT: ワールド座標を vp 行列でクリップ空間に変換し、スクリーン座標を返す。
//       カメラ後方（w <= 0）の点は無効座標を返す。
ImVec2 ProjectWorld(const math::Vector3& p, const math::Matrix4& vp,
                    const ImVec2& vpMin, const ImVec2& vpSize)
{
    const math::Vector4 clip = vp * math::Vector4{ p.x, p.y, p.z, 1.0f };
    if (clip.w < 0.001f) return { -99999.f, -99999.f };
    const float ndcX = clip.x / clip.w;
    const float ndcY = clip.y / clip.w;
    return {
        vpMin.x + (ndcX + 1.0f) * 0.5f * vpSize.x,
        vpMin.y + (1.0f - (ndcY + 1.0f) * 0.5f) * vpSize.y
    };
}

bool IsOnScreen(const ImVec2& p, const ImVec2& vpMin, const ImVec2& vpSize)
{
    return p.x > vpMin.x - 1.0f && p.x < vpMin.x + vpSize.x + 1.0f
        && p.y > vpMin.y - 1.0f && p.y < vpMin.y + vpSize.y + 1.0f;
}

std::string NormalizeWaterAssetPath(std::string path)
{
    for (char& c : path) if (c == '\\') c = '/';
    return path;
}

std::vector<float>& EnsureParam(asset::MaterialAsset& mat, const char* key, size_t count)
{
    auto& values = mat.params[key];
    if (values.size() != count)
        values.assign(count, 0.0f);
    return values;
}

void SetParam1(asset::MaterialAsset& mat, const char* key, float x)
{
    auto& v = EnsureParam(mat, key, 1);
    v[0] = x;
}

void SetParam2(asset::MaterialAsset& mat, const char* key, float x, float y)
{
    auto& v = EnsureParam(mat, key, 2);
    v[0] = x;
    v[1] = y;
}

void SetParam3(asset::MaterialAsset& mat, const char* key, float x, float y, float z)
{
    auto& v = EnsureParam(mat, key, 3);
    v[0] = x;
    v[1] = y;
    v[2] = z;
}

asset::MaterialAsset* LoadWaterMaterial(scene::WaterComponent& water)
{
    if (water.materialPath.empty())
        return nullptr;
    const auto handle = asset::AssetManager::LoadMaterial(water.materialPath);
    return asset::AssetManager::GetMaterial(handle);
}

bool SaveWaterMaterial(scene::WaterComponent& water, const std::string& projectRoot)
{
    asset::MaterialAsset* mat = LoadWaterMaterial(water);
    if (!mat)
        return false;
    return asset::SaveMaterialAssetToFile(ToProjectAssetDiskPath(projectRoot, water.materialPath), *mat);
}

void MarkWaterVisualDirty(scene::WaterComponent& water)
{
    water.texDirty = true;
    water.foamDirty = true;
}

void ApplyWaterLookPreset(scene::WaterComponent& water, const std::string& projectRoot, int preset)
{
    asset::MaterialAsset* mat = LoadWaterMaterial(water);
    if (!mat)
        return;

    if (preset == 0) {
        SetParam3(*mat, "shallowColor", 0.09f, 0.56f, 0.63f);
        SetParam3(*mat, "deepColor",    0.00f, 0.14f, 0.32f);
        SetParam1(*mat, "shallowDepth", 1.20f);
        SetParam1(*mat, "deepDepth",    8.00f);
        SetParam1(*mat, "opacity",      0.55f);
        SetParam1(*mat, "reflectivity", 0.42f);
        SetParam1(*mat, "fresnelPower", 4.20f);
        SetParam1(*mat, "refractionStrength", 0.020f);
        SetParam1(*mat, "normalStrength", 0.75f);
        SetParam1(*mat, "foamStrength", 0.55f);
        SetParam1(*mat, "rimGlowStrength", 0.32f);
        SetParam1(*mat, "minShallowAlpha", 0.48f);
        SetParam1(*mat, "specularStrength", 0.65f);
        SetParam1(*mat, "specularExponent", 96.0f);
        SetParam3(*mat, "skyReflectTint", 0.48f, 0.78f, 0.92f);
        SetParam1(*mat, "envMapBlend", 0.28f);
    } else if (preset == 1) {
        SetParam3(*mat, "shallowColor", 0.16f, 0.48f, 0.58f);
        SetParam3(*mat, "deepColor",    0.02f, 0.08f, 0.18f);
        SetParam1(*mat, "shallowDepth", 0.65f);
        SetParam1(*mat, "deepDepth",    4.50f);
        SetParam1(*mat, "opacity",      0.68f);
        SetParam1(*mat, "reflectivity", 0.28f);
        SetParam1(*mat, "fresnelPower", 5.50f);
        SetParam1(*mat, "refractionStrength", 0.012f);
        SetParam1(*mat, "normalStrength", 0.35f);
        SetParam1(*mat, "foamStrength", 0.18f);
        SetParam1(*mat, "rimGlowStrength", 0.22f);
        SetParam1(*mat, "minShallowAlpha", 0.56f);
        SetParam1(*mat, "specularStrength", 0.38f);
        SetParam1(*mat, "specularExponent", 128.0f);
        SetParam3(*mat, "skyReflectTint", 0.42f, 0.70f, 0.82f);
        SetParam1(*mat, "envMapBlend", 0.20f);
    } else {
        SetParam3(*mat, "shallowColor", 0.06f, 0.38f, 0.34f);
        SetParam3(*mat, "deepColor",    0.01f, 0.10f, 0.09f);
        SetParam1(*mat, "shallowDepth", 0.40f);
        SetParam1(*mat, "deepDepth",    2.25f);
        SetParam1(*mat, "opacity",      0.62f);
        SetParam1(*mat, "reflectivity", 0.18f);
        SetParam1(*mat, "fresnelPower", 6.50f);
        SetParam1(*mat, "refractionStrength", 0.018f);
        SetParam1(*mat, "normalStrength", 0.50f);
        SetParam1(*mat, "foamStrength", 0.28f);
        SetParam2(*mat, "normalMap1Scroll", 0.00f, 0.035f);
        SetParam2(*mat, "normalMap2Scroll", 0.015f, 0.045f);
        SetParam1(*mat, "rimGlowStrength", 0.18f);
        SetParam1(*mat, "minShallowAlpha", 0.52f);
        SetParam1(*mat, "specularStrength", 0.30f);
        SetParam1(*mat, "specularExponent", 80.0f);
        SetParam3(*mat, "skyReflectTint", 0.38f, 0.62f, 0.58f);
        SetParam1(*mat, "envMapBlend", 0.16f);
    }

    MarkWaterVisualDirty(water);
    (void)SaveWaterMaterial(water, projectRoot);
}

} // namespace

// =============================================================================
// Update — ビューポート可視化
// =============================================================================

void WaterTool::Update(
    scene::Scene&               scene,
    const renderer::Camera&     camera,
    const ImVec2&               vpMin,
    const ImVec2&               vpSize,
    const std::function<void()>& /*markDirty*/)
{
    if (!m_active) return;

    const math::Matrix4 vp = camera.GetViewProjection();
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    uint32_t waterIdx = 0;
    for (auto [water, transform] : scene.View<scene::WaterComponent, scene::Transform>()) {
        if (!water.enabled) { ++waterIdx; continue; }

        const bool isSelected = (waterIdx == m_selectedWaterIndex);

        // 水面範囲をワイヤーフレーム矩形で表示する
        // 選択中は明るい水色、非選択は半透明のグレーで描く
        const ImU32 boundsColor = isSelected
            ? IM_COL32(80, 200, 255, 200)
            : IM_COL32(120, 180, 200, 80);

        DrawWaterBounds(water, transform, vp, vpMin, vpSize);
        (void)boundsColor; // DrawWaterBounds 内で使用

        // 選択中の水面のみ波向き矢印を描画する
        if (isSelected)
            DrawWaveArrows(water, transform, vp, vpMin, vpSize);

        ++waterIdx;
    }
}

void WaterTool::DrawWaterBounds(
    const scene::WaterComponent& water,
    const scene::Transform&      tf,
    const math::Matrix4&         vp,
    const ImVec2&                vpMin,
    const ImVec2&                vpSize) const
{
    // WHAT: 水面の 4 隅をワールド座標で計算し、スクリーンに投影して矩形を描く。
    const float hx = water.extentX * 0.5f;
    const float hz = water.extentZ * 0.5f;
    const float y  = tf.position.y;

    const math::Vector3 corners[4] = {
        { tf.position.x - hx, y, tf.position.z - hz },
        { tf.position.x + hx, y, tf.position.z - hz },
        { tf.position.x + hx, y, tf.position.z + hz },
        { tf.position.x - hx, y, tf.position.z + hz },
    };

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImU32 col = IM_COL32(80, 200, 255, 160);

    for (int i = 0; i < 4; ++i) {
        const ImVec2 a = ProjectWorld(corners[i],        vp, vpMin, vpSize);
        const ImVec2 b = ProjectWorld(corners[(i+1)%4],  vp, vpMin, vpSize);
        if (IsOnScreen(a, vpMin, vpSize) || IsOnScreen(b, vpMin, vpSize))
            dl->AddLine(a, b, col, 1.5f);
    }

    // 中心に小さな十字マーカーを描く
    const ImVec2 center = ProjectWorld(tf.position, vp, vpMin, vpSize);
    if (IsOnScreen(center, vpMin, vpSize)) {
        constexpr float kCross = 5.0f;
        dl->AddLine({ center.x - kCross, center.y }, { center.x + kCross, center.y }, col, 1.0f);
        dl->AddLine({ center.x, center.y - kCross }, { center.x, center.y + kCross }, col, 1.0f);
    }
}

void WaterTool::DrawWaveArrows(
    const scene::WaterComponent& water,
    const scene::Transform&      tf,
    const math::Matrix4&         vp,
    const ImVec2&                vpMin,
    const ImVec2&                vpSize) const
{
    if (!water.enableGerstnerWaves) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const math::Vector3 origin = tf.position;

    // WHY: 波ごとに振幅に比例した長さの矢印を描く。小さい波は短く、大きい波は長く見える。
    //      波の向きは XZ 平面（水面）に沿った方向ベクトルとして描く。
    const ImU32 colors[4] = {
        IM_COL32(255, 230, 80,  220),
        IM_COL32(255, 160, 80,  200),
        IM_COL32(80,  255, 160, 200),
        IM_COL32(200, 80,  255, 200),
    };

    for (int i = 0; i < 4; ++i) {
        const scene::GerstnerWave& w = water.waves[static_cast<size_t>(i)];
        if (w.amplitude < 0.0001f || w.wavelength < 0.0001f) continue;

        const math::Vector2 dir = w.direction.Normalized();
        const float arrowLen = std::min(w.amplitude * 20.0f, water.extentX * 0.4f);

        const math::Vector3 tip = {
            origin.x + dir.x * arrowLen,
            origin.y,
            origin.z + dir.y * arrowLen,
        };

        const ImVec2 pOrigin = ProjectWorld(origin, vp, vpMin, vpSize);
        const ImVec2 pTip    = ProjectWorld(tip,    vp, vpMin, vpSize);

        if (!IsOnScreen(pTip, vpMin, vpSize) && !IsOnScreen(pOrigin, vpMin, vpSize))
            continue;

        dl->AddLine(pOrigin, pTip, colors[i], 2.0f);

        // 矢じり（tip から後ろ向き 15° の 2 本線）
        const float dx = pTip.x - pOrigin.x;
        const float dy = pTip.y - pOrigin.y;
        const float len = std::sqrt(dx*dx + dy*dy);
        if (len > 0.5f) {
            const float nx = -dx / len * 10.0f;
            const float ny = -dy / len * 10.0f;
            const float perp = 0.27f; // tan(15°)
            dl->AddLine(pTip, { pTip.x + nx - ny*perp, pTip.y + ny + nx*perp }, colors[i], 2.0f);
            dl->AddLine(pTip, { pTip.x + nx + ny*perp, pTip.y + ny - nx*perp }, colors[i], 2.0f);
        }
    }
}

// =============================================================================
// OnEditorGUI — ツールウィンドウ
// =============================================================================

// =============================================================================
// DrawContent — ウィンドウなしのタブコンテンツ描画 (NatureTool から呼ぶ)
// =============================================================================

void WaterTool::DrawContent(
    scene::Scene&                scene,
    const std::string&           projectRoot,
    const std::function<void()>& markDirty,
    UndoStack*                   undoStack)
{
    // WaterComponent がなければ何もしない
    bool hasWater = false;
    for (scene::EntityID eid : scene.GetEntities<scene::WaterComponent>())
        if (auto* w = scene.GetComponent<scene::WaterComponent>(eid))
            if (w->enabled) { hasWater = true; break; }
    if (!hasWater) {
        ImGui::TextDisabled("No WaterComponent in scene.");
        if (ImGui::Button("Create Water", { -1.0f, 0.0f })) {
            auto& go = scene.CreateGameObject("Water");
            scene::WaterComponent water{};
            water.resolutionX = 96;
            water.resolutionZ = 96;
            water.extentX = 80.0f;
            water.extentZ = 80.0f;
            water.chunkCount = 4;
            water.materialPath = DefaultWaterMaterialPath();
            water.meshDirty = true;
            water.foamDirty = true;
            water.texDirty = true;
            go.AddComponent<scene::WaterComponent>(std::move(water));
            if (markDirty)
                markDirty();
        }
        return;
    }

    // ON/OFF は NatureTool 側で管理するため、ここでは m_active を参照せず常に表示
    DrawContentBody(scene, projectRoot, markDirty, undoStack);
}

void WaterTool::DrawContentBody(
    scene::Scene&                scene,
    const std::string&           projectRoot,
    const std::function<void()>& markDirty,
    UndoStack*                   undoStack)
{
    std::vector<std::pair<scene::EntityID, std::string>> waterList;
    for (scene::EntityID eid : scene.GetEntities<scene::WaterComponent>())
        if (auto* go = scene.GetGameObject(eid))
            waterList.emplace_back(eid, go->name);

    if (m_selectedWaterIndex >= static_cast<uint32_t>(waterList.size()))
        m_selectedWaterIndex = 0;

    if (waterList.empty()) return;

    ImGui::SeparatorText("Target");
    const std::string& curName = waterList[m_selectedWaterIndex].second;
    if (ImGui::BeginCombo("##water_select", curName.c_str())) {
        for (uint32_t i = 0; i < static_cast<uint32_t>(waterList.size()); ++i) {
            const bool sel = (i == m_selectedWaterIndex);
            if (ImGui::Selectable(waterList[i].second.c_str(), sel)) m_selectedWaterIndex = i;
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    scene::EntityID selEid = waterList[m_selectedWaterIndex].first;
    if (auto* water = scene.GetComponent<scene::WaterComponent>(selEid)) {
        struct UndoTracker {
            ImGuiID activeId = 0; std::string instanceId;
            scene::WaterComponent before; bool active = false; bool changed = false;
        };
        static UndoTracker undo;
        const bool canRecordUndo = undoStack != nullptr && undoStack->IsRecordingEnabled();
        scene::WaterComponent beforeDraw;
        if (canRecordUndo) beforeDraw = *water; else undo.active = false;
        const ImGuiID activeBefore = ImGui::GetActiveID();
        bool changed = false;
        const auto trackDirty = [&]() { changed = true; if (markDirty) markDirty(); };

        ImGui::Spacing();
        DrawAssetSection(*water, "", projectRoot, trackDirty);
        ImGui::Spacing();
        DrawPresets(*water, trackDirty);
        ImGui::Spacing();
        DrawLookPresets(*water, projectRoot, trackDirty);
        ImGui::Spacing();
        DrawWaveEditor(*water, trackDirty);

        const ImGuiID activeAfter = ImGui::GetActiveID();
        auto pushCommand = [&](const std::string& iid,
                               const scene::WaterComponent& bef,
                               const scene::WaterComponent& aft) {
            if (!undoStack) return;
            scene::Scene* scenePtr = &scene;
            auto apply = [scenePtr, iid, markDirty](const scene::WaterComponent& val) {
                if (auto* target = scenePtr->FindByGuid(iid))
                    if (auto* comp = target->GetComponent<scene::WaterComponent>()) {
                        *comp = val; comp->meshDirty = true; comp->foamDirty = true; comp->texDirty = true;
                        if (markDirty) markDirty();
                    }
            };
            undoStack->Push(std::make_unique<LambdaCommand>("Edit Water",
                [apply, aft]()  { apply(aft); },
                [apply, bef]()  { apply(bef); }));
        };

        const std::string instanceId = scene.GetGameObject(selEid)->instanceId;
        if (!canRecordUndo) { undo.active = false; undo.changed = false; }
        else if (!undo.active && activeAfter != 0 && activeAfter != activeBefore) {
            undo = { activeAfter, instanceId, beforeDraw, true, changed };
        } else if (undo.active && activeAfter == undo.activeId) {
            undo.changed |= changed;
        } else if (undo.active && activeAfter != undo.activeId) {
            if (undo.changed) pushCommand(undo.instanceId, undo.before, *water);
            undo.active = false; undo.changed = false;
        } else if (!undo.active && changed && activeAfter == 0) {
            pushCommand(instanceId, beforeDraw, *water);
        }
    }
}

// =============================================================================
// OnEditorGUI — スタンドアローン用ウィンドウ (既存互換)
// =============================================================================

void WaterTool::OnEditorGUI(
    scene::Scene&                scene,
    const std::string&           projectRoot,
    const std::function<void()>& markDirty,
    UndoStack*                   undoStack)
{
    bool hasWater = false;
    for (scene::EntityID eid : scene.GetEntities<scene::WaterComponent>())
        if (auto* w = scene.GetComponent<scene::WaterComponent>(eid))
            if (w->enabled) { hasWater = true; break; }
    if (!hasWater) return;

    const ImGuiViewport* mainVP = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        { mainVP->WorkPos.x + mainVP->WorkSize.x - 270.0f,
          mainVP->WorkPos.y + mainVP->WorkSize.y - 10.0f },
        ImGuiCond_FirstUseEver, { 1.0f, 1.0f });
    ImGui::SetNextWindowBgAlpha(0.87f);
    ImGui::SetNextWindowSize({ 270.0f, 0.0f }, ImGuiCond_FirstUseEver);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;
    const char* title = m_active ? "Water Tool###WaterTool" : "Water Tool [OFF]###WaterTool";
    if (!ImGui::Begin(title, nullptr, kFlags)) { ImGui::End(); return; }

    {
        if (m_active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.5f, 0.8f, 1.0f));
        else          ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
        if (ImGui::Button(m_active ? "  Active  " : " Inactive ", { -1.0f, 0.0f }))
            m_active = !m_active;
        ImGui::PopStyleColor();
    }
    if (!m_active) ImGui::BeginDisabled();
    DrawContentBody(scene, projectRoot, markDirty, undoStack);
    if (!m_active) ImGui::EndDisabled();
    ImGui::End();
}

// =============================================================================
// Material (.mat) 参照セクション
// =============================================================================

void WaterTool::DrawAssetSection(
    scene::WaterComponent&       water,
    const std::string&           /*scenePath*/,
    const std::string&           /*projectRoot*/,
    const std::function<void()>& markDirty)
{
    ImGui::SeparatorText("Material (.mat)");

    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s", water.materialPath.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##water_mat", buf, sizeof(buf))) {
        water.materialPath = NormalizeWaterAssetPath(buf);
        MarkWaterVisualDirty(water);
        markDirty();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            std::string dropped = NormalizeWaterAssetPath(static_cast<const char*>(p->Data));
            if (util::StringUtils::EndsWith(dropped, ".mat")) {
                water.materialPath = dropped;
                MarkWaterVisualDirty(water);
                markDirty();
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (water.materialPath.empty())
        ImGui::TextDisabled("(no material — visual params missing)");
}

// =============================================================================
// 波プリセットセクション（ジオメトリ/波パラメータのみ設定。視覚パラメータは fzmat で管理）
// =============================================================================

void WaterTool::DrawPresets(
    scene::WaterComponent&       water,
    const std::function<void()>& markDirty) const
{
    ImGui::SeparatorText("Wave Presets");
    ImGui::TextDisabled("Wave parameters only (visual params are in .mat):");

    if (ImGui::Button("Ocean")) {
        water.enableGerstnerWaves = true;
        water.waves[0] = { {  1.00f, 0.20f }, 0.35f, 14.0f, 0.40f };
        water.waves[1] = { { -0.30f, 0.95f }, 0.20f, 22.0f, 0.30f };
        water.waves[2] = { {  0.70f,-0.70f }, 0.15f,  9.0f, 0.25f };
        water.waves[3] = { { -0.90f, 0.40f }, 0.10f, 18.0f, 0.20f };
        water.meshDirty = true;
        markDirty();
    }
    ImGui::SameLine();

    if (ImGui::Button("Lake")) {
        water.enableGerstnerWaves = true;
        water.waves[0] = { {  1.00f, 0.10f }, 0.08f,  6.0f, 0.25f };
        water.waves[1] = { { -0.20f, 1.00f }, 0.06f,  9.0f, 0.20f };
        water.waves[2] = { {  0.50f,-0.50f }, 0.04f,  4.0f, 0.15f };
        water.waves[3] = { { -0.80f, 0.30f }, 0.03f,  7.0f, 0.10f };
        water.meshDirty = true;
        markDirty();
    }
    ImGui::SameLine();

    if (ImGui::Button("River")) {
        water.enableGerstnerWaves = true;
        water.waves[0] = { {  0.00f, 1.00f }, 0.10f,  5.0f, 0.30f };
        water.waves[1] = { {  0.10f, 0.99f }, 0.07f,  3.0f, 0.25f };
        water.waves[2] = { { -0.10f, 1.00f }, 0.05f,  7.0f, 0.20f };
        water.waves[3] = { {  0.05f, 0.99f }, 0.03f,  2.5f, 0.15f };
        water.meshDirty = true;
        markDirty();
    }
    ImGui::SameLine();

    if (ImGui::Button("Flat")) {
        water.enableGerstnerWaves = false;
        for (auto& w : water.waves) w.amplitude = 0.0f;
        water.meshDirty = true;
        markDirty();
    }
}

// =============================================================================
// 波エディタセクション
// =============================================================================

void WaterTool::DrawLookPresets(
    scene::WaterComponent&       water,
    const std::string&           projectRoot,
    const std::function<void()>& markDirty) const
{
    ImGui::SeparatorText("Look Presets");
    if (water.materialPath.empty()) {
        ImGui::TextDisabled("Assign a water .mat to edit visual presets.");
        return;
    }

    if (ImGui::Button("Clear Coastal", { -1.0f, 0.0f })) {
        ApplyWaterLookPreset(water, projectRoot, 0);
        markDirty();
    }
    if (ImGui::Button("Calm Lake", { -1.0f, 0.0f })) {
        ApplyWaterLookPreset(water, projectRoot, 1);
        markDirty();
    }
    if (ImGui::Button("River Green", { -1.0f, 0.0f })) {
        ApplyWaterLookPreset(water, projectRoot, 2);
        markDirty();
    }

    ImGui::TextDisabled("Updates the referenced .mat and refreshes water textures.");
}

void WaterTool::DrawWaveEditor(
    scene::WaterComponent&       water,
    const std::function<void()>& markDirty) const
{
    ImGui::SeparatorText("Waves");
    bool changed = false;

    changed |= ImGui::Checkbox("Enable Gerstner Waves", &water.enableGerstnerWaves);

    if (!water.enableGerstnerWaves) ImGui::BeginDisabled();

    // WHAT: 4 本の Gerstner 波を TreeNode で折り畳み表示する。
    //       方向は -1～1 の DragFloat2 で編集し、ビューポートの矢印と連動する。
    static const ImU32 waveColors[4] = {
        IM_COL32(255, 230, 80,  255),
        IM_COL32(255, 160, 80,  255),
        IM_COL32(80,  255, 160, 255),
        IM_COL32(200, 80,  255, 255),
    };

    for (int i = 0; i < 4; ++i) {
        scene::GerstnerWave& w = water.waves[static_cast<size_t>(i)];
        ImGui::PushID(i);

        // 波のカラーインジケータ付きツリーノード
        ImGui::PushStyleColor(ImGuiCol_Text, waveColors[i]);
        const bool open = ImGui::TreeNodeEx("##wave", ImGuiTreeNodeFlags_DefaultOpen,
                                             "Wave %d  (A=%.2f)", i, w.amplitude);
        ImGui::PopStyleColor();

        if (open) {
            float dir[2] = { w.direction.x, w.direction.y };
            if (ImGui::DragFloat2("Direction", dir, 0.01f, -1.0f, 1.0f)) {
                w.direction = { dir[0], dir[1] };
                changed = true;
            }
            changed |= ImGui::DragFloat("Amplitude",  &w.amplitude,  0.01f,  0.0f, 50.0f);
            changed |= ImGui::DragFloat("Wavelength", &w.wavelength, 0.1f,   0.01f, 5000.0f);
            changed |= ImGui::DragFloat("Steepness",  &w.steepness,  0.01f,  0.0f, 1.0f);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    if (!water.enableGerstnerWaves) ImGui::EndDisabled();

    if (changed) markDirty();
}

// =============================================================================
// ProjectToScreen（公開ヘルパー、実体は ProjectWorld に委譲）
// =============================================================================

ImVec2 WaterTool::ProjectToScreen(
    const math::Vector3& p,
    const math::Matrix4& vp,
    const ImVec2&        vpMin,
    const ImVec2&        vpSize)
{
    return ProjectWorld(p, vp, vpMin, vpSize);
}

} // namespace fbzz::editor
