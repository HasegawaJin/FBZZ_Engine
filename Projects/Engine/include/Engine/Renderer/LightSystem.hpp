/// @file    LightSystem.hpp
/// @brief   ライト情報の管理と定数バッファ転送。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// Directional / Point / Spot を HLSL の LightConstants と一致する形に詰める。
/// 配列上限はシェーダー側レイアウトと同期して変更する。
#pragma once
#include "IConstantBuffer.hpp"
#include <Math/Vector3.hpp>
#include <vector>

namespace fbzz::renderer {

struct DirectionalLight {
    math::Vector3 direction = {  0.0f, -1.0f,  0.5f };
    float         _pad0     = 0.0f; // HLSL float3 は 16 バイト境界に整列するため必須
    math::Vector3 color     = {  1.0f,  1.0f,  1.0f };
    float         intensity = 1.0f;
};

struct PointLight {
    math::Vector3 position  = {};
    float         range     = 10.0f;
    math::Vector3 color     = { 1.0f, 1.0f, 1.0f };
    float         intensity = 1.0f;
};

struct SpotLight {
    math::Vector3 position  = {};
    float         range     = 10.0f;
    math::Vector3 direction = { 0.0f, -1.0f, 0.0f };
    // 角度ではなくコサイン値で保存する。HLSL 側で毎フレーム cos() を呼ばなくて済む。
    float         innerCos  = 0.966f;   // ~15°
    math::Vector3 color     = { 1.0f, 1.0f, 1.0f };
    float         outerCos  = 0.866f;   // ~30°
    float         intensity = 1.0f;
    float         _pad[3]   = {}; // 合計 64 バイトに揃えるためのパディング
};

// Constants.hlsli の LightConstants cbuffer と完全一致 (560 bytes)
struct LightConstantsCB {
    math::Vector3 lightDir;
    float         _lightPad0 = 0.0f;
    math::Vector3 lightColor;
    float         lightIntensity = 0.0f;
    PointLight    pointLights[8];
    SpotLight     spotLights[4];
    int           pointLightCount = 0;
    int           spotLightCount  = 0;
    // 空・雲・光芒の見た目を制御する昼夜減光係数。lightIntensity とは別軸。
    // WHY: 以前は空系シェーダーが lightIntensity をそのまま減光に使っていたため、
    //      太陽を明るくすると空まで白飛びし、両者を独立に調整できなかった。
    //      レイアウトを変えずに済むよう、未使用だった _lightPad2 の 1 スロットを充てる。
    //      既定 1.0 は「昼夜サイクル無効 + DirectionalLight intensity = 1.0」だった
    //      従来の空の明るさと一致する。昼夜サイクル有効時は RenderSystem が
    //      SkyRenderer::skyNightBrightness〜skyDayBrightness を補間して上書きする。
    float         skyDimmer       = 1.0f;
    float         _lightPad2      = 0.0f;
    math::Vector3 ambientColor    = { 0.08f, 0.08f, 0.08f };
    float         _ambientPad     = 0.0f;
};

class LightSystem {
public:
    void SetDirectional(const DirectionalLight& light);
    const DirectionalLight& GetDirectional() const;

    void AddPoint(const PointLight& light);
    void AddSpot(const SpotLight& light);
    void Clear();

    std::vector<PointLight>&       GetPointLights()       { return m_pointLights; }
    const std::vector<PointLight>& GetPointLights() const { return m_pointLights; }
    std::vector<SpotLight>&        GetSpotLights()        { return m_spotLights;  }
    const std::vector<SpotLight>&  GetSpotLights()  const { return m_spotLights;  }

    void Upload(IConstantBuffer& cb) const;

private:
    DirectionalLight    m_directional;
    std::vector<PointLight> m_pointLights;
    std::vector<SpotLight>  m_spotLights;
};

} // namespace fbzz::renderer
