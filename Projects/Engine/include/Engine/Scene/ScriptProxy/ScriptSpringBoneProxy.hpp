/// @file    ScriptSpringBoneProxy.hpp
/// @brief   Script から SpringBoneComponent のチェーンを組み、外力を与えるプロキシ
/// @author  Hasegawa Jin
/// @date    2026-09-01

/// @note チェーンは根ボーン名で引く。Inspector で組んだものと Script が組んだものを
/// @note 区別しないので、同じ根を指定すれば既存のチェーンがそのまま設定対象になる。
#pragma once
#include <Engine/Api.hpp>

#include <Math/Vector3.hpp>
#include <string_view>

namespace fbzz::scene {

class Script;

struct ScriptSpringBoneProxy {
    Script* script = nullptr;

    /// @note 根ボーン名のチェーンが無ければ作る。あれば maxDepth だけ更新して何もしない。
    /// @note SpringBoneComponent 自体が無ければ追加する。
    FBZZ_ENGINE_API void EnsureChain(std::string_view rootBoneName, int maxDepth = 0) const;
    [[nodiscard]] FBZZ_ENGINE_API bool HasChain(std::string_view rootBoneName) const;

    FBZZ_ENGINE_API void SetChainEnabled(std::string_view rootBoneName, bool enabled) const;
    [[nodiscard]] FBZZ_ENGINE_API bool IsChainEnabled(std::string_view rootBoneName) const;

    /// @note 揺れの適用率 [0,1]。0 で完全に FK ポーズ、1 でシミュレーション結果そのまま。
    FBZZ_ENGINE_API void SetWeight(std::string_view rootBoneName, float weight) const;
    [[nodiscard]] FBZZ_ENGINE_API float GetWeight(std::string_view rootBoneName) const;

    /// @note 静止姿勢へ戻ろうとする強さ [0,1] と速度の減衰 [0,1]。
    FBZZ_ENGINE_API void SetSpring(std::string_view rootBoneName, float stiffness, float damping) const;
    /// @note 静止方向からの最大ふれ角 [degrees]。0 で無制限。
    FBZZ_ENGINE_API void SetLimitAngle(std::string_view rootBoneName, float degrees) const;
    /// @note 揺れボーン側の衝突半径 [m]。
    FBZZ_ENGINE_API void SetRadius(std::string_view rootBoneName, float radius) const;

    /// @note チェーンに掛かる外力。重力の口をそのまま向きと強さで使う。

    /// @note 磁力のように毎フレーム向きが変わる力はここへ書く。power が 0 なら向きは無視される。
    FBZZ_ENGINE_API void SetForce(std::string_view rootBoneName,
                  const math::Vector3& direction,
                  float power) const;
    FBZZ_ENGINE_API void ClearForce(std::string_view rootBoneName) const;

    /// @note 全チェーンを静止姿勢へ戻す。ワープや状態の切り替えで慣性を捨てたいときに使う。
    FBZZ_ENGINE_API void ResetAll() const;

    FBZZ_ENGINE_API void SetEnabled(bool enabled) const;
    [[nodiscard]] FBZZ_ENGINE_API bool IsEnabled() const;
};

} /// @note namespace fbzz::scene
