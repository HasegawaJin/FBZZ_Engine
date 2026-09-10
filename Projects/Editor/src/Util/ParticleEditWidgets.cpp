/// @file    ParticleEditWidgets.cpp
/// @brief   ParticleCurve / ParticleGradient のドラッグ編集ウィジェット実装。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#include <Editor/Util/ParticleEditWidgets.hpp>

// プリセット表は Engine 側に 1 つだけ置き、Editor UI と AI が同じ語彙を使う。
#include <Engine/Asset/ParticleCurvePresets.hpp>

#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/ParticleCurveAsset.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace fbzz::editor::widgets {
namespace {

constexpr float kKeyRadius    = 5.0f;  // カーブキーの表示半径 [px]
constexpr float kKeyHitRadius = 9.0f;  // キーのヒット判定半径 [px] (表示より広めで掴みやすく)
constexpr float kMarkerWidth  = 10.0f; // グラデーションキーマーカーの幅 [px]

float Clamp01(float value) { return std::clamp(value, 0.0f, 1.0f); }

// ラベルからファイル名を作る。空白と区切りを詰めるだけで、日本語はそのまま通す。
std::string CurveAssetFileName(const char* label)
{
    std::string name = label ? label : "Curve";
    for (char& c : name) {
        if (c == ' ' || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?'
            || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    }
    return name.empty() ? std::string("Curve") : name;
}

// 保存済みの «形» を出し入れするメニュー項目。既存の "..." ポップアップの中へ出す。
//
// WHY 専用のライブラリを Assets/Curves に決め打ちするか: ファイルピッカーを開かせると、
//   «良い形ができたので残す» という一瞬の操作が中断されて結局誰も保存しなくなる。
//   1 か所に集めておけば «この演出で使う減衰» が一覧になり、選ぶだけで揃う。
//   置き場所を変えたければアセットブラウザーで動かせばよい (読み出しは走査なので追従する)。
//
// @return true if the inline curve/gradient was replaced by a loaded asset
bool CurveAssetMenuItems(const char* label, scene::ParticleCurve* curve,
                         scene::ParticleGradient* gradient, const std::string& projectRoot)
{
    const bool isGradient = gradient != nullptr;
    const char* extension = isGradient ? ".gradient" : ".curve";
    const std::string dir = projectRoot + "/Assets/Curves";

    bool replaced = false;

    if (ImGui::MenuItem(isGradient ? "Save As .gradient" : "Save As .curve")) {
        asset::ParticleCurveAsset out;
        if (isGradient) { out.gradient = *gradient; out.hasGradient = true; }
        else            { out.curve    = *curve;    out.hasCurve    = true; }

        util::FileSystem::EnsureDirectory(dir);
        const std::string base = dir + "/" + CurveAssetFileName(label);
        std::string path = base + extension;
        for (int suffix = 1; util::FileSystem::Exists(path) && suffix < 1000; ++suffix)
            path = base + " " + std::to_string(suffix) + extension;

        if (asset::SaveParticleCurveAsset(path, out))
            FBZZ_LOG_INFO("Curve: 保存しました %s", path.c_str());
    }

    if (ImGui::BeginMenu("Load")) {
        // 走査は «メニューを開いている間だけ»。数十ファイルの列挙なので毎フレームでも軽い。
        const std::vector<std::string> files = util::FileSystem::ListFiles(dir, extension);
        if (files.empty()) {
            ImGui::TextDisabled("Assets/Curves に %s がありません", extension);
        }
        for (const std::string& file : files) {
            const size_t slash = file.find_last_of("/\\");
            const std::string name = slash == std::string::npos ? file : file.substr(slash + 1);
            if (!ImGui::MenuItem(name.c_str())) continue;
            asset::ParticleCurveAsset loaded;
            if (!asset::LoadParticleCurveAssetFile(file, loaded)) continue;
            // 参照は残さず中身だけを写す。参照を持つと «アセットとインラインの
            // どちらが正か» が生まれ、Inspector で触った値が次に開くと戻る。
            if (isGradient && loaded.hasGradient) { *gradient = loaded.gradient; replaced = true; }
            else if (!isGradient && loaded.hasCurve) { *curve = loaded.curve; replaced = true; }
        }
        ImGui::EndMenu();
    }
    return replaced;
}

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

// カーブ / グラデーションのクリップボード。
// WHY: 同じ減衰カーブを size / velocity / drag へ揃えたい、あるアセットの色遷移を
//      別アセットへ持っていきたい、という要求は制作中に必ず出る。
//      これが無いと 8 キーぶんのドラッグを目分量でやり直すことになる。
scene::ParticleCurve    s_curveClipboard;
bool                    s_hasCurveClipboard = false;
scene::ParticleGradient s_gradientClipboard;
bool                    s_hasGradientClipboard = false;

} // namespace

bool CurveEditor(const char* label, scene::ParticleCurve& curve, float maxValue, float height,
                 const std::string* projectRoot)
{
    bool changed = false;
    maxValue = (std::max)(maxValue, 0.0001f);
    curve.keyCount = std::clamp<uint32_t>(curve.keyCount, 2u, static_cast<uint32_t>(curve.keys.size()));

    ImGui::PushID(label);
    ImGui::TextUnformatted(label);

    // 補間モード。キー単位ではなくカーブ単位 (GPU パッキングの都合、詳細は ParticleEmitter.hpp)。
    {
        static const char* kModes[] = { "Linear", "Step", "Smooth" };
        int mode = static_cast<int>(curve.interpolation);
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::Combo("##curve_interp", &mode, kModes, IM_ARRAYSIZE(kModes))) {
            curve.interpolation = static_cast<scene::ParticleCurveInterpolation>(mode);
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Linear: 直線 / Step: 次のキーまで保持 (点滅・段階切替)\n"
                              "Smooth: smoothstep (始点と終点で速度 0。折れ線に見えない)");
        ImGui::SameLine();
        ImGui::TextDisabled("%u/%u keys", curve.keyCount,
                            static_cast<unsigned>(curve.keys.size()));
        ImGui::SameLine();
        if (ImGui::SmallButton("...")) ImGui::OpenPopup("##curve_menu");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("プリセット / コピー & ペースト / 数値でのキー入力");

        if (ImGui::BeginPopup("##curve_menu")) {
            if (ImGui::BeginMenu("Preset")) {
                for (const asset::ParticleCurvePreset& preset : asset::ParticleCurvePresets()) {
                    // string_view は null 終端を保証しないが、この表はリテラル由来なので安全。
                    if (!ImGui::MenuItem(preset.name.data())) continue;
                    asset::ApplyParticleCurvePreset(curve, preset, maxValue);
                    changed = true;
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", preset.description.data());
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Copy Curve")) {
                s_curveClipboard = curve;
                s_hasCurveClipboard = true;
            }
            if (ImGui::MenuItem("Paste Curve", nullptr, false, s_hasCurveClipboard)) {
                curve = s_curveClipboard;
                changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("同じ減衰を size / velocity / drag へ揃えるときに使います");
            // クリップボードはセッション内、アセットは演出をまたいで «形» を残す。
            if (projectRoot != nullptr) {
                ImGui::Separator();
                if (CurveAssetMenuItems(label, &curve, nullptr, *projectRoot)) changed = true;
            }
            ImGui::Separator();
            // 数値入力。ドラッグでは 0.5 や 1.0 をちょうど掴めないため、
            // 「ここは厳密に 0 にしたい」類の指定はこちらで行う。
            ImGui::TextDisabled("Keys");
            for (uint32_t i = 0; i < curve.keyCount; ++i) {
                ImGui::PushID(static_cast<int>(i));
                ImGui::SetNextItemWidth(70.0f);
                if (ImGui::DragFloat("##t", &curve.keys[i].time, 0.005f, 0.0f, 1.0f, "t %.3f")) {
                    curve.keys[i].time = Clamp01(curve.keys[i].time);
                    changed = true;
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                if (ImGui::DragFloat("##v", &curve.keys[i].value, maxValue * 0.005f,
                                     0.0f, maxValue, "v %.3f"))
                    changed = true;
                ImGui::SameLine();
                // 最小 2 キーは Evaluate の前提なので割り込ませない。
                if (curve.keyCount > 2 && ImGui::SmallButton("-")) {
                    for (uint32_t j = i; j + 1 < curve.keyCount; ++j) curve.keys[j] = curve.keys[j + 1];
                    --curve.keyCount;
                    changed = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            if (curve.keyCount < curve.keys.size() && ImGui::SmallButton("+ Add Key")) {
                // 末尾の 1 つ手前へ、時間・値とも中点を挿す (末尾を動かさないので形が壊れない)。
                const auto& last = curve.keys[curve.keyCount - 1];
                const auto& previous = curve.keys[curve.keyCount - 2];
                curve.keys[curve.keyCount] = last;
                curve.keys[curve.keyCount - 1] = {
                    (previous.time + last.time) * 0.5f, (previous.value + last.value) * 0.5f
                };
                ++curve.keyCount;
                changed = true;
            }
            if (changed) SortKeysByTime(curve.keys, curve.keyCount);
            ImGui::EndPopup();
        }
    }

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

    // カーブ本体は Evaluate() を等間隔サンプルして描く。
    // WHY: 以前はキー間を直線で結んでいた。線形補間しか無かった頃はそれで正確だったが、
    //      Step / Smooth を足した今は「表示は直線、実際は階段」という嘘になる。
    //      評価関数そのものを描けば、補間モードを増やしても表示は自動で追従する。
    {
        constexpr int kSamples = 96;
        ImVec2 previous = toScreen(0.0f, curve.Evaluate(0.0f));
        for (int i = 1; i <= kSamples; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kSamples);
            const ImVec2 current = toScreen(t, curve.Evaluate(t));
            draw->AddLine(previous, current, IM_COL32(120, 200, 255, 255), 2.0f);
            previous = current;
        }
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

bool GradientEditor(const char* label, scene::ParticleGradient& gradient,
                    const std::string* projectRoot)
{
    bool changed = false;
    gradient.keyCount = std::clamp<uint32_t>(gradient.keyCount, 2u,
                                             static_cast<uint32_t>(gradient.keys.size()));

    ImGui::PushID(label);
    ImGui::TextUnformatted(label);

    {
        static const char* kModes[] = { "Linear", "Step", "Smooth" };
        int mode = static_cast<int>(gradient.interpolation);
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::Combo("##gradient_interp", &mode, kModes, IM_ARRAYSIZE(kModes))) {
            gradient.interpolation = static_cast<scene::ParticleCurveInterpolation>(mode);
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Step は「炎から煙へ切り替わる瞬間」のような硬い変化に使う");
        ImGui::SameLine();
        {
            static const char* kSpaces[] = { "Gamma", "Linear", "OkLab" };
            int space = static_cast<int>(gradient.colorSpace);
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::Combo("##gradient_space", &space, kSpaces, IM_ARRAYSIZE(kSpaces))) {
                gradient.colorSpace = static_cast<scene::ParticleColorSpace>(space);
                changed = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "キー間をどの色空間で混ぜるか\n"
                    "Gamma: ピッカー上の見た目どおり (従来)\n"
                    "Linear: 光として正しく足し合わさる。白熱→橙→暗赤の中間が濁らない\n"
                    "OkLab: 明度と色相が知覚的に等間隔。色相を大きく回す魔法系向け");
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%u/%u keys", gradient.keyCount,
                            static_cast<unsigned>(gradient.keys.size()));
        ImGui::SameLine();
        if (ImGui::SmallButton("...")) ImGui::OpenPopup("##gradient_menu");
        if (ImGui::BeginPopup("##gradient_menu")) {
            if (ImGui::MenuItem("Copy Gradient")) {
                s_gradientClipboard = gradient;
                s_hasGradientClipboard = true;
            }
            if (ImGui::MenuItem("Paste Gradient", nullptr, false, s_hasGradientClipboard)) {
                gradient = s_gradientClipboard;
                changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("芯炎と外炎で色温度を揃えるときに使います");
            if (projectRoot != nullptr) {
                ImGui::Separator();
                if (CurveAssetMenuItems(label, nullptr, &gradient, *projectRoot)) changed = true;
            }
            ImGui::Separator();
            // 「終端のアルファを厳密に 0 にする」はグラデーション調整で最頻出の要求で、
            // マーカーのドラッグでは正確に 0 を掴めない。数値で入れられるようにする。
            ImGui::TextDisabled("Keys");
            for (uint32_t i = 0; i < gradient.keyCount; ++i) {
                ImGui::PushID(static_cast<int>(i));
                ImGui::SetNextItemWidth(80.0f);
                if (ImGui::DragFloat("##t", &gradient.keys[i].time, 0.005f, 0.0f, 1.0f, "t %.3f")) {
                    gradient.keys[i].time = Clamp01(gradient.keys[i].time);
                    changed = true;
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(200.0f);
                if (ImGui::ColorEdit4("##c", &gradient.keys[i].color.x,
                                      ImGuiColorEditFlags_AlphaBar
                                          | ImGuiColorEditFlags_AlphaPreviewHalf
                                          | ImGuiColorEditFlags_Float
                                          | ImGuiColorEditFlags_HDR))
                    changed = true;
                ImGui::SameLine();
                if (gradient.keyCount > 2 && ImGui::SmallButton("-")) {
                    for (uint32_t j = i; j + 1 < gradient.keyCount; ++j)
                        gradient.keys[j] = gradient.keys[j + 1];
                    --gradient.keyCount;
                    changed = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            if (gradient.keyCount < gradient.keys.size() && ImGui::SmallButton("+ Add Key")) {
                const auto last = gradient.keys[gradient.keyCount - 1];
                const auto previous = gradient.keys[gradient.keyCount - 2];
                gradient.keys[gradient.keyCount] = last;
                gradient.keys[gradient.keyCount - 1].time = (previous.time + last.time) * 0.5f;
                gradient.keys[gradient.keyCount - 1].color = {
                    (previous.color.x + last.color.x) * 0.5f, (previous.color.y + last.color.y) * 0.5f,
                    (previous.color.z + last.color.z) * 0.5f, (previous.color.w + last.color.w) * 0.5f
                };
                ++gradient.keyCount;
                changed = true;
            }
            if (changed) SortKeysByTime(gradient.keys, gradient.keyCount);
            ImGui::EndPopup();
        }
    }

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

    // グラデーションバー本体は Evaluate() の等間隔サンプルを細い短冊で並べて描く。
    // WHY: カーブ側と同じ理由。キー間を水平グラデで結ぶ描き方は線形補間専用で、
    //      Step / Smooth を足すと表示と実際の色が食い違う。
    auto timeToX = [&](float time) { return rectMin.x + Clamp01(time) * width; };
    {
        constexpr int kSamples = 128;
        const float stripe = width / static_cast<float>(kSamples);
        for (int i = 0; i < kSamples; ++i) {
            const float t0 = static_cast<float>(i) / static_cast<float>(kSamples);
            const float t1 = static_cast<float>(i + 1) / static_cast<float>(kSamples);
            const ImU32 c0 = ToImColor(gradient.Evaluate(t0));
            const ImU32 c1 = ToImColor(gradient.Evaluate(t1));
            draw->AddRectFilledMultiColor(
                { rectMin.x + static_cast<float>(i) * stripe, rectMin.y },
                { rectMin.x + static_cast<float>(i + 1) * stripe, barMax.y },
                c0, c1, c1, c0);
        }
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
