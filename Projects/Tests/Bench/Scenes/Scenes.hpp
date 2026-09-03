/// @file    Scenes.hpp
/// @brief   ベンチ場面の生成関数。BenchApp が並べる順もここが正本。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#pragma once

#include "../BenchScene.hpp"

#include <memory>

namespace fbzz::bench {

std::unique_ptr<BenchScene> MakeXPBDConvergenceScene();
std::unique_ptr<BenchScene> MakeXPBDJointChainScene();
std::unique_ptr<BenchScene> MakeContactScene();
std::unique_ptr<BenchScene> MakeBVHQueryScene();

} // namespace fbzz::bench
