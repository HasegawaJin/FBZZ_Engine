// FBZZ Engine
// ParticleEditWidgets.cpp | fbzz::editor
// ParticleCurve / ParticleGradient のドラッグ編集ウィジェット実装
#include <Editor/Util/ParticleEditWidgets.hpp>

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fbzz::editor::widgets {
namespace {

constexpr float kKeyRadius    = 5.0f;  // カーブキーの表示半径 [px]
constexpr float kKeyHitRadius = 9.0f;  // キーのヒット判定半径 [px] (表示より広めで掴みやすく)
constexpr float kMarkerWidth  = 10.0f; // グラデーションキーマーカーの幅 [px]

float Clamp01(float value) { return std::clamp(value, 0.0f, 1.0f); }

// キー配列を time 昇順に保つ。ドラッグで隣を追い越した場合も表示・評価が破綻しないようにする。
template <typename Keys>
void SortKeysByTime(Keys& keys, uint32_t count)
{
    std::sort(keys.begin(), keys.begin() + count,
              [](const auto& a, const auto& b) { return a.time < b.time; });
}

ImU32 ToImColor(const math::Vector4& c, float alphaOverride = -1.0f)
{
    const float a = alphaOverride >= 0.0f ? alphaOverride : c.w;
    return IM_COL32(static_cast<int>(Clamp01(c.x) * 255.0f),
                    static_cast<int>(Clamp01(c.y) * 255.0f),
                    static_cast<int>(Clamp01(c.z) * 255.0f),
                    static_cast<int>(Clamp01(a) * 255.0f));
}

} // namespace

bool CurveEditor(const char* label, scene::ParticleCurve& curve, float maxValue, float height)
{
    bool changed = false;
    maxValue = (std::max)(maxValue, 0.0001f);
    curve.keyCount = std::clamp<uint32_t>(curve.keyCount, 2u, static_cast<uint32_t>(curve.keys.size()));

    ImGui::PushID(label);
    ImGui::TextUnformatted(label);

    const float  width = (std::max)(ImGui::GetContentRegionAvail().x, 120.0f);
    const ImVec2 size(width, height);
    ImGui::InvisibleButton("##curve_canvas", size);
    const ImVec2 rectMin = ImGui::GetItemRectMin();
    const ImVec2 rectMax = ImGui::GetItemRectMax();
    const bool   hovered = ImGui::IsItemHovered();
    ImDrawList*  draw    = ImGui::GetWindowDrawList();

    // 正規化 (time, value) ⇔ スクリーン座標の相互変換
    auto toScreen = [&](float time, float value) {
        return ImVec2(rectMin.x + Clamp01(time) * size.x,
                      rectMax.y - Clamp01(value / maxValue) * size.y);
    };
    auto toNormalized = [&](const ImVec2& p, float& outTime, float& outValue) {
        outTime  = Clamp01((p.x - rectMin.x) / size.x);
        outValue = Clamp01((rectMax.y - p.y) / size.y) * maxValue;
    };

    // 背景 + 1/4 グリッド
    draw->AddRectFilled(rectMin, rectMax, IM_COL32(22, 25, 32, 255), 3.0f);
    for (int i = 1; i < 4; ++i) {
        const float x = rectMin.x + size.x * (static_cast<float>(i) / 4.0f);
        const float y = rectMin.y + size.y * (static_cast<float>(i) / 4.0f);
        draw->AddLine({ x, rectMin.y }, { x, rectMax.y }, IM_COL32(255, 255, 255, 14));
        draw->AddLine({ rectMin.x, y }, { rectMax.x, y }, IM_COL32(255, 255, 255, 14));
    }
    draw->AddRect(rectMin, rectMax, IM_COL32(70, 76, 90, 255), 3.0f);

    // カーブ折れ線 (Evaluate と同じ線形補間なので、キー間を直線で結ぶだけで正確)
    for (uint32_t i = 0; i + 1 < curve.keyCount; ++i) {
        const ImVec2 a = toScreen(curve.keys[i].time, curve.keys[i].value);
        const ImVec2 b = toScreen(curve.keys[i + 1].time, curve.keys[i + 1].value);
        draw->AddLine(a, b, IM_COL32(120, 200, 255, 255), 2.0f);
    }
    // 端の外側は端値でクランプされることを点線ふうの薄い線で示す
    {
        const ImVec2 first = toScreen(curve.keys[0].time, curve.keys[0].value);
        const ImVec2 last  = toScreen(curve.keys[curve.keyCount - 1].time,
                                      curve.keys[curve.keyCount - 1].value);
        draw->AddLine({ rectMin.x, first.y }, first, IM_COL32(120, 200, 255, 90), 1.0f);
        draw->AddLine(last, { rectMax.x, last.y }, IM_COL32(120, 200, 255, 90), 1.0f);
    }

    // ドラッグ中のキー index を ImGui StateStorage に保持する (ウィジェット多重配置対応)
    ImGuiStorage* storage    = ImGui::GetStateStorage();
    const ImGuiID dragKeyId  = ImGui::GetID("##curve_drag_key");
    int           dragIndex  = storage->GetInt(dragKeyId, -1);
    const ImVec2  mouse      = ImGui::GetMousePos();

    // キーの描画 + ドラッグ / 削除
    int hoveredKey = -1;
    for (uint32_t i = 0; i < curve.keyCount; ++i) {
        const ImVec2 p  = toScreen(curve.keys[i].time, curve.keys[i].value);
        const float  dx = mouse.x - p.x;
        const float  dy = mouse.y - p.y;
        if (hovered && dx * dx + dy * dy <= kKeyHitRadius * kKeyHitRadius)
            hoveredKey = static_cast<int>(i);
        const bool isHot = hoveredKey == static_cast<int>(i) || dragIndex == static_cast<int>(i);
        draw->AddCircleFilled(p, kKeyRadius, isHot ? IM_COL32(255, 220, 120, 255)
                                                   : IM_COL32(235, 240, 255, 255));
        draw->AddCircle(p, kKeyRadius, IM_COL32(30, 34, 44, 255), 0, 1.5f);
    }

    // ドラッグ開始 / 継続 / 終了
    if (hovered && hoveredKey >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        storage->SetInt(dragKeyId, hoveredKey);
    dragIndex = storage->GetInt(dragKeyId, -1);
    if (dragIndex >= 0 && dragIndex < static_cast<int>(curve.keyCount)
        && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        float t = 0.0f, v = 0.0f;
        toNormalized(mouse, t, v);
        curve.keys[static_cast<uint32_t>(dragIndex)].time  = t;
        curve.keys[static_cast<uint32_t>(dragIndex)].value = v;
        changed = true;
        // ドラッグ中はツールチップで正確な値を出す (数値入力の代替)
        ImGui::SetTooltip("t=%.2f  v=%.2f", t, v);
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && dragIndex >= 0) {
        // 離した時点でソートして評価順を保証する
        SortKeysByTime(curve.keys, curve.keyCount);
        storage->SetInt(dragKeyId, -1);
        changed = true;
    }

    // ダブルクリックでキー追加 (最大数まで)
    if (hovered && hoveredKey < 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
        && curve.keyCount < curve.keys.size()) {
        float t = 0.0f, v = 0.0f;
        toNormalized(mouse, t, v);
        curve.keys[curve.keyCount] = { t, v };
        ++curve.keyCount;
        SortKeysByTime(curve.keys, curve.keyCount);
        changed = true;
    }

    // 右クリックでキー削除 (最小2キーは維持)
    if (hovered && hoveredKey >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)
        && curve.keyCount > 2) {
        for (uint32_t i = static_cast<uint32_t>(hoveredKey); i + 1 < curve.keyCount; ++i)
            curve.keys[i] = curve.keys[i + 1];
        --curve.keyCount;
        changed = true;
    }

    if (hovered && hoveredKey < 0 && dragIndex < 0)
        ImGui::SetTooltip("Drag key: move / Double-click: add / Right-click key: remove");

    ImGui::PopID();
    return changed;
}

bool GradientEditor(const char* label, scene::ParticleGradient& gradient)
{
    bool changed = false;
    gradient.keyCount = std::clamp<uint32_t>(gradient.keyCount, 2u,
                                             static_cast<uint32_t>(gradient.keys.size()));

    ImGui::PushID(label);
    ImGui::TextUnformatted(label);

    const float  width     = (std::max)(ImGui::GetContentRegionAvail().x, 120.0f);
    const float  barHeight = 22.0f;
    const float  markerH   = 14.0f;
    ImGui::InvisibleButton("##gradient_canvas", ImVec2(width, barHeight + markerH));
    const ImVec2 rectMin = ImGui::GetItemRectMin();
    const bool   hovered = ImGui::IsItemHovered();
    const ImVec2 barMax(rectMin.x + width, rectMin.y + barHeight);
    ImDrawList*  draw = ImGui::GetWindowDrawList();
    const ImVec2 mouse = ImGui::GetMousePos();

    // 透明を可視化する市松背景
    constexpr float kChecker = 8.0f;
    for (float x = 0.0f; x < width; x += kChecker) {
        for (float y = 0.0f; y < barHeight; y += kChecker) {
            const bool dark = (static_cast<int>(x / kChecker) + static_cast<int>(y / kChecker)) % 2 == 0;
            draw->AddRectFilled({ rectMin.x + x, rectMin.y + y },
                                { (std::min)(rectMin.x + x + kChecker, barMax.x),
                                  (std::min)(rectMin.y + y + kChecker, barMax.y) },
                                dark ? IM_COL32(60, 60, 60, 255) : IM_COL32(90, 90, 90, 255));
        }
    }

    // グラデーションバー本体。Evaluate と同じ線形補間なので、キー区間ごとの水平グラデで正確に描ける
    auto timeToX = [&](float time) { return rectMin.x + Clamp01(time) * width; };
    {
        const math::Vector4& first = gradient.keys[0].color;
        draw->AddRectFilledMultiColor({ rectMin.x, rectMin.y }, { timeToX(gradient.keys[0].time), barMax.y },
                                      ToImColor(first), ToImColor(first), ToImColor(first), ToImColor(first));
        for (uint32_t i = 0; i + 1 < gradient.keyCount; ++i) {
            const ImU32 c0 = ToImColor(gradient.keys[i].color);
            const ImU32 c1 = ToImColor(gradient.keys[i + 1].color);
            draw->AddRectFilledMultiColor({ timeToX(gradient.keys[i].time), rectMin.y },
                                          { timeToX(gradient.keys[i + 1].time), barMax.y },
                                          c0, c1, c1, c0);
        }
        const math::Vector4& last = gradient.keys[gradient.keyCount - 1].color;
        draw->AddRectFilledMultiColor({ timeToX(gradient.keys[gradient.keyCount - 1].time), rectMin.y },
                                      { barMax.x, barMax.y },
                                      ToImColor(last), ToImColor(last), ToImColor(last), ToImColor(last));
    }
    draw->AddRect({ rectMin.x, rectMin.y }, barMax, IM_COL32(70, 76, 90, 255));

    // 選択中キーとドラッグ中キーを StateStorage に保持
    ImGuiStorage* storage      = ImGui::GetStateStorage();
    const ImGuiID selectedId   = ImGui::GetID("##gradient_selected");
    const ImGuiID dragId       = ImGui::GetID("##gradient_drag");
    int           selectedKey  = storage->GetInt(selectedId, 0);
    int           dragKey      = storage->GetInt(dragId, -1);

    // キーマーカー (バー下の三角形 + 塗り) の描画とヒット判定
    int hoveredMarker = -1;
    for (uint32_t i = 0; i < gradient.keyCount; ++i) {
        const float x = timeToX(gradient.keys[i].time);
        const ImVec2 top(x, barMax.y);
        const ImVec2 left(x - kMarkerWidth * 0.5f, barMax.y + markerH);
        const ImVec2 right(x + kMarkerWidth * 0.5f, barMax.y + markerH);
        const bool inX = mouse.x >= left.x && mouse.x <= right.x;
        const bool inY = mouse.y >= barMax.y && mouse.y <= barMax.y + markerH;
        if (hovered && inX && inY)
            hoveredMarker = static_cast<int>(i);
        const bool isSelected = selectedKey == static_cast<int>(i);
        draw->AddTriangleFilled(top, left, right, ToImColor(gradient.keys[i].color, 1.0f));
        draw->AddTriangle(top, left, right,
                          isSelected ? IM_COL32(255, 220, 120, 255) : IM_COL32(30, 34, 44, 255),
                          isSelected ? 2.0f : 1.0f);
    }

    // マーカー操作: クリックで選択 + ドラッグ開始、ドラッグで time 変更、右クリックで削除
    if (hovered && hoveredMarker >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        storage->SetInt(selectedId, hoveredMarker);
        storage->SetInt(dragId, hoveredMarker);
        selectedKey = hoveredMarker;
    }
    dragKey = storage->GetInt(dragId, -1);
    if (dragKey >= 0 && dragKey < static_cast<int>(gradient.keyCount)
        && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float t = Clamp01((mouse.x - rectMin.x) / width);
        gradient.keys[static_cast<uint32_t>(dragKey)].time = t;
        changed = true;
        ImGui::SetTooltip("t=%.2f", t);
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && dragKey >= 0) {
        // ソートすると index が変わるため、ドラッグしていたキーを追跡して選択を維持する
        const scene::ParticleGradientKey dragged = gradient.keys[static_cast<uint32_t>(dragKey)];
        SortKeysByTime(gradient.keys, gradient.keyCount);
        for (uint32_t i = 0; i < gradient.keyCount; ++i) {
            if (gradient.keys[i].time == dragged.time
                && gradient.keys[i].color.x == dragged.color.x
                && gradient.keys[i].color.w == dragged.color.w) {
                storage->SetInt(selectedId, static_cast<int>(i));
                break;
            }
        }
        storage->SetInt(dragId, -1);
        changed = true;
    }

    // バーをダブルクリック → その時刻の補間色でキー追加
    const bool barHovered = hovered && mouse.y >= rectMin.y && mouse.y <= barMax.y;
    if (barHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
        && gradient.keyCount < gradient.keys.size()) {
        const float t = Clamp01((mouse.x - rectMin.x) / width);
        gradient.keys[gradient.keyCount] = { t, gradient.Evaluate(t) };
        ++gradient.keyCount;
        SortKeysByTime(gradient.keys, gradient.keyCount);
        changed = true;
    }

    // マーカー右クリックで削除 (最小2キーは維持)
    if (hovered && hoveredMarker >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)
        && gradient.keyCount > 2) {
        for (uint32_t i = static_cast<uint32_t>(hoveredMarker); i + 1 < gradient.keyCount; ++i)
            gradient.keys[i] = gradient.keys[i + 1];
        --gradient.keyCount;
        storage->SetInt(selectedId, 0);
        selectedKey = 0;
        changed = true;
    }

    // 選択中キーの色編集
    selectedKey = std::clamp(selectedKey, 0, static_cast<int>(gradient.keyCount) - 1);
    {
        auto& key = gradient.keys[static_cast<uint32_t>(selectedKey)];
        float color[4] = { key.color.x, key.color.y, key.color.z, key.color.w };
        char  colorLabel[64];
        std::snprintf(colorLabel, sizeof(colorLabel), "Key %d (t=%.2f)", selectedKey + 1, key.time);
        if (ImGui::ColorEdit4(colorLabel, color)) {
            key.color = { color[0], color[1], color[2], color[3] };
            changed = true;
        }
    }

    if (hovered && hoveredMarker < 0 && dragKey < 0)
        ImGui::SetTooltip("Drag marker: move / Click: select / Double-click bar: add / Right-click marker: remove");

    ImGui::PopID();
    return changed;
}

} // namespace fbzz::editor::widgets
