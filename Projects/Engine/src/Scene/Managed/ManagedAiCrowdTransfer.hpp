/// @file    ManagedAiCrowdTransfer.hpp
/// @brief   Validated numeric crowd packets with retained native work buffers.
/// @author  Hasegawa Jin
/// @date    2026-10-11
#pragma once
#include <Engine/AI/CrowdSimulation.hpp>
#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <vector>

namespace fbzz::scene::managed_detail {

/// @note Main callback thread only; no scene, caller pointer or agent identity is retained between Run calls.
class ManagedAiCrowdTransfer {
public:
    /// @return Failure leaves caller output untouched; BUFFER_TOO_SMALL reports exact outputBytes before simulation.
    ManagedBridgeStatus Run(const ManagedBridgeRequest& request, ManagedBridgeReply& reply);
private:
    std::vector<ai::CrowdProfile> m_profiles;
    std::vector<ai::CrowdAgent> m_agents, m_output;
    std::vector<ai::CrowdThreat> m_threats;
    std::vector<ai::CrowdObstacle> m_obstacles;
    ai::CrowdWorkspace m_workspace;
    bool m_inFlight = false;
};
}
