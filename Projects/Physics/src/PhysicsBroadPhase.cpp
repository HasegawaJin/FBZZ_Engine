/// @file    PhysicsBroadPhase.cpp
/// @brief   広域衝突判定の実行と診断スナップショット。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "PhysicsBroadPhaseState.hpp"
#include <Physics/BroadPhaseDebugSnapshot.hpp>
#include <algorithm>

namespace fbzz::physics {
    PhysicsSolver::PhysicsSolver() = default;
    PhysicsSolver::~PhysicsSolver() = default;
    PhysicsSolver::PhysicsSolver(PhysicsSolver&& other) noexcept = default;
    PhysicsSolver& PhysicsSolver::operator=(PhysicsSolver&& other) noexcept = default;
    PhysicsSolver::PhysicsSolver(const PhysicsSolver& other)
        : m_broadPhase(other.m_broadPhase ? std::make_unique<BroadPhaseState>(*other.m_broadPhase) : nullptr) {}
    PhysicsSolver& PhysicsSolver::operator=(const PhysicsSolver& other)
    {
        if (this != &other) m_broadPhase = other.m_broadPhase
            ? std::make_unique<BroadPhaseState>(*other.m_broadPhase) : nullptr;
        return *this;
    }

    void PhysicsSolver::BroadPhase(const std::vector<ColliderInstance>& colliders,
                                    std::vector<CollisionPair>& outPairs,
                                    const std::function<bool(int, int)>& layerFilter)
    {
        if (!m_broadPhase) m_broadPhase = std::make_unique<BroadPhaseState>();
        m_broadPhase->Update(colliders);
        m_broadPhase->CollectPairs(colliders, outPairs, layerFilter);
    }

    void PhysicsSolver::CaptureBroadPhaseDebugSnapshot(const BroadPhaseDebugRequest& request,
        const std::vector<ColliderInstance>& colliders, const std::vector<CollisionPair>& pairs,
        std::size_t selectedIndex, BroadPhaseDebugSnapshot& snapshot) const
    {
        if (!request.enabled || !m_broadPhase) return;
        const auto& state = *m_broadPhase;
        snapshot.structure = state.proxies.size() < 2 ? BroadPhaseDebugStructure::EMPTY
            : (state.allProxyBoundsFinite ? BroadPhaseDebugStructure::BVH : BroadPhaseDebugStructure::BRUTE_FORCE);
        snapshot.totalNodes = state.nodes.size();
        snapshot.totalProxies = state.proxies.size();
        snapshot.totalPairs = pairs.size();
        snapshot.broadPhaseRan = true;
        snapshot.candidatePairsAvailable = request.candidatePairs;
        const auto findProxy = [&](std::size_t sourceIndex) -> const BroadPhaseProxy* {
            const auto found = std::lower_bound(state.proxies.begin(), state.proxies.end(), sourceIndex,
                [](const BroadPhaseProxy& proxy, std::size_t index) { return proxy.colliderIndex < index; });
            return found != state.proxies.end() && found->colliderIndex == sourceIndex ? &*found : nullptr;
        };
        const BroadPhaseProxy* selected = request.selectedOnly ? findProxy(selectedIndex) : nullptr;
        if (request.selectedOnly && (!snapshot.selectionValid || !selected)) return;

        const auto addNode = [&](int index, uint32_t depth) {
            const auto& node = state.nodes[static_cast<std::size_t>(index)];
            if (depth < request.minDepth || depth > request.maxDepth) return;
            if (snapshot.nodes.size() >= request.maxNodes) { snapshot.nodesTruncated = true; return; }
            snapshot.nodes.push_back({node.bounds, index, node.parent, node.left, node.right,
                depth, static_cast<std::size_t>(node.count), node.IsLeaf()});
        };
        if (request.hierarchy && !state.nodes.empty()) {
            if (selected) {
                std::array<int, BROAD_PHASE_DEBUG_MAX_DEPTH + 1> ancestors{};
                std::size_t count = 0;
                int current = selected->leafIndex;
                while (current >= 0 && count < ancestors.size()) {
                    ancestors[count++] = current;
                    current = state.nodes[static_cast<std::size_t>(current)].parent;
                }
                if (current >= 0) { snapshot.nodesTruncated = true; snapshot.workLimitReached = true; }
                for (std::size_t index = count; index > 0; --index) {
                    ++snapshot.examinedNodes;
                    addNode(ancestors[index - 1], static_cast<uint32_t>(count - index));
                }
            } else if (request.maxNodes != 0) {
                struct Visit { int index; uint32_t depth; };
                std::array<Visit, BROAD_PHASE_DEBUG_MAX_DEPTH + 1> stack{};
                std::size_t top = 0;
                stack[top++] = {0, 0};
                while (top != 0 && snapshot.examinedNodes < BROAD_PHASE_DEBUG_MAX_WORK) {
                    const Visit visit = stack[--top];
                    ++snapshot.examinedNodes;
                    const auto& node = state.nodes[static_cast<std::size_t>(visit.index)];
                    addNode(visit.index, visit.depth);
                    if (snapshot.nodes.size() >= request.maxNodes) {
                        snapshot.nodesTruncated |= top != 0 || (!node.IsLeaf() && visit.depth < request.maxDepth);
                        break;
                    }
                    if (node.IsLeaf() || visit.depth >= request.maxDepth) continue;
                    if (top + 2 > stack.size()) {
                        snapshot.nodesTruncated = true;
                        snapshot.workLimitReached = true;
                        continue;
                    }
                    stack[top++] = {node.right, visit.depth + 1};
                    stack[top++] = {node.left, visit.depth + 1};
                }
                if (top != 0 && snapshot.examinedNodes >= BROAD_PHASE_DEBUG_MAX_WORK) {
                    snapshot.nodesTruncated = true;
                    snapshot.workLimitReached = true;
                }
            } else snapshot.nodesTruncated = true;
        }

        if (request.bounds) {
            const std::size_t count = selected ? std::size_t{1} : state.proxies.size();
            for (std::size_t index = 0; index < count; ++index) {
                if (snapshot.proxies.size() >= request.maxProxies) { snapshot.proxiesTruncated = true; break; }
                if (snapshot.examinedProxies >= BROAD_PHASE_DEBUG_MAX_WORK) {
                    snapshot.proxiesTruncated = true;
                    snapshot.workLimitReached = true;
                    break;
                }
                ++snapshot.examinedProxies;
                const auto& proxy = selected ? *selected : state.proxies[index];
                if (!FiniteBroadPhaseBounds(proxy.bounds)) { snapshot.proxiesTruncated = true; continue; }
                const auto& collider = colliders[proxy.colliderIndex];
                snapshot.proxies.push_back({{}, proxy.bounds, proxy.colliderIndex,
                    !collider.body || collider.body->IsStatic(), collider.body && collider.body->IsSleeping(),
                    collider.isTrigger, collider.layer});
            }
        }
        if (request.candidatePairs) {
            for (const auto& pair : pairs) {
                if (snapshot.pairs.size() >= request.maxPairs) { snapshot.pairsTruncated = true; break; }
                if (snapshot.examinedPairs >= BROAD_PHASE_DEBUG_MAX_WORK) {
                    snapshot.pairsTruncated = true;
                    snapshot.workLimitReached = true;
                    break;
                }
                ++snapshot.examinedPairs;
                const auto first = static_cast<std::size_t>(pair.colliderA - colliders.data());
                const auto second = static_cast<std::size_t>(pair.colliderB - colliders.data());
                if (selected && first != selectedIndex && second != selectedIndex) continue;
                const auto* a = findProxy(first);
                const auto* b = findProxy(second);
                if (!a || !b || !FiniteBroadPhaseBounds(a->bounds) || !FiniteBroadPhaseBounds(b->bounds)) {
                    snapshot.pairsTruncated = true;
                    continue;
                }
                snapshot.pairs.push_back({{}, {}, a->bounds, b->bounds, first, second});
            }
        }
    }
}
