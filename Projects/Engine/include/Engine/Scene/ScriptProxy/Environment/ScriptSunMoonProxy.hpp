/// @file    ScriptSunMoonProxy.hpp
/// @brief   Script から太陽・月ディスクの描画設定を操作するプロキシ。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once
#include <Engine/Api.hpp>

#include <Math/LinearAlgebra/Vector3.hpp>

namespace fbzz::scene {

class Script;

struct ScriptSunMoonProxy {
    Script* script = nullptr; ///< 太陽・月操作で実行スコープを照合する、非所有の Script 参照。

    FBZZ_ENGINE_API void SetEnabled(bool enabled) const;
    FBZZ_ENGINE_API void SetSun(bool enabled, float intensity) const;
    FBZZ_ENGINE_API void SetMoon(bool enabled, float size, float brightness,
                 const math::Vector3& color) const;
};

} // namespace fbzz::scene
