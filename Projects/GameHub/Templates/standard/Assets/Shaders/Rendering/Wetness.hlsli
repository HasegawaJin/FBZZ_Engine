/// @file Wetness.hlsli
/// @brief 天候の濡れをサーフェス値 (albedo / roughness) へ適用する
/// @author Hasegawa Jin
/// @date 2026-08-25
#ifndef WETNESS_HLSLI
#define WETNESS_HLSLI

// AdvancedGraphicsConstants(b8) を宣言していないシェーダー向けフォールバック。
// Shadow.hlsli の pcssEnabled と同じ理由 — 到達不能な関数本体もコンパイラが検証するため、
// 未定義のままだとビルドが落ちる。Terrain は TerrainCB から自前で値を渡す。
#ifndef HAVE_ADVANCED_GRAPHICS_CB
static const float weatherWetness   = 0.0f;
static const float weatherDarkening = 0.0f;
static const float weatherPuddle    = 0.0f;
#endif

// 水膜が微細な凹凸を埋めたあとの roughness 倍率。
static const float FBZZ_WET_ROUGHNESS_SCALE = 0.35f;

// 水たまりが成立する面の向き。N.y がこの範囲で 0→1 に立ち上がる。
// WHY 幅を持たせるか: 閾値 1 本だと、緩い傾斜のある地面で水たまりの縁が
//     等高線のような硬い線になる。
static const float FBZZ_PUDDLE_MIN_UP = 0.75f;
static const float FBZZ_PUDDLE_MAX_UP = 0.95f;

// 溜まった水そのものの値。水面は albedo をほぼ持たず、鏡に近い。
static const float FBZZ_PUDDLE_DARKEN    = 0.55f;
static const float FBZZ_PUDDLE_ROUGHNESS = 0.05f;

struct WetSurface
{
    float3 albedo;
    float  roughness;
};

/// 濡れをサーフェス値へ適用する。
///   shadingNormal: ワールド法線 (正規化済み)。水たまりの向き判定に使う
///   wetness      : 濡れ量 [0,1]。0 で入力をそのまま返す
WetSurface ApplyWetness(float3 albedo, float roughness, float3 shadingNormal,
                        float wetness, float darkening, float puddleAmount)
{
    WetSurface surface;
    surface.albedo    = albedo;
    surface.roughness = roughness;

    const float wet = saturate(wetness);
    if (wet <= 0.0f)
        return surface;

    // 多孔質な表面は水を吸って暗くなる。濡れの見た目の大半はこの 1 行で決まる。
    surface.albedo = albedo * lerp(1.0f, 1.0f - saturate(darkening), wet);
    surface.roughness = lerp(roughness, roughness * FBZZ_WET_ROUGHNESS_SCALE, wet);

    // 上向きの面ほど水が溜まり、素材の粗さを覆い隠して鏡面になる。
    const float puddle = saturate(puddleAmount) * wet *
        smoothstep(FBZZ_PUDDLE_MIN_UP, FBZZ_PUDDLE_MAX_UP, saturate(shadingNormal.y));
    surface.albedo    = lerp(surface.albedo, surface.albedo * FBZZ_PUDDLE_DARKEN, puddle);
    surface.roughness = lerp(surface.roughness, FBZZ_PUDDLE_ROUGHNESS, puddle);
    return surface;
}

/// b8 を宣言しているシェーダー向けの既定引数版。
WetSurface ApplyWetness(float3 albedo, float roughness, float3 shadingNormal)
{
    return ApplyWetness(albedo, roughness, shadingNormal,
                        weatherWetness, weatherDarkening, weatherPuddle);
}

#endif // WETNESS_HLSLI
