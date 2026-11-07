/// @file    RagdollRig.hpp
/// @brief   骨の並びから剛体と関節を組み、姿勢を往復させる
/// @author  Hasegawa Jin
/// @date    2026-09-02

/// @note Skeleton/GameObject を知らない形にするのは、«骨の姿勢の配列» と «剛体の姿勢» の変換だけに閉じるため。切り離せば、スケルトンを用意せずに単体テストで «捕獲して書き戻したら元の姿勢に戻るか» を確かめられる (Skeleton から配列を作るのは RagdollSystem の仕事)。
/// @note 剛体を «骨と骨の間» に置くのは、骨の位置に置くと剛体の中心が関節と重なり慣性が実際と食い違う (腕を振っても反動が出ない) ため。骨 i から最初の子までを 1 本のカプセルにしその中点を重心にすると、長い骨ほど回りにくい ─ ロボットの «重い» はここから出る。
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Scene/Ragdoll/RagdollProfile.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Physics/CapsuleCollider.hpp>
#include <Physics/CollisionPair.hpp>
#include <Physics/ContactPoint.hpp>
#include <Physics/PhysicsMaterial.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/XPBDContact.hpp>
#include <Physics/XPBDBodyDrive.hpp>
#include <Physics/XPBDJoint.hpp>
#include <Physics/XPBDPlaneContact.hpp>
#include <Physics/XPBDPoseAnchor.hpp>
#include <Physics/XPBDSolver.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::physics { class World; }

namespace fbzz::scene {
class RagdollControls;

/// @note 接触の作り方。どれを切っても «関節だけの関節体» まで素直に縮退する。
struct RagdollContactSettings {
    /// @note 世界のコライダーと当たる。地形・壁・階段。
    bool  world    = true;
    /// @note World の剛体とも当たり、姿勢を再積分せず、反作用を substep 境界で速度へ返す。
    bool  dynamic  = true;
    /// @note 自分の骨どうしが当たる。腕や脚が胴を貫通しなくなる。
    bool  self     = true;
    /// @note 関節グラフ上でこの段数以内の先祖・子孫とは当てない。
    /// @note 関節で繋がった剛体は必ず重なるため。除外しないと、関節が寄せた端から接触が押し返してその場で震え続ける。1 で親子、2 で祖父–孫まで。
    int   selfSkip = 2;
    /// @note 探索半径の余裕 [m]。速く動く骨がすり抜けないよう少し広めに拾う。
    float margin   = 0.08f;
    /// @note 骨の表面。世界側のマテリアルと PhysicsMaterial の規則で合成される。
    physics::PhysicsMaterial surface;
    /// @note 支持された低速の関節体を休止させる。摩擦検証や常時応答が必要な場合は false。
    bool allowRest = true;
};

/// @brief デバッグ表示用のワイヤー 1 本。色は描き手が kind から決める。
/// @note Physics の ColliderDebugGeometry と同じ理由で、Renderer を知らない層が «何を描くか» まで決めると、描画 API が変わるたびにシミュレーション側が壊れる。
struct RagdollDebugLine {
    enum class Kind : std::uint8_t {
        Body = 0,        ///< @note 剛体のカプセル
        Joint,           ///< @note 関節の可動域 (まだ余裕がある)
        JointSaturated,  ///< @note 関節の可動域 (トルク上限に張り付いている ＝ 力負け)
        Contact,         ///< @note 接触点と押し返す向き
    };

    math::Vector3 from;
    math::Vector3 to;
    Kind          kind = Kind::Body;
};

/// @note 骨 1 本のワールド姿勢。**親は必ず子より前に並んでいること。**
struct RagdollBonePose {
    /// @note 同じ配列内の親の添字。-1 なら根。
    int              parent = -1;
    std::string      name;
    math::Vector3    position = math::Vector3::ZERO;
    math::Quaternion rotation = math::Quaternion::Identity();
    math::Vector3 scale = math::Vector3::ONE;
};

/// @note Guardで運ぶ所有者のworld姿勢。位置はm、回転は単位Quaternionで、骨アニメーションを含めない。
struct RagdollOwnerPose {
    math::Vector3 position = math::Vector3::ZERO;
    math::Quaternion rotation = math::Quaternion::Identity();
};

/// @note 剛体・コライダー・関節を所有し、ソルバへ載せる。

/// @note 内部で非所有ポインタ (ソルバ → 剛体、関節 → 剛体) が絡むので、コピーもムーブも禁止する。
/// @note **コンポーネントから持つときは `std::unique_ptr<RagdollRig>` で保持すること** ─
/// @note コンポーネントは配列の再確保で動くことがある。
class RagdollRig {
public:
    FBZZ_ENGINE_API RagdollRig();
    [[nodiscard]] RagdollControls& Controls() { return *m_controls; }
    [[nodiscard]] const RagdollControls& Controls() const { return *m_controls; }
    FBZZ_ENGINE_API ~RagdollRig();

    RagdollRig(const RagdollRig&)            = delete;
    RagdollRig& operator=(const RagdollRig&) = delete;
    RagdollRig(RagdollRig&&)                 = delete;
    RagdollRig& operator=(RagdollRig&&)      = delete;

    /// @note 骨の並びから剛体と関節を組む。**この姿勢が «たわみ 0» の基準になる**ので、
    /// @note 可動域はここからの角度で測られる。バインドポーズ相当で呼ぶのが望ましい。
    FBZZ_ENGINE_API void Build(const std::vector<RagdollBonePose>& bones, const RagdollProfile& profile);
    FBZZ_ENGINE_API void Clear();

    /// @note 剛体を今の骨の姿勢へ置き直し、速度を 0 に戻す。物理へ渡す境目。
    FBZZ_ENGINE_API void Capture(const std::vector<RagdollBonePose>& bones);

    /// @note Active: 各関節のドライブ目標を今の骨の姿勢から取り直す。
    /// @note 無負荷ならこの姿勢が釣り合い点になるので、絵はクリップそのままになる。
    /// @note ownerPoseを指定したGuardでは所有者の移動だけを剛体へ運び、腰のボブ・回旋は駆動目標として扱う。
    /// @note ownerPose省略時は従来どおり各物理根の姿勢から移動を求める。Capture後の初回は所有者姿勢の基準を保存する。
    /// @note 正のsampleDtは描画サンプルの間隔で、Step内のsubstep時刻へ補間する。0は現在の物理時刻へ即時設定して履歴を捨てる。
    /// @note 負または非有限のsampleDtは姿勢・目標・履歴を変更しない。
    FBZZ_ENGINE_API void UpdateDriveTargets(const std::vector<RagdollBonePose>& bones, float sampleDt = 0.0f,
        const RagdollOwnerPose* ownerPose = nullptr);
    /// @brief 物理形状の底面を地面へ置く足首姿勢と高さを求める。world normal.y > 0.25、距離は world m。
    /// @note 形状の +Y を soleNormal (null は normal) へ合わせ、実面 normal に対する支持高さを求める。soleNormal は有限かつ長さ > 0。
    /// @note 失敗時は出力を変更しない。Rig の姿勢・目標は変更しない。
    [[nodiscard]] FBZZ_ENGINE_API bool TryGetFootPlacement(std::string_view bone,
        const math::Quaternion& boneRotation, const math::Vector3& normal,
        math::Quaternion& outRotation, float& outHeight,
        const math::Vector3* soleNormal = nullptr) const;
    /// @note Active の骨位置・支持点 [world m] と支持法線 [world unit]。自己接触・Passive は false、失敗時は出力を変えない。
    /// @note outCollider は同じ Scene 更新内でだけ参照する非所有の支持形状。
    [[nodiscard]] FBZZ_ENGINE_API bool TryGetBoneSupport(std::string_view bone, math::Vector3& outPosition,
        math::Vector3& outPoint, math::Vector3& outNormal, const physics::Collider*& outCollider) const;
    /// @return 剛体が存在し設定が有効なとき true。失敗時は変更しない。
    [[nodiscard]] FBZZ_ENGINE_API bool SetBodyDrive(std::string_view bone, const RagdollBodyDrive& settings);
    [[nodiscard]] FBZZ_ENGINE_API bool ClearBodyDrive(std::string_view bone);

    /// @brief サーボの効き方をまとめて決める。プロファイルの値へ倍率として掛かるので、骨格ごとの «どこが強いか» を保ったまま全体の力み具合だけを動かせる。
    /// @param enabled false で完全に脱力する (Passive)。
    /// @param scale トルク上限に掛かる倍率。下げるほど早く力負けする。compliance は逆数側へ掛かるので同時に柔らかくなる。
    /// @param falloff 根から 1 関節ごとに scale へ掛かる倍率。1 で一律、下げると体幹が支えて末端だけが流れる。
    /// @param damping 減衰に掛かる倍率。下げるほど押された後に揺れ戻る。
    FBZZ_ENGINE_API void SetDrive(bool enabled, float scale, float falloff, float damping);

    /// @brief 根の剛体をアニメーションへ繋ぎ止める強さ。**Active では必須**。
    /// @note 関節は隣の骨との相対しか拘束せず、根は何にも繋がっていない。切ったまま立たせると、サーボが形を保ったまま全体が重力で落ちていく。
    /// @param scale 自重を支えるのに要る力への倍率。1.0 でぎりぎり、下げると胴が沈む。
    /// @param sag 上限の力を出し切るまでに沈む距離 [m]。押されたときの «遊び»。
    /// @param tilt 上限のトルクを出し切るまでに傾く角 [rad]。
    FBZZ_ENGINE_API void SetRootAnchor(float scale, float sag, float tilt);

    void SetContactSettings(const RagdollContactSettings& settings)
    {
        m_contactSettings = settings;
    }
    [[nodiscard]] const RagdollContactSettings& GetContactSettings() const
    {
        return m_contactSettings;
    }

    /// @note このフレームの接触を作り直す。**Step の前に毎フレーム呼ぶ。**

    /// @note 形状判定は既存の `PhysicsSolver::NarrowPhase` へ丸投げする ─ カプセル vs
    /// @note 三角メッシュも地形も、こちらで書き直す理由が無い。

    /// @param world null で «世界と当たらない» (自己衝突と接地面だけになる)。
    /// @note world は次の Step が返るまで生存すること。Step 中も各 substep の接触更新に参照する。
    FBZZ_ENGINE_API void RefreshContacts(physics::World* world);
    /// @note World の相手へ未反映の力積を返す。内部の substep 境界と、最終 substep を処理する Step 後に呼ぶ。
    FBZZ_ENGINE_API void ApplyContactReactions();
    /// @note 当てないコライダー。自分の GameObject が持っているものを渡す ─
    /// @note «倒れたラグドールが自分の当たり判定と押し合う» を防ぐ。
    FBZZ_ENGINE_API void SetIgnoredColliders(std::vector<const physics::Collider*> colliders);
    void SetIgnoredBody(physics::RigidBody* body) { m_ignoredBody = body; }
    [[nodiscard]] int GetContactCount() const {
        int count=0;
        for (const auto& contact:m_contacts) if (contact.IsTouching()) ++count;
        return count;
    }

    /// @note 接地面 [m] と摩擦係数。世界にコライダーが無い場所での抜け止め。

    /// @note M3 で本物の接触が入ったので、通常は世界側が先に受け止める。この面は
    /// @note «取りこぼしたときに床下へ消えない» ためだけに残してある。
    FBZZ_ENGINE_API void SetGround(float height, float friction);
    /// @note 抜け止めの面を «届かない所» へ下げる。世界の接触だけで受け止めたいときに使う。
    FBZZ_ENGINE_API void DisableGround();
    /// @note 与えられた姿勢で、一番低い接触点が来る高さ [m]。
    /// @note 床をここより上に置くとアニメーション姿勢が床を破り、足が震える。
    [[nodiscard]] FBZZ_ENGINE_API float LowestContactHeight(const std::vector<RagdollBonePose>& bones) const;

    /// @note 全剛体へ速度を足す。radius が 0 より大きければ、origin からの距離で減衰させる。
    FBZZ_ENGINE_API void ApplyImpulse(const math::Vector3& origin,
                      const math::Vector3& velocity,
                      float                radius);
    /// @note ワールド空間の角速度 [rad/s]。radius > 0 なら距離で減衰する。
    FBZZ_ENGINE_API void ApplyAngularVelocity(const math::Vector3& origin, const math::Vector3& angularVelocity, float radius);

    /// @note 重力を変えるとサーボの実トルクも変わる ─ トルクは «自重を支えられるか» で
    /// @note 決めているので、重力が 2 倍になれば必要トルクも 2 倍になる。
    void SetGravity(const math::Vector3& gravity)
    {
        if ((m_solver.GetGravity() - gravity).LengthSq() > 0.0f) ResetRest();
        m_solver.SetGravity(gravity);
        ApplyDrive();
    }
    void SetSubsteps(int substeps) { m_solver.SetSubsteps(substeps); }
    /// @note 速度の減衰 [1/s]。空気抵抗ではなく «関節のこすれ» の代用。
    FBZZ_ENGINE_API void SetDrag(float linear, float angular);

    FBZZ_ENGINE_API void Step(float dt);
    /// @note アニメーションの根位置と骨長を保持する立位制限。距離は m、角度は rad。
    FBZZ_ENGINE_API void SetStandingGuard(bool enabled, float maxDistance, float maxRadians);

    /// @note 剛体の姿勢を骨のワールド姿勢へ戻す。
    /// @note 剛体を持たない骨 (葉など) は、fallback の «親からの相対» を保って埋める。
    FBZZ_ENGINE_API void WritePose(const std::vector<RagdollBonePose>& fallback,
                   std::vector<math::Vector3>&         outPositions,
                   std::vector<math::Quaternion>&      outRotations) const;

    /// @note 与えられた姿勢から一番離れた剛体の距離 [m]。«どれだけ効いているか» の物差し。
    [[nodiscard]] FBZZ_ENGINE_API float MeasureDeviation(const std::vector<RagdollBonePose>& bones) const;
    /// @note 全剛体の重心 (質量加重)。バランス判定と «どちらへ倒れるか» に使う。
    [[nodiscard]] FBZZ_ENGINE_API math::Vector3 CenterOfMass() const;
    /// @note トルク上限に張り付いている関節の数。0 でない ＝ どこかが力負けしている。
    [[nodiscard]] FBZZ_ENGINE_API int CountSaturatedJoints() const;
    /// @note 可動域に食い込んで押し戻されている関節の数。
    /// @note 0 でない ＝ クリップが «曲げてよいことになっていない» 所まで曲げている。
    [[nodiscard]] FBZZ_ENGINE_API int CountLimitedJoints() const;

    /// @brief クリップが要求する関節角を測り、可動域をそこまで広げる。広げるだけで狭めない («クリップが使う範囲は必ず可動域の内側» を保証する)。
    /// @note 可動域の基準はバインドポーズで、クリップがそこからどれだけ動かすかは骨格とモーション次第。プロファイルの «狭い可動域» がクリップの動きより狭いと、サーボが目標へ行けず絵が崩れる (物理ではなく «物理が許していない» だけ)。実際に使う範囲を測れば可動域の符号も自動で決まる。
    /// @param margin 測った範囲へ足す余裕 [rad]。押されて少し越える分を吸収する。
    FBZZ_ENGINE_API void SetLimitLearning(bool enabled, float margin);

    /// @brief どの規則にも当たらず fallback になった骨の名前。Build() が埋める。
    /// @note «剛体は組めたが可動域が全部 fallback の球関節» は画面では «なんとなく柔らかい» としか見えず、原因に辿り着けない。骨格を差し替えるたびに起きる失敗なので、どの骨が漏れたかを名指しできる形で残す。
    [[nodiscard]] const std::vector<std::string>& UnmatchedBones() const
    {
        return m_unmatchedBones;
    }

    [[nodiscard]] bool IsBuilt() const { return !m_bodies.empty(); }
    [[nodiscard]] int  GetBodyCount()  const { return static_cast<int>(m_bodies.size()); }
    [[nodiscard]] int  GetJointCount() const { return static_cast<int>(m_joints.size()); }
    /// @note 骨の添字 → 剛体の添字。-1 なら剛体を持たない骨。
    [[nodiscard]] FBZZ_ENGINE_API int  BodyIndexOfBone(int boneIndex) const;
    /// @note 剛体を直接触る口。範囲外で nullptr。

    /// @note «この骨だけ固定する» (m_isStatic) が要る場面 ─ 掴んでぶら下がる (M6)、
    /// @note 腰を吊る演出、片持ち梁としての試験 ─ は、拘束を足すのではなく剛体側を
    /// @note 止める方が素直に書ける。
    [[nodiscard]] FBZZ_ENGINE_API physics::RigidBody* GetBody(int bodyIndex) const;
    /// @note 骨の添字から直接引く。剛体を持たない骨で nullptr。
    [[nodiscard]] FBZZ_ENGINE_API physics::RigidBody* BodyOfBone(int boneIndex) const;

    [[nodiscard]] physics::XPBDSolver& Solver() { return m_solver; }

    /// @note 剛体・可動域・接触をワイヤーで書き出す。
    /// @note 形状は RefreshContacts が毎フレーム同期しているので、ここでは読むだけ。
    FBZZ_ENGINE_API void BuildDebugLines(std::vector<RagdollDebugLine>& out) const;

private:
    friend class RagdollControls;
    std::unique_ptr<RagdollControls> m_controls;
    float m_controlSubstep = 0.0f;
    /// @note 剛体 1 個と、それが代表する骨との対応。
    struct BodyLink {
        int                                      boneIndex = -1;
        std::unique_ptr<physics::RigidBody>      body;
        std::unique_ptr<physics::Collider> collider;
        RagdollBodyShape shape = RagdollBodyShape::CAPSULE;
        float radius = 0.0f;
        float halfHeight = 0.0f;
        math::Vector3 halfExtents = math::Vector3::ZERO;
        std::size_t instanceIndex = 0;
        /// @brief カプセル両端の球。
        /// @note NarrowPhase が返す接触点は形状の組ごとに 1 個で、寝かせた胴が «1 点で床に触れている» 状態になりそこを軸にくるくる回るため、両端でも取って 3 点で押さえる。球はカプセルの内側にあるので、カプセルより深い接触を報告することはない。
        std::unique_ptr<physics::SphereCollider> caps[2];
        /// @note 親の剛体。自己衝突の «関節グラフ上の距離» を測るのに使う。-1 で根。
        int                                      parentBody = -1;
        /// @note 骨 = 剛体 × これ。捕獲と書き戻しはこの 2 つで往復する。
        math::Vector3    boneOffset = math::Vector3::ZERO;
        math::Quaternion boneRotation = math::Quaternion::Identity();
        math::Vector3    previousPosition = math::Vector3::ZERO;
        math::Quaternion previousRotation = math::Quaternion::Identity();
        float            mass = 1.0f;
        std::string name;
        physics::XPBDBodyDrive* drive = nullptr;
        bool authoredDrive = false;
        bool customDrive = false;
        RagdollBodyDrive baseDrive;
        RagdollBodyDrive activeDrive;
        bool targetHistory = false;
        [[nodiscard]] float Reach() const { return shape == RagdollBodyShape::BOX ? halfExtents.Length() : halfHeight + radius; }
        [[nodiscard]] float Bottom(const math::Vector3& p, const math::Quaternion& q) const;
    };

    /// @note 関節と、それが繋いでいる剛体の添字。所有は m_solver 側。
    struct JointLink {
        physics::XPBDJoint* joint      = nullptr;
        int                 parentBody = -1;
        int                 childBody  = -1;
        /// @note 根から何関節目か。ドライブの落とし方に使う。
        int                 depth      = 0;
        /// @note プロファイルが与えた素の設定。倍率はここへ掛ける ─ 現在値へ掛け続けると
        /// @note 毎フレーム縮んでいき、力み具合が «時間とともに» 変わってしまう。
        RagdollServo        baseServo;
        /// @note «この関節から先を、重力に対して真横へ伸ばした姿勢で支える» のに要る
        /// @note トルクの素 [kg·m]。実トルクは これ × |g| で、重力を変えれば追従する。
        float               holdMoment = 0.0f;
    };

    /// @note 骨 i の «剛体としての» ワールド姿勢を、与えられた骨の姿勢から作る。
    FBZZ_ENGINE_API void BodyPoseFromBone(int bodyIndex, const RagdollBonePose& bone,
                          math::Vector3& outPosition, math::Quaternion& outRotation) const;
    /// @note 今の倍率を全関節のドライブへ反映する。
    FBZZ_ENGINE_API void ApplyDrive();
    FBZZ_ENGINE_API void ProjectStandingPose();
    /// @note 根はworld、子は親ローカル。速度・角速度は直前サンプルからの区間微分で、始点サンプルは0。
    struct TargetBonePose {
        math::Vector3 position;
        math::Quaternion rotation = math::Quaternion::Identity();
        math::Vector3 velocity = math::Vector3::ZERO;
        math::Vector3 angularVelocity = math::Vector3::ZERO;
    };
    struct TargetSample {
        double time = 0.0;
        std::vector<TargetBonePose> bones;
    };
    void ResetTargetSamples(const std::vector<RagdollBonePose>& bones);
    void StoreTargetSample(const std::vector<RagdollBonePose>& bones);
    void ComposeTargetSample(const TargetSample& sample, std::vector<RagdollBonePose>& output) const;
    void SampleDriveTargets(double time);
    /// @note 補間時の骨原点のworld速度を重心・親剛体空間へ変換し、Controlsの上書きを最後に適用する。
    /// @see https://modernrobotics.northwestern.edu/nu-gm-book-resource/3-3-2-twists-part-1-of-2/ Modern Robotics §3.3.2 原点と座標系による速度の変換。
    void ApplyTargetPose(const std::vector<RagdollBonePose>& bones,
        const std::vector<RagdollBonePose>* before, const std::vector<RagdollBonePose>* after, float sampleDt,
        bool useSampledVelocities = false);

    bool m_standingGuard = false;
    RagdollOwnerPose m_ownerPose;
    bool m_ownerHistory = false;
    /// @note 未消費の描画サンプルだけを保持し、固定刻みのbacklogでも将来の姿勢へ飛ばさない。
    std::vector<TargetSample> m_targetSamples;
    std::size_t m_targetSampleHead = 0;
    std::vector<std::vector<TargetBonePose>> m_targetPosePool;
    double m_sampleTime = 0.0;
    double m_simulationTime = 0.0;
    double m_targetStepTime = 0.0;
    std::vector<RagdollBonePose> m_sourceTargets;
    std::vector<RagdollBonePose> m_sampledTargets;
    std::vector<RagdollBonePose> m_sampleBefore;
    std::vector<RagdollBonePose> m_sampleAfter;
    std::vector<math::Vector3> m_sampledVelocities;
    std::vector<math::Vector3> m_sampledAngularVelocities;
    float m_standingMaxDistance = 0.15f;
    float m_standingMaxRadians = 0.25f;
    std::vector<RagdollBonePose> m_standingTargets;
    std::vector<math::Vector3> m_standingPositions;
    std::vector<math::Quaternion> m_standingRotations;
    std::vector<math::Quaternion> m_standingLocalRotations;
    /// @note この剛体は自分のものか。NarrowPhase が返した接触の «どちら側» かを判定する。
    [[nodiscard]] FBZZ_ENGINE_API int OwnBodyIndex(const physics::RigidBody* body) const;
    /// @note 自己衝突を許す組か。先祖–子孫の距離と、組んだ時点での重なりで決まる。
    [[nodiscard]] FBZZ_ENGINE_API bool AllowsSelfContact(int a, int b) const;
    FBZZ_ENGINE_API void BuildShapeInstances();
    FBZZ_ENGINE_API void CollectWorldPairs(physics::World& world);
    FBZZ_ENGINE_API void CollectSelfPairs();
    FBZZ_ENGINE_API void MakeContacts();
    FBZZ_ENGINE_API void RebuildContacts(physics::World* world);
    FBZZ_ENGINE_API void AccumulateContactReactions();
    FBZZ_ENGINE_API void CaptureContactPoses();
    FBZZ_ENGINE_API void UpdateRest(float dt);
    void ResetRest() { m_resting = false; m_restTime = 0.0f; }

    struct ContactReaction {
        physics::RigidBody* body = nullptr;
        math::Vector3 impulse;
        math::Vector3 point;
    };

    std::vector<BodyLink>  m_bodies;
    std::vector<JointLink> m_joints;
    /// @note 接地拘束。剛体 1 個につきカプセルの両端の 2 個。所有は m_solver 側。
    std::vector<physics::XPBDPlaneContact*> m_ground;
    /// @note 親の関節を持たない剛体を、アニメーションへ繋ぎ止める。所有は m_solver 側。
    /// @note 添字は m_anchorBody と対。
    std::vector<physics::XPBDPoseAnchor*>   m_anchors;
    std::vector<int>                        m_anchorBody;
    /// @brief 根 1 本ごとの «その先の» 総質量 [kg] と質量モーメント [kg·m]。繋ぎ止める力の素。添字は m_anchors と対。
    /// @note 骨の並びは 1 本の木とは限らない。繋がっていない部分木を同じソルバへ載せる構成 (壊れた脚だけを落とす等) で全身の合計を配ると、部分木 1 つの繋ぎ止めが «全部を支える力» になってしまうため、根ごとに持つ。
    std::vector<float> m_anchorMass;
    std::vector<float> m_anchorMoment;
    float m_totalMass  = 0.0f;
    bool  m_learnLimits = false;
    float m_limitMargin = 0.09f;
    float m_anchorScale = 3.0f;
    float m_anchorSag   = 0.04f;
    float m_anchorTilt  = 0.05f;
    /// @note 骨の添字 → m_bodies の添字。-1 で剛体なし。
    std::vector<int>                 m_boneToBody;
    physics::XPBDSolver              m_solver;

    /// @note 自分のカプセルと両端球を ColliderInstance にしたもの。NarrowPhase の入力。
    /// @note **CollisionPair が中身を指すので、Build 以外で再確保しないこと。**

    /// @note 各 BodyLink の instanceIndex が本体を指し、カプセルだけ両端の補助球を後続させる。
    std::vector<physics::ColliderInstance> m_instances;
    /// @note このフレームの接触。実体はここが持ち、ソルバへは非所有ポインタで渡す。
    std::vector<physics::XPBDContact>      m_contacts;
    struct CachedContact {
        const physics::Collider* a=nullptr;
        const physics::Collider* b=nullptr;
        std::weak_ptr<const void> lifetimeA,lifetimeB,bodyA,bodyB;
        math::Vector3 localPoint;
        math::Vector3 otherPoint;
        physics::XPBDContact contact;
        bool used=false;
    };
    std::vector<CachedContact> m_contactCache;
    std::vector<CachedContact> m_previousContacts;
    std::vector<physics::CollisionPair>    m_pairs;
    std::vector<physics::ContactPoint>     m_contactPoints;
    std::vector<ContactReaction>           m_contactReactions;
    /// @note RefreshContacts から次の Step 終了までの非所有参照。
    physics::World*                        m_contactWorld = nullptr;
    bool                                  m_resting = false;
    float                                 m_restTime = 0.0f;
    /// @note プロファイルのどの規則にも当たらなかった骨。Build() が集める。
    std::vector<std::string>               m_unmatchedBones;
    /// @note 組んだ時点で既に重なっている剛体の組。自己衝突から永久に外す。
    /// @note 添字は a * 剛体数 + b (a < b)。
    std::vector<bool>                      m_selfOverlapAtBuild;

    RagdollContactSettings                 m_contactSettings;
    std::vector<const physics::Collider*>  m_ignoredColliders;
    physics::RigidBody*                    m_ignoredBody = nullptr;

    /// @note プロファイルが有効にした関節を «全体として» 切るための master スイッチ。
    bool  m_driveEnabled  = true;
    float m_driveScale    = 1.0f;
    float m_driveFalloff  = 1.0f;
    float m_driveDamping  = 1.0f;
    /// @note SetGround を呼ぶまでは «届かない所» に置く。詳細は RagdollRig.cpp の kNoGround。
    float m_groundHeight  = -1.0e9f;
    float m_groundFriction = 0.9f;
};

} /// @note namespace fbzz::scene
