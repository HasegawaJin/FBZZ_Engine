// FBZZ Engine
// ScriptColliderProxy.hpp | fbzz::scene
// Script から ColliderComponent を安全に更新するショートハンド
#pragma once

#include <Math/Vector3.hpp>
#include <string>
#include <string_view>

namespace fbzz::scene {

class Script;

struct ScriptColliderProxy {
    Script* script = nullptr;

    void SetEnabled(bool enabled) const;
    void SetTrigger(bool trigger) const;
    void SetCenter(const math::Vector3& center) const;
    void SetBoxSize(const math::Vector3& size) const;
    void SetSphereRadius(float radius) const;
    void SetCapsule(float radius, float halfHeight) const;
    void SetMesh(std::string_view meshPath, int meshIndex = 0) const;

    // ── 物理マテリアル ────────────────────────────────────────────────────
    // 個別の値を直接いじる版。共有 .physmat を参照していた場合はその参照を外し、
    // このコライダー専用の値へ切り替える。
    //
    // WHY 参照を外すか: 参照を残したまま値だけ書くと、次のフレームで共有アセットの値に
    //     上書きされ「設定したのに効かない」という追いにくい症状になる。
    //     1 体だけ滑らせたいなら参照を持たないのが正しい状態。
    void SetFriction(float staticFriction, float dynamicFriction) const;
    void SetRestitution(float restitution) const;
    void SetDensity(float density) const;

    // 共有 .physmat を割り当てる。空文字で参照を外す (直前の値はそのまま残る)。
    //
    //   collider.SetPhysicsMaterial("Assets/Physics/Ice.physmat");
    void SetPhysicsMaterial(std::string_view assetPath) const;
    [[nodiscard]] std::string GetPhysicsMaterial() const;

    // 現在の実効値 (共有アセット参照時は解決後の値)。
    [[nodiscard]] float GetRestitution() const;
    [[nodiscard]] float GetStaticFriction() const;
    [[nodiscard]] float GetDynamicFriction() const;
    [[nodiscard]] float GetDensity() const;

    // 組み込みプリセットを適用する ("Default" / "Rubber" / "Ice" / "Metal" / "Wood" / "Stone")。
    // 未知の名前なら false。共有参照は外れる。
    bool ApplyPhysicsMaterialPreset(std::string_view presetName) const;
};

} // namespace fbzz::scene
