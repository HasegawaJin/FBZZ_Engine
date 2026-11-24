/// @file    ScenePhysicsCodec.hpp
/// @brief   コライダー・剛体の保存と旧物理設定の復元。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once
#include <toml++/toml.hpp>

namespace fbzz::scene {

class GameObject;

namespace scene_codec {

void WritePhysicsComponents(GameObject& go, toml::table& goTbl);
void ReadColliders(GameObject& go, const toml::table& object);
void ReadRigidBody(GameObject& go, const toml::table& object);
void ReadPhysicsComponents(GameObject& go, const toml::table& object);

} // namespace scene_codec
} // namespace fbzz::scene
