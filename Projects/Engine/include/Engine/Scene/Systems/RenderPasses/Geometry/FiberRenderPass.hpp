/// @file    FiberRenderPass.hpp
/// @brief   静的繊維の色・GBuffer・影・速度を共有ジオメトリから描く。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#pragma once
#include <Graphics/Passes/Geometry/FiberRenderPass.hpp>
#include <Engine/Asset/FiberMaterialSettings.hpp>
#include <Engine/Scene/Systems/RenderPasses/IRenderPass.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <array>
#include <cstdint>
#include <string>

namespace fbzz::math { struct Frustum; }
namespace fbzz::asset { struct MaterialAsset; }
namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::renderer { struct PerFrameCB; }
namespace fbzz::scene {

/// @brief 直前に読んだ fiberMask。同じパスの読込失敗を毎フレーム繰り返さないために持つ。
struct FiberMaskSlot {
    std::string m_path;
    renderer::ResourceHandle<renderer::TextureTag> m_texture;
};

/// @brief .mat の fiberMask (textures の tex5) を読み、settings.m_maskIndex へ bindless 添字を書く。
/// @note 未設定・読込失敗・解放済みは白 1×1 の添字にする (マスク無しと同じ見た目)。シェーダーは添字を常に有効として引く。
/// @note 添字はテクスチャの生存に紐づくので、CB へ上げる前に毎フレーム呼ぶ。
void ResolveFiberMaskTexture(asset::FiberMaterialSettings& settings, const asset::MaterialAsset* material,
    renderer::ResourceManager& resources, FiberMaskSlot& slot);
/// @brief パスを直接渡す版。FiberComponent::m_maskPath の個体ごとの差し替えに使う。空パスは白 1×1。
void ResolveFiberMaskTexture(asset::FiberMaterialSettings& settings, const std::string& path,
    renderer::ResourceManager& resources, FiberMaskSlot& slot);

using renderer::PerFrameCB;
using renderer::FiberFrameCB;
using renderer::FiberContactCB;
using renderer::UpdateFiberContactCounts;
using renderer::ExecuteFiberPass;
using renderer::ExecuteFiberGBufferPass;
using renderer::ExecuteFiberVelocityPass;
using renderer::ExecuteFiberSelectionMask;
using renderer::SubmitFiberShadowCasters;
using renderer::FiberRenderPass;
}
