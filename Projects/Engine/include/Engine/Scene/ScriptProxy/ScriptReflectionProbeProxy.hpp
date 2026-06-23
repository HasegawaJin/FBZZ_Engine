// FBZZ Engine
// ScriptReflectionProbeProxy.hpp | fbzz::scene
// Script から ReflectionProbeComponent を操作するショートハンド。
// 影響半径や強度をランタイムで変化させてポータル・水面反射の切り替えなどに使う。
#pragma once

#include <Math/Vector3.hpp>
#include <string_view>

namespace fbzz::scene {

class Script;

struct ScriptReflectionProbeProxy {
    Script* script = nullptr;

    void SetEnabled(bool enabled) const;
    void SetIntensity(float intensity) const;

    // 球影響半径 [m]。カメラがこの距離内に入るとプローブが適用される。
    void SetInfluenceRadius(float radius) const;

    // ボックス形状影響範囲に切り替える。
    void SetBoxInfluence(bool useBox) const;
    // ボックス半径 [m] を設定する (SetBoxInfluence(true) と合わせて使う)。
    void SetBoxExtents(const math::Vector3& halfExtents) const;

    // 静的環境マップを差し替える (.dds)。
    void SetCubemap(std::string_view path) const;
};

} // namespace fbzz::scene
