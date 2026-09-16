/// @file    FluidEditorExtras.hpp
/// @brief   Fluid Editor の «作り始め» と «焼いた後» — テンプレート一覧・Baked タブ・自動保存・部品の控え
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// パネル本体 (FluidEditorPanel.cpp) から呼ぶ。状態はこのモジュールの中に閉じる。
#pragma once

#include <Editor/Util/FluidDocument.hpp>

#include <cstdint>
#include <string>

namespace fbzz::editor {
struct EditorContext;
}

namespace fbzz::editor::fluideditor {

struct State;

// ── テンプレートの置き場 ──

/// プロジェクトの Assets/Templates 配下のファイルか (Fluid に限らず Templates 全体)。
/// 区切り文字と大文字小文字の差は無視する。
[[nodiscard]] bool IsFluidTemplatePath(const EditorContext& ctx, const std::string& absPath);

// ── 名前を付けて保存 ──

/// モーダルで人が選んだこと。
enum class FluidSaveAsAction : std::uint8_t {
    None,      ///< まだ決めていない (モーダルが開いていない / 触っていない)
    SavedCopy, ///< 別名で書いた
    Overwrite, ///< 元のファイルを上書きすると人が選んだ
};

struct FluidSaveAsResult {
    FluidSaveAsAction action = FluidSaveAsAction::None;
    /// SavedCopy のときだけ入る、書いた .fluid の実パス。
    std::string path;
};

/// 次のフレームから «名前を付けて保存» のモーダルを開く。sourcePath は元の .fluid (名前の既定に使う)、
/// defaultDirectory は保存先の既定 (実パス)。
void OpenFluidSaveAsModal(const std::string& sourcePath, const std::string& defaultDirectory);
/// モーダルを描く (毎フレーム呼ぶ)。書くのは document の «今のレシピ»。
[[nodiscard]] FluidSaveAsResult DrawFluidSaveAsModal(EditorContext& ctx, FluidDocument& document);

// ── テンプレート一覧 (New from template) ──

/// 次のフレームからテンプレート一覧のモーダルを開く。
void OpenFluidTemplateGallery();
/// モーダルを描く (毎フレーム呼ぶ)。テンプレートから .fluid を作ったら、その実パスを返す
/// (呼び手が開く)。作っていなければ空。defaultDirectory は作る先の既定 (実パス)。
[[nodiscard]] std::string DrawFluidTemplateGallery(EditorContext& ctx, const std::string& defaultDirectory);

// ── Baked タブ (焼いた結果を並べて再生する) ──

/// 焼き上がった .fluid を Baked タブへ渡す (焼きのジョブが Done になったフレームに呼ぶ)。
/// 隣の出力 (Flipbook / MV) を読み直す。
void SetFluidBakedResult(EditorContext& ctx, const std::string& fluidPath);
/// Baked タブの中身。焼いた結果が無ければ «まだ焼いていない» 案内だけを描く。
void DrawFluidBakedTab(EditorContext& ctx, State& state);
/// 焼いた結果を持っているか (タブを出すかの判断に使う)。
[[nodiscard]] bool HasFluidBakedResult();
/// GPU 資源を返す (パネルの OnShutdown から)。
void ShutdownFluidBakedTab(EditorContext& ctx);

// ── 自動保存 ──

/// 毎フレーム呼ぶ。設定が入っていて未保存の変更があり、最後の編集から一定時間が経っていれば保存する。
/// テンプレート (IsFluidTemplatePath) は書かない。保存したら true (呼び手がステータスに出す)。
bool TickFluidAutoSave(EditorContext& ctx, FluidDocument& document);
/// 設定 (EditorSettings に保存する)。UI はツールバーのチェックボックス。
[[nodiscard]] bool FluidAutoSaveEnabled(const EditorContext& ctx);
void SetFluidAutoSaveEnabled(EditorContext& ctx, bool enabled);

// ── 部品の控え (Outliner のコピー & ペースト) ──
//
// WHY 置き場を文書の外 (この翻訳単位の静的) にするか:
//   «この渦だけ別の .fluid へ持っていく» が目的なので、控えは FluidDocument より長生きしなければ
//   ならない。文書の中に持つと、貼る先を開いた時点 (Open は hide/solo もろとも作り直す) で消える。
//   ギャラリーや自動保存と同じく «パネル横断の持ち物» としてここに閉じる。

/// 部品 1 つを控える (元のレシピからの値のコピー。元の .fluid を閉じても残る)。
void SetFluidPartClipboard(const asset::FluidRecipe& recipe, FluidSelectionKind list, int index);
/// 控えを持っているか。
[[nodiscard]] bool HasFluidPartClipboard();
/// 控えがこの種別か (種別が違うものは貼れない — Source を Forces へ入れる意味が無い)。
[[nodiscard]] bool FluidPartClipboardMatches(FluidSelectionKind list);
/// 控えの種別 (無ければ None)。«選択がどこに居ても控えのリストへ貼る» の宛先に使う。
[[nodiscard]] FluidSelectionKind FluidPartClipboardKind();
/// 控えの表示名 (メニューに出す)。控えが無ければ空。
[[nodiscard]] std::string FluidPartClipboardLabel();
/// 控えを recipe の list の at 番目へ挿す。at が範囲外なら末尾。種別違い・上限・控え無しなら false。
/// 名前は貼る先で衝突しないよう連番を振り直す。
bool PasteFluidPartClipboard(asset::FluidRecipe& recipe, FluidSelectionKind list, int at);

/// 同じリストの中で衝突しない名前。base が空なら空のまま (既定の表示名は添字から作られるので衝突しない)。
/// ignoreIndex はその部品自身の添字 (自分の名前を «衝突» と数えない。-1 で全部を見る)。
[[nodiscard]] std::string MakeUniqueFluidPartName(const asset::FluidRecipe& recipe, FluidSelectionKind list,
                                                  const std::string& base, int ignoreIndex);

} // namespace fbzz::editor::fluideditor
