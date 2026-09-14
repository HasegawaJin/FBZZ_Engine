/// @file    JointComponent.hpp
/// @brief   physics::Constraint (ロープ・鎖・ヒンジ等) を Scene から張るコンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-09-12
///
/// Physics 側の制約は実装済みだったが Scene からの入口が無く、吊り下げ物も鎖も
/// スクリプトから組み立てるしかなかった。ここが «シーンに置いて保存できる» 唯一の入口。
///
/// WHY ConstraintComponents.hpp へ相乗りしないか:
///   同名だが別物。あちらの SocketAttachment / TransformConstraint は骨追従と
///   Transform 拘束 (Animation カテゴリ) で、剛体の関節ではない。混ぜると
///   «Constraint を足したのに物理が効かない» の取り違えが必ず起きる。
///   Physics カテゴリのコンポーネントは 1 型 1 ファイルが既存の分け方 (RigidBodyComponent /
///   VolumeComponent / CharacterControllerComponent)。
#pragma once
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <Physics/BodyHandle.hpp>
#include <vector>

namespace fbzz::scene {

/// 関節の種別。値は .scene に残るので既存の並びを崩さず末尾へ足す。
enum class JointType : int {
    Fixed    = 0,  ///< 相対位置と相対回転を固定する (溶接)
    Distance = 1,  ///< 距離を保つ (伸び縮みしない棒)
    Rope     = 2,  ///< 最大距離だけを拘束する (たるむ)
    Spring   = 3,  ///< 剛性と減衰を持つバネ
    Hinge    = 4,  ///< アンカーを共有し 1 軸で回る (蝶番)
    Slider   = 5,  ///< 1 軸方向にだけ動く
    Chain    = 6,  ///< 自分 + chainBodies を節間距離で数珠つなぎにする
};

/// 同じ GameObject の RigidBodyComponent と相手の剛体を physics::Constraint で繋ぐ。
///
/// 制約の実体は physics::World が所有し、このコンポーネントはハンドルだけを持つ。
/// 張り直し・解除は JointSync が RigidBody と同じ寿命規約 (毎フレーム申告) で行う。
struct JointComponent {
    bool      enabled = true;
    JointType type    = JointType::Fixed;

    /// 相手の剛体を持つ GameObject。Chain 以外で使う。
    ///
    /// WHY 名前ではなく EntityRef か: シーン内の特定 GameObject を指す参照は
    ///     EntityRef が既存の流儀 (SocketAttachmentComponent::target /
    ///     TransformConstraintComponent::target)。保存は instanceId なので
    ///     GameObject を改名しても切れず、Inspector も Hierarchy からのドロップで繋げる。
    EntityRef connectedBody;
    /// connectedBody が空のとき、祖先方向で最初に見つかった剛体を相手にする。
    ///
    /// WHY 要るか: EntityRef はシーン内参照なので Prefab に保存できない。必須にすると
    ///     «鎖 1 節の Prefab» が原理的に作れず、繋ぎ込みをスクリプトに書くしかなくなる。
    bool connectToParent = true;

    /// Chain の連なり。先頭は常にこの GameObject 自身で、ここには 2 節目以降を並べる。
    std::vector<EntityRef> chainBodies;
    /// Chain の反復回数。多いほど伸びにくく、そのぶん重い。
    int solverIterations = 4;

    /// 張った瞬間の相手との間隔を距離として採る。
    ///
    /// WHY 既定で入れるか: 吊り下げ物は «置いた位置のまま垂れる» のが期待で、
    ///     数値を手で合わせると配置を動かすたびに古くなる。
    bool  autoDistance = true;
    /// Distance = 保つ距離 / Rope = 最大距離 / Spring = 自然長 / Chain = 節間距離 [m]。
    float distance = 1.0f;
    /// Spring の剛性 (N/m) と減衰。
    float spring  = 10.0f;
    float damping = 0.5f;

    /// Hinge の回転軸 / Slider の移動軸 (ワールド基準)。
    math::Vector3 axis = math::Vector3::UP;
    /// Hinge のアンカー。自分側と相手側のローカル座標で、この 2 点を重ねる。
    math::Vector3 anchor          = math::Vector3::ZERO;
    math::Vector3 connectedAnchor = math::Vector3::ZERO;

    /// Hinge = 角度 [degrees] / Slider = 軸方向の距離 [m] の可動域。
    bool  useLimits  = false;
    float lowerLimit = 0.0f;
    float upperLimit = 0.0f;

    /// Hinge のモーター。目標角速度 [degrees/s] と上限トルク。
    bool  useMotor        = false;
    float motorSpeed      = 0.0f;
    float motorMaxTorque  = 0.0f;

    // ── ランタイム専用 (シリアライズしない) ──────────────────────────────
    /// physics::World が持つ制約への世代付き参照。所有は World 側。
    physics::ConstraintHandle constraintHandle;
    /// 実際に物理へ張れているか。相手が見つからない・剛体が無いときは false。
    bool  connected = false;
    /// autoDistance で採った実効距離。Inspector が «結局いくつで張ったか» を出す。
    float resolvedDistance = 0.0f;

    JointComponent() = default;
    ~JointComponent() = default;
    // WHY ハンドルを引き継がないか: 複製 (Copy Component / GameObject の複製) で
    //     同じスロットを 2 つのコンポーネントが指すと、両方が同じ制約へパラメーターを
    //     書き、片方を消したときにもう片方の関節が黙って消える。
    JointComponent(const JointComponent& o) { CopyAuthoredFrom(o); }
    JointComponent& operator=(const JointComponent& o)
    {
        if (this != &o) CopyAuthoredFrom(o);
        return *this;
    }
    JointComponent(JointComponent&&)            = default;
    JointComponent& operator=(JointComponent&&) = default;

    const char* GetTypeName() const { return "Joint"; }

    /// 相手を 1 体だけ取る種別か (Chain は連なりで受ける)。
    [[nodiscard]] bool IsPairJoint() const { return type != JointType::Chain; }
    /// distance を使う種別か。
    [[nodiscard]] bool UsesDistance() const
    {
        return type == JointType::Distance || type == JointType::Rope
            || type == JointType::Spring   || type == JointType::Chain;
    }
    /// axis を使う種別か。
    [[nodiscard]] bool UsesAxis() const
    {
        return type == JointType::Hinge || type == JointType::Slider;
    }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        {
            static constexpr const char* kTypeLabels[] = {
                "Fixed", "Distance", "Rope", "Spring", "Hinge", "Slider", "Chain"
            };
            int typeIndex = static_cast<int>(type);
            r.Enum("type", typeIndex, kTypeLabels);
            constexpr int kLastType = static_cast<int>(JointType::Chain);
            type = static_cast<JointType>(typeIndex < 0 || typeIndex > kLastType ? 0 : typeIndex);
        }

        const bool pair  = IsPairJoint();
        const bool chain = type == JointType::Chain;
        const bool hinge = type == JointType::Hinge;

        r.BeginField("connectedBody", "connectedBody");
        r.SetFieldVisible(pair);
        r.RefField("connectedBody", connectedBody, "RigidBodyComponent");
        r.EndField();
        r.FieldIf("connectToParent", connectToParent, pair,
                  "空のとき祖先をたどって最初の剛体を相手にする");

        r.BeginField("chainBodies", "chainBodies");
        r.SetFieldVisible(chain);
        r.RefListField("chainBodies", chainBodies, "RigidBodyComponent");
        r.EndField();
        r.FieldIf("solverIterations", solverIterations, chain);

        r.FieldIf("autoDistance", autoDistance, UsesDistance(),
                  "張った瞬間の間隔を距離にする");
        r.FieldIf("distance", distance, UsesDistance());
        r.FieldIf("spring", spring, type == JointType::Spring);
        r.FieldIf("damping", damping, type == JointType::Spring);

        r.FieldIf("axis", axis, UsesAxis());
        r.FieldIf("anchor", anchor, hinge);
        r.FieldIf("connectedAnchor", connectedAnchor, hinge);

        r.FieldIf("useLimits", useLimits, UsesAxis());
        r.FieldIf("lowerLimit", lowerLimit, UsesAxis());
        r.FieldIf("upperLimit", upperLimit, UsesAxis());

        r.FieldIf("useMotor", useMotor, hinge);
        r.FieldIf("motorSpeed", motorSpeed, hinge);
        r.FieldIf("motorMaxTorque", motorMaxTorque, hinge);
    }

private:
    void CopyAuthoredFrom(const JointComponent& o)
    {
        enabled          = o.enabled;
        type             = o.type;
        connectedBody    = o.connectedBody;
        connectToParent  = o.connectToParent;
        chainBodies      = o.chainBodies;
        solverIterations = o.solverIterations;
        autoDistance     = o.autoDistance;
        distance         = o.distance;
        spring           = o.spring;
        damping          = o.damping;
        axis             = o.axis;
        anchor           = o.anchor;
        connectedAnchor  = o.connectedAnchor;
        useLimits        = o.useLimits;
        lowerLimit       = o.lowerLimit;
        upperLimit       = o.upperLimit;
        useMotor         = o.useMotor;
        motorSpeed       = o.motorSpeed;
        motorMaxTorque   = o.motorMaxTorque;
        constraintHandle = {};
        connected        = false;
        resolvedDistance = 0.0f;
    }
};

} // namespace fbzz::scene
