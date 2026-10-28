/// @file    FiberPlacement.cpp
/// @brief   重心座標格子のブラシと、変更チャンクだけを再生成する草配置。
/// @author  Hasegawa Jin
/// @date    2026-10-11
#include <Graphics/Renderer/FiberPlacement.hpp>
#include <Graphics/Renderer/Mesh.hpp>
#include <Math/Ray.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace fbzz::renderer {
namespace {
bool Finite(const math::Vector3& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
math::Vector3 Point(const math::Matrix4& m, const math::Vector3& p)
{
    const auto v = m * math::Vector4{p.x,p.y,p.z,1};
    return {v.x,v.y,v.z};
}
math::Vector3 Vector(const math::Matrix4& m, const math::Vector3& p)
{
    const auto v = m * math::Vector4{p.x,p.y,p.z,0};
    return {v.x,v.y,v.z};
}
bool Affine(const math::Matrix4& m)
{
    for (const auto& row : m.m) for (float value : row) if (!std::isfinite(value)) return false;
    const math::Vector3 x{m.m[0][0],m.m[1][0],m.m[2][0]};
    const math::Vector3 y{m.m[0][1],m.m[1][1],m.m[2][1]};
    const math::Vector3 z{m.m[0][2],m.m[1][2],m.m[2][2]};
    return m.m[3][0] == 0 && m.m[3][1] == 0 && m.m[3][2] == 0 && m.m[3][3] == 1
        && std::abs(math::Vector3::Dot(x, math::Vector3::Cross(y,z))) > 1.0e-12f;
}
uint64_t Mix(uint64_t x)
{
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
void Hash(uint64_t& hash, uint64_t value) { hash = Mix(hash ^ Mix(value + 0x9e3779b97f4a7c15ull)); }
void HashFloat(uint64_t& hash, float value) { Hash(hash, std::bit_cast<uint32_t>(value)); }
float Random(uint64_t key, uint32_t lane)
{
    return static_cast<float>(Mix(key + static_cast<uint64_t>(lane) * 0x9e3779b97f4a7c15ull) >> 40) / 16777216.0f;
}
FiberPaintValue Value(const FiberPaintData& paint, uint64_t key)
{
    const auto it = paint.cells.find(key);
    return it == paint.cells.end() ? paint.defaults : it->second;
}
bool ValidValue(const FiberPaintValue& value)
{
    return std::isfinite(value.density) && value.density >= 0 && value.density <= 1
        && std::isfinite(value.length) && value.length >= 0 && value.length <= 1;
}
/// @note 格子は材質 UV を使わない。長辺が非常に大きい面でも最大 512 分割に限定する。
template<typename Visitor>
bool VisitTiles(const Mesh& mesh, float cellSize, Visitor&& visitor,
                const math::Matrix4* world = nullptr, const FiberPaintStroke* stroke = nullptr)
{
    uint64_t tiles = 0;
    for (size_t index = 0; index < mesh.cpuIndices.size(); index += 3) {
        const auto& a = mesh.cpuVertices[mesh.cpuIndices[index]];
        const auto& b = mesh.cpuVertices[mesh.cpuIndices[index+1]];
        const auto& c = mesh.cpuVertices[mesh.cpuIndices[index+2]];
        const auto ab = b.position-a.position, ac = c.position-a.position;
        const auto cross = math::Vector3::Cross(ab,ac);
        if (cross.LengthSq() <= 1.0e-16f) continue;
        const float edge = std::max({ab.Length(), ac.Length(), (c.position-b.position).Length()});
        const int divisions = static_cast<int>(std::clamp(std::ceil(edge/cellSize),1.0f,512.0f));
        const float inv = 1.0f/static_cast<float>(divisions);
        const auto normal = cross.Normalized();
        int firstU = 0, lastU = divisions-1, firstV = 0, lastV = divisions-1;
        if (world && stroke) {
            const auto wa = Point(*world,a.position), u = Vector(*world,ab), v = Vector(*world,ac);
            const float uu = math::Vector3::Dot(u,u), uv = math::Vector3::Dot(u,v), vv = math::Vector3::Dot(v,v);
            const float determinant = uu*vv-uv*uv;
            if (determinant <= 1.0e-16f) continue;
            const auto coordinates = [&](const math::Vector3& p) {
                const auto delta = p-wa;
                const float du = math::Vector3::Dot(delta,u), dv = math::Vector3::Dot(delta,v);
                return math::Vector2{(du*vv-dv*uv)/determinant, (dv*uu-du*uv)/determinant};
            };
            const auto p = coordinates(stroke->from), q = coordinates(stroke->to);
            const float ru = stroke->radius*std::sqrt(vv/determinant), rv = stroke->radius*std::sqrt(uu/determinant);
            const auto bound = [divisions](float value) {
                return static_cast<int>(std::clamp(std::floor(value*static_cast<float>(divisions)), -1.0f, static_cast<float>(divisions)));
            };
            firstU = std::max(0,bound(std::min(p.x,q.x)-ru)); lastU = std::min(divisions-1,bound(std::max(p.x,q.x)+ru));
            firstV = std::max(0,bound(std::min(p.y,q.y)-rv)); lastV = std::min(divisions-1,bound(std::max(p.y,q.y)+rv));
        }
        for (int u = firstU; u <= lastU; ++u) for (int v = firstV; v <= lastV && u+v < divisions; ++v) {
            if (++tiles > FIBER_MAX_PAINT_CELLS) return false;
            FiberPlacementTile tile;
            tile.triangle = static_cast<uint32_t>(index/3);
            tile.divisions = static_cast<uint32_t>(divisions);
            tile.key = (static_cast<uint64_t>(tile.triangle)<<32) | (static_cast<uint64_t>(u)<<16) | static_cast<uint64_t>(v);
            tile.origin = a.position + ab*(static_cast<float>(u)*inv) + ac*(static_cast<float>(v)*inv);
            tile.edgeU = ab*inv; tile.edgeV = ac*inv; tile.normal = normal;
            tile.uv = a.uv + (b.uv-a.uv)*(static_cast<float>(u)*inv) + (c.uv-a.uv)*(static_cast<float>(v)*inv);
            tile.uvU = (b.uv-a.uv)*inv; tile.uvV = (c.uv-a.uv)*inv;
            tile.half = u+v == divisions-1;
            if (!visitor(tile)) return false;
        }
    }
    return true;
}
math::Vector3 TileCenter(const FiberPlacementTile& tile)
{
    return tile.origin + (tile.edgeU+tile.edgeV)*(tile.half ? 1.0f/3.0f : 0.5f);
}
float SegmentDistance(const math::Vector3& p, const math::Vector3& a, const math::Vector3& b)
{
    const auto ab = b-a;
    const float length = ab.LengthSq();
    const float t = length > 1.0e-12f ? std::clamp(math::Vector3::Dot(p-a,ab)/length,0.0f,1.0f) : 0;
    return (p-(a+ab*t)).Length();
}
uint64_t OptionsSignature(const FiberPlacementOptions& options)
{
    uint64_t hash = 1;
    HashFloat(hash,options.density); HashFloat(hash,options.width); HashFloat(hash,options.maxSlope);
    Hash(hash,options.seed); Hash(hash,options.maxBlades);
    /// @note 平行移動では根元も密度も変わらない。回転と非一様スケールは傾斜・ワールド面積へ影響する。
    for (int row = 0; row < 3; ++row) for (int col = 0; col < 3; ++col) HashFloat(hash,options.world.m[row][col]);
    return hash;
}
bool GenerateChunk(FiberPlacementChunk& chunk, const FiberPaintData& paint,
                   const FiberPlacementOptions& options, uint64_t content)
{
    std::vector<FiberBladeRoot> roots;
    const auto normalMatrix = math::Matrix4::InverseTransposeAffine(options.world);
    const float slope = std::cos(options.maxSlope*0.017453292519943295f);
    for (const auto& tile : chunk.tiles) {
        const auto value = Value(paint,tile.key);
        if (value.density <= 0 || value.length <= 0) continue;
        if (Vector(normalMatrix,tile.normal).Normalized().y < slope) continue;
        const auto uWorld = Vector(options.world,tile.edgeU), vWorld = Vector(options.world,tile.edgeV);
        const double expected = static_cast<double>(math::Vector3::Cross(uWorld,vWorld).Length())
            * (tile.half ? 0.5 : 1.0) * options.density;
        if (!std::isfinite(expected) || expected > FIBER_MAX_PLACEMENT_BLADES) return false;
        const uint64_t tileSeed = Mix(tile.key ^ (static_cast<uint64_t>(options.seed)<<32));
        const uint32_t count = static_cast<uint32_t>(expected) + (Random(tileSeed,0) < expected-std::floor(expected) ? 1u : 0u);
        const auto axis = std::abs(tile.normal.y) < 0.9f ? math::Vector3{0,1,0} : math::Vector3{1,0,0};
        const auto tangent = math::Vector3::Cross(tile.normal,axis).Normalized();
        const auto bitangent = math::Vector3::Cross(tile.normal,tangent);
        for (uint32_t i = 0; i < count; ++i) {
            const uint64_t key = Mix(tileSeed ^ (static_cast<uint64_t>(i)+1)*0x9e3779b97f4a7c15ull);
            if (Random(key,1) >= value.density) continue;
            float u = Random(key,2), v = Random(key,3);
            if (tile.half && u+v > 1) { u = 1-u; v = 1-v; }
            FiberBladeRoot root;
            root.m_position = tile.origin + tile.edgeU*u + tile.edgeV*v;
            root.m_normal = tile.normal;
            root.m_height = value.length*(0.65f+0.35f*Random(key,4));
            root.m_rank = Random(key,5);
            const auto uv = tile.uv + tile.uvU*u + tile.uvV*v;
            root.m_u = uv.x; root.m_v = uv.y;
            const float angle = Random(key,6)*6.283185307179586f;
            for (uint32_t ribbon = 0; ribbon < 2; ++ribbon) {
                const float turn = angle + static_cast<float>(ribbon)*1.570796326794897f;
                const auto side = tangent*std::cos(turn)+bitangent*std::sin(turn);
                const float worldLength = Vector(options.world,side).Length();
                (ribbon == 0 ? root.m_side0 : root.m_side1) = side*(options.width*0.5f/worldLength);
            }
            roots.push_back(root);
            if (roots.size() > options.maxBlades) return false;
        }
    }
    /// @note 先頭から減らす既存 LOD が特定のセルを丸ごと消さないよう、独立した決定的順位で混ぜる。
    std::stable_sort(roots.begin(),roots.end(),[](const auto& a, const auto& b) { return a.m_rank < b.m_rank; });
    chunk.center = {}; chunk.radius = 0;
    if (!roots.empty()) {
        auto min = roots.front().m_position, max = min;
        for (const auto& root : roots) {
            min.x = std::min(min.x,root.m_position.x); min.y = std::min(min.y,root.m_position.y); min.z = std::min(min.z,root.m_position.z);
            max.x = std::max(max.x,root.m_position.x); max.y = std::max(max.y,root.m_position.y); max.z = std::max(max.z,root.m_position.z);
        }
        chunk.center = (min+max)*0.5f;
        chunk.radius = (max-min).Length()*0.5f;
    }
    chunk.roots = std::move(roots); chunk.contentSignature = content; ++chunk.revision;
    return true;
}
} /// @note namespace

uint64_t FiberMeshSignature(const Mesh& mesh)
{
    if (mesh.isSkinned || mesh.cpuVertices.empty() || mesh.cpuIndices.empty() || mesh.cpuIndices.size()%3 != 0
        || mesh.cpuIndices.size() > 600000) return 0;
    uint64_t hash = 1;
    Hash(hash,mesh.cpuVertices.size()); Hash(hash,mesh.cpuIndices.size());
    for (const auto& vertex : mesh.cpuVertices) {
        if (!Finite(vertex.position)) return 0;
        HashFloat(hash,vertex.position.x); HashFloat(hash,vertex.position.y); HashFloat(hash,vertex.position.z);
    }
    for (uint32_t index : mesh.cpuIndices) { if (index >= mesh.cpuVertices.size()) return 0; Hash(hash,index); }
    return hash == 0 ? 1 : hash;
}
bool ValidateFiberPaintData(const FiberPaintData& data)
{
    if (data.meshSignature == 0 || !std::isfinite(data.cellSize) || data.cellSize < 0.0001f || data.cellSize > 16
        || !ValidValue(data.defaults) || data.cells.size() > FIBER_MAX_PAINT_CELLS) return false;
    for (const auto& [key,value] : data.cells)
        if ((key>>32) >= 200000 || ((key>>16)&65535) >= 512 || (key&65535) >= 512 || !ValidValue(value)) return false;
    return true;
}
bool CreateFiberPaintData(const Mesh& mesh, float cellSize, float density, FiberPaintData& out)
{
    FiberPaintData result;
    result.meshSignature = FiberMeshSignature(mesh); result.cellSize = cellSize; result.defaults.density = density;
    if (!ValidateFiberPaintData(result) || !VisitTiles(mesh,cellSize,[](const auto&) { return true; })) return false;
    out = std::move(result);
    return true;
}
bool UpdateFiberPlacement(const Mesh& mesh, const FiberPaintData& paint, const FiberPlacementOptions& options, FiberPlacementCache& cache)
{
    if (!ValidateFiberPaintData(paint) || FiberMeshSignature(mesh) != paint.meshSignature || !Affine(options.world)
        || !std::isfinite(options.density) || options.density <= 0 || options.density > 2000
        || !std::isfinite(options.width) || options.width <= 0 || options.width > 0.5f
        || !std::isfinite(options.chunkSize) || options.chunkSize < 0.025f || options.chunkSize > 128
        || !std::isfinite(options.maxSlope) || options.maxSlope < 0 || options.maxSlope > 90
        || options.maxBlades == 0 || options.maxBlades > FIBER_MAX_PLACEMENT_BLADES) return false;
    uint64_t layout = paint.meshSignature;
    HashFloat(layout,paint.cellSize); HashFloat(layout,options.chunkSize);
    /// @note UV はペイント座標へ影響しないが、元の材質マスクを読む根元 UV は更新する。
    for (const auto& vertex : mesh.cpuVertices) {
        if (!std::isfinite(vertex.uv.x) || !std::isfinite(vertex.uv.y)) return false;
        HashFloat(layout,vertex.uv.x); HashFloat(layout,vertex.uv.y);
    }
    FiberPlacementCache replacement;
    const bool rebuild = cache.layoutSignature != layout;
    auto* working = rebuild ? &replacement : &cache;
    if (rebuild) {
        std::map<std::array<int,3>,size_t> lookup;
        if (!VisitTiles(mesh,paint.cellSize,[&](const auto& tile) {
            const auto center = TileCenter(tile);
            std::array<int,3> cell{};
            const float values[]{center.x,center.y,center.z};
            for (size_t axis = 0; axis < 3; ++axis) {
                const double value = std::floor(static_cast<double>(values[axis])/options.chunkSize);
                if (value < -1000000000 || value > 1000000000) return false;
                cell[axis] = static_cast<int>(value);
            }
            const auto [it,inserted] = lookup.emplace(cell,working->chunks.size());
            if (inserted) { FiberPlacementChunk chunk; chunk.cell = cell; working->chunks.push_back(std::move(chunk)); }
            working->chunks[it->second].tiles.push_back(tile);
            return true;
        })) return false;
        replacement.layoutSignature = layout;
    }
    const uint64_t settings = OptionsSignature(options);
    std::vector<std::pair<size_t,FiberPlacementChunk>> changes;
    uint64_t total = 0;
    for (size_t i = 0; i < working->chunks.size(); ++i) {
        const auto& chunk = working->chunks[i];
        uint64_t content = settings;
        for (const auto& tile : chunk.tiles) { const auto value = Value(paint,tile.key); HashFloat(content,value.density); HashFloat(content,value.length); }
        if (chunk.revision == 0 || content != chunk.contentSignature) {
            FiberPlacementChunk update;
            update.cell = chunk.cell; update.tiles = chunk.tiles; update.revision = chunk.revision;
            if (!GenerateChunk(update,paint,options,content)) return false;
            total += update.roots.size();
            changes.emplace_back(i,std::move(update));
        } else total += chunk.roots.size();
        if (total > options.maxBlades) return false;
    }
    for (auto& [index,chunk] : changes) working->chunks[index] = std::move(chunk);
    if (!changes.empty() || rebuild) ++working->revision;
    working->bladeCount = static_cast<uint32_t>(total); working->optionsSignature = settings;
    if (rebuild) cache = std::move(replacement);
    return true;
}
bool PaintFiberSurface(const Mesh& mesh, const math::Matrix4& world, const FiberPaintStroke& stroke,
                       FiberPaintData& data, uint32_t& changed,
                       const std::function<bool(const math::Vector3&,uint32_t)>& visible)
{
    changed = 0;
    if (!ValidateFiberPaintData(data) || FiberMeshSignature(mesh) != data.meshSignature || !Affine(world)
        || !Finite(stroke.from) || !Finite(stroke.to) || !std::isfinite(stroke.radius) || stroke.radius <= 0 || stroke.radius > 128
        || !std::isfinite(stroke.target) || stroke.target < 0 || stroke.target > 1
        || !std::isfinite(stroke.softness) || stroke.softness < 0 || stroke.softness > 1
        || !std::isfinite(stroke.maxSlope) || stroke.maxSlope < 0 || stroke.maxSlope > 90
        || (stroke.mode != FiberPaintMode::DENSITY && stroke.mode != FiberPaintMode::LENGTH && stroke.mode != FiberPaintMode::SMOOTH)) return false;
    const auto normalMatrix = math::Matrix4::InverseTransposeAffine(world);
    const float slope = std::cos(stroke.maxSlope*0.017453292519943295f);
    std::vector<std::pair<uint64_t,FiberPaintValue>> changes;
    if (!VisitTiles(mesh,data.cellSize,[&](const auto& tile) {
        const auto center = Point(world,TileCenter(tile));
        const float distance = SegmentDistance(center,stroke.from,stroke.to);
        if (distance >= stroke.radius || Vector(normalMatrix,tile.normal).Normalized().y < slope || (visible && !visible(center,tile.triangle))) return true;
        const float weight = stroke.softness > 0 ? std::clamp((1-distance/stroke.radius)/stroke.softness,0.0f,1.0f) : 1;
        auto value = Value(data,tile.key);
        const auto before = value;
        if (stroke.mode == FiberPaintMode::SMOOTH) {
            float density = value.density, length = value.length;
            int count = 1;
            const uint32_t u = static_cast<uint32_t>((tile.key>>16)&65535), v = static_cast<uint32_t>(tile.key&65535);
            for (const auto& offset : {std::array<int,2>{-1,0}, {1,0}, {0,-1}, {0,1}}) {
                const int nu = static_cast<int>(u)+offset[0], nv = static_cast<int>(v)+offset[1];
                if (nu < 0 || nv < 0 || nu+nv >= static_cast<int>(tile.divisions)) continue;
                const auto neighbor = Value(data,(static_cast<uint64_t>(tile.triangle)<<32)|(static_cast<uint64_t>(nu)<<16)|static_cast<uint64_t>(nv));
                density += neighbor.density; length += neighbor.length; ++count;
            }
            value.density += (density/static_cast<float>(count)-value.density)*weight;
            value.length += (length/static_cast<float>(count)-value.length)*weight;
        } else {
            auto& channel = stroke.mode == FiberPaintMode::DENSITY ? value.density : value.length;
            channel = stroke.target >= channel ? std::max(channel,stroke.target*weight)
                : std::min(channel,1-(1-stroke.target)*weight);
        }
        if (!(value == before)) changes.emplace_back(tile.key,value);
        return true;
    },&world,&stroke)) return false;
    size_t size = data.cells.size();
    for (const auto& [key,value] : changes) {
        const bool present = data.cells.contains(key);
        if (value == data.defaults && present) --size;
        if (!(value == data.defaults) && !present) ++size;
    }
    if (size > FIBER_MAX_PAINT_CELLS) return false;
    for (const auto& [key,value] : changes) {
        if (value == data.defaults) data.cells.erase(key); else data.cells[key] = value;
    }
    changed = static_cast<uint32_t>(changes.size());
    return true;
}
bool RaycastFiberSurface(const Mesh& mesh, const math::Matrix4& world, const math::Vector3& origin,
                         const math::Vector3& direction, float& distance, uint32_t& triangle)
{
    if (!Finite(origin) || !Finite(direction) || !Affine(world) || mesh.isSkinned || mesh.cpuIndices.size()%3 != 0) return false;
    bool hit = false;
    const math::Ray ray{origin,direction};
    for (size_t i = 0; i < mesh.cpuIndices.size(); i += 3) {
        const auto ia = mesh.cpuIndices[i], ib = mesh.cpuIndices[i+1], ic = mesh.cpuIndices[i+2];
        if (ia >= mesh.cpuVertices.size() || ib >= mesh.cpuVertices.size() || ic >= mesh.cpuVertices.size()) continue;
        float d = 0;
        if (ray.IntersectTriangle(Point(world,mesh.cpuVertices[ia].position),Point(world,mesh.cpuVertices[ib].position),
            Point(world,mesh.cpuVertices[ic].position),d) && d >= 0 && d < distance) {
            distance = d; triangle = static_cast<uint32_t>(i/3); hit = true;
        }
    }
    return hit;
}
} /// @note namespace fbzz::renderer
