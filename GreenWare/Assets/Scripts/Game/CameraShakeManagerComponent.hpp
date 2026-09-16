/// @file    CameraShakeManagerComponent.hpp
/// @brief   複数のカメラ揺れを加算合成し、カメラへ渡すオフセットを 1 本にまとめる
/// @author  Hasegawa Jin
/// @date    2026-08-22
///
/// WHY 加算にするか:
///   以前は TpsCameraComponent が「強い方が勝つ」1 本だけを持っていた。被弾と衝突が
///   同じ瞬間に起きるのは普通で、そのとき弱い方は消える。消えた側は「揺れなかった」
///   ではなく「反応が無かった」と読まれるので、当たったのに手応えが無い瞬間ができる。
///   それぞれを独立した波として持ち、足し合わせてから 1 つのオフセットにする。
///
/// WHY 揺れの生成をカメラから切り離すか:
///   揺らしたい側 (衝突・被弾・着地) はカメラの追従方式を知らなくてよいし、
///   カメラは誰がなぜ揺らしたいのかを知らなくてよい。カメラは合成済みのオフセットを
///   1 つ受け取るだけにすると、追従を作り変えても揺れの調整はやり直しにならない。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <Math/MathUtils.hpp>
#include <Scripts/Game/GameSettingsComponent.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

using namespace fbzz::scene;
using namespace fbzz::math;
using fbzz::Time;

namespace sandbox {

class CameraShakeManagerComponent : public Script {
    FBZZ_SCRIPT(CameraShakeManagerComponent)

public:
    FBZZ_GROUP("既定の形")
    FBZZ_FIELD_RANGE(float, defaultAmplitude, 0.35f, "振幅", 0.0f, 3.0f)
    FBZZ_TOOLTIP("強さ 1.0 の要求で揺れる幅 (ワールド単位)")
    FBZZ_FIELD_RANGE(float, defaultFrequency, 32.0f, "Frequency", 1.0f, 120.0f)
    FBZZ_TOOLTIP("揺れの速さ。低いと重く、高いと鋭く感じる")
    FBZZ_FIELD_RANGE(float, defaultDuration, 0.25f, "継続時間", 0.0f, 3.0f)

    FBZZ_GROUP("限界")
    FBZZ_FIELD_RANGE(float, maxAmplitude, 0.9f, "Max Amplitude", 0.0f, 5.0f)
    FBZZ_TOOLTIP("合成後の上限。加算なので、同時多発したときに画面が壊れるのを防ぐ")
    FBZZ_FIELD_RANGE_INT(int, maxShakes, 8, "Max Shakes", 1, 64)
    FBZZ_TOOLTIP("同時に保持する揺れの本数。超えたら最も弱いものから捨てる")
    FBZZ_FIELD_RANGE(float, verticalRatio, 0.65f, "Vertical Ratio", 0.0f, 2.0f)
    FBZZ_TOOLTIP("横に対する縦の振れ幅。1 未満だと横揺れ主体になる")

    FBZZ_GROUP("デバッグ")
    FBZZ_FIELD_READ_ONLY(int, debugActive, 0, "Active Shakes")
    FBZZ_FIELD_READ_ONLY(float, debugAmplitude, 0.0f, "Blended Amplitude")

    [[nodiscard]] static CameraShakeManagerComponent* Instance() { return s_instance; }

    // 強さ (0..1) だけ渡す標準の呼び方。形は既定値から作る。
    void Shake(float strength01);
    // 形まで指定する版。着地と衝突で揺れ方を描き分けたいときに使う。
    void Shake(float amplitude, float frequency, float duration);
    /// 揺れではなく «押し込み»。カメラのローカル空間で offset ぶん一瞬ずれ、seconds で戻る。
    ///
    /// WHY 揺れと別に持つか: 揺れは正弦波なので向きを持たず、«当たった方へ食い込む»
    ///     が作れない。斬撃の手応えは「前へ数 cm 沈んで戻る」の 1 往復で、
    ///     往復を繰り返す揺れとは別の語。
    void Punch(const Vector3& localOffset, float seconds);
    void StopAll() { m_shakes.clear(); m_punches.clear(); }

    // カメラが毎フレーム読む合成済みオフセット (カメラのローカル空間)。
    [[nodiscard]] Vector3 CurrentOffset() const { return m_offset; }

    void OnStart() override;
    void OnLateUpdate() override;
    void OnDestroy() override { if (s_instance == this) s_instance = nullptr; }

private:
    struct Shake_ {
        float amplitude = 0.0f;
        float frequency = 0.0f;
        float duration  = 0.0f;
        float remaining = 0.0f;
        float seed      = 0.0f;
    };

    struct Punch_ {
        Vector3 offset    = Vector3::ZERO;
        float   duration  = 0.0f;
        float   remaining = 0.0f;
    };

    static inline CameraShakeManagerComponent* s_instance = nullptr;

    std::vector<Shake_> m_shakes;
    std::vector<Punch_> m_punches;
    Vector3 m_offset = Vector3::ZERO;
    float   m_seedCounter = 0.0f;
};

FBZZ_REFLECT(CameraShakeManagerComponent)

inline void CameraShakeManagerComponent::OnStart()
{
    if (s_instance && s_instance != this) {
        debug.LogWarning("CameraShakeManagerComponent: another instance is already active. "
                         "The most recently started one takes over.");
    }
    s_instance = this;
    m_shakes.clear();
    m_offset = Vector3::ZERO;
}

inline void CameraShakeManagerComponent::Shake(float strength01)
{
    const float strength = Clamp01(strength01);
    if (strength <= 0.0f) return;
    Shake(defaultAmplitude * strength, defaultFrequency, defaultDuration);
}

inline void CameraShakeManagerComponent::Shake(float amplitude, float frequency, float duration)
{
    // Option の「カメラ揺れ」。WHY 1 引数版ではなくここで掛けるか: 形まで指定する版も
    //     同じ揺れなので、設定を 0 にしたのに一部の演出だけ揺れる状態を作らない。
    //     ここは全ての要求が必ず通る 1 本道。
    amplitude *= GameSettingsComponent::ShakeScale();
    if (amplitude <= 0.0f || duration <= 0.0f) return;

    if (static_cast<int>(m_shakes.size()) >= std::max(maxShakes, 1)) {
        // 上限に達したら最も弱いものを捨てる。古い順に捨てると、直前の強い一撃が
        // 弱い揺れの連打で押し出されて消える。
        const auto weakest = std::min_element(
            m_shakes.begin(), m_shakes.end(),
            [](const Shake_& a, const Shake_& b) { return a.amplitude < b.amplitude; });
        if (weakest != m_shakes.end() && weakest->amplitude >= amplitude) return;
        if (weakest != m_shakes.end()) m_shakes.erase(weakest);
    }

    // 位相を 1 本ごとにずらす。同位相だと重ねても振れ幅が増えるだけで、
    // 「別々の衝撃が来ている」ようには見えない。
    m_seedCounter += 1.6180339f;
    m_shakes.push_back({ amplitude, std::max(frequency, 1.0f), duration, duration, m_seedCounter });
}

inline void CameraShakeManagerComponent::Punch(const Vector3& localOffset, float seconds)
{
    const Vector3 offset = localOffset * GameSettingsComponent::ShakeScale();
    if (offset.LengthSq() <= EPSILON || seconds <= 0.0f) return;
    if (static_cast<int>(m_punches.size()) >= std::max(maxShakes, 1)) m_punches.erase(m_punches.begin());
    m_punches.push_back({ offset, seconds, seconds });
}

inline void CameraShakeManagerComponent::OnLateUpdate()
{
    // WHY 実時間で進めるか: ヒットストップ中も揺れは進めたい。停止中に完全静止すると
    //     「ドンッ」の最初のフレームが無反応に見え、解除後に遅れて揺れる。
    const float dt = std::max(time.UnscaledDeltaTime(), 0.0f);
    const float now = Time::unscaledTime;

    Vector3 sum = Vector3::ZERO;
    for (auto it = m_shakes.begin(); it != m_shakes.end();) {
        it->remaining -= dt;
        if (it->remaining <= 0.0f || it->duration <= EPSILON) {
            it = m_shakes.erase(it);
            continue;
        }

        // 終わりに向けて二乗で減衰させる。線形だと最後まで揺れ続けて収まりが悪い。
        const float falloff = Clamp01(it->remaining / it->duration);
        const float weight  = it->amplitude * falloff * falloff;
        const float phase   = now * it->frequency + it->seed;
        sum.x += std::sin(phase * 1.17f) * weight;
        sum.y += std::cos(phase * 1.73f) * weight * verticalRatio;
        ++it;
    }

    // 加算なので同時多発すると青天井になる。方向は保ったまま長さだけ抑える。
    const float length = sum.Length();
    const float limit  = std::max(maxAmplitude, 0.0f);
    if (length > limit && length > EPSILON) sum = sum * (limit / length);

    // 押し込みは最初の 1 コマで最大、あとは二乗で戻る。揺れの上限には含めない
    // (向きを持つずれを長さで削ると、押し込んだ方向そのものが変わる)。
    for (auto it = m_punches.begin(); it != m_punches.end();) {
        it->remaining -= dt;
        if (it->remaining <= 0.0f || it->duration <= EPSILON) {
            it = m_punches.erase(it);
            continue;
        }
        const float falloff = Clamp01(it->remaining / it->duration);
        sum += it->offset * (falloff * falloff);
        ++it;
    }

    m_offset = sum;
    debugActive    = static_cast<int>(m_shakes.size());
    debugAmplitude = length;
}

} // namespace sandbox
