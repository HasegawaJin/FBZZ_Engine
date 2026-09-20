/// @file    LightProbeProject.cs.hlsl
/// @brief   プローブ位置で描いた 6 面を L2 球面調和へ射影し、放射照度の多項式形で Light Probe Volume へ書く。
/// @author  Hasegawa Jin
/// @date    2026-09-19
/// @note 1 ディスパッチ = 1 プローブ (64 スレッド 1 グループ)。面は深度付き 2D RT で、何も描かれなかった画素 (深度 = クリア値) は IBL 環境キューブで埋める。
/// @note 出力の並びは Rendering/LightProbeGI.hlsli と一致させること。SHC.w はプローブの有効度 (壁に埋まっていれば 0) で、LightProbeDilate.cs.hlsl が読む。
/// @see Docs/design/light-probe-gi.md
#include "Common/Binding.hlsli"
#include "Common/Math.hlsli"
#include "Common/BindlessIndices.hlsli"
#include "Platform/Backend.hlsli"

/// @note 面 f の色は t(f)、深度は t(6 + f)、表裏は t(20 + f)、環境キューブは t16。C++ の LightProbeBakePass と一致させること。
#define LIGHT_PROBE_FACE_COLOR_SLOT 0
#define LIGHT_PROBE_FACE_DEPTH_SLOT 6
#define LIGHT_PROBE_ENVIRONMENT_SLOT 16
#define LIGHT_PROBE_FACE_FACING_SLOT 20
#define LIGHT_PROBE_THREADS 64

/// @note LAYOUT: LightProbeBakePass.cpp の LightProbeProjectCB と一致させること (448 bytes)。
cbuffer LightProbeProjectConstants : register(b0)
{
    float4x4 faceInvViewProj[6];   ///< 面ごとの逆 ViewProjection
    float3   probePos;             ///< プローブ位置 [world]
    uint     faceSize;             ///< 面の 1 辺 [px]
    uint3    probeCoord;           ///< 書き込むプローブの格子座標
    uint     gridZ;                ///< Z 方向のプローブ数 (係数区画の厚み)
    float    skyRadianceLimit;     ///< 空の輝度上限。IrradianceConvolution の DIFFUSE_RADIANCE_LIMIT と揃える
    float    surfaceRadianceLimit; ///< 面の輝度上限。発光面の一点が全体を白くするのを防ぐ
    float    environmentMip;       ///< 環境キューブを引く mip
    uint     hasEnvironment;       ///< 0 なら空は黒
    float    deringing;            ///< リンギング抑制の強さ [0,1]
    uint     hasFacing;            ///< 0 なら表裏を見ずに常に有効
    float    backfaceLimit;        ///< 裏面の立体角の割合がこれを超えたら無効 [0,1]
    float    _pad;
};

FBZZ_TEXCUBE(gEnvironment, LIGHT_PROBE_ENVIRONMENT_SLOT);
FBZZ_RWTEX3D_T(float4, gProbeSH, UAV_OUTPUT_SLOT);
SamplerState sampLinearClamp : register(SAMPLER_LINEAR_CLAMP);

groupshared float3 gsSH[9][LIGHT_PROBE_THREADS];
groupshared float  gsWeight[LIGHT_PROBE_THREADS];
groupshared float  gsBackface[LIGHT_PROBE_THREADS];

/// @brief 実数 L2 SH の 9 基底。
/// @see https://cseweb.ucsd.edu/~ravir/papers/envmap/envmap.pdf Ramamoorthi & Hanrahan, "An Efficient Representation for Irradiance Environment Maps", Eq. 3
void EvaluateSHBasis(float3 d, out float basis[9])
{
    basis[0] = 0.282095f;
    basis[1] = 0.488603f * d.y;
    basis[2] = 0.488603f * d.z;
    basis[3] = 0.488603f * d.x;
    basis[4] = 1.092548f * d.x * d.y;
    basis[5] = 1.092548f * d.y * d.z;
    basis[6] = 0.315392f * (3.0f * d.z * d.z - 1.0f);
    basis[7] = 1.092548f * d.x * d.z;
    basis[8] = 0.546274f * (d.x * d.x - d.y * d.y);
}

float Luminance(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }

float3 ClampLuminance(float3 c, float limit)
{
    return c * min(1.0f, limit / max(Luminance(c), 1e-6f));
}

[numthreads(LIGHT_PROBE_THREADS, 1, 1)]
void CSMain(uint tid : SV_GroupIndex)
{
    float3 sh[9];
    [unroll] for (uint i = 0u; i < 9u; ++i) sh[i] = 0.0f;
    float weightSum = 0.0f;
    float backfaceSum = 0.0f;

    const uint texelsPerFace = faceSize * faceSize;
    const uint texelCount    = texelsPerFace * 6u;
    const float invSize      = 1.0f / (float)faceSize;

    for (uint index = tid; index < texelCount; index += LIGHT_PROBE_THREADS) {
        const uint face = index / texelsPerFace;
        const uint local = index - face * texelsPerFace;
        const uint2 texel = uint2(local % faceSize, local / faceSize);

        /// @note 面は 90° の正方形なので NDC がそのまま接平面上の座標になる。
        const float2 ndc = float2((texel.x + 0.5f) * invSize * 2.0f - 1.0f,
                                  1.0f - (texel.y + 0.5f) * invSize * 2.0f);
        const float4 world = mul(float4(ndc, 0.5f, 1.0f), faceInvViewProj[face]);
        const float3 dir = normalize(world.xyz / world.w - probePos);

        /// @note texel の立体角は (1 + u^2 + v^2)^(-3/2) に比例する。総和で 4π へ正規化するので定数は省く。
        /// @see http://www.rorydriscoll.com/2012/01/15/cubemap-texel-solid-angle/ Driscoll, "Cubemap Texel Solid Angle"
        const float weight = pow(1.0f + dot(ndc, ndc), -1.5f);

        Texture2D<float4> colorTex = ResourceDescriptorHeap[NonUniformResourceIndex(FbzzPixelSlot(LIGHT_PROBE_FACE_COLOR_SLOT + face))];
        Texture2D<float>  depthTex = ResourceDescriptorHeap[NonUniformResourceIndex(FbzzPixelSlot(LIGHT_PROBE_FACE_DEPTH_SLOT + face))];
        const float depth = depthTex.Load(int3(texel, 0));
        /// @note 表裏は別の面 RT (カリング無しで描いた R = 表なら 1) に入っている。裏面が見える = プローブが閉じた形状の中にいる。
        if (hasFacing != 0u) {
            Texture2D<float4> facingTex = ResourceDescriptorHeap[NonUniformResourceIndex(FbzzPixelSlot(LIGHT_PROBE_FACE_FACING_SLOT + face))];
            backfaceSum += (facingTex.Load(int3(texel, 0)).r < 0.5f ? 1.0f : 0.0f) * weight;
        }

        float3 radiance;
        if (depth >= 0.99999f) {
            radiance = hasEnvironment != 0u
                ? ClampLuminance(max(gEnvironment.SampleLevel(sampLinearClamp, dir, environmentMip).rgb, 0.0f),
                                 skyRadianceLimit)
                : 0.0f;
        } else {
            radiance = ClampLuminance(max(colorTex.Load(int3(texel, 0)).rgb, 0.0f), surfaceRadianceLimit);
        }

        float basis[9];
        EvaluateSHBasis(dir, basis);
        [unroll] for (uint k = 0u; k < 9u; ++k) sh[k] += radiance * (basis[k] * weight);
        weightSum += weight;
    }

    [unroll] for (uint j = 0u; j < 9u; ++j) gsSH[j][tid] = sh[j];
    gsWeight[tid] = weightSum;
    gsBackface[tid] = backfaceSum;
    GroupMemoryBarrierWithGroupSync();

    [unroll] for (uint stride = LIGHT_PROBE_THREADS / 2u; stride > 0u; stride >>= 1u) {
        if (tid < stride) {
            [unroll] for (uint k = 0u; k < 9u; ++k) gsSH[k][tid] += gsSH[k][tid + stride];
            gsWeight[tid] += gsWeight[tid + stride];
            gsBackface[tid] += gsBackface[tid + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (tid >= 7u) return;

    /// @note 立体角を 4π へ正規化し、余弦ローブの畳み込み Â_l / π (1, 2/3, 1/4) を焼き込む。一様な放射輝度 L が L を返す単位になり、irradiance キューブと揃う。
    /// @see https://cseweb.ucsd.edu/~ravir/papers/envmap/envmap.pdf Ramamoorthi & Hanrahan, Eq. 8-9 (Â_0 = π, Â_1 = 2π/3, Â_2 = π/4)
    const float norm = 4.0f * PI / max(gsWeight[0], 1e-6f);
    /// @note リンギング抑制: 帯域ごとに Hann 窓 w_l = (1 + cos(π l / 3)) / 2 (0.75, 0.25) を deringing で掛け、強い日だまりの反対側に出る負の輪を弱める。
    /// @see https://www.ppsloan.org/publications/StupidSH36.pdf Sloan, "Stupid Spherical Harmonics (SH) Tricks", §4 Windowing
    const float window1 = lerp(1.0f, 0.75f, saturate(deringing));
    const float window2 = lerp(1.0f, 0.25f, saturate(deringing));
    float3 c[9];
    [unroll] for (uint k = 0u; k < 9u; ++k) {
        const float band = k == 0u ? 1.0f : (k < 4u ? 2.0f / 3.0f * window1 : 0.25f * window2);
        c[k] = gsSH[k][0] * (norm * band);
    }
    /// @note 裏面の割合が上限を超えたプローブは無効 (0)。上限の手前 0.1 から滑らかに落とす。
    const float backfaceRatio = gsBackface[0] / max(gsWeight[0], 1e-6f);
    const float validity = hasFacing != 0u
        ? 1.0f - smoothstep(max(backfaceLimit - 0.1f, 0.0f), backfaceLimit, backfaceRatio)
        : 1.0f;

    /// @note 評価を dot 2 回 + 1 項で済む多項式形へ並べ替える。A·(n,1) + B·(xy, yz, zz, zx) + C·(x^2 - y^2)。
    /// @see https://www.ppsloan.org/publications/StupidSH36.pdf Sloan, "Stupid Spherical Harmonics (SH) Tricks", Appendix A10
    const float3 A0 = 0.488603f * c[3];
    const float3 A1 = 0.488603f * c[1];
    const float3 A2 = 0.488603f * c[2];
    const float3 A3 = 0.282095f * c[0] - 0.315392f * c[6];
    const float3 B0 = 1.092548f * c[4];
    const float3 B1 = 1.092548f * c[5];
    const float3 B2 = 3.0f * 0.315392f * c[6];
    const float3 B3 = 1.092548f * c[7];
    const float3 C  = 0.546274f * c[8];

    float4 value;
    switch (tid) {
    case 0u: value = float4(A0.r, A1.r, A2.r, A3.r); break;
    case 1u: value = float4(A0.g, A1.g, A2.g, A3.g); break;
    case 2u: value = float4(A0.b, A1.b, A2.b, A3.b); break;
    case 3u: value = float4(B0.r, B1.r, B2.r, B3.r); break;
    case 4u: value = float4(B0.g, B1.g, B2.g, B3.g); break;
    case 5u: value = float4(B0.b, B1.b, B2.b, B3.b); break;
    default: value = float4(C, validity); break;
    }
    gProbeSH[uint3(probeCoord.xy, probeCoord.z + gridZ * tid)] = value;
}
