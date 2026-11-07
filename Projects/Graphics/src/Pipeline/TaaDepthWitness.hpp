/// @file    TaaDepthWitness.hpp
/// @brief   TAA の静止深度を保証できる入力だけを比較用の値列へ抽出する内部契約。
/// @author  Hasegawa Jin
/// @date    2026-11-06
#pragma once
#include <Graphics/Api.hpp>
#include <cstdint>
#include <vector>

namespace fbzz::renderer {
struct RenderPassContext;
namespace taa_depth_detail {
/// @return 静止した不透明深度を証明できるとき true。false のとき witness は使用しない。
/// @note Scene・資源の借用は呼出中に限り、履歴には値だけを残す。
FBZZ_GRAPHICS_API bool BuildDepthWitness(RenderPassContext& context, std::vector<uint64_t>& witness);
}
}
