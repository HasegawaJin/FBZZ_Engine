/// @file    ScriptColliderProxy.hpp
/// @brief   Script から ColliderComponent を安全に更新するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-26
#pragma once
#include <Engine/Api.hpp>

#include <Math/Vector3.hpp>
#include <string>
#include <string_view>

namespace fbzz::scene {

class Script;
class GameObject;

struct ScriptColliderProxy {
    Script* script = nullptr;

    FBZZ_ENGINE_API void SetEnabled(bool enabled) const;
    FBZZ_ENGINE_API void SetTrigger(bool trigger) const;
    FBZZ_ENGINE_API void SetCenter(const math::Vector3& center) const;

    /// @note コライダーが付いていなければ IsEnabled / IsTrigger は false、GetCenter は原点。
    [[nodiscard]] FBZZ_ENGINE_API bool          HasCollider() const;
    [[nodiscard]] FBZZ_ENGINE_API bool          IsEnabled() const;
    [[nodiscard]] FBZZ_ENGINE_API bool          IsTrigger() const;
    [[nodiscard]] FBZZ_ENGINE_API math::Vector3 GetCenter() const;

    /// @note プリミティブ形状のワールド AABB。無効状態も含め、同一オブジェクトの形状を統合する。
    /// @note 回転・中心・負スケールを反映する。対象不在・未対応は false、出力は未変更。
    /// @note 物理実体は変更しない。Transform の world 値が更新済みであること。
    [[nodiscard]] FBZZ_ENGINE_API static bool TryGetPrimitiveWorldBounds(GameObject* object,
        math::Vector3& outMin, math::Vector3& outMax);

    FBZZ_ENGINE_API void SetBoxSize(const math::Vector3& size) const;
    FBZZ_ENGINE_API void SetSphereRadius(float radius) const;
    FBZZ_ENGINE_API void SetCapsule(float radius, float halfHeight) const;
    FBZZ_ENGINE_API void SetCylinder(float radius, float halfHeight) const;
    FBZZ_ENGINE_API void SetMesh(std::string_view meshPath, int meshIndex = 0) const;

    /// @name 物理マテリアル
    /// @note 個別の値を直接いじる版。共有 .physmat を参照していた場合はその参照を外し、
    /// @note このコライダー専用の値へ切り替える。
    /// @note 参照を残したまま値だけ書くと、次のフレームで共有アセットの値に上書きされ
    /// @note 「設定したのに効かない」症状になる。1 体だけ滑らせたいなら参照を外すのが正しい。
    /// @{
    FBZZ_ENGINE_API void SetFriction(float staticFriction, float dynamicFriction) const;
    FBZZ_ENGINE_API void SetRestitution(float restitution) const;
    FBZZ_ENGINE_API void SetDensity(float density) const;

    /// @brief 共有 .physmat を割り当てる。空文字で参照を外す (直前の値はそのまま残る)。
    FBZZ_ENGINE_API void SetPhysicsMaterial(std::string_view assetPath) const;
    [[nodiscard]] FBZZ_ENGINE_API std::string GetPhysicsMaterial() const;

    /// @name 実効値 (共有アセット参照時は解決後の値)
    /// @{
    [[nodiscard]] FBZZ_ENGINE_API float GetRestitution() const;
    [[nodiscard]] FBZZ_ENGINE_API float GetStaticFriction() const;
    [[nodiscard]] FBZZ_ENGINE_API float GetDynamicFriction() const;
    [[nodiscard]] FBZZ_ENGINE_API float GetDensity() const;
    /// @}

    /// @brief 組み込みプリセットを適用する ("Default" / "Rubber" / "Ice" / "Metal" / "Wood" / "Stone")。
    /// @return 未知の名前なら false。共有参照は外れる。
    FBZZ_ENGINE_API bool ApplyPhysicsMaterialPreset(std::string_view presetName) const;
    /// @}
};

} /// @note namespace fbzz::scene
