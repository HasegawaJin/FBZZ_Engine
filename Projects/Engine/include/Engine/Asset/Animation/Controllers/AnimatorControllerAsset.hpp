/// @file    AnimatorControllerAsset.hpp
/// @brief   Animator のステートマシン定義を共有アセットとして保存・復元する。
/// @author  Hasegawa Jin
/// @date    2026-06-13
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Asset/Animation/Controllers/AnimatorControllerDefinition.hpp>
namespace fbzz::scene { struct AnimatorComponent; }
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::asset {

struct AnimatorGraphLayout {
    struct Vec2 {
        float x = 0.0f; ///< エディターのグラフ上の横座標。
        float y = 0.0f; ///< エディターのグラフ上の縦座標。
    };

    Vec2 entryPosition    = { -220.0f, 80.0f }; ///< グラフの Entry ノードの表示位置。
    Vec2 anyStatePosition = { -220.0f, 260.0f }; ///< グラフの Any State ノードの表示位置。
    Vec2 slotPosition     = { -220.0f, 440.0f }; ///< Slot 疑似ノード (レイヤーへ差し込むワンショット再生の表示)。Base Layer には出ない。
    std::unordered_map<std::string, Vec2> nodePositions; ///< 状態名で引くグラフノードの表示位置。
    std::unordered_map<std::string, std::vector<Vec2>> blendTreeMotionPositions; ///< ツリー名で引く各モーションノードの表示位置列。
};

/// @brief AnimatorComponent のランタイム状態を除いた Controller 定義。
/// @note 複数 GameObject が同じ遷移グラフを参照し、Hierarchy 選択なしで編集できる。
struct AnimatorControllerAsset {
    std::string                             defaultStateName; ///< ベースレイヤーの Entry から最初に遷移する状態名。
    std::vector<AnimationStateDefinition>      states; ///< ベースレイヤーで選択できる状態の制作データ。
    std::vector<scene::AnimationTransition> anyStateTransitions; ///< ベースレイヤーで現在状態に依存せず評価する遷移列。
    std::vector<scene::AnimatorParameter>   parameters; ///< 遷移条件とブレンドが名前で読むパラメーターの初期定義。
    std::vector<AnimationLayerDefinition>      layers; ///< ベースの上から重ねる追加アニメーションレイヤーの定義。
    std::string                             baseLayerMaskPath; ///< Base Layer 自身のマスク (.mask アセットパス)。空なら全身。
    /// @brief Editor 専用のノード配置。ランタイム Animator には適用しない。
    /// @note Controller アセット単体でグラフ編集状態を完結させる。
    AnimatorGraphLayout                     editorLayout; ///< エディターがグラフを再表示するためのノード位置。
};

[[nodiscard]] FBZZ_ENGINE_API bool LoadAnimatorControllerAsset(const std::string& path,
                                               AnimatorControllerAsset& outAsset);
[[nodiscard]] FBZZ_ENGINE_API bool SaveAnimatorControllerAsset(const std::string& path,
                                               const AnimatorControllerAsset& asset);
FBZZ_ENGINE_API void ApplyAnimatorControllerAsset(const AnimatorControllerAsset& asset,
                                  scene::AnimatorComponent& animator);
[[nodiscard]] FBZZ_ENGINE_API AnimatorControllerAsset MakeAnimatorControllerAsset(
    const scene::AnimatorComponent& animator);

} // namespace fbzz::asset
