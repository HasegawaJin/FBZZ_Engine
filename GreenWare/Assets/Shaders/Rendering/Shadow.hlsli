/// @file    Shadow.hlsli
/// @brief   PCF と PCSS の影サンプリング。
/// @author  Hasegawa Jin
/// @date    2026-06-23
#ifndef SHADOW_HLSLI
#define SHADOW_HLSLI

#include "Common/Space.hlsli"
#include "Rendering/CloudShadow.hlsli"

/// @note 雲シャドウ (Phase C) フィールドを宣言しないシェーダー (Terrain/Water 等インライン ShadowConstants) 向け
/// @note フォールバック。Constants.hlsli が HAVE_CLOUD_SHADOW を立てたシェーダーは本ブロックを飛ばし CB から読む。
#ifndef HAVE_CLOUD_SHADOW
/// @note 無効
static const float cloudShadowStrength = 0.0f;
static const float cloudShadowCoverage = 0.5f;
static const float cloudShadowScale    = 0.02f;
static const float cloudShadowSpeed    = 1.0f;
static const float cloudShadowTime     = 0.0f;
static const float cloudShadowWindX    = 1.0f;
static const float cloudShadowWindZ    = 0.3f;
#endif

/// @note 現在のフラグメントに乗せる雲影透過率 (worldPos.xz から)。strength=0 のとき 1.0。
float SampleCloudShadow(float3 worldPos)
{
    return CloudShadowFactor(worldPos.xz, cloudShadowStrength, cloudShadowCoverage,
                             cloudShadowScale, float2(cloudShadowWindX, cloudShadowWindZ),
                             cloudShadowSpeed, cloudShadowTime);
}

/// @note ShadowConstants を宣言しないシェーダー向けのカスケードフォールバック。
/// @note ComputeShadow は cascadeCount で単一マップ経路と CSM 経路を切り替える。
/// @note cascadeCount を持たないシェーダーでは 1 (= 従来の単一シャドウマップ) に落とす。
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
static const float4 cascadeSplitFar = float4(1, 1, 1, 1);
static const float4 shadowCameraPosition = float4(0, 0, 0, 0);
static const float4 shadowCameraForward = float4(0, 0, 1, 0);
#endif

/// @note AdvancedGraphicsConstants(b8) を宣言していないシェーダー向けフォールバック。
/// @note HLSL コンパイラはエントリポイントから到達できない関数でも全ボディを検証するため、
/// @note pcssEnabled / pcssLightRadius が未定義だとコンパイルエラーになる。
/// @note Terrain や Water 等 b8 を持たないシェーダーでは static const でデフォルト値を提供し、
/// @note PCSS を PCF にフォールバックさせる（Constants.hlsli が define を立てたシェーダーは本ブロックを飛ばす）。
#ifndef HAVE_ADVANCED_GRAPHICS_CB
/// @note PCF にフォールバック
static const int   pcssEnabled     = 0;
/// @note 未使用（pcssEnabled=0 のため）
static const float pcssLightRadius = 1.0f;
#endif

/// @note =========================================================================
/// @note PCF (Percentage Closer Filtering) シャドウ
/// @note shadowMap    : Texture2D<float> — シャドウデプスバッファ
/// @note shadowSampler: SamplerComparisonState (LESS_EQUAL / BORDER=1.0)
/// @note uv           : シャドウマップ UV [0,1]
/// @note depth        : ライト空間の深度値 - バイアス (比較基準)
/// @note texelSize    : 1.0 / シャドウマップ解像度
/// @note radius       : PCF カーネル半径 (1 = 3x3, 2 = 5x5)
/// @note 戻り値        : 0.0=完全に影, 1.0=完全に照らされている

/// @note 通常 Z の比較は receiver-bias <= stored なら 1 (lit)、blocker の stored が小さければ 0 (shadow)。
/// @see https://learn.microsoft.com/windows/win32/direct3dhlsl/dx-graphics-hlsl-to-samplecmplevelzero Microsoft — SampleCmpLevelZero.
/// @note =========================================================================
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

/// @note =========================================================================
/// @note カスケードシャドウ (CSM)
/// @note =========================================================================
/// @note 全カスケードは 1 枚の深度テクスチャを 2x2 に区切って共有する (アトラス)。
/// @note サンプル側の流れ:
/// @note 1. カメラ前方距離と分割遠端で担当カスケードを選ぶ
/// @note 2. 担当ライト行列で投影し、範囲外なら遠方カスケードを試す
/// @note 3. カスケード内 UV を cascadeAtlasRect でアトラス UV へ写してからテクスチャを引く

/// @note アトラスを Texture2DArray より優先する理由:
/// @note 深度テクスチャが 1 本のままなので、シャドウを読む 20 以上のシェーダーが
/// @note バインドもサンプラーも一切変えずに済む。スライス DSV を作るための
/// @note バックエンド追加実装 (DX11 / DX12 両方) も要らない。

/// @note CascadeUVToAtlas — カスケード内 UV [0,1] をアトラス全体の UV へ写す。
float2 CascadeUVToAtlas(float2 uv, float4 atlasRect)
{
    return atlasRect.xy + uv * atlasRect.zw;
}

/// @note CascadeBiasAt — cascadeBias (float4) から index 番目を取り出す。
/// @note ベクトルへの動的インデックスは HLSL では成分ごとの選択に展開され、
/// @note コンパイラやモデルによって扱いが揺れる。明示的な分岐で書いて挙動を固定する。
float CascadeBiasAt(int index)
{
    if (index <= 0) return cascadeBias.x;
    if (index == 1) return cascadeBias.y;
    if (index == 2) return cascadeBias.z;
    return cascadeBias.w;
}

/// @note SampleShadowCascadePCF — カスケード 1 枚ぶんの PCF。
/// @note PCF はカーネル半径ぶん周囲を舐めるため、タイル端で隣のカスケードへはみ出す。
/// @note アトラスでは隣が「別の深度」なので、はみ出すと帯状の誤った影が出る。
/// @note サンプル範囲をタイル内側へクランプして漏れを断つ。
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

/// @brief 視点からの前方距離が属するカスケードを選ぶ。
/// @note outUV    : そのカスケード内の UV [0,1]
/// @note outDepth : ライト空間の深度
/// @note outEdge  : カスケード端への近さ [0,1] (1 = ちょうど端)。境界ブレンドに使う
/// @note 戻り値    : カスケード番号。どれにも入らない場合は -1

/// @note ライト空間の投影は距離帯を越えて重なるため、UV 内包順では地形上の境界がカメラ移動で跳ぶ。
float CascadeSplitAt(int index)
{
    if (index <= 0) return cascadeSplitFar.x;
    if (index == 1) return cascadeSplitFar.y;
    if (index == 2) return cascadeSplitFar.z;
    return cascadeSplitFar.w;
}
int SelectShadowCascade(float3 worldPos, out float2 outUV, out float outDepth, out float outEdge)
{
    outUV    = float2(0.0f, 0.0f);
    outDepth = 0.0f;
    outEdge  = 0.0f;

    const float viewDepth = dot(worldPos - shadowCameraPosition.xyz, shadowCameraForward.xyz);
    if (viewDepth < 0.0f || viewDepth > CascadeSplitAt(cascadeCount - 1))
        return -1;
    int selected = cascadeCount - 1;
    [loop]
    for (int split = 0; split < FBZZ_MAX_SHADOW_CASCADES; ++split)
    {
        if (split >= cascadeCount) break;
        if (viewDepth <= CascadeSplitAt(split)) { selected = split; break; }
    }
    /// @note 担当タイルの外なら遠方タイルを試す。シーン境界でのフィットによる欠けを防ぐ。
    for (int i = 0; i < FBZZ_MAX_SHADOW_CASCADES; ++i)
    {
        if (i >= cascadeCount) break;
        if (i < selected) continue;

        float2 uv;
        float  depth;
        WorldToShadowUV(worldPos, cascadeViewProjection[i], uv, depth);

        if (any(uv < 0.0f) || any(uv > 1.0f) || depth < 0.0f || depth > 1.0f)
            continue;

        outUV    = uv;
        outDepth = depth;
        const float previousSplit = i == 0 ? 0.0f : CascadeSplitAt(i - 1);
        outEdge = saturate((viewDepth - previousSplit) / max(CascadeSplitAt(i) - previousSplit, 1e-4f));
        return i;
    }
    return -1;
}

/// @note ApplySlopeScaledBias — 斜め面で tan(theta) に比例してバイアスを増やす。
/// @note 面がライトに対して寝ているほど 1 テクセル内の深度差が大きく、固定バイアスでは
/// @note アクネか Peter Panning のどちらかが必ず出る。
float ApplySlopeScaledBias(float bias, float3 N, float3 L)
{
    float NdotL = saturate(dot(N, L));
    float slope = sqrt(1.0f - NdotL * NdotL) / max(NdotL, 1e-4f);
    return clamp(bias + bias * slope, bias, bias * 6.0f);
}

/// @note カスケード可視化色 (デバッグ)。緑=最も細かい → 赤=最も粗い。
static const float3 FBZZ_CASCADE_DEBUG_COLOR[FBZZ_MAX_SHADOW_CASCADES] = {
    float3(0.35f, 1.00f, 0.35f),
    float3(1.00f, 0.95f, 0.35f),
    float3(1.00f, 0.60f, 0.30f),
    float3(1.00f, 0.35f, 0.35f),
};

/// @note ShadowCascadeDebugTint — cascadeDebugView が有効なときに乗算する色。無効なら白。
/// @note 分割位置 (cascadeSplitLambda) と境界ブレンド幅は数値だけでは詰められない。
/// @note どこで切り替わっているかを画面に出すのが唯一の実用的な調整手段。
float3 ShadowCascadeDebugTint(float3 worldPos)
{
    if (cascadeDebugView == 0) return float3(1.0f, 1.0f, 1.0f);

    float2 uv; float depth; float edge;
    int index = SelectShadowCascade(worldPos, uv, depth, edge);
    /// @note どのカスケードにも入らない範囲
    if (index < 0) return float3(0.45f, 0.45f, 0.55f);
    return FBZZ_CASCADE_DEBUG_COLOR[index];
}

/// @note =========================================================================
/// @note ComputeShadow — ワールド座標からシャドウ係数を計算する
/// @note N, L を受け取りスロープスケールバイアスを適用して Self-Shadow アクネを防ぐ。
/// @note ライト錐台外 / 全カスケード外は 1.0 (照らされている) を返す。

/// @note lightVP / bias は cascadeCount <= 1 (単一シャドウマップ) のときに使う。
/// @note CSM 有効時は ShadowConstants のカスケード配列が優先され、これらは参照されない。
/// @note 体積光やパーティクル自己影のように、カスケードを持たない
/// @note 独自のライト行列で影を引きたい経路が実際にあるため。
/// @note =========================================================================
float ComputeShadow(Texture2D<float> shadowMap,
                    SamplerComparisonState shadowSampler,
                    float3 worldPos, float4x4 lightVP,
                    float2 texelSize, float bias,
                    float3 N, float3 L)
{
    /// @note 雲影は頭上の雲によるもので、シャドウマップ (直接遮蔽) とは独立。錐台外でも乗せる。
    float cloud = SampleCloudShadow(worldPos);

    /// @note 影の濃さが 0 なら、どんな factor が返っても lerp(1, 1, factor) = 1 で結果は cloud のまま。
    /// @note それでも PCF ループは毎ピクセル回っていた (既定 7x7 = 49 タップ)。
    /// @note 1080p なら 1 億回のテクスチャフェッチが、絵に一切影響しないまま消費される。
    /// @note Shadow を切ったときに実際に速くなるようにするための早期 return でもある。
    if (shadowStrength <= 0.0f)
        return cloud;

    /// @note ---- 単一シャドウマップ (従来経路) ----
    if (cascadeCount <= 1)
    {
        float2 uv;
        float  depth;
        WorldToShadowUV(worldPos, lightVP, uv, depth);

        /// @note ライト錐台の外は直接影なし (ただし雲影は乗せる)
        if (any(uv < 0.0f) || any(uv > 1.0f))
            return cloud;

        float adjustedBias = ApplySlopeScaledBias(bias, N, L);
        float factor = SampleShadowPCF(shadowMap, shadowSampler, uv,
                                       depth - adjustedBias, texelSize, shadowPcfRadius);
        /// @note shadowStrength: 1=完全な影, 0=影なし。factor=0(影) の時に (1-strength) を最小値とする。
        return lerp(1.0f - shadowStrength, 1.0f, factor) * cloud;
    }

    /// @note ---- カスケードシャドウ ----
    float2 uv;
    float  depth;
    float  edge;
    int    index = SelectShadowCascade(worldPos, uv, depth, edge);
    if (index < 0)
        /// @note 影の到達距離の外
        return cloud;

    float cascadeBiasValue = CascadeBiasAt(index);
    float adjustedBias     = ApplySlopeScaledBias(cascadeBiasValue, N, L);
    float factor = SampleShadowCascadePCF(shadowMap, shadowSampler, uv, depth - adjustedBias,
                                          cascadeAtlasRect[index], texelSize, shadowPcfRadius);

    /// @note ---- カスケード境界のクロスフェード ----
    /// @note カスケードをまたぐとテクセル密度が跳ぶため、境界に沿った不連続な線が
    /// @note 地面を横切って見える。端付近では次のカスケードの結果と混ぜて線を消す。
    if (cascadeBlend > 0.0f && index + 1 < cascadeCount)
    {
        float blendStart = 1.0f - cascadeBlend;
        if (edge > blendStart)
        {
            float2 nextUV;
            float  nextDepth;
            WorldToShadowUV(worldPos, cascadeViewProjection[index + 1], nextUV, nextDepth);

            if (all(nextUV >= 0.0f) && all(nextUV <= 1.0f) &&
                nextDepth >= 0.0f && nextDepth <= 1.0f)
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


/// @note ============================================================

/// @brief 現在の受光面をアトラス UV に投影した深度勾配を求める。
/// @pre lightVP は方向光の正射影。位置の微分はカスケード選択より前に求めること。
/// @return 退化・非有限・1 テクセル内で深度範囲の 1% を超える面は false。
/// @see https://learn.microsoft.com/windows/win32/dxtecharts/cascaded-shadow-maps#calculating-a-per-texel-depth-bias-with-ddx-and-ddy-for-large-pcfs Microsoft — receiver plane depth bias.
bool ShadowReceiverPlaneGradient(float3 worldPositionDx, float3 worldPositionDy,
                                 float4x4 lightVP, float4 atlasRect, float2 texelSize,
                                 out float2 depthGradient)
{
    depthGradient = float2(0.0f, 0.0f);
    const float3 lightDx = mul(float4(worldPositionDx, 0.0f), lightVP).xyz;
    const float3 lightDy = mul(float4(worldPositionDy, 0.0f), lightVP).xyz;
    if (!all(isfinite(lightDx)) || !all(isfinite(lightDy)))
        return false;
    const float2 uvDx = lightDx.xy * float2(0.5f, -0.5f) * atlasRect.zw;
    const float2 uvDy = lightDy.xy * float2(0.5f, -0.5f) * atlasRect.zw;
    const float determinant = uvDx.x * uvDy.y - uvDx.y * uvDy.x;
    const float determinantScale = length(uvDx) * length(uvDy);
    /// @note 絶対 epsilon は遠方の正常な小さい UV 勾配まで無効にするため、逆行列の条件を相対値で判定する。
    if (determinantScale <= 0.0f || abs(determinant) <= determinantScale * 1e-4f)
        return false;
    const float2 gradient = float2(
        uvDy.y * lightDx.z - uvDx.y * lightDy.z,
        uvDx.x * lightDy.z - uvDy.x * lightDx.z) / determinant;
    /// @note シルエットの深度不連続やライトへ平行に近い面を、広域の影を消す補正へ拡大しない。
    if (!all(isfinite(gradient)) || dot(abs(gradient), texelSize) > 0.01f)
        return false;
    depthGradient = gradient;
    return true;
}

/// @brief PCF の各比較深度を、隣接テクセル位置における受光面の深度へ合わせる。
/// @note 勾配が無効なら従来の slope bias へ戻る。有効な面の補正は材質の法線マップに依存しない。
/// @see https://learn.microsoft.com/windows/win32/dxtecharts/cascaded-shadow-maps#depth-bias Microsoft — large PCF self-shadowing.
float SampleShadowSurfacePCF(Texture2D<float> shadowMap,
                             SamplerComparisonState shadowSampler,
                             float2 uv, float depth, float4 atlasRect, float2 texelSize,
                             float bias, float3 N, float3 L, float4x4 lightVP,
                             float3 worldPositionDx, float3 worldPositionDy)
{
    float2 depthGradient;
    const bool hasReceiverPlane = ShadowReceiverPlaneGradient(worldPositionDx, worldPositionDy,
        lightVP, atlasRect, texelSize, depthGradient);
    /// @note ハードウェアの線形比較は 4 テクセルへ同じ基準を渡すため、tap 内の最大 1 テクセル差だけ bias で覆う。
    const float adjustedBias = hasReceiverPlane
        ? bias + dot(abs(depthGradient), texelSize)
        : ApplySlopeScaledBias(bias, N, L);
    const float2 centerUV = CascadeUVToAtlas(uv, atlasRect);
    const float2 inset = texelSize * (float(shadowPcfRadius) + 1.0f);
    const float2 uvMin = atlasRect.xy + inset;
    const float2 uvMax = atlasRect.xy + atlasRect.zw - inset;
    float shadow = 0.0f;
    float total = 0.0f;
    for (int y = -shadowPcfRadius; y <= shadowPcfRadius; ++y)
    for (int x = -shadowPcfRadius; x <= shadowPcfRadius; ++x)
    {
        const float2 sampleUV = clamp(centerUV + float2(x, y) * texelSize, uvMin, uvMax);
        /// @note クランプした tap の実際の UV 差を使い、タイル端でも深度勾配と同じ単位で比較する。
        const float sampleDepth = depth - adjustedBias + dot(sampleUV - centerUV, depthGradient);
        shadow += shadowMap.SampleCmpLevelZero(shadowSampler, sampleUV, sampleDepth);
        total += 1.0f;
    }
    return shadow / total;
}

/// @brief 幾何の微分がある不透明受光面に receiver plane 補正を適用する。
/// @pre worldPositionDx / Dy は PS 内の分岐・discard・カスケード選択より前に求めること。
/// @note 水面・体積光の既存 ComputeShadow はこの経路へ切り替えない。
/// @see Docs/design/terrain-layers.md
float ComputeShadowSurface(Texture2D<float> shadowMap,
                           SamplerComparisonState shadowSampler,
                           float3 worldPos, float4x4 lightVP,
                           float2 texelSize, float bias, float3 N, float3 L,
                           float3 worldPositionDx, float3 worldPositionDy)
{
    const float cloud = SampleCloudShadow(worldPos);
    if (shadowStrength <= 0.0f)
        return cloud;
    if (cascadeCount <= 1)
    {
        float2 uv;
        float depth;
        WorldToShadowUV(worldPos, lightVP, uv, depth);
        if (any(uv < 0.0f) || any(uv > 1.0f) || depth < 0.0f || depth > 1.0f)
            return cloud;
        const float factor = SampleShadowSurfacePCF(shadowMap, shadowSampler, uv, depth,
            float4(0.0f, 0.0f, 1.0f, 1.0f), texelSize, bias, N, L, lightVP,
            worldPositionDx, worldPositionDy);
        return lerp(1.0f - shadowStrength, 1.0f, factor) * cloud;
    }

    float2 uv;
    float depth;
    float edge;
    const int index = SelectShadowCascade(worldPos, uv, depth, edge);
    if (index < 0)
        return cloud;
    float factor = SampleShadowSurfacePCF(shadowMap, shadowSampler, uv, depth,
        cascadeAtlasRect[index], texelSize, CascadeBiasAt(index), N, L,
        cascadeViewProjection[index], worldPositionDx, worldPositionDy);
    if (cascadeBlend > 0.0f && index + 1 < cascadeCount)
    {
        const float blendStart = 1.0f - cascadeBlend;
        if (edge > blendStart)
        {
            float2 nextUV;
            float nextDepth;
            WorldToShadowUV(worldPos, cascadeViewProjection[index + 1], nextUV, nextDepth);
            if (all(nextUV >= 0.0f) && all(nextUV <= 1.0f) &&
                nextDepth >= 0.0f && nextDepth <= 1.0f)
            {
                const float nextFactor = SampleShadowSurfacePCF(shadowMap, shadowSampler,
                    nextUV, nextDepth, cascadeAtlasRect[index + 1], texelSize,
                    CascadeBiasAt(index + 1), N, L, cascadeViewProjection[index + 1],
                    worldPositionDx, worldPositionDy);
                const float blend = saturate((edge - blendStart) / max(cascadeBlend, 1e-4f));
                factor = lerp(factor, nextFactor, blend);
            }
        }
    }
    return lerp(1.0f - shadowStrength, 1.0f, factor) * cloud;
}

/// @note PCSS (Percentage Closer Soft Shadows)
/// @note ============================================================
/// @note ComputeShadowPCSS — PCSS アルゴリズムによるソフトシャドウ計算。
/// @note pcssEnabled == 0 の場合は通常 PCF にフォールバックする。

/// @note アルゴリズム:
/// @note 1. Blocker Search: searchRadius 範囲のシャドウマップを Poisson ディスクで
/// @note サンプルし、遮蔽ブロッカーの平均深度 avgBlockerDepth を算出する。
/// @note 2. Penumbra Size: 受光点の深度と平均ブロッカー深度の差から半影幅を推定する。
/// @note penumbraWidth = (receiverDepth - avgBlockerDepth) * pcssLightRadius / avgBlockerDepth
/// @note 3. PCF: penumbraWidth に比例したカーネルサイズで SampleShadowPCF を呼ぶ。

/// @note 16点 Poisson ディスクサンプル (正規化済み [-1, 1])
/// @note ランダムサンプルより均一分布で品質安定、DX11 SM5.0 でもコンパイル可能なハードコード
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

/// @note =========================================================================
/// @note FindBlockerDepth — PCSS ステップ 1: ブロッカー探索
/// @note shadowMap     : シャドウデプスバッファ (SampleLevel 用)
/// @note pointSampler  : 通常 SamplerState (比較なし)
/// @note uv            : シャドウマップ UV
/// @note receiverDepth : 受光点の深度値
/// @note texelSize     : 1.0 / シャドウマップ解像度
/// @note searchRadius  : 探索半径 (テクセル単位)
/// @note atlasRect     : 探索を許すアトラス矩形 (xy=UV オフセット, zw=UV スケール)。
/// @note 単一シャドウマップのときは (0,0,1,1) を渡す
/// @note 戻り値         : 遮蔽ブロッカーの平均深度、見つからない場合は -1.0

/// @note uv はタイル内 UV [0,1]。アトラスへの写像と矩形内クランプはこの関数が行う。
/// @note クランプしないと隣のカスケードの深度をブロッカーとして拾ってしまう。
/// @note =========================================================================
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

        /// @note 受光点より手前にあるテクセルがブロッカー
        if (blockerDepth < receiverDepth)
        {
            blockerSum += blockerDepth;
            blockerCount++;
        }
    }

    return (blockerCount > 0) ? (blockerSum / float(blockerCount)) : -1.0f;
}

/// @note =========================================================================
/// @note ComputeShadowPCSS — ワールド座標から PCSS シャドウ係数を計算する
/// @note shadowMap     : Texture2D<float> シャドウデプスバッファ
/// @note shadowSampler : SamplerComparisonState (比較フィルタ用)
/// @note pointSampler  : SamplerState (ブロッカー探索用の通常サンプラー)
/// @note worldPos      : フラグメントのワールド座標
/// @note lightVP       : ライトのビュープロジェクション行列
/// @note texelSize     : 1.0 / シャドウマップ解像度
/// @note bias          : 深度オフセットバイアス
/// @note N             : 頂点法線 (ワールド空間、正規化済み)
/// @note L             : ライト方向 (ワールド空間、正規化済み)
/// @note 戻り値         : 0.0=完全に影, 1.0=完全に照らされている

/// @note pcssEnabled / pcssLightRadius / shadowStrength は
/// @note AdvancedGraphicsConstants(b8) / ShadowConstants(b4) から参照する。
/// @note 呼び出し元シェーダーで両 cbuffer を宣言しておくこと。
/// @note =========================================================================
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
    /// @note pcssEnabled == 0 なら通常 PCF にフォールバック (ComputeShadow 側で雲影も適用される)
    if (pcssEnabled == 0)
    {
        return ComputeShadow(shadowMap, shadowSampler, worldPos, lightVP, texelSize, bias, N, L);
    }

    /// @note 雲影 (頭上の雲・直接遮蔽とは独立)。全 return 経路に乗せる。
    float cloud = SampleCloudShadow(worldPos);

    /// @note 影の濃さが 0 なら結果は cloud で確定する。ブロッカー探索も PCF も回さない。
    if (shadowStrength <= 0.0f)
        return cloud;

    /// @note UV / 深度 / 使用するタイルとバイアスをカスケード構成に応じて決める。
    /// @note 単一マップ時はアトラス全面 (rect = 0,0,1,1) を 1 タイルとみなせば、
    /// @note 以降の処理をカスケード有無で分岐させずに書ける。
    float2 uv;
    float  receiverDepth;
    float4 atlasRect  = float4(0.0f, 0.0f, 1.0f, 1.0f);
    float  activeBias = bias;

    if (cascadeCount <= 1)
    {
        WorldToShadowUV(worldPos, lightVP, uv, receiverDepth);
        if (any(uv < 0.0f) || any(uv > 1.0f))
            /// @note ライト錐台外は直接影なし (雲影は乗せる)
            return cloud;
    }
    else
    {
        float edge;
        int   index = SelectShadowCascade(worldPos, uv, receiverDepth, edge);
        if (index < 0)
            /// @note 影の到達距離の外
            return cloud;
        atlasRect  = cascadeAtlasRect[index];
        activeBias = CascadeBiasAt(index);
    }

    /// @note スロープスケールバイアスで Self-Shadow アクネを防ぐ
    receiverDepth -= ApplySlopeScaledBias(activeBias, N, L);

    /// @note ---- ステップ 1: ブロッカー探索 ----
    /// @note pcssLightRadius が大きいほど広い範囲でブロッカーを探し、より広い半影を生成する
    /// @note world-space → テクセル空間の近似スケール
    float searchRadius = pcssLightRadius * 10.0f;
    float avgBlocker   = FindBlockerDepth(shadowMap, pointSampler,
                                          uv, receiverDepth, texelSize, searchRadius, atlasRect);

    /// @note ブロッカーなし = 直接照射 (雲影は乗せる)
    if (avgBlocker < 0.0f)
        return cloud;

    /// @note ---- ステップ 2: 半影幅の推定 ----
    /// @note 受光点の深度とブロッカー深度の差が大きいほど半影が広がる
    /// @note 式: penumbraWidth = (d_receiver - d_blocker) * lightRadius / d_blocker
    float penumbraWidth = (receiverDepth - avgBlocker) * pcssLightRadius / max(avgBlocker, 1e-5f);

    /// @note テクセル単位のカーネル半径を算出 (最小 1, 最大 8)
    /// @note 上限 8 は SM5.0 でのループ展開コスト上限を考慮した実用値
    int pcfRadius = (int)clamp(penumbraWidth * 512.0f, 1.0f, 8.0f);

    /// @note ---- ステップ 3: 可変カーネル PCF ----
    float factor = SampleShadowCascadePCF(shadowMap, shadowSampler, uv, receiverDepth,
                                          atlasRect, texelSize, pcfRadius);

    return lerp(1.0f - shadowStrength, 1.0f, factor) * cloud;
}

/// @note SHADOW_HLSLI
#endif
