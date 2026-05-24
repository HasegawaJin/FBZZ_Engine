// FBZZ Engine
// ScriptSystem.hpp | fbzz::scene
// GameObject に付いたユーザースクリプトの実行 System
// Start / Update などのライフサイクルを Scene 全体に対して進める。
// Script の所有は ScriptComponent に残す。
#pragma once

namespace fbzz::scene {

class Scene;

void ScriptSystem(Scene& scene, float dt);

} // namespace fbzz::scene
