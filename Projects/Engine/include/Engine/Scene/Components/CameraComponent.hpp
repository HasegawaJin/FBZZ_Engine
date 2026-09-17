/// @file    CameraComponent.hpp
/// @brief   カメラの投影・カリング・背景色。位置と回転は Transform が正本。
/// @author  Hasegawa Jin
/// @date    2026-05-21

#pragma once
#include <Physics/Layer.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Scene/CameraCullingSettings.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Vector4.hpp>

namespace fbzz::scene {

struct CameraComponent {
    float fovY    = 60.0f;
    /// @note RenderSystem の viewport aspect と同期。Script の座標変換 API が同じ投影行列を
    ///       再現できるよう Scene 側にも保持する。
    float aspectRatio = 16.0f / 9.0f;
    float nearZ   = 0.1f;
    float farZ    = 1000.0f;
    bool  isMain  = true;
    bool  enabled = true;
    fbzz::LayerMask cullingMask = fbzz::Layer::Everything;

    /// 何も描かれていない画素に残る色 (HDR ターゲットのクリア値)。
    /// 空を描くシーンでは空が上書きするため見えない。RGB は 1 を超えてよい。
    ///
    /// @note RenderSettings (プロジェクト共通) ではなく cullingMask と同じ粒度でカメラごとに持つ。
    math::Vector4 backgroundColor = renderer::kDefaultBackgroundColor;

    /// 描き始めにバッファをどう初期化するか。詳細は renderer::CameraClearMode。
    /// DepthOnly は「前に描かれた絵の上に重ねる」ためのモードで、重ねる相手が
    /// 居ないカメラに指定すると前フレームの絵が残る。
    renderer::CameraClearMode clearMode = renderer::CameraClearMode::SolidColor;

    /// @name カリング設定
    /// @{
    /// @note フラスタム / オクルージョンの可否は cullingMask と同じ粒度でカメラごとに持つ
    ///       (RenderSettings 共通だとゲームカメラとサブカメラで別判断ができない)。

    /// false にするとバウンディング球の視錐台テストを行わず、全オブジェクトを提出する。
    /// @note 消失の原因がカリングか別要因かを 1 クリックで切り分けるためのスイッチ。
    bool  frustumCulling = true;

    /// CPU ソフトウェアオクルージョンカリング (OcclusionCuller) の有無。
    /// @note 遮蔽者はメッシュ実体ではなくバウンディング球の近似なので保守的側に倒してある。
    ///       床タイル・壁パネルのような実体が薄いメッシュは過大申告になるため既定は false
    ///       (CameraCullingSettings の既定と揃えてある)。数値は Analysis パネルで確認できる。
    bool  occlusionCulling = false;

    /// 全バウンディング球へ加算するワールド単位の余白 [m]。
    /// @note スキンドメッシュの球はバインドポーズから作るため、大きく動くモーションでは
    ///       実シルエットが球からはみ出す。毎フレームのポーズ bounds 生成は高価なので余白で吸収する。
    float cullingBoundsPadding = 0.0f;

    /// 描画距離の上限 [m]。0 以下で無効 (farZ まで描く)。
    /// @note farZ を縮めると深度分解能の配分が変わり Z ファイティングや影品質まで動くため、
    ///       投影行列に触れずに描画距離だけを削るための値。
    float maxDrawDistance = 0.0f;

    /// レイヤーごとの描画距離上書き [m]。0 以下の要素は maxDrawDistance を使う。
    float layerCullDistances[kCullLayerCount] = {};

    /// 距離の測り方。true = カメラからの球距離、false = 視線方向の深度距離。
    bool  cullDistanceSpherical = true;

    /// バウンディング球の画面高さ比がこの値未満なら描かない。0 以下で無効。
    /// 単位は LODLevel::screenRelativeHeight と同じ (0.01 = 画面高さの 1%)。
    /// @note LODGroup は設定済みオブジェクトにしか効かない。これはシーン全体への一律の保険で、
    ///       LOD 未設定アセットが遠景で 1 ピクセル未満の DrawCall を出し続けるのを止める。
    float smallObjectScreenHeight = 0.0f;

    const char* GetTypeName() const { return "Camera"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("isMain", isMain);
        r.Field("fovY", fovY);
        r.Field("aspectRatio", aspectRatio);
        r.Field("nearZ", nearZ);
        r.Field("farZ", farZ);
        r.Field("cullingMask", reinterpret_cast<int&>(cullingMask));
        r.Field("frustumCulling", frustumCulling);
        r.Field("occlusionCulling", occlusionCulling);
        r.Field("cullingBoundsPadding", cullingBoundsPadding);
        r.Field("maxDrawDistance", maxDrawDistance);
        r.Field("cullDistanceSpherical", cullDistanceSpherical);
        r.Field("smallObjectScreenHeight", smallObjectScreenHeight);
        int clearModeValue = static_cast<int>(clearMode);
        static constexpr const char* CLEAR_MODES[] = { "Solid Color", "Depth Only" };
        r.Enum("clearMode", clearModeValue, CLEAR_MODES);
        clearMode = static_cast<renderer::CameraClearMode>(
            clearModeValue < 0 || clearModeValue > 1 ? 0 : clearModeValue);
        r.ColorField("backgroundColor", backgroundColor);
        /// @note layerCullDistances は固定長配列で IReflector に対応する Field が無い。
        ///       永続化は SceneSerializer が配列として書き出す。
    }
    /// @}
};

} // namespace fbzz::scene
