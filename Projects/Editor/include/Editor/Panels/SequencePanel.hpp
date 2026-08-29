/// @file    SequencePanel.hpp
/// @brief   .sequence の尺を目で詰めるタイムラインパネル
/// @author  Hasegawa Jin
/// @date    2026-08-26
//
// WHY 専用パネルを持つか:
//   演出の良し悪しは数値ではなく尺で決まる。「0.2 秒ずらす」を TOML の手書きと
//   ゲームの頭からの再生で繰り返す形だと、詰める作業そのものが成立しない。
//   帯とキーを掴んで動かし、その場でスクラブして絵を確認できることが編集の本体。
//
// WHY MVP 分割しないか (VFXEditor と違う点):
//   こちらはトラック 7 種とキー編集しかなく、グラフ・ノード・配線が無い。
//   分ける対象が無いところで層を切ると、行き来のコストだけが増える。
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Engine/Asset/SequenceAsset.hpp>
#include <Engine/Scene/Entity.hpp>

#include <string>
#include <vector>

namespace fbzz::scene { struct SequencePlayerComponent; class GameObject; }

namespace fbzz::editor {

class SequencePanel final : public IPanel {
public:
    const char* GetWindowName()        const override { return "Sequence"; }
    bool        GetDefaultVisibility() const override { return false; }
    void        OnShutdown() override;

    /// AssetBrowser のダブルクリックから開く。
    void RequestOpen(const std::string& path) { m_requestedPath = path; }

protected:
    void OnRenderContent(EditorContext& ctx) override;
    void OnAfterEnd(EditorContext& ctx) override;

private:
    /// 選択の単位。トラック内のどの列 (channel) の何番目か。
    ///
    /// WHY 平坦な添字 1 本にしないか: TransformTrack は position / rotation / scale の
    ///     3 列を 1 行に重ねて描く。1 本の番号にすると、列をまたいだ削除や
    ///     並べ替えのたびに番号の意味が変わり、選択が別のキーへ滑る。
    struct Selection {
        int track   = -1;
        int channel = 0;
        int index   = -1;
        [[nodiscard]] bool HasTrack() const { return track >= 0; }
        [[nodiscard]] bool HasItem()  const { return track >= 0 && index >= 0; }
    };

    bool Load(const std::string& path);
    bool Save();
    void CloseDocument(EditorContext& ctx);

    void PushUndo();
    void Undo();
    void Redo();
    /// 直前のウィジェットが「掴まれた」フレームだけ Undo を積む。
    ///
    /// WHY 変更のたびに積まないか: DragFloat は掴んでいる間ずっと変更を返す。
    ///     そのたびに積むと、1 回のドラッグで Undo が数十個生まれ、
    ///     Ctrl+Z を押し続けても元の値まで戻れなくなる。
    void SnapshotOnActivate() { if (ImGui::IsItemActivated()) PushUndo(); }
    /// 編集を確定する。未保存フラグと、プレビューへ流す版数を同時に進める。
    void MarkEdited() { m_dirty = true; ++m_localRevision; }

    // ── プレビュー ──
    /// スクラブ先の Player を決める。明示指定 → 選択中の GO → 同じ .sequence を指す先頭。
    scene::SequencePlayerComponent* ResolvePreviewPlayer(EditorContext& ctx,
                                                         scene::EntityID& outEntity) const;
    /// 編集中の中身と再生位置をプレビューへ流す。
    void PushPreview(EditorContext& ctx);
    /// スクラブを畳んで、触られていた姿勢を SequenceSystem に戻させる。
    void ReleasePreview(EditorContext& ctx);

    // ── 描画 ──
    void DrawToolbar(EditorContext& ctx);
    void DrawTransport(EditorContext& ctx);
    void DrawTimeline(EditorContext& ctx);
    void DrawTrackContextMenu(EditorContext& ctx);
    void DrawInspector(EditorContext& ctx);
    void DrawTrackHeaderFields(asset::SequenceTrack& track);
    void DrawSelectedItemFields(asset::SequenceTrack& track);

    /// 列を時刻順へ並べ直し、選択が同じ項目を指したままになるよう添字を引き直す。
    void SortChannelKeepingSelection(int track, int channel, double keepTime);

    void AddTrack(asset::SequenceTrackType type);
    void DeleteTrack(int index);
    void AddItemAt(asset::SequenceTrack& track, int channel, double time);

    [[nodiscard]] double Duration() const;
    [[nodiscard]] double ApplySnap(double seconds) const;

    asset::SequenceAsset m_asset;
    std::string m_path;           ///< 開いているファイル (AssetBrowser が渡す形のまま)
    std::string m_assetPath;      ///< Assets 起点の正規化パス。Player との突き合わせに使う
    std::string m_requestedPath;
    bool        m_dirty = false;
    std::string m_error;
    std::string m_status;

    // スナップショット Undo。1 本の演出はトラック数十本なので丸ごと持って問題ない。
    std::vector<asset::SequenceAsset> m_undoStack;
    std::vector<asset::SequenceAsset> m_redoStack;

    Selection m_selection;

    double m_playhead   = 0.0;
    bool   m_previewing = false;   ///< スクラブをシーンへ出しているか
    bool   m_playing    = false;   ///< パネル内のプレビュー再生
    float  m_playSpeed  = 1.0f;
    bool   m_snap       = true;
    float  m_snapStep   = 1.0f / 60.0f;
    /// 編集のたびに進む版数。プレビューへ再発行すべきかの判定に使う。
    unsigned long long m_localRevision  = 0;
    unsigned long long m_pushedRevision = ~0ull;

    // 掴んでいる対象。kind は 0=帯/キーの移動、1=帯の右端。
    Selection m_drag;
    int    m_dragKind    = -1;
    double m_dragGrabTime = 0.0;
    double m_dragOrigin   = 0.0;
    double m_dragLastValue = 0.0;   ///< 並べ替え後に選び直すための開始時刻
    bool   m_dragPushedUndo = false;
    /// ルーラーを掴んでいる間。行の上へカーソルが外れてもスクラブを続ける。
    bool   m_scrubbingRuler = false;

    /// プレビューを出している Player。切り替えと後始末のために覚える。
    scene::EntityID m_previewEntity;
    /// ユーザーが明示的に選んだプレビュー先 (無効なら自動解決)。
    scene::EntityID m_pinnedEntity;

    // Inspector で時刻を編集し終えたときの並べ替え要求。
    //
    // WHY その場でやらないか: 描画中の各フィールドはベクタ要素への参照を持ったまま
    //     続きを描く。途中で並べ替えると、同じ参照が別の項目を指したまま
    //     残りのフィールドを編集してしまう。フレームの終わりまで待つ。
    int    m_pendingSortTrack   = -1;
    int    m_pendingSortChannel = 0;
    double m_pendingSortTime    = 0.0;

    int    m_contextTrack  = -1;
    double m_contextTime   = 0.0;   ///< 右クリックした時刻。ここへ項目を足す
    bool   m_openTrackMenu = false;
    std::string m_projectRoot;
};

} // namespace fbzz::editor
