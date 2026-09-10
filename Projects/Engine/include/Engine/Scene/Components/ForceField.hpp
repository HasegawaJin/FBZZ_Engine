/// @file    ForceField.hpp
/// @brief   パーティクルへ風・吸引・渦・乱流・速度場を加えるベクトルフィールド。
/// @author  Hasegawa Jin
/// @date    2026-07-15
#pragma once
#include <Engine/Scene/Script.hpp>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

// WHY: エミッター単体の gravity/velocityDamping では表現できない「空間の場」による挙動を
//      種類ごとに分ける。数値は GPU 定数バッファへ float として渡すため順序を変更しないこと。
enum class ForceFieldType : uint8_t {
    Wind = 0,    // direction 方向へ一定加速 (風)
    Attract,     // 力場中心へ引き寄せる (吸引)
    Repulse,     // 力場中心から遠ざける (反発)
    Vortex,      // direction を軸とした接線方向の渦
    Turbulence,  // カールノイズによる発散ゼロの乱流ベクトル場
    Drag,        // 速度に比例した減速 (空気抵抗)
    VectorField, // 焼いた .vfield をサンプルして任意の流れを与える
};

/// enum の要素数。Reflect / codec / UI の clamp がここを見る。
/// WHY: 三か所に散らばった «> 5 ? 5 :» が、種類を足したときに 1 つだけ直し漏れる。
inline constexpr int kForceFieldTypeCount = 7;

/// 力の «原点と向き» をどこから取るか。
///
/// WHY: 同じ設定型をシーン配置の力場とエミッター内蔵の力の両方で使うため、
///      «誰の座標系で解決するか» だけを外から指定できる必要がある。
///      重力はエミッターを傾けても下を向き続けるので World、周回はエミッター原点が
///      要るので Emitter、と 1 本の列挙で足りる。
enum class ForceFieldSpace : uint8_t {
    World = 0,  ///< 位置と向きをそのままワールドとして扱う (シーン配置の力場は常にこちら)
    Emitter,    ///< 原点 = エミッター位置、向き = エミッターの回転を受ける
};

/// 力場 1 本の設定。**GameObject に依存しない値の塊**。
///
/// WHY コンポーネントから分けるか:
///   同じ «力» が 2 か所に住んでいた — シーンへ置く ForceField と、
///   ParticleEmitter が内蔵していた gravity / velocityDamping / noise* / orbital* / radial*。
///   式はどちらも同じなのに型が違うため、評価関数が CPU で 2 本・HLSL で 2 本に分かれ、
///   «片方にだけ機能が足される» が起きていた。設定型を 1 つにすれば評価器も 1 本で済む。
struct ForceFieldSettings {
    bool enabled = true;
    ForceFieldType fieldType = ForceFieldType::Wind;
    ForceFieldSpace space = ForceFieldSpace::World;
    // 加速度の大きさ [m/s^2]。Drag のときは減衰係数 [1/s] として扱う。
    float strength = 5.0f;
    // 影響半径 [m]。0 以下でシーン全体へ減衰なしに作用する (グローバル風など)。
    float radius = 5.0f;
    // 距離減衰カーブ: influence = (1 - dist/radius)^falloffPower。radius <= 0 のとき無効。
    float falloffPower = 2.0f;
    // Wind: 風向き / Vortex: 回転軸。space が決める座標系で解釈する。
    math::Vector3 direction = { 1.0f, 0.0f, 0.0f };
    // Turbulence 用: ノイズ格子の空間周波数 [1/m] とスクロール速度。
    float noiseFrequency = 0.5f;
    float noiseSpeed = 1.0f;
    /// ParticleEmitter::forceFieldChannels と 1 ビットでも重なったエミッターにだけ作用する。
    /// WHY: 力場は本来シーン全体へ一律に効く。それだけでは「＋の粒子は−の電極へ引かれるが
    ///      ＋の電極には反発する」のように、同じ空間に住む粒子を別々の場で動かす表現が作れない。
    ///      Attract を 1 つ置いた瞬間に全エミッターの粒子が同じ 1 点へ collapse してしまう。
    ///      既定は全ビット ON なので、マスクを触らない既存シーンの挙動は変わらない。
    uint32_t channels = 0xFFFFFFFFu;

    // ── VectorField 型のときだけ意味を持つ ──
    /// 焼いた速度場 (.vfield / .fga)。空なら VectorField は何もしない。
    std::string vectorFieldPath;
    /// 場の AABB をワールドでどの寸法へ写すか (半径 [m])。
    /// WHY: アセット側の bounds をそのまま使うと、同じ場を «部屋いっぱいの渦» と
    ///      «手のひらの渦» に使い回せない。1 枚を寸法違いで貼れるようにする。
    math::Vector3 vectorFieldExtents = { 5.0f, 5.0f, 5.0f };
    /// 0 = 場を加速度として加算 / 1 = 粒子速度を場の値そのものへ寄せきる。
    /// 途中の値は「どれだけ強く場へ従わせるか」。1 に寄せるほど初速や重力を無視して
    /// 流れに乗るので、レールの上を走らせる演出はこちらを上げる。
    float vectorFieldTightness = 0.0f;

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        int typeValue = static_cast<int>(fieldType);
        r.Field("fieldType", typeValue);
        typeValue = std::clamp(typeValue, 0, kForceFieldTypeCount - 1);
        fieldType = static_cast<ForceFieldType>(typeValue);
        int spaceValue = static_cast<int>(space);
        r.Field("space", spaceValue);
        space = static_cast<ForceFieldSpace>(std::clamp(spaceValue, 0, 1));
        r.Field("strength", strength);
        r.Field("radius", radius);
        r.Field("falloffPower", falloffPower);
        r.Field("direction", direction);
        r.Field("noiseFrequency", noiseFrequency);
        r.Field("noiseSpeed", noiseSpeed);
        // IReflector は符号なし整数を扱わない。ビットパターンは int 経由でも保たれる。
        int channelsValue = static_cast<int>(channels);
        r.Field("channels", channelsValue);
        channels = static_cast<uint32_t>(channelsValue);
        r.BeginField("vectorFieldPath", "Vector Field");
        r.SetFileExtensions(".vfield,.fga");
        r.Field("vectorFieldPath", vectorFieldPath);
        r.EndField();
        r.Field("vectorFieldExtents", vectorFieldExtents);
        r.Field("vectorFieldTightness", vectorFieldTightness);
    }
};

/// 力のリストを Reflect する。ParticleEmitter::localForces と ForceField::forces が共用する。
/// WHY 自由関数か: 同じ «力の並び» を 2 つの型が持つ。片方だけキー名を変えると、
///      エミッター内蔵の力とシーンの力場で保存形式が分かれる。
void ReflectForceFieldList(IReflector& r, std::vector<ForceFieldSettings>& forces,
                           const char* key, const char* displayName);

// ForceField — シーンへ置く力場コンポーネント。
// 力場の中心は GameObject の Transform.worldPosition、
// direction は Transform.worldRotation で回転してワールド空間へ変換される。
// WHY: エミッターから独立した GameObject として配置することで、
//      1 つの風・渦を複数エミッターへ同時に効かせたり、動く力場を作れるようにする。
struct ForceField {
    /// この GameObject が発する力。
    ///
    /// WHY 1 本ではなくリストか:
    ///   «環境風» は「一定方向の風」と「乱れ」の 2 本で 1 つの概念だった (旧 WindZoneComponent)。
    ///   1 コンポーネント = 1 力にすると、風を置くのに GameObject が 2 つ要る。
    ///   ParticleEmitter::localForces と同じ形にしておけば、内蔵の力とシーンの力場で
    ///   オーサリングも評価器も 1 つに揃う。
    std::vector<ForceFieldSettings> forces = { ForceFieldSettings{} };

    const char* GetTypeName() const { return "Force Field"; }
    void Reflect(IReflector& r) { ReflectForceFieldList(r, forces, "forces", "Forces"); }
};

/// 旧 WindZoneComponent 相当の 2 本 (一定方向の風 + 乱れ) を組み立てる。
/// WHY 残すか: «風を置く» は最頻出の操作で、Wind と Turbulence を毎回 2 本足させるのは後退。
///      WindZone というコンポーネントは消したが、語彙はプリセットとして残す。
[[nodiscard]] std::vector<ForceFieldSettings> MakeAmbientWindForces(
    const math::Vector3& direction = { 0.7071f, 0.0f, 0.7071f },
    float strength = 1.0f, float turbulence = 0.0f, float pulseFrequency = 1.0f);

} // namespace fbzz::scene
