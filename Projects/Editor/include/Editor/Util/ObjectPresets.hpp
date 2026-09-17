/// @file    ObjectPresets.hpp
/// @brief   「Create」で置ける GameObject プリセットの単一登録表。
/// @author  Hasegawa Jin
/// @date    2026-08-14
#pragma once
#include <Engine/Scene/Entity.hpp>
#include <span>
#include <string_view>

namespace fbzz::scene { class GameObject; }

namespace fbzz::editor {

struct EditorContext;

/// @brief 生成物の位置をどの空間で解釈するか。
enum class PresetPlacement {
    World,     ///< 3D 空間。ルートなら Scene View の注視点へ、子なら親の原点へ置く
    UIElement, ///< Canvas 空間。祖先に Canvas が無ければ Canvas の下へ入れる
    Screen,    ///< 位置を持たない画面ルート (Canvas)。置き直さない
};

/// @brief 1 プリセットの定義。表に載せた時点でメニュー・Operator・AI の全面に現れる。
struct ObjectPreset {
    std::string_view id;          ///< "3d.cube" 形式の安定 ID。label を変えても変わらない
    std::string_view category;    ///< メニューの見出し。空なら Create 直下
    std::string_view label;       ///< メニュー表示名
    std::string_view description; ///< 何が付くか。AI がプリセットを選ぶ根拠になる

    /// @brief ctx.activeScene 上に作ってルートを返す。
    /// @return nullptr は «GameObject を作らずシーン設定 (SceneEnvironment) だけを変えた» を意味する。
    /// @note 親付け・命名・配置・選択・Undo は ObjectCreation.hpp が行うので、ここではしない。
    scene::GameObject* (*create)(EditorContext& ctx);

    PresetPlacement placement = PresetPlacement::World;
};

/// @brief 登録順のプリセット一覧。
/// @note 同じ category は連続して並ぶ (メニューはこの順に見出しを積む)。
[[nodiscard]] std::span<const ObjectPreset> ObjectPresetCatalog();

/// @return 見つからなければ nullptr。
[[nodiscard]] const ObjectPreset* FindObjectPreset(std::string_view id);

} // namespace fbzz::editor
