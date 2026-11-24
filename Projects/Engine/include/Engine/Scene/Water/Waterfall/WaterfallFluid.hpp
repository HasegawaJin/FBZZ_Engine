/// @file    WaterfallFluid.hpp
/// @brief   滝のローカルベイクに使う接続水面と衝突入力を採取する。
/// @author  Hasegawa Jin
/// @date    2026-11-11
#pragma once
#include <Engine/Api.hpp>
#include <string>

namespace fbzz::asset { struct WaterfallFluidBakeInput; }
namespace fbzz::scene {
class GameObject;
class Scene;

/// @note メインスレッドで採取する。結果は衝突データを所有し、ワーカーへ移動できる。
[[nodiscard]] FBZZ_ENGINE_API bool CaptureWaterfallFluidBake(Scene& scene, GameObject& owner,
    asset::WaterfallFluidBakeInput& out, std::string& error);
}
