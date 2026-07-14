// FBZZ Engine
// PostProcess/AntiAliasing/TAA.hlsl | PostProcess
// Temporal Anti-Aliasing — 前フレームを再投影・ブレンドしてエイリアシングを低減する
//
// アルゴリズム概要:
//   1. 深度からワールド座標を復元し、prevViewProjection で前フレーム UV を計算（再投影）
//   2. 前フレーム UV が画面外なら現フレームをそのまま出力
//   3. 現フレームの 3x3 近傍カラーで Variance Clipping を行い履歴をクランプ
//   4. taaFeedback でブレンド比を制御: lerp(history, current, 1 - taaFeedback)
//
// 入力バインディング:
//   t5  = 現フレーム HDR/LDR カラー   (TEX_GBUFFER0)
//   t21 = 前フレーム TAA 出力          (TEX_TAA_HISTORY)
//   t7  = 深度バッファ                 (TEX_DEPTH)
//   b0  = CameraConstants
//   b5  = PostProcConstants
//   b8  = AdvancedGraphicsConstants

#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Platform/Backend.hlsli"

Texture2D        texCurrent : register(TEX_GBUFFER0);    // 現フレームカラー
Texture2D        texHistory : register(TEX_TAA_HISTORY); // 前フレーム TAA 出力
Texture2D<float> texDepth   : register(TEX_DEPTH);       // 深度バッファ

SamplerState sampDefault : register(SAMPLER_DEFAULT);     // バイリニアクランプ
SamplerState sampPoint   : register(SAMPLER_POINT_CLAMP); // ポイントサンプル（再投影用）

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

// AABB クランプ: 近傍の min/max ボックスに履歴カラーをクリップする
// WHY: ゴーストアーティファクトを抑えながら、平坦領域では広いブレンド比を維持できる
float3 ClipToAABB(float3 histColor, float3 minColor, float3 maxColor)
{
    float3 center  = 0.5f * (maxColor + minColor);
    float3 extents = 0.5f * (maxColor - minColor) + 1e-4f;
    float3 offset  = histColor - center;
    float3 ts      = abs(offset) / extents;
    float  maxT    = max(ts.x, max(ts.y, ts.z));
    if (maxT > 1.0f)
        histColor = center + offset / maxT;
    return histColor;
}

// ─── ピクセルシェーダー ─────────────────────────────────────────────────────────

float4 PSMain(FSTriVSOut p) : SV_Target0
{
    float2 uv = p.uv;

    // ── 現フレームカラーを取得 ─────────────────────────────────────────────────
    float3 currentColor = texCurrent.Sample(sampDefault, uv).rgb;

    // ── 深度からワールド座標を復元し、前フレーム UV を計算 ─────────────────────
    float  ndcZ     = texDepth.Sample(sampPoint, uv).r;
    float3 worldPos = ReconstructWorldPos(uv, ndcZ, invViewProjection);

    // 前フレームのクリップ空間へ投影
    float4 prevClip = mul(float4(worldPos, 1.0f), prevViewProjection);
    prevClip.xyz   /= prevClip.w;
    float2 prevUV   = NdcToUv(prevClip.xy);

    // ── 再投影 UV が画面外なら現フレームをそのまま出力 ─────────────────────────
    if (any(prevUV < 0.0f) || any(prevUV > 1.0f))
        return float4(currentColor, 1.0f);

    // ── 3x3 近傍で Variance Clipping 用 AABB を構築 ───────────────────────────
    // WHY: 動いたオブジェクト境界でのゴーストを抑えるため、
    //      局所的なカラー範囲に履歴をクランプする
    float3 minColor = currentColor;
    float3 maxColor = currentColor;

    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            if (dx == 0 && dy == 0) continue;
            float2 neighborUV = uv + float2(dx, dy) * texelSize;
            float3 s = texCurrent.Sample(sampDefault, neighborUV).rgb;
            minColor = min(minColor, s);
            maxColor = max(maxColor, s);
        }
    }

    // ── 前フレームカラーを AABB にクランプ ────────────────────────────────────
    float3 historyColor = texHistory.Sample(sampPoint, prevUV).rgb;
    historyColor = ClipToAABB(historyColor, minColor, maxColor);

    // ── taaFeedback でブレンド ────────────────────────────────────────────────
    // taaFeedback = 0   : 現フレームのみ (TAA 無効相当)
    // taaFeedback = 0.9 : 標準的な時間的蓄積（ジッタリングと組み合わせて滑らかな AA）
    float  blend  = 1.0f - saturate(taaFeedback);
    float3 result = lerp(historyColor, currentColor, blend);

    return float4(result, 1.0f);
}
