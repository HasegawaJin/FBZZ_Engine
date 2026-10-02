/// @file    RayPathHistory.cpp
/// @brief   フレーム番号によらない Path 履歴キーの厳密比較とサンプル進行。
/// @author  Hasegawa Jin
/// @date    2026-10-01
#include <Graphics/RayTracing/RayPathHistory.hpp>
#include <Graphics/RayTracing/RayPathScene.hpp>
#include <bit>
#include <limits>

namespace fbzz::renderer {

bool RayPathIntegratorSettings::operator==(const RayPathIntegratorSettings& other) const
{
    return maxBounces == other.maxBounces && rouletteStartBounce == other.rouletteStartBounce
        && seed == other.seed && samplerVersion == other.samplerVersion
        && integratorVersion == other.integratorVersion
        && std::bit_cast<uint32_t>(maxDistance) == std::bit_cast<uint32_t>(other.maxDistance)
        && std::bit_cast<uint32_t>(radianceClamp) == std::bit_cast<uint32_t>(other.radianceClamp);
}

RayPathHistoryKey MakeRayPathHistoryKey(const RayPathScene& scene, const Camera& camera,
    uint32_t width, uint32_t height, const RayPathIntegratorSettings& integrator,
    uint64_t deviceEpoch, uint64_t shaderEpoch)
{
    RayPathHistoryKey key;
    key.sceneGeneration = scene.sceneGeneration;
    key.sceneContentRevision = scene.contentRevision;
    key.deviceEpoch = deviceEpoch;
    key.shaderEpoch = shaderEpoch;
    key.width = width;
    key.height = height;
    key.cameraWords = {std::bit_cast<uint32_t>(camera.m_position.x),
        std::bit_cast<uint32_t>(camera.m_position.y), std::bit_cast<uint32_t>(camera.m_position.z),
        std::bit_cast<uint32_t>(camera.m_rotation.x), std::bit_cast<uint32_t>(camera.m_rotation.y),
        std::bit_cast<uint32_t>(camera.m_rotation.z), std::bit_cast<uint32_t>(camera.m_rotation.w),
        std::bit_cast<uint32_t>(camera.m_fovY), std::bit_cast<uint32_t>(camera.m_aspect),
        std::bit_cast<uint32_t>(camera.m_near), std::bit_cast<uint32_t>(camera.m_far),
        std::bit_cast<uint32_t>(camera.m_orthoHeight), static_cast<uint32_t>(camera.m_projection)};
    key.integrator = integrator;
    return key;
}

bool RayPathHistory::Prepare(const RayPathHistoryKey& key)
{
    if (m_prepared && m_key == key) return false;
    m_key = key;
    m_sampleCount = 0;
    m_prepared = true;
    return true;
}

bool RayPathHistory::Commit(uint32_t sampleCount)
{
    if (!m_prepared || sampleCount == 0
        || sampleCount > std::numeric_limits<uint32_t>::max() - 1 - m_sampleCount) return false;
    m_sampleCount += sampleCount;
    return true;
}

void RayPathHistory::Reset()
{
    m_key = {};
    m_sampleCount = 0;
    m_prepared = false;
}

} /// @note namespace fbzz::renderer
