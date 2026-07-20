// FBZZ Engine
// PostProcess/Flare/LensFlare.hlsl | PostProcess
// スクリーンスペース Lens Flare — HDR バッファの輝点から
// ゴーストとハローを生成し加算合成する
//
// アルゴリズム概要:
//   1. lensFlareIntensity <= 0 なら float4(0,0,0,0) を返して早期終了
//   2. フレアベクトル = 0.5 - uv（スクリーン中心から外へ向かうベクトル）
//   3. ghostCount 個のゴーストをフレアベクトル上に均等配置
//      - 輝度 > threshold のピクセルのみゴーストとして加算
//   4. スクリーン中心からの距離でハロー（薄い円形グロー）を加算
//   5. 全て加算合成用 (alpha=0) で返す → 呼び出し側が additive blend する
//
// 入力バインディング:
//   t5  = HDR カラーバッファ  (TEX_GBUFFER0)
//   b5  = PostProcConstants
//   b8  = AdvancedGraphicsConstants

#include "Common/Constants.hlsli"
#include "Platform/Backend.hlsli"

Texture2D    texHDR      : register(TEX_GBUFFER0); // HDR カラー入力
SamplerState sampDefault : register(SAMPLER_DEFAULT);

// ─── フルスクリーントライアングル ───────────────────────────────────────────────

struct FSTriVSOut
{
    float4 svPosition : SV_POSITION;
    float2 uv         : TEXCOORD0;
};

FSTriVSOut VSMain(uint id : SV_VertexID)
{
    FSTriVSOut o;
    o.uv         = float2((id & 1u) ? 2.0f : 0.0f,
                          (id & 2u) ? 2.0f : 0.0f);
    o.svPosition = float4(o.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

// ─── ユーティリティ ──────────────────────────────────────────────────────────────

// 輝度計算 (BT.601 係数)
float Luminance(float3 c) { return dot(c, float3(0.299f, 0.587f, 0.114f)); }

// ゴースト形状の減衰: 中心からの距離でフォールオフ
// WHY: ゴーストの端をぼかすことで不自然な矩形クリップを防ぐ
float GhostFalloff(float2 ghostUV)
{
    float2 centered = ghostUV - 0.5f;
    float  dist     = length(centered) * 2.0f; // [0, √2] → 1.0 付近で境界
    return saturate(1.0f - dist * dist);
}

// ─── ピクセルシェーダー ─────────────────────────────────────────────────────────

float4 PSMain(FSTriVSOut p) : SV_Target0
{
    // レンズフレアが無効なら黒透明を返す（加算合成時に影響なし）
    if (lensFlareIntensity <= 0.0f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);

    float2 uv = p.uv;

    // フレアベクトル: スクリーン中心 (0.5, 0.5) から現ピクセルへ向かうベクトル
    // WHY: 光源とは逆方向にゴーストが連なる光学現象を再現する
    float2 flareVec = 0.5f - uv;

    // 輝度しきい値: HDR バッファの明るいピクセルのみフレアを発生させる
    static const float GHOST_THRESHOLD = 1.0f; // HDR 輝度しきい値（> 1.0 が輝点）

    // ── ゴーストの積算 ───────────────────────────────────────────────────────────
    float3 ghostAccum = float3(0.0f, 0.0f, 0.0f);
    int    ghostCount = clamp(lensFlareGhostCount, 1, 8);

    for (int i = 0; i < ghostCount; ++i)
    {
        // ゴーストを 0 番から ghostCount-1 番まで均等配置
        // lensFlareDistort でゴースト間隔をスケール
        float  t       = float(i) / float(ghostCount);
        float2 ghostUV = uv + flareVec * t * lensFlareDistort;

        // UV が画面外なら除外
        if (any(ghostUV < 0.0f) || any(ghostUV > 1.0f))
            continue;

        float3 sample    = texHDR.Sample(sampDefault, ghostUV).rgb;
        float  luminance = Luminance(sample);

        // 輝度しきい値を超えた輝点のみゴーストに寄与させる
        if (luminance > GHOST_THRESHOLD)
        {
            // 輝点のカラーを中心からのフォールオフ込みで加算
            float  falloff = GhostFalloff(ghostUV);
            float  weight  = saturate((luminance - GHOST_THRESHOLD) / GHOST_THRESHOLD);
            ghostAccum    += sample * weight * falloff;
        }
    }

    // ── ハロー (Halo): スクリーン中心からの距離で薄い円形グロー ─────────────────
    // WHY: レンズ内で散乱した光が円形に広がる現象を再現する
    float  distFromCenter = length(uv - 0.5f);
    float  haloRadius     = lensFlareHaloWidth * 0.5f; // 中心からのリング半径
    float  haloWidth      = max(lensFlareHaloWidth * 0.1f, 0.01f);
    float  haloFactor     = saturate(1.0f - abs(distFromCenter - haloRadius) / haloWidth);
    haloFactor = pow(haloFactor, 3.0f); // シャープなリング形状

    // ハローはスクリーン全体の平均的な明るさに比例させる（固定カラー）
    float3 haloColor = float3(0.8f, 0.85f, 1.0f) * haloFactor; // 青白いグロー

    // ── 最終合成 ─────────────────────────────────────────────────────────────────
    float3 result = (ghostAccum + haloColor) * lensFlareIntensity;

    // alpha = 0 で返す: 呼び出し側は Additive Blend (SrcBlend=ONE, DestBlend=ONE) を使う
    // WHY: ゴーストは光の加算なので乗算合成は不適切。アルファブレンドでなく加算合成が自然
    return float4(result, 0.0f);
}
