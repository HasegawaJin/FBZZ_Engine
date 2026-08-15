// FBZZ Engine
// IBL/PrefilteredEnvMap.cs.hlsl | IBL Baking
// GGX 重要度サンプリングで Environment Cubemap を Prefiltered Specular Cubemap に変換する CS
//
// 設計 (UE4 Split-Sum 方式):
//   - roughness に対応する各 mip を Editor 側が Dispatch で焼く (mip × 面 = 最大 6×mipCount 回)
//   - N=R=V 近似: 視線方向が不明なため法線方向を反射方向として代用する。
//     テーリングアーティファクトが生じるが実用上許容される品質。
//   - mip 選択バイアス低減: サンプルごとの PDF から最適 mip を計算してフィルタリングする。
//   - 入力: Environment Cubemap SRV (TextureCube, 事前に GenerateMips 済み)
//   - 出力: Prefiltered Cubemap の 1 面 × 1 mip 分の UAV
//
// ディスパッチ: Dispatch(ceil(mipSize/8), ceil(mipSize/8), 1) を face×mip 回
//
// CB b0 — CbPrefilter
//   faceIndex   : 処理対象の面 (0〜5)
//   outputSize  : この mip の 1 辺サイズ (px)
//   roughness   : この mip に対応する粗さ [0, 1]
//   sampleCount : GGX IS サンプル数 (通常 1024)
//   envMipCount : env cubemap の mip 数 (PDF → mip 計算に使用)

#include "IBL/IBLCommon.hlsli"
#include "Rendering/BRDF.hlsli"

cbuffer CbPrefilter : register(b0)
{
    uint  g_faceIndex;
    uint  g_outputSize;
    float g_roughness;
    uint  g_sampleCount;
    uint  g_envMipCount;
    uint  _pad0;
    uint  _pad1;
    uint  _pad2;
};

TextureCube<float4>          g_envMap  : register(t0); // Environment Cubemap SRV (with mips)
RWTexture2DArray<float4>     g_output  : register(u0); // Prefiltered Cubemap UAV (1 面×1 mip)
SamplerState                 g_sampler : register(s0); // LinearWrap

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= g_outputSize || dtid.y >= g_outputSize) return;

    float2 uv = (dtid.xy + 0.5f) / float(g_outputSize);
    float3 N  = CubeTexelToDirection(g_faceIndex, uv);

    // N = R = V の近似 (視線方向が不明)
    float3 V = N;

    float3 prefilteredColor = float3(0.0f, 0.0f, 0.0f);
    float  totalWeight      = 0.0f;

    uint envWidth;
    uint envHeight;
    uint envLevels;
    g_envMap.GetDimensions(0, envWidth, envHeight, envLevels);

    for (uint i = 0u; i < g_sampleCount; ++i)
    {
        float2 xi = Hammersley(i, g_sampleCount);

        // GGX IS でハーフベクトル H を生成し、対応するライト方向 L を求める
        float3 H_local = ImportanceSampleGGX(xi, g_roughness);
        float3 H       = TangentToWorld(N, H_local);
        float3 L       = normalize(reflect(-V, H));

        float NdotL = saturate(dot(N, L));
        if (NdotL <= 0.0f) continue;

        // サンプルの PDF から最適 mip を選ぶ (Chetan Jaggi バイアス低減)
        // PDF = D * NdotH / (4 * VdotH)  ここで D=D_GGX
        float NdotH = saturate(dot(N, H));
        float VdotH = saturate(dot(V, H));

        // D_GGX は BRDF.hlsli に定義済み
        float D      = D_GGX(NdotH, g_roughness);
        float pdf    = max(D * NdotH / max(4.0f * VdotH, EPSILON), EPSILON);

        // テクセル立体角 (サンプル立体角が大きいほど高 mip を選ぶ)
        // 入力環境 mip0 のテクセル立体角を使う。
        // WHY: 出力 mip のサイズを使うと roughness が高いほど LOD が逆に低下し、HDR firefly が残る。
        float saTexel  = 4.0f * PI / (6.0f * float(envWidth) * float(envHeight));
        float saSample = 1.0f / max(float(g_sampleCount) * pdf, EPSILON);

        // roughness=0 の鏡面は mip 0 固定
        float mipLevel = g_roughness == 0.0f
                       ? 0.0f
                       : 0.5f * log2(saSample / saTexel);
        mipLevel = clamp(mipLevel, 0.0f, float(g_envMipCount - 1u));

        float3 sampleColor = max(g_envMap.SampleLevel(g_sampler, L, mipLevel).rgb, 0.0f);

        // Karis 型の高輝度サンプル抑制。
        // WHY: HDRI の太陽などを少数の GGX サンプルだけが拾うと、平均後も孤立した
        //      高輝度テクセルが残り、Deferred HDR 上で白点として見える。
        //      完全鏡面では実環境の輝度を保持し、粗い面ほど外れ値の影響だけを弱める。
        float luminance = dot(sampleColor, float3(0.2126f, 0.7152f, 0.0722f));
        float fireflyWeight = rcp(1.0f + luminance);
        float suppression = smoothstep(0.05f, 0.35f, g_roughness);
        float sampleWeight = NdotL * lerp(1.0f, fireflyWeight, suppression);

        prefilteredColor += sampleColor * sampleWeight;
        totalWeight      += sampleWeight;
    }

    prefilteredColor /= max(totalWeight, EPSILON);

    g_output[uint3(dtid.x, dtid.y, 0)] = float4(prefilteredColor, 1.0f);
}
