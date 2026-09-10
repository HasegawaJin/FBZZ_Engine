/// @file    BossPartComponent.hpp
/// @brief   ボスの部位 1 つぶんの耐久。斬られた部位が削られ、削り切られると落ちる
/// @author  Hasegawa Jin
/// @date    2026-08-29
///
/// WHY 本体の HP と別に持つか:
///   部位を落とすのは HP を削るのとは別の勝ち筋で、目盛りが 1 桁違う
///   (本体 800 に対して部位は数発ぶん)。同じ数字を分け合うと、«脚を落とす» と
///   «倒す» のどちらを進めているのかが画面から読めなくなる。
///
/// WHY 部位が «斬られた合図» まで持つか:
///   光るのは «斬られた部位» であって脚 4 本ではない。リグ側に秒数を置くと、
///   どの部位が今光っているかをリグが別に覚えることになる。
#pragma once

#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <string>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class BossPartComponent : public Script {
    FBZZ_SCRIPT(BossPartComponent)

public:
    FBZZ_GROUP("Hit")
    // WHY 判定用の半径を自前で持つか: 斬撃の扇は bodybounds (描画バウンズ) で
    //     相手の太さを足しているが、部位はレンダラーを持たない当たり判定なので
    //     そこから寸法が出ない。届く太さをここで宣言する。
    FBZZ_FIELD_RANGE(float, hitRadius, 1.20f, "当たり半径", 0.1f, 5.0f)
    FBZZ_TOOLTIP("斬撃の扇へ足す太さ。脚は細いので、素の中心距離だけでは «見えているのに当たらない» になる")

    // 斬られた直後に輪郭が «叩かれた白» へ寄っている長さ。読む側は BossRigComponent /
    // SerpentRigComponent の DriveOutline で、寄せ具合は DamageFlash() の残り比。
    FBZZ_FIELD_RANGE(float, damageFlashSeconds, 0.18f, "閃光", 0.0f, 1.0f)
    FBZZ_TOOLTIP("斬られた部位の輪郭が白へ寄っている秒数。0 で光らない")

    // 吸い付きの当て先に選ばれている «あいだ» の保持時間。斬撃側が毎フレーム置き直す。
    //
    // WHY 真偽値を毎フレーム消さずに «保持» にするか: 置く側 (BladeComponent) と
    //     読む側 (BossRigComponent の DriveOutline) の実行順は保証されていない。
    //     フレーム頭で false に戻す形にすると、読む順によって 1 フレームおきに
    //     消えて輪郭がちらつく。少しだけ持たせれば順番に依らない。
    FBZZ_FIELD_RANGE(float, aimHoldSeconds, 0.10f, "狙い保持", 0.0f, 1.0f)
    FBZZ_TOOLTIP("吸い付きの当て先として光っている合図の保持秒数。"
                 "置く側と読む側の実行順に依らせないための猶予なので、短くて足りる")

    FBZZ_GROUP("HP")
    FBZZ_FIELD_RANGE_INT(int, maxHealth, 150, "最大 HP", 1, 1000)
    FBZZ_TOOLTIP("この部位を削り切るのに要る量。斬撃 1 発は BladeTuning の Damage (既定 25)")

    FBZZ_GROUP("デバッグ")
    // どの脚に属する部位か ("_FR" など)。BossHitboxRigComponent が生成時に入れる。
    //
    // WHY GameObject 名から切り出さないか: 名前は «当たり判定の名前» で、部位の
    //     識別子ではない。命名を変えた瞬間に IK のボーン名解決が黙って外れる。
    FBZZ_FIELD_READ_ONLY(std::string, legSuffix, "", "脚")
    FBZZ_FIELD_READ_ONLY(int, debugHealth, 0, "HP")
    FBZZ_FIELD_READ_ONLY(bool, debugBroken, false, "Broken")
    // 輪郭が出ないときの切り分け用。ここが false なら斬撃側が当て先に選んでいない、
    // true なのに画面に何も無いなら BossRigComponent 以降 (脚の対応 / ポストプロセス)。
    FBZZ_FIELD_READ_ONLY(bool, debugAimed, false, "Aimed")

    /// もぎ取られた。以後この部位は削れない。
    void Break();

    /// 斬られて削る。**削り切った呼び出しだけ** true を返す。
    ///
    /// WHY 残量ではなく «削り切った瞬間» を返すか: 呼び手 (斬撃) は毎フレーム来る。
    ///     0 かどうかを外から見て壊す形にすると、次の一振りでもう一度壊すことになる。
    bool Damage(int amount);

    /// 斬られた合図を出す。削れたかどうかとは独立に光る ─ 削り切った部位を
    /// もう一度斬っても «当たった» が返らないと、当たり判定が死んで見える。
    void Flash() { m_flash = std::max(damageFlashSeconds, 0.0f); }
    /// 1 = 斬られた瞬間 / 0 = 光っていない。
    [[nodiscard]] float DamageFlash() const;

    /// 吸い付きの当て先に選ばれている合図を置く。斬撃側が毎フレーム置き直す。
    void MarkAimed() { m_aimed = std::max(aimHoldSeconds, 0.0f); }
    /// 1 = 今狙われている / 0 = 狙われていない。
    [[nodiscard]] float AimHighlight() const;

    [[nodiscard]] bool  IsBroken()    const { return m_broken; }
    [[nodiscard]] int   Health()      const { return std::max(m_health, 0); }
    [[nodiscard]] bool  IsDepleted()  const { return m_health == 0; }
    /// 1 = 無傷 / 0 = 削り切った。
    [[nodiscard]] float HealthNormalized() const;

    void OnStart()  override;
    void OnUpdate() override;

private:
    bool  m_broken = false;
    /// 斬られた合図の残り [秒]。
    float m_flash  = 0.0f;
    /// 狙われている合図の残り [秒]。
    float m_aimed  = 0.0f;
    // -1 = 未初期化。BossHitboxRigComponent が実行時に組むので、OnStart より先に
    // 斬られうる (同じフレームに扇が通る)。
    int   m_health = -1;
};

FBZZ_REFLECT(BossPartComponent)

inline void BossPartComponent::Break()
{
    m_broken    = true;
    debugBroken = true;
    m_health    = 0;
}

inline bool BossPartComponent::Damage(int amount)
{
    if (m_broken || amount <= 0) return false;
    if (m_health < 0) m_health = std::max(maxHealth, 1);
    if (m_health == 0) return false;

    m_health = std::max(m_health - amount, 0);
    return m_health == 0;
}

inline float BossPartComponent::DamageFlash() const
{
    if (m_flash <= 0.0f || damageFlashSeconds <= 0.0f) return 0.0f;
    return Clamp01(m_flash / damageFlashSeconds);
}

inline float BossPartComponent::HealthNormalized() const
{
    if (m_health < 0) return 1.0f;
    return Clamp01(static_cast<float>(m_health) / static_cast<float>(std::max(maxHealth, 1)));
}

inline void BossPartComponent::OnStart()
{
    if (m_health < 0) m_health = std::max(maxHealth, 1);
}

inline float BossPartComponent::AimHighlight() const
{
    if (m_aimed <= 0.0f || aimHoldSeconds <= 0.0f) return 0.0f;
    // 残り比を返さず «出ているか» だけを返す。保持は実行順を吸収するための猶予で、
    // 減っていく様子を絵に出すと «狙いが外れかけている» ように見える。
    return 1.0f;
}

inline void BossPartComponent::OnUpdate()
{
    if (m_flash > 0.0f) m_flash = std::max(m_flash - Time::deltaTime, 0.0f);
    if (m_aimed > 0.0f) m_aimed = std::max(m_aimed - Time::deltaTime, 0.0f);
    debugHealth = Health();
    debugAimed  = m_aimed > 0.0f;
}

} // namespace sandbox
