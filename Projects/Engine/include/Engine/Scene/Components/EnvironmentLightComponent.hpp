/// @file    EnvironmentLightComponent.hpp
/// @brief   シーン全体の IBL (Image-Based Lighting) 設定を GameObject として管理するコンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-06-23
///
/// @note ProjectSettings の ibl はグローバル設定。RenderSystem は最初のアクティブな本コンポーネントを
///       採用して上書きし、シーンごとに異なる環境光を使い分けられるようにする。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <cstdint>
#include <string>

namespace fbzz::scene {

/// @brief グローバル IBL の入力ソースを表す。
/// @note 既定 StaticDDS は後方互換。source キーを持たない既存シーンは従来通り .dds を読み込む。
enum class IblSource : uint8_t {
    StaticDDS  = 0,  ///< irradiancePath / prefilterPath の .dds をロード (既定・後方互換)
    DynamicSky = 1,  ///< 空キューブマップを実行時に焼いて畳み込む (空連動 IBL)
};

/// EnvironmentLightComponent — シーン単位のグローバル IBL 設定コンポーネント。
/// Unreal の SkyLight に相当。シーン内に1つ配置する。
struct EnvironmentLightComponent {
    bool        enabled       = true;
    /// IBL の入力ソース。既定は後方互換の StaticDDS (.dds ロード)。
    /// DynamicSky を選ぶと RenderSystem が空連動ベイク経路 (SkyCapture / SkyLightBake) を使う。
    IblSource   source        = IblSource::StaticDDS;
    std::string irradiancePath;           ///< 拡散 IBL irradiance cubemap (.dds) — source=StaticDDS 時のみ使用
    std::string prefilterPath;            ///< 鏡面 IBL prefiltered cubemap (.dds) — source=StaticDDS 時のみ使用
    float       intensity     = 1.0f;     ///< 環境光全体スケール
    float       diffuseScale  = 1.0f;     ///< 拡散成分スケール
    float       specularScale = 1.0f;     ///< 鏡面成分スケール
    int         maxMipLevel   = 4;        ///< prefilter の最大 mip インデックス (bake mip 数 - 1)

    const char* GetTypeName() const { return "EnvironmentLight"; }

    void Reflect(IReflector& r)
    {
        r.Field("enabled",        enabled);

        /// @note enum は IReflector が直接扱わないため int 経由で反映する。
        ///       Enum() は Inspector ではドロップダウン、シリアライザでは int フィールドへフォールバックする。
        ///       範囲外値 (壊れたデータ・将来の追加値) は StaticDDS にクランプして後方互換と耐性を確保する。
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
    }
};

} // namespace fbzz::scene
