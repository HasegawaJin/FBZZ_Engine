// FBZZ Engine
// ParticleGpuSimulation.hpp | fbzz::scene
// simulationMode = Gpu が実際に GPU で回るか、回らないなら「どの設定のせいか」を判定する
//
// WHY: GPU シミュレーションは条件を全て満たしたときだけ有効になり、1 つでも外れると
//      黙って CPU へ縮退する。10 万粒子を狙って Gpu を指定したのに per-particle Trail を
//      付けたせいで CPU で回っていた、という事故が起きるが、
//      それが Editor にも lint にも実行状態にも一切出ていなかった。
//      判定を 1 か所へ集約して「落ちた理由」まで返すことで、
//      静的診断 (vfx.lint)・実行状態 (vfx.runtime)・Editor 表示の 3 経路が同じ答えを出せる。
//
// NOTE: 「GPU で大量」と「正しいソート」は両立しない、という設計判断そのものは妥当。
//       妥当な判断を担当者と AI にさせるために、まず見えている必要がある、というのがここの目的。
#pragma once

namespace fbzz::scene {

struct ParticleEmitter;

// GPU シミュレーションが使えない理由。None なら GPU で回る。
enum class ParticleGpuFallbackReason {
    None = 0,
    NotRequested,          // simulationMode が Cpu。そもそも要求していない (縮退ではない)
    LocalSpace,            // simulationSpace = Local。GPU 側は World 座標でしか積分しない
    // sortMode != None。GPU bitonic sort の導入で解消済みで、現在この値は返らない。
    // 列挙値を詰めるとシリアライズ済みの数値と食い違うため、欠番として残す。
    Sorting,
    Collision,             // collisionMode が Depth 以外。シーン形状との判定は CPU 側にしかない
    Prewarm,               // prewarm。過去へ遡ったスポーンは GPU の逐次積分では作れない
    FlipbookFrameBlending, // フレーム間補間
    MotionVectorFlipbook,  // モーションベクター付きフリップブック
    Trail,                 // per-particle Trail。GpuParticle は位置履歴を持たない
    // meshParticlePath。GPU インスタンス描画の導入で解消済みで、現在この値は返らない (欠番)。
    MeshParticle,
    SubEmitter,            // birth / death / collision SubEmitter。発火は CPU 側の処理
    SelfShadow,            // selfShadowStrength > 0。密度パスが CPU 頂点バッファを要求する
};

// 縮退理由を返す。emitter が Gpu を要求していない場合は NotRequested。
[[nodiscard]] ParticleGpuFallbackReason GetParticleGpuFallbackReason(const ParticleEmitter& emitter);

// 実際に GPU シミュレーションで回るか (= GetParticleGpuFallbackReason が None か)。
[[nodiscard]] bool CanUseGpuSimulation(const ParticleEmitter& emitter);

// 診断コード用の短い識別子 (例 "sortMode")。lint / runtime / UI が同じ語で指す。
[[nodiscard]] const char* ParticleGpuFallbackFieldName(ParticleGpuFallbackReason reason);

// 担当者と AI 向けの説明。「なぜ落ちたか」と「どうすれば GPU に載るか」を含む。
[[nodiscard]] const char* ParticleGpuFallbackDescription(ParticleGpuFallbackReason reason);

} // namespace fbzz::scene
