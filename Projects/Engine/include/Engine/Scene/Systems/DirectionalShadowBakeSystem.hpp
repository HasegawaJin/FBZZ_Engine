/// @file    DirectionalShadowBakeSystem.hpp
/// @brief   Play 前の方向ライト静的影ベイクと状態照会。
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Engine/Api.hpp>
#include <filesystem>

namespace fbzz::renderer { class IRenderer; class ResourceManager; struct RenderSettings; }

namespace fbzz::scene {
class Scene;

enum class DirectionalShadowBakeStatus { NOT_REQUIRED, PENDING, READY, UNAVAILABLE };

/// @note 次の Game View 抽出を要求する。cacheDirectory はプロジェクトの Library 内。
[[nodiscard]] FBZZ_ENGINE_API DirectionalShadowBakeStatus RequestDirectionalShadowBake(
    Scene& scene, const std::filesystem::path& cacheDirectory);
[[nodiscard]] FBZZ_ENGINE_API DirectionalShadowBakeStatus GetDirectionalShadowBakeStatus(const Scene& scene);
FBZZ_ENGINE_API void CancelDirectionalShadowBake(const Scene& scene);

/// @pre Application の CPU フレーム開始後、ネイティブの描画フレーム開始前。
/// @note 1 回に 1 プリセットを完成させ、次の通常描画が記録できる状態に戻す。
FBZZ_ENGINE_API void ProcessDirectionalShadowBake(Scene& scene, renderer::IRenderer& renderer,
    renderer::ResourceManager& resources, const renderer::RenderSettings& settings);

} /// @note namespace fbzz::scene
