// FBZZ Engine
// PostProcess/Custom/CustomPostProcessTemplate.hlsl
// カスタムポストプロセスのスターターテンプレート
//
// ── 使い方 ──────────────────────────────────────────────────────────────────
//  1. このファイルをコピーして名前を変える  (例: MyEffect.hlsl)
//  2. PSMain の "エフェクト実装" セクションを編集する
//  3. 保存後はEditor/CMakeが自動収集し、compile_shaders.ps1が差分だけをコンパイルする
//  4. 以下のいずれかで登録する:
//     [PostProcessProfile (.fzdata)] customEffects に追加:
//       [[customEffects]]
//       name       = "MyEffect"
//       enabled    = true
//       shaderPath = "assets/shaders/PostProcess/Custom/MyEffect.hlsl"
//       intensity  = 1.0
//       blend      = 1.0
//       stage      = 1        # 0 = SceneHDR / 1 = PostProcess / 2 = AfterOpaque
//       blendMode  = 0        # 0 = 置き換え / 1 = アルファ / 2 = 加算 / 3 = 事前乗算
//       inputs     = 2        # 追加入力のビット (下の表)
//       iterations = 1        # 同じシェーダーを何回重ねるか (HDR の段のみ)
//       downscale  = 1        # 1/N 解像度で走らせて拡大合成 (HDR の段のみ)
//       param0     = 0.0
//       param1     = 0.0
//       param2     = 0.0
//       param3     = 0.0      # param7 まで使える
//     (Inspector なら Post Process Profile の Custom Effects で "+" するだけでよい)
//
//     [Script]
//       postProcess.AddCustom("MyEffect",
//           "assets/shaders/PostProcess/Custom/MyEffect.hlsl");
//       postProcess.SetCustomParameters("MyEffect", 0.5f, 1.0f, 0.0f, 0.0f);
//
// ── 値が 8 個で足りなくなったら .mat にする ─────────────────────────────────
//  materialPath に .mat を指すと、シェーダー・テクスチャ (t0-t4)・名前付きパラメーター
//  (b2 = MaterialConstants) がそこから来る。Inspector の材質エディタがそのまま
//  編集 UI になり、色はカラーピッカー、テクスチャは D&D で差せる。
//    - .mat には render_path = "post_process" を書くこと (メッシュ用の .mat を
//      割り当てると頂点入力が合わず、何も出ないか画面が塗り潰される)
//    - .mat の blend_mode が blendMode より優先される
//    - シェーダー側は #define FBZZ_MATERIAL_CONSTANTS してから自分の cbuffer を
//      register(CB_MATERIAL) へ宣言する (UI / Particle のカスタムと同じ書き方)
//
// ── どの段で走らせるか (stage) ───────────────────────────────────────────────
//  PostProcess (既定) — Composite の後。トーンマップ済みの LDR で、最終画を作り直す
//                        変換 (歪み・階調・ビネット) はここ。複数あれば直列に繋がる。
//  SceneHDR           — Composite の前。値が 1 を超えられるので、書いた光がそのまま
//                        ブルームと露出へ流れる。«盤面の中で光っているもの» はこちら。
//  AfterOpaque        — 不透明の直後。デカール・パーティクル・半透明はこの後に描かれる。
//                        背景だけを歪めたい効果 (熱・水中・屈折) はここでないと、
//                        既に描かれたパーティクルごと曲がる。
//
// ── どう載せるか (blendMode / HDR の段でだけ有効) ───────────────────────────
//  0 置き換え — 画面を作り直す。t5 に «今の HDR» が来て、返した色で置き換わる
//  2 加算     — 寄与だけを描き足す。t5 は束縛されない (描き先は読めないため)。
//               ADDITIVE は rgb に a を掛けてから足すので、alpha は 1 を返すこと
//
// ── 重ねる / 縮める (iterations, downscale / HDR の段でだけ有効) ────────────
//  iterations — 同じシェーダーを続けて N 回掛ける。何回目かは customPassInfo.y、
//               総数は .z で分かる (ぼかしを «横 → 縦» に分けるのはこれ)
//  downscale  — 1/N 解像度で走らせ、実寸へ拡大して合成する。半径の大きいぼかしは
//               フル解像度でタップを増やすより桁で安い
//  NOTE: 入力テクスチャを引く UV は FBZZ_CustomInputUV(uv) を通すこと (縮小中の倍率と端の留め)。
//        縮小結果は RT の左上にしか無いため (下の PSMain がその書き方)。
//
// ── 利用可能なパラメータ (Inspector / Script から制御) ──────────────────────
//  customIntensity      — エフェクト全体の強度  [0, 1]  (0=無効, 1=最大)
//  customBlend          — 元画像との合成比率    [0, 1]  (0=元画像のみ, 1=エフェクトのみ)
//  customParameters.xyzw  — Inspector "param0..3" / SetCustomParameter("name", 0..3, v)
//  customParameters2.xyzw — Inspector "param4..7" / SetCustomParameter("name", 4..7, v)
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
// ── 入力 (inputs のビットで宣言したものだけが束縛される) ──────────────────────
//  t5 TEX_GBUFFER0     — 画面の色。宣言不要。PostProcess 段では «直前のパスの出力»、
//                        HDR の段 + 置き換えでは «今の HDR»。加算では束縛されない
//   2 t6 TEX_GBUFFER1   — オブジェクトマスク (RGB / A は申告した側との取り決め)。
//                        誰も申告していないフレームは空 (= 全部 0)。既定で有効
//   1 t7 TEX_DEPTH      — シーン深度。Texture2D<float> で宣言する
//     t8 TEX_SHADOW     — マスク側の深度 (マスクを宣言したときだけ)
//   4 t11 TEX_CUSTOM_VELOCITY — モーションベクター (TAA / MotionBlur が有効なフレーム)
//   8 t12 TEX_CUSTOM_NORMAL   — GBuffer 法線 (Deferred 系のみ)
//  16 t10 TEX_BLOOM     — ブルーム (PostProcess 段でのみ意味を持つ)
//  32 t9  TEX_SSAO      — 遮蔽
//
//  NOTE: 深度 (t7 / t8) は HDR の段では束縛されない。描き先が hdrRT で、その深度は
//        書き込み先として押さえられているため。遮蔽の判定が要るならマスク側で済ませる
//        (ObjectMask.hlsl は visibleOnly で «見えている面だけ» を残せる)。
//
// ── NOTE ──────────────────────────────────────────────────────────────────────
//  カスタムパスは複数追加でき、同じ段の中では登録順に直列で繋がる。
//  段をまたぐ順序は常に AfterOpaque → (半透明) → SceneHDR → Composite → PostProcess。
// ─────────────────────────────────────────────────────────────────────────────

#include "Common/Constants.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Common/Color.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/PostProcess.hlsli"
#include "Common/BindlessIndices.hlsli"

// 入力テクスチャ — t5 は RenderSystem が自動バインドする LDR カラーバッファ
FBZZ_TEX2D(texInput, TEX_GBUFFER0_SLOT);
// 全画面フェッチなので clamp 必須 (s0 は DX12 では WRAP)。
SamplerState sampLinear : register(SAMPLER_LINEAR_CLAMP);

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
    // 入力を引く UV。downscale を使わないなら倍率は 1 なので、
    // この掛け算は «常に書いておいて損が無い» 形になっている。
    const float2 inputUV = FBZZ_CustomInputUV(p.uv);
    const float3 original = texInput.SampleLevel(sampLinear, inputUV, 0).rgb;

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
    // const float3 n  = texInput.SampleLevel(sampLinear, FBZZ_CustomInputUV(p.uv + float2(0, 1) * texelSize), 0).rgb;
    // const float3 s  = texInput.SampleLevel(sampLinear, FBZZ_CustomInputUV(p.uv + float2(0, -1) * texelSize), 0).rgb;
    // const float3 e  = texInput.SampleLevel(sampLinear, FBZZ_CustomInputUV(p.uv + float2(1, 0) * texelSize), 0).rgb;
    // const float3 w  = texInput.SampleLevel(sampLinear, FBZZ_CustomInputUV(p.uv + float2(-1, 0) * texelSize), 0).rgb;
    // const float3 gx = e - w;
    // const float3 gy = n - s;
    // const float  edge = saturate(length(float2(dot(gx, gx), dot(gy, gy))) * param0);
    // result = lerp(result, float3(param1, param2, param3), edge);

    // ---- 例 6: スキャンライン --------------------------------------------
    // NOTE: line / point / sample / triangle は HLSL の補間モディファイア。変数名にすると DXC が弾く。
    // const float scan = 0.5f + 0.5f * sin((p.uv.y * screenSize.y + time * 60.0f) * 3.14159f);
    // result *= lerp(1.0f, scan, saturate(param0));

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
