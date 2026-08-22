/// @file RumbleManagerComponent.hpp
/// @brief ゲームパッドの振動要求を合成し、パッドへ渡す値を 1 本にまとめる
/// @author Hasegawa Jin
/// @date 2026-08-22
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
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class RumbleManagerComponent : public Script {
    FBZZ_SCRIPT(RumbleManagerComponent)

public:
    FBZZ_GROUP("Default Shape")
    FBZZ_FIELD_RANGE(float, defaultLow, 0.55f, "Low Motor", 0.0f, 1.0f)
    FBZZ_TOOLTIP("強さ 1.0 の要求で回す低周波モーター (重い揺さぶり)")
    FBZZ_FIELD_RANGE(float, defaultHigh, 0.30f, "High Motor", 0.0f, 1.0f)
    FBZZ_TOOLTIP("強さ 1.0 の要求で回す高周波モーター (鋭い刺激)")
    FBZZ_FIELD_RANGE(float, defaultDuration, 0.18f, "Duration", 0.0f, 2.0f)

    FBZZ_GROUP("Limits")
    FBZZ_FIELD(bool, enabledOnStart, true, "Enabled")
    FBZZ_TOOLTIP("切ると全ての要求を無視する。振動が苦手な人向けの一括スイッチ")
    FBZZ_FIELD_RANGE(float, masterScale, 1.0f, "Master Scale", 0.0f, 1.0f)
    FBZZ_FIELD_RANGE_INT(int, maxRequests, 8, "Max Requests", 1, 64)

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(float, debugLow, 0.0f, "Low Output")
    FBZZ_FIELD_READ_ONLY(float, debugHigh, 0.0f, "High Output")

    [[nodiscard]] static RumbleManagerComponent* Instance() { return s_instance; }

    // 強さ (0..1) だけ渡す標準の呼び方。
    void Rumble(float strength01);
    // 両モーターと長さを直接指定する版。
    void Rumble(float low, float high, float duration);
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

    static inline RumbleManagerComponent* s_instance = nullptr;

    std::vector<Request> m_requests;
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

inline void RumbleManagerComponent::StopAll()
{
    m_requests.clear();
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
