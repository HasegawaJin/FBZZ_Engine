/// @file    SequencePanel.hpp
/// @brief   .sequence の尺を目で詰めるタイムラインパネル
/// @author  Hasegawa Jin
/// @date    2026-08-26/// @note 演出の良し悪しは数値ではなく尺で決まるため、帯とキーを掴んで動かし、その場でスクラブして
/// @note       絵を確認できることが編集の本体。MVP を分割しないのは、トラック 7 種とキー編集しかなく
/// @note       グラフ・ノード・配線が無いため、分ける対象が無いところで層を切ると行き来のコストだけが増える。
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Engine/Asset/Animation/Sequences/SequenceAsset.hpp>
#include <Engine/Scene/References/Entity.hpp>

#include <string>
#include <vector>

namespace fbzz::scene { struct SequencePlayerComponent; class GameObject; }

namespace fbzz::editor {

class SequencePanel final : public IPanel {
public:
    const char* GetWindowName()        const override { return "Sequence"; }
    bool        GetDefaultVisibility() const override { return false; }
    HotkeyScope GetHotkeyScope() const override { return m_path.empty() ? HotkeyScope::None : HotkeyScope::Sequence; }
    void        OnShutdown() override;
    void OnRegisterOperators(OperatorRegistry& registry) override;
    void OnUpdate(EditorContext& ctx, float dt) override;

    /// @note AssetBrowser のダブルクリックから開く。
    void RequestOpen(const std::string& path) { m_requestedPath = path; }

protected:
    void OnRenderContent(EditorContext& ctx) override;
    void OnAfterEnd(EditorContext& ctx) override;

private:
    /// @note 選択の単位。トラック内のどの列 (channel) の何番目か。
    /// @note TransformTrack は position / rotation / scale の 3 列を 1 行に重ねて描く。
    /// @note       平坦な添字 1 本にすると、列をまたいだ削除や並べ替えで番号の意味が変わり、選択が滑る。
    struct Selection {
        int track   = -1; ///< 選択したトラックの配列添字。未選択なら -1。
        int channel = 0; ///< トラック内で選択したキーやイベントを含むチャンネル。
        int index   = -1; ///< 選択対象のチャンネル内の配列添字。未選択なら -1。
        [[nodiscard]] bool HasTrack() const { return track >= 0; }
        [[nodiscard]] bool HasItem()  const { return track >= 0 && index >= 0; }
    };

    bool Load(EditorContext& ctx, const std::string& path);
    bool Save();
    void CloseDocument(EditorContext& ctx);
    void RequestDocumentAction(EditorContext& ctx, const char* action, const std::string& path = {});

    void PushUndo();
    void Undo();
    void Redo();
    /// @note 直前のウィジェットが「掴まれた」フレームだけ Undo を積む。
    /// @note DragFloat は掴んでいる間ずっと変更を返す。変更のたびに積むと 1 回のドラッグで
    /// @note       Undo が数十個生まれ、Ctrl+Z を押し続けても元の値まで戻れなくなる。
    void SnapshotOnActivate() {
        if (ImGui::IsItemActivated()) {
            PushUndo();
        }
    }
    /// @note 編集を確定する。未保存フラグと、プレビューへ流す版数を同時に進める。
    void MarkEdited() { m_dirty = true; ++m_localRevision; }

    /// @name プレビュー
    /// @{
    /// @note スクラブ先の Player を決める。明示指定 → 選択中の GO → 同じ .sequence を指す先頭。
    scene::SequencePlayerComponent* ResolvePreviewPlayer(EditorContext& ctx,
                                                         scene::EntityID& outEntity) const;
    /// @note 編集中の中身と再生位置をプレビューへ流す。
    void PushPreview(EditorContext& ctx);
    /// @note スクラブを畳んで、触られていた姿勢を SequenceSystem に戻させる。
    void ReleasePreview(EditorContext& ctx);
    /// @}

    /// @name 描画
    /// @{
    void DrawToolbar(EditorContext& ctx);
    void DrawTransport(EditorContext& ctx);
    void DrawTimeline(EditorContext& ctx);
    void DrawTrackContextMenu(EditorContext& ctx);
    void DrawInspector(EditorContext& ctx);
    void DrawTrackHeaderFields(asset::SequenceTrack& track);
    void DrawSelectedItemFields(asset::SequenceTrack& track);
    void DrawFloatCurve(asset::SequenceTrack& track);
    void FitFloatCurve(const asset::SequenceTrack& track);

    /// @note 列を時刻順へ並べ直し、選択が同じ項目を指したままになるよう添字を引き直す。
    void SortChannelKeepingSelection(int track, int channel, double keepTime);

    void AddTrack(asset::SequenceTrackType type);
    void DeleteTrack(int index);
    void AddItemAt(asset::SequenceTrack& track, int channel, double time);

    [[nodiscard]] double Duration() const;
    [[nodiscard]] double ApplySnap(double seconds) const;

    asset::SequenceAsset m_asset; ///< このパネルに開いてタイムライン上で編集中のシーケンス。
    std::string m_path;           ///< 開いているファイル (AssetBrowser が渡す形のまま)
    std::string m_assetPath;      ///< Assets 起点の正規化パス。Player との突き合わせに使う
    std::string m_requestedPath; ///< パネルが次に開くシーケンスファイルのパス。
    bool        m_dirty = false; ///< タイムラインの編集中の値をまだ保存していないか。
    std::string m_error; ///< シーケンスを開くか保存する操作に失敗した理由。
    std::string m_status; ///< 直近の編集やプレビュー操作について表示する結果。

    std::vector<asset::SequenceAsset> m_undoStack; ///< スナップショット Undo。1 本の演出はトラック数十本なので丸ごと持って問題ない。
    std::vector<asset::SequenceAsset> m_redoStack; ///< やり直し操作で復元するシーケンスのスナップショット。

    Selection m_selection; ///< タイムラインかカーブで選択したトラックと項目。
    int m_curveTrack = -1; ///< View bounds stay fixed during dragging; Fit explicitly frames the sampled curve.
    float m_curveMin = -1.0f; ///< 表示中のカーブグラフの縦軸の下限。
    float m_curveMax = 1.0f; ///< 表示中のカーブグラフの縦軸の上限。
    double m_curveEnd = 1.0; ///< 表示中のカーブグラフの横軸の末尾 [s]。
    int m_curveDragKind = -1; ///< 現在カーブ上でドラッグしている要素の種類。ドラッグしていなければ -1。
    bool m_curveUndoPushed = false; ///< 現在のカーブドラッグの開始状態を Undo に記録済みか。

    double m_playhead   = 0.0; ///< パネル上の再生ヘッドの位置 [s]。
    bool m_apiPreview = false; ///< Editor API 経由でこのシーケンスのプレビューが要求されているか。
    bool   m_previewing = false;   ///< スクラブをシーンへ出しているか
    bool   m_playing    = false;   ///< パネル内のプレビュー再生
    float  m_playSpeed  = 1.0f; ///< パネル内のシーケンスプレビューの再生速度倍率。
    bool   m_snap       = true; ///< タイムライン上で移動する項目を時刻の刻みに揃えるか。
    float  m_snapStep   = 1.0f / 60.0f; ///< タイムラインでスナップする時刻の刻み幅 [s]。
    unsigned long long m_localRevision  = 0; ///< 編集のたびに進む版数。プレビューへ再発行すべきかの判定に使う。
    unsigned long long m_pushedRevision = ~0ull; ///< 最後にプレビュー先の SequencePlayer へ流した編集中のリビジョン。

    Selection m_drag; ///< 掴んでいる対象。kind は 0=帯/キーの移動、1=帯の右端。
    int    m_dragKind    = -1; ///< タイムライン上で進行中の移動または長さ変更の種類。
    double m_dragGrabTime = 0.0; ///< タイムライン上で掴み始めたマウス位置に相当する時刻 [s]。
    double m_dragOrigin   = 0.0; ///< タイムラインのドラッグを始める前に控えた時刻 [s]。
    double m_dragLastValue = 0.0;   ///< 並べ替え後に選び直すための開始時刻
    bool   m_dragPushedUndo = false; ///< タイムラインで現在ドラッグ中の操作を Undo に登録済みか。
    bool   m_scrubbingRuler = false; ///< ルーラーを掴んでいる間。行の上へカーソルが外れてもスクラブを続ける。

    scene::EntityID m_previewEntity; ///< プレビューを出している Player。切り替えと後始末のために覚える。
    scene::EntityID m_pinnedEntity; ///< ユーザーが明示的に選んだプレビュー先 (無効なら自動解決)。

    /// @note Inspector で時刻を編集し終えたときの並べ替え要求。
    /// @note 描画中の各フィールドはベクタ要素への参照を持ったまま続きを描くため、途中で並べ替えると
    /// @note       同じ参照が別の項目を指したまま残りを編集してしまう。フレームの終わりまで待つ。
    int    m_pendingSortTrack   = -1; ///< 描画後に時刻順へ並べ直すトラックの添字。不要なら -1。
    int    m_pendingSortChannel = 0; ///< 描画後に並べ直すトラック内のチャンネル。
    double m_pendingSortTime    = 0.0; ///< 並べ直した後に同じキーを選び直すときの基準時刻 [s]。

    int    m_contextTrack  = -1; ///< コンテキストメニューを開く行のトラック添字。
    double m_contextTime   = 0.0;   ///< 右クリックした時刻。ここへ項目を足す
    bool   m_openTrackMenu = false; ///< 指定したトラックのコンテキストメニューを開く要求。
    std::string m_projectRoot; ///< シーケンスから参照するアセットの基準となるプロジェクトルート。
    /// @}
};

} // namespace fbzz::editor
