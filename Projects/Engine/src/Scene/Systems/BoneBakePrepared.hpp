/// @file    BoneBakePrepared.hpp
/// @brief   Callback-free readiness proof for batching prepared animation resource updates.
/// @author  Hasegawa Jin
/// @date    2026-10-18
#pragma once
#include <Engine/Scene/BoneBakeRuntime.hpp>

namespace fbzz::scene {
/// @note Includes cached rejections: this proves preparation performs no loading or logging, not that a bake is supported.
bool AreBoneBakeAssetsPrepared(const AnimatorComponent& animator, const asset::Model& model);
}
