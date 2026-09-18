/// @file    ConstraintComponents.hpp
/// @brief   Bone Socket追従とTransform制約をGameObject間で再利用するComponent。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <string>

namespace fbzz::scene {

struct SocketAttachmentComponent {
    bool enabled = true;
    /// 追従先を含む GameObject。省略可 (無効な EntityRef のままでよい)。
    ///
    /// @note EntityRef は Prefab に保存できないため、必須にすると Prefab 自身が追従を宣言できない。
    ///       省略時は ConstraintSystem が自分の祖先を根へたどり最初に見つかった socketName を使う。
    EntityRef target;
    /// 追従先ソケット。GameObject 名か BoneComponent::boneName で引く。
    /// target 指定時はその部分木から、省略時は自分の祖先方向から探す。
    std::string socketName;
    /// 自分側の合わせ点。空なら自分の原点が socketName に一致する (従来動作)。
    ///
    /// @note positionOffset への手打ちはモデル修正のたびに古くなる。合わせ点をモデル側の
    ///       ソケットで宣言すれば FBX を直した瞬間に正しい位置へ追従する。
    std::string localSocketName;
    math::Vector3 positionOffset = math::Vector3::ZERO;
    math::Vector3 rotationOffsetDegrees = math::Vector3::ZERO;
    math::Vector3 scaleMultiplier = math::Vector3::ONE;
    bool followPosition = true;
    bool followRotation = true;
    bool followScale = false;

    /// socketName を書き換えたとき、旧ソケットから新ソケットへこの秒数かけて移る。
    /// 0 なら即座にスナップする (従来動作)。
    ///
    /// @note 両ソケットの「その瞬間の」姿勢を混ぜられるのは Animator / IK の後に走る
    ///       ConstraintSystem だけなので、補間はここで完結させる (Script 側では静止ポーズを
    ///       captureしがちで、歩行中に置き去りになる)。
    float blendDuration = 0.0f;

    /// @name ランタイム専用 (シリアライズしない)
    /// @{
    /// 直近フレームで実際に適用したソケット名。socketName との差分が「切り替え」。
    std::string appliedSocketName;
    /// 補間元のソケット名。空なら補間せずスナップする。
    std::string blendFromSocketName;
    float       blendRemaining = 0.0f;

    const char* GetTypeName() const { return "Socket Attachment"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("target", target);
        r.Field("socketName", socketName);
        r.Field("localSocketName", localSocketName);
        r.Field("positionOffset", positionOffset);
        r.Field("rotationOffsetDegrees", rotationOffsetDegrees);
        r.Field("scaleMultiplier", scaleMultiplier);
        r.Field("followPosition", followPosition);
        r.Field("followRotation", followRotation);
        r.Field("followScale", followScale);
        r.FloatRange("blendDuration", blendDuration, 0.0f, 2.0f);
    }
    /// @}
};

enum class TransformConstraintMode : int {
    Parent = 0, Position = 1, Rotation = 2, Scale = 3, Aim = 4, LookAt = 5
};

struct TransformConstraintComponent {
    bool enabled = true;
    EntityRef target;
    TransformConstraintMode mode = TransformConstraintMode::Parent;
    float weight = 1.0f;
    math::Vector3 positionOffset = math::Vector3::ZERO;
    math::Vector3 rotationOffsetDegrees = math::Vector3::ZERO;
    math::Vector3 scaleMultiplier = math::Vector3::ONE;
    math::Vector3 aimAxis = math::Vector3::FORWARD;
    math::Vector3 upAxis = math::Vector3::UP;
    bool maintainOffset = true;

    const char* GetTypeName() const { return "Transform Constraint"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("target", target);
        int value = static_cast<int>(mode);
        static constexpr const char* MODES[] = {
            "Parent", "Position", "Rotation", "Scale", "Aim", "Look At"
        };
        r.Enum("mode", value, MODES);
        mode = static_cast<TransformConstraintMode>(value < 0 || value > 5 ? 0 : value);
        r.FloatRange("weight", weight, 0.0f, 1.0f);
        r.Field("positionOffset", positionOffset);
        r.Field("rotationOffsetDegrees", rotationOffsetDegrees);
        r.Field("scaleMultiplier", scaleMultiplier);
        r.Field("aimAxis", aimAxis);
        r.Field("upAxis", upAxis);
        r.Field("maintainOffset", maintainOffset);
    }
};

} // namespace fbzz::scene
