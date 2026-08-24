/// @file    ElectricCharge.hlsl
/// @brief   電荷パーティクル (CPU シミュレーション経路)。
/// @author  Hasegawa Jin
/// @date    2026-08-23
///
/// PSO: SOLID_NOCULL + ADDITIVE + DEPTH_READ
///
/// ParticleEmitter.materialPath の .mat (render_path = "particle") が shader に
/// これを指すと、ParticlePass が組み込み Particle.hlsl の代わりに差す。
/// 頂点バッファからのビルボード展開は ParticleMaterial.hlsli が供給する。
///
/// 絵 (MaterialConstants と PSMain) は ElectricChargeCommon.hlsli にある。
/// simulationMode = Gpu のエミッターにはこれではなく ElectricChargeGPU.hlsl を差すこと。
/// GPU 経路は頂点バッファを持たないので、こちらを差すと粒が 1 つも出ない。
#include "Material/Effects/ElectricChargeCommon.hlsli"
