/// @file    TerrainRenderPass.hpp
/// @brief   TerrainComponent を走査してチャンクメッシュを生成・描画するシステム。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// @note ハイトマップ→GPU メッシュ変換・チャンク管理・フラスタムカリングはシーン全体を
/// @note またぐ横断的関心事で、Component に書くとチャンクキャッシュ共有等の最適化が困難に
/// @note なるため System へ分離した。実装は Systems/RenderPasses/Geometry/TerrainRenderPass.cpp。
#pragma once
#include <Graphics/Passes/Geometry/TerrainRenderPass.hpp>

#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/Systems/RenderPasses/IRenderPass.hpp>
#include <Math/Frustum.hpp>
#include <string_view>

namespace fbzz::scene { class Scene; }
namespace fbzz::asset { struct MaterialAsset; }

namespace fbzz::renderer {
    struct TerrainLayerGpu;
    class IRenderer;
    class ResourceManager;
    struct ConstantBufferTag;
    struct PipelineStateTag;
    struct ShaderTag;
}

namespace fbzz::scene {

struct RenderPassContext;
using renderer::TerrainLayerGpu;

/// @brief テクスチャの無い層へ詰める 1x1 の代替。シェーダーは添字の有効判定をしないので必ず埋める。
struct TerrainFallbackTextures {
    renderer::ResourceHandle<renderer::TextureTag> white;      ///< @note diffuse
    renderer::ResourceHandle<renderer::TextureTag> flatNormal; ///< @note normal
    renderer::ResourceHandle<renderer::TextureTag> black;      ///< @note ao_roughness (hasAoRoughness = 0 なので読まれない)
    renderer::ResourceHandle<renderer::TextureTag> gray;       ///< @note height
    [[nodiscard]] bool IsValid() const;
};

/// @brief 代替テクスチャ 4 枚を作る。所有は呼び出し側。
TerrainFallbackTextures CreateTerrainFallbackTextures(renderer::ResourceManager& resources);
void ReleaseTerrainFallbackTextures(renderer::ResourceManager& resources, TerrainFallbackTextures& textures);

/// @brief 層の .mat からテクスチャ添字以外の GPU パラメーターを読む。
/// @param material null なら既定値の層。
/// @param keyPrefix キーの接頭辞 (マテリアルプレビューの "layer0_" 等)。本編は空。
/// @note 既定値の正本。Inspector の既定値もこれに揃える (Docs/design/terrain-layers.md §3)。
TerrainLayerGpu ReadTerrainLayerParams(const asset::MaterialAsset* material, std::string_view keyPrefix = {});

/// @brief 層テクスチャをロードし、bindless 添字を layer へ書く。無いテクスチャは fallback の添字。
void FillTerrainLayerTextureIndices(renderer::ResourceManager& resources,
                                    const asset::MaterialAsset* material,
                                    std::string_view keyPrefix,
                                    const TerrainFallbackTextures& fallback,
                                    TerrainLayerGpu& layer);

using renderer::SubmitTerrainShadowCasters;
using renderer::TerrainSelectionMaskSystem;
using renderer::TerrainDrawMode;
using renderer::TerrainRenderPass;
}
