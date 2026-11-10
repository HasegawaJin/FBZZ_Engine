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
#include <limits>
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
    /// @note A shared patchGroup and noiseScale sample the same world-anchored habitat mask.
    std::string patchGroup;
    float minPatch = 0.0f, maxPatch = 1.0f;
    /// @note Distances use world XZ metres. Zero disables spacing, water fade, or the neighbor upper bound.
    float minSpacing = 0.0f, waterFade = 0.0f;
    std::string neighborRule;
    float minNeighborDistance = 0.0f, maxNeighborDistance = 0.0f;
    /// @see https://dev.epicgames.com/documentation/unreal-engine/open-world-tools-property-reference-in-unreal-engine Epic Open World Tools: Landscape Layers and Overlap Priority.
    std::string includeMaterial, excludeMaterial, competitionGroup;
    float minMaterialWeight = 0.25f, excludeMaterialWeight = 0.35f;
    float competitionRadius = 0.0f, competitionPriority = 0.0f;
    /// @note Tapers are inside their allowed ranges. Height uses world metres; slope and aspect use degrees.
    float heightFade = 0.0f, slopeFade = 0.0f;
    /// @note Downhill headings turn from +Z toward +X. Half-angle 180 disables the aspect filter.
    float aspectHeading = 0.0f, aspectHalfAngle = 180.0f, aspectMinSlope = 3.0f, aspectFade = 0.0f;
    /// @note Source tags index emitted roots, including only the placements surviving the cell cap.
    std::string canopySourceGroup, canopyGroup;
    float canopyRadius = 0.0f;
    int minCanopyCount = 0, maxCanopyCount = -1;
    bool scaleCompetitionRadius = false;
    /// @note Signed water offsets and relief are in world metres. Negative water offsets are submerged.
    bool useWaterLevel = false;
    float minWaterOffset = -1000000.0f, maxWaterOffset = 1000000.0f;
    float minNearRelief = -1000000.0f, maxNearRelief = 1000000.0f;
    float minFarRelief = -1000000.0f, maxFarRelief = 1000000.0f;
    float maxCliffDistance = 0.0f, cliffFarScale = 0.45f;
    float minPathDistance = 0.0f, maxPathDistance = 1000000.0f, pathFade = 0.0f;

    bool UsesPathBand() const { return minPathDistance > 0.0f || maxPathDistance < 1000000.0f || pathFade > 0.0f; }
    bool UsesNearRelief() const { return minNearRelief > -1000000.0f || maxNearRelief < 1000000.0f; }
    bool UsesFarRelief() const { return minFarRelief > -1000000.0f || maxFarRelief < 1000000.0f; }
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
    std::string pathMaterial;
};

struct WorldPcgMaterialWeight {
    std::string_view material;
    float weight = 0.0f;
};

struct WorldPcgSurfaceSample {
    math::Vector3 position{}, normal{0,1,0};
    float waterDistance = 1000000.0f, biomeDensity = 1.0f;
    std::string biome;
    bool water = false, excluded = false;
    /// @note Weights are keyed by profile reference and must remain readable until the next sampler call.
    std::span<const WorldPcgMaterialWeight> materialWeights;
    /// @note NaN denotes unavailable habitat data.
    float waterOffset = std::numeric_limits<float>::quiet_NaN();
    float nearRelief = std::numeric_limits<float>::quiet_NaN();
    float farRelief = std::numeric_limits<float>::quiet_NaN();
    float cliffDistance = std::numeric_limits<float>::quiet_NaN();
    float pathDistance = std::numeric_limits<float>::quiet_NaN();
    bool onPath = false;
};

/// @note Called synchronously on the caller's thread; false denotes holes or unavailable ground.
/// @note A worker must receive a callback over immutable captured samples, never a live Scene.
using WorldPcgSampler = std::function<bool(float worldX, float worldZ, WorldPcgSurfaceSample& out)>;
/// @note Optionally fills habitat fields after base terrain, material, and mask filters pass.
using WorldPcgHabitatSampler = std::function<void(const WorldPcgRule& rule, WorldPcgSurfaceSample& sample)>;

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
    uint64_t patchRejected = 0, neighborRejected = 0, spacingRejected = 0;
    uint64_t materialRejected = 0, competitionRejected = 0;
    uint64_t aspectRejected = 0;
    uint64_t canopyRejected = 0;
    uint64_t waterLevelRejected = 0, reliefRejected = 0, cliffRejected = 0, pathRejected = 0;
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
    std::span<const math::Vector3> protectedPositions, WorldPcgResult& out, std::string& diagnostic,
    const WorldPcgHabitatSampler& sampleHabitat = {});
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
