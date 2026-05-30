// FBZZ Engine
// ScriptSystem.hpp | fbzz::scene
// GameObject に付いたユーザースクリプトの実行 System
// Start / Update などのライフサイクルを Scene 全体に対して進める。
// Script の所有は ScriptComponent に残す。
#pragma once

namespace fbzz::scene {

class Scene;

void ScriptSystem(Scene& scene, float dt);

// PhysicsSystem 後に実行するスクリプト更新。カメラ追従など、物理適用後の位置を必要とする処理に使う。
void LateScriptSystem(Scene& scene, float dt);

} // namespace fbzz::scene
