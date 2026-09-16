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
    // 色を色温度から作る。true のとき color は無視され、colorTemperature が正本になる。
    // WHY color を上書きせず別フラグにするか: 温度で決めた色を Inspector の color 欄へ
    //     焼き戻すと、温度を動かすたびにオーサリング値が失われる。どちらが正本かを
    //     フラグで持てば、温度モードを切っても元の色がそのまま戻る。
    bool          useColorTemperature = false;
    // 色温度 [K]。1900=ろうそく, 2700=白熱電球, 4000=白色蛍光灯,
    // 5500=昼光, 6500=D65, 7500=曇天, 10000=晴天の日陰。
    float         colorTemperature    = 6500.0f;
    // intensity の単位 (Lighting.hlsli の LIGHT_UNIT_SCALE を参照)。
    //
    // ⚠ タイプによって意味が違う。同じ数値を入れ替えても同じ明るさにはならない。
    //
    //   Directional          … そのまま放射照度。1.0 で白い拡散面が albedo の明るさ。
    //   Point / Spot / Sphere / Tube
    //                        … 「1m 地点での明るさ」。シェーダーが 1/d^2 で減衰する
    //                          (LightAttenuation)。10m 先の寄与は 1/100 になる。
    //   Area (Rect)          … 面の**輝度 (radiance)**。減衰は形態係数
    //                          (コサイン重み付き立体角 / 2π) が担い、1/d^2 は掛からない。
    //                          面が半球を埋め尽くすとき albedo × intensity になる。
    //
    // WHY Area だけ桁が変わるか: 小さなパネルが遠くの点へ張る立体角は極めて小さい。
    //     1.6 x 1.2m のパネルを 9m 先から見た形態係数は 0.0038 しかないため、
    //     Point と同じ感覚で 10 を入れると albedo × 0.04 = ほぼ見えない。
    //     天井照明として成立させるには 100〜200 が要る。逆に Point/Spot に 140 を
    //     入れると近傍が完全に白飛びする。**タイプを変えたら必ず数値も入れ直すこと。**
    //
    //   既定 3.0 は、空由来の IBL 環境光 (概ね albedo × 1.5 相当) を明確に上回る
    //   Directional の基準値。旧既定 1.0 では環境光に埋もれて効果が見えなかった。
    //   Point / Spot で明るい屋外に存在感を出すなら 15〜30 が目安。
    float         intensity = 3.0f;
    float         range     = 10.0f;    // Directional 以外。Area では打ち切り距離のみ
    float         innerCone = 15.0f;    // Spot のみ (degrees)
    float         outerCone = 30.0f;    // Spot のみ (degrees)
    bool          enabled   = true;

    // ---- 発光体の大きさ ----
    // 点ではなく大きさを持つ光源として扱う半径 [m]。0 で厳密な点光源。
    //   Sphere : この半径の球
    //   Tube   : この半径 × sourceLength の長さを持つカプセル
    //   Point / Spot : 影のにじみ幅とハイライトの広がりだけに効く (形状は点のまま)
    //
    // WHY 形状を持たない Point / Spot にも効かせるか: 現実の電球やスポットには必ず
    //     大きさがあり、それが半影の幅とハイライトの大きさを決めている。0 のままだと
    //     どんなに詰めても影の縁が硬く、ハイライトが点にしかならない。
    float         sourceRadius = 0.0f;
    // Tube の長さ [m]。両端に半球が付くカプセルとして扱う。
    float         sourceLength = 1.0f;

    // ---- 面光源 (Type::Area) ----
    // 面の向きは Transform::Forward()、面内の軸は Right() / Up() を使う。
    // WHY 半寸法でなく全寸法で持つか: Inspector に「窓の幅 2m」と入れたいのであって、
    //     「半幅 1m」と入れたいわけではない。半分にするのは GPU へ渡す直前で行う。
    float         areaWidth  = 1.0f;    // [m]
    float         areaHeight = 1.0f;    // [m]
    // 面の裏側を照らさない。板の裏に光が回り込むのを防ぐ。
    bool          areaTwoSided = false;

    // ---- 影 ----
    // Directional はカスケードシャドウ (CSM)、それ以外は専用アトラス (4x4 = 16 タイル)。
    // タイルの消費は型で変わる:
    //   Spot / Area          … 1 枚 (Area は法線方向 75 度の錐台。真横は諦める)
    //   Point / Sphere / Tube … キューブ 6 面で 6 枚。本数上限は
    //                           ShadowSettings::maxShadowedPointLights
    // 光源の «大きさ» は深度ではなく半影の広さ (sourceRadius) として効く。
    // 割り当てはカメラから近い順で、あふれたライトは黙って影を落とさなくなる。
    bool  castShadows    = true;  // false のとき影を無効化 (shadowStrength=0 と等価)
    float shadowBias     = 1.0f;  // 基本バイアスへのスケール係数 (大きいほど Peter Panning が出やすい)
    float shadowStrength = 1.0f;  // 影の濃さ: 0=影なし, 1=完全な影
    float shadowDistance = 0.0f;  // Directional のみ: 0=シーンに自動フィット, >0=正射影の半幅 [m]
    // Directional 以外の透視投影 near 面 [m]。
    // WHY 露出させるか: near が小さいほど深度の分解能が near 側へ寄り、遠い側で
    //     アクネが出る。逆に大きくするとライトのすぐ手前にある caster が near で
    //     切り取られ、影が抜ける。ライトを壁や天井へ埋める使い方だと既定では
    //     詰められないケースが出るため、シーンごとに触れる値として持つ。
    float shadowNearPlane = 0.1f;

    // ---- Cookie (投影テクスチャ) ----
    // Spot のみ。ライトの円錐へ被せる白黒/カラーのマスクで、木漏れ日・窓枠・
    // ロゴのゴボを作る。空文字で無効。
    // NOTE: Point / Directional は未対応 (前者はキューブマップ、後者は
    //       ワールド空間のタイリングという別の仕組みが要る)。
    std::string cookiePath;
    // Cookie の見かけの回転 [degrees]。ライト自身を回すと影の向きまで変わってしまうため、
    // 模様だけを回す軸を別に持つ。
    float cookieRotation = 0.0f;

    // ---- 明滅 (Flicker) ----
    // たいまつのゆらぎ・破断面の放電・目の脈動。既定は Off で、既存シーンの絵は変わらない。
    //
    // WHY コンポーネントへ持たせるか: これまでは演出ごとに «LightComponent を取って
    //     intensity を毎フレーム書く» スクリプトを起こしていた。揺れの形はオーサリングの
    //     対象で、コードの対象ではない。
    //
    // 駆動は LightFlickerSystem。intensity を «オーサリング値 × 倍率» で毎フレーム
    // 書き換え、Play を抜けるときにオーサリング値へ戻す (保存へ焼き付かない)。
    //
    // ⚠ 同じ GameObject に VFXLightEnvelope を付けないこと。どちらも intensity を
    //   «捕まえて掛け直す» 作りなので、片方が書いた値をもう片方が基準として掴む。
    enum class FlickerMode : std::uint8_t {
        Off = 0,   // 無効
        Sine,      // 正弦。呼吸するような滑らかな脈動
        Noise,     // value noise。たいまつ・不安定な蛍光灯
        Curve,     // flickerCurve を 1 周期として繰り返す。作り込んだ放電の形
    };
    FlickerMode flickerMode = FlickerMode::Off;
    // 倍率の振れ幅。倍率 = 1 - flickerAmplitude * (1 - wave)、wave は [0, 1]。
    // 0 で «常に 1 倍» = 無効、1 で消灯まで振れる。
    // WHY 倍率の «減る側» だけを作るか: 上へ振ると白飛びの閾値を越えた瞬間から
    //     Bloom が別物になり、オーサリングした intensity がピークを表さなくなる。
    float    flickerAmplitude = 0.0f;
    float    flickerFrequency = 6.0f;   // [Hz]。Curve では 1 周の速さ
    // Sine / Curve の上へ混ぜるノイズの量 [0, 1]。1 で完全にノイズ。
    float    flickerNoise     = 0.0f;
    // 位相オフセット [0, 1)。同じ設定のライトを並べたときに揃って光らないようずらす。
    float    flickerPhase     = 0.0f;
    // ノイズ列の種。同じ seed と同じ位相なら、いつ何回走らせても同じ揺れになる。
    std::uint32_t flickerSeed = 0;
    // Curve モードの波形。横軸は 1 周期の正規化時間、縦軸は倍率 [0, 1] 想定。
    ParticleCurve flickerCurve{
        {{ {0.0f, 1.0f}, {0.5f, 0.25f}, {1.0f, 1.0f}, {1.0f, 1.0f},
           {1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f} }},
        3, ParticleCurveInterpolation::Smooth };

    // ---- 明滅のランタイム状態 (シリアライズしない) ----
    // WHY コンポーネントに持たせるか: 適用は intensity の «書き換え» なので、元の値を
    //     誰かが覚えていないとループのたびに暗くなっていく。System 側の別テーブルで
    //     持つと GameObject の破棄と寿命が合わず、使い回した ID で前の値が蘇る。
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
            // IReflector は uint32_t 非対応のため int 経由で往復させる。
            int seed = static_cast<int>(flickerSeed);
            r.Field("flickerSeed", seed);
            flickerSeed = static_cast<std::uint32_t>(seed < 0 ? 0 : seed);
        }
        r.Field("flickerCurve", flickerCurve);
    }
    // Directional / Spot / Area の方向 → Transform::Forward()
    // Point / Spot / Area の位置      → Transform::position
    // Area の面内軸                   → Transform::Right() / Up()
};

} // namespace fbzz::scene
