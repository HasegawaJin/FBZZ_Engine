/// @file    FlowField.hpp
/// @brief   媒質 (空気・水) の流速 [m/s] を定義する場。力は消費者が結合係数で導く。
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

/// @brief 流れの原始要素。ポテンシャル流 (Uniform / Sink / Source / Vortex) + 乱流 + 焼いた場で閉じる。
///
/// @note **整数値を旧 ForceFieldType と揃えて固定してある。** fieldType は Reflect が int で
///       保存し、GPU 定数バッファでも float として渡り、HLSL が同じ整数で分岐する。
///       バージョンキーを持たない既存の .scene / .vfx / .particle を読めるのはこの並びだけ。
/// @see Docs/design/flow-field.md
enum class FlowFieldType : uint8_t {
    Uniform = 0,    ///< @brief 一様流 (旧 Wind)
    Sink = 1,       ///< @brief 吸い込み。中心へ向かう流れ (旧 Attract)
    Source = 2,     ///< @brief 湧き出し。中心から遠ざかる流れ (旧 Repulse)
    Vortex = 3,     ///< @brief direction を軸とした周回流
    Curl = 4,       ///< @brief カールノイズによる発散ゼロの乱流 (旧 Turbulence)
    LegacyDrag = 5, ///< @brief **読み込み専用**。旧 Drag。読み込み時に ParticleEmitter::flowCoupling へ写す
    Baked = 6,      ///< @brief 焼いた速度場 (速度場 PNG) をサンプルする (旧 VectorField)
};

/// @brief enum の要素数。Reflect / codec / UI の clamp がここを見る。
/// @note LegacyDrag を含む。UI の選択肢からは除くこと (書かれてはいけない値なので)。
inline constexpr int kFlowFieldTypeCount = 7;

/// @brief 流れの «原点と向き» をどこから取るか。
///
/// @note 同じ設定型をシーン配置の場とエミッター内蔵の流れの両方で使うため、
///       «誰の座標系で解決するか» だけを外から指定できる必要がある。
enum class FlowFieldSpace : uint8_t {
    World = 0,  ///< @brief 位置と向きをそのままワールドとして扱う (シーン配置の場は常にこちら)
    Emitter,    ///< @brief 原点 = エミッター位置、向き = エミッターの回転を受ける
};

/// @brief 流れ 1 本の設定。**GameObject に依存しない値の塊**。
///
/// @note 同じ «流れ» が 2 か所に住んでいる — シーンへ置く FlowField と、
///       ParticleEmitter::localForces。設定型を 1 つにすれば評価器も 1 本で済む。
struct FlowFieldSettings {
    bool enabled = true;
    FlowFieldType fieldType = FlowFieldType::Uniform;
    FlowFieldSpace space = FlowFieldSpace::World;
    /// @brief 流速の大きさ [m/s]。
    /// @note Baked のときだけ無次元の倍率 (焼いた場が既に m/s を持つため)。
    float strength = 5.0f;
    /// @brief 影響半径 [m]。0 以下でシーン全体へ減衰なしに作用する。
    float radius = 5.0f;
    /// @brief 距離減衰カーブ: influence = (1 - dist/radius)^falloffPower。radius <= 0 のとき無効。
    float falloffPower = 2.0f;
    /// @brief Uniform: 流れの向き / Vortex: 回転軸。space が決める座標系で解釈する。
    math::Vector3 direction = { 1.0f, 0.0f, 0.0f };
    /// @brief Curl 用: ノイズ格子の空間周波数 [1/m] とスクロール速度。
    float noiseFrequency = 0.5f;
    float noiseSpeed = 1.0f;
    /// @brief ParticleEmitter::flowFieldChannels と 1 ビットでも重なったエミッターにだけ作用する。
    /// @note 既定は全ビット ON なので、マスクを触らない既存シーンの挙動は変わらない。
    /// @note これが無いと «＋の粒子は−の電極へ引かれるが＋の電極には反発する» のように、
    ///       同じ空間に住む粒子を別々の場で動かす表現が作れない。
    uint32_t channels = 0xFFFFFFFFu;

    /// @name Baked 型のときだけ意味を持つ
    /// @{
    /// @brief 焼いた速度場 (速度場 PNG / .fga)。空なら Baked は何もしない。
    std::string vectorFieldPath;
    /// @brief 場の AABB をワールドでどの寸法へ写すか (半径 [m])。
    /// @note アセット側の bounds をそのまま使うと、同じ場を «部屋いっぱいの渦» と
    ///       «手のひらの渦» に使い回せない。
    math::Vector3 vectorFieldExtents = { 5.0f, 5.0f, 5.0f };

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        int typeValue = static_cast<int>(fieldType);
        r.Field("fieldType", typeValue);
        typeValue = std::clamp(typeValue, 0, kFlowFieldTypeCount - 1);
        fieldType = static_cast<FlowFieldType>(typeValue);
        int spaceValue = static_cast<int>(space);
        r.Field("space", spaceValue);
        space = static_cast<FlowFieldSpace>(std::clamp(spaceValue, 0, 1));
        r.Field("strength", strength);
        r.Field("radius", radius);
        r.Field("falloffPower", falloffPower);
        r.Field("direction", direction);
        r.Field("noiseFrequency", noiseFrequency);
        r.Field("noiseSpeed", noiseSpeed);
        /// @note IReflector は符号なし整数を扱わない。ビットパターンは int 経由でも保たれる。
        int channelsValue = static_cast<int>(channels);
        r.Field("channels", channelsValue);
        channels = static_cast<uint32_t>(channelsValue);
        r.BeginField("vectorFieldPath", "Vector Field");
        r.SetFileExtensions(".png,.fga");
        r.Field("vectorFieldPath", vectorFieldPath);
        r.EndField();
        r.Field("vectorFieldExtents", vectorFieldExtents);
        /// @note 旧 vectorFieldTightness は書かない。全型が緩和になったので «どれだけ場へ
        ///       従わせるか» は ParticleEmitter::flowCoupling が一手に持つ。旧ファイルの
        ///       キーは読まれずに落ちる (未知キーは IReflector が無視する)。
    }
    /// @}
};

/// @brief 粒子の結合係数 [1/s] の既定値。
///
/// @note 旧 ParticleEmitter の «内蔵 Drag» が持っていた減衰係数と同じ枠で、単位も [1/s] のまま。
///       既定値は FlowFieldSettings::strength の既定 (5.0 — MakeDefaultLocalForces が置いていた
///       値でもある) に揃えてある。0 にすると «流れを置いても粒子が動かない» になる。
inline constexpr float kDefaultFlowCoupling = 5.0f;

/// @brief 旧 strength [m/s^2] を流速 [m/s] へ写す係数。
///
/// @note 導出: 旧式は静止粒子に対し dv = a·dt、新式は dv = (v_flow − 0)·k·dt。
///       等置して v_flow = a / k。k は既定の結合係数 kDefaultFlowCoupling。
/// @note 掛けるのは旧ファイルを読むときだけ (シーンの ForceField / エミッターの localForces)。
///       旧 Drag の strength は [1/s] なので掛けない ─ そのまま flowCoupling になる。
inline constexpr float kLegacyAccelerationToFlowSpeed = 1.0f / kDefaultFlowCoupling;

/// @brief 流れのリストを Reflect する。ParticleEmitter::localForces と FlowField::forces が共用する。
/// @note 自由関数なのは、同じ «流れの並び» を 2 つの型が持つため。片方だけキー名を変えると、
///       エミッター内蔵の流れとシーンの場で保存形式が分かれる。
void ReflectFlowFieldList(IReflector& r, std::vector<FlowFieldSettings>& forces,
                          const char* key, const char* displayName);

/// @brief シーンへ置く流れの場。中心は Transform.worldPosition、direction は worldRotation で回る。
///
/// @note リストなのは «環境風» が「一定方向の流れ」と「乱れ」の 2 本で 1 つの概念だったため。
///       1 コンポーネント = 1 本にすると GameObject が 2 つ要る。
/// @note 環境流 (シーン全体に一律の流れ) はここではなく SceneEnvironment が持つ。
///       半径 0 を «環境風» と読む暗黙の規約は 2026-09-16 に廃止した。
struct FlowField {
    std::vector<FlowFieldSettings> forces = { FlowFieldSettings{} };

    const char* GetTypeName() const { return "Flow Field"; }
    void Reflect(IReflector& r) { ReflectFlowFieldList(r, forces, "forces", "Forces"); }
};

} // namespace fbzz::scene
