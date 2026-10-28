/// @file    SpatialDebugOverlay.cpp
/// @brief   Owner-independent snapshot validation and Scene View spatial draw command construction.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#include <Editor/Util/SpatialDebugOverlay.hpp>
#include <Editor/EditorContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SpatialDebugSnapshot.hpp>
#include <Engine/Scene/Systems/PhysicsSystem.hpp>
#include <Graphics/Renderer/RenderBvhDebug.hpp>
#include <Physics/BroadPhaseDebugSnapshot.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace fbzz::editor {
namespace {
constexpr std::size_t MAX_COMMANDS = 8192;
constexpr math::Vector4 STATIC_COLOR{0.25f, 0.65f, 1.0f, 1.0f};
constexpr math::Vector4 DYNAMIC_COLOR{1.0f, 0.65f, 0.2f, 1.0f};
constexpr math::Vector4 UNOBSERVED_COLOR{0.55f, 0.55f, 0.65f, 1.0f};
constexpr math::Vector4 CANDIDATE_COLOR{1.0f, 0.9f, 0.2f, 1.0f};

bool Finite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool Bounds(const math::Vector3& low, const math::Vector3& high,
    math::Vector3& center, math::Vector3& halfExtents)
{
    if (!Finite(low) || !Finite(high) || low.x > high.x || low.y > high.y || low.z > high.z) return false;
    center = low * 0.5f + high * 0.5f;
    halfExtents = high * 0.5f - low * 0.5f;
    return Finite(center) && Finite(halfExtents);
}

/// @note This bounded sink is shared by every enabled content of one target.
struct CommandSink {
    std::vector<scene::ScriptDebugDrawCommand>& commands;
    const SpatialDebugTargetSettings& settings;
    std::size_t start = commands.size();
    std::size_t invalidBounds = 0;
    bool budgetLimited = false;

    bool Push(scene::ScriptDebugDrawCommand command)
    {
        if (commands.size() >= MAX_COMMANDS || commands.size() - start
            >= static_cast<std::size_t>(std::clamp(settings.maxDrawCommands, 1, 4096))) {
            budgetLimited = true;
            return false;
        }
        command.depthTest = settings.depthTest;
        commands.push_back(command);
        return true;
    }

    void Box(const math::Vector3& low, const math::Vector3& high, const math::Vector4& color)
    {
        scene::ScriptDebugDrawCommand command;
        command.type = scene::ScriptDebugDrawType::Box;
        command.color = color;
        if (!Bounds(low, high, command.a, command.halfExtents)) { ++invalidBounds; return; }
        (void)Push(command);
    }

    void Pair(const physics::BroadPhaseDebugPair& pair)
    {
        scene::ScriptDebugDrawCommand command;
        command.type = scene::ScriptDebugDrawType::Line;
        command.color = CANDIDATE_COLOR;
        math::Vector3 ignored;
        if (!Bounds(pair.boundsA.min, pair.boundsA.max, command.a, ignored)
            || !Bounds(pair.boundsB.min, pair.boundsB.max, command.b, ignored)) { ++invalidBounds; return; }
        (void)Push(command);
    }

    void Finish(SpatialDebugOverlayResult& result)
    {
        result.drawCommands = commands.size() - start;
        result.status += " | drawn " + std::to_string(result.drawCommands);
        if (budgetLimited) result.status += " | DRAW BUDGET TRUNCATED";
        if (invalidBounds) result.status += " | invalid bounds skipped " + std::to_string(invalidBounds);
    }
};

const char* Availability(renderer::RenderBvhDebugAvailability value)
{
    using A = renderer::RenderBvhDebugAvailability;
    switch (value) {
    case A::LINEAR_MODE: return "BVH unavailable: linear production mode. Set FBZZ_MESH_SPATIAL_MODE=bvh/auto/validate before launch";
    case A::NO_CURRENT_TREE: return "No current production BVH";
    case A::NO_SHADOW_VOLUMES: return "No shadow query / light volumes in this view";
    case A::NUMERIC_FALLBACK: return "Numeric fallback: production BVH observation unavailable";
    case A::VALIDATION_FAILED: return "Production BVH validation failed";
    case A::WAITING: return "Waiting for enabled shadow light / completed shadow query";
    case A::READY: return "Ready";
    }
    return "Unknown snapshot availability";
}
} /// @note namespace

SpatialDebugOverlayResult BuildRenderBvhDebugCommands(const renderer::RenderBvhDebugSnapshot& snapshot,
    const SpatialDebugTargetSettings& settings, const SpatialDebugOverlayContext& context,
    std::vector<scene::ScriptDebugDrawCommand>& commands)
{
    SpatialDebugOverlayResult result;
    if (!settings.CapturesHierarchy()) { result.status = "OFF"; return result; }
    if (!context.shadowQueriesEnabled) {
        result.status = "No production shadow query: shadows disabled in Scene View";
        return result;
    }
    if (!context.sceneGeneration || snapshot.sceneGeneration != context.sceneGeneration
        || !context.outputId || snapshot.viewOutputId != context.outputId
        || snapshot.viewOutputGeneration != context.outputGeneration) {
        result.status = "Waiting: scene/view snapshot identity mismatch"; return result;
    }
    if (snapshot.availability != renderer::RenderBvhDebugAvailability::READY) {
        result.status = Availability(snapshot.availability); return result;
    }
    if (!snapshot.snapshotSerial || snapshot.contextFrameStamp > context.currentResourceFrameStamp
        || context.currentResourceFrameStamp - snapshot.contextFrameStamp > 2) {
        result.status = "Waiting: stale shadow-query snapshot"; return result;
    }
    if (settings.selectedOnly && !snapshot.selectedFound) {
        result.status = snapshot.selectedLookupTruncated ? "Selected lookup budget reached"
            : "Selected object has no current Render BVH leaf"; return result;
    }
    result.snapshotAccepted = true;
    result.traversalStatusAvailable = true;
    result.status = "Shadow query / light-volume union | volumes " + std::to_string(snapshot.shadowVolumeCount)
        + " | visited " + std::to_string(snapshot.visitedNodes) + " | candidates " + std::to_string(snapshot.candidates);
    CommandSink sink{commands, settings};
    std::size_t staticNodes = 0, dynamicNodes = 0;
    for (const auto& node : snapshot.nodes) {
        if (sink.budgetLimited) break;
        if (node.depth < static_cast<uint32_t>(std::max(0, settings.minDepth))
            || node.depth > static_cast<uint32_t>(std::max(0, settings.maxDepth))
            || (settings.selectedOnly && !node.selectedPath)) continue;
        const bool isStatic = node.partition == renderer::RenderBvhDebugPartition::STATIC;
        isStatic ? ++staticNodes : ++dynamicNodes;
        math::Vector4 color = isStatic ? STATIC_COLOR : DYNAMIC_COLOR;
        switch (node.state) {
        case renderer::RenderBvhDebugState::VISITED: color = {0.25f, 1.0f, 0.4f, 1.0f}; break;
        case renderer::RenderBvhDebugState::REJECTED: color = {1.0f, 0.3f, 0.3f, 1.0f}; break;
        case renderer::RenderBvhDebugState::CANDIDATE: color = CANDIDATE_COLOR; break;
        case renderer::RenderBvhDebugState::UNOBSERVED: break;
        }
        sink.Box(node.low, node.high, color);
    }
    result.status += " | examined partitions static " + std::to_string(staticNodes) + " dynamic " + std::to_string(dynamicNodes);
    if (snapshot.selectedLookupTruncated) result.status += " | Selected lookup budget reached";
    if (snapshot.truncated) result.status += " | SNAPSHOT TRUNCATED";
    sink.Finish(result);
    return result;
}

SpatialDebugOverlayResult BuildPhysicsSpatialDebugCommands(const scene::PhysicsSpatialDebugSnapshot& snapshot,
    const SpatialDebugTargetSettings& settings, const SpatialDebugOverlayContext& context,
    std::vector<scene::ScriptDebugDrawCommand>& commands)
{
    SpatialDebugOverlayResult result;
    if (!settings.AnyCapture()) { result.status = "OFF"; return result; }
    if (!context.sceneGeneration || snapshot.sceneGeneration != context.sceneGeneration
        || !context.expectedPhysicsRevision || snapshot.revision != context.expectedPhysicsRevision || !snapshot.physics) {
        result.status = "Waiting: scene/request Physics snapshot identity mismatch"; return result;
    }
    const auto& broadphase = *snapshot.physics;
    if (!broadphase.worldEpoch || !broadphase.sequence || !broadphase.request.enabled) {
        result.status = "Waiting for a completed Physics step"; return result;
    }
    if (settings.selectedOnly && !broadphase.selectionValid) {
        result.status = broadphase.workLimitReached ? "Selected collider lookup budget reached"
            : "Selected object has no enabled Physics collider"; return result;
    }
    result.snapshotAccepted = true;
    result.status = "Final substep pre-NarrowPhase/Resolve | step sequence " + std::to_string(broadphase.sequence)
        + " | proxies " + std::to_string(broadphase.totalProxies) + " | candidates " + std::to_string(broadphase.totalPairs);
    CommandSink sink{commands, settings};
    if (settings.CapturesHierarchy()) {
        if (broadphase.structure == physics::BroadPhaseDebugStructure::BVH) {
            for (const auto& node : broadphase.nodes) {
                if (sink.budgetLimited) break;
                if (node.depth < static_cast<uint32_t>(std::max(0, settings.minDepth))
                    || node.depth > static_cast<uint32_t>(std::max(0, settings.maxDepth))) continue;
                sink.Box(node.bounds.min, node.bounds.max, UNOBSERVED_COLOR);
            }
        } else if (broadphase.structure == physics::BroadPhaseDebugStructure::BRUTE_FORCE)
            result.status += " | BVH unavailable: production brute-force broadphase";
        else if (broadphase.structure == physics::BroadPhaseDebugStructure::EMPTY)
            result.status += " | broadphase empty";
        else result.status += " | BVH unavailable";
    }
    if (settings.CapturesProxyBounds()) {
        for (const auto& proxy : broadphase.proxies) {
            if (sink.budgetLimited) break;
            sink.Box(proxy.bounds.min, proxy.bounds.max, proxy.isStatic ? STATIC_COLOR : DYNAMIC_COLOR);
        }
    }
    if (settings.CapturesCandidatePairs()) {
        if (broadphase.candidatePairsAvailable) {
            for (const auto& pair : broadphase.pairs) {
                if (sink.budgetLimited) break;
                sink.Pair(pair);
            }
        } else result.status += " | candidate pairs unavailable: no observed broadphase pass";
    }
    if (broadphase.nodesTruncated || broadphase.proxiesTruncated || broadphase.pairsTruncated || broadphase.workLimitReached)
        result.status += " | SNAPSHOT TRUNCATED";
    sink.Finish(result);
    return result;
}

void ResetSpatialDebugCapture()
{
    renderer::RenderBvhDebug::Invalidate();
    scene::PhysicsSystem::InvalidateSpatialDebugSnapshot();
}

void UpdateSpatialDebugRequests(EditorContext& ctx, scene::Scene* activeScene, bool runtimeActive,
    uint32_t outputId, uint32_t outputGeneration)
{
    auto& state = ctx.spatialDebugVisualization;
    const uint64_t generation = activeScene ? activeScene->GetRenderSceneGeneration() : 0;
    if (state.captureSceneGeneration != generation || state.captureRuntimeActive != runtimeActive) {
        ResetSpatialDebugCapture();
        state.captureSceneGeneration = generation;
        state.captureRuntimeActive = runtimeActive;
        state.renderTraversalStatusAvailable = false;
        state.physicsTraversalStatusAvailable = false;
        state.renderDrawCommands = 0;
        state.physicsDrawCommands = 0;
    }
    state.render.Clamp();
    state.physics.Clamp();
    scene::EntityID selected;
    if (activeScene && !ctx.selectedEntities.empty() && activeScene->GetGameObject(ctx.selectedEntities.front()))
        selected = ctx.selectedEntities.front();
    renderer::RenderBvhDebugRequest render;
    render.enabled = generation && outputId && state.render.CapturesHierarchy();
    render.sceneGeneration = generation;
    render.viewOutputId = outputId;
    render.viewOutputGeneration = outputGeneration;
    render.minDepth = static_cast<uint32_t>(state.render.minDepth);
    render.maxDepth = static_cast<uint32_t>(state.render.maxDepth);
    render.maxNodes = static_cast<uint32_t>(state.render.maxDrawCommands);
    render.selectedOnly = state.render.selectedOnly;
    render.sourceIndex = selected.index;
    render.sourceGeneration = selected.generation;
    renderer::RenderBvhDebug::SetRequest(render);
    scene::PhysicsSpatialDebugRequest physics;
    physics.sceneGeneration = generation;
    physics.selectedEntity = selected;
    physics.physics.enabled = generation && runtimeActive && state.physics.AnyCapture();
    physics.physics.hierarchy = state.physics.hierarchy;
    physics.physics.bounds = state.physics.proxyBounds;
    physics.physics.candidatePairs = state.physics.candidatePairs;
    physics.physics.selectedOnly = state.physics.selectedOnly;
    physics.physics.minDepth = static_cast<uint32_t>(state.physics.minDepth);
    physics.physics.maxDepth = static_cast<uint32_t>(state.physics.maxDepth);
    const auto budget = static_cast<std::size_t>(state.physics.maxDrawCommands);
    physics.physics.maxNodes = budget;
    physics.physics.maxProxies = budget;
    physics.physics.maxPairs = budget;
    state.physicsRequestRevision = scene::PhysicsSystem::SetSpatialDebugRequest(physics);
    if (!render.enabled) {
        state.renderStatus = state.render.enabled ? "No display contents selected or Scene View unavailable" : "OFF";
        state.renderDrawCommands = 0;
        state.renderTraversalStatusAvailable = false;
    }
    if (!physics.physics.enabled) {
        state.physicsStatus = !state.physics.enabled ? "OFF" : !runtimeActive ? "Play to update Physics broadphase" : "No display contents selected";
        state.physicsDrawCommands = 0;
        state.physicsTraversalStatusAvailable = false;
    }
}

void BuildSpatialDebugOverlay(EditorContext& ctx, const scene::Scene& activeScene,
    uint32_t outputId, uint32_t outputGeneration, uint64_t currentResourceFrameStamp,
    std::vector<scene::ScriptDebugDrawCommand>& commands, bool shadowQueriesEnabled)
{
    commands.clear();
    auto& state = ctx.spatialDebugVisualization;
    state.renderTraversalStatusAvailable = false;
    state.physicsTraversalStatusAvailable = false;
    state.renderDrawCommands = 0;
    state.physicsDrawCommands = 0;
    SpatialDebugOverlayContext context;
    context.sceneGeneration = activeScene.GetRenderSceneGeneration();
    context.currentResourceFrameStamp = currentResourceFrameStamp;
    context.expectedPhysicsRevision = state.physicsRequestRevision;
    context.outputId = outputId;
    context.outputGeneration = outputGeneration;
    context.shadowQueriesEnabled = shadowQueriesEnabled;
    if (state.captureSceneGeneration != context.sceneGeneration) {
        state.renderStatus = "Waiting: scene changed";
        state.physicsStatus = "Waiting: scene changed";
        return;
    }
    if (state.render.CapturesHierarchy()) {
        if (!shadowQueriesEnabled) {
            state.renderStatus = "No production shadow query: shadows disabled in Scene View";
        } else {
            const auto snapshot = renderer::RenderBvhDebug::GetSnapshot();
            if (snapshot) {
                const auto result = BuildRenderBvhDebugCommands(*snapshot, state.render, context, commands);
                state.renderStatus = result.status;
                state.renderTraversalStatusAvailable = result.traversalStatusAvailable;
                state.renderDrawCommands = result.drawCommands;
            } else state.renderStatus = "Waiting for enabled shadow light / completed shadow query";
        }
    }
    if (state.physics.AnyCapture() && state.captureRuntimeActive) {
        const auto snapshot = scene::PhysicsSystem::GetSpatialDebugSnapshot(context.sceneGeneration);
        if (snapshot) {
            const auto result = BuildPhysicsSpatialDebugCommands(*snapshot, state.physics, context, commands);
            state.physicsStatus = result.status;
            state.physicsDrawCommands = result.drawCommands;
        }
        else state.physicsStatus = "Waiting for a completed Physics step";
    }
}

} /// @note namespace fbzz::editor
