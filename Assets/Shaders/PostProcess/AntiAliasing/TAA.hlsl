// FBZZ Engine
// PostProcess/AntiAliasing/TAA.hlsl | PostProcess
// Temporal Anti-Aliasing — 前フレームを再投影・ブレンドしてエイリアシングを低減する
//
// アルゴリズム概要:
//   1. 前フレーム UV を求める
//      - モーションベクターが書かれている画素はそれを引く (オブジェクトの動きを含む)
//      - 書かれていない画素 (空・未描画) は深度からワールド座標を復元して再投影
//   2. 前フレーム UV が画面外なら現フレームをそのまま出力
//   3. 現フレームの 3x3 近傍カラーで Variance Clipping を行い履歴をクランプ
//   4. taaFeedback でブレンド比を制御: lerp(history, current, 1 - taaFeedback)
//
// WHY 深度再投影だけでは足りないか:
//   深度再投影が復元できるのは「カメラが動いた」ぶんだけ。走っているキャラクターは
//   前フレームに別の場所にいたのに、その画素は「動いていない」と判定される。
//   結果、履歴が背景の色を引いてきて輪郭に尾を引く (ゴースト)。
//
// 入力バインディング:
//   t5  = 現フレーム HDR/LDR カラー   (TEX_GBUFFER0)
//   t21 = 前フレーム TAA 出力          (TEX_TAA_HISTORY)
//   t7  = 深度バッファ                 (TEX_DEPTH)
//   t26 = モーションベクター            (TEX_VELOCITY)
//   b0  = CameraConstants
//   b5  = PostProcConstants
//   b8  = AdvancedGraphicsConstants

#include "Common/Constants.hlsli"
#include "Common/Space.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Platform/Backend.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D(texCurrent, TEX_GBUFFER0_SLOT);    // 現フレームカラー
FBZZ_TEX2D(texHistory, TEX_TAA_HISTORY_SLOT); // 前フレーム TAA 出力
FBZZ_TEX2D_T(float, texDepth, TEX_DEPTH_SLOT);       // 深度バッファ
FBZZ_TEX2D(texVelocity, TEX_VELOCITY_SLOT);    // モーションベクター (RG=速度, B=有効)
/// @note 粒子が画素を覆う割合 (ParticleReactive パス)。粒子の無いフレームは束縛されないので、読む前に添字を確かめる。
FBZZ_TEX2D(texReactive, TEX_SSAO_SLOT);

SamplerState sampDefault : register(SAMPLER_LINEAR_CLAMP); // バイリニアクランプ (s0 は DX12 では WRAP)
SamplerState sampPoint   : register(SAMPLER_POINT_CLAMP); // ポイントサンプル（再投影用）

// ─── フルスクリーントライアングル ───────────────────────────────────────────────

FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
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

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    float2 uv = p.uv;

    // ── 現フレームカラーを取得 ─────────────────────────────────────────────────
    float3 currentColor = texCurrent.Sample(sampDefault, uv).rgb;

    // 履歴を使わない設定では深度復元・近傍サンプル・履歴サンプルを全て省略する。
    if (taaFeedback <= 0.0f)
        return float4(currentColor, 1.0f);

    // ── 前フレーム UV を求める ─────────────────────────────────────────────────
    float2 prevUV;
    /// @note 速度は VelocityPass が走ったフレームだけ束縛される。無効な添字のまま読むと未定義。
    float3 velocity = IsBindlessValid(FbzzPixelSlot(TEX_VELOCITY_SLOT))
        ? texVelocity.Sample(sampPoint, uv).rgb : float3(0.0f, 0.0f, 0.0f);
    if (velocity.z > 0.5f)
    {
        // Velocity パスが書いた画素。カメラとオブジェクトの動きが両方入っている。
        prevUV = uv - velocity.xy;
    }
    else
    {
        // 空や未描画の画素。ここはカメラの動きしか無いので深度再投影で足りる。
        float  ndcZ     = texDepth.Sample(sampPoint, uv).r;
        float3 worldPos = ReconstructWorldPos(uv, ndcZ, invViewProjection);

        float4 prevClip = mul(float4(worldPos, 1.0f), prevViewProjection);
        if (abs(prevClip.w) < 1.0e-5f)
            return float4(currentColor, 1.0f);
        prevClip.xyz /= prevClip.w;
        prevUV = NdcToUv(prevClip.xy);
    }

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
    /// @note 再投影先は画素中心に揃わないので双線形で読む。最近傍だと動くたびに半画素ずつ跳ね、輪郭が揺れる。
    float3 historyColor = texHistory.Sample(sampDefault, prevUV).rgb;
    /// @note NaN は一度履歴に入るとクランプでも消えず残り続ける。見つけたら今のフレームで置き換える。
    if (any(isnan(historyColor)) || any(isinf(historyColor)))
        historyColor = currentColor;
    historyColor = ClipToAABB(historyColor, minColor, maxColor);

    // ── taaFeedback でブレンド ────────────────────────────────────────────────
    // taaFeedback = 0   : 現フレームのみ (TAA 無効相当)
    // taaFeedback = 0.9 : 標準的な時間的蓄積（ジッタリングと組み合わせて滑らかな AA）
    float  blend  = 1.0f - saturate(taaFeedback);
    // 粒子は速度を書かないので、履歴は背景の動きで引かれている。粒子が覆う画素ほど今のフレームを採る。
    const float reactive = IsBindlessValid(FbzzPixelSlot(TEX_SSAO_SLOT))
        ? saturate(texReactive.SampleLevel(sampPoint, uv, 0.0f).r) : 0.0f;
    blend = lerp(blend, 1.0f, reactive * 0.85f);
    float3 result = lerp(historyColor, currentColor, blend);

    return float4(result, 1.0f);
}
