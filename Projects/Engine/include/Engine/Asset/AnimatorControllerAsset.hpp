// FBZZ Engine
// AnimatorControllerAsset.hpp | fbzz::asset
// Animator のステートマシン定義を共有アセットとして保存・復元する
#pragma once

#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <string>

namespace fbzz::asset {

// AnimatorComponent のランタイム状態を除いた Controller 定義。
// WHY: 複数 GameObject が同じ遷移グラフを参照し、Hierarchy 選択なしで編集できるようにする。
struct AnimatorControllerAsset {
    // version 1 読込互換用。version 2 以降は State / Motion の sourcePath を使用する。
    std::vector<std::string>                clipSources;
    std::string                             defaultStateName;
    std::vector<scene::AnimationState>      states;
    std::vector<scene::AnimationTransition> anyStateTransitions;
    std::vector<scene::AnimatorParameter>   parameters;
};

[[nodiscard]] bool LoadAnimatorControllerAsset(const std::string& path,
                                               AnimatorControllerAsset& outAsset);
[[nodiscard]] bool SaveAnimatorControllerAsset(const std::string& path,
                                               const AnimatorControllerAsset& asset);
void ApplyAnimatorControllerAsset(const AnimatorControllerAsset& asset,
                                  scene::AnimatorComponent& animator);
[[nodiscard]] AnimatorControllerAsset MakeAnimatorControllerAsset(
    const scene::AnimatorComponent& animator);

} // namespace fbzz::asset
