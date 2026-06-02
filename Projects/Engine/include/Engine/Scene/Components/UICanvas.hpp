// FBZZ Engine
// UICanvas.hpp | fbzz::scene
// ランタイム UI の座標空間設定コンポーネント
// ScreenSpace と WorldSpace の変換基準を Scene に持たせる。
// UISystem は sortOrder と renderMode を見て描画順を決める。
#pragma once
#include <Engine/Scene/Script.hpp>

namespace fbzz::scene {

enum class UIRenderMode {
    ScreenSpaceOverlay = 0, // カメラに依存せず、最終画面へ直接重ねる
    WorldSpace         = 1, // 3D 空間に配置し、深度の影響を受ける
    ScreenSpaceCamera  = 2, // カメラ基準の画面 UI。現状は Overlay と同じ座標系で描く

    // 既存コード・既存シーンとの互換性用エイリアス。
    ScreenSpace = ScreenSpaceOverlay
};

enum class UICanvasScaleMode {
    ConstantPixelSize,  // canvasWidth / canvasHeight をそのままピクセル座標として扱う
    ScaleWithScreenSize // viewport と reference 解像度の差を Canvas Scaler として吸収する
};

struct UICanvas {
    float        canvasWidth  = 1920.0f;
    float        canvasHeight = 1080.0f;
    int          sortOrder    = 0;
    UIRenderMode renderMode   = UIRenderMode::ScreenSpaceOverlay;
    UICanvasScaleMode scaleMode = UICanvasScaleMode::ConstantPixelSize;
    float        referenceWidth = 1920.0f;
    float        referenceHeight = 1080.0f;
    // ScaleWithScreenSize 時の幅/高さの重み。0=幅基準、1=高さ基準、0.5=両方の幾何平均
    float        matchWidthOrHeight = 0.0f;
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
        int scaler = static_cast<int>(scaleMode);
        r.Field("scaleMode", scaler);
        scaleMode = static_cast<UICanvasScaleMode>(scaler);
        r.Field("referenceWidth", referenceWidth);
        r.Field("referenceHeight", referenceHeight);
        r.Field("matchWidthOrHeight", matchWidthOrHeight);
        r.Field("worldScale",   worldScale);
    }
};

} // namespace fbzz::scene
