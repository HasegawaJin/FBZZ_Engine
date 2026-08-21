// FBZZ Engine
// ConstraintComponents.hpp | fbzz::scene
// Bone Socket追従とTransform制約をGameObject間で再利用するComponent
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <string>

namespace fbzz::scene {

struct SocketAttachmentComponent {
    bool enabled = true;
    // 追従先を含む GameObject。省略可 (無効な EntityRef のままでよい)。
    //
    // WHY 省略できるか:
    //   EntityRef は「シーン内の特定 GameObject」への参照なので、Prefab には保存できない。
    //   必須にすると「武器 Prefab 自身が追従の宣言を持つ」ことが原理的に成立せず、
    //   宣言をスクリプトが Play 開始時に組み立てるしかなくなる。そうすると編集中は
    //   追従が成立せず、エディタと再生で配置が食い違う。
    //   省略した場合、ConstraintSystem は自分の祖先を根へ向かってたどり、最初に見つかった
    //   socketName を追従先にする。ソケットに付くものは必ずそのソケットを含む階層の中に
    //   置かれるので、この探索で十分に一意が取れる。
    EntityRef target;
    // 追従先ソケット。GameObject 名か BoneComponent::boneName で引く。
    // target 指定時はその部分木から、省略時は自分の祖先方向から探す。
    std::string socketName;
    // 自分側の合わせ点。空なら自分の原点が socketName に一致する (従来動作)。
    //
    // WHY 必要か:
    //   銃を手に持たせるとき、一致させたいのは「銃の原点」ではなく「銃のグリップ」。
    //   原点追従のままだと、グリップが原点から離れているぶん手からズレる。ズレを
    //   positionOffset へ手で打ち込むと、モデルを修正するたびに数値が古くなり、
    //   しかも左右で別の値になる。合わせ点をモデル側のソケットで宣言できれば、
    //   FBX を直した瞬間に正しい位置へ追従する。
    std::string localSocketName;
    math::Vector3 positionOffset = math::Vector3::ZERO;
    math::Vector3 rotationOffsetDegrees = math::Vector3::ZERO;
    math::Vector3 scaleMultiplier = math::Vector3::ONE;
    bool followPosition = true;
    bool followRotation = true;
    bool followScale = false;

    // socketName を書き換えたとき、旧ソケットから新ソケットへこの秒数かけて移る。
    // 0 なら即座にスナップする (従来動作)。
    //
    // WHY エンジン側に持たせるか:
    //   「ホルスターから手へ移す」は親子の付け替えではなく移動として見せたい。
    //   これをゲームスクリプトが毎フレーム補間すると、追従先の 2 つのソケットが
    //   どちらもアニメーションで動いているため、補間元を静止ポーズで captureして
    //   しまいがちで、キャラが歩いている間だけ銃がワールドに置き去りになる。
    //   両ソケットの「その瞬間の」姿勢を混ぜられるのは、Animator / IK の後に走る
    //   ConstraintSystem だけなので、補間はここで完結させる。
    float blendDuration = 0.0f;

    // ── ランタイム専用 (シリアライズしない) ──────────────────────────────
    // 直近フレームで実際に適用したソケット名。socketName との差分が「切り替え」。
    std::string appliedSocketName;
    // 補間元のソケット名。空なら補間せずスナップする。
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
