/// @file    FluidEditorExtras.hpp
/// @brief   Fluid Editor の «作り始め» と «焼いた後» — テンプレート一覧・Baked タブ・自動保存・部品の控え
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// @note パネル本体 (FluidEditorPanel.cpp) から呼ぶ。状態はこのモジュールの中に閉じる。
#pragma once

#include <Editor/Util/FluidDocument.hpp>

#include <cstdint>
#include <string>

namespace fbzz::editor {
struct EditorContext;
}

namespace fbzz::editor::fluideditor {

struct State;


/// @note プロジェクトの Assets/Templates 配下のファイルか (Fluid に限らず Templates 全体)。
/// @note 区切り文字と大文字小文字の差は無視する。
[[nodiscard]] bool IsFluidTemplatePath(const EditorContext& ctx, const std::string& absPath);


/// @note モーダルで人が選んだこと。
enum class FluidSaveAsAction : std::uint8_t {
    None,      ///< まだ決めていない (モーダルが開いていない / 触っていない)
    SavedCopy, ///< 別名で書いた
    Overwrite, ///< 元のファイルを上書きすると人が選んだ
};

struct FluidSaveAsResult {
    FluidSaveAsAction action = FluidSaveAsAction::None;
    /// @note SavedCopy のときだけ入る、書いた .fluid の実パス。
    std::string path;
};

/// @note 次のフレームから «名前を付けて保存» のモーダルを開く。sourcePath は元の .fluid (名前の既定に使う)、
/// @note defaultDirectory は保存先の既定 (実パス)。
void OpenFluidSaveAsModal(const std::string& sourcePath, const std::string& defaultDirectory);
/// @note モーダルを描く (毎フレーム呼ぶ)。書くのは document の «今のレシピ»。
[[nodiscard]] FluidSaveAsResult DrawFluidSaveAsModal(EditorContext& ctx, FluidDocument& document);


/// @note 次のフレームからテンプレート一覧のモーダルを開く。
void OpenFluidTemplateGallery();
/// @note モーダルを描く (毎フレーム呼ぶ)。テンプレートから .fluid を作ったら、その実パスを返す
/// @note (呼び手が開く)。作っていなければ空。defaultDirectory は作る先の既定 (実パス)。
[[nodiscard]] std::string DrawFluidTemplateGallery(EditorContext& ctx, const std::string& defaultDirectory);


/// @note 焼き上がった .fluid を Baked タブへ渡す (焼きのジョブが Done になったフレームに呼ぶ)。
/// @note 隣の出力 (Flipbook / MV) を読み直す。
void SetFluidBakedResult(EditorContext& ctx, const std::string& fluidPath,
                         const std::string& sourceFluidPath = {});
/// @note Baked タブの中身。焼いた結果が無ければ «まだ焼いていない» 案内だけを描く。
void DrawFluidBakedTab(EditorContext& ctx, State& state);
/// @note 焼いた結果を持っているか (タブを出すかの判断に使う)。
[[nodiscard]] bool HasFluidBakedResult();
/// @note GPU 資源を返す (パネルの OnShutdown から)。
void ShutdownFluidBakedTab(EditorContext& ctx);


/// @note 毎フレーム呼ぶ。設定が入っていて未保存の変更があり、最後の編集から一定時間が経っていれば保存する。
/// @note テンプレート (IsFluidTemplatePath) は書かない。保存したら true (呼び手がステータスに出す)。
bool TickFluidAutoSave(EditorContext& ctx, FluidDocument& document);
/// @note 設定 (EditorSettings に保存する)。UI はツールバーのチェックボックス。
[[nodiscard]] bool FluidAutoSaveEnabled(const EditorContext& ctx);
void SetFluidAutoSaveEnabled(EditorContext& ctx, bool enabled);

/// @note 部品 1 つを控える (元のレシピからの値のコピー。元の .fluid を閉じても残る)。
/// @note 控えは翻訳単位の静的で持つ。FluidDocument 内に置くと、貼る先を開いた時点 (Open は hide/solo もろとも作り直す) で消える。
void SetFluidPartClipboard(const fluid::FluidRecipe& recipe, FluidSelectionKind list, int index);
/// @note 控えを持っているか。
[[nodiscard]] bool HasFluidPartClipboard();
/// @note 控えがこの種別か (種別が違うものは貼れない — Source を Forces へ入れる意味が無い)。
[[nodiscard]] bool FluidPartClipboardMatches(FluidSelectionKind list);
/// @note 控えの種別 (無ければ None)。«選択がどこに居ても控えのリストへ貼る» の宛先に使う。
[[nodiscard]] FluidSelectionKind FluidPartClipboardKind();
/// @note 控えの表示名 (メニューに出す)。控えが無ければ空。
[[nodiscard]] std::string FluidPartClipboardLabel();
/// @note 控えを recipe の list の at 番目へ挿す。at が範囲外なら末尾。種別違い・上限・控え無しなら false。
/// @note 名前は貼る先で衝突しないよう連番を振り直す。
bool PasteFluidPartClipboard(fluid::FluidRecipe& recipe, FluidSelectionKind list, int at);

/// @note 同じリストの中で衝突しない名前。base が空なら空のまま (既定の表示名は添字から作られるので衝突しない)。
/// @note ignoreIndex はその部品自身の添字 (自分の名前を «衝突» と数えない。-1 で全部を見る)。
[[nodiscard]] std::string MakeUniqueFluidPartName(const fluid::FluidRecipe& recipe, FluidSelectionKind list,
                                                  const std::string& base, int ignoreIndex);

} // namespace fbzz::editor::fluideditor
