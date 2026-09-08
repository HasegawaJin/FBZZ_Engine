/// @file    RagdollRig.hpp
/// @brief   骨の並びから剛体と関節を組み、姿勢を往復させる
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// WHY Skeleton / GameObject を知らない形にするか:
///   ここがやるのは «骨の姿勢の配列» と «剛体の姿勢» の変換だけで、アセットの形式にも
///   シーングラフにも依存しない。切り離しておけば、スケルトンを用意せずに単体テストで
///   «捕獲して書き戻したら元の姿勢に戻るか» を確かめられる。Skeleton から配列を作るのは
///   RagdollSystem の仕事。
///
/// WHY 剛体を «骨と骨の間» に置くか:
///   骨の位置に置くと、剛体の中心が関節と重なって慣性が実際と食い違う (腕を振っても
///   反動が出ない)。骨 i から最初の子までを 1 本のカプセルにし、その中点を重心にすると、
///   長い骨ほど回りにくい ─ ロボットの «重い» はここから出る。
#pragma once

#include <Engine/Scene/Ragdoll/RagdollProfile.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <Physics/CapsuleCollider.hpp>
#include <Physics/CollisionPair.hpp>
#include <Physics/ContactPoint.hpp>
#include <Physics/PhysicsMaterial.hpp>
#include <Physics/SphereCollider.hpp>
#include <Physics/XPBDContact.hpp>
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

/// 接触の作り方。どれを切っても «関節だけの関節体» まで素直に縮退する。
struct RagdollContactSettings {
    /// 世界のコライダーと当たる。地形・壁・階段。
    bool  world    = true;
    /// 世界の «World が積分している» 剛体とも当たる。反作用はフレーム末に力積で返す。
    bool  dynamic  = true;
    /// 自分の骨どうしが当たる。腕や脚が胴を貫通しなくなる。
    bool  self     = true;
    /// 関節グラフ上でこの段数以内の先祖・子孫とは当てない。
    ///
    /// WHY 要るか: 関節で繋がった剛体は必ず重なる。除外しないと、関節が寄せた端から
    ///     接触が押し返して、その場で震え続ける。1 で親子、2 で祖父–孫まで。
    int   selfSkip = 2;
    /// 探索半径の余裕 [m]。速く動く骨がすり抜けないよう少し広めに拾う。
    float margin   = 0.08f;
    /// 骨の表面。世界側のマテリアルと PhysicsMaterial の規則で合成される。
    physics::PhysicsMaterial surface;
};

/// デバッグ表示用のワイヤー 1 本。色は描き手が kind から決める。
///
/// WHY 線分だけ返すか: Physics の ColliderDebugGeometry と同じ理由で、Renderer を
///     知らない層が «何を描くか» まで決めてしまうと、描画 API が変わるたびに
///     シミュレーション側が壊れる。
struct RagdollDebugLine {
    enum class Kind : std::uint8_t {
        Body = 0,        ///< 剛体のカプセル
        Joint,           ///< 関節の可動域 (まだ余裕がある)
        JointSaturated,  ///< 関節の可動域 (トルク上限に張り付いている ＝ 力負け)
        Contact,         ///< 接触点と押し返す向き
    };

    math::Vector3 from;
    math::Vector3 to;
    Kind          kind = Kind::Body;
};

/// 骨 1 本のワールド姿勢。**親は必ず子より前に並んでいること。**
struct RagdollBonePose {
    /// 同じ配列内の親の添字。-1 なら根。
    int              parent = -1;
    std::string      name;
    math::Vector3    position = math::Vector3::ZERO;
    math::Quaternion rotation = math::Quaternion::Identity();
};

/// 剛体・コライダー・関節を所有し、ソルバへ載せる。
///
/// 内部で非所有ポインタ (ソルバ → 剛体、関節 → 剛体) が絡むので、コピーもムーブも禁止する。
/// **コンポーネントから持つときは `std::unique_ptr<RagdollRig>` で保持すること** ─
/// コンポーネントは配列の再確保で動くことがある。
class RagdollRig {
public:
    RagdollRig() = default;
    ~RagdollRig();

    RagdollRig(const RagdollRig&)            = delete;
    RagdollRig& operator=(const RagdollRig&) = delete;
    RagdollRig(RagdollRig&&)                 = delete;
    RagdollRig& operator=(RagdollRig&&)      = delete;

    /// 骨の並びから剛体と関節を組む。**この姿勢が «たわみ 0» の基準になる**ので、
    /// 可動域はここからの角度で測られる。バインドポーズ相当で呼ぶのが望ましい。
    void Build(const std::vector<RagdollBonePose>& bones, const RagdollProfile& profile);
    void Clear();

    /// 剛体を今の骨の姿勢へ置き直し、速度を 0 に戻す。物理へ渡す境目。
    void Capture(const std::vector<RagdollBonePose>& bones);

    /// Active: 各関節のドライブ目標を今の骨の姿勢から取り直す。
    /// 無負荷ならこの姿勢が釣り合い点になるので、絵はクリップそのままになる。
    void UpdateDriveTargets(const std::vector<RagdollBonePose>& bones);

    /// サーボの効き方をまとめて決める。プロファイルの値へ倍率として掛かるので、
    /// 骨格ごとの «どこが強いか» を保ったまま全体の力み具合だけを動かせる。
    ///
    /// @param enabled false で完全に脱力する (Passive)。
    /// @param scale   トルク上限に掛かる倍率。下げるほど早く力負けする。
    ///                compliance は逆数側へ掛かるので、同時に柔らかくなる。
    /// @param falloff 根から 1 関節ごとに scale へ掛かる倍率。1 で一律。
    ///                下げると体幹が支えて末端だけが流れる。
    /// @param damping 減衰に掛かる倍率。下げるほど押された後に揺れ戻る。
    void SetDrive(bool enabled, float scale, float falloff, float damping);

    /// 根の剛体をアニメーションへ繋ぎ止める強さ。**Active では必須**。
    ///
    /// WHY 要るか: 関節は隣の骨との相対しか拘束しないので、根は何にも繋がっていない。
    ///     切ったまま立たせると、サーボが形を保ったまま全体が重力で落ちていく。
    ///
    /// @param scale 自重を支えるのに要る力への倍率。1.0 でぎりぎり、下げると胴が沈む。
    /// @param sag   上限の力を出し切るまでに沈む距離 [m]。押されたときの «遊び»。
    /// @param tilt  上限のトルクを出し切るまでに傾く角 [rad]。
    void SetRootAnchor(float scale, float sag, float tilt);

    void SetContactSettings(const RagdollContactSettings& settings)
    {
        m_contactSettings = settings;
    }
    [[nodiscard]] const RagdollContactSettings& GetContactSettings() const
    {
        return m_contactSettings;
    }

    /// このフレームの接触を作り直す。**Step の前に毎フレーム呼ぶ。**
    ///
    /// 形状判定は既存の `PhysicsSolver::NarrowPhase` へ丸投げする ─ カプセル vs
    /// 三角メッシュも地形も、こちらで書き直す理由が無い。
    ///
    /// @param world null で «世界と当たらない» (自己衝突と接地面だけになる)。
    void RefreshContacts(physics::World* world);
    /// 解かなかった相手 (World が積分している剛体) へ反作用を返す。**Step の後に呼ぶ。**
    void ApplyContactReactions();
    /// 当てないコライダー。自分の GameObject が持っているものを渡す ─
    /// «倒れたラグドールが自分の当たり判定と押し合う» を防ぐ。
    void SetIgnoredColliders(std::vector<const physics::Collider*> colliders);
    void SetIgnoredBody(physics::RigidBody* body) { m_ignoredBody = body; }
    [[nodiscard]] int GetContactCount() const { return static_cast<int>(m_contacts.size()); }

    /// 接地面 [m] と摩擦係数。世界にコライダーが無い場所での抜け止め。
    ///
    /// M3 で本物の接触が入ったので、通常は世界側が先に受け止める。この面は
    /// «取りこぼしたときに床下へ消えない» ためだけに残してある。
    void SetGround(float height, float friction);
    /// 抜け止めの面を «届かない所» へ下げる。世界の接触だけで受け止めたいときに使う。
    void DisableGround();
    /// 与えられた姿勢で、一番低い接触点が来る高さ [m]。
    /// 床をここより上に置くとアニメーション姿勢が床を破り、足が震える。
    [[nodiscard]] float LowestContactHeight(const std::vector<RagdollBonePose>& bones) const;

    /// 全剛体へ速度を足す。radius が 0 より大きければ、origin からの距離で減衰させる。
    void ApplyImpulse(const math::Vector3& origin,
                      const math::Vector3& velocity,
                      float                radius);

    /// 重力を変えるとサーボの実トルクも変わる ─ トルクは «自重を支えられるか» で
    /// 決めているので、重力が 2 倍になれば必要トルクも 2 倍になる。
    void SetGravity(const math::Vector3& gravity)
    {
        m_solver.SetGravity(gravity);
        ApplyDrive();
    }
    void SetSubsteps(int substeps) { m_solver.SetSubsteps(substeps); }
    /// 速度の減衰 [1/s]。空気抵抗ではなく «関節のこすれ» の代用。
    void SetDrag(float linear, float angular);

    void Step(float dt) { m_solver.Step(dt); }

    /// 剛体の姿勢を骨のワールド姿勢へ戻す。
    /// 剛体を持たない骨 (葉など) は、fallback の «親からの相対» を保って埋める。
    void WritePose(const std::vector<RagdollBonePose>& fallback,
                   std::vector<math::Vector3>&         outPositions,
                   std::vector<math::Quaternion>&      outRotations) const;

    /// 与えられた姿勢から一番離れた剛体の距離 [m]。«どれだけ効いているか» の物差し。
    [[nodiscard]] float MeasureDeviation(const std::vector<RagdollBonePose>& bones) const;
    /// 全剛体の重心 (質量加重)。バランス判定と «どちらへ倒れるか» に使う。
    [[nodiscard]] math::Vector3 CenterOfMass() const;
    /// トルク上限に張り付いている関節の数。0 でない ＝ どこかが力負けしている。
    [[nodiscard]] int CountSaturatedJoints() const;
    /// 可動域に食い込んで押し戻されている関節の数。
    /// 0 でない ＝ クリップが «曲げてよいことになっていない» 所まで曲げている。
    [[nodiscard]] int CountLimitedJoints() const;

    /// クリップが要求する関節角を測り、可動域をそこまで広げる。
    ///
    /// WHY 手で詰めないか: 可動域の基準はバインドポーズで、クリップがそこから
    ///     どれだけ動かすかは骨格とモーション次第。プロファイルに書いた «狭い可動域» が
    ///     クリップの動きより狭いと、サーボが目標へ行けず絵が崩れる ── 崩れているのは
    ///     物理ではなく «物理が許していない» だけ。クリップが実際に使う範囲を測れば、
    ///     どちら向きに曲がるか (可動域の符号) まで自動で決まる。
    ///
    /// 広げるだけで狭めない。«クリップが使う範囲は必ず可動域の内側» を保証する。
    ///
    /// @param margin 測った範囲へ足す余裕 [rad]。押されて少し越える分を吸収する。
    void SetLimitLearning(bool enabled, float margin);

    /// どの規則にも当たらず fallback になった骨の名前。Build() が埋める。
    ///
    /// WHY 名前で持つか: «剛体は組めたが可動域が全部 fallback の球関節» は、画面では
    ///     «なんとなく柔らかい» としか見えず、原因に辿り着けない。骨格を差し替えるたびに
    ///     起きる種類の失敗なので、どの骨が漏れたかを名指しできる形で残す。
    [[nodiscard]] const std::vector<std::string>& UnmatchedBones() const
    {
        return m_unmatchedBones;
    }

    [[nodiscard]] bool IsBuilt() const { return !m_bodies.empty(); }
    [[nodiscard]] int  GetBodyCount()  const { return static_cast<int>(m_bodies.size()); }
    [[nodiscard]] int  GetJointCount() const { return static_cast<int>(m_joints.size()); }
    /// 骨の添字 → 剛体の添字。-1 なら剛体を持たない骨。
    [[nodiscard]] int  BodyIndexOfBone(int boneIndex) const;
    /// 剛体を直接触る口。範囲外で nullptr。
    ///
    /// «この骨だけ固定する» (m_isStatic) が要る場面 ─ 掴んでぶら下がる (M6)、
    /// 腰を吊る演出、片持ち梁としての試験 ─ は、拘束を足すのではなく剛体側を
    /// 止める方が素直に書ける。
    [[nodiscard]] physics::RigidBody* GetBody(int bodyIndex) const;

    [[nodiscard]] physics::XPBDSolver& Solver() { return m_solver; }

    /// 剛体・可動域・接触をワイヤーで書き出す。
    /// 形状は RefreshContacts が毎フレーム同期しているので、ここでは読むだけ。
    void BuildDebugLines(std::vector<RagdollDebugLine>& out) const;

private:
    /// 剛体 1 個と、それが代表する骨との対応。
    struct BodyLink {
        int                                      boneIndex = -1;
        std::unique_ptr<physics::RigidBody>      body;
        std::unique_ptr<physics::CapsuleCollider> collider;
        /// カプセル両端の球。
        ///
        /// WHY カプセルだけで足りないか: NarrowPhase が返す接触点は形状の組ごとに
        ///     1 個で、寝かせた胴が «1 点で床に触れている» 状態になる。そこを軸に
        ///     くるくる回るので、両端でも取って 3 点で押さえる。球はカプセルの
        ///     内側にあるので、カプセルより深い接触を報告することはない。
        std::unique_ptr<physics::SphereCollider> caps[2];
        /// 親の剛体。自己衝突の «関節グラフ上の距離» を測るのに使う。-1 で根。
        int                                      parentBody = -1;
        /// 骨 = 剛体 × これ。捕獲と書き戻しはこの 2 つで往復する。
        math::Vector3    boneOffset = math::Vector3::ZERO;
        math::Quaternion boneRotation = math::Quaternion::Identity();
        float            mass = 1.0f;
    };

    /// 関節と、それが繋いでいる剛体の添字。所有は m_solver 側。
    struct JointLink {
        physics::XPBDJoint* joint      = nullptr;
        int                 parentBody = -1;
        int                 childBody  = -1;
        /// 根から何関節目か。ドライブの落とし方に使う。
        int                 depth      = 0;
        /// プロファイルが与えた素の設定。倍率はここへ掛ける ─ 現在値へ掛け続けると
        /// 毎フレーム縮んでいき、力み具合が «時間とともに» 変わってしまう。
        RagdollServo        baseServo;
        /// «この関節から先を、重力に対して真横へ伸ばした姿勢で支える» のに要る
        /// トルクの素 [kg·m]。実トルクは これ × |g| で、重力を変えれば追従する。
        float               holdMoment = 0.0f;
    };

    [[nodiscard]] physics::RigidBody* BodyOfBone(int boneIndex) const;
    /// 骨 i の «剛体としての» ワールド姿勢を、与えられた骨の姿勢から作る。
    void BodyPoseFromBone(int bodyIndex, const RagdollBonePose& bone,
                          math::Vector3& outPosition, math::Quaternion& outRotation) const;
    /// 今の倍率を全関節のドライブへ反映する。
    void ApplyDrive();
    /// この剛体は自分のものか。NarrowPhase が返した接触の «どちら側» かを判定する。
    [[nodiscard]] int OwnBodyIndex(const physics::RigidBody* body) const;
    /// 自己衝突を許す組か。先祖–子孫の距離と、組んだ時点での重なりで決まる。
    [[nodiscard]] bool AllowsSelfContact(int a, int b) const;
    void BuildShapeInstances();
    void CollectWorldPairs(physics::World& world);
    void CollectSelfPairs();
    void MakeContacts();

    std::vector<BodyLink>  m_bodies;
    std::vector<JointLink> m_joints;
    /// 接地拘束。剛体 1 個につきカプセルの両端の 2 個。所有は m_solver 側。
    std::vector<physics::XPBDPlaneContact*> m_ground;
    /// 親の関節を持たない剛体を、アニメーションへ繋ぎ止める。所有は m_solver 側。
    /// 添字は m_anchorBody と対。
    std::vector<physics::XPBDPoseAnchor*>   m_anchors;
    std::vector<int>                        m_anchorBody;
    /// 根 1 本ごとの «その先の» 総質量 [kg] と質量モーメント [kg·m]。繋ぎ止める力の素。
    ///
    /// WHY 根ごとに持つか: 骨の並びは 1 本の木とは限らない。繋がっていない部分木を
    ///     同じソルバへ載せる構成 (壊れた脚だけを落とす等) で全身の合計を配ると、
    ///     部分木 1 つの繋ぎ止めが «全部を支える力» になる。添字は m_anchors と対。
    std::vector<float> m_anchorMass;
    std::vector<float> m_anchorMoment;
    float m_totalMass  = 0.0f;
    bool  m_learnLimits = false;
    float m_limitMargin = 0.09f;
    float m_anchorScale = 3.0f;
    float m_anchorSag   = 0.04f;
    float m_anchorTilt  = 0.05f;
    /// 骨の添字 → m_bodies の添字。-1 で剛体なし。
    std::vector<int>                 m_boneToBody;
    physics::XPBDSolver              m_solver;

    /// 自分のカプセルと両端球を ColliderInstance にしたもの。NarrowPhase の入力。
    /// **CollisionPair が中身を指すので、Build 以外で再確保しないこと。**
    ///
    /// **剛体 1 個につき «カプセル → 端 → 端» の 3 個**。自己衝突が
    /// m_instances[i * 3] でカプセルを引くので、この並びは変えないこと。
    std::vector<physics::ColliderInstance> m_instances;
    /// このフレームの接触。実体はここが持ち、ソルバへは非所有ポインタで渡す。
    std::vector<physics::XPBDContact>      m_contacts;
    std::vector<physics::CollisionPair>    m_pairs;
    std::vector<physics::ContactPoint>     m_contactPoints;
    /// プロファイルのどの規則にも当たらなかった骨。Build() が集める。
    std::vector<std::string>               m_unmatchedBones;
    /// 組んだ時点で既に重なっている剛体の組。自己衝突から永久に外す。
    /// 添字は a * 剛体数 + b (a < b)。
    std::vector<bool>                      m_selfOverlapAtBuild;

    RagdollContactSettings                 m_contactSettings;
    std::vector<const physics::Collider*>  m_ignoredColliders;
    physics::RigidBody*                    m_ignoredBody = nullptr;

    /// プロファイルが有効にした関節を «全体として» 切るための master スイッチ。
    bool  m_driveEnabled  = true;
    float m_driveScale    = 1.0f;
    float m_driveFalloff  = 1.0f;
    float m_driveDamping  = 1.0f;
    /// SetGround を呼ぶまでは «届かない所» に置く。詳細は RagdollRig.cpp の kNoGround。
    float m_groundHeight  = -1.0e9f;
    float m_groundFriction = 0.9f;
};

} // namespace fbzz::scene
