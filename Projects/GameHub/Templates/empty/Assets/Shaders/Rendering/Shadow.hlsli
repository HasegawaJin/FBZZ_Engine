// FBZZ Engine
// Shadow.hlsli | Rendering
// PCF シャドウサンプリング
#ifndef SHADOW_HLSLI
#define SHADOW_HLSLI

#include "Common/Space.hlsli"
#include "Rendering/CloudShadow.hlsli"

// 雲シャドウ (Phase C) フィールドを宣言しないシェーダー (Terrain/Water 等インライン ShadowConstants) 向け
// フォールバック。Constants.hlsli が HAVE_CLOUD_SHADOW を立てたシェーダーは本ブロックを飛ばし CB から読む。
#ifndef HAVE_CLOUD_SHADOW
static const float cloudShadowStrength = 0.0f; // 無効
static const float cloudShadowCoverage = 0.5f;
static const float cloudShadowScale    = 0.02f;
static const float cloudShadowSpeed    = 1.0f;
static const float cloudShadowTime     = 0.0f;
static const float cloudShadowWindX    = 1.0f;
static const float cloudShadowWindZ    = 0.3f;
#endif

// 現在のフラグメントに乗せる雲影透過率 (worldPos.xz から)。strength=0 のとき 1.0。
float SampleCloudShadow(float3 worldPos)
{
    return CloudShadowFactor(worldPos.xz, cloudShadowStrength, cloudShadowCoverage,
                             cloudShadowScale, float2(cloudShadowWindX, cloudShadowWindZ),
                             cloudShadowSpeed, cloudShadowTime);
}

// ShadowConstants を宣言しないシェーダー向けのカスケードフォールバック。
// WHY: ComputeShadow は cascadeCount で単一マップ経路と CSM 経路を切り替える。
//      cascadeCount を持たないシェーダーでは 1 (= 従来の単一シャドウマップ) に落とす。
#ifndef HAVE_SHADOW_CASCADES
#define FBZZ_MAX_SHADOW_CASCADES 4
static const int   cascadeCount     = 1;
static const float cascadeBlend     = 0.0f;
static const int   cascadeDebugView = 0;
static const float4x4 cascadeViewProjection[FBZZ_MAX_SHADOW_CASCADES] = {
    float4x4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1),
    float4x4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1),
    float4x4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1),
    float4x4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1)
};
static const float4 cascadeAtlasRect[FBZZ_MAX_SHADOW_CASCADES] = {
    float4(0, 0, 1, 1), float4(0, 0, 1, 1), float4(0, 0, 1, 1), float4(0, 0, 1, 1)
};
static const float4 cascadeBias = float4(0, 0, 0, 0);
#endif

// AdvancedGraphicsConstants(b8) を宣言していないシェーダー向けフォールバック。
// WHY: HLSL コンパイラはエントリポイントから到達できない関数でも全ボディを検証するため、
//      pcssEnabled / pcssLightRadius が未定義だとコンパイルエラーになる。
//      Terrain や Water 等 b8 を持たないシェーダーでは static const でデフォルト値を提供し、
//      PCSS を PCF にフォールバックさせる（Constants.hlsli が define を立てたシェーダーは本ブロックを飛ばす）。
#ifndef HAVE_ADVANCED_GRAPHICS_CB
static const int   pcssEnabled     = 0;    // PCF にフォールバック
static const float pcssLightRadius = 1.0f; // 未使用（pcssEnabled=0 のため）
#endif

// =========================================================================
// PCF (Percentage Closer Filtering) シャドウ
//   shadowMap    : Texture2D<float> — シャドウデプスバッファ
//   shadowSampler: SamplerComparisonState (GREATER_EQUAL / BORDER=1.0)
//   uv           : シャドウマップ UV [0,1]
//   depth        : ライト空間の深度値 - バイアス (比較基準)
//   texelSize    : 1.0 / シャドウマップ解像度
//   radius       : PCF カーネル半径 (1 = 3x3, 2 = 5x5)
//   戻り値        : 0.0=完全に影, 1.0=完全に照らされている
//
//   WHY GREATER_EQUAL:
//     SampleCmpLevelZero は "stored COMP compare" を評価する。
//     照らされているピクセル: stored ≈ receiver_depth >= receiver-bias → 1.0(lit) ✓
//     影のピクセル: stored = blocker_depth < receiver_depth → 0.0(shadow) ✓
// =========================================================================
float SampleShadowPCF(Texture2D<float> shadowMap,
                      SamplerComparisonState shadowSampler,
                      float2 uv, float depth, float2 texelSize, int radius)
{
    float shadow = 0.0f;
    float total  = 0.0f;

    for (int y = -radius; y <= radius; ++y)
    for (int x = -radius; x <= radius; ++x)
    {
        float2 offset = float2(x, y) * texelSize;
        shadow += shadowMap.SampleCmpLevelZero(shadowSampler, uv + offset, depth);
        total  += 1.0f;
    }

    return shadow / total;
}

// =========================================================================
// カスケードシャドウ (CSM)
// =========================================================================
// 全カスケードは 1 枚の深度テクスチャを 2x2 に区切って共有する (アトラス)。
// サンプル側の流れ:
//   1. カスケード i のライト行列でワールド座標を投影し、カスケード内 UV [0,1] を得る
//   2. 範囲内に収まる最も手前 (= 最も細かい) カスケードを選ぶ
//   3. カスケード内 UV を cascadeAtlasRect でアトラス UV へ写してからテクスチャを引く
//
// WHY アトラス (Texture2DArray ではなく):
//   深度テクスチャが 1 本のままなので、シャドウを読む 20 以上のシェーダーが
//   バインドもサンプラーも一切変えずに済む。スライス DSV を作るための
//   バックエンド追加実装 (DX11 / DX12 両方) も要らない。

// CascadeUVToAtlas — カスケード内 UV [0,1] をアトラス全体の UV へ写す。
float2 CascadeUVToAtlas(float2 uv, float4 atlasRect)
{
    return atlasRect.xy + uv * atlasRect.zw;
}

// CascadeBiasAt — cascadeBias (float4) から index 番目を取り出す。
// WHY: ベクトルへの動的インデックスは HLSL では成分ごとの選択に展開され、
//      コンパイラやモデルによって扱いが揺れる。明示的な分岐で書いて挙動を固定する。
float CascadeBiasAt(int index)
{
    if (index <= 0) return cascadeBias.x;
    if (index == 1) return cascadeBias.y;
    if (index == 2) return cascadeBias.z;
    return cascadeBias.w;
}

// SampleShadowCascadePCF — カスケード 1 枚ぶんの PCF。
// WHY inset: PCF はカーネル半径ぶん周囲を舐めるため、タイル端で隣のカスケードへはみ出す。
//      アトラスでは隣が「別の深度」なので、はみ出すと帯状の誤った影が出る。
//      サンプル範囲をタイル内側へクランプして漏れを断つ。
float SampleShadowCascadePCF(Texture2D<float> shadowMap,
                             SamplerComparisonState shadowSampler,
                             float2 uv, float depth, float4 atlasRect,
                             float2 atlasTexelSize, int radius)
{
    float2 inset = atlasTexelSize * (float(radius) + 1.0f);
    float2 uvMin = atlasRect.xy + inset;
    float2 uvMax = atlasRect.xy + atlasRect.zw - inset;

    float shadow = 0.0f;
    float total  = 0.0f;

    for (int y = -radius; y <= radius; ++y)
    for (int x = -radius; x <= radius; ++x)
    {
        float2 sampleUV = CascadeUVToAtlas(uv, atlasRect) + float2(x, y) * atlasTexelSize;
        sampleUV = clamp(sampleUV, uvMin, uvMax);
        shadow += shadowMap.SampleCmpLevelZero(shadowSampler, sampleUV, depth);
        total  += 1.0f;
    }

    return shadow / total;
}

// SelectShadowCascade — worldPos を含む最小のカスケードを選ぶ。
//   outUV    : そのカスケード内の UV [0,1]
//   outDepth : ライト空間の深度
//   outEdge  : カスケード端への近さ [0,1] (1 = ちょうど端)。境界ブレンドに使う
//   戻り値    : カスケード番号。どれにも入らない場合は -1
//
// WHY UV 内包判定で選ぶ (ビュー深度で選ぶのではなく):
//   ビュー空間深度で選ぶ方式は、シャドウを読む全シェーダーへビュー行列か
//   ビュー深度を配る必要がある。Terrain / Water のように独自 cbuffer を持つ
//   シェーダーまで巻き込むと配線が増えて壊れやすい。
//   カスケードは手前ほど狭いので、若い番号から見て最初に入ったものが常に最も細かい。
int SelectShadowCascade(float3 worldPos, out float2 outUV, out float outDepth, out float outEdge)
{
    outUV    = float2(0.0f, 0.0f);
    outDepth = 0.0f;
    outEdge  = 0.0f;

    // NOTE: 動的 break を含むため [unroll] は付けない。最大 4 回の動的ループで足りる。
    for (int i = 0; i < FBZZ_MAX_SHADOW_CASCADES; ++i)
    {
        if (i >= cascadeCount) break;

        float2 uv;
        float  depth;
        WorldToShadowUV(worldPos, cascadeViewProjection[i], uv, depth);

        if (any(uv < 0.0f) || any(uv > 1.0f) || depth < 0.0f || depth > 1.0f)
            continue;

        outUV    = uv;
        outDepth = depth;
        // タイル中心からの最大軸距離 [0,1]。端に近いほど 1 へ寄る。
        outEdge  = max(abs(uv.x * 2.0f - 1.0f), abs(uv.y * 2.0f - 1.0f));
        return i;
    }
    return -1;
}

// ApplySlopeScaledBias — 斜め面で tan(theta) に比例してバイアスを増やす。
// 面がライトに対して寝ているほど 1 テクセル内の深度差が大きく、固定バイアスでは
// アクネか Peter Panning のどちらかが必ず出る。
float ApplySlopeScaledBias(float bias, float3 N, float3 L)
{
    float NdotL = saturate(dot(N, L));
    float slope = sqrt(1.0f - NdotL * NdotL) / max(NdotL, 1e-4f);
    return clamp(bias + bias * slope, bias, bias * 6.0f);
}

// カスケード可視化色 (デバッグ)。緑=最も細かい → 赤=最も粗い。
static const float3 FBZZ_CASCADE_DEBUG_COLOR[FBZZ_MAX_SHADOW_CASCADES] = {
    float3(0.35f, 1.00f, 0.35f),
    float3(1.00f, 0.95f, 0.35f),
    float3(1.00f, 0.60f, 0.30f),
    float3(1.00f, 0.35f, 0.35f),
};

// ShadowCascadeDebugTint — cascadeDebugView が有効なときに乗算する色。無効なら白。
// WHY: 分割位置 (cascadeSplitLambda) と境界ブレンド幅は数値だけでは詰められない。
//      どこで切り替わっているかを画面に出すのが唯一の実用的な調整手段。
float3 ShadowCascadeDebugTint(float3 worldPos)
{
    if (cascadeDebugView == 0) return float3(1.0f, 1.0f, 1.0f);

    float2 uv; float depth; float edge;
    int index = SelectShadowCascade(worldPos, uv, depth, edge);
    if (index < 0) return float3(0.45f, 0.45f, 0.55f); // どのカスケードにも入らない範囲
    return FBZZ_CASCADE_DEBUG_COLOR[index];
}

// =========================================================================
// ComputeShadow — ワールド座標からシャドウ係数を計算する
//   N, L を受け取りスロープスケールバイアスを適用して Self-Shadow アクネを防ぐ。
//   ライト錐台外 / 全カスケード外は 1.0 (照らされている) を返す。
//
//   lightVP / bias は cascadeCount <= 1 (単一シャドウマップ) のときに使う。
//   CSM 有効時は ShadowConstants のカスケード配列が優先され、これらは参照されない。
//   WHY 引数に残す: 体積光やパーティクル自己影のように、カスケードを持たない
//       独自のライト行列で影を引きたい経路が実際にあるため。
// =========================================================================
float ComputeShadow(Texture2D<float> shadowMap,
                    SamplerComparisonState shadowSampler,
                    float3 worldPos, float4x4 lightVP,
                    float2 texelSize, float bias,
                    float3 N, float3 L)
{
    // 雲影は頭上の雲によるもので、シャドウマップ (直接遮蔽) とは独立。錐台外でも乗せる。
    float cloud = SampleCloudShadow(worldPos);

    // 影の濃さが 0 なら、どんな factor が返っても lerp(1, 1, factor) = 1 で結果は cloud のまま。
    // WHY: それでも PCF ループは毎ピクセル回っていた (既定 7x7 = 49 タップ)。
    //      1080p なら 1 億回のテクスチャフェッチが、絵に一切影響しないまま消費される。
    //      Shadow を切ったときに実際に速くなるようにするための早期 return でもある。
    if (shadowStrength <= 0.0f)
        return cloud;

    // ---- 単一シャドウマップ (従来経路) ----
    if (cascadeCount <= 1)
    {
        float2 uv;
        float  depth;
        WorldToShadowUV(worldPos, lightVP, uv, depth);

        // ライト錐台の外は直接影なし (ただし雲影は乗せる)
        if (any(uv < 0.0f) || any(uv > 1.0f))
            return cloud;

        float adjustedBias = ApplySlopeScaledBias(bias, N, L);
        float factor = SampleShadowPCF(shadowMap, shadowSampler, uv,
                                       depth - adjustedBias, texelSize, shadowPcfRadius);
        // shadowStrength: 1=完全な影, 0=影なし。factor=0(影) の時に (1-strength) を最小値とする。
        return lerp(1.0f - shadowStrength, 1.0f, factor) * cloud;
    }

    // ---- カスケードシャドウ ----
    float2 uv;
    float  depth;
    float  edge;
    int    index = SelectShadowCascade(worldPos, uv, depth, edge);
    if (index < 0)
        return cloud; // 影の到達距離の外

    float cascadeBiasValue = CascadeBiasAt(index);
    float adjustedBias     = ApplySlopeScaledBias(cascadeBiasValue, N, L);
    float factor = SampleShadowCascadePCF(shadowMap, shadowSampler, uv, depth - adjustedBias,
                                          cascadeAtlasRect[index], texelSize, shadowPcfRadius);

    // ---- カスケード境界のクロスフェード ----
    // WHY: カスケードをまたぐとテクセル密度が跳ぶため、境界に沿った不連続な線が
    //      地面を横切って見える。端付近では次のカスケードの結果と混ぜて線を消す。
    if (cascadeBlend > 0.0f && index + 1 < cascadeCount)
    {
        float blendStart = 1.0f - cascadeBlend;
        if (edge > blendStart)
        {
            float2 nextUV;
            float  nextDepth;
            WorldToShadowUV(worldPos, cascadeViewProjection[index + 1], nextUV, nextDepth);

            if (all(nextUV >= 0.0f) && all(nextUV <= 1.0f))
            {
                float nextBias   = ApplySlopeScaledBias(CascadeBiasAt(index + 1), N, L);
                float nextFactor = SampleShadowCascadePCF(
                    shadowMap, shadowSampler, nextUV, nextDepth - nextBias,
                    cascadeAtlasRect[index + 1], texelSize, shadowPcfRadius);

                float t = saturate((edge - blendStart) / max(cascadeBlend, 1e-4f));
                factor  = lerp(factor, nextFactor, t);
            }
        }
    }

    return lerp(1.0f - shadowStrength, 1.0f, factor) * cloud;
}


// ============================================================
// PCSS (Percentage Closer Soft Shadows)
// ============================================================
// ComputeShadowPCSS — PCSS アルゴリズムによるソフトシャドウ計算。
// pcssEnabled == 0 の場合は通常 PCF にフォールバックする。
//
// アルゴリズム:
//   1. Blocker Search: searchRadius 範囲のシャドウマップを Poisson ディスクで
//      サンプルし、遮蔽ブロッカーの平均深度 avgBlockerDepth を算出する。
//   2. Penumbra Size: 受光点の深度と平均ブロッカー深度の差から半影幅を推定する。
//      penumbraWidth = (receiverDepth - avgBlockerDepth) * pcssLightRadius / avgBlockerDepth
//   3. PCF: penumbraWidth に比例したカーネルサイズで SampleShadowPCF を呼ぶ。

// 16点 Poisson ディスクサンプル (正規化済み [-1, 1])
// WHY: ランダムサンプルより均一分布で品質安定、DX11 SM5.0 でもコンパイル可能なハードコード
static const float2 PCSS_POISSON_DISK[16] =
{
    float2(-0.9444691f, -0.1409346f),
    float2(-0.8174540f,  0.4730292f),
    float2(-0.5808706f, -0.7196234f),
    float2(-0.5117652f,  0.1507150f),
    float2(-0.2847428f, -0.3699391f),
    float2(-0.2332620f,  0.6887614f),
    float2(-0.0374701f,  0.1408028f),
    float2( 0.0491168f, -0.7641724f),
    float2( 0.1052420f, -0.1734053f),
    float2( 0.2195470f,  0.5095905f),
    float2( 0.3937028f, -0.5105020f),
    float2( 0.4552360f,  0.1927327f),
    float2( 0.5760750f, -0.0651610f),
    float2( 0.6477049f,  0.6349671f),
    float2( 0.8018500f, -0.3605010f),
    float2( 0.9350649f,  0.2291007f),
};

// =========================================================================
// FindBlockerDepth — PCSS ステップ 1: ブロッカー探索
//   shadowMap     : シャドウデプスバッファ (SampleLevel 用)
//   pointSampler  : 通常 SamplerState (比較なし)
//   uv            : シャドウマップ UV
//   receiverDepth : 受光点の深度値
//   texelSize     : 1.0 / シャドウマップ解像度
//   searchRadius  : 探索半径 (テクセル単位)
//   atlasRect     : 探索を許すアトラス矩形 (xy=UV オフセット, zw=UV スケール)。
//                   単一シャドウマップのときは (0,0,1,1) を渡す
//   戻り値         : 遮蔽ブロッカーの平均深度、見つからない場合は -1.0
//
//   NOTE: uv はタイル内 UV [0,1]。アトラスへの写像と矩形内クランプはこの関数が行う。
//         クランプしないと隣のカスケードの深度をブロッカーとして拾ってしまう。
// =========================================================================
float FindBlockerDepth(Texture2D<float> shadowMap,
                       SamplerState     pointSampler,
                       float2           uv,
                       float            receiverDepth,
                       float2           texelSize,
                       float            searchRadius,
                       float4           atlasRect)
{
    float blockerSum   = 0.0f;
    int   blockerCount = 0;

    float2 uvMin = atlasRect.xy + texelSize;
    float2 uvMax = atlasRect.xy + atlasRect.zw - texelSize;

    [unroll]
    for (int i = 0; i < 16; ++i)
    {
        float2 offset    = PCSS_POISSON_DISK[i] * searchRadius * texelSize;
        float2 sampleUV  = clamp(CascadeUVToAtlas(uv, atlasRect) + offset, uvMin, uvMax);
        float  blockerDepth = shadowMap.SampleLevel(pointSampler, sampleUV, 0).r;

        // 受光点より手前にあるテクセルがブロッカー
        if (blockerDepth < receiverDepth)
        {
            blockerSum += blockerDepth;
            blockerCount++;
        }
    }

    return (blockerCount > 0) ? (blockerSum / float(blockerCount)) : -1.0f;
}

// =========================================================================
// ComputeShadowPCSS — ワールド座標から PCSS シャドウ係数を計算する
//   shadowMap     : Texture2D<float> シャドウデプスバッファ
//   shadowSampler : SamplerComparisonState (比較フィルタ用)
//   pointSampler  : SamplerState (ブロッカー探索用の通常サンプラー)
//   worldPos      : フラグメントのワールド座標
//   lightVP       : ライトのビュープロジェクション行列
//   texelSize     : 1.0 / シャドウマップ解像度
//   bias          : 深度オフセットバイアス
//   N             : 頂点法線 (ワールド空間、正規化済み)
//   L             : ライト方向 (ワールド空間、正規化済み)
//   戻り値         : 0.0=完全に影, 1.0=完全に照らされている
//
//   NOTE: pcssEnabled / pcssLightRadius / shadowStrength は
//         AdvancedGraphicsConstants(b8) / ShadowConstants(b4) から参照する。
//         呼び出し元シェーダーで両 cbuffer を宣言しておくこと。
// =========================================================================
float ComputeShadowPCSS(Texture2D<float>       shadowMap,
                        SamplerComparisonState shadowSampler,
                        SamplerState           pointSampler,
                        float3                 worldPos,
                        float4x4               lightVP,
                        float2                 texelSize,
                        float                  bias,
                        float3                 N,
                        float3                 L)
{
    // pcssEnabled == 0 なら通常 PCF にフォールバック (ComputeShadow 側で雲影も適用される)
    if (pcssEnabled == 0)
    {
        return ComputeShadow(shadowMap, shadowSampler, worldPos, lightVP, texelSize, bias, N, L);
    }

    // 雲影 (頭上の雲・直接遮蔽とは独立)。全 return 経路に乗せる。
    float cloud = SampleCloudShadow(worldPos);

    // 影の濃さが 0 なら結果は cloud で確定する。ブロッカー探索も PCF も回さない。
    if (shadowStrength <= 0.0f)
        return cloud;

    // UV / 深度 / 使用するタイルとバイアスをカスケード構成に応じて決める。
    // 単一マップ時はアトラス全面 (rect = 0,0,1,1) を 1 タイルとみなせば、
    // 以降の処理をカスケード有無で分岐させずに書ける。
    float2 uv;
    float  receiverDepth;
    float4 atlasRect  = float4(0.0f, 0.0f, 1.0f, 1.0f);
    float  activeBias = bias;

    if (cascadeCount <= 1)
    {
        WorldToShadowUV(worldPos, lightVP, uv, receiverDepth);
        if (any(uv < 0.0f) || any(uv > 1.0f))
            return cloud; // ライト錐台外は直接影なし (雲影は乗せる)
    }
    else
    {
        float edge;
        int   index = SelectShadowCascade(worldPos, uv, receiverDepth, edge);
        if (index < 0)
            return cloud; // 影の到達距離の外
        atlasRect  = cascadeAtlasRect[index];
        activeBias = CascadeBiasAt(index);
    }

    // スロープスケールバイアスで Self-Shadow アクネを防ぐ
    receiverDepth -= ApplySlopeScaledBias(activeBias, N, L);

    // ---- ステップ 1: ブロッカー探索 ----
    // WHY: pcssLightRadius が大きいほど広い範囲でブロッカーを探し、より広い半影を生成する
    float searchRadius = pcssLightRadius * 10.0f; // world-space → テクセル空間の近似スケール
    float avgBlocker   = FindBlockerDepth(shadowMap, pointSampler,
                                          uv, receiverDepth, texelSize, searchRadius, atlasRect);

    // ブロッカーなし = 直接照射 (雲影は乗せる)
    if (avgBlocker < 0.0f)
        return cloud;

    // ---- ステップ 2: 半影幅の推定 ----
    // 受光点の深度とブロッカー深度の差が大きいほど半影が広がる
    // 式: penumbraWidth = (d_receiver - d_blocker) * lightRadius / d_blocker
    float penumbraWidth = (receiverDepth - avgBlocker) * pcssLightRadius / max(avgBlocker, 1e-5f);

    // テクセル単位のカーネル半径を算出 (最小 1, 最大 8)
    // WHY: 上限 8 は SM5.0 でのループ展開コスト上限を考慮した実用値
    int pcfRadius = (int)clamp(penumbraWidth * 512.0f, 1.0f, 8.0f);

    // ---- ステップ 3: 可変カーネル PCF ----
    float factor = SampleShadowCascadePCF(shadowMap, shadowSampler, uv, receiverDepth,
                                          atlasRect, texelSize, pcfRadius);

    return lerp(1.0f - shadowStrength, 1.0f, factor) * cloud;
}

#endif // SHADOW_HLSLI
