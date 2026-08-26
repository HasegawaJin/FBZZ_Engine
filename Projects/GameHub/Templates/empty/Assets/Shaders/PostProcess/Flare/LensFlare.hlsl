/// @file LensFlare.hlsl
/// @brief スクリーンスペースレンズフレア (ゴースト + ハロー) を加算合成する PS
/// @author Hasegawa Jin
/// @date 2026/06/23
///
/// 入力バインディング:
///   t5  = 輝度抽出済みハーフ解像度カラー (TEX_GBUFFER0)
///   b5  = PostProcConstants (screenSize)
///   b8  = AdvancedGraphicsConstants (lensFlare*)

#include "Common/Constants.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Platform/Backend.hlsli"

Texture2D    texBright   : register(TEX_GBUFFER0);
// 全画面フェッチなので clamp 必須。ゴーストは uv を画面中心へ反転させて引くため、
// s0 (DX12 では WRAP) だと画面外へ出たゴーストが反対側の端から出てくる。
SamplerState sampDefault : register(SAMPLER_LINEAR_CLAMP);

// 画面隅に近い光源ほど寄与を落とす指数。実レンズでも軸外の光ほどフレアが弱い。
// WHY: 画面外の光源は拾えない (スクリーンスペースの限界) ので、端に届く前に寄与を
//      ほぼ 0 にしないと光源がフレームを跨ぐ瞬間にフレアがパッと消える。逆に大きすぎると
//      画面中央付近の光源でしかフレアが出なくなる。3 前後がその折り合い。
static const float FALLOFF_POWER = 3.0f;

// ゴーストの色づき。前段ほど暖色、後段ほど寒色に寄せると光学系のコーティング差らしく見える。
static const float3 GHOST_TINT_NEAR = float3(1.00f, 0.85f, 0.60f);
static const float3 GHOST_TINT_FAR  = float3(0.55f, 0.75f, 1.00f);
static const float3 HALO_TINT       = float3(0.75f, 0.85f, 1.00f);

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

// 画面中心からの距離を「隅で 1.0」に正規化する。
// WHY: UV 空間のまま測ると 16:9 では横が詰まり、ハローのリングが楕円に潰れる。
float NormalizedCenterDistance(float2 uv, float2 aspect)
{
    return length((uv - 0.5f) * aspect) / length(0.5f * aspect);
}

float3 SampleFlareSource(float2 uv, float2 aspect)
{
    // 画面外には光源が無い。clamp サンプラーの端引き伸ばしを持ち込ませない。
    if (any(uv < 0.0f) || any(uv > 1.0f))
        return float3(0.0f, 0.0f, 0.0f);

    // 分岐内なので SampleLevel — Sample は非一様フローで微分が未定義になる。
    float3 source = texBright.SampleLevel(sampDefault, uv, 0).rgb;
    return source * pow(saturate(1.0f - NormalizedCenterDistance(uv, aspect)), FALLOFF_POWER);
}

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    if (lensFlareIntensity <= 0.0f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);

    const float2 uv     = p.uv;
    const float2 aspect = float2(max(screenSize.x, 1.0f) / max(screenSize.y, 1.0f), 1.0f);
    const int    ghostCount = clamp(lensFlareGhostCount, 1, 16);
    const float  spreadBase = max(lensFlareDistort, 0.0f);

    float3 result = float3(0.0f, 0.0f, 0.0f);

    // ゴースト: 光源を画面中心で点対称に写した位置に、倍率を変えた縮小像として並べる。
    // WHY: 実レンズのゴーストは絞りを挟んだ反対側に連なる。中心対称にせず光源側へ
    //      ずらすだけだと光源自身を足し直すことになり、フレアではなく滲みにしか見えない。
    for (int i = 0; i < ghostCount; ++i)
    {
        float spread = spreadBase * (1.0f - float(i) / float(ghostCount));
        if (spread <= 1e-4f)
            continue;

        float2 sampleUV = 0.5f - (uv - 0.5f) / spread;
        float3 tint     = lerp(GHOST_TINT_NEAR, GHOST_TINT_FAR,
                               float(i) / float(max(ghostCount - 1, 1)));
        result += SampleFlareSource(sampleUV, aspect) * tint;
    }

    // ハロー: 対称像から中心方向へ haloWidth だけずらした点を拾うことで、
    //         光源を囲むリングになる。
    if (lensFlareHaloWidth > 0.0f)
    {
        float2 mirrorUV = 1.0f - uv;
        float2 toCenter = (0.5f - mirrorUV) * aspect;
        float  len      = length(toCenter);
        if (len > 1e-5f)
        {
            float2 haloVec = (toCenter / len) * lensFlareHaloWidth / aspect;
            result += SampleFlareSource(mirrorUV + haloVec, aspect) * HALO_TINT;
        }
    }

    // alpha = 1: 呼び出し側は ADDITIVE (rgb = src.rgb * src.a + dst.rgb) なので、
    // src.a はフレアそのものの寄与率になる。0 を返すと 1 画素も足されない。
    return float4(result * lensFlareIntensity, 1.0f);
}
