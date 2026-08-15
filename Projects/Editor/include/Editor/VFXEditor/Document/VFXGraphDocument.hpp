// FBZZ Engine
// VFXGraphDocument.hpp | fbzz::editor
// 編集中の .vfx 1 件を表すドキュメント (UI 非依存)
// WHY: 以前はグラフ実体・保存先・dirty・履歴・クリップボードが全て Panel の private
//      メンバーとして散らばっており、「何が保存対象で、何が表示状態か」を型で区別できなかった。
//      ここへ集約することで、View は表示に専念し、保存・巻き戻しの正しさはこの層だけで担保できる。
#pragma once

#include <Editor/VFXEditor/Document/VFXGraphClipboard.hpp>
#include <Editor/VFXEditor/Document/VFXGraphHistory.hpp>
#include <Engine/Asset/VFXGraphAsset.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::editor {

// 編集フラグ。true が代入されるたびに版数を進める。
// WHY: グラフを書き換える箇所は Inspector だけで 60 以上あり、そこへ個別に
//      「版数を進める」呼び出しを足すと必ず取りこぼす。フラグ自体に責務を持たせれば、
//      以後どこで編集してもライブ反映が漏れない。false 代入 (保存/読込) では進めない。
class VFXGraphDirtyFlag {
public:
    VFXGraphDirtyFlag& operator=(bool value)
    {
        if (value) Bump();
        m_dirty = value;
        return *this;
    }
    VFXGraphDirtyFlag& operator|=(bool value)
    {
        if (value) { m_dirty = true; Bump(); }
        return *this;
    }
    explicit operator bool() const { return m_dirty; }
    [[nodiscard]] bool IsDirty() const { return m_dirty; }
    // 実行結果に影響する変更の版数。ライブ反映の判定に使う。
    [[nodiscard]] std::uint64_t Revision() const { return m_revision; }
    // 保存対象の変更全体の版数。ノード座標のようなエディタ専用データも含む。
    [[nodiscard]] std::uint64_t ContentRevision() const { return m_contentRevision; }
    // グラフを差し替えたときに、内容が変わったことを明示的に伝える。
    void Touch() { Bump(); }
    // ノード座標やグループ枠のようなエディタ専用データだけが変わったとき用。
    // WHY: ドラッグ中は毎フレーム呼ばれる。ここで実行版数まで進めると、見た目が
    //      何も変わらないのにランタイムへの再適用が毎フレーム走ってしまう。
    //      保存は必要なので内容版数だけ進める。
    void MarkEditorLayoutDirty() { m_dirty = true; ++m_contentRevision; }

private:
    void Bump() { ++m_revision; ++m_contentRevision; }

    bool m_dirty = false;
    std::uint64_t m_revision = 0;
    std::uint64_t m_contentRevision = 0;
};

// 編集中グラフとその付帯状態。ImGui にも EditorContext にも依存しない。
// NOTE: フィールドを public にしているのは、Document 自体が「状態の入れ物」であり、
//       アクセサで包んでも不変条件を守れる性質の値がほとんど無いため。
//       不変条件を持つ dirty だけはクラス (VFXGraphDirtyFlag) として振る舞いを持たせている。
class VFXGraphDocument {
public:
    asset::VFXGraphAsset graph;
    std::string path;   // 保存先の .vfx パス。空なら未保存の新規グラフ
    std::string error;  // 直近の読み込み・検証エラー (空なら健全)

    VFXGraphDirtyFlag dirty;
    VFXGraphHistory history;
    VFXGraphClipboard clipboard;

    // 保存は許すが見た目が壊れる設定の一覧。エラーとは別の面として出す。
    // グラフ版数が変わったときだけ再収集する (毎フレームの全ノード走査を避ける)。
    std::vector<asset::VFXGraphWarning> warnings;
    // 版数 0 のグラフを読み込んだ直後にも必ず1回収集させるための番兵。
    std::uint64_t warningRevision = ~0ull;

    // 直前の編集が触ったノード。ランタイム側はこのノードだけ設定を写し直す。
    // -1 = 不明 (一括編集)。ドラッグ中の再適用をノード数に比例させないための情報。
    int liveDirtyNodeId = -1;

    // Entry から到達不能なノード id。Canvas 描画前に更新する。
    std::vector<int> unreachableNodes;

    // Solo 中のノード id。-1 で無効。アセットには保存しない表示状態。
    int soloNodeId = -1;
    // Solo の切り替えもライブ反映の再送出契機になる (値版数だけでは検知できない)。
    int lastPushedSoloNodeId = -1;

    // A/B 比較: 編集前のグラフを保持し、トグルで見比べる。
    // WHY: 少しずつ触っていると「良くなったのか慣れただけか」が分からなくなる。
    //      同じ scrub 時刻で切り替えられれば判断が一瞬で済む。
    bool hasComparisonSnapshot = false;
    bool showComparison = false;
    asset::VFXGraphAsset comparisonGraph;

    // Solo 指定ノードだけを残したプレビュー用のコピーを作る。アセットは書き換えず、
    // ライブ反映へ渡すコピーの enabled だけを落とすため、保存内容に影響しない。
    [[nodiscard]] asset::VFXGraphAsset MakeSoloFilteredGraph() const;

    // Entry から到達できないノードを集計する。実行時に一切起動しない「配線し忘れ」を
    // Canvas 上で即座に気付けるようにするための可視化用。
    void RefreshUnreachableNodes();
    [[nodiscard]] bool IsNodeUnreachable(int id) const;
};

} // namespace fbzz::editor
