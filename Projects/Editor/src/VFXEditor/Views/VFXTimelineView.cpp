// FBZZ Engine
// VFXTimelineView.cpp | fbzz::editor
// トランスポートとタイムライン View の実装
#include <Editor/VFXEditor/Views/VFXTimelineView.hpp>

#include <Editor/VFXEditor/Application/VFXEditorSession.hpp>
#include <Editor/VFXEditor/Views/VFXEditorUiCommon.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Panels/StatusBar.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Editor/Util/ParticleEditWidgets.hpp>
#include <Editor/Util/ParticleEmitterModules.hpp>
#include <Editor/Util/SchemaInspector.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/ProceduralVFXTextures.hpp>
#include <Engine/Asset/VFXAuthoringSchema.hpp>
#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/ParticleGpuSimulation.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/VFXGraphComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <imnodes.h>
#include <toml++/toml.hpp>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace fbzz::editor {

// 共有ヘルパー (ノード色・アイコン・パラメーター編集など) は fbzz::editor::vfx に
// 隔離してある。Editor 全体の名前空間を汚さないための境界。
using namespace vfx;

void VFXTimelineView::DrawTransport(EditorContext& ctx,
                                   const std::vector<scene::EntityID>& group,
                                   scene::ParticleEmitter* root)
{
    const bool hasEffect = root != nullptr;
    ImGui::BeginDisabled(!hasEffect);

    const bool isPlaying = hasEffect && root->settings.playing && !m_session.preview.paused;
    if (ImGui::Button(isPlaying ? "Pause" : "Play", { 64.0f, 0.0f })) {
        if (isPlaying) {
            m_session.preview.paused = true;
        } else {
            m_session.preview.paused = false;
            if (ctx.activeScene)
                for (const scene::EntityID id : group)
                    if (auto* emitter = ctx.activeScene->GetComponent<scene::ParticleEmitter>(id))
                        emitter->settings.playing = true;
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Space");

    ImGui::SameLine();
    if (ImGui::Button("Restart") && ctx.activeScene) {
        for (const scene::EntityID id : group)
            if (auto* emitter = ctx.activeScene->GetComponent<scene::ParticleEmitter>(id))
                emitter->ResetPlayback();
        m_session.preview.paused = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop") && ctx.activeScene) {
        // 放出だけ止め、既存粒子は寿命で消える (Unity の Stop 相当)
        for (const scene::EntityID id : group)
            if (auto* emitter = ctx.activeScene->GetComponent<scene::ParticleEmitter>(id))
                emitter->settings.playing = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Step") && hasEffect) {
        // 一時停止のまま 1/60s だけ進める。決定論スクラブなので何度押しても再現する。
        m_session.preview.RequestScrub(ctx, group, root->runtime.playTime + kScrubStep);
        m_session.preview.paused = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Advance one frame (1/60s) while paused");
    ImGui::SameLine();
    if (ImGui::Button("Burst") && hasEffect)
        root->runtime.burstPending += 10;

    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::DragFloat("##vfx_speed", &m_session.preview.speed, 0.01f, 0.05f, 4.0f, "Speed %.2fx");
    ImGui::SameLine();
    if (ImGui::SmallButton("0.25x")) m_session.preview.speed = 0.25f;
    ImGui::SameLine();
    if (ImGui::SmallButton("1x")) m_session.preview.speed = 1.0f;
    ImGui::SameLine();
    if (ImGui::SmallButton("2x")) m_session.preview.speed = 2.0f;

    if (hasEffect) {
        ImGui::SameLine();
        ImGui::Text("  %.2fs / %.2fs", root->runtime.playTime, root->settings.duration);
    }
    ImGui::EndDisabled();

    // 右端: プレビューの場所を明示する (旧パネルの偽 2D プレビュー廃止に伴う導線)
    ImGui::SameLine((std::max)(ImGui::GetCursorPosX() + 12.0f,
                               ImGui::GetContentRegionMax().x - 190.0f));
    ImGui::TextDisabled("World: VFX Preview (isolated)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Objects created here never enter the editing Scene");
}


void VFXTimelineView::DrawGraphTimeline(EditorContext& ctx)
{
    if (!m_session.showTimeline || m_session.document.graph.nodes.empty()) return;

    // 帯の位置と長さはランタイムと同じ BuildVFXGraphSchedule から得る。
    // ここで別実装を持つと、タイムラインで詰めた「間」が実行時とずれる。
    std::vector<float> startTimes;
    float duration = 0.0f;
    if (!asset::BuildVFXGraphSchedule(m_session.document.graph, startTimes, duration, nullptr)) {
        ImGui::TextDisabled("Timeline: グラフを解決できません (循環リンクなど)");
        return;
    }
    // 全ノードが 0 秒だと除算できない。最低限の見える幅を確保する。
    bool hasActorTrack = false;
    float actorDuration = 0.0f;
    if (ctx.vfxPreviewScene != nullptr) {
        if (auto* actor = ctx.vfxPreviewScene->GetGameObject(m_session.preview.actorEntity)) {
            if (auto* animator = actor->GetComponent<scene::AnimatorComponent>()) {
                hasActorTrack = true;
                for (const auto& clip : animator->clips)
                    actorDuration = (std::max)(actorDuration,
                        static_cast<float>(clip.GetDurationSeconds()));
            }
        }
    }
    const float span = (std::max)({ duration, actorDuration, 0.5f });

    constexpr float kRowHeight = 18.0f;
    constexpr float kLabelWidth = 132.0f;
    constexpr float kRulerHeight = 18.0f;
    constexpr float kBurstRadius = 4.0f;

    const float availableWidth = (std::max)(ImGui::GetContentRegionAvail().x, 240.0f);
    const float trackWidth = (std::max)(availableWidth - kLabelWidth, 120.0f);
    const float contentHeight = kRulerHeight
        + kRowHeight * static_cast<float>(m_session.document.graph.nodes.size()
                                         + (hasActorTrack ? 1 : 0));

    ImGui::BeginChild("##VFXGraphTimeline", { 0.0f, 0.0f }, true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::InvisibleButton("##VFXTimelineCanvas", { availableWidth, contentHeight });
    const ImVec2 origin = ImGui::GetItemRectMin();
    const bool canvasHovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 mouse = ImGui::GetMousePos();

    const float trackLeft = origin.x + kLabelWidth;
    const auto timeToX = [&](float t) {
        return trackLeft + std::clamp(t / span, 0.0f, 1.0f) * trackWidth;
    };
    const auto xToTime = [&](float x) {
        return std::clamp((x - trackLeft) / trackWidth, 0.0f, 1.0f) * span;
    };
    // Shift でスナップの有無を一時反転する (既定 ON なら Shift 中だけ自由移動)。
    const bool snapping = m_session.timelineSnap != ImGui::GetIO().KeyShift;
    const auto applySnap = [&](float value) {
        if (!snapping || m_session.timelineSnapStep <= 0.0f) return value;
        return std::round(value / m_session.timelineSnapStep) * m_session.timelineSnapStep;
    };

    // 目盛り
    draw->AddRectFilled(origin, { origin.x + availableWidth, origin.y + contentHeight },
                        IM_COL32(20, 23, 30, 255));
    for (int i = 0; i <= 10; ++i) {
        const float ratio = static_cast<float>(i) / 10.0f;
        const float x = trackLeft + trackWidth * ratio;
        const bool major = i % 5 == 0;
        draw->AddLine({ x, origin.y }, { x, origin.y + contentHeight },
                      IM_COL32(60, 66, 82, major ? 160 : 70));
        if (major) {
            char tick[16];
            std::snprintf(tick, sizeof(tick), "%.2fs", span * ratio);
            draw->AddText({ x + 3.0f, origin.y + 2.0f }, IM_COL32(150, 158, 175, 255), tick);
        }
    }

    int hoveredNodeId = -1;
    int hoveredKind = -1;
    int hoveredBurst = -1;

    if (hasActorTrack) {
        const float rowTop = origin.y + kRulerHeight;
        const float barTop = rowTop + 3.0f;
        const float barBottom = rowTop + kRowHeight - 3.0f;
        const std::string label = m_session.preview.actorState.empty()
            ? "Animation Track" : "Animation: " + m_session.preview.actorState;
        draw->AddText({ origin.x + 4.0f, rowTop + 2.0f },
                      IM_COL32(170, 205, 240, 255), label.c_str());
        const float barRight = (std::max)(timeToX((std::max)(actorDuration, duration)),
                                          trackLeft + 4.0f);
        draw->AddRectFilled({ trackLeft, barTop }, { barRight, barBottom },
                            IM_COL32(70, 112, 158, 255), 3.0f);
    }

    for (std::size_t index = 0; index < m_session.document.graph.nodes.size(); ++index) {
        asset::VFXGraphNode& node = m_session.document.graph.nodes[index];
        const float rowTop = origin.y + kRulerHeight
            + kRowHeight * static_cast<float>(index + (hasActorTrack ? 1 : 0));
        const float barTop = rowTop + 3.0f;
        const float barBottom = rowTop + kRowHeight - 3.0f;

        // 行ラベル (ノード種別 + 名前)。選択中は明るくする。
        const bool selected = node.id == m_session.selectedNodeId;
        draw->AddText({ origin.x + 4.0f, rowTop + 2.0f },
                      selected ? IM_COL32(255, 235, 170, 255) : IM_COL32(170, 178, 195, 255),
                      node.name.c_str());

        // Entry と Reroute は時間を持たないので帯を描かない (0 幅の帯は掴めず紛らわしい)。
        if (node.type == asset::VFXNodeType::Entry
            || asset::VFXNodeIsPassthrough(node.type)) continue;

        const float start = startTimes[index];
        const float end = start + (std::max)(node.duration, 0.0f);
        const float barLeft = timeToX(start);
        const float barRight = (std::max)(timeToX(end), barLeft + 4.0f);

        // 帯の色は Canvas のノード色と同じ関数から取る。
        // 別表を持つと「Canvas では青いのにタイムラインでは緑」というズレが起きる。
        const ImU32 baseColor = node.enabled
            ? VFXNodeColor(node.type) : IM_COL32(90, 94, 104, 255);
        draw->AddRectFilled({ barLeft, barTop }, { barRight, barBottom }, baseColor, 3.0f);
        draw->AddRect({ barLeft, barTop }, { barRight, barBottom },
                      selected ? IM_COL32(255, 220, 130, 255) : IM_COL32(20, 22, 28, 200),
                      3.0f, 0, selected ? 2.0f : 1.0f);

        const bool rowHot = canvasHovered && mouse.y >= barTop && mouse.y <= barBottom;
        if (rowHot && mouse.x >= barLeft - 4.0f && mouse.x <= barRight + 4.0f) {
            hoveredNodeId = node.id;
            // 右端 6px は duration ハンドル。掴み分けの境界はカーソルでも示す。
            hoveredKind = (mouse.x >= barRight - 6.0f) ? 1 : 0;
            ImGui::SetMouseCursor(hoveredKind == 1 ? ImGuiMouseCursor_ResizeEW
                                                   : ImGuiMouseCursor_ResizeAll);
        }

        // Particle ノードの Burst を帯の上へ三角で重ねる。
        if (node.type == asset::VFXNodeType::Particle) {
            for (std::size_t b = 0; b < node.particle.bursts.size(); ++b) {
                const float burstX = timeToX(start + node.particle.bursts[b].time);
                const float centerY = (barTop + barBottom) * 0.5f;
                const bool hot = rowHot && std::fabs(mouse.x - burstX) <= kBurstRadius + 2.0f;
                if (hot) {
                    hoveredNodeId = node.id;
                    hoveredKind = 2;
                    hoveredBurst = static_cast<int>(b);
                }
                draw->AddTriangleFilled(
                    { burstX, centerY - kBurstRadius },
                    { burstX + kBurstRadius, centerY + kBurstRadius },
                    { burstX - kBurstRadius, centerY + kBurstRadius },
                    hot ? IM_COL32(255, 235, 150, 255) : IM_COL32(240, 175, 70, 255));
            }
        }
    }

    // ── ドラッグ開始 ──
    if (canvasHovered && hoveredNodeId >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        // 1 操作分をまとめて戻せるよう、掴んだ瞬間に一度だけスナップショットを取る。
        m_session.PushUndo();
        m_timelineDragNodeId = hoveredNodeId;
        m_timelineDragKind = hoveredKind;
        m_timelineDragBurstIndex = hoveredBurst;
        m_timelineDragGrabTime = xToTime(mouse.x);
        m_session.selectedNodeId = hoveredNodeId;   // Canvas 側の選択と同期する
        m_session.selectedLinkIndex = -1;
        if (auto* node = FindGraphNode(m_session.document.graph, hoveredNodeId)) {
            m_timelineDragOriginValue =
                hoveredKind == 0 ? node->startOffset
                : hoveredKind == 1 ? node->duration
                : (hoveredBurst >= 0 && hoveredBurst < static_cast<int>(node->particle.bursts.size())
                       ? node->particle.bursts[static_cast<std::size_t>(hoveredBurst)].time
                       : 0.0f);
        }
    }

    // ── ドラッグ中 ──
    if (m_timelineDragNodeId >= 0) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            m_timelineDragNodeId = -1;
            m_timelineDragKind = -1;
            m_timelineDragBurstIndex = -1;
        } else if (auto* node = FindGraphNode(m_session.document.graph, m_timelineDragNodeId)) {
            // 掴んだ点からの差分で動かす。絶対位置で入れるとクリック位置に飛ぶ。
            const float delta = xToTime(mouse.x) - m_timelineDragGrabTime;
            bool edited = false;
            if (m_timelineDragKind == 0) {
                const float next = (std::max)(applySnap(m_timelineDragOriginValue + delta), 0.0f);
                if (next != node->startOffset) { node->startOffset = next; edited = true; }
                ImGui::SetTooltip("Start %.2fs", node->startOffset);
            } else if (m_timelineDragKind == 1) {
                // 0 幅にすると帯が掴めなくなるため下限を残す。
                const float next = (std::max)(applySnap(m_timelineDragOriginValue + delta), 0.01f);
                if (next != node->duration) {
                    node->duration = next;
                    // Particle は duration が実体側の再生時間そのもの。Inspector と同じく同期する。
                    if (node->type == asset::VFXNodeType::Particle)
                        node->particle.duration = next;
                    edited = true;
                }
                ImGui::SetTooltip("Duration %.2fs", node->duration);
            } else if (m_timelineDragKind == 2
                       && m_timelineDragBurstIndex >= 0
                       && m_timelineDragBurstIndex < static_cast<int>(node->particle.bursts.size())) {
                auto& burst = node->particle.bursts[static_cast<std::size_t>(m_timelineDragBurstIndex)];
                const float next = (std::max)(applySnap(m_timelineDragOriginValue + delta), 0.0f);
                if (next != burst.time) { burst.time = next; edited = true; }
                ImGui::SetTooltip("Burst %.2fs", burst.time);
            }
            if (edited) {
                m_session.document.dirty = true;
                m_session.document.liveDirtyNodeId = node->id;
            }
        }
    }

    // ── 再生ヘッド (目盛り帯のクリック / ドラッグでスクラブ) ──
    if (canvasHovered && hoveredNodeId < 0 && m_timelineDragNodeId < 0
        && ImGui::IsMouseDown(ImGuiMouseButton_Left) && mouse.y <= origin.y + kRulerHeight) {
        const float target = xToTime(mouse.x);
        // グラフ全体のスクラブは既存の決定論的再シミュレーション経路へ委ねる。
        m_session.preview.RequestScrub(ctx, m_session.preview.BuildEffectGroup(ctx), target);
    }

    ImGui::EndChild();

    ImGui::TextDisabled("Timeline: drag bar = start / right edge = duration / triangle = burst"
                        "   Shift = %s snap", m_session.timelineSnap ? "disable" : "enable");
    ImGui::SameLine();
    ImGui::Checkbox("Snap##VFXTimeline", &m_session.timelineSnap);
}


void VFXTimelineView::DrawEmitterTimeline(EditorContext& ctx,
                                  const std::vector<scene::EntityID>& group,
                                  scene::ParticleEmitter* root)
{
    // 横軸スパン: duration。0 (無限再生) のときは lifetime を目安に表示だけ行う
    const bool  continuous = root->settings.duration <= 0.0f;
    const float span = continuous ? (std::max)(root->settings.lifetime, 1.0f) : root->settings.duration;

    const float  width = (std::max)(ImGui::GetContentRegionAvail().x, 160.0f);
    ImGui::InvisibleButton("##vfx_timeline", ImVec2(width, kTimelineH));
    const ImVec2 rectMin = ImGui::GetItemRectMin();
    const ImVec2 rectMax = ImGui::GetItemRectMax();
    const bool   hovered = ImGui::IsItemHovered();
    ImDrawList*  draw  = ImGui::GetWindowDrawList();
    const ImVec2 mouse = ImGui::GetMousePos();

    const float rulerBottom  = rectMin.y + 22.0f;         // 上段: 目盛り + 再生ヘッド操作ゾーン
    const float markerCenter = rectMax.y - 12.0f;         // 下段: Burst マーカーゾーン

    auto timeToX = [&](float t) {
        return rectMin.x + std::clamp(t / span, 0.0f, 1.0f) * width;
    };
    auto xToTime = [&](float x) {
        return std::clamp((x - rectMin.x) / width, 0.0f, 1.0f) * span;
    };

    // 背景と目盛り
    draw->AddRectFilled(rectMin, rectMax, IM_COL32(22, 25, 32, 255), 3.0f);
    draw->AddLine({ rectMin.x, rulerBottom }, { rectMax.x, rulerBottom }, IM_COL32(60, 66, 80, 255));
    for (int i = 0; i <= 10; ++i) {
        const float x = rectMin.x + width * (static_cast<float>(i) / 10.0f);
        const bool major = i % 5 == 0;
        draw->AddLine({ x, rectMin.y }, { x, rectMin.y + (major ? 12.0f : 6.0f) },
                      IM_COL32(120, 128, 145, major ? 200 : 110));
        if (major) {
            char tick[16];
            std::snprintf(tick, sizeof(tick), "%.1fs", span * (static_cast<float>(i) / 10.0f));
            draw->AddText({ x + 3.0f, rectMin.y + 8.0f }, IM_COL32(150, 158, 175, 255), tick);
        }
    }
    draw->AddRect(rectMin, rectMax, IM_COL32(70, 76, 90, 255), 3.0f);

    // ループ / 遅延の注記
    if (root->settings.loop)
        draw->AddText({ rectMax.x - 44.0f, rectMin.y + 2.0f }, IM_COL32(120, 200, 160, 255), "loop");
    if (root->settings.startDelay > 0.0f) {
        char delayText[32];
        std::snprintf(delayText, sizeof(delayText), "delay %.2fs", root->settings.startDelay);
        draw->AddText({ rectMin.x + 4.0f, rectMin.y + 2.0f }, IM_COL32(230, 180, 90, 255), delayText);
    }

    ImGuiStorage* storage    = ImGui::GetStateStorage();
    const ImGuiID scrubId    = ImGui::GetID("##vfx_scrubbing");
    const ImGuiID burstDragId = ImGui::GetID("##vfx_burst_drag");
    const ImGuiID burstEditId = ImGui::GetID("##vfx_burst_edit");

    // ── Burst マーカー (下段の菱形。ドラッグで時刻変更 / 右クリックで詳細編集) ──
    int hoveredBurst = -1;
    for (size_t i = 0; i < root->settings.bursts.size(); ++i) {
        const float x = timeToX(root->settings.bursts[i].time);
        const ImVec2 c(x, markerCenter);
        const bool hot = hovered
            && std::fabs(mouse.x - c.x) <= kBurstMarkerR + 3.0f
            && std::fabs(mouse.y - c.y) <= kBurstMarkerR + 3.0f;
        if (hot) hoveredBurst = static_cast<int>(i);
        const ImU32 fill = hot ? IM_COL32(255, 220, 120, 255) : IM_COL32(235, 160, 70, 255);
        draw->AddQuadFilled({ c.x, c.y - kBurstMarkerR }, { c.x + kBurstMarkerR, c.y },
                            { c.x, c.y + kBurstMarkerR }, { c.x - kBurstMarkerR, c.y }, fill);
        draw->AddQuad({ c.x, c.y - kBurstMarkerR }, { c.x + kBurstMarkerR, c.y },
                      { c.x, c.y + kBurstMarkerR }, { c.x - kBurstMarkerR, c.y },
                      IM_COL32(30, 34, 44, 255), 1.0f);
        if (hot)
            ImGui::SetTooltip("Burst: %d particles @ %.2fs (x%d)\nDrag: move / Right-click: edit",
                              root->settings.bursts[i].count, root->settings.bursts[i].time,
                              (std::max)(root->settings.bursts[i].cycles, 1));
    }

    // マーカーのドラッグ
    if (hovered && hoveredBurst >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        storage->SetInt(burstDragId, hoveredBurst);
    const int burstDrag = storage->GetInt(burstDragId, -1);
    if (burstDrag >= 0 && burstDrag < static_cast<int>(root->settings.bursts.size())
        && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        root->settings.bursts[static_cast<size_t>(burstDrag)].time = xToTime(mouse.x);
        ImGui::SetTooltip("%.2fs", root->settings.bursts[static_cast<size_t>(burstDrag)].time);
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && burstDrag >= 0) {
        storage->SetInt(burstDragId, -1);
        if (ctx.markSceneDirty) ctx.markSceneDirty();
    }

    // マーカー右クリック → 詳細編集ポップアップ
    if (hovered && hoveredBurst >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        storage->SetInt(burstEditId, hoveredBurst);
        ImGui::OpenPopup("##vfx_burst_popup");
    }
    if (ImGui::BeginPopup("##vfx_burst_popup")) {
        const int editIndex = storage->GetInt(burstEditId, -1);
        if (editIndex >= 0 && editIndex < static_cast<int>(root->settings.bursts.size())) {
            auto& burst = root->settings.bursts[static_cast<size_t>(editIndex)];
            bool burstChanged = false;
            ImGui::TextDisabled("Burst %d", editIndex + 1);
            burstChanged |= ImGui::DragFloat("Time", &burst.time, 0.01f, 0.0f, span);
            burstChanged |= ImGui::DragInt("Count", &burst.count, 1, 0, 100000);
            burstChanged |= ImGui::DragInt("Cycles", &burst.cycles, 1, 1, 1000);
            burstChanged |= ImGui::DragFloat("Interval", &burst.interval, 0.01f, 0.0f, 300.0f);
            burstChanged |= ImGui::DragFloat("Probability", &burst.probability, 0.01f, 0.0f, 1.0f);
            ImGui::Separator();
            if (ImGui::MenuItem("Delete Burst")) {
                root->settings.bursts.erase(root->settings.bursts.begin() + editIndex);
                burstChanged = true;
                ImGui::CloseCurrentPopup();
            }
            if (burstChanged && ctx.markSceneDirty)
                ctx.markSceneDirty();
        } else {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // 下段の空きをダブルクリック → その時刻に Burst 追加
    if (hovered && hoveredBurst < 0 && mouse.y > rulerBottom
        && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        scene::ParticleBurst burst;
        burst.time = xToTime(mouse.x);
        root->settings.bursts.push_back(burst);
        if (ctx.markSceneDirty) ctx.markSceneDirty();
    }

    // ── 再生ヘッド (上段クリック / ドラッグで決定論スクラブ) ──
    const bool scrubbing = storage->GetBool(scrubId, false);
    if (hovered && hoveredBurst < 0 && mouse.y <= rulerBottom
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        storage->SetBool(scrubId, true);
    if (storage->GetBool(scrubId, false)) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const float t = xToTime(mouse.x);
            if (!continuous) {
                m_session.preview.RequestScrub(ctx, group, t);
                m_session.preview.paused = true; // Unity 同様、スクラブ中は一時停止して時刻を固定する
            }
            ImGui::SetTooltip("%.2fs", t);
        } else {
            storage->SetBool(scrubId, false);
        }
    }
    if (continuous && hovered && mouse.y <= rulerBottom)
        ImGui::SetTooltip("Duration is 0 (continuous) - set a Duration to enable scrubbing");

    // 再生ヘッド描画は最後 (マーカーの上に重ねる)
    {
        const float x = timeToX(root->runtime.playTime);
        draw->AddLine({ x, rectMin.y }, { x, rectMax.y }, IM_COL32(120, 200, 255, 255), 2.0f);
        draw->AddTriangleFilled({ x - 5.0f, rectMin.y }, { x + 5.0f, rectMin.y },
                                { x, rectMin.y + 7.0f }, IM_COL32(120, 200, 255, 255));
    }
    (void)scrubbing;
}


void VFXTimelineView::DrawStats(EditorContext& ctx,
                               const std::vector<scene::EntityID>& group,
                               scene::ParticleEmitter* root)
{
    // グループ合計とルート詳細を1行で。パフォーマンス確認をパネル内で完結させる。
    int totalParticles = 0;
    int totalVisible = 0;
    int gpuFallbackCount = 0;
    if (ctx.activeScene) {
        for (const scene::EntityID id : group) {
            if (const auto* emitter = ctx.activeScene->GetComponent<scene::ParticleEmitter>(id)) {
                const bool onGpu =
                    scene::CanUseGpuSimulation(emitter->settings, &emitter->runtime.material);
                totalParticles += onGpu
                    ? emitter->runtime.visibleParticleCount
                    : static_cast<int>(emitter->runtime.particles.size());
                totalVisible += emitter->runtime.visibleParticleCount;
                if (!onGpu && emitter->settings.simulationMode == scene::ParticleSimulationMode::Gpu)
                    ++gpuFallbackCount;
            }
        }
    }
    // 表示するのは「要求」ではなく「実際に走っている経路」。
    // WHY: simulationMode = Gpu にしても条件を 1 つ外すと黙って CPU へ落ちる。
    //      要求値を出していると、担当者は縮退したまま粒子数だけ増やし続けることになる。
    const auto rootFallback =
        scene::GetParticleGpuFallbackReason(root->settings, &root->runtime.material);
    const bool rootOnGpu = rootFallback == scene::ParticleGpuFallbackReason::None;
    ImGui::TextDisabled("Emitters: %d   Particles: %d (visible %d)   Root: %s / %s%s",
                        static_cast<int>(group.size()),
                        totalParticles, totalVisible,
                        rootOnGpu ? "GPU" : "CPU",
                        root->settings.simulationSpace == scene::ParticleSimulationSpace::Local ? "Local" : "World",
                        root->runtime.isCulledThisFrame ? "   [CULLED]" : "");

    // GPU を要求したのに縮退しているなら、原因の設定名まで出す。
    if (root->settings.simulationMode == scene::ParticleSimulationMode::Gpu && !rootOnGpu) {
        ImGui::SameLine();
        ImGui::TextColored({ 1.0f, 0.65f, 0.3f, 1.0f }, "   [GPU→CPU: %s]",
                           scene::ParticleGpuFallbackFieldName(rootFallback));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", scene::ParticleGpuFallbackDescription(rootFallback));
    } else if (gpuFallbackCount > 0) {
        ImGui::SameLine();
        ImGui::TextColored({ 1.0f, 0.65f, 0.3f, 1.0f }, "   [GPU→CPU x%d]", gpuFallbackCount);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("このグループ内の %d 個のエミッターが GPU 指定のまま CPU へ縮退しています。\n"
                              "各ノードを選択すると原因の設定が判ります。", gpuFallbackCount);
    }

    // Particle パスの実測 GPU 時間。
    // WHY: budget は粒子数でしか測れないが、実際のボトルネックは fill rate であることが多い。
    //      「粒子は budget 内なのに重い」を検知できるよう、実時間を常時見せる。
    //      計測は RenderSystem が全パスへ自動で仕込んでおり、ここは結果を読むだけ。
    if (ctx.renderer != nullptr) {
        for (const auto& profile : ctx.renderer->GpuProfGetResults()) {
            if (profile.name != "Particle") continue;
            // 60fps の 1 フレームは 16.6ms。1ms を超えたらエフェクト単体としては重い部類。
            const bool heavy = profile.gpuMs >= 1.0;
            ImGui::SameLine();
            ImGui::TextColored(heavy ? ImVec4{ 1.0f, 0.65f, 0.3f, 1.0f }
                                     : ImVec4{ 0.45f, 0.85f, 0.55f, 1.0f },
                               "   GPU %.3f ms", static_cast<float>(profile.gpuMs));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Particle パスの GPU 実行時間 (数フレーム前の確定値)。\n"
                                  "粒子数が budget 内でも、重なりが多いとここが伸びます。");
            break;
        }
    }
}

} // namespace fbzz::editor
