// FBZZ Engine
// PostProcess/AmbientOcclusion/BRDFIntegration.cs.hlsl | PostProcess
// BRDF 積分テーブル (LUT) をスタートアップ時に GPU でベイクする Compute Shader
//
// 出力テクスチャ (UAV): RWTexture2D<float4> at u0 (UAV_BRDF_LUT)
//   UV (x=NdotV 0→1, y=roughness 0→1) → (scale=brdf.r, bias=brdf.g)
//
// アルゴリズム:
//   UE4 / Epic Games の split-sum 近似に基づく BRDF 積分。
//   Hammersley quasi-random シーケンスで N=1024 サンプルを生成し、
//   GGX 重要度サンプリング (IS) でハーフベクトル H を引き、
//   Cook-Torrance BRDF を数値積分する。
//
//   積分式:
//     ∫ f(l,v) cos(θ_l) dl ≈ (1/N) Σ G(v,h) * VdotH / (NdotH * NdotV)  × [F0 + (1-F0)*(1-VdotH)^5]
//   これを F0×scale + bias に分解して LUT に格納する。
//
// ディスパッチ: Dispatch(LUT_SIZE/8, LUT_SIZE/8, 1)
//   推奨 LUT_SIZE = 512 (精度と速度のバランス)
//
// 参考: Brian Karis, "Real Shading in Unreal Engine 4" (SIGGRAPH 2013 Course Notes)
#ifndef BRDF_INTEGRATION_CS_HLSL
#define BRDF_INTEGRATION_CS_HLSL

#include "Common/Binding.hlsli"
#include "Common/Math.hlsli"

// BRDF LUT 出力 UAV
// WHY: 実行時の汎用 ComputeTexture は R16G16B16A16_FLOAT のため、typed UAV の
//      コンポーネント数を一致させる。利用側は RG の scale/bias のみ読む。
RWTexture2D<float4> outBRDFLut : register(UAV_BRDF_LUT);  // u0

// =========================================================================
// Hammersley 低差異列
//
// i 番目のサンプル (0 ≤ i < N) を [0,1)^2 に写像する。
// x 成分は等差、y 成分は Van der Corput 基数2列 (ビット反転)。
// GGX 重要度サンプリングの入力として使う。
//   i: サンプルインデックス
//   N: 総サンプル数
// =========================================================================
float2 Hammersley(uint i, uint N)
{
    // Van der Corput: ビットを反転して [0,1) に正規化
    uint bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    float vdc = (float)bits * 2.3283064365386963e-10f; // / 0x100000000

    return float2((float)i / (float)N, vdc);
}

// =========================================================================
// GGX 重要度サンプリング
//
// 入力の乱数 Xi から GGX 分布に従うハーフベクトル H (接線空間) を生成する。
// 接線空間: N=(0,0,1), T=(1,0,0), B=(0,1,0)
//   Xi       : [0,1)^2 の一様乱数 (Hammersley 等で生成)
//   roughness: 粗さ [0, 1]
// 戻り値: タンジェント空間のハーフベクトル H (正規化済み)
// =========================================================================
float3 ImportanceSampleGGX(float2 Xi, float roughness)
{
    float a  = roughness * roughness; // α = roughness^2

    // GGX の CDF 逆関数でθ(仰角), φ(方位角) を決定
    float phi      = TWO_PI * Xi.x;
    float cosTheta = sqrt((1.0f - Xi.y) / (1.0f + (a * a - 1.0f) * Xi.y + EPSILON));
    float sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));

    // 球面座標 → デカルト座標 (接線空間)
    return float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
}

// =========================================================================
// IBL 用 Smith-Schlick 幾何減衰 (k = roughness^2 / 2)
//
// BRDF LUT のベイクでは IBL 用リマッピングを使う。
// (ダイレクトライティング用の k=(r+1)^2/8 とは異なる)
// =========================================================================
float G_SchlickGGX_IBL_Bake(float NdotX, float roughness)
{
    float k = roughness * roughness * 0.5f;
    return NdotX / (NdotX * (1.0f - k) + k + EPSILON);
}

float G_Smith_IBL_Bake(float NdotV, float NdotL, float roughness)
{
    return G_SchlickGGX_IBL_Bake(NdotV, roughness) * G_SchlickGGX_IBL_Bake(NdotL, roughness);
}

// =========================================================================
// BRDF 積分の数値計算
//
// NdotV と roughness を受け取り、split-sum の (scale, bias) ペアを返す。
// scale: F0 に掛ける係数
// bias : F0 によらない定数項
//
// 積分: ∫ G(v,l) * VdotH / (NdotH * NdotV) * [(1-(1-VdotH)^5)] dl  → scale
//        ∫ G(v,l) * VdotH / (NdotH * NdotV) * [(1-VdotH)^5] dl      → bias
// =========================================================================
float2 IntegrateBRDF(float NdotV, float roughness)
{
    // V を接線空間に固定: N=(0,0,1), V を NdotV で決定
    float3 V;
    V.x = sqrt(max(0.0f, 1.0f - NdotV * NdotV)); // sinθ
    V.y = 0.0f;
    V.z = NdotV;                                   // cosθ = NdotV

    float3 N = float3(0.0f, 0.0f, 1.0f); // 接線空間での法線

    static const uint SAMPLE_COUNT = 1024u;
    float scale = 0.0f;
    float bias  = 0.0f;

    for (uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        // Hammersley シーケンスでサンプル点を生成
        float2 Xi = Hammersley(i, SAMPLE_COUNT);

        // GGX IS でハーフベクトルを生成し、対応するライト方向 L を計算
        float3 H = ImportanceSampleGGX(Xi, roughness);
        float3 L = normalize(2.0f * dot(V, H) * H - V);

        float NdotL = saturate(L.z);   // dot(N, L) in tangent space
        float NdotH = saturate(H.z);   // dot(N, H) in tangent space
        float VdotH = saturate(dot(V, H));

        if (NdotL > 0.0f)
        {
            // 幾何減衰 G (IBL 用リマッピング)
            float G      = G_Smith_IBL_Bake(NdotV, NdotL, roughness);
            // 重要度サンプリングの重みを含むサンプル寄与
            // G_vis = G * VdotH / (NdotH * NdotV)
            float G_Vis  = G * VdotH / (NdotH * NdotV + EPSILON);

            // Schlick フレネルを F0=1 で展開: F = F0 + (1-F0) * (1-VdotH)^5
            // scale 項: F0 に掛かる部分 = G_Vis * (1 - (1-VdotH)^5)
            // bias  項: 定数部分        = G_Vis * (1-VdotH)^5
            float Fc = Pow5(1.0f - VdotH);
            scale += G_Vis * (1.0f - Fc);
            bias  += G_Vis * Fc;
        }
    }

    return float2(scale, bias) / (float)SAMPLE_COUNT;
}

// =========================================================================
// CSMain — BRDF LUT ベイクのメインカーネル
//
// スレッドグループ: [8, 8, 1]
// ディスパッチ   : (LUT_WIDTH/8, LUT_HEIGHT/8, 1)
// 出力テクスチャ : UV(x=NdotV, y=roughness) → float2(scale, bias)
// =========================================================================
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    // 出力テクスチャのサイズを取得
    uint width, height;
    outBRDFLut.GetDimensions(width, height);

    // テクセル中心を [0, 1] に正規化
    // x 軸: NdotV (0=グレージング角, 1=真正面)
    // y 軸: roughness (0=鏡面, 1=完全拡散)
    float NdotV    = (id.x + 0.5f) / (float)width;
    float roughness = (id.y + 0.5f) / (float)height;

    // NdotV=0 は数値的に不安定なため最小値でクランプ
    NdotV = max(NdotV, EPSILON);

    // BRDF 積分を計算して LUT に書き込む
    float2 brdf = IntegrateBRDF(NdotV, roughness);
    outBRDFLut[id.xy] = float4(brdf, 0.0f, 1.0f);
}

#endif // BRDF_INTEGRATION_CS_HLSL
