/// @file    RagdollComponent.hpp
/// @brief   骨を XPBD の関節体として落とす / サーボで支える、ラグドールの設定と実行状態
/// @author  Hasegawa Jin
/// @date    2026-09-01
///
/// @note 質点ではなく剛体+関節: 回転を状態に持つことで可動域・ツイスト・慣性・サーボの
///       トルク上限を «関節の性質» として書ける。XPBD の compliance [rad/(N·m)] は定常たわみが
///       刻みに依存しないため、フレームレートが変わっても同じ硬さになる。Passive (脱力) は
///       «崩れる瞬間» だけ物理へ渡し、Active (筋力) は毎フレームの関節角をサーボ目標に取り直す。
/// @see Docs/design/active-ragdoll.md
#pragma once

#include <Engine/Scene/Ragdoll/RagdollProfile.hpp>
#include <Engine/Scene/Ragdoll/RagdollRig.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fbzz::asset { struct Skeleton; }

namespace fbzz::scene {

/// 関節を何が動かすか。起動ごとに決まる。
enum class RagdollMode : std::uint8_t {
    /// 脱力。捕獲した姿勢を初期値に、重力と可動域だけで崩れる。
    Passive = 0,
    /// サーボ。毎フレームのアニメーション姿勢を目標に取り直して引き戻し続ける。
    Active  = 1,
};

/// どのプリセットで剛体と関節を組むか。可動域の広さ・サーボの硬さ・トルク上限が変わる。
enum class RagdollProfileKind : std::uint8_t {
    /// 重機・ロボット。狭い可動域、硬いドライブ、有限のサーボトルク。
    Mech     = 0,
    /// 人型。広い可動域、柔らかいドライブ。
    Humanoid = 1,
};

/// ラグドールの進行段階。
enum class RagdollPhase : std::uint8_t {
    Idle     = 0,
    BlendIn  = 1,
    Hold     = 2,
    BlendOut = 3,
};

/// 前回の更新で何をしたか。
/// @note 骨を動かせない理由が 4 通りあり症状はどれも «何も起きない» で同じ。黙って continue
///       すると、どこを直せばよいか画面から永久に分からないため理由を列挙する。
enum class RagdollStatus : std::uint8_t {
    Idle = 0,        ///< 止まっている (正常)
    Disabled,        ///< enabled が false
    NotPlaying,      ///< 編集中で simulateInEditor が false
    NoAnimator,      ///< 同じ GameObject に AnimatorComponent が無い
    NoSkinnedMesh,   ///< 自分と直下の子にスキンドメッシュが無い / スケルトン未読み込み
    NoParticles,     ///< 根ボーンが見つからず剛体を 1 つも組めなかった
    NoBones,         ///< 剛体に対応するボーンの GameObject が引けない
    Running,         ///< 解いて骨へ書いた
};

/// 実行中だけ存在する物理の実体と、それを組んだときの条件。
/// @note コピーで空になるのは、Inspector の自動対応がコピー可能な型を要求するため
///       (ComponentRegistry の static_assert)。剛体は単一所有で複製に意味が無く、
///       «条件が変わったら組み直す» 経路で次の更新に自然に組み上がる。
struct RagdollRuntime {
    /// 剛体・関節・接地拘束。組めるまで null。
    ///
    /// unique_ptr 越しに持つのは、内部で非所有ポインタ (ソルバ → 剛体、関節 → 剛体) が
    /// 絡むため。コンポーネントは配列の再確保で動く。
    std::unique_ptr<RagdollRig> rig;
    /// このフレームの FK 姿勢。捕獲元であり、サーボの目標であり、ブレンド先でもある。
    std::vector<RagdollBonePose> bones;
    /// bones[i] に対応するスケルトンのノード添字。
    std::vector<int>             boneNodes;
    /// bones[i] の FK スケール。物理は姿勢しか動かさないので、書き戻しはこれを使う。
    std::vector<math::Vector3>   boneScales;
    double remainingTime = 0.0;

    /// rig を組んだときの条件。1 つでも変わったら組み直す。
    const asset::Skeleton*   builtSkeleton = nullptr;
    std::size_t builtNodeCount = 0;
    math::Vector3 builtScale = math::Vector3::ONE;
    std::string              builtRoot;
    std::vector<std::string> builtExtraRoots;
    int                      builtMaxDepth = -1;
    RagdollProfileKind     builtProfile  = RagdollProfileKind::Mech;
    std::vector<std::string> builtExcludedRoots;

    RagdollRuntime()  = default;
    ~RagdollRuntime() = default;
    RagdollRuntime(RagdollRuntime&&)            = default;
    RagdollRuntime& operator=(RagdollRuntime&&) = default;

    RagdollRuntime(const RagdollRuntime&) {}
    RagdollRuntime& operator=(const RagdollRuntime&)
    {
        rig.reset();
        bones.clear();
        boneNodes.clear();
        boneScales.clear();
        remainingTime = 0.0;
        builtSkeleton = nullptr;
        builtNodeCount = 0;
        builtScale = math::Vector3::ONE;
        builtRoot.clear();
        builtExtraRoots.clear();
        builtMaxDepth = -1;
        builtProfile = RagdollProfileKind::Mech;
        builtExcludedRoots.clear();
        return *this;
    }
};

/// 発生した衝撃。フレーム末で消費する。
struct RagdollImpulse {
    math::Vector3 origin    = math::Vector3::ZERO;
    math::Vector3 velocity  = math::Vector3::ZERO;
    /// 0 以下なら全剛体へ一律に効く。
    float         radius    = 0.0f;
    math::Vector3 angularVelocity = math::Vector3::ZERO;
};

/// スキンドメッシュの骨を一時的に物理へ渡す。AnimatorSystem / IKSystem / SpringBoneSystem
/// の後段で解き、スキニング行列の最終書き込み者になる。
///
/// 剛体と関節の実体は RagdollRig が持つ。コンポーネントは配列の再確保で動くので、
/// 非所有ポインタが絡む RagdollRig は必ず unique_ptr 越しに持つ。
struct RagdollComponent {
    bool enabled = true;

    /// 落とし始める骨。空ならスケルトンの根から。
    std::string rootBoneName;
    /// 追加の根。互いに繋がっていない部分木を同時に落とすときに使う。
    /// @note «壊れた脚だけを脱力» のように体の一部だけ落としたい構成があり、根 1 本では
    ///       «脚 1 本» か «全身» しか選べないため。重複と他の根の下にある骨は Build 側で
    ///       落とす (同じ骨を 2 度剛体にすると 2 つが書き戻して震える)。
    std::vector<std::string> extraRootBones;
    /// 指定したボーンとその全子孫を除外する。名前はスケルトンのノード名。
    std::vector<std::string> excludedRootBones;
    /// 根から何段まで剛体にするか。0 で葉まで。すべての根へ同じ深さが掛かる。
    int maxDepth = 0;
    /// 骨の太さ・重さ・可動域・サーボ特性を骨名から決めるプリセット。
    RagdollProfileKind profile = RagdollProfileKind::Mech;

    /// 重力加速度 [m/s^2]。実測値より重い方が «重機が崩れる» に寄る。
    float gravity = 26.0f;
    /// 1 フレームを何回に割って解くか。増やすほど硬いサーボが安定する。
    /// @note 反復回数ではなく substep: 質量比の大きい関節連鎖は同じ刻みを何度も回しても
    ///       収束せず、刻みを小さくする方が効く (Macklin 2019)。
    int   substeps = 12;
    /// 速度の減衰 [1/s]。空気抵抗ではなく «関節のこすれ» の代用。
    float linearDrag  = 0.35f;
    float angularDrag = 0.60f;

    /// 骨の表面のクーロン摩擦係数。0 で氷の上、1 前後で «倒れた所で止まる»。
    /// 世界側のマテリアルとは PhysicsMaterial の合成規則で混ざる。
    float friction    = 0.9f;
    /// 骨の反発係数。ラグドールが跳ねると重さが消えるので、既定は 0。
    float restitution = 0.0f;

    /// 世界の地形・壁と当たる。
    bool contactWorld   = true;
    /// 世界の «動く» 剛体とも当たる。瓦礫を蹴る / 弾かれる。
    bool contactDynamic = true;
    /// 自分の骨どうしが当たる。腕や脚が胴を貫通しなくなる。
    bool contactSelf    = true;
    /// Active (サーボで支えている) 間も世界の接触を解くか。
    /// @note 既定で切る: 立っている間の足の置き所はクリップが決めており、床はその通りに
    ///       踏まれている前提。押し出しを重ねると足が半径ぶん浮いてサーボと釣り合い、
    ///       体が宙に浮くため。世界と噛み合わせたいのは «崩れてから» なので支えている間は
    ///       切る (抜け止めの水平面は Active でもクリップの最下点まで下がるので残る)。
    bool contactWhileActive = false;
    /// 関節グラフ上でこの段数以内の先祖・子孫とは当てない。1 で親子、2 で祖父–孫まで。
    int  selfSkip       = 2;

    /// 世界のコライダーを取りこぼしたときの抜け止めに、水平面を 1 枚張る。
    /// @note 実接触後も残すのは、床コライダーの置き忘れが «床下へ消えていく» という
    ///       最も分かりにくい壊れ方になるため。面はオーナー足元とクリップ最下点の低い方に
    ///       置くので実際の床がある限り何もしない。段差や多層の地形では切ること。
    bool  groundPlane  = true;
    /// 抜け止めの面の高さ。オーナーの足元からの相対 [m]。
    float groundOffset = 0.0f;

    /// アニメーションから物理へ移る秒数。0 で即座に切り替わる。
    float blendIn  = 0.06f;
    /// 物理からアニメーションへ戻る秒数。
    float blendOut = 0.40f;

    /// simulateInEditor 相当。既定では Play 中だけ動く。
    bool simulateInEditor = false;

    /// サーボのトルク上限と剛性に掛かる倍率。Active 専用。
    /// @note 絶対値 [N·m] ではなく無次元にするのは、適正なトルクが骨の長さと質量で
    ///       骨格ごとに二桁違い、それを決めるのはプロファイルの仕事だから。1 でプロファイル
    ///       どおり、下げるほど力負けする。
    float driveScale   = 1.0f;
    /// 根から 1 関節下がるごとに driveScale へ掛かる倍率 [0,1]。
    /// @note 一律にしないのは、全身を同じ強さで引くと骨がクリップへ張り付き «少し遅れて
    ///       同じ絵» になるため。腰を強く末端を弱くすると «こらえている» 形が出る。
    float driveFalloff = 0.90f;
    /// サーボの減衰に掛かる倍率。下げるほど目標を追い越して揺れ戻る。
    float driveDamping = 1.0f;
    /// クリップが要求する関節角を測り、可動域をそこまで広げる。
    /// @note 既定で入れるのは、プロファイルの可動域がクリップより狭いとサーボが目標へ行けず
    ///       «アニメーションが崩れる» ためで、崩しているのは «物理が許していない» こと。
    ///       切ると手で詰めることになる。
    bool  learnLimits = true;
    /// 測った範囲へ足す余裕 [度]。押されて少し越えるぶんを吸収する。
    float limitMargin = 5.0f;

    /// 根の骨をアニメーションへ繋ぎ止める力。自重を支えるのに要る力への倍率。
    /// @note 関節は隣の骨との相対しか拘束しないため根は何にも繋がっておらず、0 だと
    ///       サーボが形を保ったまま全体が重力で落ちる。1.0 でぎりぎり自重を支える。
    float rootAnchor = 3.0f;
    /// 繋ぎ止めが上限の力を出し切るまでに沈む距離 [m]。押されたときの «遊び»。
    float rootAnchorSag = 0.04f;
    /// 同じく、上限のトルクを出し切るまでに傾く角 [rad]。
    float rootAnchorTilt = 0.05f;

    /// 衝撃を受けた瞬間に抜ける力みの割合 [0,1]。0 で «押されても硬いまま»。
    float impactSlack = 0.60f;
    /// 抜けた力みが元へ戻るまでの秒数。«こらえ直す» 時間。
    float recoverySeconds = 0.50f;
    /// 姿勢差がこの距離 [m] を超えたらサーボを捨てて脱力へ落ちる。0 で無効。
    /// @note トルク上限があっても押し続けると «たわんだまま立っている» に落ち着くため、
    ///       支え切れなくなったら倒れる、が入って初めて立っている絵に意味が出る。
    float collapseDistance = 0.90f;
    /// 根位置・骨長を維持し、FK姿勢からの変位を制限する。駆動モードは変更しない。
    bool standingGuard = false;
    float standingMaxDistance = 0.15f;
    float standingMaxDegrees = 15.0f;
    /// Play に入った時点で Active を起動する。スクリプト無しで立ったまま効かせる口。
    bool activateOnStart = false;

    RagdollMode  mode   = RagdollMode::Passive;
    RagdollPhase phase  = RagdollPhase::Idle;
    /// 現在の適用率 [0,1]。0 で完全に FK ポーズ。
    float weight = 0.0f;

    /// この起動で Hold 中に到達する適用率 [0,1]。Begin() が決める。
    /// @note 1 固定にしないのは、被弾のよろめきは «歩いている絵の上に押された分だけ» 乗せたい
    ///       ため。1 で乗せるとクリップが消え、脚を運ぶ途中で足が止まって滑る。
    float activationWeight = 1.0f;
    /// この起動での重力倍率。
    /// @note よろめきで落とすのは、0.5 秒でも 26 m/s^2 は 3m 落ち «崩れ落ちてから戻る» という
    ///       溶けた動きに見えるため。よろめきは勢いが減衰して戻るだけでよく落下は要らない。
    float activationGravity = 1.0f;
    /// Hold に留まる残り秒数。0 以下なら End() が呼ばれるまで続く。
    float holdRemaining = 0.0f;
    float phaseTimer    = 0.0f;
    float phaseStartWeight = 0.0f;

    /// 次の更新で捕獲し直す。Begin() が立てる。
    bool beginRequested = false;
    /// 次の更新でブレンドアウトへ入る。End() が立てる。
    bool endRequested   = false;
    /// 次の更新から Active で走る。BeginActive() が立てる。
    bool activeRequested = false;
    /// activateOnStart を消費済み。Play を抜けるたびに戻る。
    bool startTriggered  = false;

    /// 今の力み倍率 [0,1]。衝撃で 1-impactSlack まで落ち、recoverySeconds で 1 へ戻る。
    float muscleScale       = 1.0f;
    float recoveryRemaining = 0.0f;

    std::vector<RagdollImpulse> pendingImpulses;

    RagdollRuntime runtime;

    /// 実際に使っている接地面の高さ [m]。Passive では起動時に決まり、その起動の
    /// あいだ動かない ─ 途中で上げると、崩れ落ちている体が一瞬で持ち上がる。
    float         runtimeGround     = 0.0f;
    int           runtimeBodyCount  = 0;
    int           runtimeJointCount = 0;
    /// トルク上限に張り付いている関節の数。0 でない ＝ どこかが力負けしている。
    int           runtimeSaturated  = 0;
    /// 可動域に食い込んでいる関節の数。0 でない ＝ クリップが «曲げてよいことに
    /// なっていない» 所まで曲げている。アニメーションが崩れる主因はたいていこちら。
    int           runtimeLimited    = 0;
    /// このフレームに解いた接触の数。0 のまま落ちていくなら世界に床コライダーが無い。
    int           runtimeContacts   = 0;
    /// プロファイルのどの規則にも当たらなかった骨の数。0 でないなら、その骨は可動域も
    /// 太さも fallback のまま ─ «膝が逆に折れない» のような設定が効いていない。
    /// 骨の名前は組み直したときにログへ出る。
    int           runtimeUnmatched  = 0;
    RagdollStatus runtimeStatus     = RagdollStatus::Idle;
    /// アニメーション姿勢から一番離れた剛体の距離 [m]。collapseDistance の判定値。
    float         runtimeDeviation  = 0.0f;

    const char* GetTypeName() const { return "Ragdoll"; }

    [[nodiscard]] bool IsActive() const { return phase != RagdollPhase::Idle; }
    /// サーボで支えている最中か。
    [[nodiscard]] bool IsStanding() const
    {
        return mode == RagdollMode::Active && phase != RagdollPhase::Idle;
    }

    [[nodiscard]] RagdollProfile ResolveProfile() const
    {
        return profile == RagdollProfileKind::Humanoid ? RagdollProfile::Humanoid()
                                                       : RagdollProfile::Mech();
    }

    void Reflect(IReflector& r)
    {
        r.Field("enabled",          enabled);
        r.Field("rootBoneName",     rootBoneName);
        r.ListField("extraRootBones", extraRootBones);
        r.ListField("excludedRootBones", excludedRootBones);
        r.Field("maxDepth",         maxDepth);
        {
            static constexpr const char* kProfileLabels[] = { "Mech", "Humanoid" };
            int kind = static_cast<int>(profile);
            r.Enum("profile", kind, kProfileLabels);
            profile = static_cast<RagdollProfileKind>(std::clamp(kind, 0, 1));
        }
        r.Field("gravity",          gravity);
        r.Field("substeps",         substeps);
        r.Field("linearDrag",       linearDrag);
        r.Field("angularDrag",      angularDrag);
        r.Field("friction",         friction);
        r.Field("restitution",      restitution);
        r.Field("contactWorld",     contactWorld);
        r.Field("contactDynamic",   contactDynamic);
        r.Field("contactSelf",      contactSelf);
        r.Field("contactWhileActive", contactWhileActive);
        r.Field("selfSkip",         selfSkip);
        r.Field("groundPlane",      groundPlane);
        r.Field("groundOffset",     groundOffset);
        r.Field("blendIn",          blendIn);
        r.Field("blendOut",         blendOut);
        r.Field("simulateInEditor", simulateInEditor);
        r.Field("driveScale",       driveScale);
        r.Field("driveFalloff",     driveFalloff);
        r.Field("driveDamping",     driveDamping);
        r.Field("learnLimits",      learnLimits);
        r.Field("limitMargin",      limitMargin);
        r.Field("rootAnchor",       rootAnchor);
        r.Field("rootAnchorSag",    rootAnchorSag);
        r.Field("rootAnchorTilt",   rootAnchorTilt);
        r.Field("impactSlack",      impactSlack);
        r.Field("recoverySeconds",  recoverySeconds);
        r.Field("collapseDistance", collapseDistance);
        r.Field("standingGuard", standingGuard);
        r.Field("standingMaxDistance", standingMaxDistance);
        r.Field("standingMaxDegrees", standingMaxDegrees);
        r.Field("activateOnStart",  activateOnStart);
    }
};

} // namespace fbzz::scene
