/// @file    EnvironmentLightComponent.hpp
/// @brief   シーン全体の IBL (Image-Based Lighting) 設定を GameObject として管理するコンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-06-23
/// @note ProjectSettings の ibl はグローバル設定。RenderSystem は最初のアクティブな本コンポーネントを
/// @note 採用して上書きし、シーンごとに異なる環境光を使い分けられるようにする。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <cstdint>
#include <string>

namespace fbzz::scene {

/// @brief グローバル IBL の入力ソースを表す。
/// @note 既定 StaticDDS は後方互換。source キーを持たない既存シーンは従来通り .dds を読み込む。
enum class IblSource : uint8_t {
    StaticDDS  = 0,  ///< @note irradiancePath / prefilterPath の .dds をロード。
    DynamicSky = 1,  ///< @note 空キューブマップを実行時に焼いて畳み込む。
};

/// @note シーンの最初の有効な環境光を採用する。
struct EnvironmentLightComponent {
    bool        enabled       = true;
    /// @note DynamicSky は RenderSystem の空連動ベイク経路を使う。
    IblSource   source        = IblSource::StaticDDS;
    std::string irradiancePath;           ///< @note 拡散 IBL cube、StaticDDS のみ。
    std::string prefilterPath;            ///< @note 鏡面畳み込み IBL cube、StaticDDS のみ。
    float       intensity     = 1.0f;
    float       diffuseScale  = 1.0f;
    float       specularScale = 1.0f;
    int         maxMipLevel   = 4;        ///< @note prefilter の最大 mip インデックス。
    /// @note Ray transport の未畳み込み線形 HDR DDS cube。irradiance/prefilter を代用しない。
    std::string rawEnvironmentPath;
    float rotationY = 0.0f;               ///< @note Raw cube の右手系 Y 回転 [deg]。

    const char* GetTypeName() const { return "EnvironmentLight"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled",        enabled);

        /// @note enum は IReflector が直接扱わないため int 経由で反映する。
        /// @note 範囲外値は StaticDDS にクランプして後方互換を保つ。
        static constexpr const char* kSourceLabels[] = { "StaticDDS", "DynamicSky" };
        int sourceValue = static_cast<int>(source);
        r.Enum("source", sourceValue, kSourceLabels);
        sourceValue = (sourceValue < 0 || sourceValue > 1) ? 0 : sourceValue;
        source = static_cast<IblSource>(sourceValue);

        r.Field("irradiancePath", irradiancePath);
        r.Field("prefilterPath",  prefilterPath);
        r.Field("intensity",      intensity);
        r.Field("diffuseScale",   diffuseScale);
        r.Field("specularScale",  specularScale);
        r.Field("maxMipLevel",    maxMipLevel);
        r.Field("rawEnvironmentPath", rawEnvironmentPath);
        r.Field("rotationY", rotationY);
    }
};

} /// @note namespace fbzz::scene
