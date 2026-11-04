/// @file    ScriptJointProxy.hpp
/// @brief   Script から剛体の関節 (ロープ・鎖・ヒンジ等) を張り、切るプロキシ
/// @author  Hasegawa Jin
/// @date    2026-09-12

/// @note Connect*() はこの GameObject の JointComponent を組み替える。無ければ追加する。
/// @note Inspector で置いた関節と区別しないので、同じ口で «後から付け替える» ができる。

/// @note 制約の実体は physics::World が所有し、寿命は «毎フレーム申告» で決まる。
/// @note スクリプトが直接 World へ足すと、シーンを切り替えた瞬間に消えた剛体を
/// @note 指した制約が残るため、関節は必ずコンポーネントとして宣言する。
#pragma once
#include <Engine/Api.hpp>

#include <Math/Vector3.hpp>
#include <vector>

namespace fbzz::scene {

class GameObject;
class Script;

struct ScriptJointProxy {
    Script* script = nullptr;

    /// @note 相対位置と相対回転を固定する (溶接)。呼んだ瞬間の相対姿勢を保つ。
    FBZZ_ENGINE_API void ConnectFixed(GameObject* target) const;
    /// @note 距離を保つ棒。distance <= 0 なら呼んだ瞬間の間隔を採る。
    FBZZ_ENGINE_API void ConnectDistance(GameObject* target, float distance = 0.0f) const;
    /// @note 最大距離だけを拘束するロープ。maxLength <= 0 なら呼んだ瞬間の間隔を採る。
    FBZZ_ENGINE_API void ConnectRope(GameObject* target, float maxLength = 0.0f) const;
    /// @note バネ。restLength <= 0 なら呼んだ瞬間の間隔を自然長に採る。
    FBZZ_ENGINE_API void ConnectSpring(GameObject* target,
                       float restLength,
                       float stiffness,
                       float damping) const;
    /// @note 蝶番。anchor / connectedAnchor はそれぞれのローカル座標で、この 2 点を重ねる。
    FBZZ_ENGINE_API void ConnectHinge(GameObject* target,
                      const math::Vector3& axis,
                      const math::Vector3& anchor = math::Vector3::ZERO,
                      const math::Vector3& connectedAnchor = math::Vector3::ZERO) const;
    /// @note 1 軸方向にだけ動く拘束。
    FBZZ_ENGINE_API void ConnectSlider(GameObject* target, const math::Vector3& axis) const;
    /// @note 自分を先頭にした数珠つなぎ。following には 2 節目以降を順に並べる。
    /// @note segmentLength <= 0 なら自分と 2 節目の間隔を節間距離に採る。
    FBZZ_ENGINE_API void ConnectChain(const std::vector<GameObject*>& following,
                      float segmentLength = 0.0f,
                      int iterations = 4) const;

    /// @note 関節を切る (JointComponent を外す)。物理側の制約は次の物理更新で解放される。
    FBZZ_ENGINE_API void Break() const;
    /// @note JointComponent が付いているか。
    [[nodiscard]] FBZZ_ENGINE_API bool HasJoint() const;
    /// @note 実際に物理へ張れているか。

    /// @note 相手が見つからない・相手に剛体が無い・どちらかが無効のときは false。
    /// @note «繋いだのに垂れない» の原因切り分けはまずこれを見る。
    [[nodiscard]] FBZZ_ENGINE_API bool IsConnected() const;

    FBZZ_ENGINE_API void SetEnabled(bool enabled) const;
    [[nodiscard]] FBZZ_ENGINE_API bool IsEnabled() const;

    /// @note Hinge = 角度 [degrees] / Slider = 軸方向の距離 [m] の可動域。
    FBZZ_ENGINE_API void SetLimits(float lower, float upper) const;
    FBZZ_ENGINE_API void ClearLimits() const;
    /// @note Hinge のモーター。目標角速度 [degrees/s] と上限トルク。
    FBZZ_ENGINE_API void SetMotor(float targetSpeed, float maxTorque) const;
    FBZZ_ENGINE_API void ClearMotor() const;

    /// @note 張るときに使う距離 [m]。0 以下を渡すと «張った瞬間の間隔» へ戻る。
    FBZZ_ENGINE_API void SetDistance(float distance) const;
    /// @note 実際に張られている距離 [m] (自動で採った値を含む)。
    [[nodiscard]] FBZZ_ENGINE_API float GetDistance() const;
    /// @note 相手との今の間隔 [m]。相手が居なければ 0。
    [[nodiscard]] FBZZ_ENGINE_API float GetCurrentDistance() const;
};

} /// @note namespace fbzz::scene
