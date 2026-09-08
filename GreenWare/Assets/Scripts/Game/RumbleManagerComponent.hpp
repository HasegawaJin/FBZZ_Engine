/// @file    RumbleManagerComponent.hpp
/// @brief   ゲームパッドの振動要求を合成し、パッドへ渡す値を 1 本にまとめる
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY 直接 SetVibration を呼ばせないか:
///   パッドの振動は「今この強さ」という絶対値を書き込む API で、後から呼んだ側が
///   前の値を丸ごと置き換える。弱い連打が強い一撃を消す、という
///   カメラ揺れとまったく同じ壊れ方をする。要求を溜めて毎フレーム合成し、
///   実際に書き込むのはここだけにする。
///
/// WHY 最大値で合成するか (加算ではなく):
///   モーターの出力は 0..1 に飽和する。足し合わせるとすぐ 1 に張り付き、
///   強弱の差が消えて「常に最大で震えている」状態になる。最も強い要求を採る方が、
///   一撃の重さが残る。
///
/// WHY 持続振動を別の器で持つか:
///   照射は押しているあいだ続く。長さの決まった要求を毎フレーム積むと、上限に達した
///   ところで «最も弱いものを捨てる» が働き、同じ 1 本の照射が自分自身を押し出し始める。
///   終わりの時刻が決まっていない振動は、寿命ではなく «今の強さ» で持つ。
///
/// WHY 距離による強弱をここが持たないか:
///   «どこで起きたか» はカメラ揺れも同じ数で減衰させたい値で、振動だけのものではない。
///   規約は Utils/ShockFalloff.hpp にあり、呼び出し元が 1 度だけ近さを出して、
///   揺れと振動の両方へ同じ数を掛ける。ここは «届いた強さ» を混ぜるだけに留める。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Game/GameSettingsComponent.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

/// 持続振動の口。同時に鳴りうるものだけを並べる。
///
/// WHY 呼び出し元ごとに口を分けるか: 左右の照射は同時に走る。1 つの口を共有すると、
///     後から書いた側が相手の強さを消し、両手で撃っているのに片手ぶんしか返らない。
enum class RumbleChannel : int {
    BeamPlus,    ///< ＋ (右) の照射
    BeamMinus,   ///< − (左) の照射
    BossBeam,    ///< ボスのコアビーム (点火から消灯まで 1 本)
    BladeCharge, ///< 溜め斬りの «こらえている» 震え (押している間 1 本)
    Count,
};

class RumbleManagerComponent : public Script {
    FBZZ_SCRIPT(RumbleManagerComponent)

public:
    FBZZ_GROUP("既定の形")
    FBZZ_FIELD_RANGE(float, defaultLow, 0.55f, "Low Motor", 0.0f, 1.0f)
    FBZZ_TOOLTIP("強さ 1.0 の要求で回す低周波モーター (重い揺さぶり)")
    FBZZ_FIELD_RANGE(float, defaultHigh, 0.30f, "High Motor", 0.0f, 1.0f)
    FBZZ_TOOLTIP("強さ 1.0 の要求で回す高周波モーター (鋭い刺激)")
    FBZZ_FIELD_RANGE(float, defaultDuration, 0.18f, "継続時間", 0.0f, 2.0f)

    FBZZ_GROUP("限界")
    FBZZ_FIELD(bool, enabledOnStart, true, "有効")
    FBZZ_TOOLTIP("切ると全ての要求を無視する。振動が苦手な人向けの一括スイッチ")
    FBZZ_FIELD_RANGE(float, masterScale, 1.0f, "Master Scale", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE_INT(int, maxRequests, 8, "要求の上限", 1, 64)

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(float, debugLow, 0.0f, "Low Output")
    FBZZ_FIELD_READ_ONLY(float, debugHigh, 0.0f, "High Output")

    [[nodiscard]] static RumbleManagerComponent* Instance() { return s_instance; }

    // 強さ (0..1) だけ渡す標準の呼び方。
    void Rumble(float strength01);
    // 両モーターと長さを直接指定する版。
    void Rumble(float low, float high, float duration);

    // 終わりの時刻が決まっていない振動。呼んだ強さがそのまま «今» として残り続けるので、
    // 止めるのは呼び出し元の責任 (照射をやめた / 銃を畳んだ / 自分が消えた)。
    void Sustain(RumbleChannel channel, float low, float high);
    void StopSustain(RumbleChannel channel);

    void StopAll();

    void SetEnabled(bool value) { m_enabled = value; if (!value) StopAll(); }
    [[nodiscard]] bool IsEnabled() const { return m_enabled; }

    void OnStart() override;
    void OnUpdate() override;
    void OnDestroy() override;

private:
    struct Request {
        float low       = 0.0f;
        float high      = 0.0f;
        float duration  = 0.0f;
        float remaining = 0.0f;
    };

    struct Level {
        float low  = 0.0f;
        float high = 0.0f;
    };

    static inline RumbleManagerComponent* s_instance = nullptr;

    std::vector<Request> m_requests;
    std::array<Level, static_cast<std::size_t>(RumbleChannel::Count)> m_sustain{};
    bool  m_enabled = true;
    bool  m_driving = false;
};

FBZZ_REFLECT(RumbleManagerComponent)

inline void RumbleManagerComponent::OnStart()
{
    if (s_instance && s_instance != this) {
        debug.LogWarning("RumbleManagerComponent: another instance is already active. "
                         "The most recently started one takes over.");
    }
    s_instance = this;
    m_requests.clear();
    m_sustain.fill({});
    m_enabled = enabledOnStart;
    m_driving = false;
}

inline void RumbleManagerComponent::OnDestroy()
{
    // 止め忘れるとシーンを抜けてもパッドが震え続ける。ゲームを閉じるまで止まらない。
    StopAll();
    if (s_instance == this) s_instance = nullptr;
}

inline void RumbleManagerComponent::Rumble(float strength01)
{
    const float strength = Clamp01(strength01);
    if (strength <= 0.0f) return;
    Rumble(defaultLow * strength, defaultHigh * strength, defaultDuration);
}

inline void RumbleManagerComponent::Rumble(float low, float high, float duration)
{
    if (!m_enabled || duration <= 0.0f) return;

    // Option の「振動」。両モーターへ等しく掛ける。
    // WHY masterScale と別に持つか: masterScale は作り手が全体の重さを決める値で、
    //     こちらは遊ぶ人が下げる値。同じ変数にすると、プレイヤーが 50% にした状態が
    //     作り手の調整値として保存され、次に触ったときの基準が判らなくなる。
    const float player = GameSettingsComponent::VibrationScale();
    low  *= player;
    high *= player;
    if (low <= 0.0f && high <= 0.0f) return;

    if (static_cast<int>(m_requests.size()) >= std::max(maxRequests, 1)) {
        const auto weakest = std::min_element(
            m_requests.begin(), m_requests.end(),
            [](const Request& a, const Request& b) {
                return std::max(a.low, a.high) < std::max(b.low, b.high);
            });
        if (weakest != m_requests.end() && std::max(weakest->low, weakest->high)
                                            >= std::max(low, high)) return;
        if (weakest != m_requests.end()) m_requests.erase(weakest);
    }

    m_requests.push_back({ Clamp01(low), Clamp01(high), duration, duration });
}

inline void RumbleManagerComponent::Sustain(RumbleChannel channel, float low, float high)
{
    const auto slot = static_cast<std::size_t>(channel);
    if (slot >= m_sustain.size()) return;
    // WHY 切ってあるときに 0 を書くか (素通りしないか): 照射中に振動を切ると、
    //     切る前の強さがそのまま残り、押している間ずっと震え続ける。
    m_sustain[slot] = m_enabled ? Level{ Clamp01(low), Clamp01(high) } : Level{};
}

inline void RumbleManagerComponent::StopSustain(RumbleChannel channel)
{
    const auto slot = static_cast<std::size_t>(channel);
    if (slot < m_sustain.size()) m_sustain[slot] = {};
}

inline void RumbleManagerComponent::StopAll()
{
    m_requests.clear();
    m_sustain.fill({});
    debugLow  = 0.0f;
    debugHigh = 0.0f;
    if (m_driving) {
        input.StopVibration();
        m_driving = false;
    }
}

inline void RumbleManagerComponent::OnUpdate()
{
    // WHY 実時間で数えるか: ヒットストップ中に振動まで止まると、最も手応えが要る瞬間に
    //     何も返ってこない。時間を止めても振動は進める。
    const float dt = std::max(time.UnscaledDeltaTime(), 0.0f);

    float low = 0.0f;
    float high = 0.0f;
    for (auto it = m_requests.begin(); it != m_requests.end();) {
        it->remaining -= dt;
        if (it->remaining <= 0.0f || it->duration <= EPSILON) {
            it = m_requests.erase(it);
            continue;
        }
        // 終わりへ向けて落とす。切れる瞬間に最大のまま止まると、ぶつ切りに感じる。
        const float falloff = Clamp01(it->remaining / it->duration);
        low  = std::max(low,  it->low  * falloff);
        high = std::max(high, it->high * falloff);
        ++it;
    }

    // WHY 遊ぶ人の倍率をここで掛けるか (Sustain の中ではなく): 持続振動は «今の強さ» を
    //     置いておく器なので、押しっぱなしのまま Option を動かされうる。書き込み時に
    //     掛けると、その 1 本の照射だけ古い倍率で震え続ける。
    const float player = GameSettingsComponent::VibrationScale();
    for (const Level& level : m_sustain) {
        low  = std::max(low,  level.low  * player);
        high = std::max(high, level.high * player);
    }

    low  *= Clamp01(masterScale);
    high *= Clamp01(masterScale);
    debugLow  = low;
    debugHigh = high;

    if (low <= 0.0f && high <= 0.0f) {
        if (m_driving) {
            input.StopVibration();
            m_driving = false;
        }
        return;
    }

    // WHY 毎フレーム出し直すか: SetVibration は指定秒数で自動停止する API なので、
    //     合成結果が毎フレーム変わる使い方では「今フレームぶん」を出し続けるしかない。
    //     少し長めに出すのは、フレーム落ちで一瞬途切れて振動がガタつくのを防ぐため。
    input.SetVibration(low, high, std::max(dt * 3.0f, 0.05f));
    m_driving = true;
}

} // namespace sandbox
