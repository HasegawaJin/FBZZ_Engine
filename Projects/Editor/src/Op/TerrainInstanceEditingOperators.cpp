/// @file    TerrainInstanceEditingOperators.cpp
/// @brief   安定IDを保つ再接地・間引き・拡縮・個体変換と素材置換。
/// @author  Hasegawa Jin
/// @date    2026-11-05
#include "Tools/TerrainInstanceEditing.hpp"
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/AssetSearch.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/MeshInstancesAsset.hpp>
#include <Engine/Asset/TerrainPaintAsset.hpp>
#include <Engine/Scene/Components/InstancedMeshComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/TerrainSurfaceQuery.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace fbzz::editor {
namespace terrain_instance_editing {
bool Finite(const math::Vector3& p) { return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z); }
bool ValidScale(const math::Vector3& p) { return Finite(p)&&std::abs(p.x)>=0.00001f&&std::abs(p.y)>=0.00001f&&std::abs(p.z)>=0.00001f; }
math::Vector3 Euler(const math::Quaternion& rotation)
{
    const auto m=math::Matrix4::Rotate(rotation);
    const float pitch=std::asin(std::clamp(-m.m[1][2],-1.0f,1.0f));
    return std::abs(std::cos(pitch))<0.000001f ? math::Vector3{pitch,std::atan2(-m.m[2][0],m.m[0][0]),0}
        : math::Vector3{pitch,std::atan2(m.m[0][2],m.m[2][2]),std::atan2(m.m[1][0],m.m[1][1])};
}
uint32_t Hash(uint32_t x) { x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;return x^(x>>16); }
bool Matches(const asset::InstanceSetAsset& definition, const std::string& provenance, const OpData* sources)
{
    if (!sources || !sources->IsArray() || sources->AsArray().empty()) return true;
    for (const auto& source : sources->AsArray()) {
        const auto ref=source.AsString();
        if (!provenance.empty()) {
            const auto guid=asset::AssetDatabase::GuidFromRef(ref),other=asset::AssetDatabase::GuidFromRef(provenance);
            const auto path=asset::AssetManager::ResolveAssetPath(ref),expected=asset::AssetManager::ResolveAssetPath(provenance);
            if((!guid.empty()&&guid==other)||(!path.empty()&&!expected.empty()&&util::FileSystem::SamePathText(path,expected)))return true;
            continue;
        }
        const auto* candidate=asset::ResolveMeshInstanceDefinition(ref);
        if (!candidate) continue;
        if (std::all_of(definition.Parts().begin(),definition.Parts().end(),[&](const auto& p) {
            return std::any_of(candidate->Parts().begin(),candidate->Parts().end(),[&](const auto& q) {
                return p.meshPath==q.meshPath&&p.materialPath==q.materialPath&&p.lod==q.lod;
            });
        })) return true;
    }
    return false;
}
bool Vector(const OpData* value,math::Vector3& out)
{
    if(!value)return false;
    if(value->IsVec3()){out=value->AsVec3();return Finite(out);}
    if(!value->IsArray()||value->AsArray().size()!=3)return false;
    float* fields[]{&out.x,&out.y,&out.z};
    for(size_t i=0;i<3;++i){const auto& n=value->AsArray()[i];if(n.IsInt())*fields[i]=static_cast<float>(n.AsInt64());else if(!n.TryGetFloat(*fields[i]))return false;}
    return Finite(out);
}
struct LinkedGroup {
    math::Vector3 root;
    std::string source,owner;
    uint32_t id=0,rank=0;
    std::vector<const asset::InstanceSetAsset*> definitions;
    std::vector<std::string> owners;
    bool remove=false;
};
struct LinkedGroups {
    std::vector<LinkedGroup> groups;
    std::unordered_map<std::string,std::unordered_map<uint32_t,size_t>> rows;
};
LinkedGroups GroupRows(scene::Scene& scene,const scene::TerrainComponent& terrain,const scene::Transform& ground,
    const OpData* sources,const TerrainScatterBrush* brush,float keep,uint32_t seed)
{
    LinkedGroups result;
    using CellKey=std::tuple<std::string,int,int,int>;
    std::map<CellKey,std::vector<size_t>> cells;
    const auto cell=[](float v){return static_cast<int>(std::clamp(std::floor(static_cast<double>(v)/0.75),-1000000000.0,1000000000.0));};
    const auto groundInverse=math::Matrix4::Inverse(ground.GetWorldMatrix());
    for(auto entity:scene.GetEntities<scene::InstancedMeshComponent>()){
        auto* owner=scene.GetGameObject(entity);const auto* component=scene.GetComponent<scene::InstancedMeshComponent>(entity);
        if(!owner||!owner->activeInHierarchy()||!component||!component->m_enabled||!ValidScale(owner->transform.worldScale))continue;
        const auto* definition=asset::ResolveMeshInstanceDefinition(component->m_instanceSetRef.path);const auto snapshot=component->Snapshot();if(!definition||!snapshot)continue;
        const auto path=asset::AssetManager::ResolveAssetPath(component->m_instanceSetRef.path);
        const auto* stored=util::FileSystem::GetExtension(path)==".meshinstances"?asset::AssetManager::Get(asset::AssetManager::Load<asset::MeshInstancesAsset>(path)):nullptr;
        const auto provenance=stored?stored->terrainPaintRef:std::string{};
        if(!Matches(*definition,provenance,sources))continue;
        std::string source=provenance;
        if(source.empty()&&sources)for(const auto& ref:sources->AsArray()){
            auto one=OpData::MakeArray();one.Push(ref);if(Matches(*definition,"",&one)){source=ref.AsString();break;}
        }
        auto sourceGuid=asset::AssetDatabase::GuidFromRef(source);
        if(sourceGuid.empty()&&!source.empty())sourceGuid=asset::AssetDatabase::TryGetGuidFromPath(asset::AssetManager::ResolveAssetPath(source));
        source=!sourceGuid.empty()?sourceGuid:source.empty()?owner->instanceId:asset::AssetManager::ResolveAssetPath(source);
        const auto world=owner->transform.GetWorldMatrix();
        for(const auto& p:snapshot->placements){if(!p.active)continue;
            const auto w=world*math::Vector4{p.position,1},local=groundInverse*w;const math::Vector3 root{w.x,w.y,w.z};
            if(!Finite(root)||local.x<0||local.z<0||local.x>(terrain.columns-1)*terrain.cellSize||local.z>(terrain.rows-1)*terrain.cellSize
                ||(brush&&TerrainRegionCoverage(ground,*brush,root)<=0))continue;
            const int cx=cell(root.x),cy=cell(root.y),cz=cell(root.z);size_t group=result.groups.size();
            for(int z=cz-1;z<=cz+1&&group==result.groups.size();++z)for(int y=cy-1;y<=cy+1&&group==result.groups.size();++y)for(int x=cx-1;x<=cx+1&&group==result.groups.size();++x){
                const auto found=cells.find({source,x,y,z});if(found==cells.end())continue;
                for(size_t index:found->second){const auto& candidate=result.groups[index];
                    if((candidate.root-root).LengthSq()>0.5625f||std::find(candidate.owners.begin(),candidate.owners.end(),owner->instanceId)!=candidate.owners.end())continue;
                    const bool shared=std::any_of(candidate.definitions.begin(),candidate.definitions.end(),[&](const auto* other){
                        return std::any_of(definition->Parts().begin(),definition->Parts().end(),[&](const auto& a){return std::any_of(other->Parts().begin(),other->Parts().end(),[&](const auto& b){return a.meshPath==b.meshPath&&a.lod==b.lod;});});});
                    if(!shared){group=index;break;}
                }
            }
            if(group==result.groups.size()){
                LinkedGroup value;value.root=root;value.source=source;value.owner=owner->instanceId;value.id=value.rank=p.id;
                result.groups.push_back(std::move(value));cells[{source,cx,cy,cz}].push_back(group);
            }
            auto& linked=result.groups[group];linked.rank=std::min(linked.rank,p.id);linked.definitions.push_back(definition);linked.owners.push_back(owner->instanceId);
            result.rows[owner->instanceId][p.id]=group;
        }
    }
    std::vector<size_t> order(result.groups.size());for(size_t i=0;i<order.size();++i)order[i]=i;
    std::sort(order.begin(),order.end(),[&](size_t a,size_t b){const auto& x=result.groups[a];const auto& y=result.groups[b];const auto hx=Hash(x.rank^seed),hy=Hash(y.rank^seed);
        return hx==hy?std::tie(x.source,x.root.x,x.root.y,x.root.z,x.owner,x.id)<std::tie(y.source,y.root.x,y.root.y,y.root.z,y.owner,y.id):hx<hy;});
    const auto remove=static_cast<size_t>(std::floor(order.size()*(1.0-static_cast<double>(keep))+0.5));
    for(size_t i=0;i<remove;++i)result.groups[order[i]].remove=true;
    return result;
}
OpResult Edit(OpContext& context,const OpArgs& args)
{
    auto& ctx=context.ctx; auto& scene=*ctx.activeScene;
    const auto action=args.GetString("action","reground"), ownerId=args.GetString("owner");
    const bool individual=!ownerId.empty();
    auto* ground=scene.FindByGuid(args.GetString("terrain"));
    const auto* terrain=ground?ground->GetComponent<scene::TerrainComponent>():nullptr;
    if (!terrain||!terrain->enabled) return OpResult::Err("NO_TARGET","有効な地形を選んでください。");
    if (action!="reground"&&action!="thin"&&action!="scale"&&action!="transform"&&action!="replace") return OpResult::Err("BAD_ARG","編集方法が不正です。");
    const float keep=args.GetFloat("keep",0.7f), scale=args.GetFloat("scaleFactor",1),height=args.GetFloat("heightFactor",1);
    if (!std::isfinite(keep)||keep<0||keep>1||!std::isfinite(scale)||scale<0.01f||scale>100||!std::isfinite(height)||height<0.01f||height>100)
        return OpResult::Err("BAD_ARG","密度と大きさの範囲を確認してください。");
    if(individual&&args.GetInt("instanceId")<1)return OpResult::Err("BAD_ARG","有効な個体IDを指定してください。");
    if(const auto* sources=args.GetData("sources")){
        if(!sources->IsArray()||sources->AsArray().size()>TERRAIN_SCATTER_PALETTE_LIMIT)return OpResult::Err("BAD_ARG","素材の選択が不正です。");
        for(const auto& ref:sources->AsArray())if(!ref.IsString()||!asset::ResolveMeshInstanceDefinition(ref.AsString()))return OpResult::Err("NO_TEMPLATE","選択素材を解決できません。");
    }
    if (action=="transform"&&!individual) return OpResult::Err("BAD_ARG","変換する個体を選んでください。");
    std::vector<math::Vector3> centers;
    if (const auto* stroke=args.GetData("stroke")) {
        if (!stroke->IsArray()||stroke->AsArray().empty()||stroke->AsArray().size()>TERRAIN_SCATTER_STROKE_LIMIT) return OpResult::Err("BAD_ARG","範囲を確認してください。");
        for(const auto& p:stroke->AsArray()) {
            if (!p.IsVec3()&&!p.IsArray()) return OpResult::Err("BAD_ARG","範囲の座標が不正です。");
            if(p.IsVec3()) centers.push_back(p.AsVec3());
            else { if(p.AsArray().size()!=3)return OpResult::Err("BAD_ARG","XYZを指定してください。");
                math::Vector3 v; float* values[]{&v.x,&v.y,&v.z}; for(size_t i=0;i<3;++i) {
                    const auto& n=p.AsArray()[i]; if(n.IsInt())*values[i]=static_cast<float>(n.AsInt64());else if(!n.TryGetFloat(*values[i]))return OpResult::Err("BAD_ARG","範囲の座標が不正です。");
                } centers.push_back(v); }
        }
    }
    scene::FlushWorldTransforms(scene);
    const auto groundScale=ground->transform.worldScale;
    if(terrain->columns<2||terrain->rows<2||terrain->heightData.size()!=terrain->VertexCount()||!std::isfinite(terrain->cellSize)||terrain->cellSize<=0
        ||!Finite(groundScale)||std::abs(groundScale.x)<0.00001f||std::abs(groundScale.y)<0.00001f||std::abs(groundScale.z)<0.00001f)
        return OpResult::Err("INVALID_TERRAIN","地形の形状または変換が無効です。");
    TerrainScatterBrush brush{centers,args.GetFloat("brushRadius",5),false,args.GetBool("polygon"),args.GetFloat("feather",0)};
    if (!centers.empty()&&!ValidateTerrainRegion(ground->transform,brush)) return OpResult::Err("BAD_ARG","交差しない範囲を指定してください。");
    const auto replacement=args.GetString("replacement");
    const auto* replacementDefinition=action=="replace"?asset::ResolveMeshInstanceDefinition(replacement):nullptr;
    if (action=="replace"&&!replacementDefinition) return OpResult::Err("NO_TEMPLATE","置換先の素材を選んでください。");
    auto replacementReference=replacement;
    if(replacementDefinition){auto guid=asset::AssetDatabase::GuidFromRef(replacement);if(guid.empty())guid=asset::AssetDatabase::GuidFromPath(asset::AssetManager::ResolveAssetPath(replacement));
        if(guid.empty())return OpResult::Err("META_FAILED","置換先の素材を登録できません。");replacementReference="guid:"+guid;}
    math::Vector3 primaryRoot;math::Matrix4 delta=math::Matrix4::Identity();
    const asset::InstanceSetAsset* primaryDefinition=nullptr;OpData linkedSources=OpData::MakeArray();
    if(individual){
        auto* owner=scene.FindByGuid(ownerId);const auto* component=owner?owner->GetComponent<scene::InstancedMeshComponent>():nullptr;
        const auto snapshot=component?component->Snapshot():nullptr;
        if(!snapshot)return OpResult::Err("NO_TARGET","選択した配置がありません。");
        if(!ValidScale(owner->transform.worldScale))return OpResult::Err("INVALID_TRANSFORM","配置先の変換が無効です。");
        const auto found=std::find_if(snapshot->placements.begin(),snapshot->placements.end(),[&](const auto& p){return p.active&&p.id==static_cast<uint32_t>(args.GetInt("instanceId"));});
        if(found==snapshot->placements.end())return OpResult::Err("NO_TARGET","選択した個体がありません。");
        if(!ValidScale(found->scale))return OpResult::Err("INVALID_TRANSFORM","個体の大きさが無効です。");
        if(!args.GetString("expectedRevision").empty()&&args.GetString("expectedRevision")!=std::to_string(snapshot->revision))return OpResult::Err("PLACEMENT_CHANGED","配置が変更されました。選び直してください。");
        const auto primaryWorld=owner->transform.GetWorldMatrix()*math::Matrix4::TRS(found->position,math::Quaternion::FromEuler(found->eulerRadians),found->scale);
        primaryRoot={primaryWorld.m[0][3],primaryWorld.m[1][3],primaryWorld.m[2][3]};primaryDefinition=asset::ResolveMeshInstanceDefinition(component->m_instanceSetRef.path);
        if(!primaryDefinition)return OpResult::Err("NO_TEMPLATE","選択した個体の素材を解決できません。");
        auto next=*found;
        if(action=="transform"&&(!Vector(args.GetData("position"),next.position)||!Vector(args.GetData("rotation"),next.eulerRadians)||!Vector(args.GetData("scale"),next.scale)))return OpResult::Err("BAD_ARG","有限のXYZ変換を指定してください。");
        if(action=="scale"){next.scale.x*=scale;next.scale.y*=scale*height;next.scale.z*=scale;}
        if(!ValidScale(next.scale))return OpResult::Err("INVALID_TRANSFORM","有限で0以外の大きさを指定してください。");
        if(action=="transform"||action=="scale")delta=owner->transform.GetWorldMatrix()*math::Matrix4::TRS(next.position,math::Quaternion::FromEuler(next.eulerRadians),next.scale)*math::Matrix4::Inverse(primaryWorld);
        if(const auto* sources=args.GetData("sources"))for(const auto& ref:sources->AsArray()){
            auto one=OpData::MakeArray();one.Push(ref);if(primaryDefinition&&Matches(*primaryDefinition,"",&one))linkedSources.Push(ref);
        }
        AssetSearch::SetProjectRoot(ctx.projectRoot);
        for(const auto& entry:AssetSearch::Entries())if(entry.extension==".terrainpaint"){
            auto one=OpData::MakeArray();one.Push(OpData(entry.absolutePath));
            if(primaryDefinition&&Matches(*primaryDefinition,"",&one))linkedSources.Push(OpData(entry.absolutePath));
        }
    }
    TerrainInstanceEdit replaced; replaced.owner=asset::AssetDatabase::GenerateGuid(); replaced.definition=replacementReference;
    replaced.provenance=replacementReference; replaced.newName="Replaced Terrain Objects";
    std::unordered_set<uint32_t> replacementIds; uint32_t nextId=1;
    const auto linked= !individual&&(action=="thin"||action=="replace")
        ? GroupRows(scene,*terrain,ground->transform,args.GetData("sources"),centers.empty()?nullptr:&brush,keep,static_cast<uint32_t>(args.GetInt("seed",1))) : LinkedGroups{};
    std::vector<TerrainInstanceEdit> edits; size_t affected=0; bool found=false;
    for(auto id:scene.GetEntities<scene::InstancedMeshComponent>()) {
        auto* owner=scene.GetGameObject(id); const auto* component=scene.GetComponent<scene::InstancedMeshComponent>(id);
        if(!owner||!owner->activeInHierarchy()||!component||!component->m_enabled)continue;
        const auto* definition=asset::ResolveMeshInstanceDefinition(component->m_instanceSetRef.path); const auto snapshot=component->Snapshot();
        if(!definition||!snapshot)continue;
        const bool companion=individual&&owner->instanceId!=ownerId;
        if(companion){
            if(linkedSources.AsArray().empty()||!Matches(*definition,"",&linkedSources))continue;
            const bool sharedPart=std::any_of(definition->Parts().begin(),definition->Parts().end(),[&](const auto& p){return std::any_of(primaryDefinition->Parts().begin(),primaryDefinition->Parts().end(),[&](const auto& q){return p.meshPath==q.meshPath&&p.lod==q.lod;});});
            if(sharedPart)continue;
        }
        const auto ownerScale=owner->transform.worldScale;
        if(!Finite(ownerScale)||std::abs(ownerScale.x)<0.00001f||std::abs(ownerScale.y)<0.00001f||std::abs(ownerScale.z)<0.00001f)return OpResult::Err("INVALID_TRANSFORM","配置先の変換が無効です。");
        const auto path=asset::AssetManager::ResolveAssetPath(component->m_instanceSetRef.path);
        const auto* stored=util::FileSystem::GetExtension(path)==".meshinstances" ? asset::AssetManager::Get(asset::AssetManager::Load<asset::MeshInstancesAsset>(path)):nullptr;
        const std::string provenance=stored?stored->terrainPaintRef:std::string{};
        if(!individual&&!Matches(*definition,provenance,args.GetData("sources")))continue;
        if(individual&&!companion&&!args.GetString("expectedRevision").empty()&&args.GetString("expectedRevision")!=std::to_string(snapshot->revision))
            return OpResult::Err("PLACEMENT_CHANGED","選択した配置が変更されました。選び直してください。");
        const auto world=owner->transform.GetWorldMatrix(),inverse=math::Matrix4::Inverse(world);
        const auto groundInverse=math::Matrix4::Inverse(ground->transform.GetWorldMatrix());
        TerrainInstanceEdit edit; edit.owner=owner->instanceId;edit.definition=component->m_instanceSetRef.path;edit.provenance=provenance;
        edit.placements=snapshot->placements;
        std::vector<size_t> eligible;
        for(size_t i=0;i<edit.placements.size();++i) {
            const auto& p=edit.placements[i]; if(!p.active)continue;
            if(individual&&!companion&&p.id!=static_cast<uint32_t>(args.GetInt("instanceId")))continue;
            const auto w=world*math::Vector4{p.position,1}; const auto local=groundInverse*w;
            if(companion&&(math::Vector3{w.x,w.y,w.z}-primaryRoot).LengthSq()>0.5625f)continue;
            if(!individual&&(local.x<0||local.z<0||local.x>(terrain->columns-1)*terrain->cellSize||local.z>(terrain->rows-1)*terrain->cellSize))continue;
            if(!centers.empty()&&TerrainRegionCoverage(ground->transform,brush,{w.x,w.y,w.z})<=0)continue;
            eligible.push_back(i); found=true;
        }
        std::unordered_set<size_t> remove;
        if(action=="thin") {
            if(individual){if(keep<=0.5f)remove.insert(eligible.begin(),eligible.end());}
            else if(const auto rows=linked.rows.find(owner->instanceId);rows!=linked.rows.end())for(size_t slot:eligible){
                const auto group=rows->second.find(edit.placements[slot].id);if(group!=rows->second.end()&&linked.groups[group->second].remove)remove.insert(slot);
            }
        }
        size_t changed=0;
        for(size_t slot:eligible) {
            auto& p=edit.placements[slot]; const auto before=p;
            if(action=="thin") { if(remove.contains(slot)){p.active=false;++changed;} continue; }
            if(action=="replace") {
                if(!individual){const auto rows=linked.rows.find(owner->instanceId);if(rows==linked.rows.end())continue;const auto index=rows->second.find(p.id);if(index==rows->second.end())continue;
                    const auto& group=linked.groups[index->second];if(group.owner!=owner->instanceId||group.id!=p.id){p.active=false;++changed;continue;}}
                auto item=p;
                if(!DecomposeTerrainPlacement(world*math::Matrix4::TRS(p.position,math::Quaternion::FromEuler(p.eulerRadians),p.scale),item))
                    return OpResult::Err("INVALID_TRANSFORM","置換する配置に歪んだ変換があります。");
                if(!companion){
                    if(!replacementIds.insert(item.id).second){while(!replacementIds.insert(nextId).second)++nextId;item.id=nextId++;}
                    replaced.placements.push_back(item);
                }p.active=false;++changed;continue;
            }
            if(individual&&(action=="transform"||action=="scale")){
                if(!DecomposeTerrainPlacement(inverse*delta*world*math::Matrix4::TRS(p.position,math::Quaternion::FromEuler(p.eulerRadians),p.scale),p))return OpResult::Err("INVALID_TRANSFORM","歪みのない変換を指定してください。");
            }else if(action=="scale"){p.scale.x*=scale;p.scale.y*=scale*height;p.scale.z*=scale;}
            if(action=="reground") {
                const auto w=world*math::Vector4{p.position,1};scene::TerrainSurfaceSample sample;
                if(!scene::TrySampleTerrainWorld(*terrain,ground->transform,w.x,w.z,sample))continue;
                float embed=0;const asset::TerrainPaintAsset* paint=nullptr;
                if(!provenance.empty()&&util::FileSystem::GetExtension(asset::AssetManager::ResolveAssetPath(provenance))==".terrainpaint")
                    if(const auto* source=asset::AssetManager::Get(asset::AssetManager::Load<asset::TerrainPaintAsset>(provenance))){embed=source->placement.embedDepth;paint=source;}
                const auto point=inverse*math::Vector4{sample.position-sample.normal*embed,1};p.position={point.x,point.y,point.z};
                if(paint&&(paint->placement.alignToNormal||paint->placement.alignDownslope)&&ownerScale.x>0&&std::abs(ownerScale.x-ownerScale.y)<0.00001f&&std::abs(ownerScale.x-ownerScale.z)<0.00001f){
                    auto forward=owner->transform.worldRotation*(math::Quaternion::FromEuler(p.eulerRadians)*math::Vector3::FORWARD);
                    if(paint->placement.alignDownslope){const auto downhill=sample.normal*sample.normal.y-math::Vector3::UP;if(downhill.LengthSq()>0.000001f)forward=downhill;}
                    forward=forward-sample.normal*math::Vector3::Dot(forward,sample.normal);
                    if(forward.LengthSq()>0.000001f)p.eulerRadians=Euler((owner->transform.worldRotation.Inverse()*math::Quaternion::LookRotation(forward.Normalized(),sample.normal)).Normalized());
                }
            }
            if(p.position!=before.position||p.eulerRadians!=before.eulerRadians||p.scale!=before.scale)++changed;
        }
        if(changed){std::erase_if(edit.placements,[](const auto& p){return !p.active;});affected+=changed;edits.push_back(std::move(edit));}
    }
    if(individual&&!found)return OpResult::Err("NO_TARGET","選択した個体がありません。");
    if(!replaced.placements.empty())edits.push_back(std::move(replaced));
    return CommitTerrainInstanceEdits(ctx,std::move(edits),affected);
}
} /// @note namespace terrain_instance_editing

bool DecomposeTerrainPlacement(const math::Matrix4& matrix, renderer::InstancePlacement& placement)
{
    using namespace terrain_instance_editing;
    math::Vector3 axes[]{{matrix.m[0][0],matrix.m[1][0],matrix.m[2][0]}, {matrix.m[0][1],matrix.m[1][1],matrix.m[2][1]}, {matrix.m[0][2],matrix.m[1][2],matrix.m[2][2]}};
    math::Vector3 scale{axes[0].Length(),axes[1].Length(),axes[2].Length()};
    if(!Finite(scale)||scale.x<0.00001f||scale.y<0.00001f||scale.z<0.00001f)return false;
    if(math::Vector3::Dot(math::Vector3::Cross(axes[0],axes[1]),axes[2])<0)scale.x=-scale.x;
    axes[0]=axes[0]/scale.x;axes[1]=axes[1]/scale.y;axes[2]=axes[2]/scale.z;
    if(std::abs(math::Vector3::Dot(axes[0],axes[1]))>0.001f||std::abs(math::Vector3::Dot(axes[0],axes[2]))>0.001f||std::abs(math::Vector3::Dot(axes[1],axes[2]))>0.001f)return false;
    auto rotation=math::Matrix4::Identity();for(int i=0;i<3;++i){rotation.m[0][i]=axes[i].x;rotation.m[1][i]=axes[i].y;rotation.m[2][i]=axes[i].z;}
    placement.position={matrix.m[0][3],matrix.m[1][3],matrix.m[2][3]};placement.scale=scale;placement.eulerRadians=Euler(math::Quaternion::FromMatrix4(rotation));
    return Finite(placement.position)&&Finite(placement.eulerRadians);
}

void RegisterTerrainInstanceEditingOperators(OperatorRegistry& registry)
{
    EditorOperator op;op.id="terrain.edit_instances";op.label="Edit Terrain Objects";op.category="Terrain";op.kind=OpKind::Mutation;op.undoLabel="Edit Terrain Objects";
    const auto param=[](const char* name,OpParamType type,OpValue value={}){OpParam p;p.name=name;p.type=type;p.required=false;p.defaultValue=std::move(value);return p;};
    op.params={param("terrain",OpParamType::NodeId),param("owner",OpParamType::NodeId,std::string{}),param("action",OpParamType::String,std::string{"reground"}),
        param("sources",OpParamType::Data),param("stroke",OpParamType::Data),param("brushRadius",OpParamType::Float,5.0f),param("polygon",OpParamType::Bool,false),param("feather",OpParamType::Float,0.0f),
        param("keep",OpParamType::Float,0.7f),param("scaleFactor",OpParamType::Float,1.0f),param("heightFactor",OpParamType::Float,1.0f),param("seed",OpParamType::Int,1),
        param("instanceId",OpParamType::Int,0),param("expectedRevision",OpParamType::String,std::string{}),param("position",OpParamType::Data),param("rotation",OpParamType::Data),param("scale",OpParamType::Data),param("replacement",OpParamType::String,std::string{})};
    op.params[0].required=true;
    op.poll=[](const OpContext& c,const OpArgs&){return c.ctx.activeScene&&(!c.ctx.playMode||c.ctx.playMode->IsInEditor());};
    op.exec=terrain_instance_editing::Edit;registry.Register(std::move(op));
}
} /// @note namespace fbzz::editor
