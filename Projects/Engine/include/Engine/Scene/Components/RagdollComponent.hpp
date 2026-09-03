/// @file    RagdollComponent.hpp
/// @brief   骨を XPBD の関節体として落とす / サーボで支える、ラグドールの設定と実行状態
/// @author  Hasegawa Jin
/// @date    2026-09-01
///
/// WHY 質点系ではなく剛体と関節で組むか:
///   骨 1 本を質点 1 個にすると、状態として持てるのは位置の 3 自由度だけになる。
///   骨の姿勢は隣の質点への «向き» から後付けで組み立てるしかなく、ツイストが構造的に
///   作れない。角度制限も «制限したい量» が状態として存在しないので書けず、慣性も
///   質量分布も無い。回転を状態として持つ剛体にすると、可動域・ねじれ・慣性・
///   サーボのトルク上限がすべて «関節の性質» として素直に書ける。
///   設計の全体は Docs/design/active-ragdoll.md を参照。
///
/// WHY XPBD か:
///   «1 ステップで詰める割合» で解くと、定常たわみが刻みの 2 乗に比例して変わる。
///   compliance [rad/(N·m)] なら定常たわみが compliance × トルクで刻みに依らないので、
///   フレームレートが変わっても «同じ硬さ» が保てる。
///
/// WHY 脱力 (Passive) と筋力 (Active) の 2 通りを持つか:
///   脱力したラグドールには筋肉が無く、四足が崩れたら二度と立てない。だから転倒は
///   «崩れる瞬間だけ» 物理へ渡して静止したらクリップへ戻す ── 予兆をフレーム単位で
///   詰めてあるクリップ側の読みやすさを手放さずに «毎回違う倒れ方» だけを得る。
///   これに対し «立っているボスを押す» は、脱力では倒れるしか結果が無い。
///   Active はアニメーションが今フレームに置いた関節角を «サーボの目標» として
///   毎フレーム取り直す。無負荷なら目標と一致するので絵はクリップそのままで、
///   押されたぶんだけ沈んで戻る。実装上の違いはドライブを解くかどうかだけになる。
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
///
/// WHY 要るか: 骨を 1 本も動かせない理由が «Animator が居ない» «スキンドメッシュが
///     見つからない» «剛体が 0 個» «ボーンの GameObject が引けない» と 4 通りあり、
///     症状はどれも «何も起きない» で同じ。黙って continue すると、どこを直せば
///     よいか画面から永久に分からない。
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
///
/// WHY コピーすると空になるか: Inspector の自動対応はコンポーネントがコピー可能である
///     ことを要求する (ComponentRegistry の static_assert)。剛体は単一所有で複製に意味が
///     無く、複製先で組み直せばよい ── «条件が変わったら組み直す» 経路は元々あるので、
///     写した先を空にしておけば次の更新で自然に組み上がる。
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

    /// rig を組んだときの条件。1 つでも変わったら組み直す。
    const asset::Skeleton* builtSkeleton = nullptr;
    std::string            builtRoot;
    int                    builtMaxDepth = -1;
    RagdollProfileKind     builtProfile  = RagdollProfileKind::Mech;

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
        builtSkeleton = nullptr;
        builtRoot.clear();
        builtMaxDepth = -1;
        return *this;
    }
};

/// 発生した衝撃。フレーム末で消費する。
struct RagdollImpulse {
    math::Vector3 origin    = math::Vector3::ZERO;
    math::Vector3 velocity  = math::Vector3::ZERO;
    /// 0 以下なら全剛体へ一律に効く。
    float         radius    = 0.0f;
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
    /// 根から何段まで剛体にするか。0 で葉まで。
    int maxDepth = 0;
    /// 骨の太さ・重さ・可動域・サーボ特性を骨名から決めるプリセット。
    RagdollProfileKind profile = RagdollProfileKind::Mech;

    /// 重力加速度 [m/s^2]。実測値より重い方が «重機が崩れる» に寄る。
    float gravity = 26.0f;
    /// 1 フレームを何回に割って解くか。増やすほど硬いサーボが安定する。
    ///
    /// WHY 反復回数ではなく substep か: 位置射影を同じ刻みで何度も回しても、質量比の
    ///     大きい関節連鎖は収束しない。刻みを小さくする方が効く (Macklin 2019)。
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
    ///
    /// WHY 既定で切るか: 立っている間 «足をどこに置くか» を決めているのはクリップで、
    ///     床はその通りに踏まれている前提で作ってある。そこへカプセルの半径ぶんの
    ///     押し出しを重ねると、足が半径だけ浮いた所でサーボと釣り合い、体が宙に浮く。
    ///     世界と噛み合わせたいのは «崩れてから» なので、支えている間は切っておく。
    ///     抜け止めの水平面は Active でもクリップの最下点まで下がるので、こちらは残る。
    bool contactWhileActive = false;
    /// 関節グラフ上でこの段数以内の先祖・子孫とは当てない。1 で親子、2 で祖父–孫まで。
    int  selfSkip       = 2;

    /// 世界のコライダーを取りこぼしたときの抜け止めに、水平面を 1 枚張る。
    ///
    /// WHY 実接触が入った後も残すか: 床コライダーを置き忘れたシーンで «床下へ
    ///     消えていく» のは、原因が最も分かりにくい壊れ方になる。面は «オーナーの
    ///     足元» と «今のクリップの最下点» の低い方に置くので、実際の床がある限り
    ///     世界側が先に受け止め、この面は何もしない。**段差や多層の地形では切る。**
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
    ///
    /// WHY 絶対値 [N·m] で持たないか: 適正なトルクは骨の長さと質量で決まり、骨格ごとに
    ///     二桁違う。それを決めるのはプロファイルの仕事で、ここが持つのは «今どれだけ
    ///     力んでいるか» という無次元の量。1 でプロファイルどおり、下げるほど力負けする。
    float driveScale   = 1.0f;
    /// 根から 1 関節下がるごとに driveScale へ掛かる倍率 [0,1]。
    ///
    /// WHY 一律にしないか: 全身を同じ強さで引くと骨がクリップへ張り付き、押しても
    ///     «少し遅れて同じ絵» にしかならない。腰を強く末端を弱くすると、体幹が支えて
    ///     手足だけが流れる ─ 押されて «こらえている» 形はこの差から出る。
    float driveFalloff = 0.90f;
    /// サーボの減衰に掛かる倍率。下げるほど目標を追い越して揺れ戻る。
    float driveDamping = 1.0f;
    /// クリップが要求する関節角を測り、可動域をそこまで広げる。
    ///
    /// WHY 既定で入れるか: 可動域はバインドポーズが基準で、クリップがそこからどれだけ
    ///     動かすかは骨格とモーション次第。プロファイルの値がクリップより狭いと、
    ///     サーボが目標へ行けず «アニメーションが崩れる» ── 崩しているのは物理ではなく
    ///     «物理が許していない» ことの方。切ると手で詰めることになる。
    bool  learnLimits = true;
    /// 測った範囲へ足す余裕 [度]。押されて少し越えるぶんを吸収する。
    float limitMargin = 5.0f;

    /// 根の骨をアニメーションへ繋ぎ止める力。自重を支えるのに要る力への倍率。
    ///
    /// WHY 要るか: 関節は隣の骨との相対しか拘束しないので、根は何にも繋がっていない。
    ///     0 にして立たせると、サーボが形を保ったまま全体が重力で落ちていく。
    ///     1.0 でぎりぎり自重を支え、下げるほど胴が沈む。
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
    ///
    /// WHY 要るか: サーボにトルク上限があっても、押し続ければ «たわんだまま立っている»
    ///     に落ち着く。支え切れなくなったら倒れる、が入って初めて立っている絵に意味が出る。
    float collapseDistance = 0.90f;
    /// Play に入った時点で Active を起動する。スクリプト無しで立ったまま効かせる口。
    bool activateOnStart = false;

    RagdollMode  mode   = RagdollMode::Passive;
    RagdollPhase phase  = RagdollPhase::Idle;
    /// 現在の適用率 [0,1]。0 で完全に FK ポーズ。
    float weight = 0.0f;

    /// この起動で Hold 中に到達する適用率 [0,1]。Begin() が決める。
    ///
    /// WHY 1 固定にしないか: 転倒は体を丸ごと物理へ渡すが、被弾のよろめきは
    ///     «歩いている絵の上に押された分だけ» 乗せたい。1 で乗せるとクリップが
    ///     完全に消えるので、脚を運んでいた途中で足が止まって滑る。
    float activationWeight = 1.0f;
    /// この起動での重力倍率。
    ///
    /// WHY よろめきで重力を落とすか: 0.5 秒でも 26 m/s^2 は 3m 落ちる。薄く乗せても
    ///     «崩れ落ちてから戻る» になり、押された動きではなく溶けた動きに見える。
    ///     よろめきは «押された勢いが減衰して戻る» だけでよく、落下は要らない。
    float activationGravity = 1.0f;
    /// Hold に留まる残り秒数。0 以下なら End() が呼ばれるまで続く。
    float holdRemaining = 0.0f;
    float phaseTimer    = 0.0f;

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
        r.Field("activateOnStart",  activateOnStart);
    }
};

} // namespace fbzz::scene
