/// @file    SkyRenderer.hpp
/// @brief   大気散乱スカイドーム設定コンポーネント。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// 太陽方向・散乱係数など、空描画に必要な値を Scene に保持する。
/// 描画順やシェーダー実体は RenderSystem / Renderer が扱う。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

struct SkyRenderer {
    /// @brief Rayleigh 散乱係数 [m⁻¹]。波長ごとに異なり、空が青い理由となる。
    /// @note 既定値は地球の標準大気に基づく近似値。
    math::Vector3 rayleighScattering = { 5.8e-3f, 13.5e-3f, 33.1e-3f };
    float         mieScattering      = 21.0e-4f;
    /// @note 大気散乱そのものの明るさ。太陽ディスクの明るさは SunMoonRenderer::sunDiskIntensity が別に持つ。
    float         skyScatterIntensity = 20.0f;
    /// @note シェーダーは km 単位の半径を使う。地球以外のスケールでも空を調整できるよう Scene に保持する。
    float         planetRadius       = 6371.0f;
    float         atmosphereRadius   = 6471.0f;
    /// @brief Henyey-Greenstein 位相関数の非対称パラメーター (0=等方散乱, 1=完全前方散乱)。
    /// @note 既定 0.76 は大気中のエアロゾルに典型的な値で、太陽周辺のグローを再現する。
    float         mieG               = 0.76f;
    bool          enabled            = true;

    /// @name 昼夜カーブ
    /// @{
    /// @brief 太陽の向きは DirectionalLight の transform が唯一のソースで、dayNightEnabled が
    ///        有効な間はその太陽高度 [度] から夜/夕方/昼の3キーで色・強度だけを補間する。
    ///        高度0°(地平線)が夕方のキーなので、ライトを水平に向ければ夕方になる。
    /// @note ON の間 DirectionalLight の color/intensity はここが上書きする (向きは上書きしない)。
    ///       太陽/月ディスクの描画設定は本構造体でなく SunMoonRenderer が持つ。
    bool          dayNightEnabled = false;

    /// @brief 夕方のキー (高度 0°) から昼・夜へ抜けきるまでの角度 [度]。
    /// @note 昼側と夜側で別々に持つ理由: golden hour は地平線上わずか数度で終わるが、薄明は
    ///       地平線下十数度まで続く。対称な1本の帯だと夕焼けか薄明のどちらかが必ず潰れる。
    float         dayAltitude   = 12.0f; ///< この高度以上で完全な昼
    float         nightAltitude = 8.0f;  ///< 地平線からこの角度だけ沈むと完全な夜
    /// @}

    math::Vector3 dayColor    = { 1.0f, 0.98f, 0.95f }; ///< 日中の太陽光色
    math::Vector3 sunsetColor = { 1.0f, 0.5f, 0.2f };   ///< 日の出/日没の暖色 (高度 0° でこの色ちょうど)
    math::Vector3 nightColor  = { 0.1f, 0.15f, 0.3f };  ///< 夜の薄明 (月光相当)

    /// @brief 地表ライティングの強さ。1.0 で「白い拡散面が albedo そのままの明るさ」
    ///        (Lighting.hlsli の LIGHT_UNIT_SCALE)。
    /// @note 日中の既定 4.0 の根拠: 空の輝度は skyScatterIntensity (既定20) から SkyCapture →
    ///       irradiance キューブマップ (DIFFUSE_RADIANCE_LIMIT=4 でクランプ) を経て環境光になる。
    ///       低すぎると太陽の直接光がこの環境光に埋もれる。
    float         dayIntensity    = 4.0f;
    float         sunsetIntensity = 2.0f;
    float         nightIntensity  = 0.3f;

    /// @name 空の見た目の明るさ (地表ライティングとは独立)
    /// @{
    /// @note ground lighting と別軸に分離し、LightConstants.skyDimmer 経由で Skydome / SunMoon /
    ///       VolumetricCloud / VolumetricLight / エアリアルパースへ渡す。
    float         skyDayBrightness    = 1.5f;
    float         skySunsetBrightness = 1.0f;
    float         skyNightBrightness  = 0.1f;
    /// @}

    /// @name 雲シャドウ (Phase C: CloudShadow)
    /// @{
    /// @note 雲密度を地表へ投影した「まだら影」を全 Lit シェーダーの影係数に乗算する。専用
    ///       CloudShadowMap を焼かず、ワールド XZ で手続き型 FBM を評価する解析版 (低コスト・
    ///       パス追加不要)。視認上の空の雲とは別系統で地表の光のゆらぎを演出する。
    float         cloudShadowStrength = 0.0f;   ///< 0=無効。0.5〜0.8 で自然なまだら影
    float         cloudShadowCoverage = 0.5f;   ///< 雲量 (大きいほど影が広い) [0,1]
    /// @brief 影のまだら1周期の大きさ [m]。シェーダーへ渡すときに逆数へ変換する。
    /// @note world→ノイズ UV スケールを直接公開すると 0.02 のような単位不明な値になるため。
    float         cloudShadowSize     = 50.0f;
    float         cloudShadowSpeed    = 1.0f;   ///< 流れる速さ
    /// @}

    const char* GetTypeName() const { return "Sky Renderer"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("rayleighScattering", rayleighScattering);
        r.Field("mieScattering", mieScattering);
        r.Field("skyScatterIntensity", skyScatterIntensity);
        r.Field("planetRadius", planetRadius);
        r.Field("atmosphereRadius", atmosphereRadius);
        r.Field("mieG", mieG);
        r.Field("dayNightEnabled", dayNightEnabled);
        r.Field("dayAltitude", dayAltitude);
        r.Field("nightAltitude", nightAltitude);
        r.ColorField("dayColor", dayColor);
        r.ColorField("sunsetColor", sunsetColor);
        r.ColorField("nightColor", nightColor);
        r.Field("dayIntensity", dayIntensity);
        r.Field("sunsetIntensity", sunsetIntensity);
        r.Field("nightIntensity", nightIntensity);
        r.Field("skyDayBrightness", skyDayBrightness);
        r.Field("skySunsetBrightness", skySunsetBrightness);
        r.Field("skyNightBrightness", skyNightBrightness);
        r.Field("cloudShadowStrength", cloudShadowStrength);
        r.Field("cloudShadowCoverage", cloudShadowCoverage);
        r.Field("cloudShadowSize", cloudShadowSize);
        r.Field("cloudShadowSpeed", cloudShadowSpeed);
        if (cloudShadowSize < 1.0f) cloudShadowSize = 1.0f;
        /// @note 0 度幅は補間が 0 除算になるため下限を持つ。
        if (dayAltitude   < 0.1f) dayAltitude   = 0.1f;
        if (nightAltitude < 0.1f) nightAltitude = 0.1f;
    }
};

} // namespace fbzz::scene
