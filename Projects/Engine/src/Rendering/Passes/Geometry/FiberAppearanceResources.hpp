/// @file    FiberAppearanceResources.hpp
/// @brief   エンジンの形状を保ったまま、プロジェクトの Fiber シェーダー群と反射情報に基づく材質データを解決する。
/// @author  Hasegawa Jin
/// @date    2026-10-21
#pragma once
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Material/MaterialParamBinding.hpp>
#include <Engine/Asset/Streaming/StreamedTextureResolver.hpp>
#include <Graphics/Renderer/Materials/Material.hpp>
#include <Graphics/Renderer/Shaders/IShader.hpp>
#include <Engine/Core/Diagnostics/Logger.hpp>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

namespace fbzz::scene {
namespace fiber_extraction_detail {

struct FiberAppearanceResources {
    FiberAppearanceResources() = default;
    FiberAppearanceResources(FiberAppearanceResources&&) = default;
    FiberAppearanceResources& operator=(FiberAppearanceResources&&) = default;
    /// @note 静的キャッシュが資源管理者より長く残る場合があるため、通常の追い出し時は対応する管理者を通して明示解放する。
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

inline void FiberAppearanceWarning(FiberAppearanceResources& state, const std::string& message)
{
    if (state.warning == message) return;
    state.warning = message;
    FBZZ_LOG_WARN("Fiber appearance: %s; using the compatible built-in family", message.c_str());
}

inline bool IsSharedFiberGeometry(renderer::ShaderVertexContract vertex)
{
    return vertex == renderer::ShaderVertexContract::FIBER_SHELL
        || vertex == renderer::ShaderVertexContract::FIBER_FIN
        || vertex == renderer::ShaderVertexContract::FIBER_BLADE;
}

inline void ResolveFiberAppearance(FiberAppearanceResources& state, const asset::MaterialAssetAuthoring& values,
    const renderer::FiberMaterialSettings& settings, renderer::ResourceHandle<renderer::TextureTag> mask,
    renderer::ResourceManager& resources)
{
    const auto primary = resources.LoadShader(values.shaderPath);
    const auto capability = resources.GetShaderCapabilities(primary);
    if (!IsSharedFiberGeometry(capability.vertex)) {
        state.resolved.reset();
        /// @note プレビュー専用の旧メタデータを含む既存の Fiber 材質には、従来の固定シェーダー群を使う。
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
        /// @note Shell 材質は同じ材質 ABI の StemFin シェーダー群をすべて提供すると Hybrid を使える。
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
    /// @note ジオメトリマスクには白テクスチャへのフォールバックを含め、解決済みの tex5 を使う。
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

inline std::shared_ptr<const renderer::FiberAppearance> FiberAppearanceFor(const FiberAppearanceResources& state,
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

} /// @note namespace fiber_extraction_detail
} /// @note namespace fbzz::scene
