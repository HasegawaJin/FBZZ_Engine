/// @file    AnimationClipHelpers.hpp
/// @brief   Animator にロード済みの Source と名前からクリップを解決する共通入口。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <Engine/Api.hpp>

#include <string>

namespace fbzz::asset {
struct AnimationClip;
}
namespace fbzz::scene {
struct AnimatorComponent;
}
namespace fbzz::scene::animation_clip_detail {

/// @return Source と名前が一致するクリップ。名前不一致なら同 Source の先頭、Source 不一致・空なら nullptr。
/// @note AssetManager または手動の値配列が所有する。アセット更新・再ロード・配列再確保を跨いで保持しない。
FBZZ_ENGINE_API const asset::AnimationClip* FindClipBySource(const AnimatorComponent& animator,
                                             const std::string& sourcePath,
                                             const std::string& clipName);

}
