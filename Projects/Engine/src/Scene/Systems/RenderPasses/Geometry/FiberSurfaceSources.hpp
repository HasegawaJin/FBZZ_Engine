/// @file    FiberSurfaceSources.hpp
/// @brief   地形パッチと繊維接触履歴のレンダースレッド用キャッシュ。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#pragma once
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/FiberInteractorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <bit>
#include <cstring>
#include <limits>

namespace fbzz::scene {
namespace {
struct FiberTerrainCache {
    uint64_t m_signature = 0;
    uint64_t m_frame = (std::numeric_limits<uint64_t>::max)();
    uint64_t m_seen = 0;
    std::vector<renderer::Mesh> m_patches;
};

struct FiberContactCache {
    FiberContactCB m_data;
    uint64_t m_frame = (std::numeric_limits<uint64_t>::max)();
    float m_time = 0;
    uint32_t m_cursor = 0;
};

/// @note 各ビュー・影パスで二重に進めない。32 接触を循環保持し、過去側は速度出力まで固定する。
void UpdateFiberContacts(FiberContactCache& cache, Scene& scene)
{
    if (cache.m_frame == Time::frameCount) return;
    if (Time::time < cache.m_time || Time::frameCount < cache.m_frame) cache = {};
    cache.m_frame=Time::frameCount;
    cache.m_time=Time::time;
    for (size_t i=0;i<32;++i) {
        cache.m_data.m_centers[i+32]=cache.m_data.m_centers[i];
        cache.m_data.m_times[i+32]=cache.m_data.m_times[i];
    }
    for (auto& go : scene.GameObjects()) {
        const auto* actor=go.GetComponent<FiberInteractorComponent>();
        if (!go.activeInHierarchy() || !actor || !actor->m_enabled) continue;
        const auto center=go.transform.worldPosition;
        if (!std::isfinite(center.x)||!std::isfinite(center.y)||!std::isfinite(center.z)
            || !std::isfinite(actor->m_radius)||!std::isfinite(actor->m_strength)
            || !std::isfinite(actor->m_recoverySeconds)) continue;
        const float radius=std::clamp(actor->m_radius,0.01f,10.0f);
        size_t slot=32;
        for (size_t i=0;i<32;++i) {
            const auto& old=cache.m_data.m_centers[i];
            if (old.w>0 && (math::Vector3{old.x,old.y,old.z}-center).LengthSq()<radius*radius*0.0625f) {slot=i;break;}
        }
        if (slot==32) slot=cache.m_cursor++%32;
        cache.m_data.m_centers[slot]={center.x,center.y,center.z,radius};
        cache.m_data.m_times[slot]={Time::time,std::clamp(actor->m_strength,0.0f,1.0f),
            std::clamp(actor->m_recoverySeconds,0.05f,30.0f),0};
    }
    UpdateFiberContactCounts(cache.m_data, Time::time);
}

void ReleaseFiberTerrain(FiberTerrainCache& cache, renderer::ResourceManager& resources)
{
    for (auto& mesh : cache.m_patches) {
        resources.Release(mesh.vertexBuffer);
        resources.Release(mesh.indexBuffer);
    }
    cache.m_patches.clear();
}

/// @note dirty フラグは Terrain パスが消費するため共有しない。1 フレーム 1 回の内容署名で編集を検出する。
void UpdateFiberTerrain(FiberTerrainCache& cache, const TerrainComponent& terrain, int patchCells,
                        renderer::ResourceManager& resources)
{
    cache.m_seen=Time::frameCount;
    if (cache.m_frame==Time::frameCount) return;
    cache.m_frame=Time::frameCount;
    const int patch=std::clamp(patchCells,1,32);
    uint64_t hash=1469598103934665603ull;
    /// @note 64bit 語単位で混ぜる。回転で上位ビットの変化も下位へ回し、打ち消し合う編集を起こりにくくする。
    const auto add=[&hash](uint64_t value){hash=(std::rotl(hash,29)^value)*1099511628211ull;};
    add(static_cast<uint32_t>(terrain.columns));add(static_cast<uint32_t>(terrain.rows));add(static_cast<uint32_t>(patch));
    add(std::bit_cast<uint32_t>(terrain.cellSize));add(std::bit_cast<uint32_t>(terrain.maxHeight));
    /// @note 毎フレーム全標本を読むため、float 2 個ずつ 1 語で処理して 1 標本あたりの乗算を半分にする。
    const size_t samples=terrain.heightData.size();
    const float* heights=terrain.heightData.data();
    size_t sample=0;
    for (;sample+1<samples;sample+=2) {
        uint64_t word=0;
        std::memcpy(&word,heights+sample,sizeof(word));
        add(word);
    }
    if (sample<samples) add(std::bit_cast<uint32_t>(heights[sample]));
    add(samples);
    if (cache.m_signature==hash) return;
    cache.m_signature=hash;
    ReleaseFiberTerrain(cache,resources);
    if (terrain.columns<2 || terrain.rows<2 || terrain.columns>4097 || terrain.rows>4097
        || terrain.heightData.size()!=static_cast<size_t>(terrain.columns)*terrain.rows
        || !std::isfinite(terrain.cellSize)||terrain.cellSize<=0||!std::isfinite(terrain.maxHeight)) return;
    for (float value:terrain.heightData) if (!std::isfinite(value)) return;
    for (int z=0;z<terrain.rows-1;z+=patch) for (int x=0;x<terrain.columns-1;x+=patch) {
        const int nx=std::min(patch,terrain.columns-1-x), nz=std::min(patch,terrain.rows-1-z);
        renderer::Mesh mesh;
        for (int iz=0;iz<=nz;++iz) for (int ix=0;ix<=nx;++ix) {
            renderer::Vertex vertex;
            vertex.position={static_cast<float>(x+ix)*terrain.cellSize,
                terrain.heightData[static_cast<size_t>(z+iz)*terrain.columns+x+ix]*terrain.maxHeight,
                static_cast<float>(z+iz)*terrain.cellSize};
            vertex.normal=terrain.ComputeNormal(x+ix,z+iz);
            vertex.tangent={1,0,0};
            vertex.uv={vertex.position.x,vertex.position.z};
            mesh.cpuVertices.push_back(vertex);
        }
        for (int iz=0;iz<nz;++iz) for (int ix=0;ix<nx;++ix) {
            const uint32_t a=static_cast<uint32_t>(iz*(nx+1)+ix), b=a+static_cast<uint32_t>(nx+1);
            mesh.cpuIndices.insert(mesh.cpuIndices.end(),{a,b,a+1,a+1,b,b+1});
        }
        mesh.ComputeBounds();
        mesh.vertexCount=static_cast<uint32_t>(mesh.cpuVertices.size());
        mesh.indexCount=static_cast<uint32_t>(mesh.cpuIndices.size());
        mesh.vertexBuffer=resources.CreateVertexBuffer(mesh.cpuVertices.data(),mesh.cpuVertices.size()*sizeof(renderer::Vertex),sizeof(renderer::Vertex));
        mesh.indexBuffer=resources.CreateIndexBuffer(mesh.cpuIndices.data(),mesh.indexCount);
        cache.m_patches.push_back(std::move(mesh));
    }
}
} // namespace
} // namespace fbzz::scene
