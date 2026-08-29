/// @file    VFXTimelinePanel.cpp
/// @brief   生存窓の帯を並べて掴んで動かす。スクラブは VFXComponent の時刻を直接指定する。
/// @author  Hasegawa Jin
/// @date    2026-08-26
#include <Editor/Panels/VFXTimelinePanel.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/Util/SceneEditUtils.hpp>
#include <Editor/Util/Selection.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/VFXComponent.hpp>
#include <Engine/Scene/Components/VFXElement.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>

#include <imgui.h>
#include <algorithm>
#include <cstdio>

namespace fbzz::editor {
namespace {

constexpr float kLabelWidth  = 190.0f;
constexpr float kRowHeight   = 22.0f;
constexpr float kIndentPx    = 10.0f;
constexpr float kEdgeGrabPx  = 5.0f;
constexpr float kMinDuration = 0.01f;

// 帯の色。何で始まるかが一目で分かるよう、時間 / ループ / 発火待ちを塗り分ける。
constexpr ImU32 kBarNormal    = IM_COL32(86, 132, 196, 210);
constexpr ImU32 kBarLooping   = IM_COL32(96, 168, 120, 210);
constexpr ImU32 kBarTriggered = IM_COL32(196, 138, 70, 210);

[[nodiscard]] ImU32 BarColor(bool triggered, bool loop)
{
    if (triggered) return kBarTriggered;
    if (loop) return kBarLooping;
    return kBarNormal;
}

// 入れ子 VFX の内側へは降りない (その VFXComponent が自分の時間軸を持つ)。
[[nodiscard]] bool IsNestedRoot(scene::GameObject& gameObject)
{
    return gameObject.GetComponent<scene::VFXComponent>() != nullptr;
}

[[nodiscard]] std::string FormatSeconds(float seconds)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2fs", seconds);
    return buffer;
}

} // namespace

scene::GameObject* VFXTimelinePanel::ResolveRoot(EditorContext& ctx) const
{
    if (ctx.activeScene == nullptr) return nullptr;

    // 選択から遡って探す。子を選んだままでもタイムラインが消えないようにする。
    if (scene::GameObject* selected = ctx.GetSelectedGO()) {
        for (scene::GameObject* node = selected; node != nullptr; node = node->GetParent())
            if (node->GetComponent<scene::VFXComponent>() != nullptr) return node;
    }

    // 選択が無いときはシーンのルートから 1 つ拾う。プレハブ編集モードでは
    // .vfx のルートが唯一のルートなので、開いた直後から中身が出る。
    for (scene::GameObject* root : ctx.activeScene->GetRootGameObjects())
        if (root != nullptr && root->GetComponent<scene::VFXComponent>() != nullptr) return root;

    return nullptr;
}

void VFXTimelinePanel::CollectTracks(scene::Scene& scene, scene::GameObject& root, int depth)
{
    const int childCount = root.GetChildCount();
    for (int index = 0; index < childCount; ++index) {
        scene::GameObject* child = root.GetChild(index);
        if (child == nullptr) continue;

        Track track;
        track.entity = child->GetID();
        track.label = child->name;
        track.depth = depth;

        if (auto* element = child->GetComponent<scene::VFXElement>()) {
            track.fromEmitter = false;
            track.start = element->startDelay;
            track.duration = element->duration;
            track.loop = element->loop;
            track.trigger = element->trigger;
        } else if (auto* emitter = child->GetComponent<scene::ParticleEmitter>()) {
            track.fromEmitter = true;
            track.start = emitter->settings.startDelay;
            track.duration = emitter->settings.duration;
            track.loop = emitter->settings.loop;
        } else {
            // 窓を持たない = 層のまとめ役。行は出すが帯は描かない。
            track.isGroup = true;
        }
        m_tracks.push_back(std::move(track));

        if (!IsNestedRoot(*child)) CollectTracks(scene, *child, depth + 1);
    }
}

void VFXTimelinePanel::ApplyTrack(scene::Scene& scene, const Track& track,
                                  float start, float duration) const
{
    scene::GameObject* gameObject = scene.GetGameObject(track.entity);
    if (gameObject == nullptr) return;

    if (track.fromEmitter) {
        if (auto* emitter = gameObject->GetComponent<scene::ParticleEmitter>()) {
            emitter->settings.startDelay = start;
            emitter->settings.duration = duration;
        }
        return;
    }
    if (auto* element = gameObject->GetComponent<scene::VFXElement>()) {
        element->startDelay = start;
        element->duration = duration;
    }
}

void VFXTimelinePanel::OnRenderContent(EditorContext& ctx)
{
    scene::GameObject* root = ResolveRoot(ctx);
    if (root == nullptr) {
        ImGui::TextDisabled("VFX ルートが見つかりません。");
        ImGui::TextDisabled("Create > VFX > VFX Root で作るか、.vfx を開いてください。");
        return;
    }

    auto* vfx = root->GetComponent<scene::VFXComponent>();
    if (vfx == nullptr) return;
    scene::Scene& scene = *ctx.activeScene;

    m_tracks.clear();
    CollectTracks(scene, *root, 0);

    // 表示する尺。ランタイムが算出した実効尺を使い、まだ 0 なら帯から求める。
    float span = vfx->duration > 0.0f ? vfx->duration : vfx->resolvedDuration;
    if (span <= 0.0f) {
        for (const Track& track : m_tracks)
            span = (std::max)(span, track.start + (std::max)(track.duration, 0.0f));
    }
    if (span <= 0.0f) span = 1.0f;

    // ── ヘッダー ──
    ImGui::Text("%s", root->name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%s / %d tracks)", FormatSeconds(span).c_str(),
                        static_cast<int>(m_tracks.size()));

    if (ImGui::Button(vfx->playing ? "Pause" : "Play")) {
        m_scrubTime = -1.0f;
        if (vfx->playing) vfx->Pause();
        else vfx->Resume();
    }
    ImGui::SameLine();
    if (ImGui::Button("Restart")) {
        m_scrubTime = -1.0f;
        vfx->Restart();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &vfx->loop);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat("Speed", &vfx->speed, 0.0f, 4.0f, "%.2fx");

    float scrub = m_scrubTime >= 0.0f ? m_scrubTime : vfx->time;
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat("##scrub", &scrub, 0.0f, span, "t = %.3fs")) {
        m_scrubTime = scrub;
        vfx->playing = false;
    }
    // 掴んでいる間だけ時刻を固定する。離しても固定し続けると
    // 「Play を押しても動かない」に見える。
    if (m_scrubTime >= 0.0f && ImGui::IsItemDeactivated()) m_scrubTime = -1.0f;
    vfx->editorScrubTime = m_scrubTime;
    vfx->editorScrubFrame = Time::frameCount;

    // WHY 断っておくか: 曲線で駆動する層 (光・デカール・メッシュ・画面演出) は
    //     時刻だけで決まるのでどこへ飛ばしても同じ絵が出る。一方パーティクルは
    //     «それまでの積み重ね» なので、巻き戻すと頭から出直す。黙っていると
    //     「スクラブしたら煙が消えた」をバグとして追うことになる。
    if (m_scrubTime >= 0.0f)
        ImGui::TextDisabled("スクラブ中: パーティクルは前方向にのみ再現されます");

    ImGui::Separator();

    // ── トラック ──
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 laneOrigin = ImGui::GetCursorScreenPos();
    const float laneLeft = laneOrigin.x + kLabelWidth;
    const float laneWidth = (std::max)(ImGui::GetContentRegionAvail().x - kLabelWidth - 8.0f, 40.0f);
    const auto timeToX = [&](float t) { return laneLeft + (t / span) * laneWidth; };

    for (std::size_t index = 0; index < m_tracks.size(); ++index) {
        const Track& track = m_tracks[index];
        ImGui::PushID(static_cast<int>(index));

        const ImVec2 rowTop = ImGui::GetCursorScreenPos();
        const float indent = static_cast<float>(track.depth) * kIndentPx;

        ImGui::SetCursorScreenPos(ImVec2(rowTop.x + indent, rowTop.y));
        if (ImGui::Selectable(track.label.c_str(), false, ImGuiSelectableFlags_None,
                              ImVec2(kLabelWidth - indent, kRowHeight)))
            SelectEntity(ctx, track.entity);

        if (track.isGroup) {
            ImGui::SetCursorScreenPos(ImVec2(rowTop.x, rowTop.y + kRowHeight));
            ImGui::PopID();
            continue;
        }

        // duration <= 0 は「ルートが終わるまで」。描くときだけ実尺へ広げる。
        const float drawDuration = track.duration > 0.0f
            ? track.duration : (std::max)(span - track.start, kMinDuration);
        const float x0 = timeToX(track.start);
        const float x1 = timeToX(track.start + drawDuration);
        const ImVec2 barMin(x0, rowTop.y + 3.0f);
        const ImVec2 barMax((std::max)(x1, x0 + 3.0f), rowTop.y + kRowHeight - 3.0f);

        draw->AddRectFilled(barMin, barMax, BarColor(!track.trigger.empty(), track.loop), 3.0f);
        if (track.duration <= 0.0f)
            draw->AddRect(barMin, barMax, IM_COL32(240, 240, 240, 140), 3.0f);

        // 当たり判定はレーン全幅に置く。帯そのものを幅 0 のボタンにすると、
        // 短い窓 (0.05 秒) が掴めなくなる。
        ImGui::SetCursorScreenPos(ImVec2(laneLeft, rowTop.y));
        ImGui::InvisibleButton("##lane", ImVec2(laneWidth, kRowHeight));

        const float mouseX = ImGui::GetIO().MousePos.x;
        const bool overBar = mouseX >= barMin.x - kEdgeGrabPx && mouseX <= barMax.x + kEdgeGrabPx;

        if (ImGui::IsItemHovered() && overBar) {
            ImGui::SetTooltip("%s\nstart %s / duration %s%s%s",
                              track.label.c_str(),
                              FormatSeconds(track.start).c_str(),
                              track.duration > 0.0f
                                  ? FormatSeconds(track.duration).c_str() : "(root まで)",
                              track.trigger.empty() ? "" : "\ntrigger: ",
                              track.trigger.c_str());
        }

        if (ImGui::IsItemActivated()) {
            if (overBar) {
                m_dragEntity = track.entity;
                m_dragStart = track.start;
                m_dragDuration = drawDuration;
                if (mouseX - barMin.x <= kEdgeGrabPx)      m_dragEdge = -1;
                else if (barMax.x - mouseX <= kEdgeGrabPx) m_dragEdge = 1;
                else                                        m_dragEdge = 0;
            } else {
                m_dragEntity = scene::EntityID{};
            }
        }

        if (ImGui::IsItemActive() && m_dragEntity == track.entity) {
            // ドラッグ量は «掴んだ時点の値 + 総移動量» で出す。
            // WHY: 毎フレームの差分を足し込むと、クランプのたびに誤差が残って
            //      «掴んだ場所と帯がずれていく»。
            const float deltaTime = (ImGui::GetMouseDragDelta().x / laneWidth) * span;
            if (m_dragEdge == 0) {
                m_dragResultStart = (std::max)(m_dragStart + deltaTime, 0.0f);
                m_dragResultDuration = m_dragDuration;
            } else if (m_dragEdge < 0) {
                m_dragResultStart = std::clamp(m_dragStart + deltaTime, 0.0f,
                                               m_dragStart + m_dragDuration - kMinDuration);
                m_dragResultDuration = m_dragDuration - (m_dragResultStart - m_dragStart);
            } else {
                m_dragResultStart = m_dragStart;
                m_dragResultDuration = (std::max)(m_dragDuration + deltaTime, kMinDuration);
            }
            ApplyTrack(scene, track, m_dragResultStart, m_dragResultDuration);
            // markSceneDirty は Prefab 編集モードを見て prefabEditDirty 側へ振り分ける。
            // sceneDirty を直に立てると、編集モードを抜けたあと «触っていないシーン» に
            // 未保存扱いが残る。
            if (ctx.markSceneDirty) ctx.markSceneDirty();
        }

        // Undo は «掴んで離すまで» を 1 手にする。ドラッグ中に積むと 1 回の調整で
        // 履歴が数十件になり、戻す操作が使い物にならなくなる。
        if (ImGui::IsItemDeactivated() && m_dragEntity == track.entity) {
            const Track captured = track;
            const float finalStart = m_dragResultStart;
            const float finalDuration = m_dragResultDuration;
            // スナップショットは «編集前» を撮る必要があるので、いったん元値へ戻す。
            ApplyTrack(scene, captured, m_dragStart, m_dragDuration);
            ExecuteSceneEditWithUndo(ctx, "Adjust VFX Track", [&] {
                ApplyTrack(scene, captured, finalStart, finalDuration);
            });
            m_dragEntity = scene::EntityID{};
        }

        ImGui::SetCursorScreenPos(ImVec2(rowTop.x, rowTop.y + kRowHeight));
        ImGui::PopID();
    }

    // 再生ヘッドは全トラックの上に引く。
    const float laneBottom = ImGui::GetCursorScreenPos().y;
    if (laneBottom > laneOrigin.y) {
        const float headX = timeToX(std::clamp(vfx->time, 0.0f, span));
        draw->AddLine(ImVec2(headX, laneOrigin.y), ImVec2(headX, laneBottom),
                      IM_COL32(255, 96, 96, 200), 1.5f);
    }
}

} // namespace fbzz::editor
