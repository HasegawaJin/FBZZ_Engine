/// @file    FiberPlacementResources.hpp
/// @brief   個体別の表面ペイントと Blade チャンクの描画用キャッシュ。
/// @author  Hasegawa Jin
/// @date    2026-10-11
#pragma once
#include <Engine/Asset/FiberPaintAsset.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Scene/Components/FiberComponent.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Time.hpp>
#include <bit>
#include <map>

namespace fbzz::scene {
namespace {
struct FiberPlacementPage {
    renderer::ResourceHandle<renderer::StructuredBufferTag> buffer;
    uint32_t count = 0;
    uint64_t revision = 0, layout = 0, identity = 0, seen = 0;
    math::Vector3 center{};
    float radius = 0;
};
struct FiberPlacementResources {
    renderer::FiberPlacementCache cpu;
    renderer::FiberPaintData fullSurface;
    std::map<std::array<int,4>,FiberPlacementPage> pages;
    std::array<uint32_t,25> input{};
    uint64_t seen = 0;
    bool valid = false;
};
void ReleaseFiberPlacement(FiberPlacementResources& state, renderer::ResourceManager& resources)
{
    for (auto& [key,page] : state.pages) resources.Release(page.buffer);
    state.pages.clear();
}
/// @note 同一個体を全ビュー・影パスで共有する。未変更なら CPU 三角形・マスクを走査しない。
void UpdateFiberPlacementResources(FiberPlacementResources& state, const renderer::Mesh& mesh,
    FiberComponent& fiber, const math::Matrix4& world, renderer::ResourceManager& resources)
{
    state.seen = Time::frameCount;
    const asset::FiberPaintAsset* paint = fiber.m_paintPath.empty() ? nullptr
        : asset::AssetManager::Get(asset::AssetManager::Load<asset::FiberPaintAsset>(fiber.m_paintPath));
    const uint64_t paintRevision = paint ? paint->revision : 0;
    std::array<uint32_t,25> input{
        mesh.vertexBuffer.id,mesh.vertexBuffer.gen,mesh.indexBuffer.id,mesh.indexBuffer.gen,
        std::bit_cast<uint32_t>(fiber.m_bladeDensity),std::bit_cast<uint32_t>(fiber.m_bladeWidth),
        std::bit_cast<uint32_t>(fiber.m_chunkSize),std::bit_cast<uint32_t>(fiber.m_maxSlope),
        static_cast<uint32_t>(fiber.m_seed),static_cast<uint32_t>(fiber.m_maxBlades),
        static_cast<uint32_t>(paintRevision),static_cast<uint32_t>(paintRevision>>32),
        fiber.m_paintPath.empty() ? 0u : 1u,
        static_cast<uint32_t>(mesh.cpuVertices.size()),static_cast<uint32_t>(mesh.cpuIndices.size())};
    size_t cursor = 15;
    for (int row = 0; row < 3; ++row) for (int col = 0; col < 3; ++col) input[cursor++] = std::bit_cast<uint32_t>(world.m[row][col]);
    if (input == state.input) return;
    state.input = input;
    state.valid = false;
    fiber.m_placementFailed = true;
    fiber.m_placementRootCount = 0; fiber.m_placementChunkCount = 0;
    if (!fiber.m_paintPath.empty() && !paint) return;
    if (!paint) {
        const float scale = std::max({math::Vector3(world.m[0][0],world.m[1][0],world.m[2][0]).Length(),
            math::Vector3(world.m[0][1],world.m[1][1],world.m[2][1]).Length(),
            math::Vector3(world.m[0][2],world.m[1][2],world.m[2][2]).Length(),0.0001f});
        if (!renderer::CreateFiberPaintData(mesh,std::clamp(0.5f/scale,0.0001f,16.0f),1.0f,state.fullSurface)) return;
    }
    renderer::FiberPlacementOptions options;
    options.density = fiber.m_bladeDensity; options.width = fiber.m_bladeWidth;
    options.chunkSize = fiber.m_chunkSize; options.maxSlope = fiber.m_maxSlope;
    options.seed = static_cast<uint32_t>(fiber.m_seed); options.maxBlades = static_cast<uint32_t>(fiber.m_maxBlades); options.world = world;
    if (!renderer::UpdateFiberPlacement(mesh,paint ? paint->data : state.fullSurface,options,state.cpu)) {
        core::Logger::Warn("Fiber: surface placement rejected topology, invalid input or total blade budget; check paint asset, density and maxBlades");
        return;
    }
    static uint64_t nextIdentity = 0x8000000000000000ull;
    for (const auto& chunk : state.cpu.chunks) {
        for (size_t first = 0, pageIndex = 0; first < chunk.roots.size(); first += renderer::FIBER_MAX_BLADES, ++pageIndex) {
            const std::array<int,4> key{chunk.cell[0],chunk.cell[1],chunk.cell[2],static_cast<int>(pageIndex)};
            auto& page = state.pages[key];
            page.seen = state.cpu.revision;
            if (!page.identity) page.identity = ++nextIdentity;
            const auto count = static_cast<uint32_t>(std::min(chunk.roots.size()-first,static_cast<size_t>(renderer::FIBER_MAX_BLADES)));
            if (page.revision != chunk.revision || page.layout != state.cpu.layoutSignature || !page.buffer.IsValid()) {
                auto fresh = resources.CreateGpuLocalStructuredBuffer(chunk.roots.data()+first,count,sizeof(renderer::FiberBladeRoot));
                if (!fresh.IsValid()) return;
                resources.Release(page.buffer); page.buffer = fresh;
                page.revision = chunk.revision; page.layout = state.cpu.layoutSignature;
            }
            page.count = count; page.center = chunk.center; page.radius = chunk.radius;
        }
    }
    for (auto it = state.pages.begin(); it != state.pages.end();) {
        if (it->second.seen != state.cpu.revision) { resources.Release(it->second.buffer); it = state.pages.erase(it); }
        else ++it;
    }
    state.valid = true; fiber.m_placementFailed = false;
    fiber.m_placementRootCount = state.cpu.bladeCount; fiber.m_placementChunkCount = static_cast<uint32_t>(state.pages.size());
}
} /// @note namespace
} /// @note namespace fbzz::scene
