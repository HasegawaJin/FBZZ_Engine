// FBZZ Engine
// Material.hpp | fbzz::renderer
// シェーダー・テクスチャ・パラメータの束
//
// MaterialParams は HLSL の MaterialConstants cbuffer と完全に一致させること。
// 各フィールドの offset は 16 バイト境界で揃えており、C++ と HLSL 間のパディングが一致する。
// Unity Standard シェーダーに相当するパラメータセットを持つ。
//
// textureMask ビット割り当て (Binding.hlsli の TEX_* と対応):
//   bit 0 = albedo       (TEX_ALBEDO)
//   bit 1 = normal       (TEX_NORMAL)
//   bit 2 = metallicRough(TEX_METALLIC_ROUGH)
//   bit 3 = emissive     (TEX_EMISSIVE)
//   bit 4 = ao           (TEX_AO)
#pragma once
#include "ResourceHandle.hpp"
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <cstdint>
#include <string>

namespace fbzz::renderer {

class ResourceManager;

// GPU へ転送するマテリアルパラメータ。
// HLSL の MaterialConstants と完全に一致すること (offset / padding に注意)。
struct MaterialParams {
    // ── surface ──────────────────────────────── offset  0 (16 bytes)
    math::Vector4 albedo            = { 1.0f, 1.0f, 1.0f, 1.0f }; // RGB + alpha

    // ──────────────────────────────────────────── offset 16 (16 bytes)
    float metallic          = 0.0f;  // 金属度 [0, 1]
    float roughness         = 0.8f;  // 粗さ   [0, 1]
    float normalStrength    = 1.0f;  // 法線マップ強度 (0 = 無効, 1 = フル)
    float occlusionStrength = 1.0f;  // AO 強度 [0, 1]

    // ── emissive ─────────────────────────────── offset 32 (16 bytes)
    math::Vector3 emissiveColor     = { 1.0f, 1.0f, 1.0f }; // エミッシブ色 (emissiveScale と乗算)
    float         emissiveScale     = 0.0f;                  // エミッシブ強度 (0 = 非発光)

    // ── UV ───────────────────────────────────── offset 48 (16 bytes)
    float uvTiling[2]   = { 1.0f, 1.0f }; // UV スケール (X, Y)
    float uvOffset[2]   = { 0.0f, 0.0f }; // UV オフセット (X, Y)

    // ── alpha ────────────────────────────────── offset 64 (16 bytes)
    float    alphaCutoff = 0.5f;  // アルファカットオフ閾値 (カットアウト描画用)
    float    _pad0       = 0.0f;
    float    _pad1       = 0.0f;
    float    _pad2       = 0.0f;

    // ── flags ────────────────────────────────── offset 80 (16 bytes)
    uint32_t textureMask = 0;   // テクスチャ存在フラグ (bit 0-4)
    float    _pad3       = 0.0f;
    float    _pad4       = 0.0f;
    float    _pad5       = 0.0f;
};  // 96 bytes total

class Material {
public:
    ResourceHandle<ShaderTag> shader;

    // マテリアルテクスチャ (TEX_* スロットと対応)
    ResourceHandle<TextureTag> albedoTexture;        // t0
    ResourceHandle<TextureTag> normalTexture;        // t1
    ResourceHandle<TextureTag> metallicRoughTexture; // t2  G=roughness, B=metallic (glTF 規約)
    ResourceHandle<TextureTag> emissiveTexture;      // t3
    ResourceHandle<TextureTag> aoTexture;            // t4

    ResourceHandle<ConstantBufferTag>   paramsBuffer;

    MaterialParams params;

    std::string shaderPath;    // 参照する .hlsl パス

    void Init(ResourceManager& resources);
    // textureMask を現在のハンドル状態に合わせて同期し、paramsBuffer へ転送する。
    void Upload(ResourceManager& resources);
};

} // namespace fbzz::renderer
