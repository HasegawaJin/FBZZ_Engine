/// @file CameraFollowManagerComponent.hpp
/// @brief カメラ追従を「緩めたい」要求を軸ごとに合成し、カメラへ 0..1 のたるみを 1 本渡す
/// @author Hasegawa Jin
/// @date 2026-08-22
///
/// WHY 追従を緩めると手触りが出るか:
///   カメラがプレイヤーへ常に密着していると、画面の中でプレイヤーはほとんど動かない。
///   跳んでも回避しても、動いているのは背景だけになり、動作の大きさが伝わらない。
///   その瞬間だけ追従を緩めると、プレイヤーが画面の中を移動する。跳んだ高さも
///   回避の距離も、画面内での変位として初めて目に見える。
///
/// WHY 揺れと同じくカメラの外に置くか:
///   緩めたい側 (ジャンプ・回避・被弾・ボスの登場) は、カメラが指数補間なのか
///   バネなのかを知らなくてよい。カメラは誰がなぜ緩めたいのかを知らなくてよい。
///   CameraShakeManagerComponent と同じ形にして、要求の出し方を 1 つに揃える。
///
/// WHY 緩めるのは即座・戻すのは滑らかか:
///   踏み切りの瞬間に緩みが遅れて効くと、一番見せたい立ち上がりを逃す。
///   逆に着地でいきなり密着へ戻すと、カメラが飛んで見える。
///   非対称にすると、始まりは鋭く終わりは静かになる。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;

namespace sandbox {

class CameraFollowManagerComponent : public Script {
    FBZZ_SCRIPT(CameraFollowManagerComponent)

public:
    FBZZ_GROUP("Recovery")
    FBZZ_FIELD_RANGE(float, recoverSpeed, 4.0f, "Recover Speed", 0.5f, 30.0f)
    FBZZ_TOOLTIP("要求が切れてから密着へ戻る速さ。低いほど余韻が残る")

    FBZZ_GROUP("Limits")
    FBZZ_FIELD_RANGE_INT(int, maxRequests, 8, "Max Requests", 1, 32)
    FBZZ_TOOLTIP("同時に保持する要求の本数。超えたら最も弱いものから捨てる")

    FBZZ_GROUP("Debug")
    FBZZ_FIELD_READ_ONLY(int, debugActive, 0, "Active Requests")
    FBZZ_FIELD_READ_ONLY(float, debugHorizontal, 0.0f, "Horizontal Slack")
    FBZZ_FIELD_READ_ONLY(float, debugVertical, 0.0f, "Vertical Slack")

    [[nodiscard]] static CameraFollowManagerComponent* Instance() { return s_instance; }

    /// 軸ごとに 0 (密着のまま) 〜 1 (最も緩い) で、duration 秒ぶん要求する。
    /// 状態が続く間ゆるめたいなら、短い duration で毎フレーム呼び直す。
    /// そうすると条件が消えた時点から自然に戻り始め、解除の呼び出しが要らない。
    void Loosen(float horizontal01, float vertical01, float duration);

    /// カメラが毎フレーム読む合成済みのたるみ (0..1)。
    [[nodiscard]] float HorizontalSlack() const { return m_horizontal; }
    [[nodiscard]] float VerticalSlack() const { return m_vertical; }

    void OnStart() override;
    void OnLateUpdate() override;
    void OnDestroy() override { if (s_instance == this) s_instance = nullptr; }

private:
    struct Request_ {
        float horizontal = 0.0f;
        float vertical   = 0.0f;
        float remaining  = 0.0f;
    };

    static inline CameraFollowManagerComponent* s_instance = nullptr;

    std::vector<Request_> m_requests;
    float m_horizontal = 0.0f;
    float m_vertical   = 0.0f;
};

FBZZ_REFLECT(CameraFollowManagerComponent)

inline void CameraFollowManagerComponent::OnStart()
{
    if (s_instance && s_instance != this) {
        debug.LogWarning("CameraFollowManagerComponent: another instance is already active. "
                         "The most recently started one takes over.");
    }
    s_instance = this;
    m_requests.clear();
    m_horizontal = 0.0f;
    m_vertical   = 0.0f;
}

inline void CameraFollowManagerComponent::Loosen(float horizontal01, float vertical01,
                                                 float duration)
{
    const float horizontal = Clamp01(horizontal01);
    const float vertical   = Clamp01(vertical01);
    if (duration <= 0.0f || (horizontal <= 0.0f && vertical <= 0.0f)) return;

    // 毎フレーム呼び直す使い方が前提なので、同じ強さの要求が溜まっても
    // 合成は最大値で行う。本数の上限は暴走したときの保険でしかない。
    if (static_cast<int>(m_requests.size()) >= std::max(maxRequests, 1)) {
        const auto weakest = std::min_element(
            m_requests.begin(), m_requests.end(),
            [](const Request_& a, const Request_& b) {
                return std::max(a.horizontal, a.vertical) < std::max(b.horizontal, b.vertical);
            });
        if (weakest == m_requests.end()) return;
        if (std::max(weakest->horizontal, weakest->vertical) >= std::max(horizontal, vertical))
            return;
        m_requests.erase(weakest);
    }

    m_requests.push_back({ horizontal, vertical, duration });
}

inline void CameraFollowManagerComponent::OnLateUpdate()
{
    // WHY 実時間で数えるか: ヒットストップ中に緩みまで止まると、止めが解けた瞬間に
    //     カメラが一気に追い付く。止めている間も緩みは進めておく。
    const float dt = std::max(time.UnscaledDeltaTime(), 0.0f);

    // WHY 合成が加算ではなく最大値か: たるみは倍率であって量ではない。回避と
    //     ジャンプが重なったときに足すと 1 を超え、意味が消える。
    //     「一番緩めたい要求」がその軸を決める、が素直な合成になる。
    float targetHorizontal = 0.0f;
    float targetVertical   = 0.0f;
    for (auto it = m_requests.begin(); it != m_requests.end();) {
        it->remaining -= dt;
        if (it->remaining <= 0.0f) {
            it = m_requests.erase(it);
            continue;
        }
        targetHorizontal = std::max(targetHorizontal, it->horizontal);
        targetVertical   = std::max(targetVertical, it->vertical);
        ++it;
    }

    const float recover = 1.0f - std::exp(-std::max(recoverSpeed, 0.0f) * dt);
    const auto approach = [&](float current, float target) {
        return target >= current ? target : current + (target - current) * recover;
    };
    m_horizontal = approach(m_horizontal, targetHorizontal);
    m_vertical   = approach(m_vertical, targetVertical);

    debugActive     = static_cast<int>(m_requests.size());
    debugHorizontal = m_horizontal;
    debugVertical   = m_vertical;
}

} // namespace sandbox
