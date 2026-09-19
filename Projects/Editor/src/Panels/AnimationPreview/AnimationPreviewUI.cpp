/// @file    AnimationPreviewUI.cpp
/// @brief   Animation Preview のツールバー・ビューポート・トランスポート UI。
/// @author  Hasegawa Jin
/// @date    2026-09-16
///
/// AnimationPreviewCore.cpp が描いたレンダーターゲットを表示し、その上へ
/// スケルトン・軌跡・情報をオーバーレイし、再生とカメラの操作口を出す。
/// Inspector 埋め込みと独立パネルの両方がこのファイルの 1 つのウィジェットを呼ぶ。
#include "AnimationPreviewInternal.hpp"
#include <Editor/Panels/PreviewPanelRenderers.hpp>
#include <Editor/Util/EditorIcons.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace fbzz::editor::animpreview {

namespace {

/// @name 配色
/// @note 同じ «アクセント» がトグルの背景・タイムラインの進捗・選択ボーンの縁取りに散っている。数値を直書きすると片方だけ色がずれる。
constexpr ImU32 kAccent        = IM_COL32( 64, 132, 200, 255);
constexpr ImU32 kAccentHover   = IM_COL32( 78, 152, 224, 255);
constexpr ImU32 kAccentDim     = IM_COL32( 52, 104, 160, 255);
constexpr ImU32 kPanelBg       = IM_COL32( 24,  26,  32, 255);
constexpr ImU32 kPanelBorder   = IM_COL32( 62,  68,  80, 255);
constexpr ImU32 kTextBright    = IM_COL32(226, 232, 240, 255);
constexpr ImU32 kTextMuted     = IM_COL32(150, 158, 172, 255);
constexpr ImU32 kEventColor    = IM_COL32(255, 200,  88, 255);
constexpr ImU32 kSelectColor   = IM_COL32(255, 216,  96, 255);
constexpr ImU32 kAnimatedBone  = IM_COL32(110, 226, 160, 235);
constexpr ImU32 kStaticBone    = IM_COL32(150, 156, 168, 140);
constexpr ImU32 kGhostPast     = IM_COL32( 96, 160, 255, 110);
constexpr ImU32 kGhostFuture   = IM_COL32(255, 168,  92, 110);

/// 現在プレビューしているクリップ。オーバーレイ・タイムライン・情報表示が共通で使う。
const asset::AnimationClip* CurrentClip(const asset::Model* model)
{
    if (!model) return nullptr;
    return ResolveClip(model, g_state.target.clipName, g_state.target.animAssetPath);
}

/// @name 小物ウィジェット

/// 押されている間だけアクセント色になるトグル。幅を揃えて «帯» として並べる。
/// @note ラベル長でボタン幅が変わると、トグルを切り替えるたび隣のボタンが左右に動いて狙いが外れるため幅を固定する。
bool ToolbarToggle(const char* label, bool& value, const char* tooltip, float width)
{
    const bool active = value;
    ImGui::PushStyleColor(ImGuiCol_Button,
                          active ? kAccent : IM_COL32(44, 48, 57, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          active ? kAccentHover : IM_COL32(58, 63, 74, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          active ? kAccentHover : IM_COL32(68, 74, 86, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, active ? kTextBright : kTextMuted);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    const bool clicked = ImGui::Button(label, ImVec2(width, 0.0f));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    if (clicked) value = !value;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

/// アイコン 1 文字ぶんの正方ボタン。トランスポート (先頭 / コマ送り / 再生) 用。
bool TransportButton(const char* id, const char* glyph, const char* tooltip,
                     bool emphasized = false)
{
    const float size = ImGui::GetFrameHeight();
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_Button,
                          emphasized ? kAccent : IM_COL32(44, 48, 57, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          emphasized ? kAccentHover : IM_COL32(60, 66, 78, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentHover);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    const bool clicked = ImGui::Button(glyph, ImVec2(size, size));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    ImGui::PopID();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

/// 角丸の «チップ»。対象名やブレンド率のように «読ませたいが押させない» 情報に使う。
void InfoChip(const char* text, ImU32 textColor, ImU32 background)
{
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 padding(6.0f, 2.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 total(size.x + padding.x * 2.0f, size.y + padding.y * 2.0f);
    ImGui::Dummy(total);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + total.x, origin.y + total.y),
                      background, total.y * 0.5f);
    dl->AddText(ImVec2(origin.x + padding.x, origin.y + padding.y), textColor, text);
}

/// @name 再生操作

void SeekPreview(float seconds)
{
    g_state.time = std::clamp(seconds, 0.0f, g_state.timelineLength);
    g_state.playing = false;
}

/// フレーム単位で進める。クリップの frameRate を «見たまま» の刻みとして使う。
void StepPreviewFrames(int frames, const asset::AnimationClip* clip)
{
    const float fps = ClipFrameRate(clip);
    const float step = static_cast<float>(frames) / (std::max)(fps, 1.0f);
    float next = g_state.time + step;
    if (g_state.loop && g_state.timelineLength > 0.0001f) {
        next = WrapTime(next, g_state.timelineLength);
        g_state.time = next;
        g_state.playing = false;
        return;
    }
    SeekPreview(next);
}

/// プレビューにマウス / フォーカスがある間だけ効くキー操作。
/// @note Space / Home / End / L と «修飾なしの矢印» は EditorApp_MenuBar のどのホットキーにも割り当てが無い (矢印は Alt 付きだけが Selection History に取られている)。F は Scene Viewport スコープなので、ここで拾ってもシーンのフレーミングとは衝突しない。
void HandlePreviewHotkeys(bool interactive, const asset::AnimationClip* clip)
{
    if (!interactive) return;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || io.KeyCtrl || io.KeyAlt) return;

    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) g_state.playing = !g_state.playing;
    if (ImGui::IsKeyPressed(ImGuiKey_L, false))     g_state.loop = !g_state.loop;
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false))  SeekPreview(0.0f);
    if (ImGui::IsKeyPressed(ImGuiKey_End, false))   SeekPreview(g_state.timelineLength);

    const int stride = io.KeyShift ? 10 : 1;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))  StepPreviewFrames(-stride, clip);
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) StepPreviewFrames(stride, clip);

    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        /// @note 選択ボーンがあればそこへ寄る。無ければモデル全体を framing し直す。
        if (g_state.selectedBoneNode >= 0 &&
            g_state.selectedBoneNode < static_cast<int>(g_state.jointPositions.size())) {
            g_state.pendingFocus =
                g_state.jointPositions[static_cast<size_t>(g_state.selectedBoneNode)];
            g_state.pendingDistance = (std::max)(g_state.boundsRadius * 0.8f, 0.05f);
            g_state.hasPendingFocus = true;
        } else {
            g_state.needsFraming = true;
        }
    }
}

/// 自作タイムライン。ImGui の SliderFloat では «イベント位置を押す» ことも
/// «遷移のどこを見ているか» を色で示すこともできないため、描画から起こす。
void DrawPreviewTimeline(const asset::AnimationClip* clip, float width)
{
    const float height = ImGui::GetFrameHeight();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##PreviewTimeline", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const float length = (std::max)(g_state.timelineLength, 0.0001f);
    const float rounding = height * 0.5f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 maxCorner(origin.x + width, origin.y + height);
    dl->AddRectFilled(origin, maxCorner, IM_COL32(22, 24, 30, 255), rounding);

    const auto timeToX = [&](float t) {
        return origin.x + std::clamp(t / length, 0.0f, 1.0f) * width;
    };
    const float playhead = WrapTime(g_state.time, length);

    if (g_state.target.mode == PreviewTarget::Mode::Transition) {
        /// @note 遷移元 / ブレンド / 遷移先 を帯で描き分ける。ブレンド区間だけ
        ///       左から右へ色を渡して、どちら向きに重みが移るかを線で読ませる。
        const float blendStart = g_state.target.transitionStartSeconds;
        const float blendEnd = blendStart + g_state.target.blendSeconds;
        const float x0 = timeToX(blendStart);
        const float x1 = timeToX(blendEnd);
        dl->AddRectFilled(origin, ImVec2(x0, maxCorner.y),
                          IM_COL32(52, 74, 110, 255), rounding,
                          ImDrawFlags_RoundCornersLeft);
        dl->AddRectFilledMultiColor(ImVec2(x0, origin.y), ImVec2(x1, maxCorner.y),
                                    IM_COL32(52, 74, 110, 255),
                                    IM_COL32(58, 118, 92, 255),
                                    IM_COL32(58, 118, 92, 255),
                                    IM_COL32(52, 74, 110, 255));
        dl->AddRectFilled(ImVec2(x1, origin.y), maxCorner,
                          IM_COL32(48, 88, 70, 255), rounding,
                          ImDrawFlags_RoundCornersRight);
    } else {
        dl->AddRectFilled(origin, ImVec2(timeToX(playhead), maxCorner.y),
                          kAccentDim, rounding, ImDrawFlags_RoundCornersLeft);
    }

    /// @note フレーム目盛り。潰れて «ただの帯» に見える密度になったら描かない。
    const float fps = ClipFrameRate(clip);
    const int frameCount = (std::max)(static_cast<int>(std::lround(length * fps)), 1);
    if (width / static_cast<float>(frameCount) >= 6.0f) {
        for (int f = 0; f <= frameCount; ++f) {
            const float x = timeToX(static_cast<float>(f) / fps);
            const bool major = (f % 5) == 0;
            dl->AddLine(ImVec2(x, maxCorner.y - (major ? 6.0f : 3.0f)),
                        ImVec2(x, maxCorner.y - 1.0f),
                        IM_COL32(120, 128, 142, major ? 180 : 100));
        }
    }

    /// @note イベントマーカー。押せば «その瞬間» へ跳べるようにする。
    int hoveredEvent = -1;
    if (clip) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        for (size_t i = 0; i < clip->events.size(); ++i) {
            const float x = timeToX(static_cast<float>(clip->events[i].time));
            const float cy = origin.y + 4.0f;
            const bool underCursor = hovered && std::abs(mouse.x - x) <= 5.0f;
            const float r = underCursor ? 5.0f : 3.5f;
            const ImVec2 diamond[4] = {
                { x, cy - r }, { x + r, cy }, { x, cy + r }, { x - r, cy }
            };
            dl->AddConvexPolyFilled(diamond, 4, kEventColor);
            dl->AddLine(ImVec2(x, origin.y), ImVec2(x, maxCorner.y),
                        IM_COL32(255, 200, 88, underCursor ? 160 : 70));
            if (underCursor) hoveredEvent = static_cast<int>(i);
        }
    }

    /// @note 再生ヘッド
    const float headX = timeToX(playhead);
    dl->AddLine(ImVec2(headX, origin.y + 1.0f), ImVec2(headX, maxCorner.y - 1.0f),
                IM_COL32(245, 248, 252, 230), 1.6f);
    dl->AddCircleFilled(ImVec2(headX, origin.y + height * 0.5f),
                        g_state.scrubbing ? 6.0f : 4.5f, IM_COL32(245, 248, 252, 255));
    dl->AddRect(origin, maxCorner, kPanelBorder, rounding);

    if (hoveredEvent >= 0 && clip) {
        const auto& event = clip->events[static_cast<size_t>(hoveredEvent)];
        ImGui::SetTooltip("%s\n%.3f s (frame %d)\nClick to jump",
                          event.name.c_str(), static_cast<float>(event.time),
                          static_cast<int>(std::lround(event.time * fps)));
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            SeekPreview(static_cast<float>(event.time));
    } else if (active) {
        SeekPreview((ImGui::GetIO().MousePos.x - origin.x) / width * length);
    } else if (hovered) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - origin.x) / width, 0.0f, 1.0f);
        ImGui::SetTooltip("%.3f s  (frame %d)\nDrag to scrub",
                          t * length, static_cast<int>(std::lround(t * length * fps)));
    }
    g_state.scrubbing = active;
}

/// @name ポップアップ (表示 / ビュー設定)

/// ラベル間引きとオニオンスキン間隔。使う頻度が低いのでツールバーからは隠す。
/// @note Names / Ghost を点けた «ときだけ» 行を生やすと、トグルを押すたびビューポートの高さが跳ねるため常時ポップアップへ隔離する。
void DrawDisplayPopup()
{
    ImGui::TextDisabled("Bone labels");
    ImGui::SetNextItemWidth(190.0f);
    const char* kLabelModes[] = { "Selected / Hovered", "Animated bones", "All" };
    ImGui::Combo("##LabelMode", &g_state.labelMode, kLabelModes, 3);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "How many bone name labels to show at once.\n"
            "Dense skeletons stay readable with 'Selected / Hovered' or 'Animated bones'.");

    ImGui::Spacing();
    ImGui::TextDisabled("Onion skin offset");
    ImGui::SetNextItemWidth(190.0f);
    ImGui::SliderFloat("##GhostOffset", &g_state.ghostOffsetSeconds, 0.0f, 0.5f,
                       g_state.ghostOffsetSeconds <= 0.0001f ? "Auto" : "%.3f s");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Onion skin offset (seconds before / after current time).\n0 = automatic (timeline / 14).");
}

void DrawViewPopup()
{
    auto& view = g_state.view;
    ImGui::TextDisabled("Scene");
    ImGui::Checkbox("Floor grid", &view.showGrid);
    ImGui::Checkbox("Ground ring", &view.showGroundRing);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Contact circle and a drop line under the root bone.\n"
                          "Shows root motion and hip height against the floor.");
    ImGui::Checkbox("Wireframe", &view.wireframe);
    ImGui::Checkbox("Axis gizmo", &view.showAxisGizmo);

    ImGui::Spacing();
    ImGui::TextDisabled("Background");
    ImGui::SetNextItemWidth(190.0f);
    ImGui::Combo("##Background", &view.background,
                 kPreviewBackgroundNames, kPreviewBackgroundCount);

    ImGui::Spacing();
    ImGui::TextDisabled("Camera");
    ImGui::SetNextItemWidth(190.0f);
    ImGui::SliderFloat("##Fov", &view.fovY, 12.0f, 90.0f, "FOV %.0f deg");
    ImGui::SetNextItemWidth(190.0f);
    ImGui::SliderAngle("##LightYaw", &view.lightYaw, -180.0f, 180.0f, "Light %.0f deg");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Rotate the key light around the model to read the silhouette.");

    ImGui::Spacing();
    constexpr float HALF_PI = 1.57079632679f;
    struct CameraPreset { const char* label; float yaw; float pitch; };
    static constexpr CameraPreset kPresets[] = {
        { "Front", 0.0f,           0.0f  },
        { "Back",  3.14159265f,    0.0f  },
        { "Left",  -HALF_PI,       0.0f  },
        { "Right", HALF_PI,        0.0f  },
        { "Top",   0.0f,           1.30f },
        { "Persp", 2.55f,          0.30f },
    };
    const float presetWidth = (190.0f - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
    for (int i = 0; i < static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0])); ++i) {
        if (i % 3 != 0) ImGui::SameLine();
        if (ImGui::Button(kPresets[i].label, ImVec2(presetWidth, 0.0f))) {
            g_state.yaw = kPresets[i].yaw;
            g_state.pitch = kPresets[i].pitch;
            g_state.needsFraming = true;
        }
    }
}

/// @name オーバーレイ

struct OverlayContext {
    ImDrawList* drawList = nullptr;
    ImVec2 origin;
    ImVec2 maxCorner;
    float width = 0.0f;
    float height = 0.0f;
    bool hovered = false;
};

/// ビューポートの右下に置く XYZ トライアド。カメラの yaw / pitch だけから
/// 向きを起こすので、射影 (FOV・距離) が変わっても «向きだけ» を正確に示す。
void DrawAxisGizmo(const OverlayContext& ctx)
{
    math::Vector3 right, up, forward;
    PreviewCameraBasis(right, up, forward);

    const float radius = 22.0f;
    const ImVec2 center(ctx.maxCorner.x - radius - 14.0f, ctx.maxCorner.y - radius - 14.0f);
    ctx.drawList->AddCircleFilled(center, radius + 5.0f, IM_COL32(16, 18, 23, 150));

    struct Axis { math::Vector3 dir; ImU32 color; const char* label; };
    const Axis axes[3] = {
        { { 1.0f, 0.0f, 0.0f }, IM_COL32(232, 92, 96, 255),  "X" },
        { { 0.0f, 1.0f, 0.0f }, IM_COL32(126, 218, 120, 255), "Y" },
        { { 0.0f, 0.0f, 1.0f }, IM_COL32(104, 160, 246, 255), "Z" },
    };
    for (const Axis& axis : axes) {
        const float sx = axis.dir.x * right.x + axis.dir.y * right.y + axis.dir.z * right.z;
        const float sy = -(axis.dir.x * up.x + axis.dir.y * up.y + axis.dir.z * up.z);
        const ImVec2 tip(center.x + sx * radius, center.y + sy * radius);
        /// @note 奥へ向く軸は薄くする。手前・奥が区別できないと «鏡像» に見える。
        const float depth = axis.dir.x * forward.x + axis.dir.y * forward.y +
                            axis.dir.z * forward.z;
        const int alpha = depth > 0.0f ? 110 : 255;
        const ImU32 color = (axis.color & 0x00FFFFFFu) | (static_cast<ImU32>(alpha) << 24);
        ctx.drawList->AddLine(center, tip, color, 1.8f);
        ctx.drawList->AddCircleFilled(tip, 2.6f, color);
        ctx.drawList->AddText(ImVec2(tip.x + 3.0f, tip.y - 7.0f), color, axis.label);
    }
}

/// 左上の情報パネル。生テキストを影付きで置くより、半透明の板に載せたほうが
/// 背景の明暗に関係なく読める。
void DrawInfoOverlay(const OverlayContext& ctx, const asset::AnimationClip* clip)
{
    char lines[6][128]{};
    ImU32 colors[6]{};
    int count = 0;

    const float fps = ClipFrameRate(clip);
    const float wrapped = WrapTime(g_state.time, g_state.timelineLength);
    std::snprintf(lines[count], sizeof(lines[0]), "Clip   %s",
                  clip ? clip->name.c_str() : "<none>");
    colors[count++] = kTextBright;
    std::snprintf(lines[count], sizeof(lines[0]),
                  "Time   %.3f / %.3f s  (Frame %d @ %.0f fps)",
                  wrapped, g_state.timelineLength,
                  static_cast<int>(wrapped * fps), fps);
    colors[count++] = kTextBright;
    std::snprintf(lines[count], sizeof(lines[0]), "Bones  %d  |  Animated %d (%.0f%%)",
                  g_state.boneNodeCount, g_state.animatedNodeCount,
                  g_state.boneNodeCount > 0
                      ? 100.0f * static_cast<float>(g_state.animatedNodeCount) /
                            static_cast<float>(g_state.boneNodeCount)
                      : 0.0f);
    colors[count++] = kTextBright;
    if (g_state.target.mode == PreviewTarget::Mode::Transition) {
        std::snprintf(lines[count], sizeof(lines[0]), "Blend  %.1f%%",
                      g_state.currentBlendWeight * 100.0f);
        colors[count++] = IM_COL32(140, 255, 190, 235);
    }
    if (clip) {
        for (const auto& event : clip->events) {
            if (count >= 6) break;
            if (std::abs(static_cast<float>(event.time) - wrapped) >= 0.08f) continue;
            std::snprintf(lines[count], sizeof(lines[0]), "Event  %s", event.name.c_str());
            colors[count++] = kEventColor;
        }
    }

    const float lineHeight = ImGui::GetTextLineHeight();
    float boxWidth = 0.0f;
    for (int i = 0; i < count; ++i)
        boxWidth = (std::max)(boxWidth, ImGui::CalcTextSize(lines[i]).x);
    const ImVec2 boxMin(ctx.origin.x + 8.0f, ctx.origin.y + 8.0f);
    const ImVec2 boxMax(boxMin.x + boxWidth + 16.0f,
                        boxMin.y + lineHeight * static_cast<float>(count) + 12.0f);
    ctx.drawList->AddRectFilled(boxMin, boxMax, IM_COL32(14, 16, 20, 185), 5.0f);
    ctx.drawList->AddRect(boxMin, boxMax, IM_COL32(90, 98, 112, 120), 5.0f);
    for (int i = 0; i < count; ++i)
        ctx.drawList->AddText(ImVec2(boxMin.x + 8.0f,
                                     boxMin.y + 6.0f + lineHeight * static_cast<float>(i)),
                              colors[i], lines[i]);
}

struct JointScreen {
    int nodeIndex = -1;
    ImVec2 pos;
};

/// ビューポート上のスケルトン / 軌跡 / 残像 / 情報を重ねる。
/// @return ボーンをクリックで «選択した» か (Mask Preview の部位選択と競合させないため)
bool DrawSkeletonOverlay(const OverlayContext& ctx,
                         const asset::Model& model,
                         const asset::AnimationClip* clip)
{
    if (!g_state.debugPoseValid || !model.skeleton) return false;
    const asset::Skeleton& skeleton = *model.skeleton;
    ImDrawList* dl = ctx.drawList;
    dl->PushClipRect(ctx.origin, ctx.maxCorner, true);

    const auto project = [&](const math::Vector3& world, ImVec2& out) -> bool {
        const math::Vector4 clipPos = g_state.debugViewProjection *
            math::Vector4{ world.x, world.y, world.z, 1.0f };
        if (clipPos.w <= 0.0001f) return false;
        out.x = ctx.origin.x + (clipPos.x / clipPos.w * 0.5f + 0.5f) * ctx.width;
        out.y = ctx.origin.y + (0.5f - clipPos.y / clipPos.w * 0.5f) * ctx.height;
        return true;
    };

    /// @note 軌跡: クリップ全長の代表ボーン移動をグラデーション付きポリラインで描く。
    if (g_state.showTrail && g_state.trailPoints.size() >= 2) {
        ImVec2 previousPoint{};
        bool previousValid = project(g_state.trailPoints[0], previousPoint);
        for (size_t i = 1; i < g_state.trailPoints.size(); ++i) {
            ImVec2 point;
            const bool valid = project(g_state.trailPoints[i], point);
            if (previousValid && valid) {
                const float progress = static_cast<float>(i) /
                    static_cast<float>(g_state.trailPoints.size() - 1);
                dl->AddLine(previousPoint, point,
                            IM_COL32(255, 176, 84, 60 + static_cast<int>(150.0f * progress)),
                            2.0f);
            }
            previousPoint = point;
            previousValid = valid;
        }
        const float clipLength = ClipDurationSeconds(clip, 1.0f);
        const float normalized =
            std::clamp(WrapTime(g_state.time, clipLength) / clipLength, 0.0f, 1.0f);
        const size_t markerIndex = static_cast<size_t>(std::lround(
            normalized * static_cast<float>(g_state.trailPoints.size() - 1)));
        ImVec2 marker;
        if (project(g_state.trailPoints[markerIndex], marker))
            dl->AddCircleFilled(marker, 4.0f, IM_COL32(255, 200, 110, 255));
    }

    /// @note オニオンスキン: 過去 = 青、未来 = 橙。動きの速いボーンほど残像が離れて見える。
    const auto drawSkeletonPose = [&](const std::vector<math::Vector3>& positions,
                                      ImU32 color, float thickness) {
        for (size_t i = 0; i < skeleton.nodes.size() && i < positions.size(); ++i) {
            const auto& node = skeleton.nodes[i];
            if (node.boneIndex < 0 || node.parentIndex < 0) continue;
            if (node.parentIndex >= static_cast<int>(positions.size())) continue;
            ImVec2 point, parentPoint;
            if (project(positions[i], point) &&
                project(positions[static_cast<size_t>(node.parentIndex)], parentPoint))
                dl->AddLine(parentPoint, point, color, thickness);
        }
    };
    if (g_state.showGhost) {
        drawSkeletonPose(g_state.ghostPrevPositions, kGhostPast, 1.5f);
        drawSkeletonPose(g_state.ghostNextPositions, kGhostFuture, 1.5f);
    }

    bool boneClicked = false;
    /// @note ボーン本体: トラック有無で色分けし、ホバーで名前、クリックで詳細を選択する。
    ///       Curves モードもボーンクリックが必要なので、当たり判定・選択を有効化する。
    if (g_state.showBones || g_state.showBoneNames || g_state.showCurves ||
        g_state.selectedBoneNode >= 0) {
        std::vector<JointScreen> joints;
        joints.reserve(skeleton.nodes.size());
        const ImVec2 mousePos = ImGui::GetIO().MousePos;
        int hoveredJoint = -1;
        float hoveredDistanceSq = 12.0f * 12.0f;
        for (size_t i = 0;
             i < skeleton.nodes.size() && i < g_state.jointPositions.size(); ++i) {
            if (skeleton.nodes[i].boneIndex < 0) continue;
            ImVec2 point;
            if (!project(g_state.jointPositions[i], point)) continue;
            joints.push_back({ static_cast<int>(i), point });
            const float dx = mousePos.x - point.x;
            const float dy = mousePos.y - point.y;
            const float distanceSq = dx * dx + dy * dy;
            if (ctx.hovered && distanceSq < hoveredDistanceSq) {
                hoveredDistanceSq = distanceSq;
                hoveredJoint = static_cast<int>(i);
            }
        }

        const auto isAnimated = [&](int nodeIndex) {
            return nodeIndex >= 0 &&
                   nodeIndex < static_cast<int>(g_state.nodeHasTrack.size()) &&
                   g_state.nodeHasTrack[static_cast<size_t>(nodeIndex)] != 0;
        };
        const bool maskMode = g_maskPreview.active && g_maskPreview.loaded;
        const auto maskColorFor = [&](int nodeIndex, int alpha) {
            const auto& node = skeleton.nodes[static_cast<size_t>(nodeIndex)];
            return MaskWeightColorU32(
                asset::EvaluateAvatarMaskWeight(
                    g_maskPreview.mask,
                    asset::BuildSkeletonNodePath(skeleton, nodeIndex), node.name),
                alpha);
        };

        if (g_state.showBones) {
            for (const auto& joint : joints) {
                const auto& node = skeleton.nodes[static_cast<size_t>(joint.nodeIndex)];
                if (node.parentIndex < 0 ||
                    node.parentIndex >= static_cast<int>(g_state.jointPositions.size()))
                    continue;
                ImVec2 parentPoint;
                if (!project(g_state.jointPositions[static_cast<size_t>(node.parentIndex)],
                             parentPoint))
                    continue;
                const ImU32 boneColor = maskMode
                    ? maskColorFor(joint.nodeIndex, 235)
                    : (isAnimated(joint.nodeIndex) ? kAnimatedBone : kStaticBone);
                dl->AddLine(parentPoint, joint.pos, boneColor, 1.8f);
            }
        }
        if (g_state.showBones || g_state.showCurves) {
            /// @note Curves のみのときは線が無いので、ドットを少し控えめにして雑然さを抑える。
            const bool bonesShown = g_state.showBones;
            for (const auto& joint : joints) {
                const bool selected = joint.nodeIndex == g_state.selectedBoneNode;
                const bool hovered = joint.nodeIndex == hoveredJoint;
                const ImU32 restColor = maskMode
                    ? maskColorFor(joint.nodeIndex, bonesShown ? 235 : 150)
                    : (isAnimated(joint.nodeIndex)
                        ? IM_COL32(130, 240, 176, bonesShown ? 255 : 190)
                        : IM_COL32(168, 172, 182, bonesShown ? 190 : 120));
                const ImU32 jointColor = selected ? kSelectColor
                                       : hovered  ? IM_COL32(255, 255, 255, 255)
                                                  : restColor;
                dl->AddCircleFilled(joint.pos, (selected || hovered) ? 4.0f : 2.6f, jointColor);
                if (selected)
                    dl->AddCircle(joint.pos, 7.0f, IM_COL32(255, 216, 96, 200), 0, 1.5f);
            }
        }
        if (g_state.showBoneNames) {
            /// @note 密なスケルトンでラベルが潰れないよう、モードで表示対象を絞る。
            ///       0: 選択 + ホバーのみ / 1: アニメ有ボーン / 2: 全部
            ///       さらに 2 (All) では、直前に置いたラベルと近すぎる位置はスキップして重なりを防ぐ。
            std::vector<ImVec2> placedLabels;
            placedLabels.reserve(joints.size());
            const float minLabelGapSq = 18.0f * 18.0f;
            for (const auto& joint : joints) {
                const bool selected = joint.nodeIndex == g_state.selectedBoneNode;
                const bool hovered = joint.nodeIndex == hoveredJoint;
                bool show = selected || hovered;
                if (!show && g_state.labelMode == 1) show = isAnimated(joint.nodeIndex);
                else if (!show && g_state.labelMode == 2) show = true;
                if (!show) continue;

                /// @note 選択・ホバーは最優先で必ず出す。それ以外は近接ラベルを間引く。
                if (!selected && !hovered) {
                    bool tooClose = false;
                    for (const ImVec2& placed : placedLabels) {
                        const float dx = placed.x - joint.pos.x;
                        const float dy = placed.y - joint.pos.y;
                        if (dx * dx + dy * dy < minLabelGapSq) { tooClose = true; break; }
                    }
                    if (tooClose) continue;
                }
                placedLabels.push_back(joint.pos);

                const std::string shortName =
                    ShortBoneName(skeleton.nodes[static_cast<size_t>(joint.nodeIndex)].name);
                const ImU32 labelColor = (selected || hovered)
                    ? IM_COL32(255, 236, 170, 255)
                    : IM_COL32(210, 218, 228, 200);
                const ImVec2 textPos(joint.pos.x + 5.0f, joint.pos.y - 5.0f);
                /// @note 選択・ホバー名は背景を敷いて読みやすくする。
                if (selected || hovered) {
                    const ImVec2 textSize = ImGui::CalcTextSize(shortName.c_str());
                    dl->AddRectFilled(
                        ImVec2(textPos.x - 3.0f, textPos.y - 1.0f),
                        ImVec2(textPos.x + textSize.x + 3.0f, textPos.y + textSize.y + 1.0f),
                        IM_COL32(18, 20, 26, 200), 3.0f);
                }
                dl->AddText(textPos, labelColor, shortName.c_str());
            }
        }
        if (hoveredJoint >= 0) {
            const auto& node = skeleton.nodes[static_cast<size_t>(hoveredJoint)];
            ImGui::SetTooltip("%s\n%s", node.name.c_str(),
                              isAnimated(hoveredJoint) ? "Animated by this clip"
                                                       : "Bind pose only (no track)");
        }
        /// @note ドラッグ (オービット) と区別するため、移動量の小さいリリースだけを選択操作にする。
        if (ctx.hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
            ImGui::GetIO().MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < 16.0f) {
            boneClicked = hoveredJoint >= 0;
            /// @note 空クリックで選択解除 (-1)
            g_state.selectedBoneNode = hoveredJoint;
        }
    }

    if (g_state.showInfo) DrawInfoOverlay(ctx, clip);
    dl->PopClipRect();
    return boneClicked;
}

/// Mask Preview のメッシュ部位ヒットテスト。@return ホバー中のモデルノード index
int HitTestMaskMeshNode(const OverlayContext& ctx, const asset::Model& model)
{
    /// @note Mesh の CPU バウンドを画面へ投影し、クリック対象を軽量に求める。Preview は RenderTarget の画像なので GPU の深度を直接読めない。部位ノード単位のスクリーン領域で十分な操作感を保ち、GPU readback は避ける。
    const auto projectPoint = [&](const math::Vector3& world, ImVec2& out) {
        const math::Vector4 clipPos = g_state.debugViewProjection *
            math::Vector4{ world.x, world.y, world.z, 1.0f };
        if (clipPos.w <= 0.0001f) return false;
        out.x = ctx.origin.x + (clipPos.x / clipPos.w * 0.5f + 0.5f) * ctx.width;
        out.y = ctx.origin.y + (0.5f - clipPos.y / clipPos.w * 0.5f) * ctx.height;
        return true;
    };
    const ImVec2 mousePos = ImGui::GetIO().MousePos;
    float closestDistanceSq = FLT_MAX;
    int hovered = -1;
    for (size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex) {
        const auto& mesh = model.meshes[meshIndex];
        if (!mesh) continue;
        const int nodeIndex = model.FindNodeForMesh(static_cast<uint32_t>(meshIndex));
        if (nodeIndex < 0) continue;
        const math::Matrix4 meshWorld = meshIndex < g_state.meshPreviewWorlds.size()
            ? g_state.meshPreviewWorlds[meshIndex]
            : math::Matrix4::Identity();
        const auto transformMeshPoint = [&](const math::Vector3& point, math::Vector3& out) {
            const math::Vector4 transformed =
                meshWorld * math::Vector4{ point.x, point.y, point.z, 1.0f };
            if (std::abs(transformed.w) <= 0.0001f) return false;
            const float invW = 1.0f / transformed.w;
            out = { transformed.x * invW, transformed.y * invW, transformed.z * invW };
            return true;
        };
        math::Vector3 centerWorld;
        if (!transformMeshPoint(mesh->boundsCenter, centerWorld)) continue;
        ImVec2 center;
        if (!projectPoint(centerWorld, center)) continue;
        const float radius = (std::max)(mesh->boundsRadius, 0.05f);
        math::Vector3 radiusWorld;
        ImVec2 radiusPoint;
        const bool projectedRadius = transformMeshPoint(
            mesh->boundsCenter + math::Vector3{ radius, 0.0f, 0.0f }, radiusWorld) &&
            projectPoint(radiusWorld, radiusPoint);
        const float radiusDx = radiusPoint.x - center.x;
        const float radiusDy = radiusPoint.y - center.y;
        const float screenRadius = projectedRadius
            ? std::sqrt(radiusDx * radiusDx + radiusDy * radiusDy) : 0.0f;
        const float hitRadius = (std::max)(screenRadius, 14.0f);
        const float dx = mousePos.x - center.x;
        const float dy = mousePos.y - center.y;
        const float distanceSq = dx * dx + dy * dy;
        if (distanceSq <= hitRadius * hitRadius && distanceSq < closestDistanceSq) {
            closestDistanceSq = distanceSq;
            hovered = nodeIndex;
        }
    }
    return hovered;
}

/// @name カーブグラフ

/// X=赤 / Y=緑 / Z=青 の 3 系列を 1 枚のグラフへ重ね描きする。
void DrawCurveGraph(const char* title,
                    const std::vector<float>& cx,
                    const std::vector<float>& cy,
                    const std::vector<float>& cz,
                    float vMin, float vMax, const char* unit,
                    float playhead, float clipLength)
{
    ImGui::TextDisabled("%s", title);
    /// @note 系列の凡例。色だけだと «どれが X か» を毎回思い出す必要がある。
    const struct { const char* label; ImU32 color; } legend[3] = {
        { "X", IM_COL32(235,  96,  96, 255) },
        { "Y", IM_COL32(120, 224, 120, 255) },
        { "Z", IM_COL32(110, 170, 255, 255) },
    };
    for (const auto& entry : legend) {
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(entry.color), "%s", entry.label);
    }

    const float graphWidth = (std::max)(ImGui::GetContentRegionAvail().x, 120.0f);
    constexpr float GRAPH_HEIGHT = 62.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(title, ImVec2(graphWidth, GRAPH_HEIGHT));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 maxCorner(origin.x + graphWidth, origin.y + GRAPH_HEIGHT);
    dl->AddRectFilled(origin, maxCorner, kPanelBg, 4.0f);

    float range = vMax - vMin;
    /// @note 定数カーブは中央に平坦線として描く
    if (range < 0.0001f) range = 1.0f;
    const float pad = GRAPH_HEIGHT * 0.12f;
    const auto valueToY = [&](float v) {
        const float t = (v - vMin) / range;
        return maxCorner.y - pad - t * (GRAPH_HEIGHT - 2.0f * pad);
    };
    /// @note 縦の時間目盛り。4 等分で «どのあたりか» が読めれば十分。
    for (int i = 1; i < 4; ++i) {
        const float x = origin.x + graphWidth * static_cast<float>(i) / 4.0f;
        dl->AddLine(ImVec2(x, origin.y + 2.0f), ImVec2(x, maxCorner.y - 2.0f),
                    IM_COL32(52, 58, 70, 160));
    }
    /// @note 0 ライン (値域に 0 が含まれるときだけ) を薄く引いて符号を読めるようにする。
    if (vMin < 0.0f && vMax > 0.0f) {
        const float zeroY = valueToY(0.0f);
        dl->AddLine(ImVec2(origin.x, zeroY), ImVec2(maxCorner.x, zeroY),
                    IM_COL32(88, 96, 110, 190));
    }

    const size_t sampleCount = cx.size();
    const auto plotSeries = [&](const std::vector<float>& series, ImU32 color) {
        if (series.size() < 2) return;
        for (size_t i = 1; i < series.size(); ++i) {
            const float x0 = origin.x +
                static_cast<float>(i - 1) / (sampleCount - 1) * graphWidth;
            const float x1 = origin.x +
                static_cast<float>(i) / (sampleCount - 1) * graphWidth;
            dl->AddLine(ImVec2(x0, valueToY(series[i - 1])),
                        ImVec2(x1, valueToY(series[i])), color, 1.5f);
        }
    };
    plotSeries(cx, legend[0].color);
    plotSeries(cy, legend[1].color);
    plotSeries(cz, legend[2].color);

    /// @note 再生ヘッド (白い縦線)。スクラブ可能にして、グラフから直接時刻を掴める。
    const float headX = origin.x + playhead * graphWidth;
    dl->AddLine(ImVec2(headX, origin.y), ImVec2(headX, maxCorner.y),
                IM_COL32(245, 245, 245, 220), 1.0f);
    if (sampleCount >= 2) {
        /// @note 再生ヘッド上の実測値に点を打ち、数値行と «同じ瞬間» を見ていることを示す。
        const size_t index = static_cast<size_t>(std::lround(
            playhead * static_cast<float>(sampleCount - 1)));
        const std::vector<float>* series[3] = { &cx, &cy, &cz };
        for (int s = 0; s < 3; ++s)
            if (index < series[s]->size())
                dl->AddCircleFilled(ImVec2(headX, valueToY((*series[s])[index])),
                                    2.6f, legend[s].color);
    }
    if (hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left) && clipLength > 0.0001f) {
        const float t = std::clamp(
            (ImGui::GetIO().MousePos.x - origin.x) / graphWidth, 0.0f, 1.0f);
        SeekPreview(t * clipLength);
    }

    /// @note 右上に値域を表示 (単位付き)。
    char rangeText[64]{};
    std::snprintf(rangeText, sizeof(rangeText), "[%.2f, %.2f]%s", vMin, vMax, unit);
    const ImVec2 rangeSize = ImGui::CalcTextSize(rangeText);
    dl->AddText(ImVec2(maxCorner.x - rangeSize.x - 5.0f, origin.y + 3.0f),
                kTextMuted, rangeText);
    dl->AddRect(origin, maxCorner, kPanelBorder, 4.0f);
}

/// 選択ボーンの «今» の TRS・キー数・カーブ。無選択なら案内だけを出す。
void DrawSelectedBoneCard(const asset::Model* model, const asset::AnimationClip* clip)
{
    const bool hasSelection = g_state.selectedBoneValid && model && model->skeleton &&
        g_state.selectedBoneNode >= 0 &&
        g_state.selectedBoneNode < static_cast<int>(model->skeleton->nodes.size());
    if (!hasSelection) {
        if (g_state.showCurves)
            ImGui::TextDisabled("Curves: click a bone in the preview to inspect its channels.");
        return;
    }

    const auto& node =
        model->skeleton->nodes[static_cast<size_t>(g_state.selectedBoneNode)];
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.38f, 1.0f), "%s",
                       ShortBoneName(node.name).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("x##DeselectBone")) g_state.selectedBoneNode = -1;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Deselect this bone");
    ImGui::SameLine();
    if (ImGui::SmallButton("Focus##FocusBone") &&
        g_state.selectedBoneNode < static_cast<int>(g_state.jointPositions.size())) {
        g_state.pendingFocus =
            g_state.jointPositions[static_cast<size_t>(g_state.selectedBoneNode)];
        g_state.pendingDistance = (std::max)(g_state.boundsRadius * 0.8f, 0.05f);
        g_state.hasPendingFocus = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Move the camera to this bone (F)");

    const math::Vector3 euler = QuaternionToEulerDegrees(g_state.selectedBonePose.rotation);
    ImGui::TextDisabled(
        "T (%.2f, %.2f, %.2f)   R (%.0f, %.0f, %.0f)   S (%.2f, %.2f, %.2f)",
        g_state.selectedBonePose.translation.x,
        g_state.selectedBonePose.translation.y,
        g_state.selectedBonePose.translation.z,
        euler.x, euler.y, euler.z,
        g_state.selectedBonePose.scale.x,
        g_state.selectedBonePose.scale.y,
        g_state.selectedBonePose.scale.z);
    const bool hasAnyKeys = g_state.selectedBoneKeyCounts[0] > 0 ||
                            g_state.selectedBoneKeyCounts[1] > 0 ||
                            g_state.selectedBoneKeyCounts[2] > 0;
    ImGui::TextDisabled("Keys  P:%d  R:%d  S:%d%s",
                        g_state.selectedBoneKeyCounts[0],
                        g_state.selectedBoneKeyCounts[1],
                        g_state.selectedBoneKeyCounts[2],
                        hasAnyKeys ? "" : "  (bind pose only)");

    /// @name 選択ボーンの位置 / 回転カーブミニグラフ
    /// @note 数値の一瞬値だけでは補間の質 (急な段差・平坦な区間・往復) が読めないため、クリップ全長の XYZ カーブを重ねて再生ヘッド位置と合わせて確認できるようにする。
    if (!g_state.showCurves) return;
    if (g_state.curvePosX.empty()) {
        ImGui::TextDisabled("(no animated track for this bone)");
        return;
    }
    const float clipLength = ClipDurationSeconds(clip, g_state.timelineLength);
    const float playhead = clipLength > 0.0001f
        ? std::clamp(WrapTime(g_state.time, g_state.timelineLength) / clipLength, 0.0f, 1.0f)
        : 0.0f;
    ImGui::Spacing();
    DrawCurveGraph("Position", g_state.curvePosX, g_state.curvePosY, g_state.curvePosZ,
                   g_state.curvePosMin, g_state.curvePosMax, "", playhead, clipLength);
    DrawCurveGraph("Rotation", g_state.curveRotX, g_state.curveRotY, g_state.curveRotZ,
                   g_state.curveRotMin, g_state.curveRotMax, " deg", playhead, clipLength);
}

/// @name ルートモーション解析

/// 数値を «読める単位» で出す。1m 未満は cm へ落とさないと 0.01 ばかりが並ぶ。
void FormatDistance(char* buffer, size_t capacity, float meters)
{
    if (std::abs(meters) < 1.0f)
        std::snprintf(buffer, capacity, "%.1f cm", meters * 100.0f);
    else
        std::snprintf(buffer, capacity, "%.2f m", meters);
}

/// 「良い / 要確認 / まずい」を色で言い切る。数値だけだと «この 0.8 は大きいのか» が判らない。
void VerdictChip(const char* text, int level)
{
    static constexpr ImU32 kFg[3] = {
        IM_COL32(180, 255, 214, 255), IM_COL32(255, 226, 150, 255), IM_COL32(255, 176, 170, 255)
    };
    static constexpr ImU32 kBg[3] = {
        IM_COL32(32, 66, 50, 255), IM_COL32(76, 62, 26, 255), IM_COL32(82, 40, 42, 255)
    };
    const int index = std::clamp(level, 0, 2);
    InfoChip(text, kFg[index], kBg[index]);
}

/// 水平速度の面グラフ。平均線を重ねて «巡航しているか、脈打っているか» を形で見せる。
void DrawSpeedGraph(const RootMotionAnalysis& motion, float playhead, float clipLength)
{
    const float graphWidth = (std::max)(ImGui::GetContentRegionAvail().x, 120.0f);
    constexpr float GRAPH_HEIGHT = 56.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##RootMotionSpeed", ImVec2(graphWidth, GRAPH_HEIGHT));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 maxCorner(origin.x + graphWidth, origin.y + GRAPH_HEIGHT);
    dl->AddRectFilled(origin, maxCorner, kPanelBg, 4.0f);

    const size_t count = motion.speedSamples.size();
    if (count < 2) {
        dl->AddRect(origin, maxCorner, kPanelBorder, 4.0f);
        return;
    }
    /// @note 上下動の速度も同じ縦軸に載せたいので、両方の絶対値から目盛りを取る。
    float scale = motion.maxSpeed;
    for (const float v : motion.verticalSamples) scale = (std::max)(scale, std::abs(v));
    if (scale < 0.0001f) scale = 1.0f;

    const float pad = 5.0f;
    const float baseY = maxCorner.y - pad;
    const auto valueToY = [&](float v) {
        return baseY - std::clamp(v / scale, 0.0f, 1.0f) * (GRAPH_HEIGHT - pad * 2.0f);
    };
    const auto sampleX = [&](size_t i) {
        return origin.x + static_cast<float>(i) / static_cast<float>(count - 1) * graphWidth;
    };

    /// @note 面: ベースラインまで塗って «速度が乗っている時間» の面積を見せる。
    for (size_t i = 1; i < count; ++i) {
        const ImVec2 p0(sampleX(i - 1), valueToY(motion.speedSamples[i - 1]));
        const ImVec2 p1(sampleX(i), valueToY(motion.speedSamples[i]));
        const ImVec2 quad[4] = {
            p0, p1, ImVec2(p1.x, baseY), ImVec2(p0.x, baseY)
        };
        dl->AddConvexPolyFilled(quad, 4, IM_COL32(64, 132, 200, 80));
        dl->AddLine(p0, p1, kAccentHover, 1.6f);
    }
    /// @note 上下動 (bob) は絶対値で細く重ねる。歩幅ごとの «沈み込み» の周期が読める。
    for (size_t i = 1; i < count; ++i)
        dl->AddLine(ImVec2(sampleX(i - 1), valueToY(std::abs(motion.verticalSamples[i - 1]))),
                    ImVec2(sampleX(i), valueToY(std::abs(motion.verticalSamples[i]))),
                    IM_COL32(255, 186, 110, 170), 1.0f);

    /// @note 平均速度の水平線。Blend Tree の閾値に入れる値そのものなので目立たせる。
    const float avgY = valueToY(motion.averageSpeed);
    dl->AddLine(ImVec2(origin.x, avgY), ImVec2(maxCorner.x, avgY),
                IM_COL32(180, 226, 255, 150));

    const float headX = origin.x + playhead * graphWidth;
    dl->AddLine(ImVec2(headX, origin.y), ImVec2(headX, maxCorner.y),
                IM_COL32(245, 245, 245, 210), 1.0f);
    if (hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left) && clipLength > 0.0001f) {
        const float t =
            std::clamp((ImGui::GetIO().MousePos.x - origin.x) / graphWidth, 0.0f, 1.0f);
        SeekPreview(t * clipLength);
    }
    if (hovered) {
        const float t =
            std::clamp((ImGui::GetIO().MousePos.x - origin.x) / graphWidth, 0.0f, 1.0f);
        const size_t index = (std::min)(
            static_cast<size_t>(t * static_cast<float>(count - 1)), count - 1);
        ImGui::SetTooltip("%.3f s\nSpeed %.2f m/s\nVertical %+.2f m/s\nDrag to scrub",
                          t * clipLength, motion.speedSamples[index],
                          motion.verticalSamples[index]);
    }

    char label[48]{};
    std::snprintf(label, sizeof(label), "avg %.2f  peak %.2f m/s",
                  motion.averageSpeed, motion.maxSpeed);
    dl->AddText(ImVec2(origin.x + 6.0f, origin.y + 3.0f), kTextMuted, label);
    dl->AddRect(origin, maxCorner, kPanelBorder, 4.0f);
}

void DrawRootMotionCard(const asset::AnimationClip* clip)
{
    const RootMotionAnalysis& motion = g_state.rootMotion;
    if (!motion.valid) {
        ImGui::TextDisabled("Root motion: no skeleton root bone for this clip.");
        return;
    }

    char distance[32]{}, net[32]{}, bob[32]{}, slide[32]{};
    FormatDistance(distance, sizeof(distance), motion.pathLength);
    FormatDistance(net, sizeof(net), motion.netDistance);
    FormatDistance(bob, sizeof(bob), motion.verticalRange);
    FormatDistance(slide, sizeof(slide), motion.plantedSlide);

    ImGui::TextDisabled(
        "Travel %s (net %s)   Turn %+.0f deg   Speed %.2f avg / %.2f peak m/s   Bob %s",
        distance, net, motion.netTurnDegrees,
        motion.averageSpeed, motion.maxSpeed, bob);

    const float clipLength = ClipDurationSeconds(clip, g_state.timelineLength);
    const float playhead = clipLength > 0.0001f
        ? std::clamp(WrapTime(g_state.time, g_state.timelineLength) / clipLength, 0.0f, 1.0f)
        : 0.0f;
    DrawSpeedGraph(motion, playhead, clipLength);

    /// @name ループ整合
    /// @note 判定は «最大回転差» を主に見る。位置差は多くのリグでほぼ 0 (回転のみのトラック) で、
    ///       跳ねの原因になるのは決まって «末尾でまだ振り切っている» 関節の角度だから。
    const int loopLevel = motion.loopRotationGap < 1.0f ? 0
                        : motion.loopRotationGap < 5.0f ? 1
                                                        : 2;
    static constexpr const char* kLoopVerdict[3] = { "Loop: seamless", "Loop: minor pop",
                                                     "Loop: pops" };
    VerdictChip(kLoopVerdict[loopLevel], loopLevel);
    ImGui::SameLine(0.0f, 8.0f);
    char loopPos[32]{};
    FormatDistance(loopPos, sizeof(loopPos), motion.loopPositionGap);
    if (motion.loopWorstBone.empty()) {
        ImGui::TextDisabled("first vs last frame: %.2f deg", motion.loopRotationGap);
    } else {
        ImGui::TextDisabled("first vs last frame: %.2f deg on %s, %s offset, %.2f m/s speed step",
                            motion.loopRotationGap, motion.loopWorstBone.c_str(),
                            loopPos, motion.loopSpeedGap);
    }

    /// @name 接地と足滑り
    if (motion.footNodes[0] < 0) {
        ImGui::TextDisabled("Feet: no foot bone found (no 'foot' / 'toe' / 'ankle' in the rig).");
        return;
    }
    char feet[96]{};
    if (motion.footNodes[1] >= 0)
        std::snprintf(feet, sizeof(feet), "%s / %s",
                      motion.footNames[0].c_str(), motion.footNames[1].c_str());
    else
        std::snprintf(feet, sizeof(feet), "%s", motion.footNames[0].c_str());

    if (motion.inPlace) {
        /// @note その場クリップでは足が滑るのが «正しい»。答えるべきは «何 m/s で走らせるか»。
        VerdictChip("In-place", 1);
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::TextDisabled(
            "%s slide %s while planted. Root stays put, so drive it at ~%.2f m/s to match.",
            feet, slide, motion.requiredRootSpeed);
        return;
    }

    const float slideRatio = motion.pathLength > 0.0001f
        ? motion.plantedSlide / motion.pathLength : 0.0f;
    const int slideLevel = slideRatio < 0.05f ? 0 : slideRatio < 0.15f ? 1 : 2;
    static constexpr const char* kSlideVerdict[3] = { "Feet planted", "Feet slip",
                                                     "Feet slide" };
    VerdictChip(kSlideVerdict[slideLevel], slideLevel);
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::TextDisabled("%s: %s over %.2f s planted (%.0f%% of travel)",
                        feet, slide, motion.plantedSeconds, slideRatio * 100.0f);
}

/// @name ツールバー

/// 対象名 + Display / View ポップアップの 1 行目。
void DrawHeaderRow()
{
    InfoChip(g_state.target.label.c_str(), IM_COL32(190, 220, 255, 255),
             IM_COL32(40, 62, 88, 255));
    if (g_state.target.mode == PreviewTarget::Mode::Transition) {
        ImGui::SameLine(0.0f, 6.0f);
        char blendText[32]{};
        std::snprintf(blendText, sizeof(blendText), "Blend %.0f%%",
                      g_state.currentBlendWeight * 100.0f);
        InfoChip(blendText, IM_COL32(180, 255, 214, 255), IM_COL32(36, 68, 54, 255));
    }

    /// @note 右端へ寄せる。2 つの SmallButton ぶんの幅は固定なので実測せず定数でよい。
    constexpr float BUTTONS_WIDTH = 116.0f;
    ImGui::SameLine();
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float remaining = ImGui::GetContentRegionAvail().x;
    if (remaining > BUTTONS_WIDTH)
        ImGui::SetCursorScreenPos(
            ImVec2(cursor.x + remaining - BUTTONS_WIDTH, cursor.y));

    if (ImGui::SmallButton("Display")) ImGui::OpenPopup("##DisplayPopup");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Bone label density and onion skin spacing");
    if (ImGui::BeginPopup("##DisplayPopup")) { DrawDisplayPopup(); ImGui::EndPopup(); }
    ImGui::SameLine(0.0f, 6.0f);
    if (ImGui::SmallButton("View")) ImGui::OpenPopup("##ViewPopup");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Grid, background, wireframe, FOV and camera presets");
    if (ImGui::BeginPopup("##ViewPopup")) { DrawViewPopup(); ImGui::EndPopup(); }
}

/// Model (ドロップ差し替え) と Clip (パッケージ内 .anim) を横並びで置く。
/// @note 「どのモデルで、どのクリップを見ているか」は 1 つの問い。器を上端・クリップを最下端に離すと視線が往復する。
void DrawSourceRow(bool hasGeometry)
{
    const float avail = ImGui::GetContentRegionAvail().x;
    const bool showClip = g_state.target.mode == PreviewTarget::Mode::Clip;
    const float modelWidth = showClip ? avail * 0.52f : avail;

    const std::string geoName = hasGeometry
        ? util::FileSystem::GetFilename(g_state.target.modelPath)
        : std::string("Drop a skinned model here");
    if (!hasGeometry) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(120, 64, 48, 255));
    ImGui::Button(geoName.c_str(), ImVec2(modelWidth, 0.0f));
    if (!hasGeometry) ImGui::PopStyleColor();
    AcceptPreviewAssetDrop();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Skinned model used as the preview body.\n"
            "Drag a .fbx / .fzasset here to swap it (the clip is kept).");

    if (!showClip) return;

    /// @note モデル内蔵クリップに加え、パッケージ配下の .anim も列挙する。FBZZ は 1 クリップ = 1 FBX なのでモデル内蔵クリップは常に空で、これが無いと MiniBot.fbx を選んでも Idle / Walk / Run … を切り替えられない。
    const auto& anims = CollectPackageAnims(g_state.target.modelPath);
    const asset::Model* model = asset::AssetManager::LoadAndGet<asset::Model>(g_state.target.modelPath);
    const bool hasEmbedded = model && model->clips.size() > 1;
    if (!hasEmbedded && anims.empty()) return;

    std::string currentLabel = "<bind pose>";
    if (!g_state.target.animAssetPath.empty())
        currentLabel = util::FileSystem::GetFilename(g_state.target.animAssetPath);
    else if (const asset::AnimationClip* c = FindModelClip(model, g_state.target.clipName))
        currentLabel = c->name;

    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##PreviewClip", currentLabel.c_str())) {
        /// @note バインドポーズ (クリップ無し) へ戻す選択肢
        if (ImGui::Selectable("<bind pose>", g_state.target.animAssetPath.empty() &&
                                             g_state.target.clipName.empty())) {
            g_state.target.animAssetPath.clear();
            g_state.target.clipName.clear();
            g_state.time = 0.0f;
        }
        if (hasEmbedded) {
            for (const auto& clip : model->clips) {
                const bool selected = g_state.target.animAssetPath.empty() &&
                                      clip.name == g_state.target.clipName;
                ImGui::PushID(&clip);
                if (ImGui::Selectable(clip.name.c_str(), selected)) {
                    g_state.target.animAssetPath.clear();
                    g_state.target.clipName = clip.name;
                    g_state.time = 0.0f;
                }
                ImGui::PopID();
                if (selected) ImGui::SetItemDefaultFocus();
            }
        }
        for (const auto& animPath : anims) {
            const std::string name = util::FileSystem::GetFilename(animPath);
            const bool selected = animPath == g_state.target.animAssetPath;
            /// @note 埋め込みクリップと同名の .anim や、別フォルダの同名ファイルがありうる。
            ImGui::PushID(animPath.c_str());
            if (ImGui::Selectable(name.c_str(), selected)) {
                g_state.target.animAssetPath = animPath;
                g_state.target.clipName.clear();
                g_state.time = 0.0f;
                g_state.playing = true;
            }
            ImGui::PopID();
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Clips found in this model's package (%zu .anim files).",
                          anims.size());
}

/// オーバーレイのトグル帯。幅を等分し、切り替えても «列» がずれないようにする。
void DrawOverlayToggleRow()
{
    struct Toggle { const char* label; bool* value; const char* tooltip; };
    const Toggle toggles[] = {
        { "Mesh",   &g_state.showMesh,      "Toggle mesh rendering (Off = skeleton only)" },
        { "Bones",  &g_state.showBones,
          "Skeleton overlay\nGreen: animated by this clip / Gray: bind pose only\nClick a joint to inspect it" },
        { "Names",  &g_state.showBoneNames, "Bone name labels (density: Display menu)" },
        { "Trail",  &g_state.showTrail,     "Root bone trajectory over the whole clip" },
        { "Ghost",  &g_state.showGhost,     "Onion skin: skeleton a few frames before / after" },
        { "Info",   &g_state.showInfo,      "Time / frame / track coverage overlay" },
        { "Curves", &g_state.showCurves,    "Position / rotation curves of the selected bone" },
        { "Motion", &g_state.showRootMotion,
          "Root motion analysis\nTravel distance, speed graph, loop seam and foot sliding" },
    };
    constexpr int COUNT = static_cast<int>(sizeof(toggles) / sizeof(toggles[0]));
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float width = (std::max)(
        (ImGui::GetContentRegionAvail().x - spacing * (COUNT - 1)) / COUNT, 44.0f);
    for (int i = 0; i < COUNT; ++i) {
        if (i > 0) ImGui::SameLine();
        ToolbarToggle(toggles[i].label, *toggles[i].value, toggles[i].tooltip, width);
    }
}

/// 再生ボタン群 + タイムライン + 速度 + ループ。
void DrawTransportRow(const asset::AnimationClip* clip)
{
    const float spacing = ImGui::GetStyle().ItemSpacing.x;

    if (TransportButton("ToStart", "|<", "Jump to start (Home)")) SeekPreview(0.0f);
    ImGui::SameLine(0.0f, 2.0f);
    if (TransportButton("StepBack", "<", "Previous frame (Left / Shift+Left = 10)"))
        StepPreviewFrames(-1, clip);
    ImGui::SameLine(0.0f, 2.0f);
    if (TransportButton("PlayPause",
                        g_state.playing ? icons::Or(icons::kPause, "||")
                                        : icons::Or(icons::kPlay, ">"),
                        g_state.playing ? "Pause (Space)" : "Play (Space)", true))
        g_state.playing = !g_state.playing;
    ImGui::SameLine(0.0f, 2.0f);
    if (TransportButton("StepFwd", ">", "Next frame (Right / Shift+Right = 10)"))
        StepPreviewFrames(1, clip);
    ImGui::SameLine(0.0f, 2.0f);
    if (TransportButton("ToEnd", ">|", "Jump to end (End)"))
        SeekPreview(g_state.timelineLength);

    /// @note 速度とループを右端へ固定し、残りをタイムラインへ渡す。
    constexpr float SPEED_WIDTH = 62.0f;
    const float loopWidth = ImGui::CalcTextSize("Loop").x + ImGui::GetFrameHeight() +
                            ImGui::GetStyle().ItemInnerSpacing.x + 4.0f;
    const float reserved = SPEED_WIDTH + loopWidth + spacing * 2.0f;
    const float timelineWidth =
        (std::max)(ImGui::GetContentRegionAvail().x - reserved, 80.0f);

    ImGui::SameLine();
    DrawPreviewTimeline(clip, timelineWidth);

    ImGui::SameLine();
    ImGui::SetNextItemWidth(SPEED_WIDTH);
    ImGui::DragFloat("##PreviewSpeed", &g_state.speed, 0.05f, 0.1f, 4.0f, "x%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Playback speed\nRight-click for presets");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##SpeedPresets");
    if (ImGui::BeginPopup("##SpeedPresets")) {
        for (const float preset : { 0.1f, 0.25f, 0.5f, 1.0f, 2.0f }) {
            char label[16]{};
            std::snprintf(label, sizeof(label), "x%.2f", preset);
            if (ImGui::Selectable(label, std::abs(g_state.speed - preset) < 0.001f))
                g_state.speed = preset;
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    ImGui::Checkbox("Loop", &g_state.loop);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Loop playback (L)");

    /// @note フレーム位置の数値。タイムラインの «だいたい» を確定値で裏付ける。
    const float fps = ClipFrameRate(clip);
    const float wrapped = WrapTime(g_state.time, g_state.timelineLength);
    ImGui::TextDisabled("Frame %d / %d     %.3f / %.3f s  @ %.0f fps",
                        static_cast<int>(std::lround(wrapped * fps)),
                        static_cast<int>(std::lround(g_state.timelineLength * fps)),
                        wrapped, g_state.timelineLength, fps);
}

/// ジオメトリが無い間に出す案内。黒い矩形より «何をすればよいか» を見せる。
void DrawDropZone(const char* line1, const char* line2, float width, float height)
{
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##PreviewDropZone", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    AcceptPreviewAssetDrop();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 maxCorner(origin.x + width, origin.y + height);
    dl->AddRectFilled(origin, maxCorner,
                      hovered ? IM_COL32(34, 40, 50, 255) : IM_COL32(26, 28, 34, 255), 6.0f);
    dl->AddRect(origin, maxCorner,
                hovered ? kAccentHover : IM_COL32(104, 112, 128, 180), 6.0f, 0, 1.5f);

    const ImVec2 size1 = ImGui::CalcTextSize(line1);
    const ImVec2 size2 = ImGui::CalcTextSize(line2);
    const float centerY = origin.y + height * 0.5f;
    dl->AddText(ImVec2(origin.x + (width - size1.x) * 0.5f, centerY - size1.y - 2.0f),
                kTextBright, line1);
    dl->AddText(ImVec2(origin.x + (width - size2.x) * 0.5f, centerY + 4.0f),
                kTextMuted, line2);
}

/// ホバー中だけ出す操作ヒント。角丸の帯に載せて背景の明暗から独立させる。
void DrawViewportHint(const OverlayContext& ctx)
{
    /// @note 幅が足りないときは短い方へ落とす。はみ出したヒントは «ノイズ» にしかならない。
    const char* full = "Drag: Orbit   Alt/Middle: Pan   Wheel: Zoom   "
                       "DblClick: Frame   Space: Play   F: Focus";
    const char* shortForm = "Drag: Orbit   Wheel: Zoom   Space: Play";
    const char* hint = ImGui::CalcTextSize(full).x + 22.0f <= ctx.width ? full : shortForm;
    const ImVec2 size = ImGui::CalcTextSize(hint);
    if (size.x + 22.0f > ctx.width) return;
    const ImVec2 boxMin(ctx.origin.x + 8.0f, ctx.maxCorner.y - size.y - 12.0f);
    const ImVec2 boxMax(boxMin.x + size.x + 14.0f, boxMin.y + size.y + 6.0f);
    ctx.drawList->AddRectFilled(boxMin, boxMax, IM_COL32(14, 16, 20, 170),
                                (boxMax.y - boxMin.y) * 0.5f);
    ctx.drawList->AddText(ImVec2(boxMin.x + 7.0f, boxMin.y + 3.0f),
                          IM_COL32(186, 194, 208, 230), hint);
}

/// オービット / パン / ズーム。マウス操作の «唯一の» 解釈場所。
void HandleViewportCamera(bool hovered, bool active)
{
    const ImGuiIO& io = ImGui::GetIO();
    /// @note 中ボタンか Alt+左。どちらも «このビューポートで» 押し始めた場合だけ効かせる。
    const bool panDrag = active &&
        (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
         (io.KeyAlt && ImGui::IsMouseDragging(ImGuiMouseButton_Left)));

    if (panDrag) {
        math::Vector3 right, up, forward;
        PreviewCameraBasis(right, up, forward);
        /// @note 画面 1px あたりの移動量を距離に比例させ、寄っても引いても «掴んだ点が
        ///       指に付いてくる» 感触を保つ。
        const float scale = g_state.distance * 0.0016f;
        g_state.focus = {
            g_state.focus.x - right.x * io.MouseDelta.x * scale + up.x * io.MouseDelta.y * scale,
            g_state.focus.y - right.y * io.MouseDelta.x * scale + up.y * io.MouseDelta.y * scale,
            g_state.focus.z - right.z * io.MouseDelta.x * scale + up.z * io.MouseDelta.y * scale
        };
    } else if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        g_state.yaw -= io.MouseDelta.x * 0.012f;
        g_state.pitch = std::clamp(g_state.pitch + io.MouseDelta.y * 0.010f, -1.35f, 1.35f);
    }

    if (hovered && io.MouseWheel != 0.0f && g_state.distance > 0.0f) {
        g_state.distance *= (io.MouseWheel > 0.0f) ? 0.88f : 1.14f;
        g_state.distance = std::clamp(g_state.distance, 0.05f, 5000.0f);
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        /// @note ダブルクリックで再フレーミング (Unity の F 相当)
        g_state.needsFraming = true;
}

} // namespace

} // namespace fbzz::editor::animpreview

namespace fbzz::editor {

using namespace fbzz::editor::animpreview;

bool DrawAnimationPreviewWidget(EditorContext& ctx, float previewHeight)
{
    if (!g_maskPreview.active)
        UpdatePreviewTarget(ctx);
    if (g_state.target.mode == PreviewTarget::Mode::None) return false;

    ImGui::PushID("##AnimationPreviewWidget");

    /// @name ヘッダー: 対象名 + Display / View
    DrawHeaderRow();

    /// @name ジオメトリ枠 + クリップ選択
    /// @note FBZZ は 1 クリップ = 1 FBX なので、.anim の隣にスキンメッシュが無い。どのモデルで再生しているかを常に見せ、D&D で差し替えられるようにする。
    const bool hasGeometry =
        !g_state.target.modelPath.empty() &&
        LoadsAsPreviewableGeometry(g_state.target.modelPath);
    DrawSourceRow(hasGeometry);

    /// @note ジオメトリが無い間は黒画面を出さず、何をすればよいか明示する。
    if (!hasGeometry) {
        DrawDropZone("No skinned model for this clip",
                     "Drag a model (.fbx / .fzasset) here to preview it",
                     (std::max)(ImGui::GetContentRegionAvail().x, 64.0f),
                     (std::max)(previewHeight, 96.0f));
        ImGui::PopID();
        return true;
    }

    /// @name デバッグ表示トグルバー
    /// @note Unity の Preview は絵を見るだけだが、ここではボーン・軌跡・残像・トラック情報を重ねてアニメーションデータそのものをデバッグできるようにする (本エンジンの差別化)。
    DrawOverlayToggleRow();

    /// @name プレビュー画像 (オービット操作付き)
    const float width = (std::max)(ImGui::GetContentRegionAvail().x, 64.0f);
    const float height = (std::max)(previewHeight, 96.0f);
    const ImVec2 imageOrigin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##PreviewImage", ImVec2(width, height),
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_MouseButtonMiddle);
    const bool imageHovered = ImGui::IsItemHovered();
    const bool imageActive = ImGui::IsItemActive();
    /// @note Inspector 埋め込み時、ホイールズームが親ウィンドウのスクロールに化けないようホバー中はホイール入力の所有権をこのアイテムに移す。
    if (imageHovered) ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    /// @note FBX / .anim をプレビュー画面へ直接ドロップして対象を差し替えられるようにする。
    AcceptPreviewAssetDrop();

    HandleViewportCamera(imageHovered, imageActive);

    const asset::Model* overlayModel =
        asset::AssetManager::LoadAndGet<asset::Model>(g_state.target.modelPath);
    const asset::AnimationClip* overlayClip = CurrentClip(overlayModel);
    /// @note キー操作はビューポートに触れているときだけ。パネル全体に広げると、
    ///       クリップコンボを開いたまま矢印キーを押した場合に両方が動く。
    HandlePreviewHotkeys(imageHovered, overlayClip);

    AdvancePlayback();
    const bool rendered = RenderPreviewFrame(ctx, width / height);

    OverlayContext overlay;
    overlay.drawList = ImGui::GetWindowDrawList();
    overlay.origin = imageOrigin;
    overlay.maxCorner = ImVec2(imageOrigin.x + width, imageOrigin.y + height);
    overlay.width = width;
    overlay.height = height;
    overlay.hovered = imageHovered;

    overlay.drawList->AddRectFilled(overlay.origin, overlay.maxCorner,
                                    IM_COL32(20, 22, 26, 255), 6.0f);
    if (rendered && ctx.imguiRenderer && ctx.resources) {
        if (void* rawID =
                ctx.imguiRenderer->GetImTextureID(g_gpu.renderTarget, *ctx.resources, 0)) {
            /// @note 角丸で切り抜いて «板» ではなくビューとして見せる。
            overlay.drawList->AddImageRounded(widgets::ToImTextureID(rawID), overlay.origin,
                                              overlay.maxCorner, ImVec2(0.0f, 0.0f),
                                              ImVec2(1.0f, 1.0f),
                                              IM_COL32_WHITE, 6.0f);
        }
    } else {
        const char* message = "Preview unavailable (model or clip not found)";
        const ImVec2 textSize = ImGui::CalcTextSize(message);
        overlay.drawList->AddText(
            ImVec2(imageOrigin.x + (width - textSize.x) * 0.5f,
                   imageOrigin.y + (height - textSize.y) * 0.5f),
            kTextMuted, message);
    }
    overlay.drawList->AddRect(overlay.origin, overlay.maxCorner,
                              imageHovered ? IM_COL32(120, 132, 152, 255) : kPanelBorder,
                              6.0f);

    /// @name デバッグオーバーレイ (ボーン / 軌跡 / ゴースト / 情報)
    /// @note メッシュ描画と同じ ViewProjection で CPU 側から投影し、RT の上に 2D で重ねる。
    int hoveredMaskMeshNode = -1;
    if (g_maskPreview.active && g_maskPreview.loaded && overlayModel && imageHovered) {
        hoveredMaskMeshNode = HitTestMaskMeshNode(overlay, *overlayModel);
        if (hoveredMaskMeshNode >= 0) {
            const std::string path = MaskPathForModelNode(*overlayModel, hoveredMaskMeshNode);
            ImGui::SetTooltip("%s\nClick to select this mask node", path.c_str());
        }
    }
    const bool previewBoneClicked = rendered && overlayModel
        ? DrawSkeletonOverlay(overlay, *overlayModel, overlayClip)
        : false;
    if (g_state.view.showAxisGizmo && rendered) DrawAxisGizmo(overlay);
    if (imageHovered) DrawViewportHint(overlay);

    if (g_maskPreview.active && g_maskPreview.loaded && !previewBoneClicked &&
        hoveredMaskMeshNode >= 0 && imageHovered &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
        ImGui::GetIO().MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < 16.0f &&
        overlayModel) {
        SetAnimationMaskPreviewSelection(
            MaskPathForModelNode(*overlayModel, hoveredMaskMeshNode));
    }

    /// @name 再生コントロール
    DrawTransportRow(overlayClip);

    /// @name ルートモーション解析
    if (g_state.showRootMotion) {
        ImGui::Separator();
        DrawRootMotionCard(overlayClip);
    }

    /// @name 選択中ボーンのライブ詳細 + カーブ
    DrawSelectedBoneCard(overlayModel, overlayClip);

    ImGui::PopID();
    return true;
}

void DrawAnimationPreviewPanelContent(EditorContext& ctx)
{
    /// @note ビューポート以外が使う高さを «実測» する。マジック数値の見積もりは UI スケールやフォント、Curves 表示の有無で外れ、ビューポートが下の行を押し出すか縮みすぎるかしていた。
    const ImGuiStyle& style = ImGui::GetStyle();
    const float row = ImGui::GetFrameHeight() + style.ItemSpacing.y;
    /// @note ヘッダー / ソース / トグル / トランスポート
    float controlsHeight = row * 4.0f
                         /// @note フレーム数値行
                         + ImGui::GetTextLineHeightWithSpacing();
    if (g_state.showRootMotion) {
        /// @note 区切り + 概要行 + 速度グラフ 56px + ループ行 + 足行 (チップは 1 行ぶん)
        controlsHeight += ImGui::GetTextLineHeightWithSpacing() * 3.0f + 56.0f
                        + style.ItemSpacing.y * 3.0f;
    }
    if (g_state.selectedBoneNode >= 0) {
        /// @note 名前 + 小ボタン行
        controlsHeight += ImGui::GetFrameHeightWithSpacing()
                        /// @note TRS + Keys
                        + ImGui::GetTextLineHeightWithSpacing() * 2.0f;
        /// @note カーブは 2 枚。見出し行 + グラフ 62px を 2 組。
        if (g_state.showCurves)
            controlsHeight += (ImGui::GetTextLineHeightWithSpacing() + 62.0f +
                               style.ItemSpacing.y) * 2.0f;
    } else if (g_state.showCurves) {
        controlsHeight += ImGui::GetTextLineHeightWithSpacing();
    }

    const float previewHeight =
        (std::max)(ImGui::GetContentRegionAvail().y - controlsHeight, 120.0f);
    if (DrawAnimationPreviewWidget(ctx, previewHeight)) return;

    /// @note 対象が無い間もドロップ領域として機能させ、FBX を落とすだけでプレビューを始められる。
    DrawDropZone("Select an Animation State, Transition, or an .anim asset",
                 "or drop an FBX / .anim here to preview",
                 (std::max)(ImGui::GetContentRegionAvail().x, 64.0f),
                 (std::max)(ImGui::GetContentRegionAvail().y, 96.0f));
}

} // namespace fbzz::editor
