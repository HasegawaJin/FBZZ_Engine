/// @file    PresentationGeometry.hpp
/// @brief   Sprite と Line の描画用頂点列と正確な入力 snapshot を生成する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <span>
#include <vector>

namespace fbzz::scene {
/// @brief Non-owning Line settings and effective point sequence consumed by geometry generation.
/// @note points use the coordinate system named by space and never alias generated GPU geometry.
struct LineGeometryView {
    const LineRendererComponent& settings;
    std::span<const math::Vector3> points;
    LineSpace space = LineSpace::Local;
};

/// @note 点列の順序、色、空間、断面と頂点生成に必要な world TRS / camera の全成分を保持する。hash の一致を変更なしの根拠にしない。
FBZZ_ENGINE_API void CaptureLineGeometryInputs(const LineRendererComponent& line, const Transform& transform,
    const math::Vector3* cameraWorldPosition, std::vector<float>& output);
/// @brief Capture exact mesh inputs from an adapter-resolved point sequence.
FBZZ_ENGINE_API void CaptureLineGeometryInputs(const LineGeometryView& line, const Transform& transform,
    const math::Vector3* cameraWorldPosition, std::vector<float>& output);
FBZZ_ENGINE_API void CaptureSpriteGeometryInputs(const SpriteRendererComponent& sprite, const asset::ResolvedSprite& resolved,
    std::vector<float>& output);
/// @return 非有限入力、負の幅、world 空間の特異な姿勢では false と空の頂点列。重複した区間は描画しない。
/// @note 色と幅は全経路の距離比で補間する。World は座標と幅とも world の m、Local は local の m。
FBZZ_ENGINE_API bool BuildLineGeometry(const LineRendererComponent& line, const Transform& transform,
    const math::Vector3* cameraWorldPosition, std::vector<renderer::Vertex>& vertices, std::vector<uint32_t>& indices);
/// @brief Build a Line mesh from an adapter-resolved point sequence without mutating authoring data.
FBZZ_ENGINE_API bool BuildLineGeometry(const LineGeometryView& line, const Transform& transform,
    const math::Vector3* cameraWorldPosition, std::vector<renderer::Vertex>& vertices, std::vector<uint32_t>& indices);
/// @note Sliced の border は元画像の pixel / pixelsPerUnit による local m。狭い出力では両 border を同比率で縮める。
/// @see Docs/design/sprite-reference.md
FBZZ_ENGINE_API bool BuildSpriteGeometry(const SpriteRendererComponent& sprite, const asset::ResolvedSprite& resolved,
    std::vector<renderer::Vertex>& vertices, std::vector<uint32_t>& indices);
} /// @note namespace fbzz::scene
