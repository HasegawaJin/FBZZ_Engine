/// @file    HitstopManagerComponent.hpp
/// @brief   ヒットストップの要求を受け付け、長さと強さを 1 箇所で決める
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY 呼び出し側から切り離すか:
///   ヒットストップは「当たった」と感じさせる主要な手段で (17 章)、当たる場所が増えるほど
///   呼び出し元も増える。長さと強さを呼び出し元それぞれが持つと、全体の重さを
///   調整したいときに全部を回ることになり、しかも各所の値が少しずつずれていく。
///   呼び出し元が渡すのは「どれくらい強い当たりか (0..1)」だけにして、
///   それを何秒どの深さで止めるかはここが決める。
///
/// WHY 重ねずに 1 本へ畳むか:
///   同じフレームに複数の衝突が起きるのは普通で、素直に足すと弱い衝突が重なっただけで
///   画面が長時間止まる。強い方の深さと長い方の残り時間を採り、常に 1 本として扱う。
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Game/GameSettingsComponent.hpp>
#include <Scripts/Game/TimeManagerComponent.hpp>
#include <algorithm>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class HitstopManagerComponent : public Script {
    FBZZ_SCRIPT(HitstopManagerComponent)

public:
    FBZZ_GROUP("強度")
    // WHY 0.09 へ戻したか (2026-09-05): 一度 0.14 まで伸ばしたが、これは «世界» の止め。
    //     止まっている間は移動入力も縮んだ時間で進むので、伸ばすほど操作が重くなる。
    //     手応えを担当するのは下の Animation (当事者だけを固める) の方で、
    //     そちらは 0.18 まで伸ばしてある。
    FBZZ_FIELD_RANGE(float, maxSeconds, 0.09f, "最大秒数", 0.0f, 0.5f)
    FBZZ_TOOLTIP("最も強い当たりで止まる長さ。実際の長さは強さ (0..1) に比例して縮む")
    FBZZ_FIELD_RANGE(float, minSeconds, 0.02f, "Min Seconds", 0.0f, 0.5f)
    FBZZ_TOOLTIP("弱い当たりでも最低これだけは止める。0 にすると軽い接触が無反応になる")
    FBZZ_FIELD_RANGE(float, timeScale, 0.05f, "Time Scale", 0.0f, 1.0f)
    FBZZ_TOOLTIP("停止中のタイムスケール。0 で完全停止。少し流した方が固まって見えにくい")

    // アニメーションだけを止める口。画面全体は動いたまま、当たった当人の芝居だけが
    // 数フレーム固まる。
    //
    // WHY 全体を止めるのと別に要るか:
    //   Request() の止めは «世界が止まる» ので、強くすると画面全体がぎこちなくなる。
    //   一方 «斬った手応え» は «斬った腕と斬られた体が食い込んで止まる» ことで出る。
    //   全体を止めてそれを作ろうとすると、当たりを重くするたびにカメラも粒子も
    //   一緒に止まり、テンポの方が先に壊れる。止める対象を «当事者» に絞れば、
    //   全体の止めは «衝撃» の担当のまま短く保てて、手応えだけを深くできる。
    //
    // WHY 全体の止めより長くしてよいか:
    //   止まっているのが 1 体だけなら «画面が固まった» とは読まれない。格闘ゲームの
    //   ヒットストップも 5〜8 フレーム (0.08〜0.13 秒) の幅にある。
    FBZZ_GROUP("アニメーション")
    FBZZ_FIELD_RANGE(float, animMaxSeconds, 0.18f, "Anim Max", 0.0f, 0.6f)
    FBZZ_TOOLTIP("最も強い当たりでアニメーションが固まる長さ。強さ (0..1) に比例して縮む")
    FBZZ_FIELD_RANGE(float, animMinSeconds, 0.05f, "Anim Min", 0.0f, 0.6f)
    FBZZ_TOOLTIP("弱い当たりでも最低これだけは固める")
    FBZZ_FIELD_RANGE(float, animSpeed, 0.0f, "Anim Speed", 0.0f, 1.0f)
    FBZZ_TOOLTIP("固めている間の再生速度。0 で完全停止。0.1 前後にすると «めり込みながら"
                 "押し切る» 感じになる")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(float, debugRemaining, 0.0f, "Remaining")
    FBZZ_FIELD_READ_ONLY(int, debugFrozen, 0, "Frozen Actors")

    [[nodiscard]] static HitstopManagerComponent* Instance() { return s_instance; }

    // 当たりの強さ (0..1) から長さを決めて止める。呼び出し側の標準手段。
    void Hit(float strength01);
    // 長さと深さを直接指定する。演出上どうしても個別に決めたい場所だけで使う。
    void Request(float seconds, float scale);
    void Cancel();

    /// target のアニメーションだけを強さ (0..1) ぶん固める。全体の時間は動いたまま。
    ///
    /// 渡すのは «芝居を持っている本体»。当たった部位 (ボスの脚など) ではなく、
    /// Animator が乗っているオブジェクトを渡すこと ─ 部位に渡しても何も起きない。
    ///
    /// 同じ相手を重ねて呼んでも二重には掛からない。長い方の残りを採り、
    /// 元の速度は «最初に固めたときの値» を覚え続ける。
    void FreezeAnimation(GameObject* target, float strength01);
    /// 固めている相手を全部その場で戻す。
    void ThawAnimations();

    [[nodiscard]] bool IsActive() const { return m_remaining > 0.0f; }

    // 今かかっている止めの重さ 0..1。止まっていなければ 0。
    //
    // WHY 深さではなく長さで測るか: Hit() が強さから動かすのは長さだけで、深さ
    //     (timeScale) は 1 つの値を全員で共有している。深さを返すと、軽い接触も
    //     全力の激突も同じ数字になる。
    //
    // WHY 公開するか: 止めに重ねる画面効果 (ScreenEffectManagerComponent の Freeze)
    //     は、止めと «同じ重さ» でなければならない。最低の 0.02 秒 = 60fps で
    //     1 フレームの止めに全力の絵を出すと、当たりではなく描画のちらつきに見える。
    //     受け取る側が長さから逆算すると、Hit() の対応表をもう 1 つ持つことになる。
    [[nodiscard]] float Weight01() const;

    void OnStart() override;
    void OnUpdate() override;
    void OnDestroy() override
    {
        Cancel();
        ThawAnimations();
        if (s_instance == this) s_instance = nullptr;
    }

private:
    static inline HitstopManagerComponent* s_instance = nullptr;

    /// 固めている 1 体ぶん。
    ///
    /// WHY 元の速度を覚えるか: 解除で 1.0 へ戻すと、もともと遅回し・逆再生に
    ///     していた相手の設定を止めが踏み潰す。止めは «一時的に上書きする» もので、
    ///     何が正しい速度かを決めるのはあくまで相手側。
    struct FrozenActor {
        EntityRef target;
        float     remaining = 0.0f;
        float     restore   = 1.0f;
    };
    /// 同時に固められる数。1 回の当たりで動くのは «斬った側と斬られた側» なので
    /// 2 で足りるが、溜め斬りが複数の相手へ同時に入る余地を見て少し多めに取る。
    static constexpr int kMaxFrozen = 8;
    FrozenActor m_frozen[kMaxFrozen];

    void TickAnimations(float dt);

    float m_remaining    = 0.0f;
    float m_scale        = 1.0f;
    // 今の止めが «始まったときの» 長さ。残りだけでは重さが測れない
    // (解除の直前はどんな止めでも残り 0 になる)。
    float m_seconds      = 0.0f;
    bool  m_warnedNoTime = false;
};

FBZZ_REFLECT(HitstopManagerComponent)

inline void HitstopManagerComponent::OnStart()
{
    if (s_instance && s_instance != this) {
        debug.LogWarning("HitstopManagerComponent: another instance is already active. "
                         "The most recently started one takes over.");
    }
    s_instance     = this;
    m_remaining    = 0.0f;
    m_scale        = 1.0f;
    m_seconds      = 0.0f;
    m_warnedNoTime = false;
    // 前のプレイで固めたままの相手は居ない (Script は作り直される) が、
    // 残りだけは 0 から始める。
    for (FrozenActor& actor : m_frozen) { actor = {}; }
    debugFrozen = 0;
}

inline float HitstopManagerComponent::Weight01() const
{
    if (m_remaining <= 0.0f) return 0.0f;
    // Hit() が minSeconds〜maxSeconds へ写した長さを、そのまま逆に読む。
    // Request() を直接叩いた場合はこの範囲の外へ出るので、両端で止める。
    //
    // WHY Option の倍率を掛け戻すか: m_seconds は倍率を掛けた «後» の長さ。素の
    //     範囲と比べると、止めを弱める設定にしただけで全部の当たりが軽い判定になり、
    //     «どれくらい強い当たりだったか» が設定で変わってしまう。
    const float optionScale = std::max(GameSettingsComponent::HitstopScale(), EPSILON);
    const float low  = std::max(minSeconds, 0.0f) * optionScale;
    const float high = std::max(maxSeconds, 0.0f) * optionScale;
    if (high - low <= EPSILON) return 1.0f;
    return Clamp01((m_seconds - low) / (high - low));
}

inline void HitstopManagerComponent::Hit(float strength01)
{
    const float strength = Clamp01(strength01);
    const float seconds  = Lerp(std::max(minSeconds, 0.0f), std::max(maxSeconds, 0.0f), strength);
    Request(seconds, timeScale);
}

inline void HitstopManagerComponent::Request(float seconds, float scale)
{
    // Option の「ヒットストップ」。長さを縮める形で効かせる。
    // WHY 深さ (scale) ではなく長さか: 深さを浅くすると「止まったのにすぐ動く」
    //     半端な引っ掛かりになる。短くすれば 0 で完全に無くなり、途中の値も
    //     「軽く止まる」として素直に読める。
    seconds *= GameSettingsComponent::HitstopScale();
    if (seconds <= 0.0f) return;

    // 強い方の深さと長い方の残りを採る。足し合わせると弱い衝突の重なりで長時間止まる。
    m_remaining = std::max(m_remaining, seconds);
    m_scale     = std::min(m_scale, Clamp01(scale));
    // 重さも «長い方» に揃える。残りと別々に選ぶと、重ねた瞬間に長さと重さが
    // 食い違い、Weight01() が 1 本の止めを表さなくなる。
    m_seconds   = std::max(m_seconds, seconds);

    // WHY OnStart で有無を確かめないか: スクリプトの並び順によっては TimeManager の
    //     OnStart が後になる。並び順に依存した警告は、順番を入れ替えただけで嘘になる。
    if (auto* timeManager = TimeManagerComponent::Instance()) {
        timeManager->SetOverride(m_scale);
    } else if (!m_warnedNoTime) {
        m_warnedNoTime = true;
        debug.LogError("HitstopManagerComponent requires a TimeManagerComponent in the scene "
                       "(the time scale is only written there).");
    }
}

inline void HitstopManagerComponent::Cancel()
{
    m_remaining    = 0.0f;
    m_scale        = 1.0f;
    m_seconds      = 0.0f;
    debugRemaining = 0.0f;
    if (auto* timeManager = TimeManagerComponent::Instance())
        timeManager->ClearOverride();
}

inline void HitstopManagerComponent::FreezeAnimation(GameObject* target, float strength01)
{
    if (!target) return;

    // 全体の止めと同じ Option を掛ける。片方だけ効かない設定があると、
    // «ヒットストップを切ったのに手応えが残る» という半端な状態になる。
    const float seconds = Lerp(std::max(animMinSeconds, 0.0f),
                               std::max(animMaxSeconds, 0.0f), Clamp01(strength01))
                        * GameSettingsComponent::HitstopScale();
    if (seconds <= 0.0f) return;

    const EntityID id = target->GetID();
    FrozenActor*   slot = nullptr;

    for (FrozenActor& actor : m_frozen) {
        if (actor.remaining > 0.0f && actor.target.Resolve(scene) == target) {
            // 既に固めている相手。長い方を採るだけで、元の速度は上書きしない
            // (今の速度は自分が書いた 0 なので、控え直すと二度と戻らなくなる)。
            actor.remaining = std::max(actor.remaining, seconds);
            return;
        }
        if (!slot && actor.remaining <= 0.0f) slot = &actor;
    }
    // 空きが無いのは «同時に 8 体へ当てた» ときだけ。固め損ねても手応えが 1 回
    // 薄くなるだけなので、古いものを蹴り出してまで入れる価値はない。
    if (!slot) return;

    slot->target    = EntityRef{ id };
    slot->restore   = animator.GetSpeed(target);
    slot->remaining = seconds;
    animator.SetSpeed(target, Clamp01(animSpeed));
}

inline void HitstopManagerComponent::ThawAnimations()
{
    for (FrozenActor& actor : m_frozen) {
        if (actor.remaining <= 0.0f) continue;
        if (GameObject* target = actor.target.Resolve(scene))
            animator.SetSpeed(target, actor.restore);
        actor.remaining = 0.0f;
        actor.target    = {};
    }
    debugFrozen = 0;
}

inline void HitstopManagerComponent::TickAnimations(float dt)
{
    int alive = 0;
    for (FrozenActor& actor : m_frozen) {
        if (actor.remaining <= 0.0f) continue;

        actor.remaining -= dt;
        if (actor.remaining > 0.0f) {
            ++alive;
            continue;
        }

        // 相手が消えていても «戻し忘れ» にはならない (Resolve が空を返すだけ)。
        if (GameObject* target = actor.target.Resolve(scene))
            animator.SetSpeed(target, actor.restore);
        actor.target = {};
    }
    debugFrozen = alive;
}

inline void HitstopManagerComponent::OnUpdate()
{
    // WHY 実時間で数えるか: 止めている当人が縮んだ時間で残りを数えると、
    //     深く止めるほど解除が遅れる。timeScale 0 では永久に戻らない。
    //     アニメーションの止めも同じ ─ こちらは全体の止めと重なることがあり、
    //     縮んだ時間で数えると «全体が止まっている間だけ固まり続ける» ことになる。
    const float dt = std::max(time.UnscaledDeltaTime(), 0.0f);

    TickAnimations(dt);

    if (m_remaining <= 0.0f) return;

    m_remaining -= dt;
    debugRemaining = std::max(m_remaining, 0.0f);
    if (m_remaining > 0.0f) return;

    Cancel();
}

} // namespace sandbox
