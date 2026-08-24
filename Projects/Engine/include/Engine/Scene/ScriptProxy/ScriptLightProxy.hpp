// FBZZ Engine
// ScriptLightProxy.hpp | fbzz::scene
// Script から LightComponent を操作するショートハンド
#pragma once

#include <Math/Vector3.hpp>

namespace fbzz::scene {

class Script;

enum class LightType { Directional, Point, Spot };

struct ScriptLightProxy {
    Script* script = nullptr;

    void SetColor(const math::Vector3& color) const;
    void SetType(LightType type) const;
    void SetIntensity(float intensity) const;
    void SetRange(float range) const;
    void SetInnerCone(float degrees) const;
    void SetOuterCone(float degrees) const;
    void SetEnabled(bool enabled) const;

    // オーサリング値を起点に増減させる演出 (点滅・フェード・被弾時のフラッシュ) は、
    // 元の値を Script 側に控えておかなくても済むようここから読む。
    // LightComponent が無ければ既定値を返す。
    [[nodiscard]] math::Vector3 GetColor() const;
    [[nodiscard]] LightType     GetType() const;
    [[nodiscard]] float         GetIntensity() const;
    [[nodiscard]] float         GetRange() const;
    [[nodiscard]] float         GetInnerCone() const;
    [[nodiscard]] float         GetOuterCone() const;
    [[nodiscard]] bool          IsEnabled() const;
};

} // namespace fbzz::scene
