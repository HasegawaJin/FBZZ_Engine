/// @file    ScriptSpringBoneProxy.hpp
/// @brief   Script から SpringBoneComponent のチェーンを組み、外力を与えるプロキシ
/// @author  Hasegawa Jin
/// @date    2026-09-01
///
/// チェーンは根ボーン名で引く。Inspector で組んだものと Script が組んだものを
/// 区別しないので、同じ根を指定すれば既存のチェーンがそのまま設定対象になる。
#pragma once

#include <Math/Vector3.hpp>
#include <string_view>

namespace fbzz::scene {

class Script;

struct ScriptSpringBoneProxy {
    Script* script = nullptr;

    /// 根ボーン名のチェーンが無ければ作る。あれば maxDepth だけ更新して何もしない。
    /// SpringBoneComponent 自体が無ければ追加する。
    void EnsureChain(std::string_view rootBoneName, int maxDepth = 0) const;
    [[nodiscard]] bool HasChain(std::string_view rootBoneName) const;

    void SetChainEnabled(std::string_view rootBoneName, bool enabled) const;
    [[nodiscard]] bool IsChainEnabled(std::string_view rootBoneName) const;

    /// 揺れの適用率 [0,1]。0 で完全に FK ポーズ、1 でシミュレーション結果そのまま。
    void SetWeight(std::string_view rootBoneName, float weight) const;
    [[nodiscard]] float GetWeight(std::string_view rootBoneName) const;

    /// 静止姿勢へ戻ろうとする強さ [0,1] と速度の減衰 [0,1]。
    void SetSpring(std::string_view rootBoneName, float stiffness, float damping) const;
    /// 静止方向からの最大ふれ角 [degrees]。0 で無制限。
    void SetLimitAngle(std::string_view rootBoneName, float degrees) const;
    /// 揺れボーン側の衝突半径 [m]。
    void SetRadius(std::string_view rootBoneName, float radius) const;

    /// チェーンに掛かる外力。重力の口をそのまま向きと強さで使う。
    ///
    /// 磁力のように毎フレーム向きが変わる力はここへ書く。power が 0 なら向きは無視される。
    void SetForce(std::string_view rootBoneName,
                  const math::Vector3& direction,
                  float power) const;
    void ClearForce(std::string_view rootBoneName) const;

    /// 全チェーンを静止姿勢へ戻す。ワープや状態の切り替えで慣性を捨てたいときに使う。
    void ResetAll() const;

    void SetEnabled(bool enabled) const;
    [[nodiscard]] bool IsEnabled() const;
};

} // namespace fbzz::scene
