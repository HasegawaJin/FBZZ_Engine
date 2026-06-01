// FBZZ Engine
// WaterTool.cpp | fbzz::editor
// WaterTool の実装: ビューポート可視化・アセット管理・波エディタ UI
#include "WaterTool.hpp"
#include <Engine/Scene/WaterAssetSerializer.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector4.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <imgui.h>

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

// WHAT: .fbzzwater ファイルのデフォルト保存パスを生成する。
//       "Assets/Water/<gameobject名>.fbzzwater" の形式。
std::string MakeDefaultWaterAssetPath(const std::string& goName)
{
    std::string safe = goName;
    for (char& c : safe) {
        if (c == ' ' || c == '/' || c == '\\') c = '_';
    }
    return "Assets/Water/" + safe + ".fbzzwater";
}

// WHAT: プロジェクトルートからの相対パスをディスク上の絶対パスに変換する。
std::string ResolveToProject(const std::string& projectRoot, const std::string& assetPath)
{
    if (assetPath.empty()) return {};
    std::filesystem::path root(projectRoot);
    // Assets/ で始まる場合はプロジェクトルート配下を仮定する
    std::string normalized = assetPath;
    for (char& c : normalized) if (c == '\\') c = '/';
    return (root / normalized).string();
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

void WaterTool::OnEditorGUI(
    scene::Scene&                scene,
    const std::string&           projectRoot,
    const std::function<void()>& markDirty)
{
    // WaterComponent を持つ GO がひとつもなければ表示しない
    bool hasWater = false;
    for (scene::EntityID eid : scene.GetEntities<scene::WaterComponent>()) {
        if (auto* w = scene.GetComponent<scene::WaterComponent>(eid)) {
            if (w->enabled) { hasWater = true; break; }
        }
    }
    if (!hasWater) return;

    // WHY: 初回のみ右下に配置し、以降はドラッグで任意の位置に移動できる。
    //      TerrainTool と同じ配置方針。
    const ImGuiViewport* mainVP = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        { mainVP->WorkPos.x + mainVP->WorkSize.x - 270.0f,
          mainVP->WorkPos.y + mainVP->WorkSize.y - 10.0f },
        ImGuiCond_FirstUseEver, { 1.0f, 1.0f });
    ImGui::SetNextWindowBgAlpha(0.87f);
    ImGui::SetNextWindowSize({ 270.0f, 0.0f }, ImGuiCond_FirstUseEver);

    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoNav              |
        ImGuiWindowFlags_NoSavedSettings    |
        ImGuiWindowFlags_NoDocking          |
        ImGuiWindowFlags_NoFocusOnAppearing;

    const char* title = m_active ? "Water Tool" : "Water Tool [OFF]";
    if (!ImGui::Begin(title, nullptr, kFlags)) { ImGui::End(); return; }

    // ── ON/OFF トグル ──────────────────────────────────────────────────────
    {
        if (m_active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.5f, 0.8f, 1.0f));
        else          ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
        if (ImGui::Button(m_active ? "  Active  " : " Inactive ", { -1.0f, 0.0f }))
            m_active = !m_active;
        ImGui::PopStyleColor();
    }

    if (!m_active) ImGui::BeginDisabled();

    // ── 対象水面の選択 ─────────────────────────────────────────────────────
    {
        ImGui::Spacing();
        ImGui::SeparatorText("Target");

        // シーン内の WaterComponent を収集する
        std::vector<std::pair<scene::EntityID, std::string>> waterList;
        for (scene::EntityID eid : scene.GetEntities<scene::WaterComponent>()) {
            if (auto* go = scene.GetGameObject(eid)) {
                waterList.emplace_back(eid, go->name);
            }
        }

        // インデックスが範囲外になっていたら先頭に戻す
        if (m_selectedWaterIndex >= static_cast<uint32_t>(waterList.size()))
            m_selectedWaterIndex = 0;

        if (!waterList.empty()) {
            const std::string& curName = waterList[m_selectedWaterIndex].second;
            if (ImGui::BeginCombo("##water_select", curName.c_str())) {
                for (uint32_t i = 0; i < static_cast<uint32_t>(waterList.size()); ++i) {
                    const bool sel = (i == m_selectedWaterIndex);
                    if (ImGui::Selectable(waterList[i].second.c_str(), sel))
                        m_selectedWaterIndex = i;
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            // 選択中の WaterComponent を取得して編集 UI を表示する
            scene::EntityID selEid = waterList[m_selectedWaterIndex].first;
            if (auto* water = scene.GetComponent<scene::WaterComponent>(selEid)) {
                ImGui::Spacing();
                DrawAssetSection(*water, /*scenePath=*/"", projectRoot, markDirty);
                ImGui::Spacing();
                DrawPresets(*water, markDirty);
                ImGui::Spacing();
                DrawWaveEditor(*water, markDirty);
            }
        }
    }

    if (!m_active) ImGui::EndDisabled();
    ImGui::End();
}

// =============================================================================
// アセット管理セクション
// =============================================================================

void WaterTool::DrawAssetSection(
    scene::WaterComponent&       water,
    const std::string&           /*scenePath*/,
    const std::string&           projectRoot,
    const std::function<void()>& markDirty)
{
    ImGui::SeparatorText("Asset (.fbzzwater)");

    // パス入力
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s", water.waterAssetPath.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##asset_path", buf, sizeof(buf))) {
        water.waterAssetPath = buf;
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ASSET_PATH")) {
            std::string dropped = static_cast<const char*>(p->Data);
            // .fbzzwater だけ受け付ける
            if (dropped.size() > 10 &&
                dropped.substr(dropped.size() - 10) == ".fbzzwater")
            {
                water.waterAssetPath = dropped;
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Create ボタン: デフォルト名で新規 .fbzzwater を作成する
    if (ImGui::Button("Create")) {
        if (water.waterAssetPath.empty())
            water.waterAssetPath = MakeDefaultWaterAssetPath("Water");
        const std::string diskPath = ResolveToProject(projectRoot, water.waterAssetPath);
        if (scene::WaterAssetSerializer::Save(water, diskPath))
            markDirty();
    }
    ImGui::SameLine();

    // Save ボタン: 現在のパラメータを外部ファイルに書き出す
    const bool canSave = !water.waterAssetPath.empty();
    if (!canSave) ImGui::BeginDisabled();
    if (ImGui::Button("Save")) {
        const std::string diskPath = ResolveToProject(projectRoot, water.waterAssetPath);
        if (scene::WaterAssetSerializer::Save(water, diskPath))
            markDirty();
    }
    if (!canSave) ImGui::EndDisabled();
    ImGui::SameLine();

    // Load ボタン: 外部ファイルから再読み込みする
    if (!canSave) ImGui::BeginDisabled();
    if (ImGui::Button("Load")) {
        const std::string diskPath = ResolveToProject(projectRoot, water.waterAssetPath);
        if (scene::WaterAssetSerializer::Load(diskPath, water))
            markDirty();
    }
    if (!canSave) ImGui::EndDisabled();
    ImGui::SameLine();

    // Unlink ボタン: パスを削除してインライン保存モードに戻す
    if (!canSave) ImGui::BeginDisabled();
    if (ImGui::Button("Unlink")) {
        water.waterAssetPath.clear();
        markDirty();
    }
    if (!canSave) ImGui::EndDisabled();

    if (water.waterAssetPath.empty())
        ImGui::TextDisabled("(inline mode — scene file stores all params)");
}

// =============================================================================
// 波プリセットセクション
// =============================================================================

void WaterTool::DrawPresets(
    scene::WaterComponent&       water,
    const std::function<void()>& markDirty) const
{
    ImGui::SeparatorText("Presets");
    ImGui::TextDisabled("Apply preset:");

    // Ocean プリセット: 大きなうねり、深い青
    if (ImGui::Button("Ocean")) {
        water.shallowColor  = { 0.10f, 0.45f, 0.65f };
        water.deepColor     = { 0.00f, 0.05f, 0.20f };
        water.shallowDepth  = 0.8f;  water.deepDepth  = 12.0f;
        water.opacity       = 0.90f; water.reflectivity = 0.65f;
        water.fresnelPower  = 5.0f;
        water.normalStrength = 1.2f;
        water.enableGerstnerWaves = true;
        water.waves[0] = { {  1.00f, 0.20f }, 0.35f, 14.0f, 0.40f };
        water.waves[1] = { { -0.30f, 0.95f }, 0.20f, 22.0f, 0.30f };
        water.waves[2] = { {  0.70f,-0.70f }, 0.15f,  9.0f, 0.25f };
        water.waves[3] = { { -0.90f, 0.40f }, 0.10f, 18.0f, 0.20f };
        water.foamThreshold = 0.2f; water.foamStrength = 1.2f;
        water.meshDirty = true; water.foamDirty = true;
        markDirty();
    }
    ImGui::SameLine();

    // Lake プリセット: 穏やかな波、透明度高め
    if (ImGui::Button("Lake")) {
        water.shallowColor  = { 0.25f, 0.65f, 0.70f };
        water.deepColor     = { 0.00f, 0.15f, 0.35f };
        water.shallowDepth  = 0.4f;  water.deepDepth  = 4.0f;
        water.opacity       = 0.78f; water.reflectivity = 0.45f;
        water.fresnelPower  = 4.5f;
        water.normalStrength = 0.7f;
        water.enableGerstnerWaves = true;
        water.waves[0] = { {  1.00f, 0.10f }, 0.08f,  6.0f, 0.25f };
        water.waves[1] = { { -0.20f, 1.00f }, 0.06f,  9.0f, 0.20f };
        water.waves[2] = { {  0.50f,-0.50f }, 0.04f,  4.0f, 0.15f };
        water.waves[3] = { { -0.80f, 0.30f }, 0.03f,  7.0f, 0.10f };
        water.foamThreshold = 0.35f; water.foamStrength = 0.7f;
        water.meshDirty = true; water.foamDirty = true;
        markDirty();
    }
    ImGui::SameLine();

    // River プリセット: フローマップ想定、中程度の流れ感
    if (ImGui::Button("River")) {
        water.shallowColor  = { 0.30f, 0.70f, 0.60f };
        water.deepColor     = { 0.05f, 0.20f, 0.30f };
        water.shallowDepth  = 0.3f;  water.deepDepth  = 3.0f;
        water.opacity       = 0.82f; water.reflectivity = 0.35f;
        water.fresnelPower  = 4.0f;
        water.normalStrength = 1.0f;
        water.enableGerstnerWaves = true;
        water.waves[0] = { {  0.00f, 1.00f }, 0.10f,  5.0f, 0.30f }; // 流れ方向
        water.waves[1] = { {  0.10f, 0.99f }, 0.07f,  3.0f, 0.25f };
        water.waves[2] = { { -0.10f, 1.00f }, 0.05f,  7.0f, 0.20f };
        water.waves[3] = { {  0.05f, 0.99f }, 0.03f,  2.5f, 0.15f };
        water.foamThreshold = 0.25f; water.foamStrength = 1.0f;
        water.normalMap1Scroll = {  0.01f, 0.05f }; // 流れ方向スクロール
        water.normalMap2Scroll = { -0.01f, 0.04f };
        water.meshDirty = true; water.foamDirty = true; water.texDirty = true;
        markDirty();
    }
    ImGui::SameLine();

    // Flat プリセット: 静水（波なし）
    if (ImGui::Button("Flat")) {
        water.enableGerstnerWaves = false;
        for (auto& w : water.waves) w.amplitude = 0.0f;
        water.opacity = 0.95f; water.reflectivity = 0.75f;
        water.meshDirty = true;
        markDirty();
    }
}

// =============================================================================
// 波エディタセクション
// =============================================================================

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
