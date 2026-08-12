// FBZZ Engine
// Rendering/ParticleSelfShadow.hlsli | Common
// パーティクルの自己影: 光源側で積んだ密度から透過率を求める
//
// WHY: 受け影 (シャドウマップ) は「他の物体が落とす影」しか扱えない。粒子群が
//      自分自身へ落とす影が無いと、厚みのある煙や雲は光の当たり方が一様になり、
//      どれだけ粒子を重ねても平坦な塊に見える。厚みの表現はここでしか出せない。
//
// 手法: 光源から見た向きで全粒子のアルファを 1 枚の RT へ加算し、
//       R = Σα (柱全体の密度)、G = Σ(α × 光源側深度) を持たせる。
//       ある画素の光源側深度 d に対し「d より光源に近い側の密度」を
//       総量と平均深度から推定し、Beer-Lambert 則で透過率へ変換する。
//
// これは近似である。柱の密度分布を平均深度 1 つで代表するため、
// 前後に離れた 2 つの塊が同じ平均を持つケースは区別できない。
// 厳密解はライト方向のスライス分割 (half-angle slice rendering) が要るが、
// パス数が粒子の重なり数に比例して増え、エフェクト用途には重すぎる。
// 目的は「煙に厚みが見えること」であって物理的正しさではない、という判断。
#ifndef FBZZ_PARTICLE_SELF_SHADOW_INCLUDED
#define FBZZ_PARTICLE_SELF_SHADOW_INCLUDED

// density: 密度バッファの RG (R=Σα, G=Σα·深度)
// lightDepth01: この画素の光源側深度 [0,1]
// strength: 消衰係数。大きいほど濃く影が出る (0 で無効)
float ComputeParticleSelfShadowTransmittance(float2 density, float lightDepth01, float strength)
{
    if (strength <= 0.0f) return 1.0f;
    const float total = density.r;
    if (total <= 1.0e-4f) return 1.0f;

    // 密度の重心 (平均深度)。ここより奥にある画素ほど多くの密度に遮られている。
    const float meanDepth = density.g / total;
    // 深度差を 0..1 の遮蔽割合へ写す。係数 8 は「平均から 1/16 ほど手前/奥へ
    // ずれれば概ね全部/ゼロが手前と見なされる」硬さで、煙の厚みが階段状に見えない範囲。
    const float aheadRatio = saturate(0.5f + (lightDepth01 - meanDepth) * 8.0f);
    const float densityAhead = total * aheadRatio;
    // Beer-Lambert。exp なので密度が増えても 0 へ漸近し、黒く潰れきらない。
    return exp(-densityAhead * strength);
}

// ワールド座標を光源空間へ投影して密度バッファを引き、透過率を返す。
// テクスチャ/サンプラーを引数で受けるのは Shadow.hlsli の ComputeShadow と同じ理由
// (hlsli 側でレジスタを固定すると、スロット割り当てを呼び出し側で選べなくなる)。
float ComputeParticleSelfShadowFromMap(Texture2D densityMap, SamplerState densitySampler,
                                       float3 worldPos, float4x4 lightViewProjection,
                                       float strength)
{
    if (strength <= 0.0f) return 1.0f;
    float4 lightClip = mul(float4(worldPos, 1.0f), lightViewProjection);
    if (lightClip.w <= 1.0e-5f) return 1.0f;
    float3 lightNdc = lightClip.xyz / lightClip.w;
    float2 uv = float2(lightNdc.x * 0.5f + 0.5f, 0.5f - lightNdc.y * 0.5f);
    // 密度バッファの外は「遮る物が無い」。端で影が張り付くのを防ぐ。
    if (any(uv < 0.0f) || any(uv > 1.0f)) return 1.0f;
    float2 density = densityMap.Sample(densitySampler, uv).rg;
    return ComputeParticleSelfShadowTransmittance(density, saturate(lightNdc.z), strength);
}

#endif // FBZZ_PARTICLE_SELF_SHADOW_INCLUDED
