/// @file    ParticleGpuSimulation.hpp
/// @brief   simulationMode = Gpu が実際に GPU で回るか、回らないなら「どの設定のせいか」を判定する。
/// @author  Hasegawa Jin
/// @date    2026-08-12
///
/// @note GPU シミュレーションは条件を 1 つでも外すと黙って CPU へ縮退し、それが Editor にも
///       lint にも実行状態にも出ていなかった。判定を 1 か所へ集約して「落ちた理由」まで返し、
///       静的診断 (vfx.lint)・実行状態 (vfx.runtime)・Editor 表示・Script
///       (particle.GetGpuFallbackReason) の 4 経路が同じ答えを出せるようにする。
/// @note 「GPU で大量」と「正しいソート」が両立しないという設計判断自体は妥当。担当者と AI が
///       その判断をできるよう、まず見えている必要がある、というのがここの目的。
#pragma once

/// @note ParticleGpuFallbackReason の宣言は ScriptParticleProxy.hpp にある。Script も同じ理由を
///       受け取れる必要があり、Script に Engine 実装 (Components/) を include させない規約に
///       ParticleSimulationMode 等と同じ置き方で揃える。
#include <Engine/Scene/ScriptProxy/ScriptParticleProxy.hpp>

namespace fbzz::asset { struct ParticleMaterialSettings; }

namespace fbzz::scene {

struct ParticleEmitter;
/// GPU に載るかはオーサリング設定だけで決まるので、判定関数は設定型を受ける。
struct ParticleEmitterSettings;

/// 縮退理由を返す。emitter が Gpu を要求していない場合は NotRequested。
/// @note 設定型を受けるのは、GPU に載るかが «どう設定したか» だけで決まり、粒子列や GPU ハンドル
///       といった実行時状態を一切見ないため。実体を持たない VFX グラフのノードにも同じ判定を使える。
/// @param material 解決済みの .mat [particle]。フリップブック補間・モーションベクター・
///        自己影は素材側の値なので、渡さなければその 3 つは判定に入らない
///        (パスは runtime.material を渡す。素材を解決していない静的診断は null でよい)。
[[nodiscard]] ParticleGpuFallbackReason GetParticleGpuFallbackReason(
    const ParticleEmitterSettings& emitter,
    const asset::ParticleMaterialSettings* material = nullptr);

/// 実際に GPU シミュレーションで回るか (= GetParticleGpuFallbackReason が None か)。
[[nodiscard]] bool CanUseGpuSimulation(const ParticleEmitterSettings& emitter,
                                       const asset::ParticleMaterialSettings* material = nullptr);

/// 診断コード用の短い識別子 (例 "sortMode")。lint / runtime / UI が同じ語で指す。
[[nodiscard]] const char* ParticleGpuFallbackFieldName(ParticleGpuFallbackReason reason);

/// 担当者と AI 向けの説明。「なぜ落ちたか」と「どうすれば GPU に載るか」を含む。
[[nodiscard]] const char* ParticleGpuFallbackDescription(ParticleGpuFallbackReason reason);

} // namespace fbzz::scene
