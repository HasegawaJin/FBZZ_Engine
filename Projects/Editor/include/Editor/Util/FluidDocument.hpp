/// @file    FluidDocument.hpp
/// @brief   Fluid Editor が編集している 1 つの .fluid (レシピ・保存状態・Undo・外からの書き換えの検知)
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// WHY レシピの持ち主を 1 つにするか:
///   以前は Inspector と Volume Flipbook Baker パネルがそれぞれ別のコピーを持ち、片方の Save が
///   もう片方の書き込みを黙って潰していた。編集する人 (Fluid Editor) は必ずこの 1 つを通し、
///   他の書き手 (AI の fluid.set・Baker パネル) の変更はディスクの更新時刻で拾う。
///
/// Undo は «レシピ全体の前後のスナップショット» を UndoStack へ積む (数 KB × 履歴 128 で足りる)。
/// Undo 時に同じ .fluid が開いていればメモリへ戻し、開いていなければファイルへ書く。
#pragma once

#include <Engine/Asset/FluidRecipe.hpp>

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <utility>

namespace fbzz::editor {

struct EditorContext;

enum class FluidSelectionKind : std::uint8_t { None, Simulation, Look, Output, Bake, Source, Force, Collider };

/// Fluid Editor で選んでいるもの。Source / Force / Collider のときだけ index が意味を持つ (リスト内の添字)。
struct FluidSelection {
    FluidSelectionKind kind = FluidSelectionKind::None;
    int index = -1;

    [[nodiscard]] bool IsPart() const
    {
        return kind == FluidSelectionKind::Source || kind == FluidSelectionKind::Force
            || kind == FluidSelectionKind::Collider;
    }
    [[nodiscard]] bool operator==(const FluidSelection& other) const
    {
        return kind == other.kind && index == other.index;
    }
};

class FluidDocument {
public:
    FluidDocument() = default;
    ~FluidDocument();
    FluidDocument(const FluidDocument&) = delete;
    FluidDocument& operator=(const FluidDocument&) = delete;

    /// .fluid を開く (実パス)。今の文書は閉じる (未保存の変更は捨てる — 確認は呼び手の仕事)。
    [[nodiscard]] bool Open(const std::string& absPath, std::string& outError);
    void Close();
    [[nodiscard]] bool IsOpen() const { return m_open; }
    /// 実パス。
    [[nodiscard]] const std::string& Path() const { return m_path; }
    [[nodiscard]] const asset::FluidRecipe& Recipe() const { return m_recipe; }
    /// 変更のたびに増える通番 (プレビューの解き直しの鍵)。Open / 読み直しでも増える。
    [[nodiscard]] std::uint64_t Revision() const { return m_revision; }
    [[nodiscard]] bool IsDirty() const { return m_dirty; }

    /// 1 回で終わる変更 (部品の追加・削除・並べ替え・プリセット適用・キーを打つ)。
    /// fn がレシピを書き換え、変わっていれば Undo を 1 つ積む。変わったら true。
    bool Edit(EditorContext& ctx, const char* label, const std::function<void(asset::FluidRecipe&)>& fn);

    /// 即時モード UI 用。working は Recipe() のコピーを UI が書き換えたもの。changed なら取り込む。
    /// 操作中 (ImGui::IsAnyItemActive() — ドラッグやテキスト入力) の変更は 1 つの Undo にまとめ、
    /// 手を離したフレームで積む。操作中でない変更 (チェックボックス・コンボ) はその場で 1 つ積む。
    /// 毎フレーム、UI を描いた後に 1 回呼ぶこと (changed = false でも呼ぶ — 手を離したのを検知するため)。
    void Commit(EditorContext& ctx, const char* label, const asset::FluidRecipe& working, bool changed);

    /// ビューポートのギズモ用。Begin で操作前を覚え、Apply で値だけ変え (Undo は積まない)、End で 1 つ積む。
    void BeginInteractiveEdit();
    void ApplyInteractive(const asset::FluidRecipe& working);
    void EndInteractiveEdit(EditorContext& ctx, const char* label);
    [[nodiscard]] bool InInteractiveEdit() const { return m_interactive; }

    /// .fluid へ書く。成功したら dirty を下ろし、更新時刻を覚え直し、AssetManager へ読み直しを知らせる。
    [[nodiscard]] bool Save(EditorContext& ctx, std::string& outError);

    /// 毎フレーム呼ぶ。外 (AI・Baker パネル) からファイルが書き換わっていたら:
    /// 未保存の変更が無ければ黙って読み直し (Revision が進む)、あれば HasExternalConflict() を立てる。
    void PollExternalChange();
    [[nodiscard]] bool HasExternalConflict() const { return m_conflict; }
    /// 衝突の解決: ディスクの内容で置き換える / 手元を残す (次の Save でディスクを上書きする)。
    void ReloadFromDisk();
    void KeepLocal();

    /// その .fluid を Fluid Editor が «未保存の変更を抱えたまま» 開いているか (実パスで問い合わせる)。
    /// WHY 外から問い合わせられるようにするか: AI が .fluid へ書いても、人が未保存で開いていれば
    ///     次の保存で消える。書いた側が «消えるかもしれない» と言えるようにする。
    [[nodiscard]] static bool HasUnsavedChanges(const std::string& absPath);

    // ── プレビューだけの表示切り替え (保存しない・焼きには効かない) ──
    [[nodiscard]] bool IsHidden(FluidSelectionKind list, int index) const;
    void SetHidden(FluidSelectionKind list, int index, bool hidden);
    /// ソロにした部品だけを残す (同じリストの他は隠す)。もう一度同じものを渡すと解除。
    void ToggleSolo(FluidSelectionKind list, int index);
    [[nodiscard]] bool IsSolo(FluidSelectionKind list, int index) const;
    /// 隠した部品を enabled = false にしたプレビュー用のコピー。
    [[nodiscard]] asset::FluidRecipe PreviewRecipe() const;

    /// 部品の増減・並べ替えで添字がずれたときに hide / solo を部品へ付いて回らせる。
    /// WHY 呼び手の仕事にするか: hide / solo は «リスト内の添字» で覚えているので、レシピの
    ///     vector が動いても勝手には追従しない。どの列がどう動いたかを知っているのは
    ///     操作した側 (Outliner) だけなので、Edit の直後にその形を教えてもらう。
    void RemapVisibilityAfterInsert(FluidSelectionKind list, int insertedIndex);
    void RemapVisibilityAfterRemove(FluidSelectionKind list, int removedIndex);
    void RemapVisibilityAfterMove(FluidSelectionKind list, int from, int to);

    /// UI の選択 (保存しない)。部品の削除・並べ替えで添字がずれたら Edit の中で呼び手が直す。
    FluidSelection selection;

private:
    friend struct FluidDocumentAccess;
    /// 旧添字 → 新添字 (負を返したら «消えた» として落とす)。list 以外の印は触らない。
    void RemapVisibility(FluidSelectionKind list, const std::function<int(int)>& map);
    void ReplaceRecipe(const asset::FluidRecipe& recipe, bool markDirty);
    void PushUndo(EditorContext& ctx, const char* label, const asset::FluidRecipe& before,
                  const asset::FluidRecipe& after);

    asset::FluidRecipe m_recipe;
    asset::FluidRecipe m_interactiveBefore;
    std::string m_path;
    std::int64_t m_diskStamp = 0;
    std::uint64_t m_revision = 0;
    bool m_open = false;
    bool m_dirty = false;
    bool m_conflict = false;
    bool m_interactive = false;
    bool m_commitActive = false;
    asset::FluidRecipe m_commitBefore;
    std::string m_commitLabel;
    std::set<std::pair<int, int>> m_hidden;
    std::pair<int, int> m_solo{ -1, -1 };
};

} // namespace fbzz::editor
