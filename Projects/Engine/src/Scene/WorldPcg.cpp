/// @file    WorldPcg.cpp
/// @brief   World-anchored biome sampling, stable placement keys and atomic profile persistence.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include <Engine/Scene/WorldPcg.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Math/MathUtils.hpp>
#include <toml++/toml.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <sstream>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace fbzz::scene {
namespace world_pcg_detail {

constexpr uint64_t HASH_OFFSET = 1469598103934665603ull;
constexpr uint64_t HASH_PRIME = 1099511628211ull;
constexpr uint32_t MAX_PCG_PLACEMENTS = 1048576;
constexpr uint64_t PLACEMENT_BUDGET_STREAM = 0x68e31da496e79bb3ull;
constexpr std::array<std::pair<const char*, float WorldPcgRule::*>, 16> RULE_NUMBERS{{
    {"spacing",&WorldPcgRule::spacing},{"density",&WorldPcgRule::density},
    {"noiseScale",&WorldPcgRule::noiseScale},{"clusterStrength",&WorldPcgRule::clusterStrength},
    {"minHeight",&WorldPcgRule::minHeight},{"maxHeight",&WorldPcgRule::maxHeight},
    {"minSlope",&WorldPcgRule::minSlope},{"maxSlope",&WorldPcgRule::maxSlope},
    {"minWaterDistance",&WorldPcgRule::minWaterDistance},{"maxWaterDistance",&WorldPcgRule::maxWaterDistance},
    {"minScale",&WorldPcgRule::minScale},{"maxScale",&WorldPcgRule::maxScale},
    {"minYaw",&WorldPcgRule::minYaw},{"maxYaw",&WorldPcgRule::maxYaw},
    {"embedDepth",&WorldPcgRule::embedDepth},{"protectedDistance",&WorldPcgRule::protectedDistance}}};

bool Finite(const math::Vector3& v) { return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z); }
bool Range(float value,float low,float high) { return std::isfinite(value)&&value>=low&&value<=high; }
bool Fail(std::string& diagnostic,const std::string& message) { diagnostic=message;return false; }
void Hash(uint64_t& hash,uint64_t value) { hash=(hash^value)*HASH_PRIME; }
void Hash(uint64_t& hash,std::string_view value) { for(unsigned char c:value)Hash(hash,c);Hash(hash,0); }
void HashFloat(uint64_t& hash,float value) { Hash(hash,std::bit_cast<uint32_t>(value)); }
void HashVector(uint64_t& hash,const math::Vector3& v) { HashFloat(hash,v.x);HashFloat(hash,v.y);HashFloat(hash,v.z); }
uint64_t Mix(uint64_t value)
{
    value^=value>>30;value*=0xbf58476d1ce4e5b9ull;
    value^=value>>27;value*=0x94d049bb133111ebull;return value^(value>>31);
}
float Unit(uint64_t key,uint64_t stream) { return static_cast<float>(Mix(key+stream*0x9e3779b97f4a7c15ull)>>40)/16777216.0f; }

struct PlacementCandidate {
    const WorldPcgRule* rule;
    WorldCellId lattice;
    uint64_t key;
    math::Vector3 position;
    math::Vector3 normal;
};

struct RankedPlacement {
    uint64_t rank;
    PlacementCandidate candidate;
};

bool PlacementRankLess(const RankedPlacement& left, const RankedPlacement& right)
{
    return std::tie(left.rank, left.candidate.rule->key, left.candidate.lattice.z, left.candidate.lattice.x)
        < std::tie(right.rank, right.candidate.rule->key, right.candidate.lattice.z, right.candidate.lattice.x);
}

bool LatticeRange(const WorldPcgProfile& profile,const WorldPcgRule& rule,int32_t& x0,int32_t& x1,int32_t& z0,int32_t& z1)
{
    const double a=std::floor((static_cast<double>(profile.bounds.min.x)-rule.embedDepth)/rule.spacing);
    const double b=std::ceil((static_cast<double>(profile.bounds.max.x)+rule.embedDepth)/rule.spacing);
    const double c=std::floor((static_cast<double>(profile.bounds.min.z)-rule.embedDepth)/rule.spacing);
    const double d=std::ceil((static_cast<double>(profile.bounds.max.z)+rule.embedDepth)/rule.spacing);
    constexpr double LOW=static_cast<double>(INT32_MIN)+1.0,HIGH=static_cast<double>(INT32_MAX)-1.0;
    if(a<LOW||b>HIGH||c<LOW||d>HIGH)return false;
    x0=static_cast<int32_t>(a);x1=static_cast<int32_t>(b);z0=static_cast<int32_t>(c);z1=static_cast<int32_t>(d);return true;
}

float Noise(uint64_t seed,float x,float z,float scale)
{
    const double gx=static_cast<double>(x)/scale,gz=static_cast<double>(z)/scale;
    const auto ix=static_cast<int32_t>(std::floor(gx)),iz=static_cast<int32_t>(std::floor(gz));
    float tx=static_cast<float>(gx-ix),tz=static_cast<float>(gz-iz);
    tx=tx*tx*(3.0f-2.0f*tx);tz=tz*tz*(3.0f-2.0f*tz);
    const auto value=[&](int32_t dx,int32_t dz){return Unit(WorldCellSeed(seed,{ix+dx,iz+dz},91),7);};
    return std::lerp(std::lerp(value(0,0),value(1,0),tx),std::lerp(value(0,1),value(1,1),tx),tz);
}

math::Vector3 Rotation(float yaw,const math::Vector3& normal,bool align)
{
    const auto spin=math::Quaternion::FromAxisAngle(math::Vector3::UP,math::ToRad(yaw));
    if(!align)return {0,math::ToRad(yaw),0};
    const auto axis=math::Vector3::Cross(math::Vector3::UP,normal);
    const auto rotation=math::Quaternion{axis.x,axis.y,axis.z,1.0f+normal.y}.Normalized()*spin;
    /// @note Euler serialization uses Quaternion::FromEuler's YXZ order, preserving the same surface pose.
    const auto matrix=math::Matrix4::Rotate(rotation);
    const float pitch=std::asin(std::clamp(-matrix.m[1][2],-1.0f,1.0f));
    if(std::abs(std::cos(pitch))<=0.000001f)return {pitch,std::atan2(-matrix.m[2][0],matrix.m[0][0]),0};
    return {pitch,std::atan2(matrix.m[0][2],matrix.m[2][2]),std::atan2(matrix.m[1][0],matrix.m[1][1])};
}

struct ProtectedIndex {
    float size=0;
    std::map<std::pair<int64_t,int64_t>,std::vector<math::Vector3>> cells;
    std::pair<int64_t,int64_t> Locate(const math::Vector3& p) const
    { return {static_cast<int64_t>(std::floor(static_cast<double>(p.x)/size)),static_cast<int64_t>(std::floor(static_cast<double>(p.z)/size))}; }
    bool Near(const math::Vector3& p,float distance) const
    {
        if(distance<=0||size<=0||cells.empty())return false;
        if(std::abs(static_cast<double>(p.x)/size)>9.0e18||std::abs(static_cast<double>(p.z)/size)>9.0e18)return false;
        const auto [x,z]=Locate(p);
        for(int dx=-1;dx<=1;++dx)for(int dz=-1;dz<=1;++dz){
            const auto found=cells.find({x+dx,z+dz});if(found==cells.end())continue;
            for(const auto& item:found->second){const double a=static_cast<double>(item.x)-p.x,b=static_cast<double>(item.z)-p.z;
                if(a*a+b*b<static_cast<double>(distance)*distance)return true;}
        }
        return false;
    }
};

bool Inside(const WorldCellBounds& box,const math::Vector3& p)
{ return p.x>=box.min.x&&p.x<=box.max.x&&p.y>=box.min.y&&p.y<=box.max.y&&p.z>=box.min.z&&p.z<=box.max.z; }

uint64_t ProfileHash(const WorldPcgProfile& profile,const std::vector<const WorldPcgRule*>& rules)
{
    uint64_t hash=HASH_OFFSET;Hash(hash,profile.key);Hash(hash,profile.name);Hash(hash,profile.seed);
    HashVector(hash,profile.grid.origin);HashFloat(hash,profile.grid.cellSize);HashVector(hash,profile.bounds.min);HashVector(hash,profile.bounds.max);
    Hash(hash,profile.maxInstances);Hash(hash,profile.maxCandidates);
    if (profile.maxPlacementsPerCell) {
        Hash(hash, "maxPlacementsPerCell");
        Hash(hash, profile.maxPlacementsPerCell);
    }
    for(const auto& box:profile.exclusions){HashVector(hash,box.min);HashVector(hash,box.max);}
    for(const auto* rule:rules){Hash(hash,rule->key);Hash(hash,rule->name);Hash(hash,rule->biome);Hash(hash,rule->prototypeRef);
        for(const auto& [name,member]:RULE_NUMBERS){(void)name;HashFloat(hash,rule->*member);}Hash(hash,rule->alignToSurface?1:0);Hash(hash,rule->includeHlod?1:0);
        for(const auto& part:rule->parts){Hash(hash,part.meshPath);Hash(hash,part.materialPath);Hash(hash,part.lod);}
        for(float height:rule->lodHeights)HashFloat(hash,height);
    }
    return hash;
}

toml::array VectorData(const math::Vector3& v) { return toml::array{v.x,v.y,v.z}; }
bool ReadVector(const toml::node_view<const toml::node>& view,math::Vector3& value)
{
    const auto* array=view.as_array();if(!array||array->size()!=3)return false;
    const auto x=(*array)[0].value<float>(),y=(*array)[1].value<float>(),z=(*array)[2].value<float>();
    if(!x||!y||!z)return false;value={*x,*y,*z};return true;
}
bool ReadBounds(const toml::table& table,WorldCellBounds& box) { return ReadVector(table["min"],box.min)&&ReadVector(table["max"],box.max); }
toml::table BoundsData(const WorldCellBounds& box) { return toml::table{{"min",VectorData(box.min)},{"max",VectorData(box.max)}}; }

} /// @note namespace world_pcg_detail

bool ValidateWorldPcgProfile(const WorldPcgProfile& profile,std::string& diagnostic)
{
    using namespace world_pcg_detail;
    if(!ValidateWorldCellGrid(profile.grid,diagnostic))return false;
    if(profile.key.empty()||profile.key.size()>256||profile.name.size()>256||profile.rules.empty()||profile.rules.size()>256)
        return Fail(diagnostic,"PCG requires a key and 1..256 uniquely named rules");
    if(!Finite(profile.bounds.min)||!Finite(profile.bounds.max)||profile.bounds.min.x>=profile.bounds.max.x||profile.bounds.min.z>=profile.bounds.max.z)
        return Fail(diagnostic,"PCG requires finite, increasing half-open XZ bounds");
    if(profile.maxInstances==0||profile.maxInstances>MAX_PCG_PLACEMENTS||profile.maxCandidates==0||profile.maxCandidates>10000000||profile.exclusions.size()>4096)
        return Fail(diagnostic,"PCG candidate, instance or exclusion budget is out of range");
    if (profile.maxPlacementsPerCell > MAX_PCG_PLACEMENTS)
        return Fail(diagnostic, "PCG maxPlacementsPerCell exceeds 1048576");
    WorldCellId cell;
    if(!TryLocateWorldCell(profile.grid,profile.bounds.min,cell)||!TryLocateWorldCell(profile.grid,profile.bounds.max,cell))
        return Fail(diagnostic,"PCG bounds exceed the world cell coordinate range");
    const WorldCellGrid budgetGrid{profile.grid.origin, VEGETATION_BUDGET_CELL_SIZE};
    if (profile.maxPlacementsPerCell && (!TryLocateWorldCell(budgetGrid, profile.bounds.min, cell)
        || !TryLocateWorldCell(budgetGrid, profile.bounds.max, cell)))
        return Fail(diagnostic, "PCG bounds exceed the 128 m placement cell coordinate range");
    for(const auto& box:profile.exclusions)if(!Finite(box.min)||!Finite(box.max)||box.min.x>box.max.x||box.min.y>box.max.y||box.min.z>box.max.z)
        return Fail(diagnostic,"PCG exclusion bounds are invalid");
    std::unordered_set<std::string> keys;
    for(const auto& rule:profile.rules){
        if(rule.key.empty()||rule.key.size()>256||rule.name.size()>256||rule.biome.size()>256||rule.prototypeRef.size()>4096||!keys.insert(rule.key).second)
            return Fail(diagnostic,"PCG rule keys must be nonempty and unique");
        if(!Range(rule.spacing,0.25f,100000.0f)||!Range(rule.density,0,1)||!Range(rule.noiseScale,0.25f,1000000.0f)||!Range(rule.clusterStrength,0,1)
            ||!Range(rule.minHeight,-10000000.0f,10000000.0f)||!Range(rule.maxHeight,rule.minHeight,10000000.0f)
            ||!Range(rule.minSlope,0,90)||!Range(rule.maxSlope,rule.minSlope,90)
            ||!Range(rule.minWaterDistance,0,1000000)||!Range(rule.maxWaterDistance,rule.minWaterDistance,1000000)
            ||!Range(rule.minScale,0.001f,1000)||!Range(rule.maxScale,rule.minScale,1000)
            ||!Range(rule.minYaw,-360,360)||!Range(rule.maxYaw,rule.minYaw,360)
            ||!Range(rule.embedDepth,0,1000)||!Range(rule.protectedDistance,0,100000))return Fail(diagnostic,"PCG rule range is invalid: "+rule.key);
        if((rule.parts.empty()&&rule.prototypeRef.empty())||rule.parts.size()>64||rule.lodHeights.empty()||rule.lodHeights.size()>4)
            return Fail(diagnostic,"PCG mesh/LOD definition is invalid: "+rule.key);
        std::array<bool,4> levels{};float previous=2;
        for(float height:rule.lodHeights){if(!Range(height,0,1)||height>=previous)return Fail(diagnostic,"PCG LOD heights must strictly descend");previous=height;}
        for(const auto& part:rule.parts){if(part.meshPath.empty()||part.materialPath.empty()||part.meshPath.size()>4096||part.materialPath.size()>4096||part.lod>=rule.lodHeights.size())
                return Fail(diagnostic,"PCG mesh parts require valid material and LOD references");levels[part.lod]=true;}
        if(!rule.parts.empty())for(size_t index=0;index<rule.lodHeights.size();++index)if(!levels[index])return Fail(diagnostic,"Every PCG LOD requires a mesh part");
        int32_t x0,x1,z0,z1;if(!LatticeRange(profile,rule,x0,x1,z0,z1))return Fail(diagnostic,"PCG bounds exceed the global lattice coordinate range");
        const double noiseBound=std::max({std::abs(static_cast<double>(profile.bounds.min.x)),std::abs(static_cast<double>(profile.bounds.max.x)),
            std::abs(static_cast<double>(profile.bounds.min.z)),std::abs(static_cast<double>(profile.bounds.max.z))})/rule.noiseScale;
        if(noiseBound>static_cast<double>(INT32_MAX)-2)return Fail(diagnostic,"PCG noise coordinates exceed the supported range");
    }
    diagnostic.clear();return true;
}

bool GenerateWorldPcg(const WorldPcgProfile& profile,const WorldPcgSampler& sample,std::span<const math::Vector3> protectedPositions,
    WorldPcgResult& out,std::string& diagnostic)
{
    using namespace world_pcg_detail;
    if(!ValidateWorldPcgProfile(profile,diagnostic))return false;
    if(!sample)return Fail(diagnostic,"PCG requires a surface sampler");
    for(const auto& rule:profile.rules)if(rule.parts.empty())return Fail(diagnostic,"Resolve PCG prototype definitions before generation");
    if(protectedPositions.size()>1048576)return Fail(diagnostic,"PCG protected-placement budget exceeded");
    ProtectedIndex protectedIndex;
    std::vector<const WorldPcgRule*> rules;for(const auto& rule:profile.rules){rules.push_back(&rule);protectedIndex.size=std::max(protectedIndex.size,rule.protectedDistance);}
    std::sort(rules.begin(),rules.end(),[](const auto* a,const auto* b){return a->key<b->key;});
    for(const auto& position:protectedPositions){if(!Finite(position))return Fail(diagnostic,"PCG protected positions must be finite");
        if(protectedIndex.size>0){const auto x=static_cast<double>(position.x)/protectedIndex.size,z=static_cast<double>(position.z)/protectedIndex.size;
            if(std::abs(x)>9.0e18||std::abs(z)>9.0e18)return Fail(diagnostic,"PCG protected-position index exceeds its range");
            protectedIndex.cells[protectedIndex.Locate(position)].push_back(position);}}
    uint64_t estimated=0;
    for(const auto* rule:rules){int32_t x0,x1,z0,z1;(void)LatticeRange(profile,*rule,x0,x1,z0,z1);
        const auto nx=static_cast<uint64_t>(static_cast<int64_t>(x1)-x0),nz=static_cast<uint64_t>(static_cast<int64_t>(z1)-z0);
        if(nz&&nx>(profile.maxCandidates-estimated)/nz)return Fail(diagnostic,"PCG candidate budget exceeded before sampling");estimated+=nx*nz;}
    WorldPcgResult staged;
    using GroupKey=std::pair<WorldCellId,std::string>;
    std::map<GroupKey,WorldPcgCellGroup> groups;
    std::map<GroupKey,std::unordered_set<uint32_t>> ids;
    const auto emit = [&](const PlacementCandidate& candidate) {
        if (staged.stats.accepted >= profile.maxInstances)
            return Fail(diagnostic, "PCG instance budget exceeded; no partial result was published");
        WorldCellId cell;
        if (!TryLocateWorldCell(profile.grid, candidate.position, cell))
            return Fail(diagnostic, "PCG candidate has no valid world cell");
        const auto& rule = *candidate.rule;
        const GroupKey key{cell, rule.key};
        auto& group = groups[key];
        if (group.ruleKey.empty()) {
            group.cell = cell;
            group.ruleKey = rule.key;
            group.prototypeRef = rule.prototypeRef;
            group.includeHlod = rule.includeHlod;
            group.parts = rule.parts;
            group.lodHeights = rule.lodHeights;
        }
        /// @note Coordinate bits avoid birthday collisions in dense cells; wrapping or the remote zero alias is explicitly rejected.
        const uint32_t packed = (static_cast<uint32_t>(candidate.lattice.x) & 0xffffu)
            | ((static_cast<uint32_t>(candidate.lattice.z) & 0x7fffu) << 16);
        const uint32_t id = packed == 0 ? 0x40008000u : packed;
        if (!ids[key].insert(id).second)
            return Fail(diagnostic, "PCG stable ID range collided; reduce the owning cell size");
        renderer::InstancePlacement placement;
        placement.id = id;
        placement.position = candidate.position - WorldCellOrigin(profile.grid, cell);
        placement.eulerRadians = Rotation(std::lerp(rule.minYaw, rule.maxYaw, Unit(candidate.key, 4)),
            candidate.normal, rule.alignToSurface);
        const float scale = std::lerp(rule.minScale, rule.maxScale, Unit(candidate.key, 5));
        placement.scale = {scale, scale, scale};
        if (!Finite(placement.position) || !Finite(placement.eulerRadians))
            return Fail(diagnostic, "PCG generated a nonfinite transform");
        group.placements.push_back(placement);
        group.candidateKeys.push_back(candidate.key);
        ++staged.stats.accepted;
        return true;
    };
    const WorldCellGrid budgetGrid{profile.grid.origin, VEGETATION_BUDGET_CELL_SIZE};
    std::map<WorldCellId, std::vector<RankedPlacement>> placementCells;
    uint64_t retainedPlacements = 0;
    for(const auto* rule:rules){
        uint64_t ruleSeed=HASH_OFFSET;Hash(ruleSeed,profile.key);Hash(ruleSeed,rule->key);Hash(ruleSeed,profile.seed);HashFloat(ruleSeed,rule->spacing);
        int32_t x0,x1,z0,z1;(void)LatticeRange(profile,*rule,x0,x1,z0,z1);
        for(int64_t iz=z0;iz<z1;++iz)for(int64_t ix=x0;ix<x1;++ix){
            const uint64_t candidate=WorldCellSeed(ruleSeed,{static_cast<int32_t>(ix),static_cast<int32_t>(iz)},17);
            const float x=static_cast<float>((static_cast<double>(ix)+0.15+0.7*Unit(candidate,1))*rule->spacing);
            const float z=static_cast<float>((static_cast<double>(iz)+0.15+0.7*Unit(candidate,2))*rule->spacing);
            WorldPcgSurfaceSample surface;
            if(!sample(x,z,surface)||!Finite(surface.position)||!Finite(surface.normal)||!std::isfinite(surface.normal.LengthSq())||surface.normal.LengthSq()<0.000001f
                ||!Range(surface.biomeDensity,0,1)||!Range(surface.waterDistance,0,1000000)) {
                if(x>=profile.bounds.min.x&&x<profile.bounds.max.x&&z>=profile.bounds.min.z&&z<profile.bounds.max.z){++staged.stats.candidates;++staged.stats.noSurface;}continue;}
            const auto normal=surface.normal.Normalized();math::Vector3 position{x,surface.position.y,z};
            const auto authoredPosition=position-normal*rule->embedDepth;
            if(authoredPosition.x<profile.bounds.min.x||authoredPosition.x>=profile.bounds.max.x||authoredPosition.z<profile.bounds.min.z||authoredPosition.z>=profile.bounds.max.z)continue;
            ++staged.stats.candidates;
            if(!rule->biome.empty()&&surface.biome!=rule->biome){++staged.stats.biomeRejected;continue;}
            if(surface.excluded||std::any_of(profile.exclusions.begin(),profile.exclusions.end(),[&](const auto& box){return Inside(box,position);}))
                {++staged.stats.exclusionRejected;continue;}
            if(surface.water||surface.waterDistance<rule->minWaterDistance||surface.waterDistance>rule->maxWaterDistance)
                {++staged.stats.waterRejected;continue;}
            if(position.y<rule->minHeight||position.y>rule->maxHeight){++staged.stats.heightRejected;continue;}
            const float slope=math::ToDeg(std::acos(std::clamp(normal.y,-1.0f,1.0f)));
            if(slope<rule->minSlope||slope>rule->maxSlope){++staged.stats.slopeRejected;continue;}
            const float chance=rule->density*surface.biomeDensity*std::lerp(1.0f,Noise(ruleSeed,x,z,rule->noiseScale),rule->clusterStrength);
            if(Unit(candidate,3)>=chance){++staged.stats.densityRejected;continue;}
            if(protectedIndex.Near(position,rule->protectedDistance)){++staged.stats.protectedRejected;continue;}
            const PlacementCandidate placement{rule, {static_cast<int32_t>(ix), static_cast<int32_t>(iz)},
                candidate, authoredPosition, normal};
            if (!profile.maxPlacementsPerCell) {
                if (!emit(placement)) return false;
                continue;
            }
            WorldCellId budgetCell;
            if (!TryLocateWorldCell(budgetGrid, authoredPosition, budgetCell))
                return Fail(diagnostic, "PCG candidate has no valid placement budget cell");
            auto& ranked = placementCells[budgetCell];
            const RankedPlacement next{Mix(candidate ^ PLACEMENT_BUDGET_STREAM), placement};
            if (ranked.size() < profile.maxPlacementsPerCell) {
                if (retainedPlacements >= profile.maxInstances)
                    return Fail(diagnostic, "PCG instance budget exceeded; no partial result was published");
                ranked.push_back(next);
                std::push_heap(ranked.begin(), ranked.end(), PlacementRankLess);
                ++retainedPlacements;
            } else {
                ++staged.stats.cellBudgetRejected;
                if (PlacementRankLess(next, ranked.front())) {
                    std::pop_heap(ranked.begin(), ranked.end(), PlacementRankLess);
                    ranked.back() = next;
                    std::push_heap(ranked.begin(), ranked.end(), PlacementRankLess);
                }
            }
        }
    }
    while (!placementCells.empty()) {
        auto ranked = std::move(placementCells.begin()->second);
        placementCells.erase(placementCells.begin());
        /// @note Emit in rank order so replacement history cannot change the published asset fingerprint.
        std::sort_heap(ranked.begin(), ranked.end(), PlacementRankLess);
        for (const auto& placement : ranked)
            if (!emit(placement.candidate)) return false;
    }
    uint64_t fingerprint=ProfileHash(profile,rules);
    for(auto& [key,group]:groups){(void)key;
        Hash(fingerprint,static_cast<uint32_t>(group.cell.x));Hash(fingerprint,static_cast<uint32_t>(group.cell.z));Hash(fingerprint,group.ruleKey);
        for(size_t i=0;i<group.placements.size();++i){const auto& placement=group.placements[i];Hash(fingerprint,group.candidateKeys[i]);Hash(fingerprint,placement.id);
            HashVector(fingerprint,placement.position);HashVector(fingerprint,placement.eulerRadians);HashVector(fingerprint,placement.scale);}
        staged.groups.push_back(std::move(group));
    }
    Hash(fingerprint,staged.stats.candidates);Hash(fingerprint,staged.stats.noSurface);Hash(fingerprint,staged.stats.biomeRejected);
    Hash(fingerprint,staged.stats.densityRejected);Hash(fingerprint,staged.stats.heightRejected);Hash(fingerprint,staged.stats.slopeRejected);
    Hash(fingerprint,staged.stats.waterRejected);Hash(fingerprint,staged.stats.exclusionRejected);Hash(fingerprint,staged.stats.protectedRejected);
    if (profile.maxPlacementsPerCell) Hash(fingerprint, staged.stats.cellBudgetRejected);
    std::ostringstream text;text<<std::hex<<fingerprint;staged.fingerprint=text.str();out=std::move(staged);diagnostic.clear();return true;
}

bool SerializeWorldPcgProfile(const WorldPcgProfile& profile,std::string& outText,std::string& diagnostic)
{
    using namespace world_pcg_detail;
    if(!ValidateWorldPcgProfile(profile,diagnostic))return false;
    toml::table document{{"format","fbzz.pcg"},{"version",1},{"key",profile.key},{"name",profile.name},{"seed",std::to_string(profile.seed)},
        {"maxInstances",profile.maxInstances},{"maxCandidates",static_cast<int64_t>(profile.maxCandidates)},
        {"grid",toml::table{{"origin",VectorData(profile.grid.origin)},{"cellSize",profile.grid.cellSize}}},{"bounds",BoundsData(profile.bounds)}};
    if (profile.maxPlacementsPerCell) document.insert("maxPlacementsPerCell", profile.maxPlacementsPerCell);
    toml::array rules,exclusions;
    for(const auto& rule:profile.rules){
        toml::table row{{"key",rule.key},{"name",rule.name},{"biome",rule.biome},{"prototypeRef",rule.prototypeRef},{"alignToSurface",rule.alignToSurface},{"includeHlod",rule.includeHlod}};
        for(const auto& [name,member]:RULE_NUMBERS)row.insert(name,rule.*member);
        toml::array parts,lods;
        for(const auto& part:rule.parts)parts.push_back(toml::table{{"mesh",part.meshPath},{"material",part.materialPath},{"lod",part.lod}});
        for(float height:rule.lodHeights)lods.push_back(height);
        row.insert("parts",std::move(parts));row.insert("lodHeights",std::move(lods));rules.push_back(std::move(row));
    }
    for(const auto& box:profile.exclusions)exclusions.push_back(BoundsData(box));
    document.insert("rules",std::move(rules));document.insert("exclusions",std::move(exclusions));
    std::ostringstream text;text<<document;auto encoded=text.str();
    if(encoded.size()>8*1024*1024)return Fail(diagnostic,"PCG profile exceeds 8 MiB");
    outText=std::move(encoded);diagnostic.clear();return true;
}

bool DeserializeWorldPcgProfile(std::string_view text,WorldPcgProfile& out,std::string& diagnostic)
{
    using namespace world_pcg_detail;
    if(text.size()>8*1024*1024)return Fail(diagnostic,"PCG profile exceeds 8 MiB");
    const auto parsed=toml::parse(text,std::string_view{"WorldPcg.pcg"});
    if(!parsed||parsed["format"].value<std::string>()!="fbzz.pcg"||parsed["version"].value<int64_t>()!=1)
        return Fail(diagnostic,"Unsupported or malformed PCG profile");
    WorldPcgProfile staged;
    const auto key=parsed["key"].value<std::string>(),name=parsed["name"].value<std::string>(),seed=parsed["seed"].value<std::string>();
    const auto maxInstances=parsed["maxInstances"].value<int64_t>(),maxCandidates=parsed["maxCandidates"].value<int64_t>();
    const auto* grid=parsed["grid"].as_table();const auto* bounds=parsed["bounds"].as_table();const auto* rules=parsed["rules"].as_array();
    if(!key||!name||!seed||!maxInstances||!maxCandidates||!grid||!bounds||!rules||rules->empty()||rules->size()>256
        ||*maxInstances<1||*maxInstances>MAX_PCG_PLACEMENTS||*maxCandidates<1||*maxCandidates>10000000)return Fail(diagnostic,"PCG profile fields are incomplete");
    if (const auto cellLimit = parsed["maxPlacementsPerCell"]; cellLimit) {
        const auto value = cellLimit.value_exact<int64_t>();
        if (!value || *value < 0 || *value > MAX_PCG_PLACEMENTS)
            return Fail(diagnostic, "PCG maxPlacementsPerCell must be an integer in [0,1048576]");
        staged.maxPlacementsPerCell = static_cast<uint32_t>(*value);
    }
    const auto conversion=std::from_chars(seed->data(),seed->data()+seed->size(),staged.seed);
    if(conversion.ec!=std::errc{}||conversion.ptr!=seed->data()+seed->size())return Fail(diagnostic,"PCG seed is not an unsigned 64-bit decimal value");
    staged.key=*key;staged.name=*name;staged.maxInstances=static_cast<uint32_t>(*maxInstances);staged.maxCandidates=static_cast<uint64_t>(*maxCandidates);
    const auto cellSize=(*grid)["cellSize"].value<float>();
    if(!cellSize||!ReadVector((*grid)["origin"],staged.grid.origin)||!ReadBounds(*bounds,staged.bounds))return Fail(diagnostic,"PCG grid or bounds are incomplete");
    staged.grid.cellSize=*cellSize;
    for(const auto& node:*rules){
        const auto* row=node.as_table();if(!row)return Fail(diagnostic,"PCG rule must be a table");WorldPcgRule rule;
        const auto ruleKey=(*row)["key"].value<std::string>(),ruleName=(*row)["name"].value<std::string>(),biome=(*row)["biome"].value<std::string>(),prototype=(*row)["prototypeRef"].value<std::string>();
        const auto align=(*row)["alignToSurface"].value<bool>();const auto* parts=(*row)["parts"].as_array();const auto* lods=(*row)["lodHeights"].as_array();
        if(!ruleKey||!ruleName||!biome||!prototype||!align||!parts||parts->size()>64||!lods||lods->size()>4)return Fail(diagnostic,"PCG rule fields are incomplete");
        rule.key=*ruleKey;rule.name=*ruleName;rule.biome=*biome;rule.prototypeRef=*prototype;rule.alignToSurface=*align;rule.includeHlod=(*row)["includeHlod"].value_or(true);
        for(const auto& [field,member]:RULE_NUMBERS){const auto value=(*row)[field].value<float>();if(!value)return Fail(diagnostic,"Missing PCG rule number: "+std::string(field));rule.*member=*value;}
        for(const auto& partNode:*parts){const auto* part=partNode.as_table();if(!part)return Fail(diagnostic,"PCG mesh part must be a table");
            const auto mesh=(*part)["mesh"].value<std::string>(),material=(*part)["material"].value<std::string>();const auto lod=(*part)["lod"].value<int64_t>();
            if(!mesh||!material||!lod||*lod<0||*lod>3)return Fail(diagnostic,"PCG mesh part fields are incomplete");rule.parts.push_back({*mesh,*material,static_cast<uint32_t>(*lod)});}
        rule.lodHeights.clear();for(const auto& lod:*lods){const auto value=lod.value<float>();if(!value)return Fail(diagnostic,"PCG LOD height is not numeric");rule.lodHeights.push_back(*value);}
        staged.rules.push_back(std::move(rule));
    }
    if(const auto* exclusions=parsed["exclusions"].as_array()){
        if(exclusions->size()>4096)return Fail(diagnostic,"PCG exclusion budget exceeded");
        for(const auto& node:*exclusions){const auto* row=node.as_table();WorldCellBounds box;if(!row||!ReadBounds(*row,box))return Fail(diagnostic,"Invalid PCG exclusion bounds");staged.exclusions.push_back(box);}
    }else return Fail(diagnostic,"PCG exclusions array is missing");
    if(!ValidateWorldPcgProfile(staged,diagnostic))return false;
    out=std::move(staged);diagnostic.clear();return true;
}

bool SaveWorldPcgProfile(std::string_view path,const WorldPcgProfile& profile,std::string& diagnostic)
{
    std::string text;if(!SerializeWorldPcgProfile(profile,text,diagnostic))return false;
    if(path.empty())return world_pcg_detail::Fail(diagnostic,"PCG profile path is empty");
    const std::string filename(path);
    if(!util::FileSystem::EnsureParentDirectory(util::FileSystem::PathFromUtf8(filename))||!util::FileSystem::WriteTextAtomic(filename,text))
        return world_pcg_detail::Fail(diagnostic,"PCG profile atomic write failed: "+filename);
    diagnostic.clear();return true;
}

bool LoadWorldPcgProfile(std::string_view path,WorldPcgProfile& out,std::string& diagnostic)
{
    std::string text;std::error_code error;
    const auto size=std::filesystem::file_size(util::FileSystem::PathFromUtf8(std::string(path)),error);
    if(error||size>8*1024*1024||!util::FileSystem::ReadText(std::string(path),text))
        return world_pcg_detail::Fail(diagnostic,"PCG profile read failed or exceeds 8 MiB");
    return DeserializeWorldPcgProfile(text,out,diagnostic);
}

} /// @note namespace fbzz::scene
