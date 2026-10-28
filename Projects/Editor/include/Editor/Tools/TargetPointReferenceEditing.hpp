/// @file    TargetPointReferenceEditing.hpp
/// @brief   共通配置点を既存consumerの参照へ接続するEditor境界。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#pragma once

namespace fbzz::scene { class GameObject; }
namespace fbzz::editor {

struct EditorContext;
class OperatorRegistry;

/// @brief 参照の接続・選択・点の作成とanchor設定を共通操作へ登録する。
void RegisterTargetPointReferenceOperators(OperatorRegistry& registry);

/// @brief 既存consumerの参照を登録済み操作から編集する。
/// @return 操作を実行したらtrue。呼び出し側は古いGO/Component参照を使う描画を終了すること。
[[nodiscard]] bool DrawTargetPointReferenceInspector(EditorContext& ctx, scene::GameObject& object);

} /// @note namespace fbzz::editor
