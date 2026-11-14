/// @file    TerrainPaintClearance.cpp
/// @brief   実際の水域輪郭と保存層・海岸等高線から離隔距離を調べる。
/// @author  Hasegawa Jin
/// @date    2026-11-05
#include "TerrainPaintClearance.hpp"
#include <Engine/Profiler/ProfileScope.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/TerrainSplat.hpp>
#include <Engine/Scene/WaterSpline.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <Engine/Scene/Transform.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::editor {
namespace terrain_paint_clearance {
int Cell(float value, float size) { return static_cast<int>(std::clamp(std::floor(static_cast<double>(value)/size),-1000000000.0,1000000000.0)); }
uint64_t Key(int x,int z) { return (static_cast<uint64_t>(static_cast<uint32_t>(x))<<32)|static_cast<uint32_t>(z); }
void Index(std::span<const TerrainPaintEdge> edges,float margin,TerrainPaintEdgeIndex& index)
{
    index.cellSize=std::max(4.0f,margin);
    for(size_t i=0;i<edges.size();++i){const auto& e=edges[i];
        const int x0=Cell(std::min(e.a.x,e.b.x),index.cellSize),x1=Cell(std::max(e.a.x,e.b.x),index.cellSize);
        const int z0=Cell(std::min(e.a.z,e.b.z),index.cellSize),z1=Cell(std::max(e.a.z,e.b.z),index.cellSize);
        /// @note 長い斜めの水面外周は全候補に少数だけ照合し、外接矩形によるセル増大を避ける。
        if((static_cast<int64_t>(x1)-x0+1)*(static_cast<int64_t>(z1)-z0+1)>4096){index.global.push_back(i);continue;}
        if (index.cells.empty()) {
            index.minX = x0; index.maxX = x1;
            index.minZ = z0; index.maxZ = z1;
        } else {
            index.minX = (std::min)(index.minX, x0); index.maxX = (std::max)(index.maxX, x1);
            index.minZ = (std::min)(index.minZ, z0); index.maxZ = (std::max)(index.maxZ, z1);
        }
        for(int z=z0;z<=z1;++z)for(int x=x0;x<=x1;++x)index.cells[Key(x,z)].push_back(i);
    }
}
template<typename Test>
bool VisitEdges(std::span<const TerrainPaintEdge> edges,const TerrainPaintEdgeIndex& index,
    const math::Vector3& p,float margin,Test test)
{
    if(margin<=0)return false;
    if(index.cells.empty()&&index.global.empty()){for(const auto& e:edges)if(test(e))return true;return false;}
    for(size_t i:index.global)if(test(edges[i]))return true;
    if (index.cells.empty()) return false;
    const int cx=Cell(p.x,index.cellSize),cz=Cell(p.z,index.cellSize),radius=static_cast<int>(std::ceil(margin/index.cellSize));
    const int minX = (std::max)(cx - radius, index.minX), maxX = (std::min)(cx + radius, index.maxX);
    const int minZ = (std::max)(cz - radius, index.minZ), maxZ = (std::min)(cz + radius, index.maxZ);
    for(int z=minZ;z<=maxZ;++z)for(int x=minX;x<=maxX;++x){
        const auto found=index.cells.find(Key(x,z));if(found==index.cells.end())continue;
        for(size_t i:found->second)if(test(edges[i]))return true;
    }
    return false;
}

math::Vector3 ClosestOnEdge(const TerrainPaintEdge& e, const math::Vector3& p)
{
    const float dx=e.b.x-e.a.x,dz=e.b.z-e.a.z;
    const float length=dx*dx+dz*dz;
    const float t=length>0?std::clamp(((p.x-e.a.x)*dx+(p.z-e.a.z)*dz)/length,0.0f,1.0f):0;
    return e.a+(e.b-e.a)*t;
}

float DistanceSquaredXZ(const math::Vector3& p, const math::Vector3& q)
{
    const float x = p.x - q.x, z = p.z - q.z;
    return x * x + z * z;
}

bool WithinWaterDistance(float squaredDistance, float waterY, float groundY, float margin)
{
    return squaredDistance < margin * margin && groundY <= waterY + (std::max)(2.0f, margin);
}

bool Near(std::span<const TerrainPaintEdge> edges,const TerrainPaintEdgeIndex& index,const math::Vector3& p,float margin,bool water)
{
    return VisitEdges(edges, index, p, margin, [&](const auto& edge) {
        const auto q = ClosestOnEdge(edge, p);
        const float distance = DistanceSquaredXZ(p, q);
        return water ? WithinWaterDistance(distance, q.y, p.y, margin) : distance < margin * margin;
    });
}

bool Nearest(std::span<const TerrainPaintEdge> edges, const TerrainPaintEdgeIndex& index,
    const math::Vector3& p, float radius, math::Vector3& nearest)
{
    float best = radius * radius;
    bool found = false;
    VisitEdges(edges, index, p, radius, [&](const auto& edge) {
        const auto q = ClosestOnEdge(edge, p);
        const float distance = DistanceSquaredXZ(p, q);
        if (distance < best || (distance == best && (!found || std::abs(p.y - q.y) < std::abs(p.y - nearest.y)))) {
            best = distance;
            nearest = q;
            found = true;
        }
        return false;
    });
    return found;
}
void Contour(const math::Vector3* p,const float* v,float threshold,std::vector<TerrainPaintEdge>& edges)
{
    math::Vector3 hits[3];size_t count=0;
    for(int i=0;i<3;++i){const int j=(i+1)%3;if((v[i]>=threshold)==(v[j]>=threshold))continue;
        const float t=(threshold-v[i])/(v[j]-v[i]);hits[count++]=p[i]+(p[j]-p[i])*t;}
    if(count==2)edges.push_back({hits[0],hits[1]});
}
} /// @note namespace terrain_paint_clearance
bool TerrainPaintClearance::NearWater(const math::Vector3& p,float distance) const {return terrain_paint_clearance::Near(water,waterIndex,p,distance,true);}
float TerrainPaintClearance::WaterDistance(const math::Vector3& p, float radius) const
{
    using namespace terrain_paint_clearance;
    constexpr float NO_WATER = 1000000.0f;
    if (!(radius > 0.0f)) return NO_WATER;
    float nearest = NO_WATER;
    float lastRejected = radius;
    VisitEdges(water, waterIndex, p, radius, [&](const auto& edge) {
        const auto point = ClosestOnEdge(edge, p);
        const float squaredDistance = DistanceSquaredXZ(p, point);
        if (!WithinWaterDistance(squaredDistance, point.y, p.y, lastRejected)) return false;
        /// @note Preserve the float midpoints and strict XZ comparison of the original 16-step search.
        float low = 0.0f, high = radius;
        for (int step = 0; step < 16; ++step) {
            const float middle = (low + high) * 0.5f;
            if (WithinWaterDistance(squaredDistance, point.y, p.y, middle)) high = middle;
            else low = middle;
        }
        nearest = high;
        lastRejected = low;
        return low == 0.0f;
    });
    return nearest;
}
bool TerrainPaintClearance::NearPath(const math::Vector3& p,float distance) const {return terrain_paint_clearance::Near(paths,pathIndex,p,distance,false);}
bool TerrainPaintClearance::NearestWater(const math::Vector3& p, float radius, math::Vector3& nearest) const
{
    return terrain_paint_clearance::Nearest(water, waterIndex, p, radius, nearest);
}

float TerrainPaintClearance::PathDistance(const math::Vector3& p, float radius) const
{
    math::Vector3 nearest;
    return terrain_paint_clearance::Nearest(paths, pathIndex, p, radius, nearest)
        ? std::sqrt(terrain_paint_clearance::DistanceSquaredXZ(p, nearest)) : 1000000.0f;
}
TerrainPaintClearance BuildTerrainPaintClearance(const scene::TerrainComponent& terrain,const scene::Transform& transform,
    const TerrainScatterBrush& region,std::span<const int> excludedLayers,
    std::span<const std::pair<const scene::WaterComponent*,const scene::Transform*>> surfaces,float waterMargin,
    float pathMargin, bool combinePathLayers, std::size_t* remainingWork, bool includeFiniteWaterOutlines)
{
    FBZZ_PROFILE_SCOPE("WorldPcg::Clearance");
    TerrainPaintClearance out;
    const auto spend = [&](std::size_t count, std::size_t tests = 1) {
        if (!remainingWork) return true;
        if (count > *remainingWork / tests) { out.budgetExceeded = true; return false; }
        *remainingWork -= count * tests;
        return true;
    };
    if((waterMargin<=0&&pathMargin<=0)||region.centers.empty()||terrain.columns<2||terrain.rows<2)return out;
    std::vector<float> seas;
    if(waterMargin>0) for(const auto& [surface,pose]:surfaces) {
        if(scene::WaterSurfaceUsesHorizon(*surface)){seas.push_back(pose->worldPosition.y+0.15f);continue;}
        if (!includeFiniteWaterOutlines) continue;
        if(const auto* geometry=scene::WaterSurfaceGeometry(*surface)) {
            const auto& ids=geometry->outlineIndices;
            if (!spend(ids.size())) return out;
            for(size_t i=0;i<ids.size();++i) {
                const auto a=ids[i],b=ids[(i+1)%ids.size()];
                if(a<geometry->vertices.size()&&b<geometry->vertices.size())out.water.push_back({geometry->vertices[a].worldPosition,geometry->vertices[b].worldPosition});
            }
            for (const auto& ring : geometry->islandOutlines) {
                if (!spend(ring.size())) return out;
                for (std::size_t i = 0; i < ring.size(); ++i)
                    out.water.push_back({ring[i], ring[(i + 1) % ring.size()]});
            }
        } else if(!scene::UsesWaterSurfaceGeometry(*surface)) {
            if (!spend(4)) return out;
            math::Vector3 corners[4];const auto matrix=pose->GetWorldMatrix();
            const float signs[][2]={{-1,-1},{1,-1},{1,1},{-1,1}};
            for(int i=0;i<4;++i){const auto p=matrix*math::Vector4{signs[i][0]*surface->extentX*0.5f,0,signs[i][1]*surface->extentZ*0.5f,1};corners[i]={p.x,p.y,p.z};}
            for(int i=0;i<4;++i)out.water.push_back({corners[i],corners[(i+1)%4]});
        }
    }
    const bool buildPaths = pathMargin > 0.0f && !excludedLayers.empty();
    const auto finish = [&] {
        terrain_paint_clearance::Index(out.water, waterMargin, out.waterIndex);
        terrain_paint_clearance::Index(out.paths, pathMargin, out.pathIndex);
    };
    if (seas.empty() && !buildPaths) {
        finish();
        return out;
    }

    const auto world = transform.GetWorldMatrix();
    const auto inverse = math::Matrix4::Inverse(world);
    math::Vector3 lo{INFINITY, 0, INFINITY}, hi{-INFINITY, 0, -INFINITY};
    for (const auto& point : region.centers) {
        const auto local = inverse * math::Vector4{point, 1};
        lo.x = (std::min)(lo.x, local.x);
        lo.z = (std::min)(lo.z, local.z);
        hi.x = (std::max)(hi.x, local.x);
        hi.z = (std::max)(hi.z, local.z);
    }
    const float margin = ((std::max)(waterMargin, pathMargin) + (region.polygon ? 0.0f : region.radius))
        / (std::min)(std::abs(transform.worldScale.x), std::abs(transform.worldScale.z));
    if (!std::isfinite(terrain.cellSize) || terrain.cellSize <= 0.0f
        || !std::isfinite(margin) || !std::isfinite(lo.x) || !std::isfinite(lo.z)
        || !std::isfinite(hi.x) || !std::isfinite(hi.z)
        || (!terrain.heightData.empty() && terrain.heightData.size() != terrain.VertexCount())) {
        finish();
        return out;
    }
    const float firstX = std::floor((lo.x - margin) / terrain.cellSize) - 1.0f;
    const float firstZ = std::floor((lo.z - margin) / terrain.cellSize) - 1.0f;
    const float lastX = std::ceil((hi.x + margin) / terrain.cellSize) + 1.0f;
    const float lastZ = std::ceil((hi.z + margin) / terrain.cellSize) + 1.0f;
    /// @note Upright tiles have height-independent XZ footprints. A tilted tile can project ground into the search area.
    if (world.m[0][1] == 0.0f && world.m[2][1] == 0.0f && inverse.m[0][1] == 0.0f && inverse.m[2][1] == 0.0f
        && (lastX < 0.0f || lastZ < 0.0f || firstX > terrain.columns - 2 || firstZ > terrain.rows - 2)) {
        finish();
        return out;
    }
    const auto cellIndex = [](float value, int count) {
        return static_cast<int>(std::clamp(value, 0.0f, static_cast<float>(count - 2)));
    };
    const int x0 = cellIndex(firstX, terrain.columns), x1 = cellIndex(lastX, terrain.columns);
    const int z0 = cellIndex(firstZ, terrain.rows), z1 = cellIndex(lastZ, terrain.rows);
    const auto cells = static_cast<std::size_t>(x1 - x0 + 1) * static_cast<std::size_t>(z1 - z0 + 1);
    if (!spend(cells, 1 + seas.size() + (pathMargin > 0.0f ? excludedLayers.size() : 0))) return out;
    const auto projectVertex = [&](int x, int z) {
        const float localX = x * terrain.cellSize, localZ = z * terrain.cellSize;
        const auto p = world * math::Vector4{localX, terrain.GetHeightAt(localX, localZ), localZ, 1.0f};
        return math::Vector3{p.x, p.y, p.z};
    };
    constexpr int DX[]{0, 1, 1, 0}, DZ[]{0, 0, 1, 1};
    constexpr int TRIANGLES[][3]{{0, 1, 2}, {0, 2, 3}};
    const auto rowWidth = static_cast<std::size_t>(x1 - x0 + 2);
    std::vector<math::Vector3> north, south;
    if (!seas.empty()) {
        north.resize(rowWidth);
        south.resize(north.size());
        for (int x = x0; x <= x1 + 1; ++x) north[x - x0] = projectVertex(x, z0);
    }
    const std::size_t pathMasks = buildPaths && terrain.HasValidSplat()
        ? (combinePathLayers ? 1u : excludedLayers.size()) : 0;
    std::vector<float> pathNorth(rowWidth * pathMasks), pathSouth(pathNorth.size());
    const auto samplePathRow = [&](int z, std::span<float> row) {
        const auto first = (static_cast<std::size_t>(z) * terrain.columns + x0) * scene::TERRAIN_SPLAT_SLOTS;
        for (std::size_t mask = 0; mask < pathMasks; ++mask) {
            const auto layers = combinePathLayers ? excludedLayers : excludedLayers.subspan(mask, 1);
            for (std::size_t x = 0; x < rowWidth; ++x) {
                const auto vertex = first + x * scene::TERRAIN_SPLAT_SLOTS;
                float value = 0.0f;
                for (const int layer : layers)
                    value += scene::terrain_splat::WeightOf(terrain.splatIndices.data() + vertex,
                        terrain.splatWeights.data() + vertex, layer);
                row[mask * rowWidth + x] = value;
            }
        }
    };
    /// @note Adjacent contour cells share their path mask samples. Layer accumulation order is preserved.
    samplePathRow(z0, pathNorth);
    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; !south.empty() && x <= x1 + 1; ++x) south[x - x0] = projectVertex(x, z + 1);
        samplePathRow(z + 1, pathSouth);
        for (int x = x0; x <= x1; ++x) {
            math::Vector3 vertices[4];
            bool projected = !seas.empty();
            if (projected) {
                vertices[0] = north[x - x0];
                vertices[1] = north[x - x0 + 1];
                vertices[2] = south[x - x0 + 1];
                vertices[3] = south[x - x0];
            }
            const auto emit = [&](const float (&values)[4], float threshold, std::vector<TerrainPaintEdge>& edges) {
                const bool firstAbove = values[0] >= threshold;
                if ((values[1] >= threshold) == firstAbove && (values[2] >= threshold) == firstAbove
                    && (values[3] >= threshold) == firstAbove) return;
                if (!projected) {
                    for (int i = 0; i < 4; ++i) vertices[i] = projectVertex(x + DX[i], z + DZ[i]);
                    projected = true;
                }
                for (const auto& triangle : TRIANGLES) {
                    const math::Vector3 points[]{vertices[triangle[0]], vertices[triangle[1]], vertices[triangle[2]]};
                    const float weights[]{values[triangle[0]], values[triangle[1]], values[triangle[2]]};
                    terrain_paint_clearance::Contour(points, weights, threshold, edges);
                }
            };
            if (pathMasks != 0 && (!combinePathLayers || !terrain.IsHoleCell(x, z))) {
                for (std::size_t mask = 0; mask < pathMasks; ++mask) {
                    const auto i = mask * rowWidth + static_cast<std::size_t>(x - x0);
                    const float values[]{pathNorth[i], pathNorth[i + 1], pathSouth[i + 1], pathSouth[i]};
                    emit(values, 0.35f, out.paths);
                }
            }
            if (!seas.empty()) {
                const float heights[]{vertices[0].y, vertices[1].y, vertices[2].y, vertices[3].y};
                for (float sea : seas) emit(heights, sea, out.water);
            }
        }
        north.swap(south);
        pathNorth.swap(pathSouth);
    }
    finish();
    return out;
}
} /// @note namespace fbzz::editor
