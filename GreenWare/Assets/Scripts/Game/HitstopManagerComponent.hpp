/// @file    HitstopManagerComponent.hpp
/// @brief   ヒットストップの要求を受け付け、長さと強さを 1 箇所で決める
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// @note 呼び出し元は「どれくらい強い当たりか (0..1)」だけを渡す。長さと深さを呼び出し元
///       ごとに持たせると全体の重さを調整するたびに全箇所を回ることになるため、
///       秒数と深さへの変換はここへ集約する。
/// @note 同じフレームに複数の衝突が起きても足し合わせない。強い方の深さと長い方の残り
///       時間を採り、常に 1 本の止めとして扱う (弱い衝突の重なりで長時間止まるのを防ぐ)。
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
    /// @note 世界全体の止め。伸ばすと移動入力も縮んだ時間で進み操作が重くなるため短く抑える。
    ///       手応えは下の Animation (当事者だけを固める。0.18 秒まで) の担当にする。
    FBZZ_FIELD_RANGE(float, maxSeconds, 0.09f, "最大秒数", 0.0f, 0.5f)
    FBZZ_TOOLTIP("最も強い当たりで止まる長さ。実際の長さは強さ (0..1) に比例して縮む")
    FBZZ_FIELD_RANGE(float, minSeconds, 0.02f, "Min Seconds", 0.0f, 0.5f)
    FBZZ_TOOLTIP("弱い当たりでも最低これだけは止める。0 にすると軽い接触が無反応になる")
    FBZZ_FIELD_RANGE(float, timeScale, 0.05f, "Time Scale", 0.0f, 1.0f)
    FBZZ_TOOLTIP("停止中のタイムスケール。0 で完全停止。少し流した方が固まって見えにくい")

    /// @note 当事者だけを止める口。全体の止め (Request) を強めるとカメラ・粒子ごと止まって
    ///       テンポが壊れるため、手応えは当人の芝居を数フレーム固める側に分離する。
    ///       1 体だけの静止は «画面が固まった» と読まれないため、全体の止めより長くしてよい
    ///       (格闘ゲームの目安は 5〜8 フレーム)。
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

    /// 当たりの強さ (0..1) から長さを決めて止める。呼び出し側の標準手段。
    void Hit(float strength01);
    /// 長さと深さを直接指定する。演出上どうしても個別に決めたい場所だけで使う。
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

    /// @brief この相手を今固めているか。
    /// @note 凍結は `animator.SetSpeed(go, 0)` で作るため、再生速度を毎フレーム書く側
    ///       (ボスの足の運びなど) は固めている間だけ書くのを止める必要がある。
    ///       秒数を呼び出し側で数え直すと Option の倍率が二重に掛かる。
    [[nodiscard]] bool IsAnimationFrozen(const GameObject* target) const;

    [[nodiscard]] bool IsActive() const { return m_remaining > 0.0f; }

    /// @brief 今かかっている止めの重さ 0..1。止まっていなければ 0。
    /// @note 深さ (timeScale) は全員で共有する 1 つの値なので、深さでは軽い接触も全力の
    ///       激突も同じ数字になる。長さから逆算するのはこちらの役目にし、画面効果
    ///       (Freeze) 側が Hit() の対応表を持たずに «同じ重さ» を再現できるようにする。
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

    /// @brief 固めている 1 体ぶん。
    /// @note 解除で 1.0 へ戻すと元の遅回し・逆再生の設定を踏み潰すため、元の速度を覚える。
    ///       止めは一時的な上書きで、正しい速度を決めるのはあくまで相手側。
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
    /// 今の止めが «始まったときの» 長さ。残りだけでは重さが測れない
    /// (解除の直前はどんな止めでも残り 0 になる)。
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
    /// @note 前のプレイで固めたままの相手は居ない (Script は作り直される) が、
    ///       残りだけは 0 から始める。
    for (FrozenActor& actor : m_frozen) { actor = {}; }
    debugFrozen = 0;
}

inline float HitstopManagerComponent::Weight01() const
{
    if (m_remaining <= 0.0f) return 0.0f;
    /// @note Hit() が minSeconds〜maxSeconds へ写した長さを、そのまま逆に読む。
    ///       Request() を直接叩いた場合はこの範囲の外へ出るので、両端で止める。
    ///       m_seconds は Option の倍率を掛けた後の長さなので、素の範囲と比べる前に
    ///       同じ倍率を掛け戻す (掛け戻さないと設定次第で強さの読みが変わる)。
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
    /// @note Option の「ヒットストップ」は長さを縮める形で効かせる。深さ (scale) を
    ///       浅くする形だと「止まったのにすぐ動く」半端な引っ掛かりになるが、
    ///       長さなら 0 で完全に無くなり途中の値も「軽く止まる」と素直に読める。
    seconds *= GameSettingsComponent::HitstopScale();
    if (seconds <= 0.0f) return;

    /// @note 強い方の深さと長い方の残りを採る。足し合わせると弱い衝突の重なりで長時間止まる。
    m_remaining = std::max(m_remaining, seconds);
    m_scale     = std::min(m_scale, Clamp01(scale));
    /// @note 重さも «長い方» に揃える。残りと別々に選ぶと、重ねた瞬間に長さと重さが
    ///       食い違い、Weight01() が 1 本の止めを表さなくなる。
    m_seconds   = std::max(m_seconds, seconds);

    /// @note OnStart では有無を確かめない。スクリプトの並び順によっては TimeManager の
    ///       OnStart が後になり、並び順依存の警告は順番を入れ替えただけで嘘になる。
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

    /// @note 全体の止めと同じ Option を掛ける。片方だけ効かない設定があると、
    ///       «ヒットストップを切ったのに手応えが残る» という半端な状態になる。
    const float seconds = Lerp(std::max(animMinSeconds, 0.0f),
                               std::max(animMaxSeconds, 0.0f), Clamp01(strength01))
                        * GameSettingsComponent::HitstopScale();
    if (seconds <= 0.0f) return;

    const EntityID id = target->GetID();
    FrozenActor*   slot = nullptr;

    for (FrozenActor& actor : m_frozen) {
        if (actor.remaining > 0.0f && actor.target.Resolve(scene) == target) {
            /// @note 既に固めている相手。長い方を採るだけで、元の速度は上書きしない
            ///       (今の速度は自分が書いた 0 なので、控え直すと二度と戻らなくなる)。
            actor.remaining = std::max(actor.remaining, seconds);
            return;
        }
        if (!slot && actor.remaining <= 0.0f) slot = &actor;
    }
    /// @note 空きが無いのは «同時に 8 体へ当てた» ときだけ。固め損ねても手応えが 1 回
    ///       薄くなるだけなので、古いものを蹴り出してまで入れる価値はない。
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

inline bool HitstopManagerComponent::IsAnimationFrozen(const GameObject* target) const
{
    if (!target) return false;
    for (const FrozenActor& actor : m_frozen)
        if (actor.remaining > 0.0f && actor.target.Resolve(scene) == target) return true;
    return false;
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

        /// @note 相手が消えていても «戻し忘れ» にはならない (Resolve が空を返すだけ)。
        if (GameObject* target = actor.target.Resolve(scene))
            animator.SetSpeed(target, actor.restore);
        actor.target = {};
    }
    debugFrozen = alive;
}

inline void HitstopManagerComponent::OnUpdate()
{
    /// @note 実時間で数える。縮んだ時間で残りを数えると深く止めるほど解除が遅れ、
    ///       timeScale 0 では永久に戻らない。アニメーションの止めも同じ理由で実時間。
    const float dt = std::max(time.UnscaledDeltaTime(), 0.0f);

    TickAnimations(dt);

    if (m_remaining <= 0.0f) return;

    m_remaining -= dt;
    debugRemaining = std::max(m_remaining, 0.0f);
    if (m_remaining > 0.0f) return;

    Cancel();
}

} // namespace sandbox
