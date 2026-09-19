/// @file    CameraCullingSettings.hpp
/// @brief   CameraComponent のカリング設定を、値だけ抜き出した受け渡し用 POD。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// @note RenderSystem は Renderer 層寄りで CameraComponent (Script/Layer 依存) を知らないが、カメラ選択にはその両方の情報が要るため値だけをここへ切り出す。
/// @note SceneUtils.hpp (Window/ProjectSettings/physics::World を引く重いヘッダ) を RenderSystem.hpp へ include すると、Windows マクロ汚染が RenderSystem を include する全モジュールへ波及する。
#pragma once

namespace fbzz::scene {

/// @brief レイヤー数。`fbzz::Layer` と同じ 0-31 の 32 本。
/// @note Physics/Layer.hpp への依存を避けるためここで独立に持つ。値がずれると層ごとの距離が 1 つずつずれるが、静的検査は SceneUtils.cpp 側で行う。
inline constexpr int kCullLayerCount = 32;

struct CameraCullingSettings {
    bool  frustumCulling       = true; ///< バウンディング球の視錐台テストを行うか。false ならフラスタムカリングをしない。

    /// @brief CPU ソフトウェアオクルージョンカリングを行うか。
    /// @note 既定 false: 遮蔽者はバウンディング球の内接正方形として焼かれるため、床タイル・壁パネルのような「球に対して実体が薄い」メッシュが手前に不透明面があると誤って主張する。ゲームカメラは CameraComponent で明示的に有効化する前提とし、編集ビューでは「見えているものが見える」ことを優先する。Terrain は GBuffer パスより後に描かれ遮蔽者に登録されないため効きも薄い。
    bool  occlusionCulling     = false;

    float cullingBoundsPadding = 0.0f; ///< 全バウンディング球へ加算するワールド単位の余白 [m]。負値は呼び出し側で 0 に丸める。

    /// @name 距離カリング
    /// @{
    float maxDrawDistance = 0.0f; ///< カメラから見えなくする距離 [m]。0 以下で無効 (farZ まで描く)。

    /// @brief レイヤーごとの距離上書き [m]。0 以下の要素は maxDrawDistance にフォールバックする。
    /// @note 「小物は 30m で消したいが建物は 500m まで残したい」という距離カリングの実用形をレイヤー粒度で表す (オブジェクト個別設定は現実的でない)。Unity の Camera.layerCullDistances と同じ考え方。
    float layerCullDistances[kCullLayerCount] = {};

    /// @brief true = カメラ位置からの球距離、false = 視線方向の深度距離。
    /// @note 球距離は視界を左右に振っても消える距離が変わらない代わりに画面端のオブジェクトが中央より手前で消える。どちらが自然かはゲームの見せ方次第。
    bool  cullDistanceSpherical = true;
    /// @}

    /// @name 極小オブジェクトカリング
    /// @{
    float smallObjectScreenHeight = 0.0f; ///< 画面高さに対するバウンディング球の相対高さがこの値未満なら描かない。0 以下で無効。単位は LODLevel::screenRelativeHeight と同じ (0.01 = 画面高さの 1%)。
    /// @}
};

} // namespace fbzz::scene
