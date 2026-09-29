/// @file    ScriptRequirementReview.hpp
/// @brief   必須設定の検証結果と Inspector への移動要求。
/// @author  Hasegawa Jin
/// @date    2026-09-29
#pragma once
#include <Engine/Scene/ScriptValidation.hpp>
#include <string>
#include <vector>

namespace fbzz::editor {
struct EditorContext;
class OperatorRegistry;

struct ScriptRequirementRow {
    scene::ScriptRequirementIssue issue;
    std::string scriptId;
};

struct ScriptRequirementFocus {
    std::string nodeId;
    std::string scriptId;
    std::string field;
    std::string message;
    bool scrollPending = false;
    bool unlockPending = false;
};

struct ScriptRequirementReview {
    /// @note 比較専用。古い Scene をこのポインタから参照しない。
    const scene::Scene* checkedScene = nullptr;
    std::string scenePath;
    std::vector<ScriptRequirementRow> rows;
    ScriptRequirementFocus focus;
    int revision = 0;
    int scriptsChecked = 0;
    int unavailableScripts = 0;
    bool checked = false;
    bool focusPanel = false;
    std::string message;
};

bool CanReviewScriptRequirements(const EditorContext& ctx);
bool IsScriptRequirementReviewCurrent(const EditorContext& ctx);
void RefreshScriptRequirementReview(EditorContext& ctx);
void RegisterScriptRequirementOperators(OperatorRegistry& registry);
} /// @note namespace fbzz::editor
