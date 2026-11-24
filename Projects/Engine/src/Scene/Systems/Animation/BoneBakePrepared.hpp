/// @file    BoneBakePrepared.hpp
/// @brief   コールバックなしでバッチ更新可能なアニメーション資源を判定する。
/// @author  Hasegawa Jin
/// @date    2026-10-18
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/Animation/BoneBakeRuntime.hpp>

namespace fbzz::scene {
/// @note ベイク不可のキャッシュも含む。準備時の読込やログ出力を省けるが、ベイクの可否は各項目で判断する。
FBZZ_ENGINE_API bool AreBoneBakeAssetsPrepared(const AnimatorComponent& animator, const asset::Model& model);
}
