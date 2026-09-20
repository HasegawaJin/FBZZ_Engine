/// @file    GTAO.cs.hlsl
/// @brief   Ground Truth Ambient Occlusion。スライスごとに両側の地平角を探し、余弦重みの可視度を解析積分する。
/// @author  Hasegawa Jin
/// @date    2026-06-23
/// @note    ビュー空間は左手系 (前方 +Z、上 +Y)。カメラ深度は Reversed-Z。出力は半解像度、深度・法線はフル解像度。
/// @note    t6 = GBuffer1 (ワールド法線 RGB)、t7 = 深度、u6 = 出力。b0 Camera / b5 PostProc / b8 AdvancedGraphics。
/// @see     https://www.activision.com/cdn/research/Practical_Real_Time_Strategies_for_Accurate_Indirect_Occlusion_NEW%20VERSION_COLOR.pdf (Jimenez et al., "Practical Real-Time Strategies for Accurate Indirect Occlusion", 2016)
/// @see     https://github.com/GameTechDev/XeGTAO (Intel, XeGTAO — XeGTAO.hlsli の XeGTAO_MainPass)

#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Common/Random.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(texGBuffer1, TEX_GBUFFER1_SLOT);
FBZZ_TEX2D_T(float, texDepth, TEX_DEPTH_SLOT);
FBZZ_RWTEX2D_T(float, OutputGTAO, UAV_GTAO_RAW_SLOT);

static const float PI      = 3.14159265f;
static const float HALF_PI = 1.57079633f;

/// @brief 半径のうち、減衰が始まるまでの割合の補数。外側 61.5% で重みを 1 → 0 へ落とす。
/// @see https://github.com/GameTechDev/XeGTAO (XeGTAO.h の XE_GTAO_DEFAULT_FALLOFF_RANGE)
static const float kFalloffRange = 0.615f;
/// @brief 自分自身の画素を拾わないための最小のずらし量 [フル解像度の画素]。
/// @see https://github.com/GameTechDev/XeGTAO (XeGTAO.hlsli の pixelTooCloseThreshold)
static const float kMinSamplePixels = 1.3f;

/// @brief UV の位置の深度をフル解像度で最近傍に読み、その画素中心の視空間位置を返す。
/// @note 補間すると輪郭で手前と奥の深度が混ざり、宙に浮いた点を作って縁に黒い帯が出る。
float3 LoadViewPos(float2 uv, float2 depthSize, float4x4 ndcToView, out float ndcDepth)
{
    const int2   texel    = clamp(int2(uv * depthSize), int2(0, 0), int2(depthSize) - 1);
    const float2 centerUV = (float2(texel) + 0.5f) / depthSize;
    ndcDepth = texDepth.Load(int3(texel, 0)).r;
    const float4 ndc = float4(centerUV.x * 2.0f - 1.0f, 1.0f - centerUV.y * 2.0f, ndcDepth, 1.0f);
    const float4 vs  = mul(ndc, ndcToView);
    return vs.xyz / vs.w;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint2 pixel = id.xy;
    float2 outSize;
    OutputGTAO.GetDimensions(outSize.x, outSize.y);
    if (pixel.x >= (uint)outSize.x || pixel.y >= (uint)outSize.y)
        return;
    if (gtaoIntensity <= 0.0f)
    {
        OutputGTAO[pixel] = 1.0f;
        return;
    }

    float2 depthSize;
    texDepth.GetDimensions(depthSize.x, depthSize.y);

    /// @note NDC → 視空間を 1 回の行列積で済ませる (invViewProjection の後に view)。view は剛体なので w は保たれる。
    const float4x4 ndcToView = mul(invViewProjection, view);

    const float2 uv = (float2(pixel) + 0.5f) / outSize;
    float centerDepth;
    const float3 P = LoadViewPos(uv, depthSize, ndcToView, centerDepth);
    if (IsFarDepth(centerDepth))
    {
        OutputGTAO[pixel] = 1.0f;
        return;
    }

    const int2 centerTexel = clamp(int2(uv * depthSize), int2(0, 0), int2(depthSize) - 1);
    const float3 normalWS = texGBuffer1.Load(int3(centerTexel, 0)).rgb * 2.0f - 1.0f;
    const float3 N = normalize(mul(float4(normalWS, 0.0f), view).xyz);

    const bool  ortho = isOrthographic > 0.5f;
    /// @note 画素から見たカメラの向き。平行投影では全画素で視線が同じ。
    const float3 V = ortho ? float3(0.0f, 0.0f, -1.0f) : normalize(-P);

    /// @note ワールド半径を UV 半径へ投影する。NDC の半幅が UV の 1 なので 0.5 を掛ける。
    const float  radius   = max(gtaoRadius, 1.0e-3f);
    const float  invDepth = ortho ? 1.0f : 1.0f / max(P.z, 1.0e-4f);
    const float2 radiusUV = float2(abs(projection._m00), abs(projection._m11)) * (0.5f * radius * invDepth);
    const float  radiusPixels = radiusUV.x * depthSize.x;
    if (radiusPixels < 1.0f)
    {
        OutputGTAO[pixel] = 1.0f;
        return;
    }

    const float falloffRange = kFalloffRange * radius;
    const float falloffFrom  = radius * (1.0f - kFalloffRange);
    const float falloffMul   = -1.0f / falloffRange;
    const float falloffAdd   = falloffFrom / falloffRange + 1.0f;

    const int   slices = max(gtaoSlices, 1);
    const int   steps  = max(gtaoStepsPerSlice, 1);
    const float minS   = kMinSamplePixels / radiusPixels;

    /// @note スライス角とステップ位置を画素ごと・時間ごとにずらし、固定パターンの縞を TAA と空間ブラーで均す。
    const float sliceNoise = Hash2D(uv + float2(time * 0.1f, time * 0.07f));
    const float stepNoise  = Hash2D(uv.yx * 1.37f + float2(time * 0.05f, time * 0.11f));

    float visibility = 0.0f;
    [loop]
    for (int slice = 0; slice < slices; ++slice)
    {
        const float  phi   = (float(slice) + sliceNoise) * (PI / float(slices));
        const float  cosPhi = cos(phi);
        const float  sinPhi = sin(phi);
        /// @note 視空間 +Y は上、UV の +y は下なので、画面上の向きだけ y を反転する。
        const float2 omega = float2(cosPhi, -sinPhi) * radiusUV;

        const float3 directionVec      = float3(cosPhi, sinPhi, 0.0f);
        const float3 orthoDirectionVec = directionVec - dot(directionVec, V) * V;
        const float3 axisVec           = normalize(cross(orthoDirectionVec, V));
        const float3 projectedNormal   = N - axisVec * dot(N, axisVec);
        const float  projectedLength   = length(projectedNormal);
        if (projectedLength < 1.0e-4f)
            continue;

        const float signNorm = dot(orthoDirectionVec, projectedNormal) >= 0.0f ? 1.0f : -1.0f;
        const float cosNorm  = saturate(dot(projectedNormal, V) / projectedLength);
        const float n        = signNorm * acos(cosNorm);

        /// @note 何も当たらなければ地平は接平面 (法線から ±90°)。そこから上へ持ち上がった分だけ遮蔽になる。
        const float lowHorizonCos0 = cos(n + HALF_PI);
        const float lowHorizonCos1 = cos(n - HALF_PI);
        float horizonCos0 = lowHorizonCos0;
        float horizonCos1 = lowHorizonCos1;

        [loop]
        for (int step = 0; step < steps; ++step)
        {
            /// @note 二乗で中心寄りに密に置く (近い遮蔽ほど効くため)。
            float s = (float(step) + stepNoise) / float(steps);
            s = s * s + minS;
            const float2 offset = omega * s;

            float depth0;
            float depth1;
            const float3 S0 = LoadViewPos(uv + offset, depthSize, ndcToView, depth0);
            const float3 S1 = LoadViewPos(uv - offset, depthSize, ndcToView, depth1);

            const float3 delta0 = S0 - P;
            const float3 delta1 = S1 - P;
            const float  dist0  = length(delta0);
            const float  dist1  = length(delta1);
            float shc0 = dot(delta0 / max(dist0, 1.0e-5f), V);
            float shc1 = dot(delta1 / max(dist1, 1.0e-5f), V);

            /// @note 半径の外側ほど影響を弱める。空の画素は遮蔽物ではない。
            const float weight0 = IsFarDepth(depth0) ? 0.0f : saturate(dist0 * falloffMul + falloffAdd);
            const float weight1 = IsFarDepth(depth1) ? 0.0f : saturate(dist1 * falloffMul + falloffAdd);
            shc0 = lerp(lowHorizonCos0, shc0, weight0);
            shc1 = lerp(lowHorizonCos1, shc1, weight1);

            horizonCos0 = max(horizonCos0, shc0);
            horizonCos1 = max(horizonCos1, shc1);
        }

        /// @note +omega 側の地平が h1、-omega 側が h0。法線の半球 (n ± 90°) の外へは出さない。
        float h0 = -acos(clamp(horizonCos1, -1.0f, 1.0f));
        float h1 =  acos(clamp(horizonCos0, -1.0f, 1.0f));
        h0 = n + max(h0 - n, -HALF_PI);
        h1 = n + min(h1 - n,  HALF_PI);

        /// @note 余弦重みの可視度の解析積分 (Jimenez et al. 2016, Eq. 10)。
        const float sinN  = sin(n);
        const float iarc0 = (cosNorm + 2.0f * h0 * sinN - cos(2.0f * h0 - n)) * 0.25f;
        const float iarc1 = (cosNorm + 2.0f * h1 * sinN - cos(2.0f * h1 - n)) * 0.25f;
        visibility += projectedLength * (iarc0 + iarc1);
    }

    visibility = saturate(visibility / float(slices));
    OutputGTAO[pixel] = lerp(1.0f, visibility, saturate(gtaoIntensity));
}
