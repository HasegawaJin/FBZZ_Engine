/// @file    GeometryPipeline.hpp
/// @brief   実効経路に従うジオメトリ描画パスの構成境界。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#pragma once
#include <Graphics/Api.hpp>
#include <functional>

namespace fbzz::renderer { struct OpaqueRenderPlan; }

namespace fbzz::renderer {

class RenderPipeline;

/// @note スキニング・ライト供給・影を、本描画を登録する前に追加する。
FBZZ_GRAPHICS_API void BuildGeometryPreparation(RenderPipeline& pipeline, bool clusteredEnabled);

/// @note 呼び出し元で解決済みの方式だけを使い、設定や GPU 資源の可用性を再判定しない。
/// @note Raster は既存順序を維持し、Hybrid SCREEN_FIRST は baseline と空から SSR 入力を作る。
/// @see Docs/design/graphics-library.md
/// @see Docs/design/RayTracing.md
/// @param deferredSurfaceReady 表面入力が揃った後、照明合成より前に追加するパス。
/// @param hybridScreenFirst SSR 入力を反射 resolve 前に作る Deferred 専用構成。
FBZZ_GRAPHICS_API void BuildGeometryPipeline(RenderPipeline& pipeline, const renderer::OpaqueRenderPlan& plan,
    const std::function<void()>& deferredSurfaceReady = {}, bool hybridScreenFirst = false);

/// @note 不透明描画後の拡張を登録してから呼ぶ。静的・スキンドを一つの半透明キューへ集める。
FBZZ_GRAPHICS_API void BuildGeometryTransparency(RenderPipeline& pipeline, const renderer::OpaqueRenderPlan& plan);

/// @note 水面の背景に使う光芒とコースティクスを先に合成する。
FBZZ_GRAPHICS_API void BuildWaterComposition(RenderPipeline& pipeline);

} /// @note namespace fbzz::renderer
