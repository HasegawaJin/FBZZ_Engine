/// @file    ScriptReflectionProbeProxy.hpp
/// @brief   Script から ReflectionProbeComponent を操作するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-23

/// @note 影響半径や強度をランタイムで変化させてポータル・水面反射の切り替えなどに使う。
#pragma once
#include <Engine/Api.hpp>

#include <Math/Vector3.hpp>
#include <string_view>

namespace fbzz::scene {

class Script;

struct ScriptReflectionProbeProxy {
    Script* script = nullptr;

    FBZZ_ENGINE_API void SetEnabled(bool enabled) const;
    FBZZ_ENGINE_API void SetIntensity(float intensity) const;

    /// @note 球影響半径 [m]。カメラがこの距離内に入るとプローブが適用される。
    FBZZ_ENGINE_API void SetInfluenceRadius(float radius) const;

    /// @note ボックス形状影響範囲に切り替える。
    FBZZ_ENGINE_API void SetBoxInfluence(bool useBox) const;
    /// @note ボックス半径 [m] を設定する (SetBoxInfluence(true) と合わせて使う)。
    FBZZ_ENGINE_API void SetBoxExtents(const math::Vector3& halfExtents) const;

    /// @note 静的環境マップを差し替える (.dds)。
    FBZZ_ENGINE_API void SetCubemap(std::string_view path) const;

    [[nodiscard]] FBZZ_ENGINE_API bool  IsEnabled() const;
    [[nodiscard]] FBZZ_ENGINE_API float GetIntensity() const;
    [[nodiscard]] FBZZ_ENGINE_API float GetInfluenceRadius() const;
};

} /// @note namespace fbzz::scene
