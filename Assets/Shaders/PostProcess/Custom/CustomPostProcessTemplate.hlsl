/// @file    CustomPostProcessTemplate.hlsl
/// @brief   カスタムポストプロセスのスターターテンプレート
/// @author  Hasegawa Jin
/// @date    2026-06-22
///
/// @note ── 使い方 ──────────────────────────────────────────────────────────────────
/// @note 1. このファイルをコピーして名前を変える  (例: MyEffect.hlsl)
/// @note 2. PSMain の "エフェクト実装" セクションを編集する
/// @note 3. 保存後はEditor/CMakeが自動収集し、compile_shaders.ps1が差分だけをコンパイルする
/// @note 4. 以下のいずれかで登録する:
/// @note [PostProcessProfile (.fzdata)] customEffects に追加:
/// @note [[customEffects]]
/// @note name       = "MyEffect"
/// @note enabled    = true
/// @note shaderPath = "assets/shaders/PostProcess/Custom/MyEffect.hlsl"
/// @note intensity  = 1.0
/// @note blend      = 1.0
/// @note stage      = 1        # 0 = SceneHDR / 1 = PostProcess / 2 = AfterOpaque
/// @note blendMode  = 0        # 0 = 置き換え / 1 = アルファ / 2 = 加算 / 3 = 事前乗算
/// @note inputs     = 2        # 追加入力のビット (下の表)
/// @note iterations = 1        # 同じシェーダーを何回重ねるか (HDR の段のみ)
/// @note downscale  = 1        # 1/N 解像度で走らせて拡大合成 (HDR の段のみ)
/// @note param0     = 0.0
/// @note param1     = 0.0
/// @note param2     = 0.0
/// @note param3     = 0.0      # param7 まで使える
/// @note (Inspector なら Post Process Profile の Custom Effects で "+" するだけでよい)
///
/// @note [Script]
/// @note postProcess.AddCustom("MyEffect",
/// @note "assets/shaders/PostProcess/Custom/MyEffect.hlsl");
/// @note postProcess.SetCustomParameters("MyEffect", 0.5f, 1.0f, 0.0f, 0.0f);
///
/// @note ── 値が 8 個で足りなくなったら .mat にする ─────────────────────────────────
/// @note materialPath に .mat を指すと、シェーダー・テクスチャ (t0-t4)・名前付きパラメーター
/// @note (b2 = MaterialConstants) がそこから来る。Inspector の材質エディタがそのまま
/// @note 編集 UI になり、色はカラーピッカー、テクスチャは D&D で差せる。
/// @note - .mat には render_path = "post_process" を書くこと (メッシュ用の .mat を
/// @note 割り当てると頂点入力が合わず、何も出ないか画面が塗り潰される)
/// @note - .mat の blend_mode が blendMode より優先される
/// @note - シェーダー側は #define FBZZ_MATERIAL_CONSTANTS してから自分の cbuffer を
/// @note register(CB_MATERIAL) へ宣言する (UI / Particle のカスタムと同じ書き方)
///
/// @note ── どの段で走らせるか (stage) ───────────────────────────────────────────────
/// @note PostProcess (既定) — Composite の後。トーンマップ済みの LDR で、最終画を作り直す
/// @note 変換 (歪み・階調・ビネット) はここ。複数あれば直列に繋がる。
/// @note SceneHDR           — Composite の前。値が 1 を超えられるので、書いた光がそのまま
/// @note ブルームと露出へ流れる。«盤面の中で光っているもの» はこちら。
/// @note AfterOpaque        — 不透明の直後。デカール・パーティクル・半透明はこの後に描かれる。
/// @note 背景だけを歪めたい効果 (熱・水中・屈折) はここでないと、
/// @note 既に描かれたパーティクルごと曲がる。
///
/// @note ── どう載せるか (blendMode / HDR の段でだけ有効) ───────────────────────────
/// @note 0 置き換え — 画面を作り直す。t5 に «今の HDR» が来て、返した色で置き換わる
/// @note 2 加算     — 寄与だけを描き足す。t5 は束縛されない (描き先は読めないため)。
/// @note ADDITIVE は rgb に a を掛けてから足すので、alpha は 1 を返すこと
///
/// @note ── 重ねる / 縮める (iterations, downscale / HDR の段でだけ有効) ────────────
/// @note iterations — 同じシェーダーを続けて N 回掛ける。何回目かは customPassInfo.y、
/// @note 総数は .z で分かる (ぼかしを «横 → 縦» に分けるのはこれ)
/// @note downscale  — 1/N 解像度で走らせ、実寸へ拡大して合成する。半径の大きいぼかしは
/// @note フル解像度でタップを増やすより桁で安い
/// @note NOTE: 入力テクスチャを引く UV は FBZZ_CustomInputUV(uv) を通すこと (縮小中の倍率と端の留め)。
/// @note 縮小結果は RT の左上にしか無いため (下の PSMain がその書き方)。
///
/// @note ── 利用可能なパラメータ (Inspector / Script から制御) ──────────────────────
/// @note customIntensity      — エフェクト全体の強度  [0, 1]  (0=無効, 1=最大)
/// @note customBlend          — 元画像との合成比率    [0, 1]  (0=元画像のみ, 1=エフェクトのみ)
/// @note customParameters.xyzw  — Inspector "param0..3" / SetCustomParameter("name", 0..3, v)
/// @note customParameters2.xyzw — Inspector "param4..7" / SetCustomParameter("name", 4..7, v)
///
/// @note ── 利用可能な共通定数 (PostProcConstants b5) ────────────────────────────────
/// @note screenSize   float2   — レンダーターゲット解像度 (ピクセル)
/// @note texelSize    float2   — 1 テクセルの UV サイズ (1.0 / screenSize)
/// @note time         float    — アプリ起動からの経過秒
///
/// @note ── 利用可能なユーティリティ (Rendering/PostProcess.hlsli) ───────────────────
/// @note LensDistortUV(uv, amount)                     UV 歪み
/// @note ApplySepia(color, intensity)                  セピア変換
/// @note ApplyInvert(color, intensity)                 色反転
/// @note ApplyPosterize(color, levels)                 ポスタリゼーション
/// @note ApplyFilmGrain(color, uv, intensity, resp)    フィルムグレイン
/// @note ApplyVignette(color, uv, int, sm, rnd, col)   ビネット
/// @note ApplyColorAdjustments(color, ...)             コントラスト/彩度/色相
/// @note ApplyShadowHighlight(color, shadow, hi)       シャドウ・ハイライト
/// @note ApplyColorFilter(color, filterColor, int)     カラーフィルター
/// @note ApplyPixelateUV(uv, pixelSize)                ピクセル化 UV
///
/// @note ── 入力 (inputs のビットで宣言したものだけが束縛される) ──────────────────────
/// @note t5 TEX_GBUFFER0     — 画面の色。宣言不要。PostProcess 段では «直前のパスの出力»、
/// @note HDR の段 + 置き換えでは «今の HDR»。加算では束縛されない
/// @note 2 t6 TEX_GBUFFER1   — オブジェクトマスク (RGB / A は申告した側との取り決め)。
/// @note 誰も申告していないフレームは空 (= 全部 0)。既定で有効
/// @note 1 t7 TEX_DEPTH      — シーン深度。Texture2D<float> で宣言する
/// @note t8 TEX_SHADOW     — マスク側の深度 (マスクを宣言したときだけ)
/// @note 4 t11 TEX_CUSTOM_VELOCITY — モーションベクター (TAA / MotionBlur が有効なフレーム)
/// @note 8 t12 TEX_CUSTOM_NORMAL   — GBuffer 法線 (Deferred 系のみ)
/// @note 16 t10 TEX_BLOOM     — ブルーム (PostProcess 段でのみ意味を持つ)
/// @note 32 t9  TEX_SSAO      — 遮蔽
///
/// @note NOTE: 深度 (t7 / t8) は HDR の段では束縛されない。描き先が hdrRT で、その深度は
/// @note 書き込み先として押さえられているため。遮蔽の判定が要るならマスク側で済ませる
/// @note (ObjectMask.hlsl は visibleOnly で «見えている面だけ» を残せる)。
///
/// @note ── NOTE ──────────────────────────────────────────────────────────────────────
/// @note カスタムパスは複数追加でき、同じ段の中では登録順に直列で繋がる。
/// @note 段をまたぐ順序は常に AfterOpaque → (半透明) → SceneHDR → Composite → PostProcess。
/// @note ─────────────────────────────────────────────────────────────────────────────

#include "Common/Constants.hlsli"
#include "Common/Fullscreen.hlsli"
#include "Common/Color.hlsli"
#include "Platform/Backend.hlsli"
#include "Rendering/PostProcess.hlsli"
#include "Common/BindlessIndices.hlsli"

/// @note 入力テクスチャ — t5 は RenderSystem が自動バインドする LDR カラーバッファ
FBZZ_TEX2D(texInput, SRV_POST_COLOR_SLOT);
/// @note 全画面フェッチなので clamp 必須 (s0 は DX12 では WRAP)。
SamplerState sampLinear : register(SAMPLER_LINEAR_CLAMP);

/// @note ── 頂点シェーダー ─────────────────────────────────────────────────────────
/// @note 三頂点だけで画面全体を覆う fullscreen triangle を生成する。
/// @note 頂点バッファ不要で DrawCall.vertexCount = 3 のみで動作する。
FBZZFullscreenVertex VSMain(uint id : SV_VertexID)
{
    return FBZZMakeFullscreenVertex(id);
}

/// @note ── ピクセルシェーダー ─────────────────────────────────────────────────────
/// @note ここから下を自由にカスタマイズしてください。
/// @note customBlend と customIntensity を最終段で適用するパターンを守れば
/// @note Inspector/Script からのリアルタイム制御が自動で機能する。

float4 PSMain(FBZZFullscreenVertex p) : SV_Target0
{
    /// @note ---- 入力サンプリング ------------------------------------------------
    /// @note 入力を引く UV。downscale を使わないなら倍率は 1 なので、
    /// @note この掛け算は «常に書いておいて損が無い» 形になっている。
    const float2 inputUV = FBZZ_CustomInputUV(p.uv);
    const float3 original = texInput.SampleLevel(sampLinear, inputUV, 0).rgb;

    /// @note ---- パラメータ展開 --------------------------------------------------
    /// @note customParameters は Inspector の "Parameters" フィールド、または
    /// @note Script の SetCustomParameters("name", x, y, z, w) で設定する。
    /// @note 用途は自由。例として以下の意味を割り当てている:
    const float param0 = customParameters.x;  /// @note 例: エフェクト強度 A
    const float param1 = customParameters.y;  /// @note 例: エフェクト強度 B
    const float param2 = customParameters.z;  /// @note 例: 閾値 / 半径など
    const float param3 = customParameters.w;  /// @note 例: 汎用

    /// @note ====================================================================
    /// @note エフェクト実装セクション
    /// @note ここに処理を書く。最終的に float3 result に結果を入れて返す。
    /// @note 下に例として各種エフェクトをコメントアウトで示す。
    /// @note ====================================================================

    float3 result = original;  /// @note ← デフォルトはパススルー

    /// @note ---- 例 1: セピア ---------------------------------------------------
    /// @note result = ApplySepia(result, param0);

    /// @note ---- 例 2: ビネット -------------------------------------------------
    /// @note const float3 vignetteColor = float3(0.0f, 0.0f, 0.0f);
    /// @note result = ApplyVignette(result, p.uv, param0, 0.4f, 1.0f, vignetteColor);

    /// @note ---- 例 3: フィルムグレイン ------------------------------------------
    /// @note result = ApplyFilmGrain(result, p.uv, param0, param1);

    /// @note ---- 例 4: UV 歪み + クロマティックアベレーション --------------------
    /// @note const float2 uvR = LensDistortUV(p.uv, param0 * 0.01f) + texelSize * float2( 2.0f, 0.0f);
    /// @note const float2 uvB = LensDistortUV(p.uv, param0 * 0.01f) + texelSize * float2(-2.0f, 0.0f);
    /// @note result.r = texInput.SampleLevel(sampLinear, uvR, 0).r;
    /// @note result.b = texInput.SampleLevel(sampLinear, uvB, 0).b;

    /// @note ---- 例 5: アウトライン (Sobel エッジ検出) ---------------------------
    /// @note const float3 n  = texInput.SampleLevel(sampLinear, FBZZ_CustomInputUV(p.uv + float2(0, 1) * texelSize), 0).rgb;
    /// @note const float3 s  = texInput.SampleLevel(sampLinear, FBZZ_CustomInputUV(p.uv + float2(0, -1) * texelSize), 0).rgb;
    /// @note const float3 e  = texInput.SampleLevel(sampLinear, FBZZ_CustomInputUV(p.uv + float2(1, 0) * texelSize), 0).rgb;
    /// @note const float3 w  = texInput.SampleLevel(sampLinear, FBZZ_CustomInputUV(p.uv + float2(-1, 0) * texelSize), 0).rgb;
    /// @note const float3 gx = e - w;
    /// @note const float3 gy = n - s;
    /// @note const float  edge = saturate(length(float2(dot(gx, gx), dot(gy, gy))) * param0);
    /// @note result = lerp(result, float3(param1, param2, param3), edge);

    /// @note ---- 例 6: スキャンライン --------------------------------------------
    /// @note NOTE: line / point / sample / triangle は HLSL の補間モディファイア。変数名にすると DXC が弾く。
    /// @note const float scan = 0.5f + 0.5f * sin((p.uv.y * screenSize.y + time * 60.0f) * 3.14159f);
    /// @note result *= lerp(1.0f, scan, saturate(param0));

    /// @note ---- 例 7: 時間アニメーション (pulse glow) ---------------------------
    /// @note const float pulse = 0.5f + 0.5f * sin(time * param0 * 6.283f);
    /// @note result += Luminance(result) * pulse * param1;

    /// @note ---- 例 8: ピクセル化 -----------------------------------------------
    /// @note const float2 pixUV = ApplyPixelateUV(p.uv, max(param0 * 16.0f, 1.0f));
    /// @note result = texInput.SampleLevel(sampLinear, pixUV, 0).rgb;

    /// @note ====================================================================
    /// @note 最終合成 — 変更禁止推奨
    /// @note customIntensity でエフェクト強度を調整し、
    /// @note customBlend で元画像とブレンドしてから返す。
    /// @note ====================================================================
    const float3 effected = lerp(original, result, saturate(customIntensity));
    return float4(lerp(original, effected, saturate(customBlend)), 1.0f);
}
