/// @file    GrassCardFields.hlsli
/// @brief   Grass-card values appended to the shared Fiber material layout.
/// @author  Hasegawa Jin
/// @date    2026-11-04
    float cardAlphaCutoff;
    float cardSway;
    float cardSwayHz;
    FBZZ_MATERIAL_TEX_ALBEDO
    float4 cardSpeciesWeights;
    float4 cardSpeciesHeight;
    float4 cardSpeciesWidth;
    float4 cardSpeciesSway;
    float4 cardSpeciesSwayHz;
    uint cardSpeciesAtlas;
