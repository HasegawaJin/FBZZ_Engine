/// @file    ManagedParticleServices.cpp
/// @brief   粒子設定の組検証・typed asset 解決と既存 ParticleProxy の転送。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include "ManagedParticleServices.hpp"
#include "ManagedServicesBridge.hpp"
#include "ManagedObjectControls.hpp"
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/SceneAssetReferences.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/Model.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
namespace fbzz::scene::managed_detail {
namespace {
bool AssetPath(const std::string& reference, SceneAssetKind kind, std::string& path)
{
    if (reference.empty()) { path.clear(); return true; }
    if (!reference.starts_with("guid:") || reference.find('\0') != std::string::npos) return false;
    path = asset::AssetDatabase::PathFromGuid(reference.substr(5));
    const auto classified = ClassifySceneAssetReference(path);
    if (path.empty() || !classified || classified->kind != kind) return false;
    if (kind == SceneAssetKind::Material) return asset::AssetManager::LoadAndGet<asset::MaterialAsset>(path) != nullptr;
    return asset::AssetManager::LoadAndGet<asset::Model>(path) != nullptr;
}
ManagedBridgeStatus Reference(const std::string& path, std::vector<std::string>& text)
{
    if (path.empty()) { text = {""}; return ManagedBridgeStatus::OK; }
    const auto resolved = asset::AssetManager::ResolveAssetPath(path);
    const auto guid = asset::AssetDatabase::TryGetGuidFromPath(resolved);
    if (guid.empty()) return ManagedBridgeStatus::NOT_FOUND;
    text = {"guid:" + guid}; return ManagedBridgeStatus::OK;
}
bool Enum(int64_t value, int maximum) { return value >= 0 && value <= maximum; }
} /// @note namespace
bool IsParticleServiceRead(uint32_t selector)
{
    if (selector >= 13 && selector <= 49) return (selector & 1U) != 0;
    if (selector == 51 || (selector >= 52 && selector <= 66 && !(selector & 1U))) return true;
    return selector >= 68 && selector <= 72;
}
ManagedBridgeStatus RunParticleServices(Script& view, GameObject& target, const ManagedBridgeRequest& request,
    int64_t integer, std::span<const float> p, std::span<const std::string> t, ManagedServiceResult& result)
{
    if (request.op != ManagedBridgeOp::SERVICE_PARTICLE) return ManagedBridgeStatus::UNSUPPORTED;
    if (request.flags > 1 || !std::all_of(p.begin(), p.end(), [](float value) { return std::isfinite(value); })) return ManagedBridgeStatus::INVALID_ARGUMENT;
    auto* row = target.GetComponent<ParticleEmitter>();
    if (!row || ManagedObjectControls::IsPendingComponentRemoval(target, static_cast<uint32_t>(ManagedNativeComponentKind::PARTICLE_SYSTEM))) return ManagedBridgeStatus::NOT_FOUND;
    const bool read = request.flags == 1;
    if (read != IsParticleServiceRead(request.index)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (!read && !Script::IsInPlayMode()) return ManagedBridgeStatus::INVALID_SCOPE;
    const auto args = [&](size_t floats, size_t strings = 0, bool hasInteger = false) {
        return p.size() == floats && t.size() == strings && (hasInteger || integer == 0);
    };
    const auto vector = [&]() { return math::Vector3{p[0], p[1], p[2]}; };
    auto& settings = row->settings;
    ManagedServiceResult output;
    if (read) {
        if (!args(0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        const auto v3 = [&](const math::Vector3& value) { output.values = {value.x, value.y, value.z}; };
        switch (request.index) {
        case 13: v3(settings.emitPosition); break;
        case 15: v3(settings.emitVelocity); break;
        case 17: output.values = {settings.velocitySpread}; break;
        case 19: v3(settings.GravityAcceleration()); break;
        case 21: output.values = {settings.colorStart.x, settings.colorStart.y, settings.colorStart.z, settings.colorStart.w,
            settings.colorEnd.x, settings.colorEnd.y, settings.colorEnd.z, settings.colorEnd.w}; break;
        case 23: output.values = {settings.sizeStart, settings.sizeEnd}; break;
        case 25: output.integer = view.particle.GetMaxParticles(); if (output.integer < 1) return ManagedBridgeStatus::INVALID_ARGUMENT; break;
        case 27: output.integer = (settings.loop ? 1 : 0) | (settings.clearOnStop ? 2 : 0); output.values = {settings.duration}; break;
        case 29:
            output.integer = static_cast<int>(settings.shape); if (!Enum(output.integer, 4)) return ManagedBridgeStatus::INVALID_ARGUMENT; break;
        case 31: output.values = {settings.sphereRadius}; break;
        case 33: output.values = {settings.coneRadius, settings.coneAngleDegrees}; break;
        case 35: v3(settings.boxExtents); break;
        case 37: {
            const auto status = Reference(settings.meshShapePath, output.text); if (status != ManagedBridgeStatus::OK) return status;
            output.integer = settings.meshShapeIndex;
            output.values = {settings.meshShapeScale, settings.meshShapeNormalVelocity, settings.meshShapeFollowSkinnedAnimation ? 1.0f : 0.0f}; break;
        }
        case 39: output.values = {settings.meshShapeNormalVelocity}; break;
        case 41:
            output.integer = static_cast<int>(settings.sortMode); if (!Enum(output.integer, 1)) return ManagedBridgeStatus::INVALID_ARGUMENT; break;
        case 43:
            output.integer = static_cast<int>(view.particle.GetSimulationMode()); if (!Enum(output.integer, 1)) return ManagedBridgeStatus::INVALID_ARGUMENT; break;
        case 45:
            output.integer = static_cast<int>(settings.simulationSpace); if (!Enum(output.integer, 1)) return ManagedBridgeStatus::INVALID_ARGUMENT; break;
        case 47:
            output.integer = static_cast<int>(settings.renderMode); if (!Enum(output.integer, 3)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            output.values = {settings.stretchedVelocityScale}; break;
        case 49: {
            const auto mode = static_cast<int>(settings.collisionMode), response = static_cast<int>(settings.collisionResponse);
            if (!Enum(mode, 3) || !Enum(response, 2)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            output.integer = mode | (response << 8); output.values = {settings.collisionRadius, settings.collisionBounciness}; break;
        }
        case 51: output.integer = view.particle.GetCollisionCount(); break;
        case 52: output.values = {settings.rateOverDistance}; break;
        case 54: output.integer = settings.prewarm; break;
        case 56: output.integer = settings.subEmitterBurstCount; output.text = {settings.birthSubEmitter, settings.deathSubEmitter, settings.collisionSubEmitter}; break;
        case 58: output.values = {settings.flowCoupling}; break;
        case 60: output.values = {settings.angularVelocityMin, settings.angularVelocityMax}; break;
        case 62: {
            const auto* noise = settings.FindLocalForce(FlowFieldType::Curl);
            output.values = noise ? std::vector<float>{noise->strength, noise->noiseFrequency, noise->noiseSpeed} : std::vector<float>{0, 0.5f, 1}; break;
        }
        case 64: output.integer = settings.receiveFlowFields; break;
        case 66: output.integer = view.particle.GetFlowFieldChannels(); break;
        case 68: output.integer = view.particle.IsGpuSimulated(); break;
        case 69:
            output.integer = static_cast<int>(view.particle.GetGpuFallbackReason()); if (!Enum(output.integer, 12)) return ManagedBridgeStatus::INVALID_ARGUMENT; break;
        case 70: output.text = {view.particle.GetGpuFallbackField()}; break;
        case 71: output.text = {view.particle.GetGpuFallbackDescription()}; break;
        case 72: {
            const auto status = Reference(settings.materialPath, output.text); if (status != ManagedBridgeStatus::OK) return status; break;
        }
        default: return ManagedBridgeStatus::UNSUPPORTED;
        }
    } else switch (request.index) {
    case 14: case 16: case 20:
        if (!args(3)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (request.index == 14) view.particle.SetEmitPosition(vector());
        if (request.index == 16) view.particle.SetEmitVelocity(vector());
        if (request.index == 20) view.particle.SetGravity(vector());
        break;
    case 18: case 32: case 40: case 53: case 59:
        if (!args(1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (request.index == 18) view.particle.SetVelocitySpread(p[0]);
        if (request.index == 32) view.particle.SetSphereShape(p[0]);
        if (request.index == 40) view.particle.SetMeshShapeNormalVelocity(p[0]);
        if (request.index == 53) view.particle.SetRateOverDistance(p[0]);
        if (request.index == 59) view.particle.SetVelocityDamping(p[0]);
        break;
    case 22:
        if (!args(8)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.particle.SetColor({p[0], p[1], p[2], p[3]}, {p[4], p[5], p[6], p[7]}); break;
    case 24: case 34: case 61:
        if (!args(2)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (request.index == 24) view.particle.SetSize(p[0], p[1]);
        if (request.index == 34) view.particle.SetConeShape(p[0], p[1]);
        if (request.index == 61) view.particle.SetAngularVelocity(p[0], p[1]);
        break;
    case 26:
        if (!args(0, 0, true) || integer < INT32_MIN || integer > INT32_MAX) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.particle.SetMaxParticles(static_cast<int>(integer)); break;
    case 28:
        if (!args(1, 0, true) || !Enum(integer, 3)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.particle.SetPlayback((integer & 1) != 0, p[0], (integer & 2) != 0); break;
    case 30: case 42: case 44: case 46:
        if (!args(0, 0, true) || !Enum(integer, request.index == 30 ? 4 : 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (request.index == 30) view.particle.SetShape(static_cast<ParticleEmitterShape>(integer));
        if (request.index == 42) view.particle.SetSortMode(static_cast<ParticleSortMode>(integer));
        if (request.index == 44) view.particle.SetSimulationMode(static_cast<ParticleSimulationMode>(integer));
        if (request.index == 46) view.particle.SetSimulationSpace(static_cast<ParticleSimulationSpace>(integer));
        break;
    case 36:
        if (!args(3)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.particle.SetBoxShape(vector()); break;
    case 38: {
        if (!args(3, 1, true) || integer < INT32_MIN || integer > INT32_MAX || (p[2] != 0 && p[2] != 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        std::string path; output.integer = AssetPath(t[0], SceneAssetKind::Model, path);
        if (output.integer && !path.empty()) {
            const auto* model = asset::AssetManager::LoadAndGet<asset::Model>(path);
            if (!model || (integer >= 0 && (static_cast<uint64_t>(integer) >= model->meshes.size() || !model->meshes[static_cast<size_t>(integer)]))) output.integer = 0;
        }
        if (output.integer) view.particle.SetMeshShape(path, static_cast<int>(integer), p[0], p[2] != 0, p[1]);
        break;
    }
    case 48:
        if (!args(1, 0, true) || !Enum(integer, 3)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.particle.SetRenderMode(static_cast<ParticleRenderMode>(integer), p[0]); break;
    case 50: {
        if (!args(2, 0, true) || integer < 0 || integer > 0x203 || (integer & 0xFC) || !Enum(integer >> 8, 2)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.particle.SetCollision(static_cast<ParticleCollisionMode>(integer & 3), static_cast<ParticleCollisionResponse>(integer >> 8), p[0], p[1]); break;
    }
    case 55: case 65:
        if (!args(0, 0, true) || !Enum(integer, 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (request.index == 55) view.particle.SetPrewarm(integer != 0); else view.particle.SetReceiveFlowFields(integer != 0);
        break;
    case 57:
        if (!args(0, 3, true) || integer < INT32_MIN || integer > INT32_MAX
            || std::any_of(t.begin(), t.end(), [](const auto& name) { return name.find('\0') != std::string::npos; })) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.particle.SetSubEmitters(t[0], t[1], t[2], static_cast<int>(integer)); break;
    case 63:
        if (!args(3)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.particle.SetNoise(p[0], p[1], p[2]); break;
    case 67:
        if (!args(0, 0, true) || integer < 0 || integer > UINT32_MAX) return ManagedBridgeStatus::INVALID_ARGUMENT;
        view.particle.SetFlowFieldChannels(static_cast<uint32_t>(integer)); break;
    case 73: {
        if (!args(0, 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        std::string path; output.integer = AssetPath(t[0], SceneAssetKind::Material, path);
        if (output.integer) settings.materialPath = std::move(path);
        break;
    }
    default: return ManagedBridgeStatus::UNSUPPORTED;
    }
    if (!std::all_of(output.values.begin(), output.values.end(), [](float value) { return std::isfinite(value); })) return ManagedBridgeStatus::INVALID_ARGUMENT;
    result = std::move(output); return ManagedBridgeStatus::OK;
}
} /// @note namespace fbzz::scene::managed_detail
