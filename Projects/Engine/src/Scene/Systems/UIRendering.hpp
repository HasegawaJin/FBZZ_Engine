/// @file    UIRendering.hpp
/// @brief   UI の描画処理が共有する Canvas 状態と内部インターフェース。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#pragma once

#include <Engine/Scene/Systems/UISystem.hpp>
#include <Engine/Renderer/RenderLayer.hpp>
#include <cstddef>

namespace fbzz::asset { struct UIThemeStyle; }
namespace fbzz::renderer { struct DrawCall; }

namespace fbzz::scene {
struct UIButton;
struct UIImage;
struct UIText;

namespace ui_runtime_detail {
struct Rect;
struct UITransform2D;
}

namespace ui_render_detail {

struct CanvasDrawContext {
    renderer::IRenderer& device;
    renderer::ResourceManager& resources;
    UISystemContext& context;
    const math::Matrix4& canvasToClip;
    renderer::ResourceHandle<renderer::PipelineStateTag> pipelineState;
    renderer::RenderLayer layer;
};

/// @note マスクの半平面と 180 度以下の扇形は、同じ凸多角形クリップを使う。
inline constexpr int MAX_CLIP_POLYGON_VERTICES = 32;

struct ClipVertex {
    math::Vector2 position;
    math::Vector2 uv;
    math::Vector4 color{ 1.0f, 1.0f, 1.0f, 1.0f };
};

/// @note dot(position - point, normal) >= 0 側を残す。入出力の頂点は同じ座標系で扱う。
int ClipHalfPlane(const ClipVertex* input, int vertexCount, const math::Vector2& point,
    const math::Vector2& normal, ClipVertex* output, int outputCapacity);
/// @note 頂点は Canvas 空間。描画側の Mask / Scroll View の全半平面を適用する。
int ClipPolygonToPlanes(const std::vector<UIClipPlane>& planes, const ClipVertex* input,
    int vertexCount, ClipVertex* output, int outputCapacity);
/// @return スタックへ積んだ面の数。呼び出し側が子孫の描画後に取り除く。
std::size_t PushClipRect(std::vector<UIClipPlane>& planes, const ui_runtime_detail::Rect& rect, float rotationZ);

void PrepareUIResources(UISystemContext& context, renderer::ResourceManager& resources);
UIMaterialBinding* ResolveUIMaterial(UISystemContext& context, renderer::ResourceManager& resources,
    const std::string& materialPath);
/// @note テキストは firstTextureSlot = 1 を渡し、スロット 0 をフォントアトラス用に確保する。
void BindUIMaterial(const CanvasDrawContext& draw, const UIMaterialBinding& binding,
    renderer::DrawCall& drawCall, std::size_t firstTextureSlot);
void ApplyUIMaterialOverrides(renderer::ResourceManager& resources, UIMaterialBinding& binding,
    const UIImage& image, renderer::DrawCall& drawCall);
renderer::FontAtlas& GetOrLoadFontAtlas(const std::string& basePath, float rasterPixelHeight,
    UISystemContext& context, renderer::ResourceManager& resources);

void DrawImageElement(const CanvasDrawContext& draw, GameObject& element, UIImage& image, const UIButton* button,
    const ui_runtime_detail::UITransform2D& transform, float groupOpacity, const asset::UIThemeStyle* theme);
void SubmitSolidQuad(const CanvasDrawContext& draw, const math::Vector2& position, const math::Vector2& size,
    const math::Vector4& color, float rotationZ = 0.0f);
void DrawTextElement(const CanvasDrawContext& draw, const UIText& text,
    const ui_runtime_detail::UITransform2D& transform, float groupOpacity, const asset::UIThemeStyle* theme);

} /// @note namespace ui_render_detail
} /// @note namespace fbzz::scene
