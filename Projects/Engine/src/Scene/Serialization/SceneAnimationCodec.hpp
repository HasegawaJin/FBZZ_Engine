/// @file    SceneAnimationCodec.hpp
/// @brief   スケルトンと Animator の TOML 保存・復元。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once
#include <toml++/toml.hpp>

namespace fbzz::scene {

class GameObject;
class Scene;

namespace scene_codec {

void WriteSkeletonAndAnimator(GameObject& go, toml::table& goTbl, Scene& scene);
void ReadSkeletonComponents(GameObject& go, const toml::table& object, bool passiveScripts);
void ReadAnimatorComponent(GameObject& go, const toml::table& object);

} // namespace scene_codec
} // namespace fbzz::scene
