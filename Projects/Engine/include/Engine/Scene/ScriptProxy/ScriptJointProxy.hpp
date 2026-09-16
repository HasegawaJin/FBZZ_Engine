/// @file    ScriptJointProxy.hpp
/// @brief   Script から剛体の関節 (ロープ・鎖・ヒンジ等) を張り、切るプロキシ
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// Connect*() はこの GameObject の JointComponent を組み替える。無ければ追加する。
/// Inspector で置いた関節と区別しないので、同じ口で «後から付け替える» ができる。
///
/// WHY Constraint を直に作らせないか: 制約の実体は physics::World が所有し、
///     寿命は «毎フレーム申告» で決まる。スクリプトが直接 World へ足すと、
///     シーンを切り替えた瞬間に消えた剛体を指した制約が残る。関節は必ず
///     コンポーネントとして宣言し、張るのは PhysicsSystem に任せる。
#pragma once

#include <Math/Vector3.hpp>
#include <vector>

namespace fbzz::scene {

class GameObject;
class Script;

struct ScriptJointProxy {
    Script* script = nullptr;

    /// 相対位置と相対回転を固定する (溶接)。呼んだ瞬間の相対姿勢を保つ。
    void ConnectFixed(GameObject* target) const;
    /// 距離を保つ棒。distance <= 0 なら呼んだ瞬間の間隔を採る。
    void ConnectDistance(GameObject* target, float distance = 0.0f) const;
    /// 最大距離だけを拘束するロープ。maxLength <= 0 なら呼んだ瞬間の間隔を採る。
    void ConnectRope(GameObject* target, float maxLength = 0.0f) const;
    /// バネ。restLength <= 0 なら呼んだ瞬間の間隔を自然長に採る。
    void ConnectSpring(GameObject* target,
                       float restLength,
                       float stiffness,
                       float damping) const;
    /// 蝶番。anchor / connectedAnchor はそれぞれのローカル座標で、この 2 点を重ねる。
    void ConnectHinge(GameObject* target,
                      const math::Vector3& axis,
                      const math::Vector3& anchor = math::Vector3::ZERO,
                      const math::Vector3& connectedAnchor = math::Vector3::ZERO) const;
    /// 1 軸方向にだけ動く拘束。
    void ConnectSlider(GameObject* target, const math::Vector3& axis) const;
    /// 自分を先頭にした数珠つなぎ。following には 2 節目以降を順に並べる。
    /// segmentLength <= 0 なら自分と 2 節目の間隔を節間距離に採る。
    void ConnectChain(const std::vector<GameObject*>& following,
                      float segmentLength = 0.0f,
                      int iterations = 4) const;

    /// 関節を切る (JointComponent を外す)。物理側の制約は次の物理更新で解放される。
    void Break() const;
    /// JointComponent が付いているか。
    [[nodiscard]] bool HasJoint() const;
    /// 実際に物理へ張れているか。
    ///
    /// 相手が見つからない・相手に剛体が無い・どちらかが無効のときは false。
    /// «繋いだのに垂れない» の原因切り分けはまずこれを見る。
    [[nodiscard]] bool IsConnected() const;

    void SetEnabled(bool enabled) const;
    [[nodiscard]] bool IsEnabled() const;

    /// Hinge = 角度 [degrees] / Slider = 軸方向の距離 [m] の可動域。
    void SetLimits(float lower, float upper) const;
    void ClearLimits() const;
    /// Hinge のモーター。目標角速度 [degrees/s] と上限トルク。
    void SetMotor(float targetSpeed, float maxTorque) const;
    void ClearMotor() const;

    /// 張るときに使う距離 [m]。0 以下を渡すと «張った瞬間の間隔» へ戻る。
    void SetDistance(float distance) const;
    /// 実際に張られている距離 [m] (自動で採った値を含む)。
    [[nodiscard]] float GetDistance() const;
    /// 相手との今の間隔 [m]。相手が居なければ 0。
    [[nodiscard]] float GetCurrentDistance() const;
};

} // namespace fbzz::scene
