// FBZZ Engine
// VFXTimelineView.hpp | fbzz::editor
// トランスポートと 2 種のタイムライン (グラフのマルチトラック / 単体 Emitter)
// WHY: 爆発の「閃光 → 衝撃波 → 煙」の間合いは、数値を眺めても詰められない。
//      実行スケジュールをそのまま帯として描き、掴んで動かせることが編集の本体なので、
//      ドラッグ中の状態を持つ独立した View として切り出す。
#pragma once

#include <Engine/Scene/Entity.hpp>
#include <vector>

namespace fbzz::scene { struct ParticleEmitter; }

namespace fbzz::editor {

struct EditorContext;
class VFXEditorSession;

class VFXTimelineView {
public:
    explicit VFXTimelineView(VFXEditorSession& session) : m_session(session) {}

    // 再生 / 一時停止 / Restart / Step / 速度。エフェクト集合全体へ適用する。
    void DrawTransport(EditorContext& ctx, const std::vector<scene::EntityID>& group,
                       scene::ParticleEmitter* root);

    // グラフモード用のマルチトラック タイムライン。
    // 行 = ノードで、帯の位置と長さが実行スケジュール (BuildVFXGraphSchedule) そのもの。
    void DrawGraphTimeline(EditorContext& ctx);

    // 単体 Emitter モードのタイムライン (Burst マーカー付きスクラブバー)。
    void DrawEmitterTimeline(EditorContext& ctx, const std::vector<scene::EntityID>& group,
                             scene::ParticleEmitter* root);

    void DrawStats(EditorContext& ctx, const std::vector<scene::EntityID>& group,
                   scene::ParticleEmitter* root);

private:
    VFXEditorSession& m_session;

    // ドラッグ中の対象。-1 = ドラッグなし。
    // kind は 0=帯本体(startOffset) / 1=右端(duration) / 2=Burst マーカー。
    int m_timelineDragNodeId = -1;
    int m_timelineDragKind = -1;
    int m_timelineDragBurstIndex = -1;
    // ドラッグ開始時のマウス時刻と対象値。差分で動かし、掴んだ点がずれないようにする。
    float m_timelineDragGrabTime = 0.0f;
    float m_timelineDragOriginValue = 0.0f;
};

} // namespace fbzz::editor
