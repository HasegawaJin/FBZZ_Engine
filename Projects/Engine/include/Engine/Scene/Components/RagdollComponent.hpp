/// @file    RagdollComponent.hpp
/// @brief   骨をそのまま質点系として落とす、一時的なラグドールの設定と実行状態
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
/// WHY 常時ではなく «倒れる間だけ» か:
///   ラグドールには筋肉が無く、四足が脱力したら二度と立てない。歩行と攻撃は
///   アニメーションが持ち、崩れる瞬間だけ物理へ渡して、静止したらクリップへ戻す。
///   予兆をフレーム単位で詰めてあるクリップ側の読みやすさを手放さずに、
///   «毎回違う倒れ方» だけを手に入れる。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fbzz::asset { struct Skeleton; }

namespace fbzz::scene {

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

    std::vector<RagdollParticle> particles;
    std::vector<RagdollLink>     links;
    std::vector<RagdollImpulse>  pendingImpulses;

    /// particles を組んだときのスケルトン。差し替わったら組み直す。
    const asset::Skeleton* builtSkeleton = nullptr;
    float groundHeight = 0.0f;

    int           runtimeParticleCount = 0;
    int           runtimeLinkCount     = 0;
    RagdollStatus runtimeStatus        = RagdollStatus::Idle;

    const char* GetTypeName() const { return "Ragdoll"; }

    [[nodiscard]] bool IsActive() const { return phase != RagdollPhase::Idle; }

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
    }
};

} // namespace fbzz::scene
