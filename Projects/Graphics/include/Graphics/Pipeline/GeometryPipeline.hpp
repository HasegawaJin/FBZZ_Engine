/// @file    GeometryPipeline.hpp
/// @brief   実効経路に従うジオメトリ描画パスの構成境界。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#pragma once

namespace fbzz::renderer { struct OpaqueRenderPlan; }

namespace fbzz::renderer {

class RenderPipeline;

/// @note スキニング・ライト供給・影を、本描画を登録する前に追加する。
void BuildGeometryPreparation(RenderPipeline& pipeline, bool clusteredEnabled);

/// @note 呼び出し元で解決済みの方式だけを使い、設定や GPU 資源の可用性を再判定しない。
/// @note 移行中は透明メッシュ・空・SSR も含む既存の登録順を保つ。AfterOpaque の是正とは分ける。
/// @see Docs/design/graphics-library.md
void BuildGeometryPipeline(RenderPipeline& pipeline, const renderer::OpaqueRenderPlan& plan);

/// @note 水面の背景に使う光芒とコースティクスを先に合成する。
void BuildWaterComposition(RenderPipeline& pipeline);

} /// @note namespace fbzz::renderer
