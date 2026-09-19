/// @file    FiberMaterialSettings.hpp
/// @brief   Fiber の material 値を有限範囲へ解決する GPU 共通レイアウト。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#pragma once
#include <Math/Vector4.hpp>

namespace fbzz::asset {
struct MaterialAsset;

/// @note FiberCommon.hlsli の MaterialConstants と同じ順序。色は線形 RGB。
struct FiberMaterialSettings {
    math::Vector4 m_rootColor = { 0.16f, 0.07f, 0.025f, 1.0f };
    math::Vector4 m_tipColor = { 0.65f, 0.4f, 0.18f, 1.0f };
    float m_length = 0.08f;
    float m_density = 0.85f;
    float m_thickness = 0.32f;
    float m_taper = 0.85f;
    float m_frequency = 100.0f;
    float m_windResponse = 0.025f;
    float m_maxBend = 0.04f;
    float m_gravityBend = 0.01f;
    float m_roughness = 0.5f;
    float m_specular = 0.25f;
    float m_transmission = 0.0f;
    float m_rootOcclusion = 0.65f;
    float m_grass = 0.0f;
    float m_worldMapping = 0.0f;
    float m_finWidth = 0.35f;
    float m_padding = 0.0f;
};
static_assert(sizeof(FiberMaterialSettings) == 96);

/// @return 欠損 / 非有限値は既定値、範囲外は clamp。asset が null なら既定の毛皮。
[[nodiscard]] FiberMaterialSettings ResolveFiberMaterial(const MaterialAsset* asset);

} // namespace fbzz::asset
