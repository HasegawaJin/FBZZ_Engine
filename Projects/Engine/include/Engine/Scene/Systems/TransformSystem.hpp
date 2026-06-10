// FBZZ Engine
// TransformSystem.hpp | fbzz::scene
// 親子階層からワールド Transform を再計算する System
// position / rotation / scale (ローカル) を元に worldPosition / worldRotation を更新する。
// GameObject の親子 ID をたどり、循環は作らない前提で処理する。
#pragma once

namespace fbzz::scene {
class Scene;

void TransformSystem(Scene& scene);
} // namespace fbzz::scene
