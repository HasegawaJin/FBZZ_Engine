/// @file    IrradianceConvolution.cs.hlsl
/// @brief   Environment Cubemap を Lambertian 拡散 IBL 用 Irradiance Cubemap へ畳み込む CS
/// @author  Hasegawa Jin
/// @date    2026-06-23
///
/// @note 設計:
/// @note - 半球上の Riemann sum で数値積分する (重要度サンプリング不要 — 拡散は低周波)
/// @note - 出力解像度は 32×32 で十分 (拡散 IBL は高周波情報を必要としない)
/// @note - 1 Dispatch = 1 面。Editor 側が 6 回ループ。
/// @note - 入力: Environment Cubemap SRV (TextureCube)
/// @note - 出力: Irradiance Cubemap (RWTexture2DArray — 1 面分 UAV)
///
/// @note ディスパッチ: Dispatch(ceil(outSize/8), ceil(outSize/8), 1) を 6 回
///
/// @note CB b0 — CbIblFace
/// @note faceIndex  : 処理対象の面 (0〜5)
/// @note textureSize: 出力解像度 (通常 32)

#include "IBL/IBLCommon.hlsli"
#include "Common/BindlessIndices.hlsli"

cbuffer CbIblFace : register(FBZZ_REGISTER(b, CB_IBL_FACE_SLOT))
{
    uint g_faceIndex;
    uint g_textureSize;
    uint g_phiSteps;    /// @note 半球積分の φ 分割数 (0 ならデフォルト 200)
    uint g_thetaSteps;  /// @note 半球積分の θ 分割数 (0 ならデフォルト 50)
};

FBZZ_TEXCUBE_T(float4, g_envMap, SRV_IBL_SOURCE_SLOT); /// @note Environment Cubemap SRV
FBZZ_RWTEX2DARRAY_T(float4, g_output, UAV_IBL_FACE_SLOT); /// @note Irradiance Cubemap UAV (1 面分)
SamplerState                 g_sampler : register(SAMPLER_DEFAULT); /// @note LinearWrap

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= g_textureSize || dtid.y >= g_textureSize) return;

    float2 uv = (dtid.xy + 0.5f) / float(g_textureSize);
    float3 N  = CubeTexelToDirection(g_faceIndex, uv);

    /// @note N に垂直な接線基底 (TBN)
    float3 up    = abs(N.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 right = normalize(cross(up, N));
    float3 fwd   = cross(N, right);

    /// @note 半球上の Riemann sum。サンプル数は CB (g_phiSteps/g_thetaSteps) で制御する。
    /// @note Editor の DDS ベイクは高品質 (200×50=10000)、空連動 IBL の実行時ベイクは低品質
    /// @note (例 64×16=1024) を渡して畳み込みコストを約 1/10 に抑える。分散低減 mip を併用するため
    /// @note 少サンプルでも 32^2 拡散 irradiance の品質を保てる。0 の場合は従来デフォルトにフォールバック。
    const uint PHI_STEPS   = g_phiSteps   > 0u ? g_phiSteps   : 200u;
    const uint THETA_STEPS = g_thetaSteps > 0u ? g_thetaSteps : 50u;

    /// @note 各積分サンプルが覆う立体角に近い入力mipを使い、HDR高輝度テクセルの孤立ヒットを防ぐ。
    /// @note mip0を直接読むと太陽のような微小光源を一部の出力テクセルだけが拾い、
    /// @note diffuse IBL に白いfireflyが焼き込まれる。事前生成mipは平均輝度を保ったまま分散を下げる。
    uint envWidth;
    uint envHeight;
    uint envLevels;
    g_envMap.GetDimensions(0, envWidth, envHeight, envLevels);
    float sampleGridResolution = sqrt(float(PHI_STEPS * THETA_STEPS));
    float sourceMip = clamp(log2(float(envWidth) / sampleGridResolution),
                            0.0f, float(envLevels - 1u));

    float3 irradiance = float3(0.0f, 0.0f, 0.0f);
    float  nSamples   = 0.0f;

    for (uint pi = 0u; pi < PHI_STEPS; ++pi)
    {
        float phi = (float(pi) + 0.5f) / float(PHI_STEPS) * TWO_PI;

        for (uint ti = 0u; ti < THETA_STEPS; ++ti)
        {
            float theta = (float(ti) + 0.5f) / float(THETA_STEPS) * HALF_PI;

            /// @note 球面座標 → 接線空間
            float3 local = float3(
                sin(theta) * cos(phi),
                sin(theta) * sin(phi),
                cos(theta));

            /// @note 接線空間 → ワールド空間
            float3 worldDir = right * local.x + fwd * local.y + N * local.z;

            float3 radiance = max(g_envMap.SampleLevel(g_sampler, worldDir, sourceMip).rgb, 0.0f);

            /// @note Diffuse IBL から太陽などの直接光ピークだけを除去する。
            /// @note 高輝度ピークをirradianceへ残すとDirectionalLightと二重計上され、
            /// @note 影がIBL側の太陽光で白くなる。色相を維持して線形輝度のみ上限を設ける。
            static const float DIFFUSE_RADIANCE_LIMIT = 4.0f;
            float luminance = dot(radiance, float3(0.2126f, 0.7152f, 0.0722f));
            radiance *= min(1.0f, DIFFUSE_RADIANCE_LIMIT / max(luminance, EPSILON));

            /// @note Lambertian 余弦加重 (cos(θ) sin(θ) dθdφ の立体角補正)
            irradiance += radiance * cos(theta) * sin(theta);
            nSamples += 1.0f;
        }
    }

    /// @note 積分値に正規化定数 π / N を掛ける
    irradiance = PI * irradiance / nSamples;

    g_output[uint3(dtid.x, dtid.y, 0)] = float4(irradiance, 1.0f);
}
