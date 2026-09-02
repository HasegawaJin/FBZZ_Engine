/// @file    ScriptRagdollProxy.hpp
/// @brief   Script から RagdollComponent を起動・停止し、衝撃を与えるプロキシ
/// @author  Hasegawa Jin
/// @date    2026-09-01
///
/// Begin() は «倒れる数秒» のための一時状態 ── 今の姿勢を捕獲し、脱力して崩れる。
/// BeginActive() は立ったまま走らせる ── 筋力が毎フレームのアニメーション姿勢へ
/// 引き戻し続けるので、無負荷ならクリップそのままの絵で、押されたぶんだけ動く。
/// どちらも End() でアニメーションへ戻る。
#pragma once

#include <Math/Vector3.hpp>

namespace fbzz::scene {

class Script;

struct ScriptRagdollProxy {
    Script* script = nullptr;

    /// 今の姿勢を捕獲して落とし始める。RagdollComponent が無ければ追加する。
    ///
    /// @param holdSeconds  0 より大きければその秒数で自動的にブレンドアウトへ入る。
    /// @param maxWeight    Hold 中の適用率 [0,1]。1 で完全に物理、0.4 なら
    ///                     «再生中のクリップの上に押された分だけ乗る» (よろめき)。
    /// @param gravityScale この起動での重力倍率。よろめきは 0 に近い値にしないと、
    ///                     押された動きではなく «崩れ落ちて戻る» 動きになる。
    void Begin(float holdSeconds = 0.0f,
               float maxWeight = 1.0f,
               float gravityScale = 1.0f) const;
    /// 立ったまま物理を効かせ始める (アクティブラグドール)。
    ///
    /// 質点は毎フレームのアニメーション姿勢を目標に取り直して引き戻される。押されて
    /// いない間は目標と一致するので絵はクリップそのまま ── 適用率は 1 のままでよい。
    ///
    /// @param holdSeconds 0 より大きければその秒数で自動的にブレンドアウトへ入る。
    ///                    0 なら End() を呼ぶまで、あるいは支え切れずに崩れるまで続く。
    void BeginActive(float holdSeconds = 0.0f) const;
    /// アニメーションへ戻し始める。blendOut 秒かけて FK ポーズへ寄る。
    void End() const;

    /// 全ての質点へ一律に速度を足す [m/s]。倒す «向き» はここで決める。
    void Push(const math::Vector3& velocity) const;
    /// origin から radius の内側だけに、距離で減衰させた速度を足す。
    void PushAt(const math::Vector3& origin,
                const math::Vector3& velocity,
                float radius) const;

    [[nodiscard]] bool  IsActive() const;
    /// 筋力で支えている最中か。押されて崩れた瞬間に false へ落ちる。
    [[nodiscard]] bool  IsStanding() const;
    /// 適用率 [0,1]。0 で完全にアニメーション、1 で完全に物理。
    [[nodiscard]] float GetWeight() const;
    /// アニメーション姿勢から一番離れた骨の距離 [m]。«どれだけ効いているか»。
    [[nodiscard]] float GetDeviation() const;

    /// 前回の更新で何をしたか。骨が動かないときに理由を名指しする
    /// ("NoAnimator" / "NoSkinnedMesh" / "NoParticles" / "NoBones" / "Running" など)。
    /// 戻り値は静的な文字列なので寿命を気にしなくてよい。
    [[nodiscard]] const char* GetStatus() const;
    /// 組めた質点の数。0 なら根ボーンが見つかっていない。
    [[nodiscard]] int GetParticleCount() const;

    /// 落とし始める骨と、そこから何段まで落とすか (0 で葉まで)。
    void SetRoot(const char* rootBoneName, int maxDepth = 0) const;
    void SetGravity(float gravity) const;
    void SetBlend(float blendIn, float blendOut) const;

    /// 筋力の強さ。
    ///
    /// @param stiffness 根の骨が 1 ステップ (1/60 秒) で姿勢差を詰める割合 [0,1]。
    /// @param falloff   根から 1 段下がるごとの倍率 [0,1]。下げるほど末端が流れる。
    /// @param damping   追加の速度減衰 [0,1]。下げるほど押された後に揺れ戻る。
    void SetMuscle(float stiffness, float falloff, float damping) const;
    /// 押された瞬間の力み具合。
    ///
    /// @param impactSlack     衝撃で抜ける筋力の割合 [0,1]。0 で «押されても硬い»。
    /// @param recoverySeconds 抜けた筋力が戻るまでの秒数 ＝ こらえ直す時間。
    void SetRecovery(float impactSlack, float recoverySeconds) const;
    /// 姿勢差がこの距離 [m] を超えたら脱力へ落ちる。0 で «何をされても立っている»。
    void SetCollapse(float distance) const;

    void SetEnabled(bool enabled) const;
    [[nodiscard]] bool IsEnabled() const;
};

} // namespace fbzz::scene
