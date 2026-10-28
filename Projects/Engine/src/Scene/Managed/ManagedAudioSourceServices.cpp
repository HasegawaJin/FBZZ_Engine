/// @file    ManagedAudioSourceServices.cpp
/// @brief   既存 audio selector を維持し、音源設定と局所フィルターを公開する。
/// @author  Hasegawa Jin
/// @date    2026-10-11
#include "ManagedAudioSourceServices.hpp"
#include "ManagedServicesBridge.hpp"
#include "ManagedObjectControls.hpp"
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
namespace fbzz::scene::managed_detail {
ManagedBridgeStatus RunAudioSourceSettings(GameObject& target, const ManagedBridgeRequest& request,
    int64_t integer, std::span<const float> p, std::span<const std::string> text,
    uint64_t relatedId, ManagedServiceResult& result)
{
    if (request.op != ManagedBridgeOp::SERVICE_AUDIO || request.index < 46 || request.index > 56
        || request.flags > 1 || relatedId != 0 || !text.empty()
        || !std::all_of(p.begin(), p.end(), [](float value) { return std::isfinite(value); }))
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    const bool read = request.index == 46 || request.index == 48 || request.index == 50
        || request.index == 52 || request.index == 54;
    if (read != (request.flags == 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (!read && !Script::IsInPlayMode()) return ManagedBridgeStatus::INVALID_SCOPE;
    const size_t count = read || request.index == 47 || request.index == 49 ? 0
        : request.index == 56 ? 2 : 1;
    if (p.size() != count) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (request.index == 47) {
        if (integer < (std::numeric_limits<int>::min)() || integer > (std::numeric_limits<int>::max)())
            return ManagedBridgeStatus::INVALID_ARGUMENT;
    } else if (request.index == 49) {
        if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
    } else if (integer != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (count != 0 && (p[0] < 0.0f || p[0] > 1.0f || (count == 2 && p[1] < 0.0f)))
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (ManagedObjectControls::IsPendingComponentRemoval(target, 6)) return ManagedBridgeStatus::NOT_FOUND;
    auto* source = target.GetComponent<AudioSourceComponent>();
    if (!source) return ManagedBridgeStatus::NOT_FOUND;
    ManagedServiceResult output;
    switch (request.index) {
    case 46: output.integer = source->priority; break;
    case 47: source->priority = static_cast<int>(integer); break;
    case 48: output.integer = source->streaming; break;
    case 49: source->streaming = integer != 0; break;
    case 50: output.values = {source->airAbsorption}; break;
    case 51: source->airAbsorption = p[0]; break;
    case 52: output.values = {source->dopplerLevel}; break;
    case 53: source->dopplerLevel = p[0]; break;
    case 54: output.values = {source->lowPassCutoff}; break;
    case 55: source->SetLowPassCutoff(p[0]); break;
    case 56: source->FadeLowPassTo(p[0], p[1]); break;
    }
    result = std::move(output);
    return ManagedBridgeStatus::OK;
}
} /// @note namespace fbzz::scene::managed_detail
