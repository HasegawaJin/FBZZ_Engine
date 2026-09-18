/// @file    Scenes.hpp
/// @brief   ベンチ場面の生成関数。BenchApp が並べる順もここが正本。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#pragma once

#include "../BenchScene.hpp"

#include <memory>
#include <vector>

namespace fbzz::bench {

std::unique_ptr<BenchScene> MakeXPBDConvergenceScene();
std::unique_ptr<BenchScene> MakeXPBDJointChainScene();
std::unique_ptr<BenchScene> MakeContactScene();
std::unique_ptr<BenchScene> MakeBVHQueryScene();
std::unique_ptr<BenchScene> MakeCCDScene();
std::unique_ptr<BenchScene> MakeConvexHullScene();
std::unique_ptr<BenchScene> MakeRagdollScene();

/// @brief 全場面を並べ順どおりに生成する。
/// @note 画面の一覧と --measure の行順を揃えるため、並べ順はここだけで決める。
inline std::vector<std::unique_ptr<BenchScene>> MakeAllScenes()
{
    std::vector<std::unique_ptr<BenchScene>> scenes;
    scenes.push_back(MakeXPBDConvergenceScene());
    scenes.push_back(MakeXPBDJointChainScene());
    scenes.push_back(MakeContactScene());
    scenes.push_back(MakeBVHQueryScene());
    scenes.push_back(MakeCCDScene());
    scenes.push_back(MakeConvexHullScene());
    scenes.push_back(MakeRagdollScene());
    return scenes;
}

} // namespace fbzz::bench
