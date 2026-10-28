/// @file    BroadPhaseDebugSnapshot.hpp
/// @brief   Bounded value diagnostics of the actual Physics broadphase.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#pragma once
#include <Physics/BodyHandle.hpp>
#include <Physics/Collider.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fbzz::physics
{
    inline constexpr std::size_t BROAD_PHASE_DEBUG_MAX_ITEMS = 4096;
    inline constexpr std::size_t BROAD_PHASE_DEBUG_MAX_WORK = 65536;
    inline constexpr uint32_t BROAD_PHASE_DEBUG_MAX_DEPTH = 64;

    enum class BroadPhaseDebugStructure { UNAVAILABLE, BVH, BRUTE_FORCE, EMPTY };

    struct BroadPhaseDebugRequest
    {
        bool enabled = false;
        bool hierarchy = true;
        bool bounds = false;
        bool candidatePairs = false;
        bool selectedOnly = false;
        uint32_t minDepth = 0;
        uint32_t maxDepth = 8;
        std::size_t maxNodes = 512;
        std::size_t maxProxies = 512;
        std::size_t maxPairs = 256;
        ColliderHandle selected;
        uint64_t selectedWorldEpoch = 0;
    };

    /// @note Node indices identify the source BVH within this snapshot only; filtered relatives may be absent.
    struct BroadPhaseDebugNode
    {
        AABB bounds;
        int sourceIndex = -1;
        int parent = -1;
        int left = -1;
        int right = -1;
        uint32_t depth = 0;
        std::size_t proxyCount = 0;
        bool leaf = false;
    };

    struct BroadPhaseDebugProxy
    {
        ColliderHandle handle;
        AABB bounds;
        std::size_t sourceIndex = 0;
        bool isStatic = true;
        bool isSleeping = false;
        bool isTrigger = false;
        int layer = 0;
    };

    /// @note Accepted broadphase candidates before NarrowPhase; these are not confirmed contacts.
    struct BroadPhaseDebugPair
    {
        ColliderHandle handleA;
        ColliderHandle handleB;
        AABB boundsA;
        AABB boundsB;
        std::size_t sourceIndexA = 0;
        std::size_t sourceIndexB = 0;
    };

    /// @note Values describe the final substep before NarrowPhase/Resolve, in world space.
    /// @note No mutable tree, Collider, Body or scene pointer crosses this boundary.
    struct BroadPhaseDebugSnapshot
    {
        uint64_t worldEpoch = 0;
        uint64_t sequence = 0;
        BroadPhaseDebugStructure structure = BroadPhaseDebugStructure::UNAVAILABLE;
        BroadPhaseDebugRequest request;
        std::vector<BroadPhaseDebugNode> nodes;
        std::vector<BroadPhaseDebugProxy> proxies;
        std::vector<BroadPhaseDebugPair> pairs;
        std::size_t totalNodes = 0;
        std::size_t totalProxies = 0;
        std::size_t totalPairs = 0;
        std::size_t examinedNodes = 0;
        std::size_t examinedProxies = 0;
        std::size_t examinedPairs = 0;
        bool broadPhaseRan = false;
        bool candidatePairsAvailable = false;
        bool traversalStatusAvailable = false;
        bool selectionValid = false;
        bool nodesTruncated = false;
        bool proxiesTruncated = false;
        bool pairsTruncated = false;
        bool workLimitReached = false;
    };
} /// @note namespace fbzz::physics
