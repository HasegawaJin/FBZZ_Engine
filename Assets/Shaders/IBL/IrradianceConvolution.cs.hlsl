// FBZZ Engine
// IBL/IrradianceConvolution.cs.hlsl | IBL Baking
// Environment Cubemap を Lambertian 拡散 IBL 用 Irradiance Cubemap へ畳み込む CS
//
// 設計:
//   - 半球上の Riemann sum で数値積分する (重要度サンプリング不要 — 拡散は低周波)
//   - 出力解像度は 32×32 で十分 (拡散 IBL は高周波情報を必要としない)
//   - 1 Dispatch = 1 面。Editor 側が 6 回ループ。
//   - 入力: Environment Cubemap SRV (TextureCube)
//   - 出力: Irradiance Cubemap (RWTexture2DArray — 1 面分 UAV)
//
// ディスパッチ: Dispatch(ceil(outSize/8), ceil(outSize/8), 1) を 6 回
//
// CB b0 — CbIblFace
//   faceIndex  : 処理対象の面 (0〜5)
//   textureSize: 出力解像度 (通常 32)

#include "IBL/IBLCommon.hlsli"
#include "Common/BindlessIndices.hlsli"

cbuffer CbIblFace : register(b0)
{
    uint g_faceIndex;
    uint g_textureSize;
    uint g_phiSteps;    // 半球積分の φ 分割数 (0 ならデフォルト 200)
    uint g_thetaSteps;  // 半球積分の θ 分割数 (0 ならデフォルト 50)
};

FBZZ_TEXCUBE_T(float4, g_envMap, 0); // Environment Cubemap SRV
FBZZ_RWTEX2DARRAY_T(float4, g_output, 0); // Irradiance Cubemap UAV (1 面分)
SamplerState                 g_sampler : register(s0); // LinearWrap

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= g_textureSize || dtid.y >= g_textureSize) return;

    float2 uv = (dtid.xy + 0.5f) / float(g_textureSize);
    float3 N  = CubeTexelToDirection(g_faceIndex, uv);

    // N に垂直な接線基底 (TBN)
    float3 up    = abs(N.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 right = normalize(cross(up, N));
    float3 fwd   = cross(N, right);

    // 半球上の Riemann sum。サンプル数は CB (g_phiSteps/g_thetaSteps) で制御する。
    // WHY: Editor の DDS ベイクは高品質 (200×50=10000)、空連動 IBL の実行時ベイクは低品質
    //      (例 64×16=1024) を渡して畳み込みコストを約 1/10 に抑える。分散低減 mip を併用するため
    //      少サンプルでも 32^2 拡散 irradiance の品質を保てる。0 の場合は従来デフォルトにフォールバック。
    const uint PHI_STEPS   = g_phiSteps   > 0u ? g_phiSteps   : 200u;
    const uint THETA_STEPS = g_thetaSteps > 0u ? g_thetaSteps : 50u;

    // 各積分サンプルが覆う立体角に近い入力mipを使い、HDR高輝度テクセルの孤立ヒットを防ぐ。
    // WHY: mip0を直接読むと太陽のような微小光源を一部の出力テクセルだけが拾い、
    //      diffuse IBL に白いfireflyが焼き込まれる。事前生成mipは平均輝度を保ったまま分散を下げる。
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

            // 球面座標 → 接線空間
            float3 local = float3(
                sin(theta) * cos(phi),
                sin(theta) * sin(phi),
                cos(theta));

            // 接線空間 → ワールド空間
            float3 worldDir = right * local.x + fwd * local.y + N * local.z;

            float3 radiance = max(g_envMap.SampleLevel(g_sampler, worldDir, sourceMip).rgb, 0.0f);

            // Diffuse IBL から太陽などの直接光ピークだけを除去する。
            // WHY: 高輝度ピークをirradianceへ残すとDirectionalLightと二重計上され、
            //      影がIBL側の太陽光で白くなる。色相を維持して線形輝度のみ上限を設ける。
            static const float DIFFUSE_RADIANCE_LIMIT = 4.0f;
            float luminance = dot(radiance, float3(0.2126f, 0.7152f, 0.0722f));
            radiance *= min(1.0f, DIFFUSE_RADIANCE_LIMIT / max(luminance, EPSILON));

            // Lambertian 余弦加重 (cos(θ) sin(θ) dθdφ の立体角補正)
            irradiance += radiance * cos(theta) * sin(theta);
            nSamples += 1.0f;
        }
    }

    // 積分値に正規化定数 π / N を掛ける
    irradiance = PI * irradiance / nSamples;

    g_output[uint3(dtid.x, dtid.y, 0)] = float4(irradiance, 1.0f);
}
