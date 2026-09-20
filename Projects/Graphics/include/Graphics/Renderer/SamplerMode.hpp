/// @file    SamplerMode.hpp
/// @brief   テクスチャサンプラーのプリセット一覧。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note Renderer バックエンドが共通名からネイティブサンプラー状態を選ぶ。
/// @note 描画処理やポストプロセスの呼び出し側は enum だけを指定する。
#pragma once

namespace fbzz::renderer
{

enum class SamplerMode
{
    /// @name Wrap (繰り返し)
    /// @{
    WRAP_ANISOTROPIC,   ///< @note 0  メッシュテクスチャ標準。アニソトロピック x16
    WRAP_TRILINEAR,     ///< @note 1  三線形補間。ミップマップあり
    WRAP_BILINEAR,      ///< @note 2  双線形補間。ミップマップなし
    WRAP_POINT,         ///< @note 3  最近傍。ピクセルアート・デバッグ用
    /// @}

    /// @name Clamp (端でクランプ)
    /// @{
    CLAMP_ANISOTROPIC,  ///< @note 4  スカイボックス・キューブマップ等
    CLAMP_LINEAR,       ///< @note 5  UI・レンダーターゲット参照・ポストプロセス
    CLAMP_POINT,        ///< @note 6  G バッファ参照・ディファードシェーディング
    /// @}

    /// @name 境界色 0
    /// @{
    BORDER_ZERO,        ///< @note 7  シャドウマップ PCF 用
    /// @}

    /// @name 低負荷アニソ
    /// @{
    WRAP_ANISOTROPIC_4X, ///< @note 8  地形ディフューズ用。x4 アニソ（x16 の約 1/3 コスト）

    COUNT
    /// @}
};

} /// @note namespace fbzz::renderer
