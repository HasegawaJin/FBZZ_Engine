/// @file    ManagedWorldFamilyServices.cpp
/// @brief   Terrain/Water/VFX/Sequence の実行境界で packet と権限を検証し、実 row/proxy へ転送する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include "ManagedWorldFamilyServices.hpp"
#include "ManagedServicesBridge.hpp"
#include "ManagedObjectControls.hpp"
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/WaterInteraction.hpp>
#include <Engine/Scene/Components/VFXComponent.hpp>
#include <Engine/Scene/Components/SequencePlayerComponent.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/SequenceAsset.hpp>
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
namespace fbzz::scene::managed_detail {
namespace {
bool Flag(int64_t value) { return value == 0 || value == 1; }
bool ParseIndex(const std::string& text, int& value)
{
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
bool MaterialPath(const std::string& reference, std::string& path)
{
    if (reference.empty()) { path.clear(); return true; }
    if (reference.rfind("guid:", 0) != 0) return false;
    path = asset::AssetDatabase::PathFromGuid(reference.substr(5));
    if (path.empty()) return false;
    return asset::AssetManager::Get<asset::MaterialAsset>(asset::AssetManager::Load<asset::MaterialAsset>(path)) != nullptr;
}
bool DoubleText(double value, std::vector<std::string>& output)
{
    if (!std::isfinite(value)) return false;
    char buffer[64]; const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (result.ec != std::errc{}) return false;
    output.emplace_back(buffer, result.ptr); return true;
}
} /// @note namespace
ManagedBridgeStatus RunWorldFamilyServices(Scene& scene, Script& view, GameObject& target, GameObject* related,
    const ManagedBridgeRequest& request, int64_t integer, std::span<const float> p,
    std::span<const std::string> t, ManagedServiceResult& result, bool fixedUpdate)
{
    if (request.flags > 1 || !std::all_of(p.begin(), p.end(), [](float value) { return std::isfinite(value); }))
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    const bool read = request.flags == 1;
    ManagedServiceResult output;
    const auto grid = std::bit_cast<uint64_t>(integer);
    const int x = std::bit_cast<int32_t>(static_cast<uint32_t>(grid)), z = std::bit_cast<int32_t>(static_cast<uint32_t>(grid >> 32));
    switch (request.op) {
    case ManagedBridgeOp::SERVICE_TERRAIN: {
        auto* terrain = target.GetComponent<TerrainComponent>();
        if (!terrain || ManagedObjectControls::IsPendingComponentRemoval(target, static_cast<uint32_t>(ManagedNativeComponentKind::TERRAIN))) return ManagedBridgeStatus::NOT_FOUND;
        if (related || request.index > 17) return ManagedBridgeStatus::INVALID_ARGUMENT;
        const bool expectedRead = request.index == 0 || (request.index >= 2 && request.index <= 8)
            || request.index == 10 || request.index == 11 || request.index == 13 || request.index == 16 || request.index == 17;
        if (read != expectedRead) return ManagedBridgeStatus::INVALID_ARGUMENT;
        switch (request.index) {
        case 0: case 2: case 3: case 4: case 5: case 10:
            if (integer || !p.empty() || !t.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 0) output.integer = terrain->enabled;
            if (request.index == 2) output.integer = terrain->columns;
            if (request.index == 3) output.integer = terrain->rows;
            if (request.index == 4) output.values = {terrain->cellSize};
            if (request.index == 5) output.values = {terrain->maxHeight};
            if (request.index == 10) output.integer = terrain->LayerCount();
            break;
        case 1:
            if (!Flag(integer) || !p.empty() || !t.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            terrain->enabled = integer != 0; break;
        case 6: case 7: case 8: {
            if (integer || p.size() != 2 || !t.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 8) {
                math::Vector3 position, normal;
                output.integer = view.terrain.TryGetSurfaceWorld(p[0], p[1], position, normal);
                if (output.integer) output.values = {position.x, position.y, position.z, normal.x, normal.y, normal.z};
                break;
            }
            if (terrain->columns < 2 || terrain->rows < 2 || (!terrain->heightData.empty() && terrain->heightData.size() != terrain->VertexCount())
                || !std::isfinite(terrain->cellSize) || terrain->cellSize <= 0 || !std::isfinite(terrain->maxHeight)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 6) output.values = {terrain->GetHeightAt(p[0], p[1])};
            else if (request.index == 7) {
                const auto normal = terrain->GetNormalAt((std::clamp)(p[0], 0.0f, (terrain->columns - 1) * terrain->cellSize),
                    (std::clamp)(p[1], 0.0f, (terrain->rows - 1) * terrain->cellSize));
                output.values = {normal.x, normal.y, normal.z};
            }
            break;
        }
        case 16: case 17: {
            if (integer || p.size() != 4 || !t.empty() || p[3] < 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
            math::Vector3 position, normal;
            const math::Vector3 reference{p[0], p[1], p[2]};
            output.integer = request.index == 16
                ? view.terrain.TryGetFloorWorld(reference, p[3], position, normal)
                : view.terrain.TryGetCeilingWorld(reference, p[3], position, normal);
            if (output.integer) output.values = {position.x, position.y, position.z, normal.x, normal.y, normal.z};
            break;
        }
        case 9:
            if (!fixedUpdate || !Script::IsInPlayMode()) return ManagedBridgeStatus::INVALID_SCOPE;
            if (p.size() != 1 || !t.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            output.integer = terrain->SetHeightAtGrid(x, z, p[0]); break;
        case 11: case 12: {
            int layer = 0;
            if (t.size() != 1 || !ParseIndex(t[0], layer) || p.size() != (request.index == 11 ? 0U : 1U)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 11) output.values = {terrain->GetLayerWeightAtGrid(x, z, layer)};
            else {
                if (p[0] < 0 || p[0] > 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = terrain->PaintLayerAtGrid(x, z, layer, p[0]);
            }
            break;
        }
        case 13:
            if (!p.empty() || !t.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            output.integer = terrain->IsHoleCell(x, z); break;
        case 14: {
            if (!fixedUpdate || !Script::IsInPlayMode()) return ManagedBridgeStatus::INVALID_SCOPE;
            if (p.size() != 1 || (p[0] != 0 && p[0] != 1) || !t.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            const bool old = terrain->IsHoleCell(x, z);
            output.integer = terrain->SetHoleCell(x, z, p[0] != 0);
            if (output.integer && old != (p[0] != 0)) terrain->RequestHoleRebuild();
            break;
        }
        case 15: {
            if (!p.empty() || t.size() != 1 || integer < 0 || integer > std::numeric_limits<int>::max()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            std::string path; if (!MaterialPath(t[0], path)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            output.integer = terrain->SetLayerMaterial(static_cast<int>(integer), path); break;
        }
        }
        break;
    }
    case ManagedBridgeOp::SERVICE_WATER: {
        auto* water = target.GetComponent<WaterComponent>();
        if (!water || ManagedObjectControls::IsPendingComponentRemoval(target, static_cast<uint32_t>(ManagedNativeComponentKind::WATER))) return ManagedBridgeStatus::NOT_FOUND;
        if (related || !t.empty() || request.index > 13) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (read != (request.index == 0 || request.index == 2 || request.index == 4 || request.index == 6 || request.index == 8 || request.index == 9 || request.index == 10)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        switch (request.index) {
        case 0: case 2: case 4: case 6: case 8:
            if (integer || !p.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 0) output.integer = water->enabled;
            if (request.index == 2) output.values = {water->waveAmplitudeScale};
            if (request.index == 4) output.integer = water->enableGerstnerWaves;
            if (request.index == 6) output.integer = water->buoyancyEnabled;
            if (request.index == 8) output.values = {water->current.x, water->current.y};
            break;
        case 1: case 5: case 7:
            if (!Flag(integer) || !p.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 1) water->enabled = integer != 0;
            if (request.index == 5) water->enableGerstnerWaves = integer != 0;
            if (request.index == 7) water->buoyancyEnabled = integer != 0;
            break;
        case 3:
            if (integer || p.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.water.SetWaveAmplitudeScale(p[0]); break;
        case 9: case 10:
            if (integer || p.size() != 3) return ManagedBridgeStatus::INVALID_ARGUMENT;
            output.values = {request.index == 9 ? view.water.GetSurfaceHeightWorld(p[0], p[1], p[2]) : view.water.GetSurfaceHeightLocal(p[0], p[1], p[2])}; break;
        case 11: case 12:
            if (!Script::IsInPlayMode()) return ManagedBridgeStatus::INVALID_SCOPE;
            if (integer || p.size() != 4 || p[3] < 0 || p[3] > 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 11) view.water.AddRipple({p[0], p[1], p[2]}, p[3]);
            else view.water.Splash({p[0], p[1], p[2]}, p[3]);
            if (request.index == 11)
                static_cast<void>(QueueWaterSurfaceInteraction(scene, target.GetID(), {p[0], p[1], p[2]}, p[3]));
            break;
        case 13: {
            if (!Script::IsInPlayMode()) return ManagedBridgeStatus::INVALID_SCOPE;
            if (integer < 0 || integer > 2 || p.size() != 9 || p[6] <= 0 || p[6] > 32
                || p[7] <= 0 || p[7] > 1 || p[8] <= 0 || p[8] > 60)
                return ManagedBridgeStatus::INVALID_ARGUMENT;
            const WaterInteractionImpulse impulse{static_cast<WaterInteractionKind>(integer),
                {p[0], p[1], p[2]}, {p[3], p[4], p[5]}, p[6], p[7], p[8]};
            output.integer = QueueWaterInteraction(scene, target.GetID(), impulse);
            break;
        }
        }
        break;
    }
    case ManagedBridgeOp::SERVICE_VFX: {
        auto* vfx = target.GetComponent<VFXComponent>();
        if (!vfx || ManagedObjectControls::IsPendingComponentRemoval(target, static_cast<uint32_t>(ManagedNativeComponentKind::VFX))) return ManagedBridgeStatus::NOT_FOUND;
        if (related || request.index > 11 || read != (request.index == 0 || request.index == 2 || (request.index >= 4 && request.index <= 6))) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (request.index >= 7 && !Script::IsInPlayMode()) return ManagedBridgeStatus::INVALID_SCOPE;
        switch (request.index) {
        case 0: case 2: case 4: case 5: case 6:
            if (integer || !p.empty() || !t.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 0) output.integer = vfx->enabled;
            if (request.index == 2) output.values = {vfx->speed};
            if (request.index == 4) output.integer = vfx->playing;
            if (request.index == 5) output.values = {vfx->time};
            if (request.index == 6) output.values = {vfx->duration > 0 ? vfx->duration : vfx->resolvedDuration};
            break;
        case 1:
            if (!Flag(integer) || !p.empty() || !t.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            vfx->enabled = integer != 0; break;
        case 3:
            if (integer || p.size() != 1 || !t.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.vfx.SetSpeed(p[0]); break;
        case 7:
            if (!Flag(integer) || !p.empty() || !t.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.vfx.Play(integer != 0); break;
        case 8: case 9: case 10:
            if (integer || !p.empty() || !t.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 8) vfx->Pause();
            if (request.index == 9) vfx->Resume();
            if (request.index == 10) vfx->Stop();
            break;
        case 11:
            if (integer || !p.empty() || t.size() != 1 || t[0].empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            vfx->Trigger(t[0]); break;
        }
        break;
    }
    case ManagedBridgeOp::SERVICE_SEQUENCE: {
        auto* sequence = target.GetComponent<SequencePlayerComponent>();
        if (!sequence || ManagedObjectControls::IsPendingComponentRemoval(target, static_cast<uint32_t>(ManagedNativeComponentKind::SEQUENCE))) return ManagedBridgeStatus::NOT_FOUND;
        if (request.index > 14 || (related && request.index != 13)
            || read != (request.index == 0 || request.index == 2 || (request.index >= 4 && request.index <= 7))) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (request.index >= 8 && request.index <= 12 && !Script::IsInPlayMode()) return ManagedBridgeStatus::INVALID_SCOPE;
        switch (request.index) {
        case 0: case 2: case 4: case 5: case 6: case 7:
            if (integer || !p.empty() || !t.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 0) output.integer = sequence->enabled;
            if (request.index == 2) output.values = {sequence->speed};
            if (request.index == 4) output.integer = sequence->playing;
            if (request.index == 5) output.integer = sequence->paused;
            if (request.index == 6 && !DoubleText(sequence->time, output.text)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 7) {
                const auto* source = sequence->authoringSequence.get();
                if (!source && !sequence->sequencePath.empty()) {
                    /// @note queryはasset import/pinを開始しない。SequenceSystemまたは明示loaderが取得済みの値だけを読む。
                    const auto& store = asset::AssetStore<asset::SequenceAsset>::Get();
                    const auto found = store.cache.find(asset::AssetManager::CanonicalKey(sequence->sequencePath));
                    source = found == store.cache.end() ? nullptr : asset::AssetManager::Get<asset::SequenceAsset>(found->second);
                    if (!source) return ManagedBridgeStatus::NOT_FOUND;
                }
                if (!DoubleText(source ? source->GetDurationSeconds() : 0, output.text)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            }
            break;
        case 1:
            if (!Flag(integer) || !p.empty() || !t.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            sequence->enabled = integer != 0; break;
        case 3:
            if (integer || p.size() != 1 || !t.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            sequence->speed = p[0]; break;
        case 8: case 9: case 10: case 11: case 14:
            if (integer || !p.empty() || !t.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 8) sequence->Play();
            if (request.index == 9) sequence->Stop();
            if (request.index == 10) sequence->Pause();
            if (request.index == 11) sequence->Resume();
            if (request.index == 14) sequence->bindings.clear();
            break;
        case 12: {
            if (integer || !p.empty() || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
            double seconds = 0; const auto parsed = std::from_chars(t[0].data(), t[0].data() + t[0].size(), seconds);
            if (parsed.ec != std::errc{} || parsed.ptr != t[0].data() + t[0].size() || !std::isfinite(seconds)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            sequence->SetTime(seconds); break;
        }
        case 13:
            if (integer || !p.empty() || t.size() != 1 || t[0].empty() || (related && related->GetScene() != &scene)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            view.sequence.Bind(t[0], related); break;
        }
        break;
    }
    default: return ManagedBridgeStatus::UNSUPPORTED;
    }
    if (!std::all_of(output.values.begin(), output.values.end(), [](float value) { return std::isfinite(value); })) return ManagedBridgeStatus::INVALID_ARGUMENT;
    result = std::move(output); return ManagedBridgeStatus::OK;
}
} /// @note namespace fbzz::scene::managed_detail
