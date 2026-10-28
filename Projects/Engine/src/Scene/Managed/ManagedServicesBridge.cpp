/// @file    ManagedServicesBridge.cpp
/// @brief   固定値 packet の検証と既存 native ScriptProxy への型付き転送。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include "ManagedServicesBridge.hpp"
#include "ManagedPacketRead.hpp"
#include "ManagedObjectControls.hpp"
#include "ManagedAuthoringServices.hpp"
#include "ManagedStoreTransfers.hpp"
#include "ManagedMaterialTransfers.hpp"
#include "ManagedPhysicsBridge.hpp"
#include "ManagedRuntimeServices.hpp"
#include "ManagedWorldFamilyServices.hpp"
#include "ManagedRenderService.hpp"
#include "ManagedJointService.hpp"
#include "ManagedJointTransfers.hpp"
#include "ManagedAssetServices.hpp"
#include "ManagedComponentServices.hpp"
#include "ManagedAudioSourceServices.hpp"
#include "ManagedComponentTransfers.hpp"
#include "ManagedRenderComponentServices.hpp"
#include "ManagedRenderTransfers.hpp"
#include "ManagedParticleServices.hpp"
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/CameraComponent.hpp>
#include <Engine/Scene/Components/CharacterControllerComponent.hpp>
#include <Engine/Scene/Components/NavMeshAgentComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/UIButton.hpp>
#include <Engine/Scene/Components/UIImage.hpp>
#include <Engine/Scene/Components/UIText.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <bit>
#include <charconv>
#include <Engine/Scene/Components/UIControls.hpp>
#include <Engine/Scene/Components/UICanvasGroup.hpp>
#include <Engine/Scene/ScriptRuntime.hpp>
#include <Math/MathUtils.hpp>
#include <span>
#include <unordered_map>

namespace fbzz::scene::managed_detail {
namespace {

class ProxyScript final : public Script {
public:
    /// @note callback を駆動しない facade 専用文脈。Profiler 登録や作者 lifecycle は発生させない。
    void Bind(Scene& scene, GameObject& target)
    { m_scene = &scene; m_gameObject = &target; }
    void Clear() { m_scene = nullptr; m_gameObject = nullptr; }
};

template<typename T>
bool Read(std::string_view bytes, size_t& offset, T& value)
{
    return ReadManagedValue(bytes, offset, value);
}

struct Arguments {
    uint64_t relatedId = 0;
    int64_t integer = 0;
    std::vector<float> values;
    std::vector<std::string> text;
};

bool Parse(const ManagedBridgeRequest& request, Arguments& arguments)
{
    if (!request.input || request.inputBytes < 24 || request.inputBytes > 1048576 || request.flags > 1) return false;
    const std::string_view bytes(request.input, request.inputBytes);
    size_t offset = 0;
    uint32_t floats = 0, strings = 0;
    if (!Read(bytes, offset, arguments.relatedId) || !Read(bytes, offset, arguments.integer)
        || !Read(bytes, offset, floats) || !Read(bytes, offset, strings) || floats > 128 || strings > 128) return false;
    arguments.values.resize(floats);
    for (float& value : arguments.values) if (!Read(bytes, offset, value) || !std::isfinite(value)) return false;
    for (uint32_t index = 0; index < strings; ++index) {
        uint32_t length = 0;
        if (!Read(bytes, offset, length) || length > bytes.size() - offset) return false;
        arguments.text.emplace_back(bytes.substr(offset, length)); offset += length;
    }
    return offset == bytes.size();
}

bool FitsInt(int64_t integer)
{ return integer >= std::numeric_limits<int>::min() && integer <= std::numeric_limits<int>::max(); }

bool FiniteVector(const math::Vector3& value)
{ return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }
bool NormalizeVector(const math::Vector3& value, math::Vector3& output)
{
    const float length = value.Length();
    if (!FiniteVector(value) || !std::isfinite(length) || math::NearlyZero(length)) return false;
    output = value / length; return FiniteVector(output);
}
/// @note Perspective の DirectX depth を解析的に戻し、極端な行列で Inverse の MathContract を発火させない。
ManagedBridgeStatus CameraProjection(GameObject& target, const ManagedBridgeRequest& request, const Arguments& arguments, ManagedServiceResult& output)
{
    const auto& p = arguments.values;
    const uint32_t method = request.index;
    const size_t expected = method == 18 ? 2 : 3;
    if (request.flags != 1 || arguments.integer || arguments.relatedId || !arguments.text.empty() || p.size() != expected)
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    const auto* component = target.GetComponent<CameraComponent>();
    if (!component) return ManagedBridgeStatus::NOT_FOUND;
    const bool tryResult = method == 17 || method == 19 || method == 20;
    output.integer = 0;
    if (!tryResult) output.values.assign(method == 18 ? 6 : 3, 0.0f);
    if (!component->enabled || !target.activeInHierarchy() || !std::isfinite(component->fovY) || component->fovY <= 0 || component->fovY >= 180
        || !std::isfinite(component->aspectRatio) || component->aspectRatio <= math::EPSILON || !std::isfinite(component->nearZ)
        || component->nearZ <= 0 || !std::isfinite(component->farZ) || component->farZ <= component->nearZ) return ManagedBridgeStatus::OK;
    const auto& pose = target.transform;
    const math::Vector3 rawForward = pose.worldRotation * math::Vector3::FORWARD;
    const math::Vector3 rawRight = pose.worldRotation * math::Vector3::RIGHT;
    const math::Vector3 rawUp = pose.worldRotation * math::Vector3::UP;
    math::Vector3 forward, right;
    if (!FiniteVector(pose.worldPosition) || !NormalizeVector((pose.worldPosition + rawForward) - pose.worldPosition, forward)
        || !NormalizeVector(math::Vector3::Cross(rawUp, forward), right)) return ManagedBridgeStatus::OK;
    const math::Vector3 up = math::Vector3::Cross(forward, right);
    const float yScale = 1.0f / std::tan(math::ToRad(component->fovY) * 0.5f);
    const float xScale = yScale / component->aspectRatio;
    const float q = component->farZ / (component->farZ - component->nearZ);
    const float zBias = -component->nearZ * component->farZ / (component->farZ - component->nearZ);
    if (!std::isfinite(xScale) || !std::isfinite(yScale) || xScale == 0 || yScale == 0 || !std::isfinite(q) || !std::isfinite(zBias)) return ManagedBridgeStatus::OK;
    const auto runtime = ScriptRuntime::GetCurrent();
    const float width = static_cast<float>(std::max(runtime.viewportWidth, 1u));
    const float height = static_cast<float>(std::max(runtime.viewportHeight, 1u));
    const auto unproject = [&](float x, float y, float z, math::Vector3& result) {
        const float nx = (x / width) * 2.0f - 1.0f;
        const float ny = 1.0f - (y / height) * 2.0f;
        const float denominator = q - std::clamp(z, 0.0f, 1.0f);
        if (!std::isfinite(nx) || !std::isfinite(ny) || denominator == 0 || !std::isfinite(denominator)) return false;
        const float depth = -zBias / denominator;
        const math::Vector3 candidate = pose.worldPosition + forward * depth + right * (nx * depth / xScale) + up * (ny * depth / yScale);
        if (!FiniteVector(candidate)) return false;
        result = candidate; return true;
    };
    math::Vector3 value;
    if (method == 8) {
        if (unproject(p[0], p[1], p[2], value)) output.values = {value.x, value.y, value.z};
    } else if (method == 18) {
        math::Vector3 near = math::Vector3::ZERO, far = math::Vector3::ZERO, direction = math::Vector3::ZERO;
        if (unproject(p[0], p[1], 0, near) && unproject(p[0], p[1], 1, far)) (void)NormalizeVector(far - near, direction);
        output.values = {near.x, near.y, near.z, direction.x, direction.y, direction.z};
    } else if (method == 20) {
        if (p[2] <= 0) return ManagedBridgeStatus::OK;
        const float halfHeight = std::tan(math::ToRad(component->fovY) * 0.5f) * p[2];
        value = pose.worldPosition + rawForward * p[2] + rawRight * ((p[0] * 2.0f - 1.0f) * halfHeight * component->aspectRatio)
            + rawUp * ((1.0f - p[1] * 2.0f) * halfHeight);
        if (FiniteVector(value)) { output.integer = 1; output.values = {value.x, value.y, value.z}; }
    } else {
        /// @note 元の world projection と同じ行列の演算順を維持する。LookAt の正規化前提は上で検証済み。
        renderer::Camera camera; camera.m_position = pose.worldPosition; camera.m_rotation = pose.worldRotation;
        camera.m_fovY = component->fovY; camera.m_aspect = component->aspectRatio; camera.m_near = component->nearZ; camera.m_far = component->farZ;
        const math::Vector4 clip = camera.GetViewProjection() * math::Vector4(p[0], p[1], p[2], 1);
        if (!std::isfinite(clip.w) || clip.w <= 0.000001f) return ManagedBridgeStatus::OK;
        value = {0.5f + 0.5f * clip.x / clip.w, 0.5f - 0.5f * clip.y / clip.w, clip.z / clip.w};
        if (!FiniteVector(value)) return ManagedBridgeStatus::OK;
        if (method == 17) output.integer = value.x >= 0 && value.x <= 1 && value.y >= 0 && value.y <= 1 && value.z >= 0 && value.z <= 1 ? 1 : 0;
        else if (method == 19) { output.integer = 1; output.values = {value.x, value.y, value.z}; }
        else {
            value.x *= width; value.y *= height;
            if (FiniteVector(value)) output.values = {value.x, value.y, value.z};
        }
    }
    return ManagedBridgeStatus::OK;
}

ManagedBridgeStatus CharacterOperation(GameObject& target, const ManagedBridgeRequest& request, const Arguments& arguments,
    ManagedServiceResult& output, bool fixedUpdate, bool collisionCallback)
{
    auto* controller = target.GetComponent<CharacterControllerComponent>();
    if (!controller) return ManagedBridgeStatus::NOT_FOUND;
    const uint32_t method = request.index;
    constexpr size_t counts[] = {1, 3, 1, 3, 3, 6, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 3};
    if (method >= std::size(counts)) return ManagedBridgeStatus::UNSUPPORTED;
    const bool readOnly = (method >= 6 && method <= 10) || method == 15;
    if (request.flags != (readOnly ? 1u : 0u) || arguments.values.size() != counts[method] || !arguments.text.empty()
        || arguments.relatedId || (method == 14 ? arguments.integer != 0 && arguments.integer != 1 : arguments.integer != 0))
        return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (!readOnly && !fixedUpdate && !(method == 16 && collisionCallback)) return ManagedBridgeStatus::INVALID_SCOPE;
    auto* component = target.GetComponent<RigidBodyComponent>();
    physics::RigidBody* body = component && component->enabled ? component->rigidBody.get() : nullptr;
    if (readOnly) {
        if (method == 6 || method == 7 || method == 10) {
            const auto value = method == 6 ? controller->GetVelocity(body) : method == 7 ? controller->GetHorizontalVelocity(body) : controller->groundNormal;
            output.values = {value.x, value.y, value.z};
        } else if (method == 8) output.integer = controller->isGrounded ? 1 : 0;
        else if (method == 9) output.values = {controller->verticalSpeed};
        else output.integer = controller->enabled ? 1 : 0;
        return ManagedBridgeStatus::OK;
    }
    if (method <= 5 && !body) return ManagedBridgeStatus::NOT_FOUND;
    if (!controller->IsFiniteState() || (body && !body->IsFiniteState())) return ManagedBridgeStatus::INVALID_ARGUMENT;
    CharacterControllerComponent candidate = *controller;
    physics::RigidBody bodyCandidate = body ? *body : physics::RigidBody{};
    physics::RigidBody* bodyView = body ? &bodyCandidate : nullptr;
    const auto& p = arguments.values;
    math::Vector3 normal = math::Vector3::ZERO;
    switch (method) {
    case 0:
        if (p[0] < 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
        candidate.Tick(bodyView, p[0]); break;
    case 1: candidate.Jump(bodyView, {p[0], p[1], p[2]}); break;
    case 2:
        if (!std::isfinite((p[0] - bodyCandidate.GetVelocity().y) * bodyCandidate.GetMass())) return ManagedBridgeStatus::INVALID_ARGUMENT;
        candidate.JumpAtVelocity(bodyView, p[0]); break;
    case 3: candidate.SetHorizontalVelocity(bodyView, {p[0], p[1], p[2]}); break;
    case 4: candidate.AddHorizontalVelocity(bodyView, {p[0], p[1], p[2]}); break;
    case 5:
        if (p[3] < 0 || !std::isfinite(math::Vector3{p[0], p[1], p[2]}.LengthSq())
            || (p[4] >= 0 && !std::isfinite(p[4] * p[3])) || (p[5] >= 0 && !std::isfinite(p[5] * p[3]))) return ManagedBridgeStatus::INVALID_ARGUMENT;
        candidate.Move(bodyView, {p[0], p[1], p[2]}, p[3], p[4], p[5]); break;
    case 11:
        normal = {p[0], p[1], p[2]};
        if (!std::isfinite(normal.LengthSq())) return ManagedBridgeStatus::INVALID_ARGUMENT;
        candidate.ForceGrounded(normal); break;
    case 12: candidate.ForceAirborne(); break;
    case 13: candidate.UseAutomaticGrounding(); break;
    case 14: candidate.enabled = arguments.integer == 1; break;
    case 16: {
        normal = {p[0], p[1], p[2]};
        if (!std::isfinite(normal.LengthSq())) return ManagedBridgeStatus::INVALID_ARGUMENT;
        CollisionInfo contact; contact.contactNormal = normal;
        candidate.RegisterGroundContact(contact);
        /// @note RegisterGroundContact は実 body を GameObject から引くため、コピー検証では最大の速度射影を先に検証する。
        if (body && normal.LengthSq() > math::EPSILON) {
            const math::Vector3 unit = normal.Normalized();
            const float dot = math::Vector3::Dot(bodyCandidate.GetVelocity(), unit);
            if (!std::isfinite(dot) || !FiniteVector(bodyCandidate.GetVelocity() - unit * dot)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        break;
    }
    default: return ManagedBridgeStatus::UNSUPPORTED;
    }
    if (!candidate.IsFiniteState() || (body && !bodyCandidate.IsFiniteState())) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (method == 16) {
        CollisionInfo contact; contact.self = &target; contact.contactNormal = normal;
        controller->RegisterGroundContact(contact);
    } else {
        *controller = candidate;
        if (body) *body = bodyCandidate;
    }
    return ManagedBridgeStatus::OK;
}

bool DecodeSpec(std::span<const float> values, int64_t integer, audio::SynthSpec& spec)
{
    if (values.size() < 19 || values[0] < 0 || values[0] > 4 || std::floor(values[0]) != values[0]) return false;
    spec.wave = static_cast<audio::SynthWave>(static_cast<uint32_t>(values[0]));
    spec.attack = values[1];
    spec.sustain = values[2];
    spec.decay = values[3];
    spec.punch = values[4];
    spec.startFrequency = values[5];
    spec.minFrequency = values[6];
    spec.slide = values[7];
    spec.vibratoDepth = values[8];
    spec.vibratoRate = values[9];
    spec.dutyCycle = values[10];
    spec.dutySweep = values[11];
    spec.lowPassCutoff = values[12];
    spec.lowPassSweep = values[13];
    spec.highPassCutoff = values[14];
    spec.bitCrush = values[15];
    spec.repeatRate = values[16];
    spec.arpeggioMod = values[17];
    spec.amplitude = values[18];
    const uint64_t packed = std::bit_cast<uint64_t>(integer); spec.seed = static_cast<uint32_t>(packed); spec.sampleRate = static_cast<uint32_t>(packed >> 32);
    return true;
}
void EncodeSpec(const audio::SynthSpec& spec, ManagedServiceResult& result)
{
    result.values = {static_cast<float>(spec.wave), spec.attack, spec.sustain, spec.decay, spec.punch, spec.startFrequency, spec.minFrequency, spec.slide, spec.vibratoDepth, spec.vibratoRate, spec.dutyCycle, spec.dutySweep, spec.lowPassCutoff, spec.lowPassSweep, spec.highPassCutoff, spec.bitCrush, spec.repeatRate, spec.arpeggioMod, spec.amplitude};
    result.integer = std::bit_cast<int64_t>((static_cast<uint64_t>(spec.sampleRate) << 32) | spec.seed);
}

} /// @note namespace

struct ManagedServicesBridge::Impl {
    ManagedComponentTransfers componentTransfers;
    ManagedRenderTransfers renderTransfers;
    ManagedAssetServices assetServices;
    ManagedJointTransfers jointTransfers;
    ManagedStoreTransfers storeTransfers;
    ManagedMaterialTransfers materialTransfers;
    ManagedRuntimeServices runtimeServices;
    ProxyScript view;
    ManagedPhysicsBridge physics;
    struct ClipRecord { uint64_t generation; uint64_t owner; uint64_t scene; SynthClip clip; };
    struct LoopRecord { uint64_t generation; uint64_t owner; uint64_t scene; AudioLoop loop; };
    std::unordered_map<int64_t, ClipRecord> clips;
    std::unordered_map<int64_t, LoopRecord> loops;
    int64_t nextHandle = 1;
    ManagedBridgeStatus AudioAdvanced(Scene& scene, Script& caller, GameObject& target, const ManagedBridgeRequest& request,
        const Arguments& arguments, ManagedServiceResult& output);
};

ManagedBridgeStatus ManagedServicesBridge::Impl::AudioAdvanced(Scene& scene, Script& caller, GameObject& target,
    const ManagedBridgeRequest& request, const Arguments& arguments, ManagedServiceResult& output)
{
    const auto& p = arguments.values; const auto& t = arguments.text; const int64_t integer = arguments.integer;
    if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const bool readOnly = request.index == 29 || request.index == 30 || request.index == 31 || request.index == 45;
    if (request.flags != (readOnly ? 1u : 0u)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const uint64_t sceneId = scene.GetRenderSceneGeneration();
    const auto clipRecord = clips.find(integer);
    const auto loopRecord = loops.find(integer);
    const auto owned = [&](const auto& record) { return record.generation == request.generation && record.owner == request.ownerId && record.scene == sceneId; };
    const auto requireClip = [&]() { return integer > 0 && clipRecord != clips.end() && owned(clipRecord->second); };
    const auto requireLoop = [&]() { return integer > 0 && loopRecord != loops.end() && owned(loopRecord->second); };
    audio::SynthSpec spec;
    const uint64_t packed = std::bit_cast<uint64_t>(integer);
    const uint32_t seed = static_cast<uint32_t>(packed);
    const uint32_t preset = static_cast<uint32_t>(packed >> 32);
    switch (request.index) {
    case 29:
        if (!p.empty() || !t.empty() || preset > 6) return ManagedBridgeStatus::INVALID_ARGUMENT;
        EncodeSpec(view.audio.MakeSpec(static_cast<audio::SynthPreset>(preset), seed), output); return ManagedBridgeStatus::OK;
    case 30: {
        if (p.size() != 20 || t.size() != 1 || p[19] < 0 || p[19] > 1 || !DecodeSpec(p, integer, spec)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        uint32_t mutationSeed = 0; const auto parsed = std::from_chars(t[0].data(), t[0].data() + t[0].size(), mutationSeed);
        if (parsed.ec != std::errc{} || parsed.ptr != t[0].data() + t[0].size()) return ManagedBridgeStatus::INVALID_ARGUMENT;
        EncodeSpec(view.audio.MutateSpec(spec, p[19], mutationSeed), output); return ManagedBridgeStatus::OK;
    }
    case 31:
        if (!p.empty() || t.size() != 1 || integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (view.audio.LoadSpec(t[0], spec)) EncodeSpec(spec, output); return ManagedBridgeStatus::OK;
    case 32: case 33: {
        if (!t.empty() || (request.index == 32 ? p.size() != 19 || !DecodeSpec(p, integer, spec) : !p.empty() || preset > 6))
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (nextHandle == INT64_MAX) return ManagedBridgeStatus::PENDING_LIMIT;
        const SynthClip clip = request.index == 32 ? view.audio.Synthesize(spec) : view.audio.Synthesize(static_cast<audio::SynthPreset>(preset), seed);
        if (!clip.IsValid()) return ManagedBridgeStatus::OK;
        output.integer = nextHandle++; clips.emplace(output.integer, ClipRecord{request.generation, request.ownerId, sceneId, clip}); return ManagedBridgeStatus::OK;
    }
    case 34:
        if (!p.empty() || !t.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (!integer) return ManagedBridgeStatus::OK;
        if (!requireClip()) return ManagedBridgeStatus::NOT_FOUND;
        view.audio.ReleaseClip(clipRecord->second.clip); clips.erase(clipRecord); return ManagedBridgeStatus::OK;
    case 35: case 36:
        if (!p.empty() || !t.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (!requireClip()) return ManagedBridgeStatus::NOT_FOUND;
        if (!target.GetComponent<AudioSourceComponent>()) return ManagedBridgeStatus::NOT_FOUND;
        if (request.index == 35) view.audio.PlayClip(clipRecord->second.clip); else view.audio.PlayClipAsSource(clipRecord->second.clip);
        return ManagedBridgeStatus::OK;
    case 37: case 38: case 39:
        if ((request.index == 39 ? t.size() != 1 : !t.empty())
            || (request.index == 38 ? !p.empty() || preset > 6 : p.size() != 19 || !DecodeSpec(p, integer, spec))) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (request.index != 39 && !target.GetComponent<AudioSourceComponent>()) return ManagedBridgeStatus::NOT_FOUND;
        if (request.index == 37) view.audio.PlaySynth(spec);
        else if (request.index == 38) view.audio.PlaySynth(static_cast<audio::SynthPreset>(preset), seed);
        else view.audio.PlaySynth2D(spec, t[0]);
        return ManagedBridgeStatus::OK;
    case 40: case 41:
        if (request.index == 40 ? !p.empty() || t.size() != 1 : p.size() != 4 || !t.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (!requireClip()) return ManagedBridgeStatus::NOT_FOUND;
        if (request.index == 40) view.audio.PlayClip2D(clipRecord->second.clip, t[0]);
        else view.audio.PlayClipAtPoint(clipRecord->second.clip, {p[0], p[1], p[2]}, p[3]);
        return ManagedBridgeStatus::OK;
    case 42: {
        if (p.size() != 5 || t.size() != 3 || (integer && !requireLoop())) return integer ? ManagedBridgeStatus::NOT_FOUND : ManagedBridgeStatus::INVALID_ARGUMENT;
        if (caller.scene.Self() != &target) return ManagedBridgeStatus::INVALID_SCOPE;
        if (nextHandle == INT64_MAX || !std::isfinite(std::max(p[3], 0.0f) + 0.001f)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        AudioLoopSettings settings; settings.label = t[1]; settings.bus = t[2]; settings.spatialBlend = p[2]; settings.minDistance = p[3]; settings.maxDistance = p[4];
        AudioLoop loop = integer ? loopRecord->second.loop : AudioLoop{};
        const bool succeeded = caller.audio.UpdateLoop(loop, t[0], p[0], p[1], settings);
        output.integer = succeeded ? 1 : 0;
        if (succeeded) {
            const int64_t handle = integer ? integer : nextHandle++;
            if (integer) loopRecord->second.loop = loop; else loops.emplace(handle, LoopRecord{request.generation, request.ownerId, sceneId, loop});
            output.text = {std::to_string(handle)};
        }
        return ManagedBridgeStatus::OK;
    }
    case 43: case 44: case 45:
        if (!p.empty() || !t.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (caller.scene.Self() != &target) return ManagedBridgeStatus::INVALID_SCOPE;
        if (!integer) { output.integer = 0; return ManagedBridgeStatus::OK; }
        if (!requireLoop()) return ManagedBridgeStatus::NOT_FOUND;
        if (request.index == 43) output.integer = caller.audio.StopLoop(loopRecord->second.loop) ? 1 : 0;
        else if (request.index == 44) { caller.audio.ReleaseLoop(loopRecord->second.loop); loops.erase(loopRecord); }
        else output.integer = loopRecord->second.loop.IsValid() ? 1 : 0;
        return ManagedBridgeStatus::OK;
    default: return ManagedBridgeStatus::UNSUPPORTED;
    }
}

ManagedServicesBridge::ManagedServicesBridge() : m_impl(std::make_unique<Impl>()) { }
ManagedServicesBridge::~ManagedServicesBridge() = default;

void ManagedServicesBridge::CancelCursorOwner(uint64_t ownerId) { m_impl->runtimeServices.CancelOwner(ownerId); }

bool ManagedServicesBridge::CursorOwner(const ManagedBridgeRequest& request, uint64_t& ownerId)
{
    Arguments arguments;
    if (!Parse(request, arguments)) return false;
    if (request.index == 0) { ownerId = std::bit_cast<uint64_t>(arguments.integer); return ownerId != 0; }
    if (request.index > 3 || arguments.text.size() != 1) return false;
    const auto& text = arguments.text[0];
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), ownerId);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && ownerId != 0;
}

void ManagedServicesBridge::CancelOwner(uint64_t ownerId, Script* caller)
{
    m_impl->storeTransfers.CancelOwner(ownerId);
    m_impl->componentTransfers.CancelOwner(ownerId);
    m_impl->renderTransfers.CancelOwner(ownerId);
    m_impl->assetServices.CancelOwner(ownerId, caller);
    m_impl->jointTransfers.CancelOwner(ownerId);
    m_impl->materialTransfers.CancelOwner(ownerId);
    m_impl->runtimeServices.CancelOwner(ownerId);
    for (auto it = m_impl->clips.begin(); it != m_impl->clips.end();) {
        if (it->second.owner != ownerId) { ++it; continue; }
        m_impl->view.audio.ReleaseClip(it->second.clip); it = m_impl->clips.erase(it);
    }
    for (auto it = m_impl->loops.begin(); it != m_impl->loops.end();) {
        if (it->second.owner != ownerId) { ++it; continue; }
        if (caller) caller->audio.ReleaseLoop(it->second.loop);
        it = m_impl->loops.erase(it);
    }
}

void ManagedServicesBridge::RetireGeneration(uint64_t generation)
{
    m_impl->storeTransfers.RetireGeneration(generation);
    m_impl->componentTransfers.RetireGeneration(generation);
    m_impl->renderTransfers.RetireGeneration(generation);
    m_impl->assetServices.RetireGeneration(generation);
    m_impl->jointTransfers.RetireGeneration(generation);
    m_impl->materialTransfers.RetireGeneration(generation);
    m_impl->runtimeServices.RetireGeneration(generation);
    for (auto it = m_impl->clips.begin(); it != m_impl->clips.end();) {
        if (it->second.generation != generation) { ++it; continue; }
        m_impl->view.audio.ReleaseClip(it->second.clip); it = m_impl->clips.erase(it);
    }
    std::erase_if(m_impl->loops, [generation](const auto& entry) { return entry.second.generation == generation; });
}

ManagedBridgeStatus ManagedServicesBridge::Run(Scene& scene, Script& caller, GameObject& target, GameObject* related,
    const ManagedBridgeRequest& request, ManagedServiceResult& result, bool fixedUpdate, bool collisionCallback)
{
    if (!target.IsValid() || target.GetScene() != &scene || (related && (!related->IsValid() || related->GetScene() != &scene)))
        return ManagedBridgeStatus::NOT_FOUND;
    const bool store = request.op == ManagedBridgeOp::SERVICE_SAVE || request.op == ManagedBridgeOp::SERVICE_CONFIG;
    if (request.op == ManagedBridgeOp::SERVICE_ASSETS && request.index >= 12 && request.index <= 14) {
        if (related) return ManagedBridgeStatus::INVALID_ARGUMENT;
        return m_impl->assetServices.Run(scene, caller, target, request, 0, {}, {}, result);
    }
    if (request.op == ManagedBridgeOp::SERVICE_MATERIAL && request.index >= 33 && request.index <= 35) {
        if (related) return ManagedBridgeStatus::INVALID_ARGUMENT;
        m_impl->view.Bind(scene, target);
        struct Reset { ProxyScript& view; ~Reset() { view.Clear(); } } reset{m_impl->view};
        return m_impl->materialTransfers.Run(scene, target, m_impl->view, request, result);
    }
    if (store && request.index >= 23 && request.index <= 25) {
        if (related) return ManagedBridgeStatus::INVALID_ARGUMENT;
        return m_impl->storeTransfers.Run(scene, target, request, result);
    }
    Arguments arguments;
    if (!Parse(request, arguments)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const auto& p = arguments.values;
    const auto& t = arguments.text;
    const int64_t integer = arguments.integer;
    if (request.op >= ManagedBridgeOp::SERVICE_LINE && request.op <= ManagedBridgeOp::SERVICE_INSTANCES) {
        if ((arguments.relatedId != 0) != (related != nullptr)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        m_impl->view.Bind(scene, target);
        struct Reset { ProxyScript& view; ~Reset() { view.Clear(); } } reset{m_impl->view};
        if (request.op == ManagedBridgeOp::SERVICE_RAGDOLL && request.index >= 40 && request.index <= 43) {
            if (related) return ManagedBridgeStatus::INVALID_ARGUMENT;
            return m_impl->componentTransfers.Run(scene, target, request, integer, p, t, result, fixedUpdate);
        }
        if ((request.op == ManagedBridgeOp::SERVICE_LINE && request.index >= 50 && request.index <= 53)
            || (request.op == ManagedBridgeOp::SERVICE_MESH && request.index >= 50 && request.index <= 54)) {
            if (related) return ManagedBridgeStatus::INVALID_ARGUMENT;
            return m_impl->renderTransfers.Run(scene, target, m_impl->view, request, integer, p, t, result);
        }
        if (request.op <= ManagedBridgeOp::SERVICE_SKINNED_MESH || request.op==ManagedBridgeOp::SERVICE_INSTANCES)
            return RunRenderComponentServices(scene, m_impl->view, target, related, request, integer, p, t, result);
        return RunComponentServices(scene, m_impl->view, target, related, request, integer, p, t, result, fixedUpdate);
    }
    if (request.op == ManagedBridgeOp::SERVICE_ASSETS || (request.op == ManagedBridgeOp::SERVICE_MATERIAL && request.index == 42)) {
        if (related || arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
        return m_impl->assetServices.Run(scene, caller, target, request, integer, p, t, result);
    }
    if (request.op == ManagedBridgeOp::SERVICE_JOINT && request.index == 19) {
        if (related || arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
        return m_impl->jointTransfers.Begin(scene, target, request, integer, p, t, result, fixedUpdate);
    }
    if (request.op == ManagedBridgeOp::SERVICE_JOINT) {
        if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
        m_impl->view.Bind(scene, target);
        struct Reset { ProxyScript& view; ~Reset() { view.Clear(); } } reset{m_impl->view};
        return RunManagedJointService(scene, m_impl->view, target, related, request, p, t, result, fixedUpdate);
    }
    if (request.op == ManagedBridgeOp::SERVICE_RENDER) {
        if (related || arguments.relatedId || caller.scene.Self() != &target) return ManagedBridgeStatus::INVALID_SCOPE;
        m_impl->view.Bind(scene, target);
        struct Reset { ProxyScript& view; ~Reset() { view.Clear(); } } reset{m_impl->view};
        return RunManagedRenderService(m_impl->view, request, integer, p, t, result);
    }
    if (request.op == ManagedBridgeOp::SERVICE_DISPLAY) {
        if (related || arguments.relatedId || caller.scene.Self() != &target) return ManagedBridgeStatus::INVALID_SCOPE;
        m_impl->view.Bind(scene, target);
        struct Reset { ProxyScript& view; ~Reset() { view.Clear(); } } reset{m_impl->view};
        return RunManagedDisplayService(scene, m_impl->view, request, integer, p, t, result);
    }
    if (request.op == ManagedBridgeOp::SERVICE_MATERIAL && (request.index == 30 || request.index == 32)) {
        if (arguments.relatedId || related) return ManagedBridgeStatus::INVALID_ARGUMENT;
        m_impl->view.Bind(scene, target);
        struct Reset { ProxyScript& view; ~Reset() { view.Clear(); } } reset{m_impl->view};
        return m_impl->materialTransfers.Open(scene, target, m_impl->view, request, integer, p, t, result);
    }
    if (store && (request.index == 22 || request.index == 26)) {
        if (arguments.relatedId || related) return ManagedBridgeStatus::INVALID_ARGUMENT;
        return m_impl->storeTransfers.Begin(scene, target, request, integer, p, t, result);
    }
    if (request.op >= ManagedBridgeOp::SERVICE_MATERIAL && request.op <= ManagedBridgeOp::SERVICE_CONFIG) {
        if (arguments.relatedId || related) return ManagedBridgeStatus::INVALID_ARGUMENT;
        m_impl->view.Bind(scene, target);
        struct Reset { ProxyScript& view; ~Reset() { view.Clear(); } } reset{m_impl->view};
        return RunManagedAuthoringService(scene, m_impl->view, target, request, integer, p, t, result);
    }
    if (request.op >= ManagedBridgeOp::SERVICE_CURSOR && request.op <= ManagedBridgeOp::SERVICE_SCENE) {
        if (arguments.relatedId || related) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (request.op == ManagedBridgeOp::SERVICE_PARTICLE && request.index >= 13) {
            m_impl->view.Bind(scene, target);
            struct Reset { ProxyScript& view; ~Reset() { view.Clear(); } } reset{m_impl->view};
            return RunParticleServices(m_impl->view, target, request, integer, p, t, result);
        }
        return m_impl->runtimeServices.Run(scene, caller, target, request, integer, p, t, result);
    }
    if (request.op >= ManagedBridgeOp::SERVICE_TERRAIN && request.op <= ManagedBridgeOp::SERVICE_SEQUENCE) {
        if ((arguments.relatedId != 0) != (related != nullptr)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        m_impl->view.Bind(scene, target);
        struct Reset { ProxyScript& view; ~Reset() { view.Clear(); } } reset{m_impl->view};
        return RunWorldFamilyServices(scene, m_impl->view, target, related, request, integer, p, t, result, fixedUpdate);
    }
    /// @note 所有者サービスも予約除去中の row を proxy 経由で読まない。global/2D 操作は row を要求しない。
    uint32_t componentKind = 0;
    switch (request.op) {
    case ManagedBridgeOp::SERVICE_ANIMATOR: componentKind = 5; break;
    case ManagedBridgeOp::SERVICE_CAMERA: componentKind = 8; break;
    case ManagedBridgeOp::SERVICE_NAVIGATION: componentKind = 9; break;
    case ManagedBridgeOp::SERVICE_CHARACTER: componentKind = 10; break;
    case ManagedBridgeOp::COLLIDER_READ:
    case ManagedBridgeOp::COLLIDER_WRITE:
    case ManagedBridgeOp::COLLIDER_COMMAND:
        if (integer > 0 && integer < 32) componentKind = static_cast<uint32_t>(integer);
        break;
    case ManagedBridgeOp::PHYSICS_BODY_READ_ROTATION:
    case ManagedBridgeOp::PHYSICS_BODY_WRITE_ROTATION: componentKind = 1; break;
    case ManagedBridgeOp::SERVICE_AUDIO:
        if (request.index <= 10 || request.index == 12 || (request.index >= 14 && request.index <= 19)
            || (request.index >= 23 && request.index <= 25) || (request.index >= 35 && request.index <= 38)) componentKind = 6;
        break;
    case ManagedBridgeOp::SERVICE_UI:
        if (request.index <= 5 || request.index == 10 || request.index == 17) componentKind = 13;
        else if (request.index == 6 || request.index == 12 || request.index == 13 || request.index == 18
            || (request.index >= 24 && request.index <= 27)) componentKind = 12;
        else if (request.index == 7 || request.index == 14 || request.index == 15 || request.index == 19
            || request.index == 20 || request.index == 28 || (request.index >= 62 && request.index <= 64)) componentKind = 11;
        else if (request.index >= 29 && request.index <= 33) componentKind = 20;
        else if (request.index >= 34 && request.index <= 37) componentKind = 21;
        else if (request.index >= 38 && request.index <= 41) componentKind = 22;
        else if (request.index >= 42 && request.index <= 47) componentKind = 23;
        else if (request.index >= 48 && request.index <= 51) componentKind = 24;
        else if (request.index >= 55 && request.index <= 57) componentKind = 25;
        else if (request.index >= 58 && request.index <= 61) componentKind = 26;
        else if (request.index >= 76 && request.index <= 79) componentKind = 14;
        else if (request.index >= 65 && request.index <= 73) {
            if (ManagedObjectControls::IsPendingComponentRemoval(target, 11)
                || ManagedObjectControls::IsPendingComponentRemoval(target, 12)) return ManagedBridgeStatus::NOT_FOUND;
        } else if (request.index >= 52 && request.index <= 54) {
            for (uint32_t kind : {13U, 20U, 21U, 23U})
                if (ManagedObjectControls::IsPendingComponentRemoval(target, kind)) return ManagedBridgeStatus::NOT_FOUND;
        } else if (request.index == 11 || request.index == 21 || request.index == 75) {
            size_t depth = 0;
            for (GameObject* current = &target; current && depth < scene.EntitySlotCount(); current = current->GetParent(), ++depth)
                if (current->GetComponent<UICanvas>()) {
                    if (ManagedObjectControls::IsPendingComponentRemoval(*current, 14)) return ManagedBridgeStatus::NOT_FOUND;
                    break;
                }
        }
        break;
    default: break;
    }
    if (componentKind && ManagedObjectControls::IsPendingComponentRemoval(target, componentKind)) return ManagedBridgeStatus::NOT_FOUND;
    ManagedServiceResult output;
    if (request.op == ManagedBridgeOp::COLLIDER_READ || request.op == ManagedBridgeOp::COLLIDER_WRITE) {
        const bool writing = request.op == ManagedBridgeOp::COLLIDER_WRITE;
        if (request.flags != (writing ? 0u : 1u) || integer < 0 || integer > UINT32_MAX || arguments.relatedId)
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        const auto status = m_impl->physics.RunCollider(target, static_cast<uint32_t>(integer), request.index, writing, p, t, output, fixedUpdate);
        if (status != ManagedBridgeStatus::OK) return status;
    } else if (request.op == ManagedBridgeOp::COLLIDER_COMMAND) {
        if (request.flags || integer < 0 || integer > UINT32_MAX || arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
        const auto status = m_impl->physics.RunColliderCommand(target, static_cast<uint32_t>(integer), request.index, p, t, output, fixedUpdate);
        if (status != ManagedBridgeStatus::OK) return status;
    } else if (request.op == ManagedBridgeOp::PHYSICS_BODY_READ_ROTATION || request.op == ManagedBridgeOp::PHYSICS_BODY_WRITE_ROTATION) {
        const bool writing = request.op == ManagedBridgeOp::PHYSICS_BODY_WRITE_ROTATION;
        if (request.flags != (writing ? 0u : 1u) || request.index || integer || arguments.relatedId || !t.empty())
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        const auto status = m_impl->physics.RunRotation(target, writing, p, output, fixedUpdate);
        if (status != ManagedBridgeStatus::OK) return status;
    } else {
        m_impl->view.Bind(scene, target);
        struct Reset { ProxyScript& view; ~Reset() { view.Clear(); } } reset{m_impl->view};
        switch (request.op) {
        case ManagedBridgeOp::SERVICE_ANIMATOR:
            if (!target.GetComponent<AnimatorComponent>()) return ManagedBridgeStatus::NOT_FOUND;
            switch (request.index) {
            case 0: {
                /// @note Animator.SetFloat
                if (request.flags != 0u || p.size() != 1 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.animator.SetFloat(t[0], p[0]);
                break;
            }
            case 1: {
                /// @note Animator.GetFloat
                if (request.flags != 1u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {m_impl->view.animator.GetFloat(t[0])};
                break;
            }
            case 2: {
                /// @note Animator.SetInt
                if (request.flags != 0u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (!FitsInt(integer)) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.animator.SetInt(t[0], static_cast<int>(integer));
                break;
            }
            case 3: {
                /// @note Animator.GetInt
                if (request.flags != 1u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.animator.GetInt(t[0]));
                break;
            }
            case 4: {
                /// @note Animator.SetBool
                if (request.flags != 0u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.animator.SetBool(t[0], integer != 0);
                break;
            }
            case 5: {
                /// @note Animator.GetBool
                if (request.flags != 1u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.animator.GetBool(t[0]));
                break;
            }
            case 6: {
                /// @note Animator.SetTrigger
                if (request.flags != 0u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.animator.SetTrigger(t[0]);
                break;
            }
            case 7: {
                /// @note Animator.IsInState
                if (request.flags != 1u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.animator.IsInState(t[0]));
                break;
            }
            case 8: {
                /// @note Animator.GetNormalizedTime
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {m_impl->view.animator.GetNormalizedTime()};
                break;
            }
            case 9: {
                /// @note Animator.GetCurrentState
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.text.emplace_back(m_impl->view.animator.GetCurrentState());
                break;
            }
            case 10: {
                /// @note Animator.GetBlendToState
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.text.emplace_back(m_impl->view.animator.GetBlendToState());
                break;
            }
            case 11: {
                /// @note Animator.GetBlendToNormalizedTime
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {m_impl->view.animator.GetBlendToNormalizedTime()};
                break;
            }
            case 12: {
                /// @note Animator.GetSpeed
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {m_impl->view.animator.GetSpeed()};
                break;
            }
            case 13: {
                /// @note Animator.IsPlaying
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.animator.IsPlaying());
                break;
            }
            case 14: {
                /// @note Animator.GetRootMotionDeltaPosition
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                const auto value = m_impl->view.animator.GetRootMotionDeltaPosition(); output.values = {value.x, value.y, value.z};
                break;
            }
            case 15: {
                /// @note Animator.GetRootMotionDeltaRotation
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                const auto value = m_impl->view.animator.GetRootMotionDeltaRotation(); output.values = {value.x, value.y, value.z, value.w};
                break;
            }
            case 16: {
                /// @note Animator.GetRootMotionWorldDelta
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                const auto value = m_impl->view.animator.GetRootMotionWorldDelta(); output.values = {value.x, value.y, value.z};
                break;
            }
            case 17: {
                /// @note Animator.GetRootMotionWorldVelocity
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                const auto value = m_impl->view.animator.GetRootMotionWorldVelocity(); output.values = {value.x, value.y, value.z};
                break;
            }
            case 18: {
                /// @note Animator.GetRootMotionDeltaTime
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {m_impl->view.animator.GetRootMotionDeltaTime()};
                break;
            }
            case 19: {
                /// @note Animator.IsRootMotionAppliedByEngine
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.animator.IsRootMotionAppliedByEngine());
                break;
            }
            case 20: {
                /// @note Animator.GetRootMotionMode
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.animator.GetRootMotionMode());
                break;
            }
            case 21: {
                /// @note Animator.GetRootMotionPositionScale
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {m_impl->view.animator.GetRootMotionPositionScale()};
                break;
            }
            case 22: {
                /// @note Animator.GetRootMotionRotationScale
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {m_impl->view.animator.GetRootMotionRotationScale()};
                break;
            }
            case 23: {
                /// @note Animator.GetRootMotionNodeName
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.text.emplace_back(m_impl->view.animator.GetRootMotionNodeName());
                break;
            }
            case 24: {
                /// @note Animator.GetCurrentBlendWeights
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.blendWeights = target.GetComponent<AnimatorComponent>()->currentBlendWeights;
                break;
            }
            case 25: {
                /// @note Animator.SetSpeed
                if (request.flags != 0u || p.size() != 1 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.animator.SetSpeed(p[0]);
                break;
            }
            case 26: {
                /// @note Animator.SetLocalTimeScale
                if (request.flags != 0u || p.size() != 1 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (p[0] < 0 || p[0] > 8) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.animator.SetLocalTimeScale(p[0]);
                break;
            }
            case 27: {
                /// @note Animator.Play
                if (request.flags != 0u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.animator.Play(t[0]);
                break;
            }
            case 28: {
                /// @note Animator.SetPlaying
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.animator.SetPlaying(integer != 0);
                break;
            }
            case 29: {
                /// @note Animator.SetLayerWeight
                if (request.flags != 0u || p.size() != 1 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.animator.SetLayerWeight(t[0], p[0]);
                break;
            }
            case 30: {
                /// @note Animator.GetLayerWeight
                if (request.flags != 1u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {m_impl->view.animator.GetLayerWeight(t[0])};
                break;
            }
            case 31: {
                /// @note Animator.GetLayerState
                if (request.flags != 1u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.text.emplace_back(m_impl->view.animator.GetLayerState(t[0]));
                break;
            }
            case 32: {
                /// @note Animator.IsLayerInState
                if (request.flags != 1u || p.size() != 0 || t.size() != 2) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.animator.IsLayerInState(t[0], t[1]));
                break;
            }
            case 33: {
                /// @note Animator.PlayLayerState
                if (request.flags != 0u || p.size() != 0 || t.size() != 2) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.animator.PlayLayerState(t[0], t[1]);
                break;
            }
            case 34: {
                /// @note Animator.SetLayerMask
                if (request.flags != 0u || p.size() != 0 || t.size() != 2) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.animator.SetLayerMask(t[0], t[1]);
                break;
            }
            case 35: {
                /// @note Animator.GetLayerMask
                if (request.flags != 1u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.text.emplace_back(m_impl->view.animator.GetLayerMask(t[0]));
                break;
            }
            case 36: {
                /// @note Animator.PlaySlot
                if (request.flags != 0u || p.size() != 3 || t.size() != 3) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.animator.PlaySlot(t[0], t[1], t[2], p[0], p[1], p[2], integer != 0);
                break;
            }
            case 37: {
                /// @note Animator.StopSlot
                if (request.flags != 0u || p.size() != 1 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.animator.StopSlot(t[0], p[0]);
                break;
            }
            case 38: {
                /// @note Animator.IsSlotPlaying
                if (request.flags != 1u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.animator.IsSlotPlaying(t[0]));
                break;
            }
            case 39: {
                /// @note Animator.GetSlotWeight
                if (request.flags != 1u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {m_impl->view.animator.GetSlotWeight(t[0])};
                break;
            }
            case 40: {
                /// @note Animator.SetSlotSpeed
                if (request.flags != 0u || p.size() != 1 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.animator.SetSlotSpeed(t[0], p[0]);
                break;
            }
            case 41: {
                /// @note Animator.GetSlotTime
                if (request.flags != 1u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {m_impl->view.animator.GetSlotTime(t[0])};
                break;
            }
            case 42: {
                /// @note Animator.SetStateSpeed
                if (request.flags != 0u || p.size() != 1 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.animator.SetStateSpeed(t[0], p[0]);
                break;
            }
            case 43: {
                /// @note Animator.GetStateSpeed
                if (request.flags != 1u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {m_impl->view.animator.GetStateSpeed(t[0])};
                break;
            }
            case 44: {
                /// @note Animator.SetMorphWeight
                if (request.flags != 0u || p.size() != 1 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.animator.SetMorphWeight(t[0], p[0]);
                break;
            }
            case 45: {
                /// @note Animator.GetMorphWeight
                if (request.flags != 1u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {m_impl->view.animator.GetMorphWeight(t[0])};
                break;
            }
            case 46: {
                /// @note Animator.SetRootMotionMode
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (!FitsInt(integer)) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.animator.SetRootMotionMode(static_cast<int>(integer));
                break;
            }
            case 47: {
                /// @note Animator.SetRootMotionPositionScale
                if (request.flags != 0u || p.size() != 1 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.animator.SetRootMotionPositionScale(p[0]);
                break;
            }
            case 48: {
                /// @note Animator.SetRootMotionRotationScale
                if (request.flags != 0u || p.size() != 1 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.animator.SetRootMotionRotationScale(p[0]);
                break;
            }
            case 49: {
                /// @note Animator.SetRootMotionNodeName
                if (request.flags != 0u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.animator.SetRootMotionNodeName(t[0]);
                break;
            }
            case 50: {
                if (request.flags != 0u || p.size() != 4 || !t.empty() || arguments.relatedId
                    || (integer != 0 && integer != 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (!target.GetComponent<AnimatorComponent>()->SetUpdateLod(integer != 0, p[0], p[1], p[2], p[3]))
                    return ManagedBridgeStatus::INVALID_ARGUMENT;
                break;
            }
            default: return ManagedBridgeStatus::UNSUPPORTED;
            }
            break;
        case ManagedBridgeOp::SERVICE_AUDIO:
            if (request.index >= 46) {
                const auto status = RunAudioSourceSettings(target, request, integer, p, t, arguments.relatedId, output);
                if (status != ManagedBridgeStatus::OK) return status;
                break;
            }
            if (request.index >= 29) {
                const auto status = m_impl->AudioAdvanced(scene, caller, target, request, arguments, output);
                if (status != ManagedBridgeStatus::OK) return status;
                break;
            }
            if ((request.index <= 10 || request.index == 12 || (request.index >= 14 && request.index <= 19)
                || request.index == 23 || request.index == 24 || request.index == 25) && !target.GetComponent<AudioSourceComponent>())
                return ManagedBridgeStatus::NOT_FOUND;
            switch (request.index) {
            case 0: {
                /// @note AudioSource.Play
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.audio.Play();
                break;
            }
            case 1: {
                /// @note AudioSource.Stop
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.audio.Stop();
                break;
            }
            case 2: {
                /// @note AudioSource.Pause
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.audio.Pause();
                break;
            }
            case 3: {
                /// @note AudioSource.Resume
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.audio.Resume();
                break;
            }
            case 4: {
                /// @note AudioSource.IsPlaying
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.audio.IsPlaying());
                break;
            }
            case 5: {
                /// @note AudioSource.GetVolume
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {m_impl->view.audio.GetVolume()};
                break;
            }
            case 6: {
                /// @note AudioSource.GetPitch
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {m_impl->view.audio.GetPitch()};
                break;
            }
            case 7: {
                /// @note AudioSource.GetLoop
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.audio.GetLoop());
                break;
            }
            case 8: {
                /// @note AudioSource.IsPaused
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.audio.IsPaused());
                break;
            }
            case 9: {
                /// @note AudioSource.GetSpatialBlend
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {m_impl->view.audio.GetSpatialBlend()};
                break;
            }
            case 10: {
                /// @note AudioSource.GetBus
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.text.emplace_back(m_impl->view.audio.GetBus());
                break;
            }
            case 11: {
                /// @note AudioSource.IsBGMPlaying
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.audio.IsBGMPlaying());
                break;
            }
            case 12: {
                /// @note AudioSource.Play
                if (request.flags != 0u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.audio.Play(t[0]);
                break;
            }
            case 13: {
                /// @note AudioSource.Preload
                if (request.flags != 0u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.audio.Preload(t[0]));
                break;
            }
            case 14: {
                /// @note AudioSource.SetVolume
                if (request.flags != 0u || p.size() != 1 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.audio.SetVolume(p[0]);
                break;
            }
            case 15: {
                /// @note AudioSource.SetLoop
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.audio.SetLoop(integer != 0);
                break;
            }
            case 16: {
                /// @note AudioSource.SetPitch
                if (request.flags != 0u || p.size() != 1 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.audio.SetPitch(p[0]);
                break;
            }
            case 17: {
                /// @note AudioSource.SetSpatialBlend
                if (request.flags != 0u || p.size() != 1 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.audio.SetSpatialBlend(p[0]);
                break;
            }
            case 18: {
                /// @note AudioSource.Set3DDistances
                if (request.flags != 0u || p.size() != 3 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.audio.Set3DDistances(p[0], p[1], p[2]);
                break;
            }
            case 19: {
                /// @note AudioSource.PlayOneShot
                if (request.flags != 0u || p.size() != 1 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.audio.PlayOneShot(t[0], p[0]);
                break;
            }
            case 20: {
                /// @note AudioSource.PlayAtPoint
                if (request.flags != 0u || p.size() != 4 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.audio.PlayAtPoint(t[0], math::Vector3{p[0], p[1], p[2]}, p[3]);
                break;
            }
            case 21: {
                /// @note AudioSource.PlayBGM
                if (request.flags != 0u || p.size() != 1 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.audio.PlayBGM(t[0], integer != 0, p[0]);
                break;
            }
            case 22: {
                /// @note AudioSource.StopBGM
                if (request.flags != 0u || p.size() != 1 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.audio.StopBGM(p[0]);
                break;
            }
            case 23: {
                /// @note AudioSource.FadeTo
                if (request.flags != 0u || p.size() != 2 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.audio.FadeTo(p[0], p[1]);
                break;
            }
            case 24: {
                /// @note AudioSource.FadeOutAndStop
                if (request.flags != 0u || p.size() != 1 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.audio.FadeOutAndStop(p[0]);
                break;
            }
            case 25: {
                /// @note AudioSource.SetBus
                if (request.flags != 0u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.audio.SetBus(t[0]);
                break;
            }
            case 26: {
                /// @note AudioSource.SetBusVolume
                if (request.flags != 0u || p.size() != 1 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.audio.SetBusVolume(t[0], p[0]);
                break;
            }
            case 27: {
                /// @note AudioSource.GetBusVolume
                if (request.flags != 1u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {m_impl->view.audio.GetBusVolume(t[0])};
                break;
            }
            case 28: {
                /// @note AudioSource.SetBusLowPass
                if (request.flags != 0u || p.size() != 1 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.audio.SetBusLowPass(t[0], p[0]);
                break;
            }
            default: return ManagedBridgeStatus::UNSUPPORTED;
            }
            break;
        case ManagedBridgeOp::SERVICE_UI:
            {
                bool present = false;
                if (request.index <= 5 || request.index == 10 || request.index == 17) present = target.GetComponent<UIButton>() != nullptr;
                else if (request.index == 6 || request.index == 12 || request.index == 13 || request.index == 18 || (request.index >= 24 && request.index <= 27)) present = target.GetComponent<UIImage>() != nullptr;
                else if (request.index == 7 || request.index == 14 || request.index == 15 || request.index == 19 || request.index == 20 || request.index == 28 || (request.index >= 62 && request.index <= 64)) present = target.GetComponent<UIText>() != nullptr;
                else if (request.index == 21 || (request.index >= 76 && request.index <= 79)) present = target.GetComponent<UICanvas>() != nullptr;
                else if (request.index >= 29 && request.index <= 33) present = target.GetComponent<UISlider>() != nullptr;
                else if (request.index >= 34 && request.index <= 37) present = target.GetComponent<UIToggle>() != nullptr;
                else if (request.index >= 38 && request.index <= 41) present = target.GetComponent<UIScrollView>() != nullptr;
                else if (request.index >= 42 && request.index <= 47) present = target.GetComponent<UIInputField>() != nullptr;
                else if (request.index >= 48 && request.index <= 51) present = target.GetComponent<UICanvasGroup>() != nullptr;
                else if (request.index >= 55 && request.index <= 57) present = target.GetComponent<UIDragSource>() != nullptr;
                else if (request.index >= 58 && request.index <= 61) present = target.GetComponent<UIDropTarget>() != nullptr;
                else present = target.GetComponent<UIButton>() || target.GetComponent<UIImage>() || target.GetComponent<UIText>()
                    || target.GetComponent<UICanvas>() || target.GetComponent<UISlider>() || target.GetComponent<UIToggle>()
                    || target.GetComponent<UIScrollView>() || target.GetComponent<UIInputField>() || target.GetComponent<UICanvasGroup>()
                    || target.GetComponent<UIDragSource>() || target.GetComponent<UIDropTarget>();
                if (!present) return ManagedBridgeStatus::NOT_FOUND;
            }
            switch (request.index) {
            case 0: {
                /// @note UI.WasClicked
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.ui.WasClicked());
                break;
            }
            case 1: {
                /// @note UI.WasHoverEnter
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.ui.WasHoverEnter());
                break;
            }
            case 2: {
                /// @note UI.WasHoverExit
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.ui.WasHoverExit());
                break;
            }
            case 3: {
                /// @note UI.IsHovered
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.ui.IsHovered());
                break;
            }
            case 4: {
                /// @note UI.IsPressed
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.ui.IsPressed());
                break;
            }
            case 5: {
                /// @note UI.IsInteractable
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.ui.IsInteractable());
                break;
            }
            case 6: {
                /// @note UI.IsImageEnabled
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.ui.IsImageEnabled());
                break;
            }
            case 7: {
                /// @note UI.IsTextEnabled
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.ui.IsTextEnabled());
                break;
            }
            case 8: {
                /// @note UI.IsPointerOverridden
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.ui.IsPointerOverridden());
                break;
            }
            case 9: {
                /// @note UI.IsPointerOverUI
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.ui.IsPointerOverUI());
                break;
            }
            case 10: {
                /// @note UI.GetButtonPhase
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.ui.GetButtonPhase());
                break;
            }
            case 11: {
                /// @note UI.GetCanvasMousePosition
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                const auto value = m_impl->view.ui.GetCanvasMousePosition(); output.values = {value.x, value.y};
                break;
            }
            case 12: {
                /// @note UI.GetImageFillAmount
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {m_impl->view.ui.GetImageFillAmount()};
                break;
            }
            case 13: {
                /// @note UI.GetImageColor
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                const auto value = m_impl->view.ui.GetImageColor(); output.values = {value.x, value.y, value.z, value.w};
                break;
            }
            case 14: {
                /// @note UI.GetText
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.text.emplace_back(m_impl->view.ui.GetText());
                break;
            }
            case 15: {
                /// @note UI.GetTextColor
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                const auto value = m_impl->view.ui.GetTextColor(); output.values = {value.x, value.y, value.z, value.w};
                break;
            }
            case 16: {
                /// @note UI.GetFocus
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.object = m_impl->view.ui.GetFocus();
                break;
            }
            case 17: {
                /// @note UI.SetButtonInteractable
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetButtonInteractable(integer != 0);
                break;
            }
            case 18: {
                /// @note UI.SetImageColor
                if (request.flags != 0u || p.size() != 4 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetImageColor(math::Vector4{p[0], p[1], p[2], p[3]});
                break;
            }
            case 19: {
                /// @note UI.SetText
                if (request.flags != 0u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetText(t[0]);
                break;
            }
            case 20: {
                /// @note UI.SetTextColor
                if (request.flags != 0u || p.size() != 4 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetTextColor(math::Vector4{p[0], p[1], p[2], p[3]});
                break;
            }
            case 21: {
                /// @note UI.SetCanvasSortOrder
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (!FitsInt(integer)) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetCanvasSortOrder(static_cast<int>(integer));
                break;
            }
            case 22: {
                /// @note UI.SetPointer
                if (request.flags != 0u || p.size() != 2 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetPointer(math::Vector2{p[0], p[1]}, integer != 0);
                break;
            }
            case 23: {
                /// @note UI.ClearPointer
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.ClearPointer();
                break;
            }
            case 24: {
                /// @note UI.SetImageSpriteRect
                if (request.flags != 0u || p.size() != 6 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetImageSpriteRect(p[0], p[1], p[2], p[3], p[4], p[5]);
                break;
            }
            case 25: {
                /// @note UI.SetImageTexture
                if (request.flags != 0u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetImageTexture(t[0]);
                break;
            }
            case 26: {
                /// @note UI.SetImageFillAmount
                if (request.flags != 0u || p.size() != 1 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetImageFillAmount(p[0]);
                break;
            }
            case 27: {
                /// @note UI.SetImageEnabled
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetImageEnabled(&target, integer != 0);
                break;
            }
            case 28: {
                /// @note UI.SetTextEnabled
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetTextEnabled(&target, integer != 0);
                break;
            }
            case 29: {
                /// @note UI.GetSliderValue
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {m_impl->view.ui.GetSliderValue(&target)};
                break;
            }
            case 30: {
                /// @note UI.SetSliderValue
                if (request.flags != 0u || p.size() != 1 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetSliderValue(&target, p[0]);
                break;
            }
            case 31: {
                /// @note UI.WasSliderChanged
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.ui.WasSliderChanged(&target));
                break;
            }
            case 32: {
                /// @note UI.SetSliderRange
                if (request.flags != 0u || p.size() != 2 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetSliderRange(&target, p[0], p[1]);
                break;
            }
            case 33: {
                /// @note UI.SetSliderInteractable
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetSliderInteractable(&target, integer != 0);
                break;
            }
            case 34: {
                /// @note UI.IsToggleOn
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.ui.IsToggleOn(&target));
                break;
            }
            case 35: {
                /// @note UI.SetToggleOn
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetToggleOn(&target, integer != 0);
                break;
            }
            case 36: {
                /// @note UI.WasToggleChanged
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.ui.WasToggleChanged(&target));
                break;
            }
            case 37: {
                /// @note UI.SetToggleInteractable
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetToggleInteractable(&target, integer != 0);
                break;
            }
            case 38: {
                /// @note UI.GetScrollPosition
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                const auto value = m_impl->view.ui.GetScrollPosition(&target); output.values = {value.x, value.y};
                break;
            }
            case 39: {
                /// @note UI.SetScrollPosition
                if (request.flags != 0u || p.size() != 2 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetScrollPosition(&target, math::Vector2{p[0], p[1]});
                break;
            }
            case 40: {
                /// @note UI.SetScrollContentSize
                if (request.flags != 0u || p.size() != 2 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetScrollContentSize(&target, math::Vector2{p[0], p[1]});
                break;
            }
            case 41: {
                /// @note UI.WasScrollChanged
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.ui.WasScrollChanged(&target));
                break;
            }
            case 42: {
                /// @note UI.GetFieldText
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.text.emplace_back(m_impl->view.ui.GetFieldText(&target));
                break;
            }
            case 43: {
                /// @note UI.SetFieldText
                if (request.flags != 0u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetFieldText(&target, t[0]);
                break;
            }
            case 44: {
                /// @note UI.WasFieldChanged
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.ui.WasFieldChanged(&target));
                break;
            }
            case 45: {
                /// @note UI.WasSubmitted
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.ui.WasSubmitted(&target));
                break;
            }
            case 46: {
                /// @note UI.IsFieldFocused
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.ui.IsFieldFocused(&target));
                break;
            }
            case 47: {
                /// @note UI.SetFieldFocused
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetFieldFocused(&target, integer != 0);
                break;
            }
            case 48: {
                /// @note UI.GetGroupAlpha
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {m_impl->view.ui.GetGroupAlpha(&target)};
                break;
            }
            case 49: {
                /// @note UI.SetGroupAlpha
                if (request.flags != 0u || p.size() != 1 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetGroupAlpha(&target, p[0]);
                break;
            }
            case 50: {
                /// @note UI.SetGroupInteractable
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetGroupInteractable(&target, integer != 0);
                break;
            }
            case 51: {
                /// @note UI.SetGroupBlocksRaycasts
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer != 0 && integer != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetGroupBlocksRaycasts(&target, integer != 0);
                break;
            }
            case 52: {
                /// @note UI.SetFocus
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetFocus(&target);
                break;
            }
            case 53: {
                /// @note UI.IsFocused
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.ui.IsFocused(&target));
                break;
            }
            case 54: {
                /// @note UI.ClearFocus
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.ClearFocus(&target);
                break;
            }
            case 55: {
                /// @note UI.IsDragging
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.ui.IsDragging(&target));
                break;
            }
            case 56: {
                /// @note UI.WasDropped
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.ui.WasDropped(&target));
                break;
            }
            case 57: {
                /// @note UI.GetDropTarget
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.object = m_impl->view.ui.GetDropTarget(&target);
                break;
            }
            case 58: {
                /// @note UI.WasReceived
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.ui.WasReceived(&target));
                break;
            }
            case 59: {
                /// @note UI.GetReceivedFrom
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.object = m_impl->view.ui.GetReceivedFrom(&target);
                break;
            }
            case 60: {
                /// @note UI.GetReceivedPayload
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.text.emplace_back(m_impl->view.ui.GetReceivedPayload(&target));
                break;
            }
            case 61: {
                /// @note UI.IsDropHovered
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.ui.IsDropHovered(&target));
                break;
            }
            case 62: {
                /// @note UI.SetVisibleCharacters
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (!FitsInt(integer)) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetVisibleCharacters(&target, static_cast<int>(integer));
                break;
            }
            case 63: {
                /// @note UI.GetVisibleCharacters
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.ui.GetVisibleCharacters(&target));
                break;
            }
            case 64: {
                /// @note UI.GetCharacterCount
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.ui.GetCharacterCount(&target));
                break;
            }
            case 65: {
                /// @note UI.SetMaterial
                if (request.flags != 0u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetMaterial(&target, t[0]);
                break;
            }
            case 66: {
                /// @note UI.SetMaterialFloat
                if (request.flags != 0u || p.size() != 1 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetMaterialFloat(&target, t[0], p[0]);
                break;
            }
            case 67: {
                /// @note UI.SetMaterialVector2
                if (request.flags != 0u || p.size() != 2 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetMaterialVector2(&target, t[0], p[0], p[1]);
                break;
            }
            case 68: {
                /// @note UI.SetMaterialVector4
                if (request.flags != 0u || p.size() != 4 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetMaterialVector4(&target, t[0], math::Vector4{p[0], p[1], p[2], p[3]});
                break;
            }
            case 69: {
                /// @note UI.SetMaterialColor
                if (request.flags != 0u || p.size() != 4 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetMaterialColor(&target, t[0], math::Vector4{p[0], p[1], p[2], p[3]});
                break;
            }
            case 70: {
                /// @note UI.SetMaterialTexture
                if (request.flags != 0u || p.size() != 0 || t.size() != 2) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.SetMaterialTexture(&target, t[0], t[1]);
                break;
            }
            case 71: {
                /// @note UI.ClearMaterialOverride
                if (request.flags != 0u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.ClearMaterialOverride(&target, t[0]);
                break;
            }
            case 72: {
                /// @note UI.ClearMaterialOverrides
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.ui.ClearMaterialOverrides(&target);
                break;
            }
            case 73: {
                /// @note UI.HasMaterialOverride
                if (request.flags != 1u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.ui.HasMaterialOverride(&target, t[0]));
                break;
            }
            case 74: {
                /// @note UI.Find
                if (request.flags != 1u || p.size() != 0 || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.object = m_impl->view.ui.Find(&target, t[0]);
                break;
            }
            case 75: {
                /// @note UI.TryGetCanvasSize
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                math::Vector2 value;
                output.integer = m_impl->view.ui.TryGetCanvasSize(value, &target) ? 1 : 0;
                if (output.integer) output.values = {value.x, value.y};
                break;
            }
            case 76: {
                if (request.flags != 1u || integer || !p.empty() || !t.empty() || arguments.relatedId)
                    return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(target.GetComponent<UICanvas>()->depthMode);
                break;
            }
            case 77: {
                if (request.flags || integer < 0 || integer > 1 || !p.empty() || !t.empty() || arguments.relatedId)
                    return ManagedBridgeStatus::INVALID_ARGUMENT;
                target.GetComponent<UICanvas>()->depthMode = static_cast<UICanvasDepthMode>(integer);
                break;
            }
            case 78: {
                if (request.flags != 1u || integer || !p.empty() || !t.empty() || arguments.relatedId)
                    return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {target.GetComponent<UICanvas>()->surfaceOffset};
                break;
            }
            case 79: {
                if (request.flags || integer || p.size() != 1 || !t.empty() || arguments.relatedId
                    || !std::isfinite(p[0]) || p[0] < -1.0f || p[0] > 1.0f)
                    return ManagedBridgeStatus::INVALID_ARGUMENT;
                target.GetComponent<UICanvas>()->surfaceOffset = p[0];
                break;
            }
            default: return ManagedBridgeStatus::UNSUPPORTED;
            }
            break;
        case ManagedBridgeOp::SERVICE_CAMERA:
            if (!target.GetComponent<CameraComponent>()) return ManagedBridgeStatus::NOT_FOUND;
            if (request.index == 7 || request.index == 8 || request.index >= 17) {
                if (request.index > 20) return ManagedBridgeStatus::UNSUPPORTED;
                const auto status = CameraProjection(target, request, arguments, output);
                if (status != ManagedBridgeStatus::OK) return status;
                break;
            }
            switch (request.index) {
            case 0: {
                /// @note Camera.SetAsMain
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.camera.SetAsMain();
                break;
            }
            case 1: {
                /// @note Camera.SetFOV
                if (request.flags != 0u || p.size() != 1 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (p[0] <= 0 || p[0] >= 180) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.camera.SetFOV(p[0]);
                break;
            }
            case 2: {
                /// @note Camera.SetAspectRatio
                if (request.flags != 0u || p.size() != 1 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (p[0] <= math::EPSILON) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.camera.SetAspectRatio(p[0]);
                break;
            }
            case 3: {
                /// @note Camera.SetNearFar
                if (request.flags != 0u || p.size() != 2 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (p[0] <= 0 || p[1] <= p[0] || !std::isfinite(p[0] * p[1] / (p[1] - p[0]))) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.camera.SetNearFar(p[0], p[1]);
                break;
            }
            case 4: {
                /// @note Camera.SetCullingMask
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer < 0 || integer > UINT32_MAX) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.camera.SetCullingMask(static_cast<uint32_t>(integer));
                break;
            }
            case 5: {
                /// @note Camera.SetBackgroundColor
                if (request.flags != 0u || p.size() != 4 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.camera.SetBackgroundColor(math::Vector4{p[0], p[1], p[2], p[3]});
                break;
            }
            case 6: {
                /// @note Camera.SetClearMode
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer < 0 || integer > 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.camera.SetClearMode(static_cast<renderer::CameraClearMode>(integer));
                break;
            }
            case 7: {
                /// @note Camera.WorldToScreenPoint
                if (request.flags != 1u || p.size() != 3 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                const auto value = m_impl->view.camera.WorldToScreenPoint(math::Vector3{p[0], p[1], p[2]}); output.values = {value.x, value.y, value.z};
                break;
            }
            case 8: {
                /// @note Camera.ScreenToWorldPoint
                if (request.flags != 1u || p.size() != 3 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                const auto value = m_impl->view.camera.ScreenToWorldPoint(math::Vector3{p[0], p[1], p[2]}); output.values = {value.x, value.y, value.z};
                break;
            }
            case 9: {
                /// @note Camera.GetFOV
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {m_impl->view.camera.GetFOV()};
                break;
            }
            case 10: {
                /// @note Camera.GetNearZ
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {m_impl->view.camera.GetNearZ()};
                break;
            }
            case 11: {
                /// @note Camera.GetFarZ
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {m_impl->view.camera.GetFarZ()};
                break;
            }
            case 12: {
                /// @note Camera.GetAspectRatio
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {m_impl->view.camera.GetAspectRatio()};
                break;
            }
            case 13: {
                /// @note Camera.GetCullingMask
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.camera.GetCullingMask());
                break;
            }
            case 14: {
                /// @note Camera.GetBackgroundColor
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                const auto value = m_impl->view.camera.GetBackgroundColor(); output.values = {value.x, value.y, value.z, value.w};
                break;
            }
            case 15: {
                /// @note Camera.GetClearMode
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.camera.GetClearMode());
                break;
            }
            case 16: {
                /// @note Camera.IsMain
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.camera.IsMain());
                break;
            }
            case 17: {
                /// @note Camera.IsVisible
                if (request.flags != 1u || p.size() != 3 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.camera.IsVisible(math::Vector3{p[0], p[1], p[2]}));
                break;
            }
            case 18: {
                /// @note Camera.ScreenPointToRay
                if (request.flags != 1u || p.size() != 2 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                const auto value = m_impl->view.camera.ScreenPointToRay(p[0], p[1]); output.values = {value.origin.x, value.origin.y, value.origin.z, value.direction.x, value.direction.y, value.direction.z};
                break;
            }
            case 19: {
                /// @note Camera.TryWorldToViewportPoint
                if (request.flags != 1u || p.size() != 3 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                math::Vector3 value;
                output.integer = m_impl->view.camera.TryWorldToViewportPoint(math::Vector3{p[0], p[1], p[2]}, value, &target) ? 1 : 0;
                if (output.integer) output.values = {value.x, value.y, value.z};
                break;
            }
            case 20: {
                /// @note Camera.TryViewportToWorldPoint
                if (request.flags != 1u || p.size() != 3 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                math::Vector3 value;
                output.integer = m_impl->view.camera.TryViewportToWorldPoint(math::Vector2{p[0], p[1]}, p[2], value, &target) ? 1 : 0;
                if (output.integer) output.values = {value.x, value.y, value.z};
                break;
            }
            default: return ManagedBridgeStatus::UNSUPPORTED;
            }
            break;
        case ManagedBridgeOp::SERVICE_NAVIGATION:
            if (!target.GetComponent<NavMeshAgentComponent>()) return ManagedBridgeStatus::NOT_FOUND;
            switch (request.index) {
            case 0: {
                /// @note NavMeshAgent.SetDestination
                if (request.flags != 0u || p.size() != 3 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.navigation.SetDestination(math::Vector3{p[0], p[1], p[2]});
                break;
            }
            case 1: {
                /// @note NavMeshAgent.CancelPath
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.navigation.CancelPath();
                break;
            }
            case 2: {
                /// @note NavMeshAgent.Pause
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.navigation.Pause();
                break;
            }
            case 3: {
                /// @note NavMeshAgent.Resume
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.navigation.Resume();
                break;
            }
            case 4: {
                /// @note NavMeshAgent.Warp
                if (request.flags != 0u || p.size() != 3 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                const auto status = m_impl->physics.MoveWorld(target, {p[0], p[1], p[2]}, fixedUpdate);
                if (status != ManagedBridgeStatus::OK) return status;
                target.GetComponent<NavMeshAgentComponent>()->Stop();
                break;
            }
            case 5: {
                /// @note NavMeshAgent.SetTarget
                if (request.flags != 0u || p.size() != 1 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (!related || !arguments.relatedId) return ManagedBridgeStatus::NOT_FOUND;
                m_impl->view.navigation.SetTarget(*related, p[0]);
                break;
            }
            case 6: {
                /// @note NavMeshAgent.ClearTarget
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.navigation.ClearTarget();
                break;
            }
            case 7: {
                /// @note NavMeshAgent.IsFollowingTarget
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.navigation.IsFollowingTarget());
                break;
            }
            case 8: {
                /// @note NavMeshAgent.BakeNavMesh
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.navigation.BakeNavMesh();
                break;
            }
            case 9: {
                /// @note NavMeshAgent.IsPaused
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.navigation.IsPaused());
                break;
            }
            case 10: {
                /// @note NavMeshAgent.IsMoving
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.navigation.IsMoving());
                break;
            }
            case 11: {
                /// @note NavMeshAgent.HasPath
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.navigation.HasPath());
                break;
            }
            case 12: {
                /// @note NavMeshAgent.HasArrived
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.navigation.HasArrived());
                break;
            }
            case 13: {
                /// @note NavMeshAgent.IsStuck
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.navigation.IsStuck());
                break;
            }
            case 14: {
                /// @note NavMeshAgent.GetRemainingDistance
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {m_impl->view.navigation.GetRemainingDistance()};
                break;
            }
            case 15: {
                /// @note NavMeshAgent.GetVelocity
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                const auto value = m_impl->view.navigation.GetVelocity(); output.values = {value.x, value.y, value.z};
                break;
            }
            case 16: {
                /// @note NavMeshAgent.SetSpeed
                if (request.flags != 0u || p.size() != 1 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.navigation.SetSpeed(p[0]);
                break;
            }
            case 17: {
                /// @note NavMeshAgent.GetSpeed
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {m_impl->view.navigation.GetSpeed()};
                break;
            }
            case 18: {
                /// @note NavMeshAgent.SetAngularSpeed
                if (request.flags != 0u || p.size() != 1 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.navigation.SetAngularSpeed(p[0]);
                break;
            }
            case 19: {
                /// @note NavMeshAgent.GetAngularSpeed
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {m_impl->view.navigation.GetAngularSpeed()};
                break;
            }
            case 20: {
                /// @note NavMeshAgent.GetDestination
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                const auto value = m_impl->view.navigation.GetDestination(); output.values = {value.x, value.y, value.z};
                break;
            }
            case 21: {
                /// @note NavMeshAgent.GetStoppingDistance
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.values = {m_impl->view.navigation.GetStoppingDistance()};
                break;
            }
            case 22: {
                /// @note NavMeshAgent.SetStoppingDistance
                if (request.flags != 0u || p.size() != 1 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.navigation.SetStoppingDistance(p[0]);
                break;
            }
            case 23: {
                /// @note NavMeshAgent.GetAreaMask
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.navigation.GetAreaMask());
                break;
            }
            case 24: {
                /// @note NavMeshAgent.SetAreaMask
                if (request.flags != 0u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (!FitsInt(integer)) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                m_impl->view.navigation.SetAreaMask(static_cast<int>(integer));
                break;
            }
            case 25: {
                /// @note NavMeshAgent.CanSeeTarget
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.integer = static_cast<int64_t>(m_impl->view.navigation.CanSeeTarget());
                break;
            }
            case 26: {
                /// @note NavMeshAgent.GetDetectedTarget
                if (request.flags != 1u || p.size() != 0 || t.size() != 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
                if (arguments.relatedId) return ManagedBridgeStatus::INVALID_ARGUMENT;
                output.object = m_impl->view.navigation.GetDetectedTarget();
                break;
            }
            default: return ManagedBridgeStatus::UNSUPPORTED;
            }
            break;
        case ManagedBridgeOp::SERVICE_CHARACTER: {
            const auto status = CharacterOperation(target, request, arguments, output, fixedUpdate, collisionCallback);
            if (status != ManagedBridgeStatus::OK) return status;
            break;
        }
        default: return ManagedBridgeStatus::UNSUPPORTED;
        }
    }
    for (float value : output.values) if (!std::isfinite(value)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (output.object && (!output.object->IsValid() || output.object->GetScene() != &scene)) return ManagedBridgeStatus::NOT_FOUND;
    result = std::move(output);
    return ManagedBridgeStatus::OK;
}
void ManagedServicesBridge::EndCall(uint64_t call)
{
    m_impl->storeTransfers.EndCall(call); m_impl->materialTransfers.EndCall(call); m_impl->jointTransfers.EndCall(call);
    m_impl->componentTransfers.EndCall(call); m_impl->renderTransfers.EndCall(call);
    m_impl->assetServices.EndCall(call);
}
ManagedBridgeStatus ManagedServicesBridge::ReadMaterialPacket(Scene& scene, Script& caller, GameObject& target,
    const ManagedBridgeRequest& request, ManagedBridgeReply& reply)
{
    m_impl->view.Bind(scene, target);
    struct Reset { ProxyScript& view; ~Reset() { view.Clear(); } } reset{m_impl->view};
    ManagedServiceResult ignored;
    return m_impl->materialTransfers.Run(scene, target, m_impl->view, request, ignored, &reply);
}

ManagedBridgeStatus ManagedServicesBridge::RunJointPacket(Scene& scene, GameObject& target, const ManagedBridgeRequest& request,
    ManagedServiceResult& result, ManagedBridgeStatus (*resolver)(void*, uint64_t, GameObject*&), void* context, bool fixed)
{ return m_impl->jointTransfers.Run(scene, target, request, result, resolver, context, fixed); }
} /// @note namespace fbzz::scene::managed_detail
