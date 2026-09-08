/// @file    EnemyHealthComponent.hpp
/// @brief   ダメージで減る HP と撃破処理。今の盤面ではボスだけが持つ
/// @author  Hasegawa Jin
/// @date    2026-08-19
#pragma once

#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Script.hpp>
#include <Scripts/Combat/EnemyDeathVfxComponent.hpp>
#include <Scripts/Combat/IDamageable.hpp>
#include <Scripts/Utils/SeLibrary.hpp>
#include <algorithm>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class EnemyHealthComponent : public Script, public IDamageable {
    FBZZ_SCRIPT_DERIVED(EnemyHealthComponent, Script, IDamageable)
    FBZZ_OPTIONAL_COMPONENT(AudioSourceComponent)

public:
    FBZZ_GROUP("HP")
    FBZZ_FIELD_RANGE_INT(int, maxHealth, 100, "最大 HP", 1, 10000)

    FBZZ_GROUP("撃破")
    // 上限が 5 秒だとボスに足りない。ボスは撃破からリザルトへ移るまで数秒あり
    // (GameFlowComponent の Boss End Delay)、その間より先に消えると «倒した相手が
    // 居ないまま結果を待つ» 画になる。待ちを伸ばすときは必ずこちらも一緒に伸ばす。
    FBZZ_FIELD_RANGE(float, destroyDelay, 0.05f, "Destroy Delay", 0.0f, 12.0f)
    FBZZ_TOOLTIP("倒れてから GameObject を畳むまでの秒数。撃破演出が付いていれば"
                 "その長さの方が優先される («最低でもこれだけは残す» の意味)")

    FBZZ_GROUP("手応え")
    FBZZ_FIELD_AUDIO(sfxHit, "", "SFX Hit")
    FBZZ_FIELD_AUDIO(sfxDeath, "", "撃破の効果音")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugHealth, 0, "HP")

    // ── IDamageable ─────────────────────────────────────────────────────────
    bool ApplyDamage(int amount) override;
    [[nodiscard]] int  CurrentHealth() const override { return m_health; }
    [[nodiscard]] int  MaxHealth()     const override { return std::max(maxHealth, 1); }
    [[nodiscard]] bool IsAlive()       const override { return m_health > 0; }

    /// CurrentHealth() の別名。既存の呼び出し側がこちらを使っている。
    [[nodiscard]] int Current() const { return m_health; }
    [[nodiscard]] float Normalized() const
    {
        return maxHealth > 0
            ? Clamp01(static_cast<float>(m_health) / static_cast<float>(maxHealth))
            : 0.0f;
    }

    /// 撃破音を相手ごとの束へ差し替える。相手側が起動時に自分の束を預ける。
    ///
    /// WHY Inspector の欄で足りないか: sfxDeath は 1 本しか持てないので、変奏を
    ///     持たせられない。かといってここが相手のスクリプトを知ると、相手が
    ///     EnemyHealth を見ている今の向きと合わせて include が輪になる。
    ///     «束を預ける» 向きだけにすれば、知る側は 1 方向で済む。
    void SetDestroyVoice(const se::Bank* bank) { m_destroyVoice = bank; }

    /// 被弾音を差し替える。撃破音と同じ «預ける» 向き。
    ///
    /// WHY 必要か: 共通の束 (kEnemyFlinch) は軽い装甲が鳴る音で、ボスに当てると
    ///     «同じくらいのものに当たった» と読める。ボスへ通る一撃はとどめだけなので、
    ///     その 1 撃の重さが伝わらないと «今の攻め方で合っているのか» が
    ///     耳から判断できなくなる。
    void SetFlinchVoice(const se::Bank* bank) { m_flinchVoice = bank; }
    void ResetHealth();
    void OnDestroy() override { IDamageable::Unbind(scene.Self(), this); }
    void OnStart() override
    {
        ResetHealth();
        // «殴られる側» として名乗る (IDamageable::Of のコメント参照)。
        IDamageable::Bind(scene.Self(), this);
        // ボスは盤面を動き回る。どの方向で何が起きたかが分かる必要があるので 3D。
        se::EnsureSource(scene, "SE", 1.0f);
    }

private:
    /// 量が決まった後の適用。ひるみ / 撃破の反応もここで返す。
    /// @ret この呼び出しで死亡したら true。
    bool Deal(int damage);

    int m_health = 0;
    /// AI が預けた機種ごとの撃破音。預かる前 (と DLL リロード直後) は共通の束を使う。
    const se::Bank* m_destroyVoice = nullptr;
    const se::Bank* m_flinchVoice  = nullptr;
};

FBZZ_REFLECT(EnemyHealthComponent)

inline void EnemyHealthComponent::ResetHealth()
{
    m_health = std::max(maxHealth, 1);
    debugHealth = m_health;
}

inline bool EnemyHealthComponent::ApplyDamage(int amount)
{
    if (amount <= 0 || !IsAlive()) return false;
    Deal(amount);
    return true;
}

inline bool EnemyHealthComponent::Deal(int damage)
{
    m_health = std::max(0, m_health - damage);
    debugHealth = m_health;

    if (m_health > 0) {
        se::Play(audio, sfxHit, m_flinchVoice ? *m_flinchVoice : se::kEnemyFlinch);
        return false;
    }

    // WHY 自分ではなく位置で鳴らすか: destroyDelay の後にこの GameObject は消える。
    //     自分の AudioSource で鳴らすと、撃破音が鳴り終わる前に音源ごと消えて
    //     途中で切れる。撃破は「そこで起きたこと」なので、場所に残す。
    if (!sfxDeath.empty()) audio.PlayAtPoint(sfxDeath, transform.worldPosition);
    else                   se::PlayAt(audio, m_destroyVoice ? *m_destroyVoice : se::kEnemyDestroy,
                                      transform.worldPosition);

    // 撃破の «見え» はここでは作らない。演出が付いていればそれに任せる。
    // WHY 消えるまでの時間を演出に合わせるか: 粒はエミッターが持っているので、
    //     GameObject を先に畳むと、まだ空中に居る粒までその瞬間に消える。
    //     destroyDelay は «最低でもこれだけは残す» の意味になる。
    float delay = std::max(destroyDelay, 0.0f);
    if (auto* deathVfx = scene.GetScript<EnemyDeathVfxComponent>()) {
        deathVfx->Begin();
        delay = std::max(delay, deathVfx->TotalSeconds());
    }
    scene.DestroySelf(delay);
    return true;
}

} // namespace sandbox
