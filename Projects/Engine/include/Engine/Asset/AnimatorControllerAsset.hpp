/// @file    AnimatorControllerAsset.hpp
/// @brief   Animator のステートマシン定義を共有アセットとして保存・復元する。
/// @author  Hasegawa Jin
/// @date    2026-06-13
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Asset/AnimatorControllerDefinition.hpp>
namespace fbzz::scene { struct AnimatorComponent; }
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::asset {

struct AnimatorGraphLayout {
    struct Vec2 {
        float x = 0.0f;
        float y = 0.0f;
    };

    Vec2 entryPosition    = { -220.0f, 80.0f };
    Vec2 anyStatePosition = { -220.0f, 260.0f };
    /// @brief Slot 疑似ノード (レイヤーへ差し込むワンショット再生の表示)。Base Layer には出ない。
    Vec2 slotPosition     = { -220.0f, 440.0f };
    std::unordered_map<std::string, Vec2> nodePositions;
    std::unordered_map<std::string, std::vector<Vec2>> blendTreeMotionPositions;
};

/// @brief AnimatorComponent のランタイム状態を除いた Controller 定義。
/// @note 複数 GameObject が同じ遷移グラフを参照し、Hierarchy 選択なしで編集できる。
struct AnimatorControllerAsset {
    std::string                             defaultStateName;
    std::vector<AnimationStateDefinition>      states;
    std::vector<scene::AnimationTransition> anyStateTransitions;
    std::vector<scene::AnimatorParameter>   parameters;
    std::vector<AnimationLayerDefinition>      layers;
    /// @brief Base Layer 自身のマスク (.mask アセットパス)。空なら全身。
    std::string                             baseLayerMaskPath;
    /// @brief Editor 専用のノード配置。ランタイム Animator には適用しない。
    /// @note Controller アセット単体でグラフ編集状態を完結させる。
    AnimatorGraphLayout                     editorLayout;
};

[[nodiscard]] FBZZ_ENGINE_API bool LoadAnimatorControllerAsset(const std::string& path,
                                               AnimatorControllerAsset& outAsset);
[[nodiscard]] FBZZ_ENGINE_API bool SaveAnimatorControllerAsset(const std::string& path,
                                               const AnimatorControllerAsset& asset);
FBZZ_ENGINE_API void ApplyAnimatorControllerAsset(const AnimatorControllerAsset& asset,
                                  scene::AnimatorComponent& animator);
[[nodiscard]] FBZZ_ENGINE_API AnimatorControllerAsset MakeAnimatorControllerAsset(
    const scene::AnimatorComponent& animator);

} /// @note namespace fbzz::asset
