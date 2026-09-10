/// @file    ParticleForces.hpp
/// @brief   粒子に効く力の収集と適用。シーンに置いた力場と内蔵の力を同じ形で扱う。
/// @author  Hasegawa Jin
/// @date    2026-09-11
///
/// WHY ParticlePass から分けるか:
///   重力・空気抵抗・乱流・周回・放射・速度場・シーンの力場が 1 本の評価器へ統合され、
///   «力» は粒子の発生や描画とは独立した層になった。同じファイルに置いておくと、
///   力を 1 種類足すたびに 2800 行のファイルを開くことになる。
#pragma once
#include "Engine/Scene/Components/ParticleForceField.hpp"
#include <cstdint>
#include <vector>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::asset { struct VectorFieldAsset; }

namespace fbzz::scene {

class Scene;
struct Transform;
struct ParticleEmitter;
struct RenderPassContext;

/// 1 フレーム分に解決した力場 1 本 (ワールド空間へ解決済み)。
/// シーンに置いた ParticleForceField と、エミッターが内蔵する力の両方がこの形になる。
struct ActiveForceField {
    math::Vector3          position;
    float                  radius;
    math::Vector3          direction; // Wind: 風向き / Vortex: 回転軸 (正規化済み)
    float                  strength;
    ParticleForceFieldType type;
    float                  falloffPower;
    float                  noiseFrequency;
    float                  noiseSpeed;
    uint32_t               channels;  // ParticleEmitter::forceFieldChannels と AND を取る

    // ── VectorField 型のときだけ使う ──
    const asset::VectorFieldAsset* vectorField = nullptr;
    /// ワールド → 場のローカルへ戻す逆回転。場を回して置けるようにするため。
    math::Quaternion       inverseRotation{};
    math::Vector3          extents = { 1.0f, 1.0f, 1.0f };
    float                  tightness = 0.0f;

    /// エミッター内蔵の力か。Drag over Lifetime カーブが掛かる相手を選ぶのに使う。
    /// WHY: 寿命による減衰の作り分けは «この粒子の設定» で、シーンに置いた空気抵抗は
    ///      «その場所の性質»。後者に粒子の寿命を掛けると、同じ場が粒ごとに違う強さになる。
    bool                   local = false;
};

/// 力場がこのエミッターに作用するか。
[[nodiscard]] bool AffectsEmitter(const ActiveForceField& field, uint32_t emitterChannels);

/// 設定 1 本をワールド空間の ActiveForceField へ解決する。
/// origin / rotation は «この力をどの座標系で置くか» を呼び出し側が決めて渡す。
[[nodiscard]] ActiveForceField ResolveForceField(const ParticleForceFieldSettings& settings,
                                                 const math::Vector3&    origin,
                                                 const math::Quaternion& rotation,
                                                 bool                    isLocal);

/// シーンから有効な ParticleForceField を収集しワールド空間へ解決する。
/// パス先頭で 1 回だけ収集して全エミッターで共有する (エミッターごとに走査すると
/// O(エミッター数×オブジェクト数) になる)。
[[nodiscard]] std::vector<ActiveForceField> GatherForceFields(Scene& scene, uint32_t cullingMask);
[[nodiscard]] std::vector<ActiveForceField> GatherForceFields(RenderPassContext& ctx);

/// このエミッターに効く力を 1 本のリストへまとめる。すべて **ワールド空間** で解決する。
///
/// 並び順は «内蔵の力が先»。Drag が速度に掛け算で効く以外はすべて加算なので、
/// 順序が絵を変えるのは Drag が絡むときだけ。それでも順を固定しておくのは、
/// シーンにオブジェクトを 1 つ足しただけで既存の煙の減衰が変わらないようにするため。
///
/// receiveForceFields が効くのはシーン側だけ。内蔵の力は «このエミッターの運動» なので、
/// 環境の風を切っても重力まで消えたりはしない。
void ResolveEmitterForces(const ParticleEmitter& emitter, const Transform& tf,
                          const std::vector<ActiveForceField>& sceneFields,
                          std::vector<ActiveForceField>&       outFields);

/// 力場を粒子速度へ適用する。式は ParticleGpuSim.cs.hlsl の ApplyForceFields と一致させること
/// (VectorField を除く。あちらは CPU 専用で、GPU では ParticleGpuFallbackReason が立つ)。
/// @param dragScale Drag over Lifetime カーブの現在値。内蔵の Drag 力にだけ掛かる。
void ApplyForceFields(const std::vector<ActiveForceField>& fields,
                      uint32_t             emitterChannels,
                      const math::Vector3& position,
                      math::Vector3&       velocity,
                      float dt, float time, float dragScale = 1.0f);

} // namespace fbzz::scene
