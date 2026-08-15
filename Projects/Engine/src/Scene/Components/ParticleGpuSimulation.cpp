// FBZZ Engine
// ParticleGpuSimulation.cpp | fbzz::scene
// GPU シミュレーション可否とその縮退理由の判定
#include <Engine/Scene/Components/ParticleGpuSimulation.hpp>

#include <Engine/Scene/Components/ParticleEmitter.hpp>

namespace fbzz::scene {

ParticleGpuFallbackReason GetParticleGpuFallbackReason(const ParticleEmitter& emitter)
{
    if (emitter.simulationMode != ParticleSimulationMode::Gpu)
        return ParticleGpuFallbackReason::NotRequested;

    // 判定順は「事故になりやすい順」。Trail や SubEmitter は狙って設定したつもりで
    // GPU を落としている代表格なので、複数該当したときはこちらを先に名指しする。
    //
    // NOTE: sortMode はここに無い。GPU 側で bitonic sort を回すようになったため、
    //       半透明の前後関係を保ったまま GPU シミュレーションを使える
    //       (ParticleGpuSortKeys / ParticleGpuSortStep / ParticleGpuSortLocal)。
    if (emitter.simulationSpace != ParticleSimulationSpace::World)
        return ParticleGpuFallbackReason::LocalSpace;
    if (emitter.collisionMode != ParticleCollisionMode::None
        && emitter.collisionMode != ParticleCollisionMode::Depth)
        return ParticleGpuFallbackReason::Collision;
    if (emitter.trailEnabled)
        return ParticleGpuFallbackReason::Trail;
    // NOTE: meshParticlePath はここに無い。ParticleGpuMesh.hlsl のインスタンス描画で
    //       GPU 経路でもメッシュパーティクルを出せるようになったため。
    if (emitter.prewarm)
        return ParticleGpuFallbackReason::Prewarm;
    if (emitter.flipbookFrameBlending)
        return ParticleGpuFallbackReason::FlipbookFrameBlending;
    if (emitter.motionVectorFlipbook)
        return ParticleGpuFallbackReason::MotionVectorFlipbook;
    if (!emitter.birthSubEmitter.empty() || !emitter.deathSubEmitter.empty()
        || !emitter.collisionSubEmitter.empty())
        return ParticleGpuFallbackReason::SubEmitter;
    // 自己影の密度パスは CPU が組んだ頂点バッファを光源視点で描き直す方式のため、
    // GPU 経路 (頂点バッファを持たない) では測れない。
    if (emitter.selfShadowStrength > 0.0f)
        return ParticleGpuFallbackReason::SelfShadow;
    return ParticleGpuFallbackReason::None;
}

bool CanUseGpuSimulation(const ParticleEmitter& emitter)
{
    return GetParticleGpuFallbackReason(emitter) == ParticleGpuFallbackReason::None;
}

const char* ParticleGpuFallbackFieldName(ParticleGpuFallbackReason reason)
{
    switch (reason) {
    case ParticleGpuFallbackReason::None:                  return "";
    case ParticleGpuFallbackReason::NotRequested:          return "simulationMode";
    case ParticleGpuFallbackReason::LocalSpace:            return "simulationSpace";
    case ParticleGpuFallbackReason::Collision:             return "collisionMode";
    case ParticleGpuFallbackReason::Prewarm:               return "prewarm";
    case ParticleGpuFallbackReason::FlipbookFrameBlending: return "flipbookFrameBlending";
    case ParticleGpuFallbackReason::MotionVectorFlipbook:  return "motionVectorFlipbook";
    case ParticleGpuFallbackReason::Trail:                 return "trailEnabled";
    case ParticleGpuFallbackReason::SubEmitter:            return "subEmitter";
    case ParticleGpuFallbackReason::SelfShadow:            return "selfShadowStrength";
    }
    return "";
}

const char* ParticleGpuFallbackDescription(ParticleGpuFallbackReason reason)
{
    switch (reason) {
    case ParticleGpuFallbackReason::None:
        return "GPU シミュレーションで実行されます。";
    case ParticleGpuFallbackReason::NotRequested:
        return "simulationMode が Cpu です (縮退ではありません)。"
               "1 万粒子を超えるなら Gpu を検討してください。";
    case ParticleGpuFallbackReason::LocalSpace:
        return "simulationSpace = Local のため CPU で実行されます。"
               "GPU 経路はワールド座標でしか積分しません。"
               "エミッターに追従させる必要が無ければ World にすると GPU に載ります。";
    case ParticleGpuFallbackReason::Collision:
        return "collisionMode がシーン形状との判定のため CPU で実行されます。"
               "画面内の見た目だけで足りるなら Depth (深度バッファ衝突) にすると GPU に載ります。";
    case ParticleGpuFallbackReason::Prewarm:
        return "prewarm のため CPU で実行されます。"
               "「最初から定常状態で存在する」表現は GPU の逐次積分では作れません。";
    case ParticleGpuFallbackReason::FlipbookFrameBlending:
        return "flipbookFrameBlending のため CPU で実行されます。"
               "コマ数が十分あれば補間を切っても目立ちません。";
    case ParticleGpuFallbackReason::MotionVectorFlipbook:
        return "motionVectorFlipbook のため CPU で実行されます。";
    case ParticleGpuFallbackReason::Trail:
        return "trailEnabled のため CPU で実行されます。"
               "GPU 側の粒子は位置履歴を持てません。"
               "太い帯が欲しいだけなら Trail ノードを併用し、本体は GPU に載せられます。";
    case ParticleGpuFallbackReason::SubEmitter:
        return "birth / death / collision SubEmitter が設定されているため CPU で実行されます。"
               "発火判定は CPU 側にしかありません。";
    case ParticleGpuFallbackReason::SelfShadow:
        return "selfShadowStrength > 0 のため CPU で実行されます。"
               "自己影は CPU が組んだ頂点バッファを光源視点で描き直して密度を測るため、"
               "頂点バッファを持たない GPU 経路では測れません。"
               "厚みより粒子数が要る場面では selfShadowStrength を 0 にしてください。";
    }
    return "";
}

} // namespace fbzz::scene
