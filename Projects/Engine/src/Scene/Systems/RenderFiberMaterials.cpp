/// @file    RenderFiberMaterials.cpp
/// @brief   繊維の共有材質と個体のマスク・色・形状設定を解決する。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "RenderFiberExtraction.hpp"
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/StreamedTextureResolver.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/FiberComponent.hpp>
#include <Graphics/Renderer/ITexture.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::scene {

void ResolveFiberMaskTexture(asset::FiberMaterialSettings& settings, const asset::MaterialAsset* material,
    renderer::ResourceManager& resources, FiberMaskSlot& slot)
{
    std::string path;
    const auto snapshot = material ? material->ReadAuthoring() : asset::MaterialAsset::AuthoringSnapshotPtr{};
    const auto* values = snapshot ? &snapshot->Data() : nullptr;
    if (values) {
        if (const auto it = values->textures.find(asset::FIBER_MASK_TEXTURE_KEY); it != values->textures.end())
            path = it->second;
    }
    ResolveFiberMaskTexture(settings, path, resources, slot);
}

void ResolveFiberMaskTexture(asset::FiberMaterialSettings& settings, const std::string& path,
    renderer::ResourceManager& resources, FiberMaskSlot& slot)
{
    /// @note 毎フレーム利用権を更新する。読込失敗は resolver が保持し、同期 I/O を繰り返さない。
    {
        slot.m_path = path;
        slot.m_texture = path.empty() ? renderer::ResourceHandle<renderer::TextureTag>{} : asset::StreamedTextureResolver::Engine().ResolveGpu(resources, path);
    }
    const renderer::ITexture* texture = resources.Get(slot.m_texture);
    if (!texture) texture = resources.Get(resources.GetWhiteTexture());
    settings.m_maskIndex = texture ? texture->GetBindlessIndex() : renderer::INVALID_BINDLESS_INDEX;
}

namespace fiber_extraction_detail {

/// @note DX12 の CB は Update のたびに次の提出でアリーナへ複製し直す。同じ内容の再転送を省く。
const FiberMaterialEntry& GetFiberMaterial(FiberResources& cache, renderer::ResourceManager& resources,
    const std::string& path)
{
    auto& materialEntry = cache.m_materials[path];
    if (materialEntry.m_frame == Time::frameCount) return materialEntry;
    materialEntry.m_frame = Time::frameCount;
    const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(path);
    const auto authoringSnapshot = asset::ReadMaterialAuthoring(handle);
    const auto* material = authoringSnapshot ? &authoringSnapshot->Data() : nullptr;
    materialEntry.m_valid = material != nullptr;
    if (material) {
        materialEntry.m_settings = asset::ResolveFiberMaterial(*material);
        const auto maskParameter = material->textures.find(asset::FIBER_MASK_TEXTURE_KEY);
        const std::string maskPath = maskParameter != material->textures.end() ? maskParameter->second : std::string{};
        ResolveFiberMaskTexture(materialEntry.m_settings, maskPath, resources, materialEntry.m_mask);
        if (materialEntry.m_snapshot != authoringSnapshot || materialEntry.m_appearance.resolved
            || materialEntry.m_shaderVersion != resources.GetShaderVersion()) {
            ResolveFiberAppearance(materialEntry.m_appearance, *material, materialEntry.m_settings, materialEntry.m_mask.m_texture, resources);
            materialEntry.m_snapshot = authoringSnapshot;
            materialEntry.m_shaderVersion = resources.GetShaderVersion();
        }
    }
    return materialEntry;
}

/// @brief FiberComponent の個体ごとの倍率を共有 .mat の解決結果へ掛ける。
/// @note 長さ・密度・毛束は ResolveFiberMaterial と同じ上限で丸める。色の乗算は 1 を超えてよい (HDR)。非有限は 1 とみなす。
asset::FiberMaterialSettings ApplyFiberOverrides(const asset::FiberMaterialSettings& baseSettings, const FiberComponent& fiber,
    FiberResources& cache, renderer::ResourceManager& resources)
{
    const auto scale = [](float value) { return std::isfinite(value) ? std::max(value, 0.0f) : 1.0f; };
    asset::FiberMaterialSettings settings = baseSettings;
    const math::Vector3 tint{ scale(fiber.m_colorTint.x), scale(fiber.m_colorTint.y), scale(fiber.m_colorTint.z) };
    settings.m_rootColor = { baseSettings.m_rootColor.x * tint.x, baseSettings.m_rootColor.y * tint.y, baseSettings.m_rootColor.z * tint.z, baseSettings.m_rootColor.w };
    settings.m_tipColor = { baseSettings.m_tipColor.x * tint.x, baseSettings.m_tipColor.y * tint.y, baseSettings.m_tipColor.z * tint.z, baseSettings.m_tipColor.w };
    settings.m_length = std::clamp(baseSettings.m_length * scale(fiber.m_lengthScale), 0.0f, 2.0f);
    settings.m_density = std::clamp(baseSettings.m_density * scale(fiber.m_densityScale), 0.0f, 1.0f);
    settings.m_clumping = std::clamp(baseSettings.m_clumping * scale(fiber.m_clumpingScale), 0.0f, 1.0f);
    if (!fiber.m_maskPath.empty())
        ResolveFiberMaskTexture(settings, fiber.m_maskPath, resources, cache.m_maskOverrides[fiber.m_maskPath]);
    return settings;
}

} /// @note namespace fiber_extraction_detail

} /// @note namespace fbzz::scene
