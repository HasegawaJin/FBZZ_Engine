// FBZZ Engine
// EnvironmentResources.cpp | fbzz::scene
// 空連動 IBL の状態オブジェクト — dirty 判定の実装。
#include <Engine/Scene/Systems/RenderPasses/EnvironmentResources.hpp>
#include <cmath>

namespace fbzz::scene {

bool EnvironmentResources::SkySignature::ApproxEquals(const SkySignature& o, float eps) const
{
    // 全駆動パラメータが許容誤差内なら「同じ空」とみなす。
    // 1 つでも差があれば IBL を焼き直す必要があるため早期 return する。
    // NOTE: 関数名に near/far を使わない (windows.h が near/far を空マクロとして定義するため)。
    auto approx = [eps](float a, float b) { return std::fabs(a - b) <= eps; };
    auto approxVec = [&](const math::Vector3& a, const math::Vector3& b) {
        return approx(a.x, b.x) && approx(a.y, b.y) && approx(a.z, b.z);
    };

    return approxVec(sunDirection, o.sunDirection)
        && approxVec(rayleigh, o.rayleigh)
        && approx(mieScattering, o.mieScattering)
        && approx(sunIntensity, o.sunIntensity)
        && approx(mieG, o.mieG)
        && approx(planetRadius, o.planetRadius)
        && approx(atmosphereRadius, o.atmosphereRadius);
}

bool EnvironmentResources::ConsumeDirty(const SkySignature& current)
{
    // 強制 dirty (初回 / MarkDirty) か、signature が変化していれば再ベイクが必要。
    const bool changed = m_forceDirty || !m_hasSignature
                      || !m_lastSignature.ApproxEquals(current);

    // どちらにせよ最新 signature を記録し、強制フラグは消費する。
    m_lastSignature = current;
    m_hasSignature  = true;
    m_forceDirty    = false;

    return changed;
}

} // namespace fbzz::scene
