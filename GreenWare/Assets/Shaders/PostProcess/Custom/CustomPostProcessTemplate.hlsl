// FBZZ Engine
// PostProcess/Custom/CustomPostProcessTemplate.hlsl
// カスタムポストプロセスのスターターテンプレート
//
// ── 使い方 ──────────────────────────────────────────────────────────────────
//  1. このファイルをコピーして名前を変える  (例: MyEffect.hlsl)
//  2. PSMain の "エフェクト実装" セクションを編集する
//  3. 保存後はEditor/CMakeが自動収集し、compile_shaders.ps1が差分だけをコンパイルする
//  4. 以下のいずれかで登録する:
//     [.fzpp] custom_effects に追加:
//       [[custom_effects]]
//       name    = "MyEffect"
//       enabled = true
//       shader  = "assets/shaders/PostProcess/Custom/MyEffect.hlsl"
//       intensity   = 1.0
//       blend       = 1.0
//       parameters  = [0.0, 0.0, 0.0, 0.0]
//
//     [Script]
//       postProcess.AddCustom("MyEffect",
//           "assets/shaders/PostProcess/Custom/MyEffect.hlsl");
//       postProcess.SetCustomParameters("MyEffect", 0.5f, 1.0f, 0.0f, 0.0f);
//
// ── 利用可能なパラメータ (Inspector / Script から制御) ──────────────────────
//  customIntensity      — エフェクト全体の強度  [0, 1]  (0=無効, 1=最大)
//  customBlend          — 元画像との合成比率    [0, 1]  (0=元画像のみ, 1=エフェクトのみ)
//  customParameters.x   — Inspector "Parameters[0]" / SetCustomParameter("name", 0, v)
//  customParameters.y   — Inspector "Parameters[1]" / SetCustomParameter("name", 1, v)
//  customParameters.z   — Inspector "Parameters[2]" / SetCustomParameter("name", 2, v)
//  customParameters.w   — Inspector "Parameters[3]" / SetCustomParameter("name", 3, v)
//
// ── 利用可能な共通定数 (PostProcConstants b5) ────────────────────────────────
//  screenSize   float2   — レンダーターゲット解像度 (ピクセル)
//  texelSize    float2   — 1 テクセルの UV サイズ (1.0 / screenSize)
//  time         float    — アプリ起動からの経過秒
//
// ── 利用可能なユーティリティ (Rendering/PostProcess.hlsli) ───────────────────
//  LensDistortUV(uv, amount)                     UV 歪み
//  ApplySepia(color, intensity)                  セピア変換
//  ApplyInvert(color, intensity)                 色反転
//  ApplyPosterize(color, levels)                 ポスタリゼーション
//  ApplyFilmGrain(color, uv, intensity, resp)    フィルムグレイン
//  ApplyVignette(color, uv, int, sm, rnd, col)   ビネット
//  ApplyColorAdjustments(color, ...)             コントラスト/彩度/色相
//  ApplyShadowHighlight(color, shadow, hi)       シャドウ・ハイライト
//  ApplyColorFilter(color, filterColor, int)     カラーフィルター
//  ApplyPixelateUV(uv, pixelSize)                ピクセル化 UV
//
// ── NOTE ──────────────────────────────────────────────────────────────────────
//  入力テクスチャ texInput (t5) = 直前のポストプロセスまたは Composite 後の LDR 画像。
//  カスタムパスは複数追加でき、各パスの出力が次のパスの入力になる (チェーン処理)。
//  深度バッファは現在このパスにはバインドされていない。
// ─────────────────────────────────────────────────────────────────────────────

#include "Common/Constants.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Common/Color.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/PostProcess.hlsli"

// 入力テクスチャ — t5 は RenderSystem が自動バインドする LDR カラーバッファ
Texture2D    texInput   : register(TEX_GBUFFER0);
SamplerState sampLinear : register(SAMPLER_DEFAULT);

// ── 頂点シェーダー ─────────────────────────────────────────────────────────
// WHY: 三頂点だけで画面全体を覆う fullscreen triangle を生成する。
//      頂点バッファ不要で DrawCall.vertexCount = 3 のみで動作する。
FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

// ── ピクセルシェーダー ─────────────────────────────────────────────────────
// ここから下を自由にカスタマイズしてください。
// customBlend と customIntensity を最終段で適用するパターンを守れば
// Inspector/Script からのリアルタイム制御が自動で機能する。

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    // ---- 入力サンプリング ------------------------------------------------
    const float3 original = texInput.SampleLevel(sampLinear, p.uv, 0).rgb;

    // ---- パラメータ展開 --------------------------------------------------
    // customParameters は Inspector の "Parameters" フィールド、または
    // Script の SetCustomParameters("name", x, y, z, w) で設定する。
    // 用途は自由。例として以下の意味を割り当てている:
    const float param0 = customParameters.x;  // 例: エフェクト強度 A
    const float param1 = customParameters.y;  // 例: エフェクト強度 B
    const float param2 = customParameters.z;  // 例: 閾値 / 半径など
    const float param3 = customParameters.w;  // 例: 汎用

    // ====================================================================
    // エフェクト実装セクション
    // ここに処理を書く。最終的に float3 result に結果を入れて返す。
    // 下に例として各種エフェクトをコメントアウトで示す。
    // ====================================================================

    float3 result = original;  // ← デフォルトはパススルー

    // ---- 例 1: セピア ---------------------------------------------------
    // result = ApplySepia(result, param0);

    // ---- 例 2: ビネット -------------------------------------------------
    // const float3 vignetteColor = float3(0.0f, 0.0f, 0.0f);
    // result = ApplyVignette(result, p.uv, param0, 0.4f, 1.0f, vignetteColor);

    // ---- 例 3: フィルムグレイン ------------------------------------------
    // result = ApplyFilmGrain(result, p.uv, param0, param1);

    // ---- 例 4: UV 歪み + クロマティックアベレーション --------------------
    // const float2 uvR = LensDistortUV(p.uv, param0 * 0.01f) + texelSize * float2( 2.0f, 0.0f);
    // const float2 uvB = LensDistortUV(p.uv, param0 * 0.01f) + texelSize * float2(-2.0f, 0.0f);
    // result.r = texInput.SampleLevel(sampLinear, uvR, 0).r;
    // result.b = texInput.SampleLevel(sampLinear, uvB, 0).b;

    // ---- 例 5: アウトライン (Sobel エッジ検出) ---------------------------
    // const float3 n  = texInput.SampleLevel(sampLinear, p.uv + float2( 0,  1) * texelSize, 0).rgb;
    // const float3 s  = texInput.SampleLevel(sampLinear, p.uv + float2( 0, -1) * texelSize, 0).rgb;
    // const float3 e  = texInput.SampleLevel(sampLinear, p.uv + float2( 1,  0) * texelSize, 0).rgb;
    // const float3 w  = texInput.SampleLevel(sampLinear, p.uv + float2(-1,  0) * texelSize, 0).rgb;
    // const float3 gx = e - w;
    // const float3 gy = n - s;
    // const float  edge = saturate(length(float2(dot(gx, gx), dot(gy, gy))) * param0);
    // result = lerp(result, float3(param1, param2, param3), edge);

    // ---- 例 6: スキャンライン --------------------------------------------
    // const float line = 0.5f + 0.5f * sin((p.uv.y * screenSize.y + time * 60.0f) * 3.14159f);
    // result *= lerp(1.0f, line, saturate(param0));

    // ---- 例 7: 時間アニメーション (pulse glow) ---------------------------
    // const float pulse = 0.5f + 0.5f * sin(time * param0 * 6.283f);
    // result += Luminance(result) * pulse * param1;

    // ---- 例 8: ピクセル化 -----------------------------------------------
    // const float2 pixUV = ApplyPixelateUV(p.uv, max(param0 * 16.0f, 1.0f));
    // result = texInput.SampleLevel(sampLinear, pixUV, 0).rgb;

    // ====================================================================
    // 最終合成 — 変更禁止推奨
    // customIntensity でエフェクト強度を調整し、
    // customBlend で元画像とブレンドしてから返す。
    // ====================================================================
    const float3 effected = lerp(original, result, saturate(customIntensity));
    return float4(lerp(original, effected, saturate(customBlend)), 1.0f);
}
