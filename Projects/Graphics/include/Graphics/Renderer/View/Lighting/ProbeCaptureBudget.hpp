/// @file    ProbeCaptureBudget.hpp
/// @brief   Shared frame limits and fair owner cursors for atomic probe capture attempts.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#pragma once
#include <cstdint>

namespace fbzz::renderer {

/// @note One attempt includes all six faces and convolution; failure also spends its slot. No partially updated cube is published.
/// @see Docs/design/RayTracing.md Probe updates share a frame budget across views.
class ProbeCaptureBudget {
public:
    [[nodiscard]] bool CanAttempt(uint64_t frameSerial, uint64_t resourceEpoch, uint32_t limit)
    {
        if (!m_hasFrame || frameSerial != m_frameSerial || resourceEpoch != m_resourceEpoch) {
            m_hasFrame = true;
            m_frameSerial = frameSerial;
            m_resourceEpoch = resourceEpoch;
            m_attempts = 0;
        }
        return m_attempts < limit;
    }

    void MarkAttempt(uint64_t sceneGeneration, uint32_t ownerIndex, uint32_t ownerGeneration)
    {
        ++m_attempts;
        m_sceneGeneration = sceneGeneration;
        m_ownerIndex = ownerIndex;
        m_ownerGeneration = ownerGeneration;
        m_hasCursor = true;
    }

    [[nodiscard]] bool IsAfterCursor(uint64_t sceneGeneration, uint32_t ownerIndex, uint32_t ownerGeneration) const
    {
        return !m_hasCursor || sceneGeneration != m_sceneGeneration || ownerIndex > m_ownerIndex
            || (ownerIndex == m_ownerIndex && ownerGeneration > m_ownerGeneration);
    }

private:
    uint64_t m_frameSerial = 0, m_resourceEpoch = 0, m_sceneGeneration = 0;
    uint32_t m_attempts = 0, m_ownerIndex = 0, m_ownerGeneration = 0;
    bool m_hasFrame = false, m_hasCursor = false;
};

} /// @note namespace fbzz::renderer
