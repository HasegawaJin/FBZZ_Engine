/// @file    TerrainPaintClearance.cpp
/// @brief   実際の水域輪郭と保存層・海岸等高線から離隔距離を調べる。
/// @author  Hasegawa Jin
/// @date    2026-11-05
#include "TerrainPaintClearance.hpp"
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
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
    const int cx=Cell(p.x,index.cellSize),cz=Cell(p.z,index.cellSize),radius=static_cast<int>(std::ceil(margin/index.cellSize));
    for(int z=cz-radius;z<=cz+radius;++z)for(int x=cx-radius;x<=cx+radius;++x){
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

bool Near(std::span<const TerrainPaintEdge> edges,const TerrainPaintEdgeIndex& index,const math::Vector3& p,float margin,bool water)
{
    return VisitEdges(edges, index, p, margin, [&](const auto& edge) {
        const auto q = ClosestOnEdge(edge, p);
        return DistanceSquaredXZ(p, q) < margin * margin && (!water || p.y <= q.y + std::max(2.0f, margin));
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
    float pathMargin, bool combinePathLayers)
{
    TerrainPaintClearance out;
    if((waterMargin<=0&&pathMargin<=0)||region.centers.empty()||terrain.columns<2||terrain.rows<2)return out;
    const auto inverse=math::Matrix4::Inverse(transform.GetWorldMatrix()),world=transform.GetWorldMatrix();
    math::Vector3 lo{INFINITY,0,INFINITY},hi{-INFINITY,0,-INFINITY};
    for(const auto& p:region.centers){const auto q=inverse*math::Vector4{p,1};lo.x=std::min(lo.x,q.x);lo.z=std::min(lo.z,q.z);hi.x=std::max(hi.x,q.x);hi.z=std::max(hi.z,q.z);}
    const float margin=(std::max(waterMargin,pathMargin)+(region.polygon?0:region.radius))/std::min(std::abs(transform.worldScale.x),std::abs(transform.worldScale.z));
    const int x0=std::clamp(static_cast<int>(std::floor((lo.x-margin)/terrain.cellSize))-1,0,terrain.columns-2);
    const int z0=std::clamp(static_cast<int>(std::floor((lo.z-margin)/terrain.cellSize))-1,0,terrain.rows-2);
    const int x1=std::clamp(static_cast<int>(std::ceil((hi.x+margin)/terrain.cellSize))+1,0,terrain.columns-2);
    const int z1=std::clamp(static_cast<int>(std::ceil((hi.z+margin)/terrain.cellSize))+1,0,terrain.rows-2);
    std::vector<float> seas;
    if(waterMargin>0) for(const auto& [surface,pose]:surfaces) {
        scene::WaterSurfaceHeightSample sample;static_cast<void>(scene::TrySampleWaterSurfaceHeightWorld(*surface,*pose,region.centers.front(),0,sample));
        if(scene::WaterSurfaceUsesHorizon(*surface)){seas.push_back(pose->worldPosition.y+0.15f);continue;}
        if(const auto* geometry=scene::WaterSurfaceGeometry(*surface)) {
            const auto& ids=geometry->outlineIndices;
            for(size_t i=0;i<ids.size();++i) {
                const auto a=ids[i],b=ids[(i+1)%ids.size()];
                if(a<geometry->vertices.size()&&b<geometry->vertices.size())out.water.push_back({geometry->vertices[a].worldPosition,geometry->vertices[b].worldPosition});
            }
            for (const auto& ring : geometry->islandOutlines)
                for (std::size_t i = 0; i < ring.size(); ++i)
                    out.water.push_back({ring[i], ring[(i + 1) % ring.size()]});
        } else if(!scene::UsesWaterSurfaceGeometry(*surface)) {
            math::Vector3 corners[4];const auto matrix=pose->GetWorldMatrix();
            const float signs[][2]={{-1,-1},{1,-1},{1,1},{-1,1}};
            for(int i=0;i<4;++i){const auto p=matrix*math::Vector4{signs[i][0]*surface->extentX*0.5f,0,signs[i][1]*surface->extentZ*0.5f,1};corners[i]={p.x,p.y,p.z};}
            for(int i=0;i<4;++i)out.water.push_back({corners[i],corners[(i+1)%4]});
        }
    }
    if (seas.empty() && (pathMargin <= 0.0f || excludedLayers.empty())) {
        terrain_paint_clearance::Index(out.water, waterMargin, out.waterIndex);
        return out;
    }
    for(int z=z0;z<=z1;++z)for(int x=x0;x<=x1;++x) {
        math::Vector3 vertices[4];const int dx[]{0,1,1,0},dz[]{0,0,1,1};
        for(int i=0;i<4;++i){const float lx=(x+dx[i])*terrain.cellSize,lz=(z+dz[i])*terrain.cellSize;const auto q=world*math::Vector4{lx,terrain.GetHeightAt(lx,lz),lz,1};vertices[i]={q.x,q.y,q.z};}
        const int triangles[][3]={{0,1,2},{0,2,3}};
        const auto pathContour = [&](std::span<const int> layers) {
            float values[4]{};
            for(int layer:layers)for(int i=0;i<4;++i)values[i]+=terrain.GetLayerWeightAtGrid(x+dx[i],z+dz[i],layer);
            for(const auto& triangle:triangles){math::Vector3 p[3];float v[3];for(int i=0;i<3;++i){p[i]=vertices[triangle[i]];v[i]=values[triangle[i]];}terrain_paint_clearance::Contour(p,v,0.35f,out.paths);}
        };
        if (pathMargin > 0 && combinePathLayers && !terrain.IsHoleCell(x, z)) pathContour(excludedLayers);
        if (pathMargin > 0 && !combinePathLayers)
            for (int layer : excludedLayers) pathContour(std::span{&layer, 1u});
        for(float sea:seas)for(const auto& triangle:triangles){math::Vector3 p[3];float v[3];for(int i=0;i<3;++i){p[i]=vertices[triangle[i]];v[i]=p[i].y;}terrain_paint_clearance::Contour(p,v,sea,out.water);}
    }
    terrain_paint_clearance::Index(out.water,waterMargin,out.waterIndex);
    terrain_paint_clearance::Index(out.paths,pathMargin,out.pathIndex);
    return out;
}
} /// @note namespace fbzz::editor
