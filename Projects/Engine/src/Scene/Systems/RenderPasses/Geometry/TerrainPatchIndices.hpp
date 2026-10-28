/// @file    TerrainPatchIndices.hpp
/// @brief   Keep shared patch borders at unit spacing while coarsening the interior and preserving holes.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <algorithm>
#include <cassert>
#include <vector>

namespace fbzz::scene {
/// @note Appends triangle-list indices into the unchanged full-resolution vertex grid; every LOD shares the same border polyline.
/// @note Hole blocks retain unit cells; adjacent coarse blocks split their common edge to avoid internal T-junctions.
inline void BuildTerrainPatchLodIndices(const TerrainComponent& terrain,int x0,int z0,int x1,int z1,int step,std::vector<uint32_t>& output)
{
    assert(step>0); const int width=x1-x0,height=z1-z0,stride=width+1;
    const bool holes=terrain.holeData.size()==terrain.CellCount();
    const auto index=[&](int x,int z){return static_cast<uint32_t>(z*stride+x);};
    const auto hasHole=[&](int x,int z,int endX,int endZ) {
        if (!holes) return false;
        for (int cz=z;cz<endZ;++cz) for (int cx=x;cx<endX;++cx)
            if (terrain.IsHoleCell(x0+cx,z0+cz)) return true;
        return false;
    };
    const auto quad=[&](int x,int z,int endX,int endZ) {
        output.insert(output.end(),{index(x,z),index(x,endZ),index(endX,z),index(endX,z),index(x,endZ),index(endX,endZ)});
    };
    for (int z=0;z<height;z+=step) for (int x=0;x<width;x+=step) {
        const int endX=std::min(x+step,width),endZ=std::min(z+step,height);
        if (hasHole(x,z,endX,endZ)) {
            for (int cz=z;cz<endZ;++cz) for (int cx=x;cx<endX;++cx)
                if (!terrain.IsHoleCell(x0+cx,z0+cz)) quad(cx,cz,cx+1,cz+1);
            continue;
        }
        /// @note A one-cell-wide strip has no interior fan vertex; retain unit quads and split its neighbor's shared edge.
        if (step==1 || endX-x==1 || endZ-z==1) {
            for (int cz=z;cz<endZ;++cz) for (int cx=x;cx<endX;++cx) quad(cx,cz,cx+1,cz+1);
            continue;
        }
        const bool left=x==0 || hasHole(std::max(x-step,0),z,x,endZ);
        const bool right=endX==width || width-endX==1 || hasHole(endX,z,std::min(endX+step,width),endZ);
        const bool top=endZ==height || height-endZ==1 || hasHole(x,endZ,endX,std::min(endZ+step,height));
        const bool bottom=z==0 || hasHole(x,std::max(z-step,0),endX,z);
        if (!left && !right && !top && !bottom) { quad(x,z,endX,endZ); continue; }
        const int centerX=x+(endX-x)/2,centerZ=z+(endZ-z)/2;
        const uint32_t center=index(centerX,centerZ);
        const auto triangle=[&](int ax,int az,int bx,int bz) {
            /// @note Partial edge blocks may place the center on a border; omit zero-area XZ triangles, including vertical height folds.
            const int area=(az-centerZ)*(bx-centerX)-(ax-centerX)*(bz-centerZ);
            if (area>0) output.insert(output.end(),{center,index(ax,az),index(bx,bz)});
        };
        for (int cz=z;cz<endZ;) { const int next=left ? cz+1 : endZ; triangle(x,cz,x,next); cz=next; }
        for (int cx=x;cx<endX;) { const int next=top ? cx+1 : endX; triangle(cx,endZ,next,endZ); cx=next; }
        for (int cz=endZ;cz>z;) { const int next=right ? cz-1 : z; triangle(endX,cz,endX,next); cz=next; }
        for (int cx=endX;cx>x;) { const int next=bottom ? cx-1 : x; triangle(cx,z,next,z); cx=next; }
    }
}
} /// @note namespace fbzz::scene
