/// @file    RenderEnvironment.hpp
/// @brief   解決済みの空・雲・水中・集光の描画入力。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Renderer/Resources/ResourceHandle.hpp>
#include <Graphics/RayTracing/Scene/RayEnvironment.hpp>
#include <Math/LinearAlgebra/Vector2.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <Math/LinearAlgebra/Vector4.hpp>
#include <optional>
namespace fbzz::renderer {
struct RenderSkyInput {
    math::Vector3 rayleighScattering{}; ///< 大気モデルへ渡す RGB レイリー散乱係数。
    float mieScattering = 0.0f; ///< 大気モデルへ渡すミー散乱係数。
    float planetRadius = 0.0f; ///< 地平線を決める惑星の半径 [km]。
    float atmosphereRadius = 0.0f; ///< 惑星中心から大気層外端までの半径 [km]。
    float skyScatterIntensity = 0.0f; ///< 空を描く際の大気散乱の強度倍率。
    float mieG = 0.0f; ///< ミー散乱の位相関数に渡す異方性係数。
};
struct RenderSunMoonInput {
    bool sunEnabled = false; ///< 空の太陽円盤を描くかどうか。
    float sunDiskIntensity = 0.0f; ///< 太陽円盤の輝度倍率。
    bool moonEnabled = false; ///< 空の月を描くかどうか。
    float moonSize = 0.0f; ///< 月の見かけの面積倍率。角半径には平方根を使う。
    float moonBrightness = 0.0f; ///< 月の輝度倍率。
    math::Vector3 moonColor{}; ///< 月の描画に使う RGB の色。
    float starBrightness = 0.0f; ///< 星の輝度倍率。
};
/// @note CloudVolume.hlsli の VolumetricCloudConstants と同じ配列順。
struct RenderCloudConstants {
    math::Vector4 cloudLayer; ///< 雲層の下端・上端のワールド Y [m]、密度、被覆率。
    math::Vector4 cloudNoise; ///< 主ノイズの座標倍率 [m⁻¹]、詳細倍率、経過時間 [s]、追跡距離上限 [m]。
    math::Vector4 cloudWind; ///< 風向 X、風速、風向 Z、視線方向の積分ステップ数。
    math::Vector4 cloudLighting; ///< 光の吸収、環境光の強度、逆光の縁の強度、光芒の強度。
    math::Vector4 cloudAlbedo; ///< xyz は雲の RGB 反射色、w は環境光の高さ方向勾配の強さ。
    math::Vector4 cloudWeather; ///< 気象ノイズの座標倍率 [m⁻¹]、気象の寄与、細部の寄与、時間変化の速さ。
    math::Vector4 cloudShading; ///< 消散係数、太陽の強度、粉末散乱の強度、多重散乱の強度。
    math::Vector4 cloudProfile; ///< 雲層上下端のぼかし幅、散乱の異方性、光源方向の積分ステップ数。
    math::Vector4 cloudRange; ///< 描画開始距離とフェード距離 [m]、地平線で減衰させる強さ、予約枠。
    math::Vector4 cloudSunTint; ///< xyz は雲へ届く日光の RGB 色補正、w は予約枠。
    math::Vector4 cloudAmbTint; ///< xyz は雲へ届く環境光の RGB 色補正、w は予約枠。
};
static_assert(sizeof(RenderCloudConstants) == 176);
struct RenderUnderwaterInput {
    bool enabled = false; ///< 当該ビューに適用する水中効果が見つかったかどうか。
    float strength = 0.0f; ///< 水面を境に水中効果を出し入れする補間率。
    float depth = 0.0f; ///< カメラ位置で測った水面からカメラまでの鉛直距離 [m]。
    float fogDensity = 0.0f; ///< 簡易水中霧に使う密度。
    math::Vector3 color{}; ///< 水深から補間した簡易水中霧の RGB 色。
    bool opticalEnabled = false; ///< 吸収と散乱を使う水中照明を評価するかどうか。
    math::Vector3 absorption{}; ///< 水中の RGB 吸収係数 [m⁻¹]。
    math::Vector3 scattering{}; ///< 水中の RGB 散乱係数 [m⁻¹]。
    float phaseG = 0.0f; ///< 水中散乱の位相関数に渡す異方性係数。
    float surfaceY = 0.0f; ///< カメラ位置で採取した水面のワールド Y [m]。
    float bottomY = 0.0f; ///< カメラ位置で採取した水域下端のワールド Y [m]。
    float maxDistance = 120.0f; ///< 視線方向に水中光を積分する最大距離 [m]。
    float lightStrength = 1.0f; ///< 水中散乱の照明強度倍率。
    float waveAmplitude = 0.0f; ///< 水中照明に渡す代表波の振幅 [m]。
    float waveFrequency = 0.12f; ///< 水中照明に渡す代表波の空間角周波数 [rad/m]。
    float waveSpeed = 1.0f; ///< 水中照明に渡す代表波の時間変化の速さ。
    float distortion = 0.001f; ///< 水中で画面を歪ませる強さ。
    math::Vector2 center{}; ///< 水域中心のワールド XZ [m]。
    math::Vector2 halfExtent{}; ///< 矩形水域の中心から向かい合う辺までの各軸距離 [m]。
    float cosYaw = 1.0f; ///< ワールド XZ を矩形の軸に合わせる水面姿勢の余弦。
    float sinYaw = 0.0f; ///< ワールド XZ を矩形の軸に合わせる水面姿勢の正弦。
    int lightSteps = 48; ///< 水中散乱を積分するステップ数。
};
struct RenderCausticsInput {
    bool enabled = false; ///< このビューでコースティクスを投影する水面があるかどうか。
    float maxDistance = 120.0f; ///< 水中光が有効な距離の上限 [m]。
    float intensity = 0.0f; ///< 投影するコースティクス照明の強度倍率。
    float tiling = 1.0f; ///< ワールド座標から投影模様の繰り返しへ変換する倍率。
    float surfaceY = 0.0f; ///< コースティクスを投影する基準水面のワールド Y [m]。
    float timeOffset = 0.0f; ///< 描画時刻に材質の速度を掛けた模様の時間オフセット。
    float centerX = 0.0f; ///< 投影する水域の中心のワールド X [m]。
    float centerZ = 0.0f; ///< 投影する水域の中心のワールド Z [m]。
    float halfExtentX = 0.0f; ///< 矩形水域の中心から X 辺までの距離 [m]。遠景水域は 0。
    float halfExtentZ = 0.0f; ///< 矩形水域の中心から Z 辺までの距離 [m]。遠景水域は 0。
    float waveAmp = 0.0f; ///< 投影に使う代表波の振幅 [m]。
    float waveFreq = 0.12f; ///< 投影に使う代表波の空間角周波数 [rad/m]。
    float waveSpeed = 1.0f; ///< 投影に使う代表波の時間変化の速さ。
    bool opticalEnabled = false; ///< 吸収と散乱による水中での光の変化を適用するかどうか。
    math::Vector3 absorption{}; ///< 水中の RGB 吸収係数 [m⁻¹]。
    math::Vector3 scattering{}; ///< 水中の RGB 散乱係数 [m⁻¹]。
    float phaseG = 0.0f; ///< 水中散乱の位相関数に渡す異方性係数。
    float cosYaw = 1.0f; ///< ワールド XZ を投影矩形の軸に合わせる水面姿勢の余弦。
    float sinYaw = 0.0f; ///< ワールド XZ を投影矩形の軸に合わせる水面姿勢の正弦。
    ResourceHandle<TextureTag> texture; ///< コースティクスの明暗模様を読む非所有のテクスチャハンドル。
};
struct RenderEnvironmentInput {
    RayEnvironmentInput rayEnvironment; ///< レイが場面外へ抜けたときに参照する環境光の入力。
    std::optional<RenderSkyInput> sky; ///< 取得できた大気と空の描画設定。
    std::optional<RenderSunMoonInput> sunMoon; ///< 取得できた太陽・月・星の描画設定。
    RenderCloudConstants cloud{}; ///< 体積雲の積分シェーダーへ渡す雲層と光の定数。
    bool cloudEnabled = false; ///< 体積雲を描く入力があるかどうか。
    bool cloudHalfResolution = false; ///< 雲を半解像度で積分してから拡大するかどうか。
    bool cameraUnderwater = false; ///< このビューのカメラが水面の下にあるかどうか。
    RenderUnderwaterInput underwater; ///< カメラの当該水域から解決した水中効果。
    RenderCausticsInput caustics; ///< このビューの水面から解決したコースティクス投影設定。
};
} // namespace fbzz::renderer
