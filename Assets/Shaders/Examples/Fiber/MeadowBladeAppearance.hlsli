/// @file    MeadowBladeAppearance.hlsli
/// @brief   User-authored appearance example; no vertex or coverage changes.
/// @author  Hasegawa Jin
/// @date    2026-10-21
FBZZ_MATERIAL_TEX(appearanceTexture, texAlbedoIndex);

float3 FiberAppearanceAlbedo(FiberPixel p, FiberStrand strand, float3 engineColor)
{
    float pattern = enablePattern ? 0.5f + 0.5f * sin((p.root.x + p.root.z * 0.35f) * patternScale) : 0.0f;
    if (texAlbedoIndex != 0xffffffffu)
        pattern *= appearanceTexture.Sample(fiberMaskSampler, p.uv * max(patternScale * 0.05f, 0.001f)).g;
    float3 tint = lerp(float3(1.0f, 1.0f, 1.0f), accentColor.rgb, saturate(pattern * patternStrength));
    return engineColor * tint;
}

float3 FiberAppearanceLighting(FiberPixel p, FiberStrand strand, float3 engineLighting)
{
    float3 base = FiberStrandColor(strand, p.height);
    float3 color = FiberAppearanceAlbedo(p, strand, base);
    float bands = float(clamp(colorBands, 1u, 16u));
    float3 illumination = engineLighting / max(base, 0.005f);
    if (colorBands > 1u) illumination = floor(max(illumination, 0.0f) * bands + 0.5f) / bands;
    return color * illumination;
}
