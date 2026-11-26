/// @file    ConstraintComponents.hpp
/// @brief   Bone Socket追従とTransform制約をGameObject間で再利用するComponent。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#pragma once

#include <Engine/Scene/References/EntityRef.hpp>
#include <Engine/Reflection/Reflection.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <string>

namespace fbzz::scene {

struct SocketAttachmentComponent {
    bool enabled = true; ///< 追従設定を毎フレームの ConstraintSystem で適用する。
    /// @note 追従先を含む GameObject。省略可 (無効な EntityRef のままでよい)。
    ///
    /// @note EntityRef は Prefab に保存できないため、必須にすると Prefab 自身が追従を宣言できない。
    /// @note 省略時は ConstraintSystem が自分の祖先を根へたどり最初に見つかった socketName を使う。
    EntityRef target; ///< 追従先ソケットを探す部分木のルート。未指定は自分の祖先を順に探す。
    /// @note 追従先ソケット。GameObject 名か BoneComponent::boneName で引く。
    /// @note target 指定時はその部分木から、省略時は自分の祖先方向から探す。
    std::string socketName; ///< 追従先のソケットを引くボーン名または GameObject 名。
    /// @note 自分側の合わせ点。空なら自分の原点が socketName に一致する (従来動作)。
    ///
    /// @note positionOffset への手打ちはモデル修正のたびに古くなる。合わせ点をモデル側の
    /// @note ソケットで宣言すれば FBX を直した瞬間に正しい位置へ追従する。
    std::string localSocketName; ///< 追従先に重ねる自分側のソケット名。空なら自分の原点を使う。
    math::Vector3 positionOffset = math::Vector3::ZERO; ///< 追従先の回転を基準に加える位置のずれ（m）。
    math::Vector3 rotationOffsetDegrees = math::Vector3::ZERO; ///< 追従先の回転の後に適用するローカルオイラー角（度）。
    math::Vector3 scaleMultiplier = math::Vector3::ONE; ///< 追従先のワールドスケールへ軸ごとに掛ける倍率。
    bool followPosition = true; ///< 位置を追従先へ合わせる。
    bool followRotation = true; ///< 回転を追従先へ合わせる。
    bool followScale = false; ///< スケールを追従先へ合わせる。

    /// @note socketName を書き換えたとき、旧ソケットから新ソケットへこの秒数かけて移る。
    /// @note 0 なら即座にスナップする (従来動作)。
    ///
    /// @note 両ソケットの「その瞬間の」姿勢を混ぜられるのは Animator / IK の後に走る
    /// @note ConstraintSystem だけなので、補間はここで完結させる (Script 側では静止ポーズを
    /// @note captureしがちで、歩行中に置き去りになる)。
    float blendDuration = 0.0f; ///< 追従先の名前を切り替えた際の補間時間（秒）。0 で即時切り替え。

    /// @name ランタイム専用 (シリアライズしない)
    /// @{
    /// @note 直近フレームで実際に適用したソケット名。socketName との差分が「切り替え」。
    std::string appliedSocketName; ///< 直近に適用した追従先名。socketName の切り替え検出に使う。
    std::string blendFromSocketName; ///< 補間元のソケット名。空なら補間せずスナップする。
    float       blendRemaining = 0.0f; ///< 旧ソケットと新ソケットの補間を終えるまでの残り時間（秒）。

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
    bool enabled = true; ///< この Transform 制約を ConstraintSystem で適用する。
    EntityRef target; ///< 姿勢または注視位置の参照元にするオブジェクト。
    TransformConstraintMode mode = TransformConstraintMode::Parent; ///< 追従先から位置・回転・スケールのどれを適用するか、または注視を選ぶ。
    float weight = 1.0f; ///< 現在姿勢から制約の目標へ近づける補間割合（0〜1）。
    math::Vector3 positionOffset = math::Vector3::ZERO; ///< 位置制約で対象の回転を基準に追加するずれ（m）。
    math::Vector3 rotationOffsetDegrees = math::Vector3::ZERO; ///< 回転制約で対象の回転の後に適用するオイラー角（度）。
    math::Vector3 scaleMultiplier = math::Vector3::ONE; ///< スケール制約で対象のワールドスケールへ掛ける軸別倍率。
    math::Vector3 aimAxis = math::Vector3::FORWARD; ///< Aim または LookAt で対象に向ける自分のローカル軸。
    math::Vector3 upAxis = math::Vector3::UP; ///< Aim または LookAt でねじれを決める自分のローカル上向き軸。
    bool maintainOffset = true; ///< 位置・回転のオフセットとスケール倍率を目標姿勢に適用する。

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

}
