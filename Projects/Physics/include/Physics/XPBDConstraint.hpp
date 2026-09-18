/// @file    XPBDConstraint.hpp
/// @brief   XPBD の拘束インターフェースと、位置・角度補正の共通形
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// @note 既存の Constraint と別系統にする理由: Constraint は «積分の後に 1 回だけ位置を直す»
///       前提で速度を直さない。関節体を立たせたまま支えるには小さい substep を大量に回して
///       毎回解き直し、位置から速度を出し直す必要があり、同じ基底に乗せると両方が
///       中途半端になる。既存の Constraint はロープ・バネ用にそのまま残す。
/// @note 剛性ではなく compliance (剛性の逆数) で持つ理由: 位置射影を «1 ステップで詰める割合»
///       で書くと定常たわみが刻みの 2 乗に比例して変わる。compliance α なら定常たわみが
///       α·トルクになり刻みに依らない ─ 単位が [rad/(N·m)] の物理量になるため。
#pragma once

#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Physics/RigidBody.hpp>

namespace fbzz::physics
{
    /// XPBDSolver が substep ごとに解く拘束。
    class XPBDConstraint
    {
    public:
        virtual ~XPBDConstraint() = default;

        /// @brief substep のはじめに呼ばれる。蓄積した λ を捨てる。
        /// @note λ を跨いで持ち越すと «誤差を溜めて押し返す» 積分項になり、大きな誤差が続く
        ///       場面 (被弾のひるみ) でワインドアップして離した瞬間に跳ね返る。定常誤差は
        ///       compliance が決めるので溜める必要が無い。
        /// @note 純粋仮想にする理由: XPBDSolver は substep ごとに無条件で呼ぶ。空の既定実装を
        ///       置くと λ を持つ拘束を新規に足したとき実装し忘れても動いてしまい、
        ///       上のワインドアップだけが症状として出る。λ を持たない拘束は空実装を明示する。
        virtual void ResetLambda() = 0;

        /// 位置パス。h は substep の刻み [s]。
        virtual void SolvePosition(float h) = 0;

        /// 速度パス。位置から速度を出し直した «後» に呼ばれる。減衰・摩擦・反発はここ。
        virtual void SolveVelocity(float h) { (void)h; }
    };

    /// 回転ベクトル angular を h 倍して q へ積み、正規化する。
    /// h = 1 なら «回転補正をそのまま当てる» 意味になる。
    [[nodiscard]] math::Quaternion IntegrateRotation(const math::Quaternion& q,
                                                     const math::Vector3&    angular,
                                                     float                   h);

    /// 2 つの姿勢の差から角速度を出す。substep 末の速度更新で使う。
    [[nodiscard]] math::Vector3 AngularVelocityFromDelta(const math::Quaternion& previous,
                                                         const math::Quaternion& current,
                                                         float                   h);

    /// 一般化逆質量 w = 1/m + (r × n)ᵀ I⁻¹ (r × n)。
    /// r は重心からアンカーへのワールドベクトル。body が null / 静的なら 0。
    [[nodiscard]] float GeneralizedInverseMass(const RigidBody*     body,
                                               const math::Vector3& r,
                                               const math::Vector3& n);

    /// 位置拘束を 1 回解く。b に null を渡すと «動かない相手» になる (床・壁)。
    /// @param rA,rB     各剛体の重心からアンカーへのワールドベクトル。
    /// @param correction A のアンカーが B のアンカーからずれている量 [m]。**A はこの向きと逆へ動く。**
    /// @param compliance 0 で剛。単位は [m/N]。
    /// @param lambda    この substep で蓄積した λ。拘束が持ち、ResetLambda で捨てる。
    /// @param maxLambda λ の絶対値の上限 (0 以下で無制限)。クーロン摩擦は「接線の λ ≤ μ×法線の λ」なので、
    ///                  法線側の λ に μ を掛けて渡せば「滑り出すまでは止まる」がそのまま書ける。
    /// @return 今回加えた Δλ。力に直すと Δλ/h² [N]。
    float SolvePositional(RigidBody* a, RigidBody* b,
                          const math::Vector3& rA, const math::Vector3& rB,
                          const math::Vector3& correction,
                          float compliance, float h, float& lambda,
                          float maxLambda = 0.0f);

    /// 角度拘束を 1 回解く。b に null を渡すと «回らない相手» になる。
    /// @param correction 軸 × 角度 [rad]。**A はこの向きと逆へ回る。**
    /// @param compliance 0 で剛。単位は [rad/(N·m)]。
    /// @param maxLambda  λ の絶対値の上限 (0 以下で無制限)。トルク上限 τ_max [N·m] を掛けたいなら
    ///                   τ_max·h² を渡す (λ/h² がトルクなので λ を切ることがそのままトルクを切ることに
    ///                   なる。上限に当たった関節は目標へ追従できなくなり back-drive する)。
    /// @return 今回加えた Δλ。トルクに直すと Δλ/h² [N·m]。
    float SolveAngular(RigidBody* a, RigidBody* b,
                       const math::Vector3& correction,
                       float compliance, float h, float& lambda,
                       float maxLambda = 0.0f);

    /// 相対角速度を deltaOmega だけ変える。減衰・摩擦のような速度パスの補正で使う。
    /// a の角速度が -deltaOmega 側、b が +deltaOmega 側へ動く。
    ///
    /// @param maxImpulse 角力積の上限 [N·m·s]。0 以下で無制限。
    ///                   トルク上限 τ_max を掛けたいなら τ_max·h を渡す ─ 減衰も
    ///                   関節が出す «力» なので、位置パスと同じ上限に従わないと、
    ///                   «出せないはずのトルクで荷重を支える» ことになる。
    void ApplyAngularVelocityChange(RigidBody* a, RigidBody* b,
                                    const math::Vector3& deltaOmega,
                                    float maxImpulse = 0.0f);

    /// 接触点での相対速度を deltaV だけ変える。反発と動摩擦に使う。
    /// a の速度が -deltaV 側、b が +deltaV 側へ動く。b に null を渡すと «動かない相手»。
    ///
    /// @param rA,rB      各剛体の重心から接触点へのワールドベクトル。
    /// @param maxImpulse 力積の上限 [N·s]。0 以下で無制限。
    ///                   動摩擦を «法線力積 × μ» で切るために使う。
    void ApplyVelocityChangeAtPoint(RigidBody* a, RigidBody* b,
                                    const math::Vector3& rA, const math::Vector3& rB,
                                    const math::Vector3& deltaV,
                                    float maxImpulse = 0.0f);

    /// クォータニオンを «軸 × 角度» [rad] へ直す。常に短い方の弧を採る。
    [[nodiscard]] math::Vector3 RotationVector(const math::Quaternion& q);

    /// q を «X 軸まわりのツイスト» と «残りのスイング» へ分解する (q = swing * twist)。
    void DecomposeSwingTwist(const math::Quaternion& q,
                             math::Quaternion&       swing,
                             math::Quaternion&       twist);
} // namespace fbzz::physics
