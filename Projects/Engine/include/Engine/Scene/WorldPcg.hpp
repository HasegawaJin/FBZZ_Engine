/// @file    WorldPcg.hpp
/// @brief   Deterministic authored biome placement profiles and cell-split generation.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Asset/InstanceSetAsset.hpp>
#include <Engine/Scene/WorldCell.hpp>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::scene {

struct WorldPcgRule {
    std::string key, name, biome, prototypeRef;
    std::vector<asset::InstanceMeshPart> parts;
    std::vector<float> lodHeights{0.2f, 0.08f, 0.02f, 0.0f};
    /// @note Spacing and noise scale use world metres; slope and yaw use degrees.
    float spacing = 8.0f, density = 1.0f;
    /// @note Strength blends uniform density toward smooth, world-anchored value-noise density.
    float noiseScale = 128.0f, clusterStrength = 0.0f;
    float minHeight = -100000.0f, maxHeight = 100000.0f;
    float minSlope = 0.0f, maxSlope = 45.0f;
    float minWaterDistance = 0.0f, maxWaterDistance = 1000000.0f;
    float minScale = 0.8f, maxScale = 1.2f;
    float minYaw = 0.0f, maxYaw = 360.0f;
    bool alignToSurface = false;
    bool includeHlod = true;
    /// @note Embedding follows the negative surface normal; protected distance is measured in world XZ.
    float embedDepth = 0.0f, protectedDistance = 0.0f;
};

struct WorldPcgProfile {
    std::string key = "WorldPcg", name;
    WorldCellGrid grid;
    /// @note Bounds own embedded root positions in half-open world XZ intervals; their Y components are ignored.
    WorldCellBounds bounds{{0,0,0},{256,0,256}};
    uint64_t seed = 1;
    uint32_t maxInstances = 100000;
    uint64_t maxCandidates = 1000000;
    std::vector<WorldPcgRule> rules;
    /// @note Exclusion boxes are closed world XYZ intervals, including their boundary.
    std::vector<WorldCellBounds> exclusions;
    /// @note Aggregate cap across rules in each 128 m XZ cell at grid.origin; zero disables the cap.
    uint32_t maxPlacementsPerCell = 0;
};

struct WorldPcgSurfaceSample {
    math::Vector3 position{}, normal{0,1,0};
    float waterDistance = 1000000.0f, biomeDensity = 1.0f;
    std::string biome;
    bool water = false, excluded = false;
};

/// @note Called synchronously on the caller's thread; false denotes holes or unavailable ground.
/// @note A worker must receive a callback over immutable captured samples, never a live Scene.
using WorldPcgSampler = std::function<bool(float worldX, float worldZ, WorldPcgSurfaceSample& out)>;

struct WorldPcgCellGroup {
    WorldCellId cell;
    std::string ruleKey, prototypeRef;
    bool includeHlod = true;
    std::vector<asset::InstanceMeshPart> parts;
    std::vector<float> lodHeights;
    /// @note Positions are local to WorldCellOrigin(profile.grid,cell); IDs are positive 31-bit keys.
    std::vector<renderer::InstancePlacement> placements;
    /// @note Full candidate keys disambiguate hash collisions and identify authored overrides.
    std::vector<uint64_t> candidateKeys;
};

struct WorldPcgStats {
    uint64_t candidates = 0, accepted = 0, noSurface = 0, biomeRejected = 0;
    uint64_t densityRejected = 0, heightRejected = 0, slopeRejected = 0;
    uint64_t waterRejected = 0, exclusionRejected = 0, protectedRejected = 0;
    uint64_t cellBudgetRejected = 0;
};

struct WorldPcgResult {
    std::vector<WorldPcgCellGroup> groups;
    WorldPcgStats stats;
    std::string fingerprint;
};

[[nodiscard]] FBZZ_ENGINE_API bool ValidateWorldPcgProfile(const WorldPcgProfile& profile, std::string& diagnostic);
/// @return Failure leaves out unchanged, including candidate/instance budget and ID collision failures.
/// @note A world-anchored jitter lattice keeps existing candidates fixed when bounds or cell size change.
/// @note The optional placement cap keeps stable candidates after filtering and does not change streaming cells.
/// @note Results contain only generated ownership; protected/manual placements are read-only input.
/// @see https://dev.epicgames.com/documentation/en-us/unreal-engine/using-pcg-generation-modes-in-unreal-engine Epic PCG partitioned generation concepts.
[[nodiscard]] FBZZ_ENGINE_API bool GenerateWorldPcg(const WorldPcgProfile& profile, const WorldPcgSampler& sample,
    std::span<const math::Vector3> protectedPositions, WorldPcgResult& out, std::string& diagnostic);
/// @return Validated UTF-8 TOML; failure leaves outText unchanged and performs no file I/O.
[[nodiscard]] FBZZ_ENGINE_API bool SerializeWorldPcgProfile(const WorldPcgProfile& profile, std::string& outText, std::string& diagnostic);
/// @return Invalid or unknown-version text leaves out unchanged and performs no file I/O.
[[nodiscard]] FBZZ_ENGINE_API bool DeserializeWorldPcgProfile(std::string_view text, WorldPcgProfile& out, std::string& diagnostic);
/// @note Native UTF-8 TOML .pcg profiles contain rules and presentation definitions, never generated placements.
/// @return Invalid, unknown-version or unreadable input leaves out unchanged.
[[nodiscard]] FBZZ_ENGINE_API bool LoadWorldPcgProfile(std::string_view path, WorldPcgProfile& out, std::string& diagnostic);
/// @return Validated atomic write; failed validation never replaces an existing file.
[[nodiscard]] FBZZ_ENGINE_API bool SaveWorldPcgProfile(std::string_view path, const WorldPcgProfile& profile, std::string& diagnostic);

} /// @note namespace fbzz::scene
