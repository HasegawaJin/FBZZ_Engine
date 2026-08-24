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
    ScreenSpaceCamera  = 2, // カメラ前方 planeDistance に配置し深度テストに参加する画面 UI

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
    // WorldSpace / ScreenSpaceCamera 時の 1 キャンバスピクセルあたりのワールド単位
    float        worldScale    = 0.01f;
    // ScreenSpaceCamera 専用: カメラ前方に Canvas を配置する距離 (ワールド単位)
    float        planeDistance = 2.0f;
    // WorldSpace 専用: true のとき Canvas を常にカメラへ正対させる (ビルボード)。
    // 敵の頭上に出す体力ゲージなど、3D 空間に置きつつ常に読めるようにする UI に使う。
    bool         faceCamera   = false;
    bool         enabled      = true;

    // 直近のフレームで UISystem が解決したマウス位置 (Canvas 空間、左上原点)。
    //
    // WHY コンポーネントに置くか: スクリーン → Canvas の変換は renderMode と
    //     Canvas Scaler と viewport 寸法で決まり、UISystem の中だけが全部を知っている。
    //     スクリプトが同じ式を書き直すと、Editor の Game ビューのように
    //     viewport ≠ ウィンドウの場面でだけ静かにずれる。
    // NOTE: 実行時の値でシーンへは保存しない (Reflect に出さない)。
    //       UISystem が回る前は 0。ドラッグ操作 (スライダー等) が読む。
    math::Vector2 resolvedMousePosition = {};

    const char* GetTypeName() const { return "UICanvas"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",      enabled);
        r.FloatRange("canvasWidth",  canvasWidth,  1.0f, 16384.0f);
        r.FloatRange("canvasHeight", canvasHeight, 1.0f, 16384.0f);
        r.Field("sortOrder",    sortOrder);
        static constexpr const char* kRenderModeLabels[] = {
            "Screen Space Overlay", "World Space", "Screen Space Camera"
        };
        int mode = static_cast<int>(renderMode);
        r.Enum("renderMode", mode, kRenderModeLabels);
        mode = (mode < 0 || mode > 2) ? 0 : mode;
        renderMode = static_cast<UIRenderMode>(mode);

        r.Group("Canvas Scaler");
        static constexpr const char* kScaleModeLabels[] = {
            "Constant Pixel Size", "Scale With Screen Size"
        };
        int scaler = static_cast<int>(scaleMode);
        r.Enum("scaleMode", scaler, kScaleModeLabels);
        scaler = (scaler < 0 || scaler > 1) ? 0 : scaler;
        scaleMode = static_cast<UICanvasScaleMode>(scaler);
        r.FloatRange("referenceWidth", referenceWidth, 1.0f, 16384.0f);
        r.FloatRange("referenceHeight", referenceHeight, 1.0f, 16384.0f);
        r.FloatRange("matchWidthOrHeight", matchWidthOrHeight, 0.0f, 1.0f);

        r.Group("World & Camera Space");
        r.FloatRange("worldScale", worldScale, 0.00001f, 1.0f);
        r.FloatRange("planeDistance", planeDistance, 0.01f, 10000.0f);
        r.Field("faceCamera",    faceCamera);
    }
};

} // namespace fbzz::scene
