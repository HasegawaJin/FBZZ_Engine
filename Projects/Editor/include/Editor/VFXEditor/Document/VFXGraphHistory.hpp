// FBZZ Engine
// VFXGraphHistory.hpp | fbzz::editor
// VFX グラフ編集のスナップショット履歴
// WHY: Scene の UndoStack と混ぜると「シーンを触っていないのにシーン側の Undo が動く」
//      状態になる。アセットエディタ内で完結する独立した履歴として持つ。
#pragma once

#include <Engine/Asset/VFXGraphAsset.hpp>
#include <cstdint>
#include <vector>

namespace fbzz::editor {

// グラフ全体のスナップショットを積むだけの単純な履歴。
// NOTE: 差分ではなく全体コピーなのは、ノード追加・リンク張り替え・グループ移動など
//       変更の形が多すぎて、差分表現の維持コストが実利を上回るため。
//       上限を設けてメモリを頭打ちにする運用でカバーしている (Push 側の責務)。
struct VFXGraphHistory {
    std::vector<asset::VFXGraphAsset> undoStack;
    std::vector<asset::VFXGraphAsset> redoStack;
    // グラフ本体と同じ順序で状態IDを保持し、保存地点まで戻ったときだけ dirty を解除する。
    // WHY: スナップショット同士を全フィールド比較すると、新しいVFX設定を追加するたびに
    //      比較処理の更新が必要になる。単調増加IDならアセット構造に依存せず保存点を追跡できる。
    std::vector<std::uint64_t> undoStateIds;
    std::vector<std::uint64_t> redoStateIds;
    std::uint64_t currentStateId = 0;
    std::uint64_t savedStateId = 0;
    std::uint64_t nextStateId = 1;

    [[nodiscard]] bool CanUndo() const { return !undoStack.empty(); }
    [[nodiscard]] bool CanRedo() const { return !redoStack.empty(); }

    // グラフを差し替えたとき (読み込み・Template 置換) は履歴の連続性が切れる。
    void Clear()
    {
        undoStack.clear();
        redoStack.clear();
        undoStateIds.clear();
        redoStateIds.clear();
        currentStateId = 0;
        savedStateId = 0;
        nextStateId = 1;
    }
};

} // namespace fbzz::editor
