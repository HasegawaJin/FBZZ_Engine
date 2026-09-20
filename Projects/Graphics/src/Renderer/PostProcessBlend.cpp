/// @file    PostProcessBlend.cpp
/// @brief   ボリューム合成プリミティブと、解決結果の描画設定への流し込み。
/// @author  Hasegawa Jin
/// @date    2026-08-12
#include <Graphics/Renderer/PostProcessBlend.hpp>
#include <algorithm>

namespace fbzz::renderer {

float BlendFloat(float a, float b, float t)
{
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    return a + (b - a) * t;
}

bool BlendBool(bool a, bool b, float t) { return t >= 0.5f ? b : a; }

int BlendInt(int a, int b, float t) { return t >= 0.5f ? b : a; }

void BlendColor3(const float (&a)[3], const float (&b)[3], float t, float (&out)[3])
{
    for (int i = 0; i < 3; ++i) out[i] = BlendFloat(a[i], b[i], t);
}

void ApplyVolumeSettings(const VolumeSettings& volume, RenderSettings& out)
{
    out.postProcess      = volume.post;
    out.ssr              = volume.ssr;
    out.gtao             = volume.gtao;
    out.contactShadow    = volume.contactShadow;
    out.taa              = volume.taa;
    out.motionBlur       = volume.motionBlur;
    out.volumetricLight  = volume.volumetricLight;
    /// @note グリッド寸法だけは RenderSettings 側の値を残す。Volume 側は寸法を持たず、
    /// @note       ブレンドすると解像度が毎フレーム変わってボリュームの再確保が走るため、
    /// @note       中身だけ差し替えて寸法は据え置く。
    {
        const uint32_t gx = out.froxelFog.gridX;
        const uint32_t gy = out.froxelFog.gridY;
        const uint32_t gz = out.froxelFog.gridZ;
        out.froxelFog       = volume.froxelFog;
        out.froxelFog.gridX = gx;
        out.froxelFog.gridY = gy;
        out.froxelFog.gridZ = gz;
    }
    out.autoExposure     = volume.autoExposure;
    out.lensFlare        = volume.lensFlare;
    out.lutColorGrading  = volume.lutColorGrading;

    /// @note 別々のボリュームが別々のスロットを ON にすると、合成結果として
    /// @note       FXAA+TAA / SSAO+GTAO が同時に立ちうる。描画へ渡す直前に、
    /// @note       TOML・Inspector と同じ排他規則へ寄せる。
    (void)out.NormalizeExclusivePipelineSlots();
}

float PostProcessVolumeDistanceWeight(float distance, float radius, float blendDistance)
{
    if (radius <= 0.0f) return 0.0f;
    if (distance >= radius) return 0.0f;

    /// @note blendDistance が半径以上だと内側の「完全適用域」が消える。
    /// @note       その場合は中心で 1、境界で 0 の単純な線形降下にする。
    const float fade = std::clamp(blendDistance, 0.0f, radius);
    const float solidRadius = radius - fade;
    if (distance <= solidRadius) return 1.0f;
    if (fade <= 0.0f) return 1.0f;
    return std::clamp((radius - distance) / fade, 0.0f, 1.0f);
}

} /// @note namespace fbzz::renderer
