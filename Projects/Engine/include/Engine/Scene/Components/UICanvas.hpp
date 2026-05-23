// FBZZ Engine
// UICanvas.hpp | fbzz::scene
// Runtime UI canvas coordinate space (Screen Space or World Space)
#pragma once
#include <Engine/Scene/Script.hpp>

namespace fbzz::scene {

enum class UIRenderMode {
    ScreenSpace, // Overlay on top of 3D, ignores depth
    WorldSpace   // Placed in 3D space, affected by depth
};

struct UICanvas {
    float        canvasWidth  = 1920.0f;
    float        canvasHeight = 1080.0f;
    int          sortOrder    = 0;
    UIRenderMode renderMode   = UIRenderMode::ScreenSpace;
    // WorldSpace: canvas pixel size in world units (smaller = physically smaller canvas)
    float        worldScale   = 0.01f;
    bool         enabled      = true;

    const char* GetTypeName() const { return "UICanvas"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",      enabled);
        r.Field("canvasWidth",  canvasWidth);
        r.Field("canvasHeight", canvasHeight);
        r.Field("sortOrder",    sortOrder);
        int mode = static_cast<int>(renderMode);
        r.Field("renderMode",   mode);
        renderMode = static_cast<UIRenderMode>(mode);
        r.Field("worldScale",   worldScale);
    }
};

} // namespace fbzz::scene
