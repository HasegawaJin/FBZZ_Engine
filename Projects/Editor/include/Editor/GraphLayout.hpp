/// @file    GraphLayout.hpp
/// @brief   Animation Graph Editor 専用のノード配置情報を保持する。
/// @author  Hasegawa Jin
/// @date    2026-06-08
///
/// @note AnimatorComponent はランタイムデータなので、エディター上の表示座標を混ぜない。
#pragma once
#include <imgui.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::editor {

/// 1つの AnimatorComponent に対応するノード配置。
/// key は AnimationState::name。ステート名変更時はパネル側で追随して更新する。
struct GraphLayout {
    std::unordered_map<std::string, ImVec2> nodePositions;
    /// BlendTree は State を親レイヤーとして Motion ノード座標を個別に保持する。
    /// @note Motion はランタイムデータであり、Editor 座標を AnimatorComponent へ混在させない。
    std::unordered_map<std::string, std::vector<ImVec2>> blendTreeMotionPositions;
    /// 特殊ノードは State 名と衝突しない専用フィールドで保持する。
    /// @note 予約名を map key にすると、ユーザー定義 State と競合するため。
    ImVec2 entryPosition = ImVec2(-220.0f, 80.0f);
    ImVec2 anyStatePosition = ImVec2(-220.0f, 260.0f);
    ImVec2 slotPosition = ImVec2(-220.0f, 440.0f);
};

} // namespace fbzz::editor
