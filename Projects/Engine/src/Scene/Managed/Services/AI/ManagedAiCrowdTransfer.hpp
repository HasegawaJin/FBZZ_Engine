/// @file    ManagedAiCrowdTransfer.hpp
/// @brief   ネイティブ側の作業バッファを再利用する、群衆の数値パケットの検証と実行。
/// @author  Hasegawa Jin
/// @date    2026-10-11
#pragma once
#include <Engine/Api.hpp>
#include <Engine/AI/CrowdSimulation.hpp>
#include <Engine/Scene/ManagedBridgeAbi.hpp>
#include <vector>

namespace fbzz::scene::managed_detail {

/// @note メインスレッド上のコールバックからだけ呼ぶ。Run 間でシーン、呼び出し元のポインター、エージェントの識別情報は保持しない。
class ManagedAiCrowdTransfer {
public:
    /// @return 失敗時は呼び出し元の出力を変更しない。BUFFER_TOO_SMALL はシミュレーション前に必要な outputBytes を正確に返す。
    FBZZ_ENGINE_API ManagedBridgeStatus Run(const ManagedBridgeRequest& request, ManagedBridgeReply& reply);
private:
    std::vector<ai::CrowdProfile> m_profiles;
    std::vector<ai::CrowdAgent> m_agents, m_output;
    std::vector<ai::CrowdThreat> m_threats;
    std::vector<ai::CrowdObstacle> m_obstacles;
    ai::CrowdWorkspace m_workspace;
    bool m_inFlight = false;
};
}
