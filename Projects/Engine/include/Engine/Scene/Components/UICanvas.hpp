/// @file    UICanvas.hpp
/// @brief   ランタイム UI の座標空間設定コンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-05-23
///
/// @note ScreenSpace と WorldSpace の変換基準を Scene に持たせる。
/// @note UISystem は sortOrder と renderMode を見て描画順を決める。
#pragma once
#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Reflection.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector4.hpp>

namespace fbzz::scene {

enum class UIRenderMode {
    ScreenSpaceOverlay = 0, ///< @note カメラに依存せず、最終画面へ直接重ねる
    WorldSpace         = 1, ///< @note 3D 空間に配置し、深度の影響を受ける
    ScreenSpaceCamera  = 2, ///< @note カメラ前方 planeDistance に配置し深度テストに参加する画面 UI

};

enum class UICanvasScaleMode {
    ConstantPixelSize,  ///< @note canvasWidth / canvasHeight をそのままピクセル座標として扱う
    ScaleWithScreenSize ///< @note viewport と reference 解像度の差を Canvas Scaler として吸収する
};

enum class UICanvasDepthMode {
    SCENE_DEPTH = 0,   ///< @note 不透明なシーンメッシュに遮蔽される。UI 自身は深度を書かない。
    ALWAYS_ON_TOP = 1 ///< @note 3D 空間の位置・大きさを保ったままメッシュの手前へ描く。
};

struct UICanvas {
    float        canvasWidth  = 1920.0f;
    float        canvasHeight = 1080.0f;
    int          sortOrder    = 0;
    UIRenderMode renderMode   = UIRenderMode::ScreenSpaceOverlay;
    UICanvasScaleMode scaleMode = UICanvasScaleMode::ConstantPixelSize;
    float        referenceWidth = 1920.0f;
    float        referenceHeight = 1080.0f;
    /// @note ScaleWithScreenSize 時の幅/高さの重み。0=幅基準、1=高さ基準、0.5=両方の幾何平均
    float        matchWidthOrHeight = 0.0f;
    /// @note WorldSpace / ScreenSpaceCamera 時の 1 キャンバスピクセルあたりのワールド単位
    float        worldScale    = 0.01f;
    /// @note ScreenSpaceCamera 専用: カメラ前方に Canvas を配置する距離 (ワールド単位)
    float        planeDistance = 2.0f;
    /// @note WorldSpace 専用: true のとき Canvas を常にカメラへ正対させる (ビルボード)。
    /// @note 敵の頭上に出す体力ゲージなど、3D 空間に置きつつ常に読めるようにする UI に使う。
    bool         faceCamera   = false;
    UICanvasDepthMode depthMode = UICanvasDepthMode::SCENE_DEPTH;
    /// @note WorldSpace / ScreenSpaceCamera の平面をローカル -Z (表示面の手前) へずらす距離 [m]。
    float        surfaceOffset = 0.0f;
    bool         enabled      = true;

    /// @note 表示を保証したい領域から外す余白 (左, 上, 右, 下 / Canvas ピクセル)。
    /// @note canvasWidth 自体を縮めないのは、アンカー 0..1 の意味が動き「別のレイアウト」に
    /// @note なるため。Canvas の広さはそのままに、子から見た親の矩形だけを内側へ寄せる。
    /// @note OS から自動取得しないのは、実測できるのはウィンドウ寸法までで、オーバースキャンや
    /// @note ウルトラワイドで避けたい量は確認できない環境の値を信じることになるため。
    math::Vector4 safeArea = { 0.0f, 0.0f, 0.0f, 0.0f };

    /// @note キーボード / ゲームパッドの方向入力でフォーカスを移動させる。
    /// @note 立てた Canvas でだけ働く (UIControls.hpp の UINavigation を参照)。
    bool         navigationEnabled = false;
    /// @note 倒しっぱなしのときの初回待ちと以降の間隔 (秒)。滑って飛ばないようにする。
    float        navigationRepeatDelay    = 0.4f;
    float        navigationRepeatInterval = 0.12f;

    /// @note ランタイム専用: いまフォーカスが乗っている要素。シーンへは保存しない。
    EntityRef    focusedObject{};

    /// @note 直近のフレームで UISystem が解決したマウス位置 (Canvas 空間、左上原点)。
    /// @note コンポーネントに置くのは、スクリーン → Canvas の変換が renderMode / Canvas Scaler /
    /// @note viewport 寸法で決まり、UISystem だけが全部を知るため (スクリプトが式を書き直すと
    /// @note viewport ≠ ウィンドウの場面で静かにずれる)。
    /// @note 実行時の値でシーンへは保存しない (Reflect に出さない)。UISystem が回る前は 0。
    math::Vector2 resolvedMousePosition = {};

    /// @note Empty or unresolved assignment preserves legacy component appearance. Shared across screens by GUID/path.
    DataAssetRef theme = { {}, "UIThemeAsset" };

    const char* GetTypeName() const { return "UICanvas"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",      enabled);
        r.Field("theme", theme);
        r.Tooltip("Shared game UI theme (.fzdata). Empty or missing uses each component's authored values.");
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

        r.Field("safeArea", safeArea);
        r.Tooltip("表示を保証したい領域から外す余白 (左, 上, 右, 下 / Canvas px)。"
                  "Canvas の広さは変えず、直下の子から見た親の矩形だけを内側へ寄せます");

        r.Group("World & Camera Space");
        r.FloatRange("worldScale", worldScale, 0.00001f, 1.0f);
        r.FloatRange("planeDistance", planeDistance, 0.01f, 10000.0f);
        r.Field("faceCamera",    faceCamera);
        static constexpr const char* kDepthModeLabels[] = { "Scene Depth", "Always On Top" };
        int depth = static_cast<int>(depthMode);
        r.Enum("depthMode", depth, kDepthModeLabels);
        depthMode = static_cast<UICanvasDepthMode>((depth < 0 || depth > 1) ? 0 : depth);
        r.Tooltip("World/Camera Space only. Scene Depth hides UI behind opaque meshes; UI never writes scene depth.");
        r.FloatRange("surfaceOffset", surfaceOffset, -1.0f, 1.0f);
        r.Tooltip("Metres toward the canvas front (-local Z). Use a small value for a display mounted on a mesh.");

        r.Group("Navigation");
        r.Field("navigationEnabled", navigationEnabled);
        r.Tooltip("方向キー / パッドでフォーカスを移動させます。"
                  "移動先には UI Navigation コンポーネントが要ります");
        r.FieldIf("navigationRepeatDelay", navigationRepeatDelay, navigationEnabled,
                  "倒しっぱなしで次へ進むまでの初回待ち (秒)");
        r.FieldIf("navigationRepeatInterval", navigationRepeatInterval, navigationEnabled,
                  "以降の移動間隔 (秒)");
    }
};

} /// @note namespace fbzz::scene
