/// @file    ManagedScriptJobs.cpp
/// @brief   owner の生存・世代・有効状態を配送ごとに照合する native jobs。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include "ManagedScriptJobs.hpp"
#include <Engine/Scene/Managed/ManagedScriptAdapter.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Scene/Managed/ManagedEventPayload.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptEvent.hpp>
#include <Engine/Scene/ScriptProxy/ScriptTweenProxy.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace fbzz::scene::managed_detail {
namespace {
struct Job {
    ManagedScriptAdapter* owner = nullptr;
    uint64_t generation = 0;
    uint64_t token = 0;
    uint32_t kind = 0;
    uint32_t ease = 0;
    bool unscaled = false;
    double remaining = 0;
    double duration = 0;
    double elapsed = 0;
    double interval = 0;
    float from = 0;
    float to = 0;
    ScriptEventToken subscription;
    ScriptEventToken nativeSubscription;
};
std::map<uint64_t, Job>& Jobs() { static std::map<uint64_t, Job> jobs; return jobs; }
bool Live(const Job& job)
{
    return job.owner && job.owner->Generation() == job.generation && job.owner->HasLiveContext()
        && job.owner->scene.IsActiveAndEnabled();
}
}

ManagedBridgeStatus RegisterManagedJob(ManagedScriptAdapter& owner, const ManagedBridgeRequest& request)
{
    const uint64_t token = std::bit_cast<uint64_t>(request.integer);
    const bool tween = request.op == ManagedBridgeOp::REGISTER_TWEEN;
    if (!token || request.flags > 1 || !std::isfinite(request.x) || !std::isfinite(request.y) || !std::isfinite(request.z)
        || (!tween && (request.index > 2 || request.x < 0 || (request.index == 1 && (request.x > 16777216 || std::floor(request.x) != request.x))
            || (request.index == 2 && request.y <= 0))) || (tween && (request.index > 9 || request.z < 0)))
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    auto& jobs = Jobs();
    const auto found = jobs.find(token);
    if (found != jobs.end() && (found->second.owner != &owner || found->second.generation != owner.Generation() || found->second.subscription.IsValid()))
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (found == jobs.end() && jobs.size() >= 32768) return ManagedBridgeStatus::CAPACITY_LIMIT;
    Job job;
    job.owner = &owner; job.generation = owner.Generation(); job.token = token;
    job.kind = tween ? 3 : request.index; job.ease = request.index; job.unscaled = request.flags != 0;
    job.remaining = request.x; job.interval = request.y;
    job.from = request.x; job.to = request.y; job.duration = request.z;
    jobs.insert_or_assign(token, job);
    return ManagedBridgeStatus::OK;
}

void CancelManagedJob(ManagedScriptAdapter& owner, uint64_t token)
{
    const auto found = Jobs().find(token);
    if (found == Jobs().end() || found->second.owner != &owner) return;
    if (found->second.subscription.IsValid()) ScriptEventBus::Unsubscribe(found->second.subscription);
    if (found->second.nativeSubscription.IsValid()) ScriptEventBus::Unsubscribe(found->second.nativeSubscription);
    Jobs().erase(found);
}
void CancelManagedOwner(ManagedScriptAdapter& owner)
{
    std::vector<uint64_t> tokens;
    for (const auto& [token, job] : Jobs()) if (job.owner == &owner) tokens.push_back(token);
    for (uint64_t token : tokens) CancelManagedJob(owner, token);
}
ManagedBridgeStatus SubscribeManagedEvent(ManagedScriptAdapter& owner, uint64_t token, std::string_view channel)
{
    if (!token || !channel.starts_with("managed:") || channel.size() > 1024 || channel.find('\0') != std::string_view::npos
        || Jobs().contains(token)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (Jobs().size() >= 32768) return ManagedBridgeStatus::CAPACITY_LIMIT;
    Job job;
    job.owner = &owner; job.generation = owner.Generation(); job.token = token; job.kind = 4;
    job.subscription = ScriptEventBus::SubscribeRaw(&owner, channel, [token](const void* value) {
        const auto found = Jobs().find(token);
        if (found == Jobs().end() || !Live(found->second)) return;
        const Job snapshot = found->second;
        const auto& payload = *static_cast<const ManagedEventPayload*>(value);
        ManagedScriptRuntime::Instance().InvokeOwned(*snapshot.owner, ManagedCallback::EVENT, token, payload.json, 0, false);
    });
    if (!job.subscription.IsValid()) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (channel == "managed:Fbzz.SequenceEvent") {
        job.nativeSubscription = ScriptEventBus::SubscribeRaw(&owner, SequenceEvent::EVENT_NAME, [token](const void* value) {
            const auto found = Jobs().find(token);
            if (!value || found == Jobs().end() || !Live(found->second)) return;
            const Job snapshot = found->second;
            const auto& event = *static_cast<const SequenceEvent*>(value);
            const SequenceEventInfo info{event.name, event.intParam, event.floatParam, event.sequenceTime, event.sequenceName};
            ManagedScriptRuntime::Instance().InvokeSequenceEvent(*snapshot.owner, info, token);
        });
        if (!job.nativeSubscription.IsValid()) {
            ScriptEventBus::Unsubscribe(job.subscription);
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
    }
    Jobs().emplace(token, job);
    return ManagedBridgeStatus::OK;
}
ManagedBridgeStatus PublishManagedEvent(std::string_view input)
{
    const auto separator = input.find('\0');
    if (separator == std::string_view::npos || separator > 1024 || !input.substr(0, separator).starts_with("managed:")
        || input.size() > 1024 * 1024 || input.substr(separator + 1).find('\0') != std::string_view::npos)
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    const ManagedEventPayload payload{input.substr(separator + 1)};
    ScriptEventBus::PublishRaw(input.substr(0, separator), &payload);
    return ManagedBridgeStatus::OK;
}
void TickManagedJobs(Scene& scene, float scaledDelta, float unscaledDelta)
{
    if (!std::isfinite(scaledDelta) || !std::isfinite(unscaledDelta) || scaledDelta < 0 || unscaledDelta < 0) return;
    std::vector<uint64_t> tokens;
    for (const auto& [token, job] : Jobs())
        if (job.kind != 4 && job.owner->HasLiveContext() && job.owner->scene.Self()->GetScene() == &scene) tokens.push_back(token);
    for (uint64_t token : tokens) {
        auto found = Jobs().find(token);
        if (found == Jobs().end()) continue;
        if (!Live(found->second)) { CancelManagedOwner(*found->second.owner); continue; }
        Job& job = found->second;
        const double delta = job.unscaled ? unscaledDelta : scaledDelta;
        bool completed = true;
        float value = 0;
        if (job.kind == 3) {
            job.elapsed = std::min(job.elapsed + delta, job.duration);
            const float alpha = job.duration == 0 ? 1.0f : static_cast<float>(job.elapsed / job.duration);
            const float eased = ScriptTweenProxy::Evaluate(static_cast<TweenEase>(job.ease), alpha);
            value = static_cast<float>(static_cast<double>(job.from) + (static_cast<double>(job.to) - job.from) * eased);
            completed = alpha >= 1;
        } else {
            job.remaining -= job.kind == 1 ? 1.0 : delta;
            if (job.remaining > 0) continue;
            if (job.kind == 2) { job.remaining += job.interval; completed = false; }
        }
        const Job snapshot = job;
        if (completed) Jobs().erase(found);
        ManagedScriptRuntime::Instance().InvokeOwned(*snapshot.owner, ManagedCallback::JOB, token, {}, value, completed);
    }
}
}
