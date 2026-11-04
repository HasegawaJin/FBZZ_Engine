/// @file    ManagedRuntimeServices.cpp
/// @brief   値検証・実 Component 操作・所有要求 cleanup と deferred 終了/Scene intent。
/// @author  Hasegawa Jin
/// @date    2026-10-05
#include "ManagedRuntimeServices.hpp"
#include "ManagedServicesBridge.hpp"
#include "ManagedIntegerParsing.hpp"
#include "ManagedObjectControls.hpp"
#include <Engine/Core/Application.hpp>
#include <Engine/Core/Cursor.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneManager.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptRuntime.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace fbzz::scene::managed_detail {
namespace {
bool Flag(int64_t value) { return value == 0 || value == 1; }
bool Nonnegative(float value) { return std::isfinite(value) && value >= 0; }
bool Policy(std::span<const float> values)
{
    return values.size() == 2 && std::isfinite(values[0]) && values[0] >= 0 && values[0] <= 2
        && std::trunc(values[0]) == values[0] && (values[1] == 0 || values[1] == 1);
}

} /// @note namespace

struct ManagedRuntimeServices::Impl {
    struct CursorRecord { uint64_t generation; uint64_t owner; uint64_t scene; EntityID entity; core::CursorRequestId request; };
    int64_t nextCursor = 1;
    std::unordered_map<int64_t, CursorRecord> cursors;
};
ManagedRuntimeServices::ManagedRuntimeServices() : m_impl(std::make_unique<Impl>()) {}
ManagedRuntimeServices::~ManagedRuntimeServices()
{
    for (const auto& [token, record] : m_impl->cursors) core::Cursor::Release(record.request);
}
void ManagedRuntimeServices::CancelOwner(uint64_t owner)
{
    for (auto it = m_impl->cursors.begin(); it != m_impl->cursors.end();) {
        if (it->second.owner != owner) { ++it; continue; }
        core::Cursor::Release(it->second.request); it = m_impl->cursors.erase(it);
    }
}
void ManagedRuntimeServices::RetireGeneration(uint64_t generation)
{
    for (auto it = m_impl->cursors.begin(); it != m_impl->cursors.end();) {
        if (it->second.generation != generation) { ++it; continue; }
        core::Cursor::Release(it->second.request); it = m_impl->cursors.erase(it);
    }
}
ManagedBridgeStatus ManagedRuntimeServices::Run(Scene& scene, Script& caller, GameObject& target,
    const ManagedBridgeRequest& request, int64_t integer, std::span<const float> p,
    std::span<const std::string> t, ManagedServiceResult& result)
{
    const bool read = request.flags == 1;
    if (request.flags > 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
    ManagedServiceResult output;
    switch (request.op) {
    case ManagedBridgeOp::SERVICE_APPLICATION:
        if (!p.empty() || !t.empty() || integer || request.index > 6 || read != (request.index != 4)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        switch (request.index) {
        case 0: output.integer = Script::IsInPlayMode(); break;
        case 1: output.integer = core::Application::Get().IsRunning(); break;
        case 2: output.integer = core::Application::Get().GetWindowWidth(); break;
        case 3: output.integer = core::Application::Get().GetWindowHeight(); break;
        case 5: output.integer = core::Application::Get().IsEditorHosted(); break;
        case 6:
            /// @note Development も assert 有効。managed DLL のビルド構成から host の構成を推測しない。
#ifdef NDEBUG
            output.integer = 0;
#else
            output.integer = 1;
#endif
            break;
        case 4:
            if (!ScriptRuntime::RequestQuit(scene.GetRenderSceneGeneration())) return ManagedBridgeStatus::INVALID_SCOPE;
            break;
        }
        break;
    case ManagedBridgeOp::SERVICE_SCENE: {
        if (request.index == 2 || request.index == 3) {
            if (!read || integer || p.size() != 4 || !t.empty() || p[3] < 0 || !std::all_of(p.begin(), p.end(), [](float value) { return std::isfinite(value); })) return ManagedBridgeStatus::INVALID_ARGUMENT;
            math::Vector3 position, normal;
            const math::Vector3 reference{p[0], p[1], p[2]};
            output.integer = request.index == 2
                ? caller.scene.TryGetTerrainFloorAt(reference, p[3], position, normal)
                : caller.scene.TryGetTerrainCeilingAt(reference, p[3], position, normal);
            if (output.integer) output.values = {position.x, position.y, position.z, normal.x, normal.y, normal.z};
            break;
        }
        if (!p.empty() || integer || request.index > 1 || read != (request.index == 0) || t.size() != (read ? 0U : 1U)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        SceneManager* manager = ScriptRuntime::GetCurrent().sceneManager;
        if (!manager || manager->GetActive() != &scene) return ManagedBridgeStatus::INVALID_SCOPE;
        if (read) output.text.push_back(manager->ActiveSceneName());
        else {
            if (!Script::IsInPlayMode()) return ManagedBridgeStatus::INVALID_SCOPE;
            if (t[0].empty() || t[0].find('\0') != std::string::npos) return ManagedBridgeStatus::INVALID_ARGUMENT;
            output.integer = manager->LoadScene(t[0]);
        }
        break;
    }
    case ManagedBridgeOp::SERVICE_CURSOR: {
        if (request.index > 11 || read != (request.index >= 3)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (request.index >= 4) {
            if (integer || !p.empty() || !t.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 4) output.integer = core::Cursor::IsVisible();
            else if (request.index == 5) output.integer = static_cast<int>(core::Cursor::GetLockMode());
            else if (request.index == 6) output.integer = static_cast<int>(core::Cursor::GetShape());
            else {
                const auto state = core::Cursor::GetState();
                switch (request.index) {
                case 7: output.integer = state.effective.visible; break;
                case 8: output.integer = static_cast<int>(state.effective.lockMode); break;
                case 9: output.integer = static_cast<int>(state.inputOwner); break;
                case 10: output.integer = static_cast<int>(state.suspendedReason); break;
                case 11: output.integer = state.windowActive; break;
                }
            }
            break;
        }
        if (!Script::IsInPlayMode()) return ManagedBridgeStatus::INVALID_SCOPE;
        if (request.index == 0) {
            int priority = 0;
            if (!Policy(p) || t.size() != 1 || !ParseInteger(t[0], priority) || std::bit_cast<uint64_t>(integer) != request.ownerId
                || !caller.scene.IsActiveAndEnabled() || caller.RequirementsBlocked()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (m_impl->nextCursor == std::numeric_limits<int64_t>::max()) return ManagedBridgeStatus::CAPACITY_LIMIT;
            const auto id = core::Cursor::Push({static_cast<core::CursorLockMode>(static_cast<int>(p[0])), p[1] != 0}, priority,
                target.GetID().index + 1, caller.GetTypeName());
            const int64_t token = m_impl->nextCursor++;
            m_impl->cursors.emplace(token, Impl::CursorRecord{request.generation, request.ownerId, scene.GetRenderSceneGeneration(), target.GetID(), id});
            output.integer = token;
        } else {
            if (integer <= 0 || t.size() != 1 || (request.index == 1 ? !Policy(p) : !p.empty())) return ManagedBridgeStatus::INVALID_ARGUMENT;
            const auto found = m_impl->cursors.find(integer);
            const bool valid = found != m_impl->cursors.end() && found->second.owner == request.ownerId
                && found->second.generation == request.generation && found->second.scene == scene.GetRenderSceneGeneration()
                && found->second.entity == target.GetID() && core::Cursor::IsRequestActive(found->second.request);
            output.integer = valid ? 1 : 0;
            if (valid && request.index == 1) core::Cursor::UpdateRequest(found->second.request,
                {static_cast<core::CursorLockMode>(static_cast<int>(p[0])), p[1] != 0});
            if (request.index != 3 && found != m_impl->cursors.end() && (!valid || request.index == 2)) {
                if (found->second.owner != request.ownerId || found->second.generation != request.generation) return ManagedBridgeStatus::INVALID_SCOPE;
                core::Cursor::Release(found->second.request); m_impl->cursors.erase(found);
            }
        }
        break;
    }
    case ManagedBridgeOp::SERVICE_LIGHT: {
        constexpr auto kind = static_cast<uint32_t>(ManagedNativeComponentKind::LIGHT);
        auto* light = target.GetComponent<LightComponent>();
        if (!light || ManagedObjectControls::IsPendingComponentRemoval(target, kind)) return ManagedBridgeStatus::NOT_FOUND;
        if (!t.empty() || request.index > 12 || read != (request.index <= 6)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (read) {
            if (!p.empty() || integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
            switch (request.index) {
            case 0: output.values = {light->color.x, light->color.y, light->color.z}; break;
            case 1: output.integer = static_cast<int>(light->type); break;
            case 2: output.values = {light->intensity}; break;
            case 3: output.values = {light->range}; break;
            case 4: output.values = {light->innerCone}; break;
            case 5: output.values = {light->outerCone}; break;
            case 6: output.integer = light->enabled; break;
            }
        } else {
            switch (request.index) {
            case 7:
                if (integer || p.size() != 3 || !std::all_of(p.begin(), p.end(), Nonnegative)) return ManagedBridgeStatus::INVALID_ARGUMENT;
                light->color = {p[0], p[1], p[2]}; break;
            case 8:
                if (!p.empty() || integer < 0 || integer > 5) return ManagedBridgeStatus::INVALID_ARGUMENT;
                light->type = static_cast<LightComponent::Type>(integer); break;
            case 9:
            case 10:
                if (integer || p.size() != 1 || !Nonnegative(p[0])) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (request.index == 9) light->intensity = p[0]; else light->range = p[0]; break;
            case 11:
                if (integer || p.size() != 2 || !Nonnegative(p[0]) || !Nonnegative(p[1]) || p[0] > p[1] || p[1] > 180)
                    return ManagedBridgeStatus::INVALID_ARGUMENT;
                light->innerCone = p[0]; light->outerCone = p[1]; break;
            case 12:
                if (!p.empty() || !Flag(integer)) return ManagedBridgeStatus::INVALID_ARGUMENT;
                light->enabled = integer != 0; break;
            }
        }
        break;
    }
    case ManagedBridgeOp::SERVICE_PARTICLE: {
        constexpr auto kind = static_cast<uint32_t>(ManagedNativeComponentKind::PARTICLE_SYSTEM);
        auto* particle = target.GetComponent<ParticleEmitter>();
        if (!particle || ManagedObjectControls::IsPendingComponentRemoval(target, kind)) return ManagedBridgeStatus::NOT_FOUND;
        if (!t.empty() || request.index > 12 || read != (request.index <= 5)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (read) {
            if (!p.empty() || integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
            switch (request.index) {
            case 0: output.integer = particle->settings.enabled; break;
            case 1: output.integer = particle->settings.playing; break;
            case 2: output.integer = static_cast<int64_t>(particle->runtime.particles.size()); break;
            case 3: output.values = {particle->settings.emitRate}; break;
            case 4: output.values = {particle->settings.lifetime}; break;
            case 5: output.values = {particle->runtime.playTime}; break;
            }
        } else {
            if (!Script::IsInPlayMode()) return ManagedBridgeStatus::INVALID_SCOPE;
            switch (request.index) {
            case 6:
            case 7:
            case 10:
                if (!p.empty() || !Flag(integer)) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (request.index == 6) particle->Play(integer != 0);
                else if (request.index == 7) particle->Stop(integer != 0);
                else particle->settings.enabled = integer != 0;
                break;
            case 8:
                if (!p.empty() || integer < 0 || integer > std::numeric_limits<int>::max()
                    || particle->runtime.burstPending < 0 || integer > std::numeric_limits<int>::max() - particle->runtime.burstPending)
                    return ManagedBridgeStatus::INVALID_ARGUMENT;
                particle->Burst(static_cast<int>(integer)); break;
            case 9:
                if (!p.empty() || integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                particle->ClearParticles(); break;
            case 11:
            case 12:
                if (integer || p.size() != 1 || !Nonnegative(p[0]) || (request.index == 12 && p[0] < 0.01f)) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (request.index == 11) particle->settings.emitRate = p[0]; else particle->settings.lifetime = p[0]; break;
            }
        }
        break;
    }
    default: return ManagedBridgeStatus::UNSUPPORTED;
    }
    result = std::move(output);
    return ManagedBridgeStatus::OK;
}
} /// @note namespace fbzz::scene::managed_detail
