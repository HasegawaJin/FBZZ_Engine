/// @file    SkinningPolicy.hpp
/// @brief   Graphics が解決した Skinning 実行方式の問い合わせ。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include <Graphics/Api.hpp>
namespace fbzz::renderer {
class IRenderer;
[[nodiscard]] FBZZ_GRAPHICS_API bool UseOwnedSkinningBatch();
/// @note FBZZ_SKINNING_WAIT_OPTIMIZATION=0 keeps the original wait boundaries for comparison.
[[nodiscard]] FBZZ_GRAPHICS_API bool UseSkinningWaitOptimization();
/// @note Publishes an already-owned command prefix once per CPU frame when the worker is idle.
[[nodiscard]] FBZZ_GRAPHICS_API bool TryStartOwnedSkinningReadAhead(IRenderer& renderer);
/// @note Joins the accepted prefix before the producer resumes native descriptor access, without submitting later commands.
FBZZ_GRAPHICS_API void FinishOwnedSkinningReadAhead(IRenderer& renderer);
} /// @note namespace fbzz::renderer
