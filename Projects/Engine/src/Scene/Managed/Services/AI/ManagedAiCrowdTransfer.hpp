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
    /// @brief 群衆の入力パケットを検証し、成功時にだけ更新後のスナップショットを公開する。
    /// @param[in] request 呼び出し元が所有する入力パケットと出力バッファ。書き込み範囲は入力と重ねない。
    /// @param[out] reply エージェント件数と出力バイト数を格納する。容量不足の場合は必要バイト数を返す。
    /// @return 失敗時は呼び出し元の出力バッファを変更しない。容量不足はシミュレーション前に返す。
    FBZZ_ENGINE_API ManagedBridgeStatus Run(const ManagedBridgeRequest& request, ManagedBridgeReply& reply);
private:
    std::vector<ai::CrowdProfile> m_profiles; ///< 入力パケットから展開するプロファイル配列。
    std::vector<ai::CrowdAgent> m_agents; ///< 更新前の個体を入力スロット順に保持する一時配列。
    std::vector<ai::CrowdAgent> m_output; ///< 公開前の更新後スナップショット。
    std::vector<ai::CrowdThreat> m_threats; ///< 入力パケットから展開するワールド空間の脅威球。
    std::vector<ai::CrowdObstacle> m_obstacles; ///< 入力パケットから展開するワールド空間の障害物球。
    ai::CrowdWorkspace m_workspace; ///< 呼び出し間で容量を再利用する群衆シミュレーションの作業領域。
    bool m_inFlight = false; ///< 作業領域を使用中で、同じ転送への再入を拒否する状態。
};
}
