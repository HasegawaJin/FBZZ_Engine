/// @file    TerrainPatchIndices.hpp
/// @brief   共有境界を元の間隔に保ち、穴を保持して内部だけを粗くする。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <Engine/Scene/Components/Terrain/TerrainComponent.hpp>
#include <algorithm>
#include <cassert>
#include <vector>

namespace fbzz::scene {
/// @note 解像度を保った頂点格子に三角形インデックスを追加する。境界の折れ線は全 LOD で共有する。
/// @note 穴がある区画は元のセル幅を保ち、粗い隣接区画との共有辺を分割して T 字接続を防ぐ。
inline void BuildTerrainPatchLodIndices(const TerrainComponent& terrain,int x0,int z0,int x1,int z1,int step,std::vector<uint32_t>& output)
{
    assert(step>0); const int width=x1-x0,height=z1-z0,stride=width+1;
    const bool holes=terrain.holeData.size()==terrain.CellCount();
    const auto index=[&](int x,int z){return static_cast<uint32_t>(z*stride+x);};
    const auto hasHole=[&](int x,int z,int endX,int endZ) {
        if (!holes) {
            return false;
        }
        for (int cz=z;cz<endZ;++cz) for (int cx=x;cx<endX;++cx)
            if (terrain.IsHoleCell(x0+cx,z0+cz)) {
                return true;
            }
        return false;
    };
    const auto quad=[&](int x,int z,int endX,int endZ) {
        output.insert(output.end(),{index(x,z),index(x,endZ),index(endX,z),index(endX,z),index(x,endZ),index(endX,endZ)});
    };
    for (int z=0;z<height;z+=step) for (int x=0;x<width;x+=step) {
        const int endX=std::min(x+step,width),endZ=std::min(z+step,height);
        if (hasHole(x,z,endX,endZ)) {
            for (int cz=z;cz<endZ;++cz) for (int cx=x;cx<endX;++cx)
                if (!terrain.IsHoleCell(x0+cx,z0+cz)) {
                    quad(cx,cz,cx+1,cz+1);
                }
            continue;
        }
        // 幅 1 セルの帯に内部頂点を置けないため、元の四角形を残して隣の粗いセルの共有辺を分割する。
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
            // 残余区画の中心が境界に載ることがあるため、鉛直な折り返しを含め XZ 面積がゼロの三角形を除く。
            const int area=(az-centerZ)*(bx-centerX)-(ax-centerX)*(bz-centerZ);
            if (area>0) {
                output.insert(output.end(),{center,index(ax,az),index(bx,bz)});
            }
        };
        for (int cz=z;cz<endZ;) { const int next=left ? cz+1 : endZ; triangle(x,cz,x,next); cz=next; }
        for (int cx=x;cx<endX;) { const int next=top ? cx+1 : endX; triangle(cx,endZ,next,endZ); cx=next; }
        for (int cz=endZ;cz>z;) { const int next=right ? cz-1 : z; triangle(endX,cz,endX,next); cz=next; }
        for (int cx=endX;cx>x;) { const int next=bottom ? cx-1 : x; triangle(cx,z,next,z); cx=next; }
    }
}
} // namespace fbzz::scene
