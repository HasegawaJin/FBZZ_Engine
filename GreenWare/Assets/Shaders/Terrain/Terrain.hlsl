// FBZZ Engine
// Terrain.hlsl | Terrain
// ハイトマップ地形の頂点・ピクセルシェーダー
//
// 定数バッファスロット:
//   b0 = CameraConstants   (per-frame)
//   b1 = TerrainCB         (per-chunk: world / WVP / layer tiling / normal strength)
//   b3 = LightConstants    (per-frame)
//
// テクスチャスロット:
//   t0 = スプラットマップ  RGBA8 (R=layer0, G=layer1, B=layer2, A=layer3)
//   t1 = layer0 ディフューズ
//   t2 = layer1 ディフューズ
//   t3 = layer2 ディフューズ
//   t4 = layer3 ディフューズ
//   t5 = layer0 法線
//   t6 = layer1 法線
//   t7 = layer2 法線
//   t8 = layer3 法線
//   t9  = layer0 AO/Roughness (R=AO, G=Roughness)
//   t10 = layer1 AO/Roughness
//   t11 = layer2 AO/Roughness
//   t12 = layer3 AO/Roughness
//   t13 = shadow depth
//
// サンプラースロット:
//   s0 = WRAP_ANISOTROPIC  ディフューズ用（タイリングあり）
//   s1 = BORDER_ZERO       shadow PCF 用比較サンプラー
//   s2 = CLAMP_LINEAR      スプラットマップ用（UV を [0,1] にクランプ）
//
// 頂点フォーマット (C++ 側 TerrainVertex と同期すること):
//   POSITION  : float3  offset  0  (12 bytes)
//   NORMAL    : float3  offset 12  (12 bytes)
//   TANGENT   : float3  offset 24  (12 bytes)
//   TEXCOORD0 : float2  offset 36  ( 8 bytes)
//   stride = 44 bytes

#include "Common/Binding.hlsli"

// ============================================================================
// 定数バッファ宣言
// WHY: Common/Constants.hlsli は b1 に ObjectConstants を定義するが、
//      地形は同スロットを TerrainCB として使うためここで直接宣言する。
// ============================================================================

cbuffer CameraConstants : register(CB_CAMERA)
{
    float4x4 view;
    float4x4 projection;
    float4x4 viewProjection;
    float4x4 invViewProjection;
    float3   cameraPos;
    float    nearZ;
    float    farZ;
    float    _camReserved;   // Water パスのみ waterSsrEnabled として使う枠
    float    isOrthographic; // 1 = 平行投影
    float    _camPad;
};

cbuffer TerrainCB : register(CB_OBJECT)
{
    float4x4 worldMatrix;           // テレインローカル → ワールド変換
    float4x4 wvpMatrix;            // テレインローカル → クリップ空間変換
    float4   layerTiling[4];       // xy = tilingX, tilingZ (per layer)
    float4   layerNormalStrength;  // xyzw = normalStrength (per layer)
    float4   layerMaterial[4];     // x=roughness, y=AO
    float4   layerTextureFlags;     // xyzw = AO/Roughness texture exists per layer
    float4   layerAutoHeight[4];    // x=minHeight, y=maxHeight, z=fade, w=enabled
    float4   layerAutoSlope[4];     // x=minSlope, y=maxSlope, z=fade, w=strength
    float4   weather;               // x=wetness, y=darkening, z=puddleAmount
};

#define MAX_POINT_LIGHTS 8
#define MAX_SPOT_LIGHTS  4

struct PointLightData { float3 position; float range; float3 color; float intensity; };
struct SpotLightData  {
    float3 position; float range;
    float3 direction; float innerCos;
    float3 color; float outerCos;
    float  intensity; float3 _pad;
};

cbuffer LightConstants : register(CB_LIGHT)
{
    float3         lightDir;       float _lightPad;
    float3         lightColor;     float lightIntensity;
    PointLightData pointLights[MAX_POINT_LIGHTS];
    SpotLightData  spotLights[MAX_SPOT_LIGHTS];
    int            pointLightCount;
    int            spotLightCount;
    float2         _lightPad2;
    float3         ambientColor;
    float          _ambientPad;
};

// AdvancedGraphicsConstants (b8) — IBL / 画面空間 AO / 接触影。定義は共有ヘッダーが持つ。
// WHY 地形にも要るか: SSAO / GTAO / 接触影を Forward でも効かせるため、
//     地形マテリアルが自分の画素で結果を引く。強度は b8 が運ぶ。
#include "Common/AdvancedGraphicsConstants.hlsli"
// ShadowConstants (b4) — カスケード配列を含むためレイアウトは 1 か所で定義する。
#include "Common/ShadowConstants.hlsli"

// WHY: LightConstants の ambientColor グローバルを参照するため cbuffer 宣言の後に include する。
#include "Rendering/Lighting.hlsli"
#include "Rendering/Shadow.hlsli"
#include "Rendering/Wetness.hlsli"
#include "Common/BindlessIndices.hlsli"

// ============================================================================
// テクスチャ・サンプラー宣言 (Phase 4)
//
// g_splatmap (t0):
//   RGBA8 スプラットマップ。各チャンネルが各レイヤーのブレンドウェイトを表す。
//   4 チャンネルの合計が 1 になるよう正規化してある（ペイントツールが保証）。
//   シェーダー側でも安全のため正規化する。
//
// g_diffuse[4] (t1-t4):
//   各レイヤーのアルベドテクスチャ。未設定レイヤーは 1×1 白テクスチャで代替。
//   WHY: シェーダーは常に [unroll] 4 レイヤー固定でブレンドする。
//        未設定レイヤーの splat ウェイトは 0 なので白テクスチャの寄与は 0 になる。
//        動的分岐を排除することで GPU パイプラインの効率を上げる。
//
// s0 = ディフューズ用 Wrap Anisotropic（タイリング UV で繰り返しサンプリング）
// s1 = シャドウ用 Comparison Border Zero（ライト錐台外を非遮蔽として扱う）
// s2 = スプラットマップ用 Clamp Linear（UV が [0,1] を超えた場合に端値を維持）
// ============================================================================
FBZZ_TEX2D(g_splatmap, 0);
/// @brief レイヤー i のテクスチャ。枠は旧 t1..t4 / t5..t8 / t9..t12 (TerrainRenderPass が textures[1..12] へ差す)。
/// @note テーブル撤去後は register(tN) の配列を束縛できないため、枠番号から添字を引く。
Texture2D TerrainDiffuse(uint i)     { return ResourceDescriptorHeap[NonUniformResourceIndex(FbzzPixelSlot(1u + i))]; }
Texture2D TerrainNormal(uint i)      { return ResourceDescriptorHeap[NonUniformResourceIndex(FbzzPixelSlot(5u + i))]; }
Texture2D TerrainAoRoughness(uint i) { return ResourceDescriptorHeap[NonUniformResourceIndex(FbzzPixelSlot(9u + i))]; }
FBZZ_TEX2D_T(float, g_shadowMap, 13);
// 地形レイヤーはワールド座標でタイリングするので wrap。異方性は x4。
// WHY x16 の s0 を使わないか: 地形は画面を広く覆い、レイヤーごとに diffuse / normal /
//     aoRoughness の 3 枚を引く。x16 のコストがそのまま枚数ぶん乗る。地面は視線に対して
//     浅い角度で伸びるため異方性は効くが、x4 で見た目はほぼ変わらず約 1/3 のコストで済む。
SamplerState g_sampler       : register(SAMPLER_WRAP_ANISO4);
SamplerComparisonState g_shadowSampler : register(SAMPLER_SHADOW);
// スプラットマップは地形 1 枚に 1:1 で貼るので clamp。端で繰り返すと対岸が滲む。
SamplerState g_samplerClamp  : register(SAMPLER_LINEAR_CLAMP);

// ============================================================================
// 頂点入力・補間構造体
// ============================================================================
struct TerrainVSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float3 tangent  : TANGENT;
    float2 uv       : TEXCOORD0;
};

struct TerrainPSInput
{
    float4 svPosition   : SV_POSITION;
    float3 worldPos     : TEXCOORD0;
    float3 worldNormal  : TEXCOORD1;
    float2 uv           : TEXCOORD2;
    float  localHeight  : TEXCOORD3;
    float3 worldTangent : TEXCOORD4;
};

// ============================================================================
// 頂点シェーダー
// ============================================================================
TerrainPSInput VSMain(TerrainVSInput v)
{
    TerrainPSInput o;
    o.svPosition   = mul(float4(v.position, 1.0f), wvpMatrix);
    float4 wpos4   = mul(float4(v.position, 1.0f), worldMatrix);
    o.worldPos     = wpos4.xyz;
    o.worldNormal  = normalize(mul(v.normal,  (float3x3)worldMatrix));
    o.worldTangent = normalize(mul(v.tangent, (float3x3)worldMatrix));
    o.uv           = v.uv;
    o.localHeight  = v.position.y;
    return o;
}

float TerrainBandMask(float value, float minValue, float maxValue, float fade)
{
    // WHAT: [minValue, maxValue] の範囲内を 1、範囲外を 0 に近づける滑らかなマスク。
    // WHY: 高さ・傾斜の境界を硬く切ると等高線状の境目が見えるため、fade 幅で自然に混ぜる。
    if (maxValue <= minValue)
        return 1.0f;

    float f = max(fade, 0.0001f);
    float lower = smoothstep(minValue, minValue + f, value);
    float upper = 1.0f - smoothstep(maxValue - f, maxValue, value);
    return saturate(lower * upper);
}

float4 ApplyAutoBlend(float4 paintedSplat, float localHeight, float slope)
{
    float4 autoWeight = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 autoStrength = float4(0.0f, 0.0f, 0.0f, 0.0f);

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        float heightMask = TerrainBandMask(
            localHeight,
            layerAutoHeight[i].x,
            layerAutoHeight[i].y,
            layerAutoHeight[i].z);
        float slopeMask = TerrainBandMask(
            slope,
            layerAutoSlope[i].x,
            layerAutoSlope[i].y,
            layerAutoSlope[i].z);
        float enabled = saturate(layerAutoHeight[i].w);
        autoStrength[i] = saturate(layerAutoSlope[i].w) * enabled;
        autoWeight[i] = heightMask * slopeMask * autoStrength[i];
    }

    float autoSum = autoWeight.r + autoWeight.g + autoWeight.b + autoWeight.a;
    if (autoSum <= 0.001f)
        return paintedSplat;

    autoWeight /= autoSum;
    float blend = saturate(max(max(autoStrength.r, autoStrength.g), max(autoStrength.b, autoStrength.a)));
    float4 result = lerp(paintedSplat, autoWeight, blend);
    float sum = result.r + result.g + result.b + result.a;
    return sum > 0.001f ? result / sum : paintedSplat;
}

// ============================================================================
// ピクセルシェーダー (スプラットマップ 4 レイヤーブレンド)
// ============================================================================
float4 PSMain(TerrainPSInput p) : SV_Target0
{
    // 幾何法線 (頂点補間) を TBN 基準に使う。法線マップ適用後の N はループで組み立てる。
    float3 Ng = normalize(p.worldNormal);
    float3 V = normalize(cameraPos - p.worldPos);
    float3 L = normalize(-lightDir);

    // ── スプラットマップからレイヤーウェイトを取得 ─────────────────
    // スプラットマップは Clamp サンプラーでサンプリングする。
    // WHY: UV が地形端に近い頂点でわずかに [0,1] をはみ出すことがあり、
    //      Wrap だと反対端の値を参照してブレンドが壊れる。
    float4 splat = g_splatmap.Sample(g_samplerClamp, p.uv);

    // 4 チャンネルの合計で正規化（ペイントツールが保証するが数値誤差を安全に処理）
    float wsum = splat.r + splat.g + splat.b + splat.a;
    if (wsum > 0.001f) splat /= wsum;
    float slope = saturate(1.0f - abs(Ng.y));
    splat = ApplyAutoBlend(splat, p.localHeight, slope);

    // ── 4 レイヤーのアルベドをウェイトブレンド ───────────────────
    // [unroll] を使って静的展開する。
    // WHY: ループ変数で Texture2D 配列を動的インデックスすると SM 5.0 では
    //      テクスチャフェッチが最適化されない場合がある。
    //      [unroll] で展開することで各テクスチャフェッチが独立したコンパイル済み
    //      命令になり、GPU パイプラインが並列フェッチを行いやすくなる。
    // TBN をループ前に一度だけ構築する (Gram-Schmidt 直交化)。
    // WHY: 補間後の worldTangent は法線と厳密には直交しないため、法線成分を除去してから組む。
    //      DDX/DDY の画面空間微分を使わず頂点タンジェントから復元し PS コストを抑える。
    float3 T = normalize(p.worldTangent - Ng * dot(Ng, p.worldTangent));
    float3 B = normalize(cross(Ng, T));

    // 画面空間の UV 勾配をループ外で一度だけ求める。
    // WHY: 下のレイヤーループは splat≈0 のレイヤーを動的分岐でスキップする。分岐内で通常の Sample を
    //      使うと、レイヤー境界でクアッド内制御フローが分岐したとき異方性/ミップ勾配が未定義になり
    //      継ぎ目のちらつきが出る。勾配を分岐外で計算し SampleGrad に渡せば、フェッチを省きつつ
    //      異方性フィルタ品質を Sample と同一に保てる (tiledUV = uv * tiling なので勾配も tiling 倍)。
    float2 duvdx = ddx(p.uv);
    float2 duvdy = ddy(p.uv);

    // NOTE: diffuse / AO-Roughness / normal の 3 枚を同一 tiledUV で同じループ内サンプルし、法線ブレンドを融合する。
    //       法線を別ループに分けないことで tiledUV 再計算とテクスチャキャッシュミスを削減する。
    float3 albedo    = float3(0.0f, 0.0f, 0.0f);
    float3 blendedN  = float3(0.0f, 0.0f, 0.0f);
    float  roughness = 0.0f;
    float  ao = 0.0f;
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        // 寄与ゼロのレイヤーはテクスチャフェッチ (diffuse/AO/normal の異方性 3 枚) を丸ごと省く。
        // WHY: 地形の大半のピクセルは 1〜2 レイヤーしか使わない。splat≈0 のレイヤーを飛ばすと、
        //      単一レイヤー領域では最大 9 枚のフェッチを削減でき、TerrainForward の主コストを大きく下げる。
        //      寄与は最終的に splat[i]≈0 倍されるため、スキップしても結果は数値的に等価。
        // NOTE: ここで `continue` を使うとデータ依存の制御フローが生まれ、FXC が [unroll] を
        //       展開できず g_diffuse[i] の i がリテラルにならない (X3512/X3511)。
        //       正の if ガードで囲むことで各反復が i=0..3 のリテラル添字に確実に展開される。
        if (splat[i] > 0.001f)
        {
            float2 tiledUV = p.uv * layerTiling[i].xy;
            float2 gradX   = duvdx * layerTiling[i].xy;
            float2 gradY   = duvdy * layerTiling[i].xy;
            float3 d       = TerrainDiffuse(i).SampleGrad(g_sampler, tiledUV, gradX, gradY).rgb;
            float2 aoRoughnessTex = TerrainAoRoughness(i).SampleGrad(g_sampler, tiledUV, gradX, gradY).rg;
            float3 tn      = TerrainNormal(i).SampleGrad(g_sampler, tiledUV, gradX, gradY).xyz * 2.0f - 1.0f;
            tn.xy *= layerNormalStrength[i];
            tn = normalize(tn);
            float hasAoRoughnessTex = saturate(layerTextureFlags[i]);
            float layerAO = lerp(saturate(layerMaterial[i].y), aoRoughnessTex.r, hasAoRoughnessTex);
            float layerRoughness = lerp(saturate(layerMaterial[i].x), aoRoughnessTex.g, hasAoRoughnessTex);
            albedo        += d * splat[i];
            roughness     += layerRoughness * splat[i];
            ao            += layerAO * splat[i];
            blendedN      += normalize(T * tn.x + B * tn.y + Ng * tn.z) * splat[i];
        }
    }

    float3 N = normalize(blendedN);

    // 天候。Deferred の TerrainGBuffer.hlsl と同じ値・同じ順序で掛ける。
    const WetSurface wet = ApplyWetness(albedo, roughness, N,
                                        weather.x, weather.y, weather.z);
    albedo    = wet.albedo;
    roughness = wet.roughness;

    float shadow = ComputeShadow(g_shadowMap, g_shadowSampler, p.worldPos,
        lightViewProjection, shadowMapTexelSize, shadowBias, N, L);
    // Forward の画面空間 AO / 接触影。Deferred では b8 が 0 なので素通りする。
    shadow *= FBZZ_ScreenContactShadow(p.svPosition.xy);
    // 地形のレイヤー AO と画面空間 AO は別物 (材質の凹凸 と 形状同士の遮蔽) なので掛け合わせる。
    ao *= FBZZ_ScreenAO(p.svPosition.xy);

    // AO は間接光（環境光）成分のみを遮蔽する物理的に正しい扱いにする。
    // WHY: 以前は最終結果全体に AO を乗算しており、直射日光やポイントライトまで
    //      谷地形で不自然に暗くなっていた。ambient を分離し AO はそこだけに掛ける。
    float3 ambient = albedo * ambientColor * ao;
    float3 direct  = Lighting_BlinnPhong_Direct(
        N, V, L, albedo, roughness, lightColor, lightIntensity) * shadow;
    float3 result  = ambient + direct;

    // 点光源 / スポットライト — 走査元は clusterLightMode が決める
    // (b3 の固定長配列 / StructuredBuffer / クラスタリスト)。
    FBZZ_PUNCTUAL_BEGIN(p.worldPos, p.svPosition.xy, N)
        result += Lighting_BlinnPhong_Direct(
            N, V, ps.L, albedo, saturate(roughness + ps.roughnessBias),
            ps.color, ps.intensity);
    FBZZ_PUNCTUAL_END


    return float4(result, 1.0f);
}
