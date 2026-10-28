/// @file    ManagedRenderComponentServices.cpp
/// @brief   描画Componentの実rowを検証し、型付きC#操作を転送する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include "ManagedRenderComponentServices.hpp"
#include "ManagedServicesBridge.hpp"
#include "ManagedObjectControls.hpp"
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderSceneExtractor.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>
#include <Engine/Scene/Components/MeshTrailComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/InstancedMeshComponent.hpp>
#include <Engine/Asset/InstanceSetAsset.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Scene/Components/ProceduralMeshComponent.hpp>
#include <Engine/Scene/SceneAssetReferences.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Asset/ModelAsset.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
namespace fbzz::scene::managed_detail {
namespace {
using Status = ManagedBridgeStatus;
bool Packet(bool read, bool expected, int64_t integer, std::span<const float> p, std::span<const std::string> t,
    size_t floats, size_t strings, bool allowInteger = false)
{
    return read == expected && (allowInteger || integer == 0) && p.size() == floats && t.size() == strings;
}
Status Bool(bool& value, bool read, bool expected, int64_t integer, std::span<const float> p,
    std::span<const std::string> t, ManagedServiceResult& out)
{
    if (!Packet(read, expected, integer, p, t, 0, 0, !expected) || (!expected && integer != 0 && integer != 1)) return Status::INVALID_ARGUMENT;
    if (expected) out.integer = value; else value = integer != 0;
    return Status::OK;
}
Status Int(int& value, bool read, bool expected, int64_t integer, std::span<const float> p,
    std::span<const std::string> t, ManagedServiceResult& out,
    int minimum = (std::numeric_limits<int>::min)(), int maximum = (std::numeric_limits<int>::max)())
{
    if (!Packet(read, expected, integer, p, t, 0, 0, !expected) || (!expected && (integer < minimum || integer > maximum))) return Status::INVALID_ARGUMENT;
    if (expected) out.integer = value; else value = static_cast<int>(integer);
    return Status::OK;
}
Status Float(float& value, bool read, bool expected, int64_t integer, std::span<const float> p,
    std::span<const std::string> t, ManagedServiceResult& out, float minimum = -(std::numeric_limits<float>::max)())
{
    if (!Packet(read, expected, integer, p, t, expected ? 0U : 1U, 0) || (!expected && p[0] < minimum)) return Status::INVALID_ARGUMENT;
    if (expected) out.values = {value}; else value = p[0];
    return Status::OK;
}
Status V2(math::Vector2& value, bool read, bool expected, int64_t integer, std::span<const float> p,
    std::span<const std::string> t, ManagedServiceResult& out, float minimum = -(std::numeric_limits<float>::max)())
{
    if (!Packet(read, expected, integer, p, t, expected ? 0U : 2U, 0) || (!expected && (p[0] < minimum || p[1] < minimum))) return Status::INVALID_ARGUMENT;
    if (expected) out.values = {value.x, value.y}; else value = {p[0], p[1]};
    return Status::OK;
}
Status V4(math::Vector4& value, bool read, bool expected, int64_t integer, std::span<const float> p,
    std::span<const std::string> t, ManagedServiceResult& out)
{
    if (!Packet(read, expected, integer, p, t, expected ? 0U : 4U, 0)) return Status::INVALID_ARGUMENT;
    if (expected) out.values = {value.x, value.y, value.z, value.w}; else value = {p[0], p[1], p[2], p[3]};
    return Status::OK;
}
template<class T> Status Enum(T& value, bool read, bool expected, int64_t integer, std::span<const float> p,
    std::span<const std::string> t, ManagedServiceResult& out, int minimum, int maximum)
{
    int current = static_cast<int>(value);
    const auto status = Int(current, read, expected, integer, p, t, out, minimum, maximum);
    if (status == Status::OK && !expected) value = static_cast<T>(current);
    return status;
}
bool AssetPath(const std::string& reference, SceneAssetKind kind, std::string& path)
{
    if (reference.empty()) { path.clear(); return true; }
    if (!reference.starts_with("guid:") || reference.find('\0') != std::string::npos) return false;
    path = asset::AssetDatabase::PathFromGuid(reference.substr(5));
    const auto classified = ClassifySceneAssetReference(path);
    if (path.empty() || !classified || classified->kind != kind) return false;
    if (kind == SceneAssetKind::Material) return asset::AssetManager::Load<asset::MaterialAsset>(path).IsValid();
    if (kind == SceneAssetKind::Texture) return asset::AssetManager::Load<asset::TextureAsset>(path).IsValid();
    return asset::AssetManager::Load<asset::ModelAsset>(path).IsValid();
}
Status Reference(const std::string& path, ManagedServiceResult& out)
{
    if (path.empty()) { out.text = {""}; return Status::OK; }
    const auto resolved = asset::AssetManager::ResolveAssetPath(path);
    const auto guid = asset::AssetDatabase::TryGetGuidFromPath(resolved);
    if (guid.empty()) return Status::NOT_FOUND;
    out.text = {"guid:" + guid}; return Status::OK;
}
Status Material(std::string& path, bool read, int64_t integer, std::span<const float> p, std::span<const std::string> t,
    ManagedServiceResult& out)
{
    if (!Packet(read, read, integer, p, t, 0, read ? 0U : 1U)) return Status::INVALID_ARGUMENT;
    if (read) return Reference(path, out);
    std::string candidate; out.integer = AssetPath(t[0], SceneAssetKind::Material, candidate);
    if (out.integer) path = std::move(candidate);
    return Status::OK;
}
}
ManagedBridgeStatus RunRenderComponentServices(Scene& scene, Script& view, GameObject& target, GameObject* related,
    const ManagedBridgeRequest& request, int64_t integer, std::span<const float> p,
    std::span<const std::string> t, ManagedServiceResult& result)
{
    if (!target.IsValid() || target.GetScene() != &scene) return Status::NOT_FOUND;
    if (related || request.flags > 1 || !std::all_of(p.begin(), p.end(), [](float value) { return std::isfinite(value); })
        || std::any_of(t.begin(), t.end(), [](const auto& value) { return value.find('\0') != std::string::npos; })) return Status::INVALID_ARGUMENT;
    const bool read = request.flags == 1;
    const bool play = Script::IsInPlayMode();
    ManagedServiceResult out;
    Status status = Status::UNSUPPORTED;
    switch (request.op) {
    case ManagedBridgeOp::SERVICE_INSTANCES: {
        auto* row=target.GetComponent<InstancedMeshComponent>();
        if (!row || ManagedObjectControls::IsPendingComponentRemoval(target,53)) return Status::NOT_FOUND;
        switch (request.index) {
        case 0: case 1: status=Bool(row->m_enabled,read,request.index==0,integer,p,t,out); break;
        case 2: case 3: status=Bool(row->m_castShadows,read,request.index==2,integer,p,t,out); break;
        case 4: {
            if (!Packet(read,true,integer,p,t,0,0)) return Status::INVALID_ARGUMENT;
            const auto data=row->Snapshot();
            out.integer=data ? std::count_if(data->placements.begin(),data->placements.end(),[](const auto& value){return value.active;}) : 0;
            status=Status::OK; break;
        }
        case 5:
            if (!Packet(read,true,integer,p,t,0,0)) return Status::INVALID_ARGUMENT;
            status=Reference(row->m_instanceSetRef.path,out); break;
        case 6: {
            if (!Packet(read,false,integer,p,t,0,1)) return Status::INVALID_ARGUMENT;
            const auto* candidate=asset::DataAssetRegistry::Resolve(t[0]);
            out.integer=candidate && std::string_view(candidate->GetTypeName())==asset::InstanceSetAsset::TYPE_NAME
                && static_cast<const asset::InstanceSetAsset*>(candidate)->Snapshot()!=nullptr;
            if (out.integer) { row->m_instanceSetRef.path=t[0]; row->ResetOverrides(); }
            status=Status::OK; break;
        }
        case 7:
            if (!play || !Packet(read,false,integer,p,t,9,0,true) || integer<=0 || integer>INT32_MAX) return Status::INVALID_ARGUMENT;
            out.integer=row->SetTransform(static_cast<uint32_t>(integer),{p[0],p[1],p[2]},{p[3],p[4],p[5]},{p[6],p[7],p[8]}); status=Status::OK; break;
        case 8:
            if (!play || !Packet(read,false,integer,p,t,0,0,true) || integer<=0 || integer>INT32_MAX) return Status::INVALID_ARGUMENT;
            out.integer=row->Remove(static_cast<uint32_t>(integer)); status=Status::OK; break;
        case 9:
            if (!play || !Packet(read,false,integer,p,t,0,0)) return Status::INVALID_ARGUMENT;
            row->ResetOverrides(); status=Status::OK; break;
        case 10:
            if (!play || !Packet(read,false,integer,p,t,9,0)) return Status::INVALID_ARGUMENT;
            out.integer=row->Add({p[0],p[1],p[2]},{p[3],p[4],p[5]},{p[6],p[7],p[8]}); status=Status::OK; break;
        default: return Status::INVALID_ARGUMENT;
        }
        break;
    }
    case ManagedBridgeOp::SERVICE_LINE: {
        auto* row = target.GetComponent<LineRendererComponent>();
        if (!row || ManagedObjectControls::IsPendingComponentRemoval(target, 35)) return Status::NOT_FOUND;
        switch (request.index) {
        case 0: case 1: status = Bool(row->enabled, read, request.index == 0, integer, p, t, out); break;
        case 2: case 3: status = V4(row->startColor, read, request.index == 2, integer, p, t, out); break;
        case 4: case 5: status = V4(row->endColor, read, request.index == 4, integer, p, t, out); break;
        case 6: case 7: status = Float(row->startWidth, read, request.index == 6, integer, p, t, out, 0); break;
        case 8: case 9: status = Float(row->endWidth, read, request.index == 8, integer, p, t, out, 0); break;
        case 10: case 11: status = Enum(row->space, read, request.index == 10, integer, p, t, out, 0, 1); break;
        case 12: case 13: status = Bool(row->loop, read, request.index == 12, integer, p, t, out); break;
        case 16: case 17: status = Bool(row->billboard, read, request.index == 16, integer, p, t, out); break;
        case 18: case 19: status = Enum(row->shape, read, request.index == 18, integer, p, t, out, 0, 1); break;
        case 20: case 21: status = Int(row->radialSegments, read, request.index == 20, integer, p, t, out, 3, 32); break;
        case 22: case 23: status = Int(row->sortingLayer, read, request.index == 22, integer, p, t, out); break;
        case 24: case 25: status = Int(row->orderInLayer, read, request.index == 24, integer, p, t, out); break;
        case 14:
            if (!Packet(read, true, integer, p, t, 0, 0)) return Status::INVALID_ARGUMENT;
            out.integer = static_cast<int64_t>(row->points.size()); status = Status::OK; break;
        case 15:
            if (!Packet(read, true, integer, p, t, 0, 0, true) || integer < 0 || static_cast<uint64_t>(integer) >= row->points.size()) return Status::INVALID_ARGUMENT;
            { const auto& point = row->points[static_cast<size_t>(integer)]; out.values = {point.x, point.y, point.z}; } status = Status::OK; break;
        case 26: case 27:
            if (read != (request.index == 26)) return Status::INVALID_ARGUMENT;
            status = Material(row->materialPath, read, integer, p, t, out); break;
        case 28:
            if (!Packet(read, false, integer, p, t, 6, 0, true) || (integer != 0 && integer != 1)) return Status::INVALID_ARGUMENT;
            out.integer = view.gameplay.SetLine({p[0], p[1], p[2]}, {p[3], p[4], p[5]}, integer != 0); status = Status::OK; break;
        case 29:
            if (!Packet(read, false, integer, p, t, 8, 0)) return Status::INVALID_ARGUMENT;
            out.integer = view.gameplay.SetLineColors({p[0], p[1], p[2], p[3]}, {p[4], p[5], p[6], p[7]}); status = Status::OK; break;
        case 30:
            if (!Packet(read, false, integer, p, t, 2, 0) || p[0] < 0 || p[1] < 0) return Status::INVALID_ARGUMENT;
            out.integer = view.gameplay.SetLineWidth(p[0], p[1]); status = Status::OK; break;
        case 54: {
            uint64_t count = 0;
            if (!Packet(read, true, integer, p, t, 0, 1, true) || integer < 0) return Status::INVALID_ARGUMENT;
            const auto parsed = std::from_chars(t[0].data(), t[0].data() + t[0].size(), count);
            if (parsed.ec != std::errc{} || parsed.ptr != t[0].data() + t[0].size() || count != row->points.size()
                || static_cast<uint64_t>(integer) > count) return Status::INVALID_ARGUMENT;
            const size_t end = (std::min)(row->points.size(), static_cast<size_t>(integer) + 42);
            for (size_t index = static_cast<size_t>(integer); index < end; ++index) {
                const auto& point = row->points[index]; out.values.insert(out.values.end(), {point.x, point.y, point.z});
            }
            status = Status::OK; break;
        }
        default: return Status::INVALID_ARGUMENT;
        }
        break;
    }
    case ManagedBridgeOp::SERVICE_SPRITE: {
        auto* row = target.GetComponent<SpriteRendererComponent>();
        if (!row || ManagedObjectControls::IsPendingComponentRemoval(target, 36)) return Status::NOT_FOUND;
        switch (request.index) {
        case 0: case 1: status = Bool(row->enabled, read, request.index == 0, integer, p, t, out); break;
        case 2: case 3: status = V4(row->color, read, request.index == 2, integer, p, t, out); break;
        case 4: case 5: status = V2(row->size, read, request.index == 4, integer, p, t, out, 0); break;
        case 6: case 7: status = V2(row->pivot, read, request.index == 6, integer, p, t, out); break;
        case 8: case 9: status = Enum(row->drawMode, read, request.index == 8, integer, p, t, out, 0, 2); break;
        case 10: case 11: status = Bool(row->flipX, read, request.index == 10, integer, p, t, out); break;
        case 12: case 13: status = Bool(row->flipY, read, request.index == 12, integer, p, t, out); break;
        case 14: case 15: status = Bool(row->receiveLighting, read, request.index == 14, integer, p, t, out); break;
        case 16: case 17: status = Bool(row->useSpriteNativeSize, read, request.index == 16, integer, p, t, out); break;
        case 18: case 19: status = Int(row->sortingLayer, read, request.index == 18, integer, p, t, out); break;
        case 20: case 21: status = Int(row->orderInLayer, read, request.index == 20, integer, p, t, out); break;
        case 22:
            if (!Packet(read, true, integer, p, t, 0, 0)) return Status::INVALID_ARGUMENT;
            out.text = {row->spritePath}; status = Status::OK; break;
        case 23: {
            if (!Packet(read, false, integer, p, t, 0, 2) || t[1].find("::") != std::string::npos) return Status::INVALID_ARGUMENT;
            std::string path; out.integer = AssetPath(t[0], SceneAssetKind::Texture, path);
            if (out.integer && !t[1].empty()) {
                if (path.empty()) out.integer = 0;
                else {
                    const auto sprite = asset::ResolveSpriteReference(asset::MakeSpriteReference(path, t[1]), 1.f, 1.f);
                    if (!sprite.resolved || sprite.IsBroken()) out.integer = 0;
                    else {
                        const auto id = asset::LookupSpriteId(path, t[1]);
                        if (id.empty()) out.integer = 0;
                        else path = asset::MakeSpriteReference(path, id);
                    }
                }
            }
            if (out.integer) view.gameplay.SetSprite(path);
            status = Status::OK; break;
        }
        case 24: case 25:
            if (read != (request.index == 24)) return Status::INVALID_ARGUMENT;
            status = Material(row->materialPath, read, integer, p, t, out); break;
        default: return Status::INVALID_ARGUMENT;
        }
        break;
    }
    case ManagedBridgeOp::SERVICE_SORTING_GROUP: {
        auto* row = target.GetComponent<SortingGroupComponent>();
        if (!row || ManagedObjectControls::IsPendingComponentRemoval(target, 37)) return Status::NOT_FOUND;
        switch (request.index) {
        case 0: case 1: status = Bool(row->enabled, read, request.index == 0, integer, p, t, out); break;
        case 2: case 3: status = Int(row->sortingLayer, read, request.index == 2, integer, p, t, out); break;
        case 4: case 5: status = Int(row->orderInLayer, read, request.index == 4, integer, p, t, out); break;
        default: return Status::INVALID_ARGUMENT;
        }
        break;
    }
    case ManagedBridgeOp::SERVICE_TRAIL: {
        auto* row = target.GetComponent<TrailComponent>();
        if (!row || ManagedObjectControls::IsPendingComponentRemoval(target, 38)) return Status::NOT_FOUND;
        if (!read && !play) return Status::INVALID_SCOPE;
        switch (request.index) {
        case 0:
            if (!Packet(read, true, integer, p, t, 0, 0)) return Status::INVALID_ARGUMENT;
            out.integer = view.trail.IsEnabled(); break;
        case 1:
            if (!Packet(read, false, integer, p, t, 1, 0, true) || (integer != 0 && integer != 1) || (p[0] != 0 && p[0] != 1)) return Status::INVALID_ARGUMENT;
            view.trail.SetEnabled(integer != 0, p[0] != 0); break;
        case 2: case 3:
            if (!Packet(read, request.index == 2, integer, p, t, request.index == 2 ? 0U : 1U, 0)) return Status::INVALID_ARGUMENT;
            if (read) out.values = {row->duration};
            else { if (p[0] < 0) return Status::INVALID_ARGUMENT; view.trail.SetDuration(p[0]); } break;
        case 4:
            if (!Packet(read, true, integer, p, t, 0, 0)) return Status::INVALID_ARGUMENT;
            out.integer = view.trail.GetPointCount(); break;
        case 5: case 26:
            if (!Packet(read, false, integer, p, t, 0, 0)) return Status::INVALID_ARGUMENT;
            if (request.index == 5) view.trail.Clear(); else view.trail.ClearAttachBone(); break;
        case 6:
            if (!Packet(read, true, integer, p, t, 0, 0)) return Status::INVALID_ARGUMENT;
            out.integer = row->maxPoints; break;
        case 7:
            if (!Packet(read, false, integer, p, t, 0, 0, true) || integer < 2 || integer > (std::numeric_limits<int>::max)()) return Status::INVALID_ARGUMENT;
            view.trail.SetMaxPoints(static_cast<int>(integer)); break;
        case 8: case 9:
            if (!Packet(read, request.index == 8, integer, p, t, request.index == 8 ? 0U : 2U, 0)) return Status::INVALID_ARGUMENT;
            if (read) out.values = {row->sampleInterval, row->minVertexDist};
            else { if (p[0] < 0 || p[1] < 0) return Status::INVALID_ARGUMENT; view.trail.SetSampling(p[0], p[1]); } break;
        case 10: case 11:
            if (!Packet(read, request.index == 10, integer, p, t, request.index == 10 ? 0U : 2U, 0)) return Status::INVALID_ARGUMENT;
            if (read) out.values = {row->widthStart, row->widthEnd};
            else { if (p[0] < 0 || p[1] < 0) return Status::INVALID_ARGUMENT; view.trail.SetWidth(p[0], p[1]); } break;
        case 12: case 13:
            if (!Packet(read, request.index == 12, integer, p, t, request.index == 12 ? 0U : 8U, 0)) return Status::INVALID_ARGUMENT;
            if (read) out.values = {row->colorStart.x, row->colorStart.y, row->colorStart.z, row->colorStart.w, row->colorEnd.x, row->colorEnd.y, row->colorEnd.z, row->colorEnd.w};
            else view.trail.SetColor({p[0], p[1], p[2], p[3]}, {p[4], p[5], p[6], p[7]}); break;
        case 14: case 15:
            status = Enum(row->widthEasing, read, request.index == 14, integer, p, t, out, 0, 3); if (status != Status::OK) return status; break;
        case 16: case 17:
            status = Enum(row->alignment, read, request.index == 16, integer, p, t, out, 0, 1); if (status != Status::OK) return status; break;
        case 18: case 19:
            status = Enum(row->uvMode, read, request.index == 18, integer, p, t, out, 0, 1); if (status != Status::OK) return status; break;
        case 20: case 21:
            status = Int(row->smoothSubdivisions, read, request.index == 20, integer, p, t, out, 0, 8); if (status != Status::OK) return status; break;
        case 22:
            if (!Packet(read, true, integer, p, t, 0, 0)) return Status::INVALID_ARGUMENT;
            out.text = {row->attachBone}; out.values = {row->attachOffset.x, row->attachOffset.y, row->attachOffset.z}; break;
        case 23:
            if (!Packet(read, false, integer, p, t, 3, 1) || t[0].empty()) return Status::INVALID_ARGUMENT;
            view.trail.SetAttachBone(t[0], {p[0], p[1], p[2]}); break;
        case 24: case 25:
            if (read != (request.index == 24)) return Status::INVALID_ARGUMENT;
            status = Material(row->materialPath, read, integer, p, t, out); if (status != Status::OK) return status; break;
        case 27: case 28:
            status = Float(row->breakDistance, read, request.index == 27, integer, p, t, out, 0); if (status != Status::OK) return status; break;
        case 29: case 30:
            if (!Packet(read, request.index == 29, integer, p, t, request.index == 29 ? 0U : 2U, 0)) return Status::INVALID_ARGUMENT;
            if (read) out.values = {row->uvScrollSpeed, row->uvTiling};
            else { if (p[1] < 0) return Status::INVALID_ARGUMENT; row->uvScrollSpeed = p[0]; row->uvTiling = p[1]; } break;
        default: return Status::INVALID_ARGUMENT;
        }
        status = Status::OK; break;
    }
    case ManagedBridgeOp::SERVICE_MESH_TRAIL: {
        auto* row = target.GetComponent<MeshTrailComponent>();
        if (!row || ManagedObjectControls::IsPendingComponentRemoval(target, 39)) return Status::NOT_FOUND;
        if (!read && !play) return Status::INVALID_SCOPE;
        switch (request.index) {
        case 0:
            if (!Packet(read, true, integer, p, t, 0, 0)) return Status::INVALID_ARGUMENT;
            out.integer = view.meshTrail.IsEnabled(); break;
        case 1:
            if (!Packet(read, false, integer, p, t, 1, 0, true) || (integer != 0 && integer != 1) || (p[0] != 0 && p[0] != 1)) return Status::INVALID_ARGUMENT;
            view.meshTrail.SetEnabled(integer != 0, p[0] != 0); break;
        case 2: case 3:
            if (!Packet(read, request.index == 2, integer, p, t, request.index == 2 ? 0U : 1U, 0)) return Status::INVALID_ARGUMENT;
            if (read) out.values = {row->duration};
            else { if (p[0] < 0) return Status::INVALID_ARGUMENT; view.meshTrail.SetDuration(p[0]); } break;
        case 4:
            if (!Packet(read, true, integer, p, t, 0, 0)) return Status::INVALID_ARGUMENT;
            out.integer = view.meshTrail.GetSampleCount(); break;
        case 5: case 15:
            if (!Packet(read, false, integer, p, t, 0, 0)) return Status::INVALID_ARGUMENT;
            if (request.index == 5) view.meshTrail.Clear(); else view.meshTrail.ClearExcludedMeshIndices(); break;
        case 6:
            if (!Packet(read, true, integer, p, t, 0, 0)) return Status::INVALID_ARGUMENT;
            out.integer = row->maxSamples; break;
        case 7:
            if (!Packet(read, false, integer, p, t, 0, 0, true) || integer < 1 || integer > (std::numeric_limits<int>::max)()) return Status::INVALID_ARGUMENT;
            view.meshTrail.SetMaxSamples(static_cast<int>(integer)); break;
        case 8: case 9:
            if (!Packet(read, request.index == 8, integer, p, t, request.index == 8 ? 0U : 2U, 0)) return Status::INVALID_ARGUMENT;
            if (read) out.values = {row->sampleInterval, row->minVertexDist};
            else { if (p[0] < 0 || p[1] < 0) return Status::INVALID_ARGUMENT; view.meshTrail.SetSampling(p[0], p[1]); } break;
        case 10: case 11:
            if (!Packet(read, request.index == 10, integer, p, t, request.index == 10 ? 0U : 8U, 0)) return Status::INVALID_ARGUMENT;
            if (read) out.values = {row->colorStart.x, row->colorStart.y, row->colorStart.z, row->colorStart.w, row->colorEnd.x, row->colorEnd.y, row->colorEnd.z, row->colorEnd.w};
            else view.meshTrail.SetColor({p[0], p[1], p[2], p[3]}, {p[4], p[5], p[6], p[7]}); break;
        case 12: case 13:
            status = Bool(row->doubleSided, read, request.index == 12, integer, p, t, out); if (status != Status::OK) return status; break;
        case 14:
            if (!Packet(read, false, integer, p, t, 0, 0, true) || integer < 0 || integer > (std::numeric_limits<int>::max)()) return Status::INVALID_ARGUMENT;
            view.meshTrail.AddExcludedMeshIndex(static_cast<int>(integer)); break;
        case 16: case 17:
            if (read != (request.index == 16)) return Status::INVALID_ARGUMENT;
            status = Material(row->materialPath, read, integer, p, t, out); if (status != Status::OK) return status; break;
        default: return Status::INVALID_ARGUMENT;
        }
        status = Status::OK; break;
    }
    case ManagedBridgeOp::SERVICE_MESH:
    case ManagedBridgeOp::SERVICE_SKINNED_MESH: {
        const bool skinned = request.op == ManagedBridgeOp::SERVICE_SKINNED_MESH;
        auto* const ownerScene = target.GetScene();
        const auto ownerId = target.GetID();
        auto* mesh = target.GetComponent<MeshRenderer>();
        auto* skin = target.GetComponent<SkinnedMeshRenderer>();
        if ((skinned ? skin == nullptr : mesh == nullptr) || ManagedObjectControls::IsPendingComponentRemoval(target, skinned ? 41U : 40U)) return Status::NOT_FOUND;
        switch (request.index) {
        case 0: case 1:
            status = Bool(skinned ? skin->enabled : mesh->enabled, read, request.index == 0, integer, p, t, out); break;
        case 2: case 3:
            status = Bool(skinned ? skin->castShadows : mesh->castShadows, read, request.index == 2, integer, p, t, out); break;
        case 5: {
            if (!Packet(read, false, integer, p, t, 0, 1, !skinned) || integer < 0 || integer > (std::numeric_limits<int>::max)()) return Status::INVALID_ARGUMENT;
            std::string path; out.integer = AssetPath(t[0], SceneAssetKind::Model, path);
            if (out.integer && !path.empty()) {
                const auto* model = asset::AssetManager::Get<asset::ModelAsset>(asset::AssetManager::Load<asset::ModelAsset>(path));
                if (!model || (skinned ? !model->IsSkinned() : model->lods.empty() || static_cast<uint64_t>(integer) >= model->lods[0].submeshes.size())) out.integer = 0;
            }
            if (out.integer) {
                if (skinned) view.mesh.SetModelPath(path);
                else {
                    target.RemoveComponent<ProceduralMeshComponent>();
                    view.mesh.SetMeshPath(path.empty() ? path : path + ":" + std::to_string(integer));
                }
            }
            status = Status::OK; break;
        }
        case 6: {
            if (skinned || !Packet(read, false, integer, p, t, 0, 0, true) || integer < 0 || integer > 7) return Status::INVALID_ARGUMENT;
            static constexpr const char* PRIMITIVES[] = {"cube", "sphere", "plane", "quad", "cylinder", "cone", "torus", "capsule"};
            target.RemoveComponent<ProceduralMeshComponent>();
            view.mesh.SetMeshPath(std::string("primitive:") + PRIMITIVES[integer]); status = Status::OK; break;
        }
        case 7: case 8: case 55: case 56: {
            if (skinned) return Status::INVALID_ARGUMENT;
            const auto* procedural = target.GetComponent<ProceduralMeshComponent>();
            const std::vector<MeshVertex>* vertices = procedural ? &procedural->builder.Vertices() : mesh->mesh ? &mesh->mesh->cpuVertices : nullptr;
            const std::vector<uint32_t>* indices = procedural ? &procedural->builder.Indices() : mesh->mesh ? &mesh->mesh->cpuIndices : nullptr;
            const size_t count = request.index == 7 || request.index == 55 ? (vertices ? vertices->size() : 0) : (indices ? indices->size() : 0);
            if (request.index <= 8) {
                if (!Packet(read, true, integer, p, t, 0, 0)) return Status::INVALID_ARGUMENT;
                out.integer = static_cast<int64_t>(count); status = Status::OK; break;
            }
            uint64_t expected = 0;
            if (!Packet(read, true, integer, p, t, 0, 1, true) || integer < 0) return Status::INVALID_ARGUMENT;
            const auto parsed = std::from_chars(t[0].data(), t[0].data() + t[0].size(), expected);
            if (parsed.ec != std::errc{} || parsed.ptr != t[0].data() + t[0].size() || expected != count || static_cast<uint64_t>(integer) > count) return Status::INVALID_ARGUMENT;
            const size_t end = (std::min)(count, static_cast<size_t>(integer) + (request.index == 55 ? 8U : 128U));
            for (size_t index = static_cast<size_t>(integer); index < end; ++index) {
                if (request.index == 56) out.text.push_back(std::to_string((*indices)[index]));
                else {
                    const auto& v = (*vertices)[index];
                    out.values.insert(out.values.end(), {v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y, v.normal.z,
                        v.tangent.x, v.tangent.y, v.tangent.z, v.uv.x, v.uv.y, v.color.x, v.color.y, v.color.z, v.color.w});
                }
            }
            status = Status::OK; break;
        }
        case 9:
            if (skinned || !Packet(read, false, integer, p, t, 0, 0)) return Status::INVALID_ARGUMENT;
            { const bool enabled = mesh->enabled; view.mesh.Clear(); mesh->enabled = enabled; }
            mesh->mesh = nullptr; mesh->meshPath.clear(); mesh->meshPathDirty = false; status = Status::OK; break;
        case 11: {
            if (skinned || !Packet(read, false, integer, p, t, 0, 1)) return Status::INVALID_ARGUMENT;
            std::string path; out.integer = AssetPath(t[0], SceneAssetKind::Material, path);
            if (out.integer) view.mesh.SetProceduralMaterial(path);
            status = Status::OK; break;
        }
        default: return Status::INVALID_ARGUMENT;
        }
        if (!skinned && !read && status == Status::OK)
            NotifyMeshRendererChanged(*ownerScene, ownerId);
        break;
    }

    default: return Status::UNSUPPORTED;
    }
    if (status != Status::OK) return status;
    if (!std::all_of(out.values.begin(), out.values.end(), [](float value) { return std::isfinite(value); })) return Status::INVALID_ARGUMENT;
    result = std::move(out); return Status::OK;
}
} /// @note namespace fbzz::scene::managed_detail

