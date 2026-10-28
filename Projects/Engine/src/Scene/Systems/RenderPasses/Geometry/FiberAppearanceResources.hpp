/// @file    FiberAppearanceResources.hpp
/// @brief   Resolve project Fiber shader families and reflected material bytes without replacing engine geometry.
/// @author  Hasegawa Jin
/// @date    2026-10-21
#pragma once
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialParamBinding.hpp>
#include <Engine/Asset/StreamedTextureResolver.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Graphics/Renderer/IShader.hpp>
#include <Engine/Core/Logger.hpp>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

namespace fbzz::scene {
namespace {

struct FiberAppearanceResources {
    FiberAppearanceResources() = default;
    FiberAppearanceResources(FiberAppearanceResources&&) = default;
    FiberAppearanceResources& operator=(FiberAppearanceResources&&) = default;
    /// @note The static cache may outlive its owner; normal eviction explicitly releases through the known manager.
    ~FiberAppearanceResources() { material.paramsBuffer = {}; }
    renderer::Material material;
    std::shared_ptr<renderer::FiberAppearance> resolved;
    std::array<renderer::ResourceHandle<renderer::ShaderTag>, 10> shaders{}, finShaders{};
    bool finValid = false;
    renderer::ShaderVertexContract geometry = renderer::ShaderVertexContract::UNKNOWN;
    std::string path, warning;
    uint64_t shaderVersion = UINT64_MAX;
    mutable uint32_t mismatchWarnings = 0;
    bool valid = false;
};

void FiberAppearanceWarning(FiberAppearanceResources& state, const std::string& message)
{
    if (state.warning == message) return;
    state.warning = message;
    FBZZ_LOG_WARN("Fiber appearance: %s; using the compatible built-in family", message.c_str());
}

bool IsSharedFiberGeometry(renderer::ShaderVertexContract vertex)
{
    return vertex == renderer::ShaderVertexContract::FIBER_SHELL
        || vertex == renderer::ShaderVertexContract::FIBER_FIN
        || vertex == renderer::ShaderVertexContract::FIBER_BLADE;
}

void ResolveFiberAppearance(FiberAppearanceResources& state, const asset::MaterialAssetAuthoring& values,
    const renderer::FiberMaterialSettings& settings, renderer::ResourceHandle<renderer::TextureTag> mask,
    renderer::ResourceManager& resources)
{
    const auto primary = resources.LoadShader(values.shaderPath);
    const auto capability = resources.GetShaderCapabilities(primary);
    if (!IsSharedFiberGeometry(capability.vertex)) {
        state.resolved.reset();
        /// @note Legacy Fiber materials intentionally use the old fixed family, including old preview-only metadata.
        const bool legacy = capability.preview == renderer::ShaderPreviewKind::FIBER_SHELL
            || capability.preview == renderer::ShaderPreviewKind::FIBER_FIN
            || capability.preview == renderer::ShaderPreviewKind::FIBER_BLADE;
        if (!legacy && !values.shaderPath.empty())
            FiberAppearanceWarning(state, "shader does not declare fiber_shell_v1, fiber_fin_v1 or fiber_blade_v1");
        return;
    }
    const std::string path = asset::AssetDatabase::IsGuidRef(values.shaderPath)
        ? asset::AssetDatabase::PathFromGuid(asset::AssetDatabase::GuidFromRef(values.shaderPath))
        : asset::AssetManager::ResolveAssetPath(values.shaderPath);
    if (state.path != path || state.shaderVersion != resources.GetShaderVersion()
        || state.geometry != capability.vertex) {
        state.path = path;
        state.shaderVersion = resources.GetShaderVersion();
        state.geometry = capability.vertex;
        state.valid = false;
        state.shaders = {};
        state.finShaders = {};
        state.finValid = false;
        state.warning.clear();
        if (path.size() < 5 || path.substr(path.size() - 5) != ".hlsl") {
            FiberAppearanceWarning(state, "project family requires a .hlsl primary shader");
            state.resolved.reset();
            return;
        }
        const std::string stem = path.substr(0, path.size() - 5);
        constexpr const char* suffixes[]{"", "GBuffer", "Velocity", "Shadow", "Selection"};
        const auto* reference = resources.Get(primary);
        bool valid = reference && renderer::HasFiberMaterialPrefix(reference->GetDescriptor());
        for (size_t mode = 0; mode < 5; ++mode) {
            const size_t skinCount = capability.vertex == renderer::ShaderVertexContract::FIBER_BLADE ? 1 : 2;
            for (size_t skin = 0; skin < skinCount; ++skin) {
                const auto shader = resources.LoadShader(stem + (skin ? "Skinned" : "") + suffixes[mode] + ".hlsl");
                const auto* native = resources.Get(shader);
                if (!native || resources.GetShaderCapabilities(shader).vertex != capability.vertex
                    || !renderer::HasFiberMaterialPrefix(native->GetDescriptor())
                    || !reference || !renderer::HaveMatchingFiberMaterialLayouts(reference->GetDescriptor(), native->GetDescriptor())) valid = false;
                state.shaders[mode * 2 + skin] = shader;
            }
        }
        state.shaderVersion = resources.GetShaderVersion();
        if (!valid) {
            FiberAppearanceWarning(state, "incomplete family, mismatched capability or incompatible 128-byte material prefix");
            state.resolved.reset();
            return;
        }
        /// @note A Shell material opts into Hybrid by supplying a complete StemFin family with the same material ABI.
        std::error_code finError;
        if (capability.vertex == renderer::ShaderVertexContract::FIBER_SHELL
            && std::filesystem::is_regular_file(stem + "Fin.hlsl", finError)) {
            state.finValid = true;
            for (size_t mode = 0; mode < 5; ++mode) {
                for (size_t skin = 0; skin < 2; ++skin) {
                    const auto shader = resources.LoadShader(stem + "Fin" + (skin ? "Skinned" : "") + suffixes[mode] + ".hlsl");
                    const auto* native = resources.Get(shader);
                    if (!native || resources.GetShaderCapabilities(shader).vertex != renderer::ShaderVertexContract::FIBER_FIN
                        || !renderer::HasFiberMaterialPrefix(native->GetDescriptor())
                        || !renderer::HaveMatchingFiberMaterialLayouts(reference->GetDescriptor(), native->GetDescriptor())) state.finValid = false;
                    state.finShaders[mode * 2 + skin] = shader;
                }
            }
            if (!state.finValid) state.finShaders = {};
        }
        state.shaderVersion = resources.GetShaderVersion();
        state.valid = true;
    }
    if (!state.valid) {
        state.resolved.reset();
        return;
    }
    const auto* native = resources.Get(state.shaders[0]);
    if (!native) {
        state.resolved.reset();
        return;
    }
    const auto& descriptor = native->GetDescriptor();
    state.material.Init(resources, descriptor.cbufferSize);
    asset::InitDefaultMaterialParams(descriptor, state.material.paramData);
    asset::ApplyMaterialAssetParams(values, descriptor, state.material.paramData);
    std::memcpy(state.material.paramData.data(), &settings, sizeof(settings));
    const auto textures = asset::ResolveMaterialTexturePaths(values);
    state.material.textures.resize(textures.size());
    for (size_t slot = 0; slot < textures.size(); ++slot)
        state.material.textures[slot] = textures[slot].empty() ? renderer::ResourceHandle<renderer::TextureTag>{}
            : asset::StreamedTextureResolver::Engine().ResolveGpu(resources, textures[slot]);
    /// @note Geometry mask remains the resolved tex5, including the white fallback.
    state.material.textures[5] = mask.IsValid() ? mask : resources.GetWhiteTexture();
    state.material.Upload(resources, descriptor);
    if (!state.resolved || state.resolved->params != state.material.paramData
        || state.resolved->shaders != state.shaders || state.resolved->finShaders != state.finShaders
        || state.resolved->supportsHybrid != state.finValid) {
        auto value = std::make_shared<renderer::FiberAppearance>();
        value->geometry = state.geometry;
        value->shaders = state.shaders;
        value->finShaders = state.finShaders;
        value->supportsHybrid = state.finValid;
        value->params = state.material.paramData;
        value->materialCB = state.material.paramsBuffer;
        state.resolved = std::move(value);
    }
}

std::shared_ptr<const renderer::FiberAppearance> FiberAppearanceFor(const FiberAppearanceResources& state,
    renderer::FiberRenderMode mode)
{
    if (!state.resolved) return {};
    if (renderer::MatchesFiberAppearance(state.resolved->geometry, mode, state.resolved->supportsHybrid)) return state.resolved;
    const uint32_t mask = 1u << static_cast<uint32_t>(mode);
    if (!(state.mismatchWarnings & mask)) {
        state.mismatchWarnings |= mask;
        if (mode == renderer::FiberRenderMode::HYBRID)
            FBZZ_LOG_WARN("Fiber appearance: Hybrid requires a Shell primary and a complete matching StemFin static/skinned family with identical material layout; using built-in geometry and appearance");
        else FBZZ_LOG_WARN("Fiber appearance: component mode does not match the project's declared family; using built-in geometry and appearance");
    }
    return {};
}

} /// @note namespace
} /// @note namespace fbzz::scene

