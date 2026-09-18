/// @file    FiberMotionHistory.hpp
/// @brief   ビューごとの繊維変形履歴。GPU 資源は所有しない。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#pragma once
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>

namespace fbzz::renderer {

struct FiberDeformationState {
    math::Matrix4 m_world = math::Matrix4::Identity();
    math::Vector3 m_wind{};
    float m_time = 0.0f;
    float m_turbulence = 0.0f;
    float m_pulseFrequency = 0.0f;
    float m_length = 0.0f;
    float m_windResponse = 0.0f;
    float m_maxBend = 0.0f;
    float m_gravityBend = 0.0f;
    int m_mode = 0;
    int m_shellCount = 1;
};

struct FiberMotionHistory {
    uint64_t m_viewKey = 0;
    uint64_t m_surfaceKey = 0;
    uint64_t m_resourceVersion = 0;
    uint64_t m_frame = 0;
    bool m_initialized = false;
    bool m_valid = false;
    FiberDeformationState m_current;
    FiberDeformationState m_previous;

    /// @note 同一フレームの再描画では履歴を進めない。欠落・時刻巻戻し・方式変更は速度をリセットする。
    /// @see https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-27-motion-blur-post-processing-effect
    void Advance(uint64_t frame, const FiberDeformationState& state)
    {
        if (m_initialized && m_frame == frame) return;
        m_valid = m_initialized && frame > m_frame && frame - m_frame == 1
            && state.m_time >= m_current.m_time
            && state.m_mode == m_current.m_mode && state.m_shellCount == m_current.m_shellCount;
        m_previous = m_valid ? m_current : state;
        m_current = state;
        m_frame = frame;
        m_initialized = true;
    }
};

} // namespace fbzz::renderer
