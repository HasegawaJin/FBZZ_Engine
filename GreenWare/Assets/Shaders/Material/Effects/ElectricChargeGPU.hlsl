/// @file    ElectricChargeGPU.hlsl
/// @brief   電荷パーティクル (GPU シミュレーション経路)。絵は CPU 版と同一。
/// @author  Hasegawa Jin
/// @date    2026-08-24
///
/// PSO: SOLID_NOCULL + ADDITIVE + DEPTH_READ
///
/// WHY 経路ごとに .hlsl が要るか:
///   GPU シミュレーションは頂点バッファを持たず、VS が StructuredBuffer<GpuParticle> を
///   SV_VertexID で引く。VS の供給を切り替えるのは include より前の #define なので、
///   1 本の .hlsl では両方を出せない。差し替え先は .mat の shader が決めるため、
///   .mat も経路ごとに分ける (ElectricCharge*GPU.mat)。
///
/// WHY 縮退判定では守れないか:
///   ParticleGpuFallbackReason はオーサリング設定だけを見るので、カスタムシェーダーに
///   GPU 用 VS があるかは判定に入らない。CPU 用シェーダーを差したまま Gpu にすると、
///   «縮退した» とも言われないまま粒が 1 つも出ない。ここを間違えないための唯一の
///   歯止めが «経路ごとに別の .mat» という形。
#define FBZZ_PARTICLE_GPU
#include "Material/Effects/ElectricChargeCommon.hlsli"
