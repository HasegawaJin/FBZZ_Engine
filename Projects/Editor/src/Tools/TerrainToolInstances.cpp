/// @file    TerrainToolInstances.cpp
/// @brief   個体の選択・ギズモ・範囲操作と配置診断の操作画面。
/// @author  Hasegawa Jin
/// @date    2026-11-05
#include "TerrainTool.hpp"
#include "TerrainInstanceEditing.hpp"
#include <Editor/EditorContext.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/MeshInstancesAsset.hpp>
#include <Engine/Scene/Components/InstancedMeshComponent.hpp>
#include <Engine/Scene/MeshResolver.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Math/MathUtils.hpp>
#include <ImGuizmo.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace fbzz::editor {
namespace terrain_tool_instances {
ImVec2 Project(const renderer::Camera& camera,const ImVec2& origin,const ImVec2& size,const math::Vector3& point)
{
    const auto clip=camera.GetViewProjection()*math::Vector4{point,1};
    if(clip.w<=0.00001f)return {-100000,-100000};
    return {origin.x+(clip.x/clip.w+1)*0.5f*size.x,origin.y+(1-clip.y/clip.w)*0.5f*size.y};
}
ImU32 Color(TerrainPaintReason reason)
{
    switch(reason){
    case TerrainPaintReason::WATER:return IM_COL32(99,169,223,210);
    case TerrainPaintReason::PATH:return IM_COL32(223,185,101,210);
    case TerrainPaintReason::SLOPE:return IM_COL32(218,125,109,210);
    case TerrainPaintReason::SPACING:case TerrainPaintReason::OCCUPIED:return IM_COL32(190,151,213,210);
    default:return IM_COL32(179,185,189,180);
    }
}
bool RayBox(const math::Vector3& origin,const math::Vector3& direction,const math::Vector3& center,const math::Vector3& extents,float& near)
{
    const float p[]{origin.x-center.x,origin.y-center.y,origin.z-center.z},d[]{direction.x,direction.y,direction.z},e[]{extents.x,extents.y,extents.z};
    float low=0,high=near;
    for(int i=0;i<3;++i){if(std::abs(d[i])<0.0000001f){if(std::abs(p[i])>e[i])return false;continue;}
        float a=(-e[i]-p[i])/d[i],b=(e[i]-p[i])/d[i];if(a>b)std::swap(a,b);low=std::max(low,a);high=std::min(high,b);if(low>high)return false;}
    near=low;return true;
}
bool ValidScale(const math::Vector3& p)
{
    return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&std::abs(p.x)>=0.00001f&&std::abs(p.y)>=0.00001f&&std::abs(p.z)>=0.00001f;
}
} /// @note namespace terrain_tool_instances

void TerrainTool::RefreshInstanceSelection(EditorContext& context)
{
    auto* owner=context.activeScene?context.activeScene->FindByGuid(m_instanceOwner):nullptr;
    const auto* component=owner?owner->GetComponent<scene::InstancedMeshComponent>():nullptr;
    const auto snapshot=component?component->Snapshot():nullptr;
    if(!owner||!owner->activeInHierarchy()||!component||!component->m_enabled||!snapshot||!terrain_tool_instances::ValidScale(owner->transform.worldScale)
        ||context.activeScene->GetComponentLifetime<scene::InstancedMeshComponent>(owner->GetID())!=m_instanceLifetime){m_instanceOwner.clear();m_instanceId=0;m_paintPreviewInputs.clear();return;}
    if(snapshot->revision==m_instanceRevision)return;
    const auto found=std::find_if(snapshot->placements.begin(),snapshot->placements.end(),[&](const auto& p){return p.id==m_instanceId&&p.active;});
    if(found==snapshot->placements.end()){m_instanceOwner.clear();m_instanceId=0;m_paintPreviewInputs.clear();return;}
    if(m_instanceDragging){m_instanceDragging=false;m_scatterStatus="Placement changed; the pending transform was cancelled.";}
    m_instanceDraft=m_instanceOriginal=*found;m_instanceRevision=snapshot->revision;
}

bool TerrainTool::SelectPaintInstance(EditorContext& context,const std::string& ownerId,uint32_t id)
{
    auto* owner=context.activeScene?context.activeScene->FindByGuid(ownerId):nullptr;
    const auto* component=owner?owner->GetComponent<scene::InstancedMeshComponent>():nullptr;
    const auto snapshot=component?component->Snapshot():nullptr;
    if(!snapshot||!std::any_of(snapshot->placements.begin(),snapshot->placements.end(),[&](const auto& p){return p.active&&p.id==id;}))return false;
    const auto generation=context.activeScene->GetRenderSceneGeneration();
    if(m_strokeSceneGeneration!=generation){
        m_strokeSceneGeneration=generation;m_strokeBeforeTerrains.clear();m_scatterStroke.clear();m_paintArea.clear();m_paintPreviewTerrainId.clear();m_hitTerrain=nullptr;
        m_strokeActive=m_rampDragging=m_flattenLocked=false;
    }
    m_instanceOwner=ownerId;m_instanceId=id;m_instanceRevision=0;m_instanceDragging=false;
    m_instanceLifetime=context.activeScene->GetComponentLifetime<scene::InstancedMeshComponent>(owner->GetID());
    m_active=true;m_mode=Mode::Paint;m_paintTarget=PaintTarget::Instances;m_instanceMode=1;m_editScope=3;
    RefreshInstanceSelection(context);PublishInstancePreview(context);return true;
}

void TerrainTool::PublishInstancePreview(EditorContext& context)
{
    m_paintPreviewInputs.clear();
    auto* owner=context.activeScene?context.activeScene->FindByGuid(m_instanceOwner):nullptr;
    const auto* component=owner?owner->GetComponent<scene::InstancedMeshComponent>():nullptr;
    if(!component||!m_instanceId)return;
    const auto finite=[](const math::Vector3& p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);};
    if(!finite(m_instanceDraft.position)||!finite(m_instanceDraft.eulerRadians)||!finite(m_instanceDraft.scale)
        ||std::abs(m_instanceDraft.scale.x)<0.00001f||std::abs(m_instanceDraft.scale.y)<0.00001f||std::abs(m_instanceDraft.scale.z)<0.00001f)return;
    const bool changed=m_instanceDraft.position!=m_instanceOriginal.position||m_instanceDraft.eulerRadians!=m_instanceOriginal.eulerRadians||m_instanceDraft.scale!=m_instanceOriginal.scale;
    auto data=std::make_shared<renderer::InstanceSetData>();data->placements.push_back(m_instanceDraft);
    m_paintPreviewInputs.push_back({component->m_instanceSetRef.path,data,owner->transform.GetWorldMatrix(),0x7fffffffffffffffull,!changed,true});
    m_paintPreviewGeneration=context.activeScene->GetRenderSceneGeneration();
}

void TerrainTool::ApplyInstanceAdjustment(EditorContext& context,const std::string& action)
{
    if(m_heightMapTargetId.empty()){m_scatterStatus="Choose a terrain first.";return;}
    OpArgs args;args.Set("terrain",m_heightMapTargetId);args.Set("action",action);
    auto sources=OpData::MakeArray();for(const auto& source:m_paintPalette)sources.Push(OpData(source.reference));args.Set("sources",std::move(sources));
    const bool single=m_editScope==3||action=="transform";
    if(single){
        if(m_instanceOwner.empty()||!m_instanceId){m_scatterStatus="Select an object in the viewport first.";return;}
        args.Set("owner",m_instanceOwner);args.Set("instanceId",static_cast<int>(m_instanceId));args.Set("expectedRevision",std::to_string(m_instanceRevision));
    }else if(m_editScope!=2){
        const auto points=m_editScope==1?std::span<const math::Vector3>(m_paintArea):std::span<const math::Vector3>(&m_paintPreviewCenter,1);
        if((m_editScope==1&&points.size()<3)||(m_editScope==0&&m_paintPreviewTerrainId!=m_heightMapTargetId)){m_scatterStatus="Mark an area or point the brush at the terrain first.";return;}
        auto stroke=OpData::MakeArray();for(const auto& p:points)stroke.Push(OpData(p));args.Set("stroke",std::move(stroke));args.Set("brushRadius",m_brush.radius);args.Set("polygon",m_editScope==1);
    }
    args.Set("keep",m_editKeep);args.Set("scaleFactor",m_editScale);args.Set("heightFactor",m_editHeight);args.Set("seed",m_scatterSeed);
    if(action=="replace"){
        if(m_paintPalette.empty()){m_scatterStatus="Choose the replacement asset first.";return;}
        args.Set("replacement",m_paintPalette.front().reference);
    }
    if(action=="transform"){args.Set("position",OpData(m_instanceDraft.position));args.Set("rotation",OpData(m_instanceDraft.eulerRadians));args.Set("scale",OpData(m_instanceDraft.scale));}
    const auto result=InvokeOperator(context,"terrain.edit_instances",args);m_scatterStatus=result.ok?result.message:"Error: "+result.message;
    m_nextPaintPreviewTime=0;
    if(result.ok){m_instanceRevision=0;RefreshInstanceSelection(context);PublishInstancePreview(context);}
}

void TerrainTool::DrawInstanceWorkspace(EditorContext& context,scene::GameObject*)
{
    const char* names[]{"Paint","Select object","Outline area"};
    for(int i=0;i<3;++i){if(i)ImGui::SameLine();if(ImGui::RadioButton(names[i],m_instanceMode==i)){
        m_instanceMode=i;m_scatterStroke.clear();m_paintPreviewInputs.clear();m_nextPaintPreviewTime=0;
        if(i==1)m_editScope=3;else if(i==2)m_editScope=1;else m_editScope=0;
    }}
    if(m_instanceMode==2){
        ImGui::TextWrapped("Click corners on the terrain. Enter fills the outline. Backspace removes a corner; Esc clears it.");
        ImGui::Text("Area corners: %zu",m_paintArea.size());
        ImGui::BeginDisabled(m_paintArea.size()<3||m_paintPalette.empty());
        if(ImGui::Button("Fill Area")){
            auto args=ScatterStrokeArguments(m_heightMapTargetId,m_paintArea,m_brush.radius,m_scatterErase!=ImGui::GetIO().KeyCtrl);
            const auto result=InvokeOperator(context,"terrain.paint_instances",args);m_scatterStatus=result.message;m_nextPaintPreviewTime=0;
        }ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button("Clear Area")){m_paintArea.clear();m_nextPaintPreviewTime=0;}
    }
    if(m_instanceMode!=1)return;
    RefreshInstanceSelection(context);
    ImGui::TextWrapped("Click a tree, grass clump or stone. The selected palette filters picking. W / E / R: move / rotate / scale.");
    if(m_instanceOwner.empty())return;
    ImGui::Text("Selected object ID: %u",m_instanceId);
    ImGui::DragFloat3("Position [local m]",&m_instanceDraft.position.x,0.05f);
    math::Vector3 angles{math::ToDeg(m_instanceDraft.eulerRadians.x),math::ToDeg(m_instanceDraft.eulerRadians.y),math::ToDeg(m_instanceDraft.eulerRadians.z)};
    if(ImGui::DragFloat3("Rotation [deg]",&angles.x,0.5f))m_instanceDraft.eulerRadians={math::ToRad(angles.x),math::ToRad(angles.y),math::ToRad(angles.z)};
    ImGui::DragFloat3("Scale",&m_instanceDraft.scale.x,0.01f);
    if(ImGui::Button("Apply Transform"))ApplyInstanceAdjustment(context,"transform");
    ImGui::SameLine();if(ImGui::Button("Reset Preview"))m_instanceDraft=m_instanceOriginal;
    if(ImGui::Button("Delete Object")){const float previous=m_editKeep;m_editKeep=0;ApplyInstanceAdjustment(context,"thin");m_editKeep=previous;}
    const char* gizmos[]{"Move","Rotate","Scale"};for(int i=0;i<3;++i){if(i)ImGui::SameLine();if(ImGui::RadioButton(gizmos[i],m_instanceGizmo==i))m_instanceGizmo=i;}
    PublishInstancePreview(context);
    if(m_paintPalette.empty())DrawInstanceAdjustments(context,nullptr);
}

void TerrainTool::DrawInstanceAdjustments(EditorContext& context,scene::GameObject*)
{
    if(!ImGui::CollapsingHeader("Adjust existing objects"))return;
    const char* scopes[]{"Last brush position","Outlined area","Whole terrain","Selected object"};
    ImGui::Combo("Edit scope",&m_editScope,scopes,4);
    if(m_editScope!=3)ImGui::TextWrapped("Applies to the selected palette within this scope. Whole terrain includes all its matching objects.");
    ImGui::BeginDisabled((m_editScope!=3&&m_paintPalette.empty())||m_heightMapTargetId.empty());
    if(ImGui::Button("Reground Objects"))ApplyInstanceAdjustment(context,"reground");
    ImGui::SetItemTooltip("Fits roots to edited ground. IDs and size stay fixed; asset rules control slope alignment.");
    float percent=m_editKeep*100;
    if(ImGui::SliderFloat("Keep density",&percent,0,100,"%.0f%%",ImGuiSliderFlags_AlwaysClamp))m_editKeep=percent/100;
    if(ImGui::Button("Thin Objects"))ApplyInstanceAdjustment(context,"thin");
    ImGui::DragFloat("Size multiplier",&m_editScale,0.01f,0.01f,100,"%.2f",ImGuiSliderFlags_AlwaysClamp);
    ImGui::DragFloat("Height multiplier",&m_editHeight,0.01f,0.01f,100,"%.2f",ImGuiSliderFlags_AlwaysClamp);
    if(ImGui::Button("Resize Objects"))ApplyInstanceAdjustment(context,"scale");
    ImGui::BeginDisabled(m_paintPalette.empty());
    if(ImGui::Button("Replace With First Palette Asset"))ApplyInstanceAdjustment(context,"replace");ImGui::EndDisabled();ImGui::EndDisabled();
}

void TerrainTool::DrawPaintDiagnostics(const renderer::Camera& camera,const ImVec2& origin,const ImVec2& size) const
{
    if(!m_paintShowDiagnostics||!m_showPaintPreview)return;
    auto* draw=ImGui::GetForegroundDrawList();draw->PushClipRect(origin,{origin.x+size.x,origin.y+size.y},true);
    for(const auto& point:m_paintDiagnostics.points){
        const auto screen=terrain_tool_instances::Project(camera,origin,size,point.position+math::Vector3{0,0.05f,0});
        const auto color=terrain_tool_instances::Color(point.reason);draw->AddLine({screen.x-3,screen.y-3},{screen.x+3,screen.y+3},color,1.5f);draw->AddLine({screen.x-3,screen.y+3},{screen.x+3,screen.y-3},color,1.5f);
    }draw->PopClipRect();
}

void TerrainTool::DrawPaintDiagnosticSummary() const
{
    if(!m_paintShowDiagnostics||m_instanceMode==1)return;
    for(size_t i=1;i<static_cast<size_t>(TerrainPaintReason::COUNT);++i)if(m_paintDiagnostics.counts[i])
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(terrain_tool_instances::Color(static_cast<TerrainPaintReason>(i))),"%s: %zu",TerrainPaintReasonLabel(static_cast<TerrainPaintReason>(i)),m_paintDiagnostics.counts[i]);
}

void TerrainTool::UpdatePaintArea(EditorContext& context,const renderer::Camera& camera,const ImVec2& origin,const ImVec2& size,bool pressed,bool hovered)
{
    if(pressed&&m_hitTerrain&&m_hitTerrain->instanceId==m_heightMapTargetId&&m_paintArea.size()<TERRAIN_SCATTER_STROKE_LIMIT){m_paintArea.push_back(m_hitPoint);m_paintPreviewTerrainId=m_heightMapTargetId;m_nextPaintPreviewTime=0;}
    const bool keyboard=hovered&&!ImGui::GetIO().WantTextInput;
    if(keyboard&&ImGui::IsKeyPressed(ImGuiKey_Backspace)&&!m_paintArea.empty()){m_paintArea.pop_back();m_nextPaintPreviewTime=0;}
    if(keyboard&&ImGui::IsKeyPressed(ImGuiKey_Escape)){m_paintArea.clear();m_nextPaintPreviewTime=0;}
    if(keyboard&&ImGui::IsKeyPressed(ImGuiKey_Enter)&&m_paintArea.size()>=3&&!m_paintPalette.empty()){
        const auto result=InvokeOperator(context,"terrain.paint_instances",ScatterStrokeArguments(m_heightMapTargetId,m_paintArea,m_brush.radius,m_scatterErase!=ImGui::GetIO().KeyCtrl));m_scatterStatus=result.message;m_nextPaintPreviewTime=0;
    }
    auto* draw=ImGui::GetForegroundDrawList();draw->PushClipRect(origin,{origin.x+size.x,origin.y+size.y},true);
    for(size_t i=0;i<m_paintArea.size();++i){const auto a=terrain_tool_instances::Project(camera,origin,size,m_paintArea[i]);draw->AddCircleFilled(a,4,IM_COL32(99,181,138,255));
        if(m_paintArea.size()>1){const auto b=terrain_tool_instances::Project(camera,origin,size,m_paintArea[(i+1)%m_paintArea.size()]);draw->AddLine(a,b,IM_COL32(99,181,138,220),2);}}
    draw->PopClipRect();
}

void TerrainTool::UpdateInstanceSelection(EditorContext& context,const renderer::Camera& camera,const ImVec2& origin,const ImVec2& size,bool hovered)
{
    using namespace terrain_tool_instances;
    scene::FlushWorldTransforms(*context.activeScene);RefreshInstanceSelection(context);
    if(hovered&&!ImGui::GetIO().WantTextInput&&!ImGui::IsMouseDown(ImGuiMouseButton_Right)){
        if(ImGui::IsKeyPressed(ImGuiKey_W))m_instanceGizmo=0;if(ImGui::IsKeyPressed(ImGuiKey_E))m_instanceGizmo=1;if(ImGui::IsKeyPressed(ImGuiKey_R))m_instanceGizmo=2;
    }
    bool over=false;
    if(auto* owner=context.activeScene->FindByGuid(m_instanceOwner)){
        ImGuizmo::PushID(this);
        const bool other=ImGuizmo::IsUsingAny()&&!ImGuizmo::IsUsing();
        if(!other){
            auto view=math::Matrix4::Transpose(camera.GetViewMatrix()),projection=math::Matrix4::Transpose(camera.GetProjectionMatrix());
            auto matrix=math::Matrix4::Transpose(owner->transform.GetWorldMatrix()*math::Matrix4::TRS(m_instanceDraft.position,math::Quaternion::FromEuler(m_instanceDraft.eulerRadians),m_instanceDraft.scale));
            ImGuizmo::SetDrawlist();ImGuizmo::SetOrthographic(camera.m_projection==renderer::ProjectionMode::Orthographic);ImGuizmo::SetRect(origin.x,origin.y,size.x,size.y);
            const ImGuizmo::OPERATION ops[]{ImGuizmo::TRANSLATE,ImGuizmo::ROTATE,ImGuizmo::SCALE};
            ImGuizmo::Manipulate(&view.m[0][0],&projection.m[0][0],ops[m_instanceGizmo],m_instanceGizmo==0?ImGuizmo::WORLD:ImGuizmo::LOCAL,&matrix.m[0][0]);
            over=ImGuizmo::IsOver();const bool usingGizmo=ImGuizmo::IsUsing();
            if(usingGizmo){auto candidate=m_instanceDraft;const auto local=math::Matrix4::Inverse(owner->transform.GetWorldMatrix())*math::Matrix4::Transpose(matrix);
                if(DecomposeTerrainPlacement(local,candidate))m_instanceDraft=candidate;}
            if(m_instanceDragging&&!usingGizmo){m_instanceDragging=false;if(ImGui::IsMouseReleased(ImGuiMouseButton_Left))ApplyInstanceAdjustment(context,"transform");else m_instanceDraft=m_instanceOriginal;}
            m_instanceDragging=usingGizmo;
        }ImGuizmo::PopID();
    }
    if(hovered&&!over&&!m_instanceDragging&&!ImGui::GetIO().KeyAlt&&ImGui::IsMouseClicked(ImGuiMouseButton_Left)&&context.resources){
        const auto mouse=ImGui::GetMousePos();const auto ray=math::Ray::FromNDC((mouse.x-origin.x)/size.x*2-1,1-(mouse.y-origin.y)/size.y*2,camera.m_position,math::Matrix4::Inverse(camera.GetViewProjection()));
        float nearest=std::numeric_limits<float>::max();std::string picked;uint32_t pickedId=0;
        for(auto id:context.activeScene->GetEntities<scene::InstancedMeshComponent>()){
            auto* owner=context.activeScene->GetGameObject(id);const auto* component=context.activeScene->GetComponent<scene::InstancedMeshComponent>(id);
            if(!owner||!owner->activeInHierarchy()||!component||!component->m_enabled)continue;
            const auto* definition=asset::ResolveMeshInstanceDefinition(component->m_instanceSetRef.path);const auto snapshot=component->Snapshot();if(!definition||!snapshot)continue;
            const auto file=asset::AssetManager::ResolveAssetPath(component->m_instanceSetRef.path);
            const auto* stored=util::FileSystem::GetExtension(file)==".meshinstances"?asset::AssetManager::Get(asset::AssetManager::Load<asset::MeshInstancesAsset>(file)):nullptr;
            bool eligible=m_paintPalette.empty();
            for(const auto& source:m_paintPalette){
                if(stored&&!stored->terrainPaintRef.empty()){
                    const auto sourcePath=asset::AssetManager::ResolveAssetPath(source.reference),storedPath=asset::AssetManager::ResolveAssetPath(stored->terrainPaintRef);
                    eligible=eligible||(!sourcePath.empty()&&!storedPath.empty()&&util::FileSystem::SamePathText(sourcePath,storedPath));continue;
                }
                const auto* candidate=asset::ResolveMeshInstanceDefinition(source.reference);if(!candidate)continue;
                eligible=eligible||std::all_of(definition->Parts().begin(),definition->Parts().end(),[&](const auto& p){return std::any_of(candidate->Parts().begin(),candidate->Parts().end(),[&](const auto& q){return p.meshPath==q.meshPath&&p.materialPath==q.materialPath;});});
            }if(!eligible)continue;
            math::Vector3 low{INFINITY,INFINITY,INFINITY},high{-INFINITY,-INFINITY,-INFINITY};
            for(const auto& part:definition->Parts())if(part.lod==0)if(const auto* mesh=scene::ResolveMeshPath(part.meshPath,*context.resources)){
                auto extents=mesh->boundsExtents;if(extents.LengthSq()<0.000001f)extents={mesh->boundsRadius,mesh->boundsRadius,mesh->boundsRadius};
                low.x=std::min(low.x,mesh->boundsCenter.x-extents.x);low.y=std::min(low.y,mesh->boundsCenter.y-extents.y);low.z=std::min(low.z,mesh->boundsCenter.z-extents.z);
                high.x=std::max(high.x,mesh->boundsCenter.x+extents.x);high.y=std::max(high.y,mesh->boundsCenter.y+extents.y);high.z=std::max(high.z,mesh->boundsCenter.z+extents.z);
            }if(!std::isfinite(low.x))continue;
            if(!ValidScale(owner->transform.worldScale))continue;
            const auto inverse=math::Matrix4::Inverse(owner->transform.GetWorldMatrix());const auto o=inverse*math::Vector4{ray.origin,1},d=inverse*math::Vector4{ray.direction,0};
            for(const auto& placement:snapshot->placements){if(!placement.active||!ValidScale(placement.scale))continue;const auto rotation=math::Quaternion::FromEuler(placement.eulerRadians).Inverse();
                auto a=rotation*(math::Vector3{o.x,o.y,o.z}-placement.position),b=rotation*math::Vector3{d.x,d.y,d.z};
                a={a.x/placement.scale.x,a.y/placement.scale.y,a.z/placement.scale.z};b={b.x/placement.scale.x,b.y/placement.scale.y,b.z/placement.scale.z};
                float distance=nearest;if(RayBox(a,b,(low+high)*0.5f,(high-low)*0.5f,distance)&&distance<nearest){nearest=distance;picked=owner->instanceId;pickedId=placement.id;}
            }
        }
        m_instanceOwner=picked;m_instanceId=pickedId;m_instanceRevision=0;
        if(auto* owner=context.activeScene->FindByGuid(picked))m_instanceLifetime=context.activeScene->GetComponentLifetime<scene::InstancedMeshComponent>(owner->GetID());
        RefreshInstanceSelection(context);
    }
    PublishInstancePreview(context);
}
} /// @note namespace fbzz::editor
