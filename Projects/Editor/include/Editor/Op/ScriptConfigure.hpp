/// @file    ScriptConfigure.hpp
/// @brief   名前と階層範囲で指定する Script 設定の共通入口。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#pragma once

#include <Editor/Op/OpData.hpp>
#include <string>
#include <vector>

namespace fbzz::scene { class Scene; class GameObject; }
namespace fbzz::editor {
class OperatorRegistry;

/// @note 文字列は一意な名前またはルートからの階層パス。配列は順序と重複を保持する。
/// @note root="$owner" は設定先を基準にする。曖昧な単一参照・不正階層は失敗し output を変更しない。
[[nodiscard]] bool ResolveScriptObjects(scene::Scene& scene, const OpData& selector,
    scene::GameObject* owner, std::vector<scene::GameObject*>& output, std::string& diagnostic);

void RegisterScriptConfigureOperators(OperatorRegistry& registry);
} /// @note namespace fbzz::editor
