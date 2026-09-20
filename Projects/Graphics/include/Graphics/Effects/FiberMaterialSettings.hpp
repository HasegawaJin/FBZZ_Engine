/// @file    FiberMaterialSettings.hpp
/// @brief   繊維材質の GPU レイアウト。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Math/Vector4.hpp>
#include <cstdint>
namespace fbzz::renderer {
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
    /// @note 以下の毛皮の質感は既定 0 (無効)。Inspector が欠けたキーを 0 で埋めるのと揃える。
    float m_specularShift = 0.0f;
    float m_secondarySpecular = 0.0f;
    float m_colorVariation = 0.0f;
    float m_clumping = 0.0f;
    float m_clumpTwist = 0.0f;
    /// @note fiberMask (.mat の tex5) の bindless 添字。ResolveFiberMaterial は書かず、描画側が毎フレーム書く。
    /// @see scene::ResolveFiberMaskTexture
    std::uint32_t m_maskIndex = 0xFFFFFFFFu;
    float m_padding[3]{};
};
static_assert(sizeof(FiberMaterialSettings) == 128);

}
