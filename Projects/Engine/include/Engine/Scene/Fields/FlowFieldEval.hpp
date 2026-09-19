/// @file    FlowFieldEval.hpp
/// @brief   流れの場の解決・サンプル・粒子への適用。式は ParticleGpuSim.cs.hlsl と対。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// @note 描画パスの private ヘッダー (RenderPasses/Geometry/ParticleForces.hpp) から
///       Scene/Fields/ へ移した。WaterSystem / VolumetricCloudPass が «描画パスの内部» を
///       include していた依存の逆立ちを断つため。
/// @see Docs/design/flow-field.md
#pragma once
#include "Engine/Scene/Fields/FlowField.hpp"
#include <cstdint>
#include <vector>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::fluid { struct VectorFieldAsset; }

namespace fbzz::scene {

class Scene;
struct Transform;
struct ParticleEmitter;

/// @brief 1 フレーム分に解決した流れ 1 本 (ワールド空間へ解決済み)。
/// @brief シーンに置いた FlowField と、エミッターが内蔵する流れの両方がこの形になる。
struct ActiveFlowField {
    math::Vector3 position;
    float         radius;
    math::Vector3 direction;  ///< @brief Uniform: 流向 / Vortex: 回転軸 (正規化済み)
    float         strength;   ///< @brief 流速 [m/s] (Baked のみ無次元の倍率)
    FlowFieldType type;
    float         falloffPower;
    float         noiseFrequency;
    float         noiseSpeed;
    uint32_t      channels;   ///< @brief ParticleEmitter::flowFieldChannels と AND を取る

    /// @name Baked 型のときだけ使う
    /// @{
    const fluid::VectorFieldAsset* vectorField = nullptr;
    /// @brief ワールド → 場のローカルへ戻す逆回転。場を回して置けるようにするため。
    math::Quaternion inverseRotation{};
    math::Vector3    extents = { 1.0f, 1.0f, 1.0f };
    /// @}
};

/// @brief 流れがこのエミッターに作用するか。
[[nodiscard]] bool AffectsEmitter(const ActiveFlowField& field, uint32_t emitterChannels);

/// @brief ワールド球と流れの有効範囲の交差。無限の場と不明な Bounds は残す。
[[nodiscard]] bool FlowIntersectsSphere(const ActiveFlowField& field, const math::Vector3& center, float radius);

/// @return 場の流速の保守的上限 [m/s]。GPU 粒子の次フレームの Bounds に使う。
[[nodiscard]] float FlowSpeedBound(const ActiveFlowField& field);

/// @brief 設定 1 本をワールド空間の ActiveFlowField へ解決する。
/// @param origin,rotation «この流れをどの座標系で置くか» を呼び出し側が決めて渡す。
[[nodiscard]] ActiveFlowField ResolveFlowField(const FlowFieldSettings& settings,
                                               const math::Vector3&    origin,
                                               const math::Quaternion& rotation);

/// @brief シーン上の有効な FlowField をワールド空間へ解決して out へ集める。
/// @note カメラを引数に取らない。場はカメラに属さないので、カリングマスクで絞ると
///       «Scene View でレイヤーを隠すと粒子の動きが変わる» が起きる (flow-field.md §5)。
void GatherFlowFields(Scene& scene, std::vector<ActiveFlowField>& out);

/// @brief 1 点の媒質速度 [m/s] を返す純関数。**この式が場の定義そのもの**。
///
/// @param covered 非 null なら «この点を覆う場が 1 本でもあったか» を返す。
/// @note 各原始要素の «速度» を足し合わせるだけ。力にするのは消費者の仕事。
/// @note LegacyDrag は流れではないので何も足さない (読み込み時に flowCoupling へ写る)。
/// @note 式は ParticleGpuSim.cs.hlsl の SampleFlow と一致させること。
[[nodiscard]] math::Vector3 SampleFlow(const math::Vector3& position,
                                       const std::vector<ActiveFlowField>& fields,
                                       uint32_t channels, float time,
                                       bool* covered = nullptr);

/// @brief このエミッターに効く流れを 1 本のリストへまとめる。すべて **ワールド空間** で解決する。
///
/// @note ワールドに揃えるのは、混在していると simulationSpace = Local のエミッターを
///       傾けたときに内蔵の流れだけが一緒に傾くため。
/// @note receiveFlowFields が効くのはシーン側だけ。内蔵の流れは «このエミッターの運動»。
void ResolveEmitterForces(const ParticleEmitter& emitter, const Transform& tf,
                          const std::vector<ActiveFlowField>& sceneFields,
                          std::vector<ActiveFlowField>& outFields, bool useGpuBounds = false);

/// @brief 流れへ粒子速度を緩和させる。**v += (v_flow − v) · coupling · dt**。
/// @param coupling 結合係数 [1/s]。ParticleEmitter::flowCoupling に Drag over Lifetime カーブの現在値を掛けたもの。
/// @note 式は ParticleGpuSim.cs.hlsl の ApplyFlowFields と一致させること (Baked を除く。あちらはアトラス常駐分だけが GPU で効く)。
/// @note 重力はここに含めない。媒質の運動ではなく加速度なので ParticleEmitter::gravity が別に積分する (flow-field.md §3)。
/// @warning **場に覆われていない点では何もしない。** «場が無い» は «流速 0 の静止した空気» ではなく «媒質について何も言っていない» と読む。0 へ緩和させると場を 1 つ置いただけで半径の外の粒子も一斉に減速する。空気抵抗が欲しければ «半径 0・strength 0 の Uniform» を置く (flow-field.md §3)。
void ApplyFlowFields(const std::vector<ActiveFlowField>& fields,
                     uint32_t             emitterChannels,
                     const math::Vector3& position,
                     math::Vector3&       velocity,
                     float dt, float time, float coupling);

} // namespace fbzz::scene
