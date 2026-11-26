/// @file    ScriptRequirementReview.hpp
/// @brief   必須設定の検証結果と Inspector への移動要求。
/// @author  Hasegawa Jin
/// @date    2026-09-29
#pragma once
#include <Engine/Scene/Scripting/ScriptValidation.hpp>
#include <string>
#include <vector>

namespace fbzz::editor {
struct EditorContext;
class OperatorRegistry;

struct ScriptRequirementRow {
    scene::ScriptRequirementIssue issue; ///< 対象スクリプトの必須設定から見つかった未設定または解決不能の要件。
    std::string scriptId; ///< 検証結果の要件を持つスクリプト個体の識別子。
};

struct ScriptRequirementFocus {
    std::string nodeId; ///< Inspector で選択する GameObject の識別子。
    std::string scriptId; ///< Inspector で移動先とするスクリプト個体の識別子。
    std::string field; ///< Inspector の表示を移す設定フィールドの名前。
    std::string message; ///< Inspector に表示する設定移動の診断。
    bool scrollPending = false; ///< 移動先のフィールドが見える位置までスクロールする一回限りの要求。
    bool unlockPending = false; ///< 移動要求を処理するために Inspector のロックを解除するか。
};

struct ScriptRequirementReview {
    /// @note 比較専用。古い Scene をこのポインタから参照しない。
    const scene::Scene* checkedScene = nullptr; ///< 検証したシーンと現在のシーンを照合する借用ポインター。照合だけに使う。
    std::string scenePath; ///< 必須設定を検証した時点の編集シーンのパス。
    std::vector<ScriptRequirementRow> rows; ///< 最後に行ったスクリプト検証で見つかった要件別の結果。
    ScriptRequirementFocus focus; ///< 検証結果から Inspector の対象へ移る保留中の要求。
    int revision = 0; ///< UI が検証結果の更新を検知するためのリビジョン。
    int scriptsChecked = 0; ///< 最後の検証で必須要件を調べたスクリプトの件数。
    int unavailableScripts = 0; ///< 検証できるスクリプト個体を取得できなかった件数。
    bool checked = false; ///< 現在表示している検証結果を作成済みか。
    bool focusPanel = false; ///< 検証結果を表示するパネルにフォーカスを移す一回限りの要求。
    std::string message; ///< 検証結果から問題箇所を表示できなかった場合に、Script Requirements パネルへ出す診断。
};

bool CanReviewScriptRequirements(const EditorContext& ctx);
bool IsScriptRequirementReviewCurrent(const EditorContext& ctx);
void RefreshScriptRequirementReview(EditorContext& ctx);
void RegisterScriptRequirementOperators(OperatorRegistry& registry);
} // namespace fbzz::editor
