/// @file PunctualShadow.hlsli
/// @brief Spot / Point ライトのシャドウアトラス サンプリング
/// @author Hasegawa Jin
/// @date 2026-08-25
//
// Directional のカスケード (Rendering/Shadow.hlsli) とは別経路。
//
// WHY Shadow.hlsli の関数を呼ばないか:
//   あちらは CloudShadow と b4 / b8 のフォールバック用 static const 群を抱えており、
//   include 順が Constants.hlsli より前になった瞬間に「cbuffer メンバーと static const の
//   二重定義」でコンパイルが落ちる。このファイルは Lighting.hlsli 経由で 40 以上の
//   シェーダーへ無条件に届くため、include 順に一切依存しない作りでなければならない。
//   依存は Space.hlsli (純粋関数のみ) と b12 だけに閉じてある。
#ifndef PUNCTUAL_SHADOW_HLSLI
#define PUNCTUAL_SHADOW_HLSLI

#include "Common/Space.hlsli"
#include "Common/ClusterConstants.hlsli"   // FBZZ_LIGHT_TYPE_*
#include "Common/PunctualShadowConstants.hlsli"
#include "Common/BindlessIndices.hlsli"

FBZZ_TEX2D_T(float, gPunctualShadowAtlas, TEX_PUNCTUAL_SHADOW_SLOT);
SamplerComparisonState gPunctualShadowSamp  : register(SAMPLER_SHADOW_PUNCTUAL);
FBZZ_TEX2D_T(float4, gLightCookieAtlas, TEX_LIGHT_COOKIE_SLOT);
SamplerState           gLightCookieSamp     : register(SAMPLER_COOKIE);

// FBZZ_CubeFaceIndex — 光源からフラグメントへ向かうベクトルが属するキューブ面。
// 面の順序は D3D の標準 (+X, -X, +Y, -Y, +Z, -Z)。C++ 側が積む 6 本の LookAt と
// 一致させること。ずれると「特定の方向だけ影が出ない」という追いにくい壊れ方をする。
int FBZZ_CubeFaceIndex(float3 dir)
{
    const float3 a = abs(dir);
    if (a.x >= a.y && a.x >= a.z) return (dir.x > 0.0f) ? 0 : 1;
    if (a.y >= a.z)               return (dir.y > 0.0f) ? 2 : 3;
    return (dir.z > 0.0f) ? 4 : 5;
}

// FBZZ_PunctualSlopeBias — 面がライトに対して寝ているほどバイアスを増やす。
// 1 テクセル内の深度差は tan(theta) に比例するため、固定値ではアクネか
// Peter Panning のどちらかが必ず出る。上限は素のバイアスの 6 倍。
float FBZZ_PunctualSlopeBias(float bias, float3 N, float3 L)
{
    const float NdotL = saturate(dot(N, L));
    const float slope = sqrt(1.0f - NdotL * NdotL) / max(NdotL, 1e-4f);
    return clamp(bias + bias * slope, bias, bias * 6.0f);
}

// FBZZ_PunctualShadowPCF — アトラスのタイル 1 枚に閉じた PCF。
//   radius : カーネル半径 (タップ数は (2r+1)^2)
//   spread : 1 タップあたりのテクセル歩幅。1 で隣接テクセル、大きいほど柔らかい縁
//
// WHY 歩幅と半径の両方を呼び出し側が決めるか: 半影の広さは歩幅 × 半径で決まるが、
//     «標本が途切れないか» は歩幅だけで決まる。両方を外から渡せる形にしておかないと、
//     「半影は広いが 9 点しか撃たない」カーネルを作れてしまう (呼び出し側の注記を参照)。
//
// WHY 矩形へクランプするか: カーネルはタイル端で隣のスロットへはみ出す。隣は
//     「別のライトの深度」なので、漏れると無関係な影が帯状に貼り付く。
float FBZZ_PunctualShadowPCF(float2 uv, float depth, float4 rect, int radius, float spread)
{
    const float2 step  = punctualShadowTexel * spread;
    const float2 inset = punctualShadowTexel * (float(radius) * spread + 1.0f);
    const float2 uvMin = rect.xy + inset;
    const float2 uvMax = rect.xy + rect.zw - inset;

    float shadow = 0.0f;
    float total  = 0.0f;

    for (int y = -radius; y <= radius; ++y)
    for (int x = -radius; x <= radius; ++x)
    {
        float2 sampleUV = rect.xy + uv * rect.zw + float2(x, y) * step;
        sampleUV = clamp(sampleUV, uvMin, uvMax);
        shadow += gPunctualShadowAtlas.SampleCmpLevelZero(gPunctualShadowSamp, sampleUV, depth);
        total  += 1.0f;
    }

    return shadow / total;
}

// FBZZ_SamplePunctualShadow — スロット 1 枚を引く。
//   slot : PunctualLight::shadowIndex (Point ではキューブ面を足した後の値)
//   戻り値: 0.0 = 完全に影, 1.0 = 遮蔽なし。錐台外も 1.0。
float FBZZ_SamplePunctualShadow(int slot, float3 worldPos, float3 N, float3 L)
{
    if (slot < 0 || slot >= punctualShadowCount) return 1.0f;

    const float4 params = punctualShadowParams[slot];
    if (params.y <= 0.0f) return 1.0f;  // 影の濃さ 0 — PCF ループごと省く

    float2 uv;
    float  depth;
    WorldToShadowUV(worldPos, punctualShadowVP[slot], uv, depth);

    // ライト錐台の外は遮蔽なし。Spot ではコーンの外側、Point では面の外側にあたる。
    if (any(uv < 0.0f) || any(uv > 1.0f) || depth < 0.0f || depth > 1.0f)
        return 1.0f;

    const float bias = FBZZ_PunctualSlopeBias(params.x, N, L);

    // params.z = 光源半径ぶんの半影 (テクセル)。広げるのは歩幅だけでなく半径も。
    //
    // WHY 半径も増やすか: 歩幅だけ伸ばしてもタップ数は増えないので、標本が散るだけで
    //     半影にならない。半影 4 テクセルのとき 3x3 の 9 点は 8 テクセル四方へ広がり、
    //     その隙間 (Spot 1 枚 512² では床で 50cm 間隔) へ人 1 人ぶんのシルエットが
    //     まるごと落ちる。「大きい光源にすると影が消える」のがこれ。
    //     歩幅は 2 テクセル以下に抑える — 比較サンプラーが 1 タップで 2x2 を畳むので、
    //     そこまでは標本が途切れない。足りないぶんは半径で埋める。
    //     上限 3 (7x7 = 49 タップ) はライト 1 本あたりのコストの実用上限。
    const float penumbra  = clamp(params.z, 0.0f, 8.0f);
    const int   minRadius = clamp(punctualShadowPcf, 0, 3);
    const int   radius    = clamp((int)ceil(penumbra * 0.5f), minRadius, 3);
    const float spread    = max(penumbra / max((float)radius, 1.0f), 1.0f);

    const float factor = FBZZ_PunctualShadowPCF(uv, depth - bias,
                                                punctualShadowRect[slot],
                                                radius, spread);

    // params.y: 1 = 完全な影, 0 = 影なし。
    return lerp(1.0f - params.y, 1.0f, factor);
}

// FBZZ_PunctualShadowFactor — ライト 1 本ぶんの遮蔽率。型に応じてスロットを選ぶ。
//   lightType   : FBZZ_LIGHT_TYPE_*
//   shadowIndex : Spot はスロットそのもの、Point は 6 面の先頭スロット
//   L           : サーフェス → ライト方向 (正規化済み)
float FBZZ_PunctualShadowFactor(uint lightType, int shadowIndex,
                                float3 lightPos, float3 worldPos,
                                float3 N, float3 L)
{
    if (shadowIndex < 0 || punctualShadowCount <= 0) return 1.0f;

    // 全方位のライトはキューブ 6 面のどれかを引く。Sphere / Tube も CPU 側が
    // 「中心から見た点光源」として 6 面を焼くので、面の選び方は Point と同じでよい。
    // 形状ぶんの広がりは半影 (params.z) が受け持つ。
    if (lightType == FBZZ_LIGHT_TYPE_POINT
     || lightType == FBZZ_LIGHT_TYPE_SPHERE
     || lightType == FBZZ_LIGHT_TYPE_TUBE)
        return FBZZ_SamplePunctualShadow(
            shadowIndex + FBZZ_CubeFaceIndex(worldPos - lightPos), worldPos, N, L);

    // Spot と Area は 1 枚。Area は法線方向へ張った錐台なので、外れた点は
    // 錐台外として 1.0 (遮蔽なし) が返る。
    return FBZZ_SamplePunctualShadow(shadowIndex, worldPos, N, L);
}

// =========================================================================
// Cookie (投影テクスチャ)
// =========================================================================
// スポットの円錐へ被せる白黒 / カラーのマスク。木漏れ日・窓枠・ロゴのゴボを作る。
// シャドウとは独立したスロットを持つので、影を落とさないライトにも付けられる。
//
// NOTE: Spot 専用。Point はキューブマップ、Directional はワールド空間のタイリングという
//       別の仕組みが要るため、この経路では扱わない (CPU 側が cookieIndex を配らない)。

// FBZZ_SampleLightCookie — Cookie スロット 1 枚を引いて透過色を返す。
//   戻り値: ライト色へ乗算する RGB。スロット無効 / 投影範囲外は白 (素通り)。
float3 FBZZ_SampleLightCookie(int slot, float3 worldPos)
{
    if (slot < 0 || slot >= lightCookieCount) return float3(1.0f, 1.0f, 1.0f);

    float2 uv;
    float  depth;
    WorldToShadowUV(worldPos, lightCookieVP[slot], uv, depth);

    // 錐台の外は素通り。スポットのコーン減衰 (SpotConeWeight) が別に掛かるので、
    // ここで 0 にするとコーンの縁が二重に落ちて輪郭が硬くなる。
    if (any(uv < 0.0f) || any(uv > 1.0f) || depth < 0.0f || depth > 1.0f)
        return float3(1.0f, 1.0f, 1.0f);

    // タイル内 UV → アトラス UV。端 1 テクセルは隣のタイルとの補間を避けて内側へ寄せる。
    const float4 rect  = lightCookieRect[slot];
    const float2 inset = lightCookieTexel;
    const float2 atlasUV = clamp(rect.xy + uv * rect.zw,
                                 rect.xy + inset,
                                 rect.xy + rect.zw - inset);

    const float4 cookie = gLightCookieAtlas.SampleLevel(gLightCookieSamp, atlasUV, 0);
    // アルファは「遮り」として RGB へ畳む。白黒マスクを RGB だけで描いても、
    // 切り抜き PNG を入れても、どちらも期待どおりに暗くなる。
    return cookie.rgb * cookie.a;
}

#endif // PUNCTUAL_SHADOW_HLSLI
