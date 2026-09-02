/// @file    RagdollComponent.hpp
/// @brief   骨を質点系として落とす / 筋力で支える、ラグドールの設定と実行状態
/// @author  Hasegawa Jin
/// @date    2026-09-01
///
/// WHY 剛体と制約で組まないか:
///   Physics の HingeConstraint はアンカー 2 点を同じ位置へ寄せるだけで軸を拘束せず、
///   位置を直接書き換えて速度を直さない。ロープなら破綻しないが、13 節を超える
///   関節体を «立たせたまま» 支えると伸びとジッターが出る。倒れる数秒だけが要るなら、
///   骨 1 本を 1 質点とする Verlet と距離拘束で足りる ─ 位置ベースの距離拘束は
///   反復するほど収束が保証される形なので、質量比にも刻みにも強い。
///
/// WHY 脱力 (Passive) と筋力 (Active) の 2 通りを持つか:
///   脱力したラグドールには筋肉が無く、四足が崩れたら二度と立てない。だから転倒は
///   «崩れる瞬間だけ» 物理へ渡して静止したらクリップへ戻す ── 予兆をフレーム単位で
///   詰めてあるクリップ側の読みやすさを手放さずに «毎回違う倒れ方» だけを得る。
///   これに対し «立っているボスを押す» は、脱力では倒れるしか結果が無い。
///   Active はアニメーションが今フレームに置いた骨の位置を «筋力の目標» として
///   毎フレーム取り直し、質点をそこへ引き戻し続ける。無負荷なら目標と一致するので
///   絵はクリップそのままで、押されたぶんだけ沈んで戻る。歩行と攻撃の読みやすさを
///   保ったまま、立ったまま «効いている絵» を出せる。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::asset { struct Skeleton; }

namespace fbzz::scene {

/// 質点を何が動かすか。起動ごとに決まる。
enum class RagdollMode : std::uint8_t {
    /// 脱力。捕獲した姿勢を初期値に、重力と距離拘束だけで崩れる。
    Passive = 0,
    /// 筋力。毎フレームのアニメーション姿勢を目標に取り直して引き戻し続ける。
    Active  = 1,
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
///     見つからない» «質点が 0 本» «ボーンの GameObject が引けない» と 4 通りあり、
///     症状はどれも «何も起きない» で同じ。黙って continue すると、どこを直せば
///     よいか画面から永久に分からない。
enum class RagdollStatus : std::uint8_t {
    Idle = 0,        ///< 止まっている (正常)
    Disabled,        ///< enabled が false
    NotPlaying,      ///< 編集中で simulateInEditor が false
    NoAnimator,      ///< 同じ GameObject に AnimatorComponent が無い
    NoSkinnedMesh,   ///< 自分と直下の子にスキンドメッシュが無い / スケルトン未読み込み
    NoParticles,     ///< 根ボーンが見つからず質点を 1 つも組めなかった
    NoBones,         ///< 質点に対応するボーンの GameObject が引けない
    Running,         ///< 解いて骨へ書いた
};

/// 骨 1 本ぶんの質点。シーンへは保存しない。
struct RagdollParticle {
    int nodeIndex       = -1;
    /// 同じ配列内の親の添字。-1 なら根。
    int parentParticle  = -1;
    /// 根から何段目か。Active の筋力は根から遠いほど弱い。
    int depth           = 0;
    /// この質点の筋力 [0,1]。1 ステップで姿勢差を詰める割合。Passive では 0。
    float muscle        = 0.0f;

    math::Vector3 position     = math::Vector3::ZERO;
    math::Vector3 prevPosition = math::Vector3::ZERO;
    /// 0 で固定。根を床へ縫い付けたいときに使う。
    float invMass = 1.0f;

    /// 捕獲した瞬間のワールド姿勢。回転はここからの «向きの差» で組み直す。
    math::Vector3    restDirection   = math::Vector3::ZERO;
    math::Quaternion captureRotation = math::Quaternion::Identity();
    math::Vector3    captureScale    = math::Vector3::ONE;
    /// 最初の子。回転を作る向きの相手。-1 なら葉。
    int   firstChild = -1;
    bool  hasRest    = false;

    math::Vector3    simPosition = math::Vector3::ZERO;
    math::Quaternion simRotation = math::Quaternion::Identity();
};

/// 質点 2 個を結ぶ距離拘束。
struct RagdollLink {
    int   a = -1;
    int   b = -1;
    float restLength = 0.0f;
    /// 1 で剛。曲がりを抑える «筋交い» は 1 より下げて、完全には固めない。
    float stiffness  = 1.0f;
};

/// 発生した衝撃。フレーム末で消費する。
struct RagdollImpulse {
    math::Vector3 origin    = math::Vector3::ZERO;
    math::Vector3 velocity  = math::Vector3::ZERO;
    /// 0 以下なら全質点へ一律に効く。
    float         radius    = 0.0f;
};

/// スキンドメッシュの骨を一時的に物理へ渡す。AnimatorSystem / IKSystem / SpringBoneSystem
/// の後段で解き、スキニング行列の最終書き込み者になる。
struct RagdollComponent {
    bool enabled = true;

    /// 落とし始める骨。空ならスケルトンの根から。
    std::string rootBoneName;
    /// 根から何段まで質点にするか。0 で葉まで。
    int maxDepth = 0;

    /// 重力加速度 [m/s^2]。実測値より重い方が «重機が崩れる» に寄る。
    float gravity = 26.0f;
    /// 1 ステップあたりの速度減衰 [0,1]。大きいほど早く静まる。
    float damping = 0.04f;
    /// 距離拘束の反復回数。少ないと脚が伸びる。
    int   iterations = 10;
    /// 関節を跨いだ «筋交い» の強さ [0,1]。0 で完全に脱力し、1 で折れ曲がらない。
    float braceStiffness = 0.55f;

    /// 接地判定に使う骨の太さ [m]。
    float boneRadius = 0.28f;
    /// 接地面の高さ。Begin() したときのオーナーの足元から決める。
    float groundOffset = 0.0f;
    /// 接地中の水平減衰 [0,1]。1 で滑らない。
    float groundFriction = 0.55f;

    /// アニメーションから物理へ移る秒数。0 で即座に切り替わる。
    float blendIn  = 0.06f;
    /// 物理からアニメーションへ戻る秒数。
    float blendOut = 0.40f;

    /// simulateInEditor 相当。既定では Play 中だけ動く。
    bool simulateInEditor = false;

    /// 根の質点が 1 ステップで姿勢差を詰める割合 [0,1]。Active 専用。
    ///
    /// WHY 力 [N] ではなく «詰める割合» か: 距離拘束も接地も位置を直接動かす解き方で
    ///     揃えてあり、そこへ力だけ別単位で混ぜると刻みが変わるたびに釣り合いが動く。
    ///     割合なら «何ステップで戻るか» が質量にも重力にも依らず読める。
    float muscleStiffness = 0.45f;
    /// 根から 1 段下がるごとに筋力へ掛かる倍率 [0,1]。
    ///
    /// WHY 一律にしないか: 全身を同じ強さで引くと骨がクリップへ張り付き、押しても
    ///     «少し遅れて同じ絵» にしかならない。腰を強く末端を弱くすると、体幹が支えて
    ///     手足だけが流れる ─ 押されて «こらえている» 形はこの差から出る。
    float muscleFalloff = 0.86f;
    /// Active 中の追加速度減衰 [0,1]。目標を追い越して揺れ戻るのを抑える。
    float muscleDamping = 0.18f;
    /// 衝撃を受けた瞬間に抜ける筋力の割合 [0,1]。0 で «押されても硬いまま»。
    float impactSlack = 0.60f;
    /// 抜けた筋力が元へ戻るまでの秒数。«こらえ直す» 時間。
    float recoverySeconds = 0.50f;
    /// 姿勢差がこの距離 [m] を超えたら筋力を捨てて脱力へ落ちる。0 で無効。
    ///
    /// WHY 要るか: 筋力は無限に強く、これが無いと «どれだけ殴っても最後は立っている»。
    ///     支え切れなくなったら倒れる、が入って初めて立っている絵に意味が出る。
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

    /// 今の筋力倍率 [0,1]。衝撃で 1-impactSlack まで落ち、recoverySeconds で 1 へ戻る。
    float muscleScale       = 1.0f;
    float recoveryRemaining = 0.0f;

    std::vector<RagdollParticle> particles;
    std::vector<RagdollLink>     links;
    std::vector<RagdollImpulse>  pendingImpulses;

    /// particles を組んだときのスケルトン。差し替わったら組み直す。
    const asset::Skeleton* builtSkeleton = nullptr;
    float groundHeight = 0.0f;
    /// 床へ足す骨の太さ [m]。起動時に決まり、その起動のあいだ動かない。
    float groundThickness = 0.0f;

    int           runtimeParticleCount = 0;
    int           runtimeLinkCount     = 0;
    RagdollStatus runtimeStatus        = RagdollStatus::Idle;
    /// アニメーション姿勢から一番離れた質点の距離 [m]。collapseDistance の判定値。
    float         runtimeDeviation     = 0.0f;

    const char* GetTypeName() const { return "Ragdoll"; }

    [[nodiscard]] bool IsActive() const { return phase != RagdollPhase::Idle; }
    /// 筋力で支えている最中か。
    [[nodiscard]] bool IsStanding() const
    {
        return mode == RagdollMode::Active && phase != RagdollPhase::Idle;
    }

    void Reflect(IReflector& r)
    {
        r.Field("enabled",          enabled);
        r.Field("rootBoneName",     rootBoneName);
        r.Field("maxDepth",         maxDepth);
        r.Field("gravity",          gravity);
        r.Field("damping",          damping);
        r.Field("iterations",       iterations);
        r.Field("braceStiffness",   braceStiffness);
        r.Field("boneRadius",       boneRadius);
        r.Field("groundOffset",     groundOffset);
        r.Field("groundFriction",   groundFriction);
        r.Field("blendIn",          blendIn);
        r.Field("blendOut",         blendOut);
        r.Field("simulateInEditor", simulateInEditor);
        r.Field("muscleStiffness",  muscleStiffness);
        r.Field("muscleFalloff",    muscleFalloff);
        r.Field("muscleDamping",    muscleDamping);
        r.Field("impactSlack",      impactSlack);
        r.Field("recoverySeconds",  recoverySeconds);
        r.Field("collapseDistance", collapseDistance);
        r.Field("activateOnStart",  activateOnStart);
    }
};

} // namespace fbzz::scene
