/// @file    RenderEnvironmentExtraction.hpp
/// @brief   環境描画の水中設定と雲の抽出。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include <Graphics/Renderer/RenderEnvironment.hpp>

namespace fbzz::scene { struct RenderPassContext; }

namespace fbzz::scene::environment_extraction_detail {

renderer::RenderUnderwaterInput EvaluateUnderwaterInfo(const RenderPassContext& context);
renderer::RenderCausticsInput FindCausticsSource(RenderPassContext& context);
void ExtractCloud(RenderPassContext& context, renderer::RenderEnvironmentInput& environment);

} /// @note namespace fbzz::scene::environment_extraction_detail
