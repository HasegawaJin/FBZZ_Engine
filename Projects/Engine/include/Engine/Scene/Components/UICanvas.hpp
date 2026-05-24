// FBZZ Engine
// UICanvas.hpp | fbzz::scene
// ランタイム UI の座標空間設定コンポーネント
// ScreenSpace と WorldSpace の変換基準を Scene に持たせる。
// UISystem は sortOrder と renderMode を見て描画順を決める。
#pragma once
#include <Engine/Scene/Script.hpp>

namespace fbzz::scene {

enum class UIRenderMode {
    ScreenSpace, // 3D 描画の上に重ね、深度を無視する
    WorldSpace   // 3D 空間に配置し、深度の影響を受ける
};

struct UICanvas {
    float        canvasWidth  = 1920.0f;
    float        canvasHeight = 1080.0f;
    int          sortOrder    = 0;
    UIRenderMode renderMode   = UIRenderMode::ScreenSpace;
    // WorldSpace 時の 1 キャンバスピクセルあたりのワールド単位。小さいほど物理サイズも小さい
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
