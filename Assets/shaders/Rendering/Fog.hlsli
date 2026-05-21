// FBZZ Engine
// Fog.hlsli | Rendering
// 指数フォグ・高度フォグ
#ifndef FOG_HLSLI
#define FOG_HLSLI

// =========================================================================
// 指数フォグ (Exponential Fog)
//   depth   : カメラからの距離 (ビュー空間 Z)
//   density : フォグ密度 (大きいほど濃い)
//   戻り値   : フォグ係数 [0=完全にフォグ, 1=フォグなし]
// =========================================================================
float FogFactor(float depth, float density)
{
    return saturate(exp(-density * depth));
}

// =========================================================================
// 高度フォグ (Height-based Exponential Fog)
//   worldY    : フラグメントのワールド Y 座標
//   fogBase   : フォグが最も濃い高度
//   fogHeight : フォグの減衰高度スケール
// =========================================================================
float HeightFogFactor(float worldY, float fogBase, float fogHeight, float density)
{
    float h = max(worldY - fogBase, 0.0f) / max(fogHeight, 0.001f);
    return saturate(exp(-density * h));
}

// =========================================================================
// フォグを適用する
//   color   : フォグ適用前の色
//   factor  : FogFactor() または HeightFogFactor() の戻り値
//   fogColor: フォグの色 (空の色など)
// =========================================================================
float3 ApplyFog(float3 color, float factor, float3 fogColor)
{
    return lerp(fogColor, color, factor);
}

#endif // FOG_HLSLI