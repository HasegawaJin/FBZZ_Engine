/// @file    EnvironmentResources.cpp
/// @brief   空連動 IBL の状態オブジェクト — dirty 判定の実装。
/// @author  Hasegawa Jin
/// @date    2026-07-01
#include <Graphics/Pipeline/EnvironmentResources.hpp>
#include <cmath>

namespace fbzz::renderer {

bool EnvironmentResources::SkySignature::AtmosphereApproxEquals(const SkySignature& o, float eps) const
{
    /// @note 太陽方向を除く駆動パラメータが許容誤差内なら「(方向以外は) 同じ空」とみなす。太陽方向は連続的に動くため ConsumeDirty の角度閾値で別途扱う。
    /// @note 引数名に near/far を使わない (`windows.h` が near/far を空マクロとして定義するため)。
    auto approx = [eps](float a, float b) { return std::fabs(a - b) <= eps; };
    auto approxVec = [&](const math::Vector3& a, const math::Vector3& b) {
        return approx(a.x, b.x) && approx(a.y, b.y) && approx(a.z, b.z);
    };

    return approxVec(rayleigh, o.rayleigh)
        && approx(mieScattering, o.mieScattering)
        && approx(skyScatterIntensity, o.skyScatterIntensity)
        && approx(mieG, o.mieG)
        && approx(planetRadius, o.planetRadius)
        && approx(atmosphereRadius, o.atmosphereRadius);
}

bool EnvironmentResources::ConsumeDirty(const SkySignature& current, float currentTimeSeconds)
{
    /// @note 強制 dirty (初回 / MarkDirty / 解像度変更) は間引かず即座にベイクする。
    if (m_forceDirty || !m_hasSignature) {
        m_lastSignature = current;
        m_hasSignature  = true;
        m_forceDirty    = false;
        m_lastBakeTime  = currentTimeSeconds;
        return true;
    }

    /// @note last frame でなく last "baked" signature と比較する。微小変化の累積を取りこぼさず、ゆっくりした太陽移動でも閾値超過時点で必ず焼き直せる。
    /// @note 太陽方向は角度閾値、大気パラメータは eps 比較で変化を判定する。
    /// @note cos(約1.5°): これ以上動いたら再ベイク対象
    constexpr float kSunAngleCosThreshold = 0.99966f;
    constexpr float kParamEps             = 1.0e-4f;
    /// @note 秒: 時間帯アニメ中の最大ベイク頻度 (約30Hz)
    constexpr float kMinRebakeInterval    = 0.033f;

    const float sunCos  = math::Vector3::Dot(m_lastSignature.sunDirection, current.sunDirection);
    const bool  changed = (sunCos < kSunAngleCosThreshold)
                       || !m_lastSignature.AtmosphereApproxEquals(current, kParamEps);
    if (!changed)
        /// @note 空が実質変わっていない → キャッシュ済み IBL を再利用
        return false;

    /// @note 変化はあるが、連続変化 (時間帯アニメ) を毎フレーム焼き直さないよう最低間隔で間引く。
    /// @note 間隔未達なら signature を更新せずに変化を保持し、次の機会にまとめて焼き直す。
    if (currentTimeSeconds - m_lastBakeTime < kMinRebakeInterval)
        return false;

    m_lastSignature = current;
    m_lastBakeTime  = currentTimeSeconds;
    return true;
}

} /// @note namespace fbzz::renderer
