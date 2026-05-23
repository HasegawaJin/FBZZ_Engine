// FBZZ Engine
// UIImage.hpp | fbzz::scene
// Runtime UI sprite rectangle with optional Anchor/Pivot layout
#pragma once
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector4.hpp>
#include <Engine/Scene/Script.hpp>
#include <string>

namespace fbzz::scene {

struct UIImage {
    // --- Position/size (used when useAnchor == false) ---
    math::Vector2 position = { 0.0f, 0.0f };
    math::Vector2 size     = { 100.0f, 100.0f };

    // --- Anchor/Pivot layout (used when useAnchor == true) ---
    // anchorMin/anchorMax: normalized 0..1 relative to parent canvas.
    // anchoredPosition: offset of the pivot from the anchor center (px).
    // pivot: normalized point within the rect (0.5,0.5 = center).
    // sizeDelta: added to the stretch from anchorMin to anchorMax.
    bool          useAnchor       = false;
    math::Vector2 anchorMin       = { 0.5f, 0.5f };
    math::Vector2 anchorMax       = { 0.5f, 0.5f };
    math::Vector2 pivot           = { 0.5f, 0.5f };
    math::Vector2 anchoredPosition = { 0.0f, 0.0f };
    math::Vector2 sizeDelta        = { 100.0f, 100.0f };

    // --- Appearance ---
    std::string   texturePath = ""; // runtime texture path; loaded by UISystem each frame if non-empty
    renderer::ResourceHandle<renderer::TextureTag> texture = {};
    math::Vector4 color   = { 1.0f, 1.0f, 1.0f, 1.0f };
    math::Vector2 uvMin   = { 0.0f, 0.0f };
    math::Vector2 uvMax   = { 1.0f, 1.0f };
    bool          enabled = true;

    const char* GetTypeName() const { return "UIImage"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",          enabled);
        r.Field("position",         position);
        r.Field("size",             size);
        r.Field("useAnchor",        useAnchor);
        r.Field("anchorMin",        anchorMin);
        r.Field("anchorMax",        anchorMax);
        r.Field("pivot",            pivot);
        r.Field("anchoredPosition", anchoredPosition);
        r.Field("sizeDelta",        sizeDelta);
        r.Field("texturePath",      texturePath);
        r.Field("color",            color);
        r.Field("uvMin",            uvMin);
        r.Field("uvMax",            uvMax);
    }
};

} // namespace fbzz::scene
