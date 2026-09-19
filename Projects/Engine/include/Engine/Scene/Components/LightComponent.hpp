/// @file    LightComponent.hpp
/// @brief   ライトの発光設定を持つコンポーネント (方向と位置は Transform から取る)
/// @author  Hasegawa Jin
/// @date    2025-06-12
#pragma once
#include <Engine/Scene/ParticleCurve.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>
#include <string>

namespace fbzz::scene {

struct LightComponent {
    enum class Type { Directional, Point, Spot, Area, Sphere, Tube };

    Type          type      = Type::Directional;
    math::Vector3 color     = { 1.0f, 1.0f, 1.0f };
    /// 色を色温度から作る。true のとき color は無視され、colorTemperature が正本になる。
    /// @note 温度で決めた色を Inspector の color 欄へ焼き戻すと温度を動かすたびに
    ///       オーサリング値が失われるため、どちらが正本かをフラグで分離する。
    bool          useColorTemperature = false;
    /// 色温度 [K]。1900=ろうそく, 2700=白熱電球, 4000=白色蛍光灯,
    /// 5500=昼光, 6500=D65, 7500=曇天, 10000=晴天の日陰。
    float         colorTemperature    = 6500.0f;
    /// intensity の単位 (Lighting.hlsli の LIGHT_UNIT_SCALE を参照)。
    /// @note タイプで意味が違い、同じ数値を入れ替えても同じ明るさにならない。Directional は
    ///       放射照度そのまま。Point/Spot/Sphere/Tube は 1m 地点の明るさで 1/d² 減衰
    ///       (LightAttenuation)。Area は面の輝度で形態係数のみが減衰を担い 1/d² は掛からず、
    ///       Point の 10〜30 に対し 100〜200 が目安になる (タイプを変えたら数値も入れ直す)。
    ///       既定 3.0 は空由来 IBL 環境光 (albedo×1.5 相当) を明確に上回る Directional 基準値。
    float         intensity = 3.0f;
    float         range     = 10.0f;    ///< Directional 以外。Area では打ち切り距離のみ
    float         innerCone = 15.0f;    ///< Spot のみ (degrees)
    float         outerCone = 30.0f;    ///< Spot のみ (degrees)
    bool          enabled   = true;

    /// @name 発光体の大きさ
    /// @{
    /// 点ではなく大きさを持つ光源として扱う半径 [m]。0 で厳密な点光源。
    ///   Sphere : この半径の球
    ///   Tube   : この半径 × sourceLength の長さを持つカプセル
    ///   Point / Spot : 影のにじみ幅とハイライトの広がりだけに効く (形状は点のまま)
    /// @note 現実の光源は必ず大きさを持ち、それが半影の幅とハイライトの大きさを決める。
    float         sourceRadius = 0.0f;
    /// Tube の長さ [m]。両端に半球が付くカプセルとして扱う。
    float         sourceLength = 1.0f;
    /// @}

    /// @name 面光源 (Type::Area)
    /// @{
    /// 面の向きは Transform::Forward()、面内の軸は Right() / Up() を使う。
    /// @note Inspector には「窓の幅 2m」のように全寸法で入れたい。半分にするのは
    ///       GPU へ渡す直前で行う。
    float         areaWidth  = 1.0f;    ///< [m]
    float         areaHeight = 1.0f;    ///< [m]
    /// 面の裏側を照らさない。板の裏に光が回り込むのを防ぐ。
    bool          areaTwoSided = false;
    /// @}

    /// @name 影
    /// @{
    /// Directional はカスケードシャドウ (CSM)、それ以外は専用アトラス (4x4 = 16 タイル)。
    /// @note タイル消費: Spot/Area は 1 枚 (Area は法線方向 75 度の錐台)、Point/Sphere/Tube は
    ///       キューブ 6 面で 6 枚 (上限は ShadowSettings::maxShadowedPointLights)。割り当ては
    ///       カメラに近い順で、あふれたライトは黙って影を落とさなくなる。sourceRadius は
    ///       深度ではなく半影の広さとして効く。
    bool  castShadows    = true;  ///< false のとき影を無効化 (shadowStrength=0 と等価)
    float shadowBias     = 1.0f;  ///< 基本バイアスへのスケール係数 (大きいほど Peter Panning が出やすい)
    float shadowStrength = 1.0f;  ///< 影の濃さ: 0=影なし, 1=完全な影
    float shadowDistance = 0.0f;  ///< Directional のみ: 0=シーンに自動フィット, >0=正射影の半幅 [m]
    /// Directional 以外の透視投影 near 面 [m]。
    /// @note near が小さいほど深度分解能が near 側へ寄り遠い側でアクネが出る。大きくすると
    ///       手前の caster が near で切り取られ影が抜ける。壁・天井埋め込みだと既定では
    ///       詰められないケースがあるため露出する。
    float shadowNearPlane = 0.1f;
    /// @}

    /// @name Cookie (投影テクスチャ)
    /// @{
    /// Spot のみ。ライトの円錐へ被せる白黒/カラーのマスクで、木漏れ日・窓枠・
    /// ロゴのゴボを作る。空文字で無効。
    /// @note Point / Directional は未対応 (前者はキューブマップ、後者はワールド空間の
    ///       タイリングという別の仕組みが要る)。
    std::string cookiePath;
    /// Cookie の見かけの回転 [degrees]。ライト自身を回すと影の向きまで変わってしまうため、
    /// 模様だけを回す軸を別に持つ。
    float cookieRotation = 0.0f;
    /// @}

    /// @name 明滅 (Flicker)
    /// @{
    /// たいまつのゆらぎ・破断面の放電・目の脈動。既定は Off で、既存シーンの絵は変わらない。
    /// @note 駆動は LightFlickerSystem。intensity を «オーサリング値 × 倍率» で毎フレーム
    ///       書き換え、Play を抜けるとオーサリング値へ戻す (保存へ焼き付かない)。
    /// @warning 同じ GameObject に VFXLightEnvelope を付けないこと。どちらも intensity を
    ///          «捕まえて掛け直す» 作りなので、片方が書いた値をもう片方が基準として掴む。
    enum class FlickerMode : std::uint8_t {
        Off = 0,   ///< 無効
        Sine,      ///< 正弦。呼吸するような滑らかな脈動
        Noise,     ///< value noise。たいまつ・不安定な蛍光灯
        Curve,     ///< flickerCurve を 1 周期として繰り返す。作り込んだ放電の形
    };
    FlickerMode flickerMode = FlickerMode::Off;
    /// 倍率の振れ幅。倍率 = 1 - flickerAmplitude * (1 - wave)、wave は [0, 1]。
    /// 0 で «常に 1 倍» = 無効、1 で消灯まで振れる。
    /// @note 上へ振ると白飛びの閾値を越えた瞬間から Bloom が別物になり、オーサリングした
    ///       intensity がピークを表さなくなるため、減る側だけを作る。
    float    flickerAmplitude = 0.0f;
    float    flickerFrequency = 6.0f;   ///< [Hz]。Curve では 1 周の速さ
    /// Sine / Curve の上へ混ぜるノイズの量 [0, 1]。1 で完全にノイズ。
    float    flickerNoise     = 0.0f;
    /// 位相オフセット [0, 1)。同じ設定のライトを並べたときに揃って光らないようずらす。
    float    flickerPhase     = 0.0f;
    /// ノイズ列の種。同じ seed と同じ位相なら、いつ何回走らせても同じ揺れになる。
    std::uint32_t flickerSeed = 0;
    /// Curve モードの波形。横軸は 1 周期の正規化時間、縦軸は倍率 [0, 1] 想定。
    ParticleCurve flickerCurve{
        {{ {0.0f, 1.0f}, {0.5f, 0.25f}, {1.0f, 1.0f}, {1.0f, 1.0f},
           {1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f} }},
        3, ParticleCurveInterpolation::Smooth };
    /// @}

    /// @name 明滅のランタイム状態 (シリアライズしない)
    /// @{
    /// @note 適用は intensity の «書き換え» なので元の値をどこかが覚えている必要がある。
    ///       System 側の別テーブルで持つと GameObject の破棄と寿命が合わず、使い回した
    ///       ID で前の値が蘇るためコンポーネント側に持つ。
    float flickerBaseIntensity = 0.0f;
    float flickerTime          = 0.0f;
    bool  flickerCaptured      = false;

    const char* GetTypeName() const { return "Light"; }
    void Reflect(IReflector& r)
    {
        int typeValue = static_cast<int>(type);
        r.Field("type", typeValue);
        if (typeValue < 0) typeValue = 0;
        if (typeValue > 5) typeValue = 5;
        type = static_cast<Type>(typeValue);
        r.Field("enabled", enabled);
        r.ColorField("color", color);
        r.Field("useColorTemperature", useColorTemperature);
        r.Field("colorTemperature",    colorTemperature);
        r.Field("intensity", intensity);
        r.Field("sourceRadius", sourceRadius);
        r.Field("sourceLength", sourceLength);
        r.Field("range", range);
        r.Field("innerCone", innerCone);
        r.Field("outerCone", outerCone);
        r.Field("areaWidth",    areaWidth);
        r.Field("areaHeight",   areaHeight);
        r.Field("areaTwoSided", areaTwoSided);
        r.Field("castShadows",    castShadows);
        r.Field("shadowBias",     shadowBias);
        r.Field("shadowStrength", shadowStrength);
        r.Field("shadowDistance", shadowDistance);
        r.Field("shadowNearPlane", shadowNearPlane);
        r.Field("cookiePath",     cookiePath);
        r.Field("cookieRotation", cookieRotation);
        {
            int mode = static_cast<int>(flickerMode);
            r.Field("flickerMode", mode);
            if (mode < 0) mode = 0;
            if (mode > 3) mode = 3;
            flickerMode = static_cast<FlickerMode>(mode);
        }
        r.Field("flickerAmplitude", flickerAmplitude);
        r.Field("flickerFrequency", flickerFrequency);
        r.Field("flickerNoise",     flickerNoise);
        r.Field("flickerPhase",     flickerPhase);
        {
            /// @note IReflector は uint32_t 非対応のため int 経由で往復させる。
            int seed = static_cast<int>(flickerSeed);
            r.Field("flickerSeed", seed);
            flickerSeed = static_cast<std::uint32_t>(seed < 0 ? 0 : seed);
        }
        r.Field("flickerCurve", flickerCurve);
    }
    /// Directional / Spot / Area の方向 → Transform::Forward()
    /// Point / Spot / Area の位置      → Transform::position
    /// Area の面内軸                   → Transform::Right() / Up()
    /// @}
};

} // namespace fbzz::scene
