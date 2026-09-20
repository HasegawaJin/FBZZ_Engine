/// @file    CombatManagerComponent.hpp
/// @brief   ダメージの適用を 1 箇所に集め、戦果を数える
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// @note ダメージは誰が誰へ何点入れるかをこの 1 本の API へ通す。敵の接触攻撃が
///       `PlayerComponent::TakeDamage()` を直接呼ぶ経路と共存すると、無敵時間や
///       被ダメージの記録のような要求が必ず 2 箇所の編集になり片方が漏れる。
/// @note ダメージの「値」は持たない。耐久・攻撃力は敵の種類や攻撃する側の性質で、
///       ここは適用と集計だけを担う。
/// @note `GameFlowComponent` から切り出す。戦闘の解決 (ダメージ式) は頻繁に変わるが
///       進行管理 (勝敗・遷移) はほぼ変わらず、同居すると互いの変更に巻き込み合う。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/CharacterEvent.hpp>
#include <Scripts/Combat/EnemyHealthComponent.hpp>
#include <Scripts/Combat/EyeSpriteComponent.hpp>
#include <Scripts/Combat/IDamageable.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

using namespace fbzz::scene;

namespace sandbox {

class CombatManagerComponent : public Script {
    FBZZ_SCRIPT(CombatManagerComponent)

public:
    /// @note チェインの長さはここが持つ。表示側 (HUD) に持たせると、HUD を消しただけで
    ///       «途切れずに手を出し続けたか» の数え方まで消える。
    FBZZ_GROUP("Chain")
    FBZZ_FIELD_RANGE(float, chainSeconds, 2.2f, "Chain Window", 0.2f, 10.0f)
    FBZZ_TOOLTIP("次の一撃がこの秒数以内なら同じチェインとして数える")
    FBZZ_FIELD_RANGE_INT(int, highChainCount, 4, "High Chain At", 2, 20)
    FBZZ_TOOLTIP("この長さに達したチェインは «伸びた» 側の音に変わる。"
                 "2 に下げると連鎖のたびに鳴るので、めったに出ない長さを置く")

    /// @note 倍率は転倒中だけに乗せる。立っているボスが斬撃で削れると «弾いて崩す» という
    ///       手順自体が要らなくなるため、«立っている間は弾く・転んだら叩き込む» の担当を
    ///       割ったまま、とどめの手応えだけを増やす。
    FBZZ_GROUP("Blade Chain")
    FBZZ_FIELD_RANGE(float, bladeRushStep, 0.08f, "Rush / Hit", 0.0f, 1.0f)
    FBZZ_TOOLTIP("転倒中、連鎖が 1 つ伸びるごとに斬撃ダメージへ加算する割合。0 で切る")
    FBZZ_FIELD_RANGE(float, bladeRushMax, 1.6f, "Rush Cap x", 1.0f, 4.0f)

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugKills, 0, "Kills")
    FBZZ_FIELD_READ_ONLY(int, debugPushKills, 0, "押しで倒す")
    FBZZ_TOOLTIP("ハザードへ押し込んで落とした数。0 のままなら押しが道具として "
                 "成立していない (Docs/development-plan.md の物差し)")
    FBZZ_FIELD_READ_ONLY(int, debugDamageToEnemies, 0, "Damage Dealt")
    FBZZ_FIELD_READ_ONLY(int, debugDamageToPlayer, 0, "Damage Taken")
    FBZZ_FIELD_READ_ONLY(int, debugChain, 0, "Chain")
    FBZZ_FIELD_READ_ONLY(int, debugChainHits, 0, "Chain Hits")
    FBZZ_FIELD_READ_ONLY(int, debugBestChain, 0, "Best Chain")
    FBZZ_FIELD_READ_ONLY(int, debugPerfectDodges, 0, "Perfect Dodges")
    FBZZ_FIELD_READ_ONLY(int, debugCadence, 0, "Cadence")
    FBZZ_FIELD_READ_ONLY(int, debugFaces, 0, "Faces")
    FBZZ_TOOLTIP("出来事を受け取れる目の数。想定より少なければ、そのキャラクターに "
                 "EyeSpriteComponent が付いていない")

    /// 誰から呼ばれるかは決まっていない (敵 AI・罠・ハザード)。置き場所に依存しない窓口を持つ。
    [[nodiscard]] static CombatManagerComponent* Instance() { return s_instance; }

    [[nodiscard]] int Kills() const { return m_kills; }
    [[nodiscard]] int DamageDealt() const { return m_damageToEnemies; }
    [[nodiscard]] int DamageTaken() const { return m_damageToPlayer; }
    /// ハザードへ押し込んで落とした数。ランク評価の «押し» の軸。
    [[nodiscard]] int PushKills() const { return m_pushKills; }
    /// 削り切ったのではなく «落として倒した» 経路が申告する。
    void AddPushKill() { ++m_pushKills; debugPushKills = m_pushKills; }

    /// 続いている連鎖の長さ。途切れると 0 に戻る。
    [[nodiscard]] int ChainCount() const { return m_chain; }
    /// このプレイでの最長。リザルトで使う。
    [[nodiscard]] int BestChain() const { return m_bestChain; }

    /// この連鎖でここまでに «当たった数»。深さ (m_chain) とは別に数える。
    [[nodiscard]] int ChainHits() const { return m_chainHits; }

    /// 斬撃が当たった。弾きと同じ連鎖として数える (表示・ランク・猶予は共通)。
    /// @param hits この一振りで当たった数 (溜め斬りは複数)。
    void RegisterBladeChain(int hits);
    void RegisterBladeHit(int hits, bool rush)
    {
        if (hits <= 0) return;
        ++m_bladeHitSerial;
        if (rush) ++m_rushHitSerial;
        RegisterBladeChain(hits);
    }
    [[nodiscard]] int BladeHitSerial() const { return m_bladeHitSerial; }
    [[nodiscard]] int RushHitSerial() const { return m_rushHitSerial; }
    /// 転倒中の斬撃に乗せる倍率。連鎖が続いているほど重い。転倒していなければ呼ばない。
    [[nodiscard]] float BladeRushMultiplier() const
    {
        const int links = m_chainHits > 1 ? m_chainHits - 1 : 0;
        const float raw = 1.0f + std::max(bladeRushStep, 0.0f) * static_cast<float>(links);
        return std::min(raw, std::max(bladeRushMax, 1.0f));
    }

    /// ジャスト回避 (回避中に攻撃を弾いた) を 1 回数える。PlayerComponent が申告する。
    void AddPerfectDodge() { ++m_perfectDodges; debugPerfectDodges = m_perfectDodges; }
    [[nodiscard]] int PerfectDodges() const { return m_perfectDodges; }

    /// @name 拍
    /// @{
    /// 連撃がいま何段 «拍に乗って» 繋がっているか。剣が振り出しのたびに申告する (途切れたら 0)。
    ///
    /// @note 拍を数えるのは剣 (Player の内部モジュール) だが、HUD がプレイヤーの内部を
    ///       探しに行くと表示の都合でプレイヤーの構成を知ることになるため、連鎖の隣に
    ///       置いて HUD はここ 1 か所だけを読めばよい形にする。
    void ReportCadence(int cadence, bool onBeat)
    {
        m_cadence = std::max(cadence, 0);
        if (onBeat) ++m_beatSerial;
        debugCadence = m_cadence;
    }
    /// 締めまで全段を拍に乗せて当てた。
    void ReportPerfectCadence()
    {
        ++m_perfectSerial;
        ++m_perfectCadences;
    }
    [[nodiscard]] int Cadence() const { return m_cadence; }
    /// 拍に乗った回数 / 揃えた締めの回数。HUD は前フレームとの差で «今起きた» を取る
    /// (通知にすると、読む側の更新順で取り逃がす)。
    [[nodiscard]] int BeatSerial() const { return m_beatSerial; }
    [[nodiscard]] int PerfectSerial() const { return m_perfectSerial; }
    [[nodiscard]] int PerfectCadences() const { return m_perfectCadences; }

    /// 連鎖が途切れるまでの残り (1 → 直後 / 0 → 途切れた)。表示の減衰に使う。
    [[nodiscard]] float ChainRemaining01() const
    {
        return chainSeconds <= 0.0f ? 0.0f
                                    : std::clamp(m_chainRemaining / chainSeconds, 0.0f, 1.0f);
    }

    /// 敵を 1 体倒した瞬間に呼ばれる。撃破の演出を挿す口。
    std::function<void()> onEnemyDefeated;

    /// プレイヤーへのダメージ。敵の接触攻撃など、量が決まっている経路はここを通す。
    /// 実際に減ったら true。無敵時間などで弾かれた場合は false。
    /// @param source 押し出しの起点 (当たった物の位置)。渡すと «そこから離れる» 向きへ
    ///               押す。nullptr なら押さない。
    /// @note 押す / 押さないは «殴られた» という 1 つの出来事の一部なのでここで配る。
    ///       敵ごとに書くと «この敵だけ押されない» が普通に起きる (ImpactFeedbackManager
    ///       が揺れと振動を 1 箇所で配っているのと同じ形)。
    bool DamagePlayer(GameObject* player, int amount, const Vector3* source = nullptr);

    /// プレイヤーへの一撃。弾けるかどうかを渡し、どう終わったかを受け取る。
    /// 踏みつけ・突進・噛みつきのように «弾かれたら反応する» 攻撃はこちらを使う。
    ///
    /// @note DamagePlayer と分ける。bool だと «無敵で通らなかった» と «弾き返された» が
    ///       同じになり、踏みつけを弾かれたボスの脚を跳ね上げる反応が組めない。
    PlayerHitResult HitPlayer(GameObject* player, int amount, const Vector3* source,
                              PlayerHitKind kind);

    /// 弾いた回数。リザルトの伸びしろとして運ぶ。
    [[nodiscard]] int Parries() const { return m_parries; }

    FBZZ_GROUP("Knockback")
    FBZZ_FIELD_RANGE(float, playerKnockSpeed, 7.0f, "速さ", 0.0f, 30.0f)
    FBZZ_TOOLTIP("被弾で押し出される初速 [m/s]。0 で押さない")
    FBZZ_FIELD_RANGE(float, playerKnockSeconds, 0.22f, "継続時間", 0.0f, 1.5f)
    FBZZ_TOOLTIP("押される時間。長いほど «操作を取り上げられた» に寄るので短く")

    /// 量が決まった後の敵へのダメージ。ハザードや自壊のように、式を持たない経路が通る。
    /// この呼び出しで倒したら true。
    bool DamageEnemyDirect(GameObject* target, int amount);

    /// character の身に起きたことを、そのキャラクターの反応へ配る。
    ///
    /// 被弾と撃破はここが自分で流すので、外から呼ぶのは「見つけた」「攻撃を出した」など
    /// ダメージを伴わない出来事だけでよい。
    ///
    /// @note 反応先は呼び出し元に選ばせない。同じ «倒れた» に表情・アニメーション・
    ///       HUD・ボイスがそれぞれ反応するため、呼び出し元が名指しすると反応を 1 つ
    ///       足すたびに敵 AI とプレイヤーの両方を触ることになる。
    void Notify(GameObject* character, CharacterEvent event) const;

    void OnStart() override;
    void OnUpdate() override;
    void OnDestroy() override;
    /// @}

private:
    static inline CombatManagerComponent* s_instance = nullptr;

    /// 連鎖を進める。深さは 1 フレームに 1 つ、当たった数は hits ぶん。猶予は張り直す。
    void AdvanceChain(int hits);

    bool m_warnedNoPlayerTarget = false;
    /// 受け手の検算を始めるまでの猶予 (秒)。スクリプト間のライフサイクルの
    /// 前後関係を吸収する。
    static constexpr float kTargetGraceSeconds = 0.5f;
    float m_targetGrace = 0.0f;

    int m_kills = 0;
    int m_pushKills = 0;
    int m_damageToEnemies = 0;
    int m_damageToPlayer = 0;

    int   m_chain = 0;
    int   m_bestChain = 0;
    float m_chainRemaining = 0.0f;
    /// 最後に連鎖を 1 つ進めたフレーム。同じフレームの 2 発目以降は数えない。
    std::uint64_t m_chainFrame = 0;
    /// この連鎖で当たった回数。同じフレームでも 1 発ごとに進む (倍率の元)。
    int m_chainHits = 0;
    int m_bladeHitSerial = 0;
    int m_rushHitSerial = 0;
    int m_perfectDodges = 0;
    int m_parries = 0;
    int m_cadence = 0;
    int m_beatSerial = 0;
    int m_perfectSerial = 0;
    int m_perfectCadences = 0;
};

FBZZ_REFLECT(CombatManagerComponent)

inline void CombatManagerComponent::OnStart()
{
    if (s_instance && s_instance != this) {
        debug.LogWarning("CombatManagerComponent: another instance is already active. "
                         "The most recently started one takes over.");
    }
    s_instance = this;

    m_kills = 0;
    m_pushKills = 0;
    debugPushKills = 0;
    m_damageToEnemies = 0;
    m_damageToPlayer = 0;
    m_chain = 0;
    m_bestChain = 0;
    m_chainRemaining = 0.0f;
    m_chainFrame = 0;
    m_chainHits = 0;
    m_bladeHitSerial = 0;
    m_rushHitSerial = 0;
    m_perfectDodges = 0;
    debugPerfectDodges = 0;
    m_parries = 0;
    m_cadence = 0;
    m_beatSerial = 0;
    m_perfectSerial = 0;
    m_perfectCadences = 0;
    debugCadence = 0;

    /// @note 連鎖の読み上げは画面の出来事であって空間の出来事ではないので UI バスの 2D。
    se::EnsureSource(scene, "UI");
}

inline void CombatManagerComponent::OnDestroy()
{
    if (s_instance == this) s_instance = nullptr;
}

inline void CombatManagerComponent::OnUpdate()
{
    /// @name 受け手の検算
    /// @note 1 フレーム目では判定しない。このエンジンは «全員の OnStart → 全員の
    ///       OnUpdate» の順では回らず、CombatManager の最初の OnUpdate が
    ///       PlayerComponent の OnStart より先に走ることがあるため。
    /// @note 攻撃が当たるまでは待たない。DamagePlayer 側の報告は «敵が初めて殴った瞬間»
    ///       まで出ないため、猶予を置いて 1 度だけ能動的に確かめる。
    if (!m_warnedNoPlayerTarget && m_targetGrace < kTargetGraceSeconds) {
        m_targetGrace += std::max(time.UnscaledDeltaTime(), 0.0f);
        if (m_targetGrace >= kTargetGraceSeconds) {
            GameObject* player = scene.FindWithTag("Player", true);
            if (!player) {
                m_warnedNoPlayerTarget = true;
                debug.LogError("CombatManagerComponent: tag=Player の GameObject が無い。"
                               "敵の攻撃は誰にも当たらない。");
            } else if (!IDamageable::Of(player)) {
                m_warnedNoPlayerTarget = true;
                debug.LogError("CombatManagerComponent: '" + player->name
                               + "' が IDamageable として名乗っていない。"
                                 "PlayerComponent::OnStart の IDamageable::Bind() を"
                                 "確認すること。このままでは敵の攻撃が一切通らない。");
            }
        }
    }

    debugFaces = EyeSpriteComponent::RegisteredCount();

    /// @note 実時間で数える。連鎖の途中は必ずヒットストップが掛かり、縮んだ時間で
    ///       数えると派手に決まった連鎖ほど猶予が伸びて別物の判定になる。
    if (m_chainRemaining > 0.0f) {
        m_chainRemaining -= std::max(time.UnscaledDeltaTime(), 0.0f);
        if (m_chainRemaining <= 0.0f) {
            m_chainRemaining = 0.0f;
            m_chain = 0;
            m_chainHits = 0;
            debugChainHits = 0;
            debugChain = 0;
        }
    }
}

inline void CombatManagerComponent::Notify(GameObject* character, CharacterEvent event) const
{
    if (!character) return;

    /// @note 反応を持たないキャラクターは黙って素通りさせる。目を付けていないだけで
    ///       敵が 1 種類まるごと «壊れている» ように見えてはいけない。
    if (auto* eyes = EyeSpriteComponent::For(character)) eyes->React(event);
}

inline bool CombatManagerComponent::DamagePlayer(GameObject* player, int amount,
                                                 const Vector3* source)
{
    return HitPlayer(player, amount, source, PlayerHitKind::Unblockable)
        == PlayerHitResult::Damaged;
}

inline PlayerHitResult CombatManagerComponent::HitPlayer(GameObject* player, int amount,
                                                         const Vector3* source,
                                                         PlayerHitKind kind)
{
    /// @note `PlayerComponent` ではなく `IDamageable` で引く。「殴られる側」であることだけが
    ///       関心で、プレイヤーかどうかは知らなくてよい。
    /// @note `scene.GetScript<IDamageable>()` は使わない。この環境では横断インターフェースで
    ///       引くと基底たどりが効かず必ず nullptr が返るため、確実に引ける
    ///       `IDamageable::Of` の名簿を使う。
    auto* target = IDamageable::Of(player);
    if (!target) {
        /// @note ここだけ名指しで報告する。敵の攻撃が «当たっているのに減らない» とき
        ///       画面には «避けられている» としか出ず、受け手不在か無敵かはこの行が
        ///       無いと見えない。
        if (!m_warnedNoPlayerTarget) {
            m_warnedNoPlayerTarget = true;
            debug.LogError("CombatManagerComponent: the player object has no IDamageable "
                           "(PlayerComponent). Enemy attacks can never deal damage. "
                           "IDamageable::Bind() を OnStart で呼んでいるか確認すること。");
        }
        return PlayerHitResult::Ignored;
    }

    const int before = target->CurrentHealth();
    const PlayerHitResult result = target->ReceiveHit(std::max(amount, 1), source, kind);

    if (result == PlayerHitResult::Parried) {
        ++m_parries;
        /// @note 弾きは «手を止めていない» ので連鎖を切らない。CHAIN の表示がここでも伸びる。
        RegisterBladeChain(1);
        return result;
    }
    if (result != PlayerHitResult::Damaged) return result;

    /// @note 無敵時間で弾かれた分を数えないよう、実際に減った量だけを積む。
    m_damageToPlayer += std::max(before - target->CurrentHealth(), 0);
    debugDamageToPlayer = m_damageToPlayer;

    /// @note 押しは «通った» ときだけ。無敵で弾いた一撃でも押すと、避けているのに
    ///       位置だけ持っていかれる。押し方を知っているのは殴られた側 (IDamageable)。
    if (source && playerKnockSpeed > 0.0f)
        target->ApplyKnockback(*source, playerKnockSpeed, playerKnockSeconds);

    Notify(player, target->IsAlive() ? CharacterEvent::Hurt : CharacterEvent::Defeated);
    return result;
}

inline bool CombatManagerComponent::DamageEnemyDirect(GameObject* target, int amount)
{
    auto* health = scene.GetScript<EnemyHealthComponent>(target);
    if (!health || !health->IsAlive()) return false;

    const int before = health->Current();
    const bool defeated = health->ApplyDamage(std::max(amount, 1));
    m_damageToEnemies += std::max(before - health->Current(), 0);
    debugDamageToEnemies = m_damageToEnemies;

    Notify(target, health->IsAlive() ? CharacterEvent::Hurt : CharacterEvent::Defeated);
    if (!defeated) return false;

    ++m_kills;
    debugKills = m_kills;
    if (onEnemyDefeated) onEnemyDefeated();
    return true;
}

/// @note 1 フレームに何度当たっても深さは 1 つしか進めない。溜め斬りは 4 部位へ同時に
///       入るため 1 当たり 1 連鎖で数えると «順番に繋いだ» と «一度に潰した» が
///       同じ数字になる。当たった数 (倍率の元) は別に全部積む。猶予は毎回張り直す。
inline void CombatManagerComponent::AdvanceChain(int hits)
{
    const std::uint64_t frame = time.FrameCount();
    if (frame != m_chainFrame) {
        m_chainFrame = frame;
        ++m_chain;
        m_bestChain    = std::max(m_bestChain, m_chain);
        debugChain     = m_chain;
        debugBestChain = m_bestChain;
    }
    m_chainHits += std::max(hits, 1);
    debugChainHits = m_chainHits;
    m_chainRemaining = std::max(chainSeconds, 0.0f);
}

inline void CombatManagerComponent::RegisterBladeChain(int hits)
{
    const int before = m_chain;
    AdvanceChain(hits);

    /// @note 節目だけ鳴らす。斬撃は毎秒のように当たるため 2 連目から毎回鳴らすと、
    ///       UI の読み上げが斬撃音と同じ頻度で重なって «区切り» でなくなる。
    const int step = std::max(highChainCount, 2);
    if (m_chain != before && m_chain >= step && (m_chain % step) == 0)
        se::Play(audio, se::kUiComboHigh);
}

} // namespace sandbox
