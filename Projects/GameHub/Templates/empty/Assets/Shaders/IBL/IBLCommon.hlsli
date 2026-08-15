// FBZZ Engine
// IBL/IBLCommon.hlsli | IBL Baking
// IBL ベイク用 CS 間の共通関数群
//
// 含まれる機能:
//   - CubeTexelToDirection  : Cubemap テクセル座標 → ワールド方向ベクトル
//   - Hammersley            : 低差異列 (GGX IS 用)
//   - ImportanceSampleGGX   : GGX 重要度サンプリング (接線空間 H を返す)
//   - TangentToWorld        : TBN 行列で接線空間 → ワールド空間変換
//
// 依存: Common/Math.hlsli (PI / TWO_PI / HALF_PI / EPSILON)
#ifndef IBL_COMMON_HLSLI
#define IBL_COMMON_HLSLI

#include "Common/Math.hlsli"

// 1 / (2π) — Math.hlsli に INV_TWO_PI がないため定義
static const float INV_TWO_PI = 0.15915494309189f;

// Cubemap face index と正規化テクセル UV から世界空間方向ベクトルを返す。
//
// DX11 の Cubemap face 順序 (D3D11_TEXTURECUBE_FACE):
//   0: +X  1: -X  2: +Y  3: -Y  4: +Z  5: -Z
//
// uv は [0, 1] 範囲。テクセル中心を渡すこと。
float3 CubeTexelToDirection(uint face, float2 uv)
{
    // [0,1] → [-1,1] に変換
    float2 st = uv * 2.0f - 1.0f;

    switch (face)
    {
        case 0: return normalize(float3( 1.0f, -st.y, -st.x)); // +X
        case 1: return normalize(float3(-1.0f, -st.y,  st.x)); // -X
        case 2: return normalize(float3( st.x,  1.0f,  st.y)); // +Y
        case 3: return normalize(float3( st.x, -1.0f, -st.y)); // -Y
        case 4: return normalize(float3( st.x, -st.y,  1.0f)); // +Z
        default: return normalize(float3(-st.x, -st.y, -1.0f)); // -Z
    }
}

// 世界方向ベクトル → Equirectangular UV
// 返り値: (0,1)^2
float2 DirToEquirectUV(float3 dir)
{
    // φ: [-π, π] → [0, 1] (水平方向)
    // θ: [-π/2, π/2] → [0, 1] (垂直方向)
    float phi   = atan2(dir.z, dir.x);
    float theta = asin(clamp(dir.y, -1.0f, 1.0f));
    return float2(phi * INV_TWO_PI + 0.5f, theta * INV_PI + 0.5f);
}

// Hammersley 低差異列 (Van der Corput 基数 2)
// i: サンプルインデックス [0, N)
// 返り値: [0,1)^2
float2 Hammersley(uint i, uint N)
{
    uint bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    float vdc = float(bits) * 2.3283064365386963e-10f; // 1 / 2^32
    return float2(float(i) / float(N), vdc);
}

// GGX 分布による重要度サンプリング (接線空間)
//
// xi      : Hammersley() が返す [0,1)^2 の乱数
// roughness: 粗さ [0, 1]
// 返り値  : 接線空間のハーフベクトル H (N=(0,0,1), T=(1,0,0), B=(0,1,0))
float3 ImportanceSampleGGX(float2 xi, float roughness)
{
    float a  = roughness * roughness; // α = roughness²
    float phi      = TWO_PI * xi.x;
    float cosTheta = sqrt((1.0f - xi.y) / max(1.0f + (a * a - 1.0f) * xi.y, EPSILON));
    float sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));
    return float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
}

// 接線空間ベクトルを世界空間に変換する TBN 行列合成
//
// N     : ワールド法線 (正規化済み)
// local : 接線空間のベクトル
// 返り値: ワールド空間のベクトル (正規化済み)
float3 TangentToWorld(float3 N, float3 local)
{
    // N に垂直な接線基底を構築 (Frisvad/Duff 法)
    float3 up    = abs(N.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 right = normalize(cross(up, N));
    float3 fwd   = cross(N, right);
    return normalize(right * local.x + fwd * local.y + N * local.z);
}

#endif // IBL_COMMON_HLSLI
