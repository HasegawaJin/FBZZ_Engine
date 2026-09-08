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

#include <string>
#include <vector>

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
    /// 各関節のサーボは毎フレームのアニメーション姿勢を目標に取り直して引き戻される。
    /// 押されていない間は目標と一致するので絵はクリップそのまま ── 適用率は 1 でよい。
    ///
    /// @param holdSeconds 0 より大きければその秒数で自動的にブレンドアウトへ入る。
    ///                    0 なら End() を呼ぶまで、あるいは支え切れずに崩れるまで続く。
    void BeginActive(float holdSeconds = 0.0f) const;
    /// アニメーションへ戻し始める。blendOut 秒かけて FK ポーズへ寄る。
    void End() const;

    /// 落とす部分木の根をまとめて差し替える。RagdollComponent が無ければ追加する。
    ///
    /// 互いに繋がっていない骨を並べてよい ─ «壊れた脚だけを脱力させる» のように、
    /// 体の一部をいくつか落とす構成のための口。空の並びを渡すと骨格の根 1 本に戻る。
    ///
    /// WHY 1 つずつ足す口にしないか: 根が変わるとリグを組み直すので、1 本ずつ足すと
    ///     «足した回数だけ組み直す» ことになる。壊れた脚が 2 本目・3 本目と増える
    ///     場面がまさにそれなので、いま落としたい全部を 1 回で渡す形にする。
    ///     走っている最中に呼んでも、組み直した剛体はその時の姿勢で捕獲されるので
    ///     既に垂れている部分木が跳ね上がることはない。
    ///
    /// @param maxDepth 各根から何段まで剛体にするか。0 で葉まで。
    void SetRoots(const std::vector<std::string>& rootBoneNames, int maxDepth = 0) const;
    /// 今 落とす対象になっている根の数。0 なら骨格の根から (＝全身)。
    [[nodiscard]] int GetRootCount() const;

    /// 全ての剛体へ一律に速度を足す [m/s]。倒す «向き» はここで決める。
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
    /// 組めた剛体の数。0 なら根ボーンが見つかっていない。
    [[nodiscard]] int GetParticleCount() const;
    /// サーボのトルク上限に張り付いている関節の数。
    ///
    /// 0 でない ＝ どこかが «力負けしている»。押しても動かないなら Muscle が強すぎ、
    /// 常に 0 でないなら弱すぎる ─ ロボット感の調整はこの数を見ながら詰める。
    [[nodiscard]] int GetSaturatedJointCount() const;
    /// 可動域に食い込んでいる関節の数。
    ///
    /// «アニメーションが崩れる» の原因を切り分ける値。0 でないなら、崩しているのは
    /// 物理ではなく «物理が許していない» 方 ── クリップが可動域の外まで曲げている。
    [[nodiscard]] int GetLimitedJointCount() const;
    /// このフレームに解いた接触の数。落ちていくのに 0 のままなら、世界に床コライダーが無い。
    [[nodiscard]] int GetContactCount() const;
    /// プロファイルのどの規則にも当たらなかった骨の数。
    ///
    /// 0 でないなら、その骨は可動域も太さも fallback のまま ＝ «膝が逆に折れない» の
    /// ような設定が効いていない。骨の名前は組み直したときにログへ出る。
    [[nodiscard]] int GetUnmatchedBoneCount() const;

    /// 落とし始める骨と、そこから何段まで落とすか (0 で葉まで)。
    void SetRoot(const char* rootBoneName, int maxDepth = 0) const;
    void SetGravity(float gravity) const;
    void SetBlend(float blendIn, float blendOut) const;

    /// サーボの効き方。**どれもプロファイルが与えた値への «倍率»** で、1 が既定。
    ///
    /// @param stiffness トルク上限と剛性に掛かる倍率。下げるほど早く力負けする。
    /// @param falloff   根から 1 関節下がるごとの倍率 [0,1]。下げるほど末端が流れる。
    /// @param damping   サーボの減衰に掛かる倍率。下げるほど押された後に揺れ戻る。
    void SetMuscle(float stiffness, float falloff, float damping) const;
    /// 押された瞬間の力み具合。
    ///
    /// @param impactSlack     衝撃で抜けるサーボ出力の割合 [0,1]。0 で «押されても硬い»。
    /// @param recoverySeconds 抜けた力みが戻るまでの秒数 ＝ こらえ直す時間。
    void SetRecovery(float impactSlack, float recoverySeconds) const;
    /// 姿勢差がこの距離 [m] を超えたら脱力へ落ちる。0 で «何をされても立っている»。
    void SetCollapse(float distance) const;

    void SetEnabled(bool enabled) const;
    [[nodiscard]] bool IsEnabled() const;
};

} // namespace fbzz::scene
