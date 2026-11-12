/// @file    WorldPcgAuthoringComponent.hpp
/// @brief   Saved PCG volumes and generated-output ownership for checked regeneration.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Engine/Scene/Reflection.hpp>
#include <Math/Vector3.hpp>
#include <string>

namespace fbzz::scene {

struct WorldHlodSourceComponent {
    bool includeHlod = true;
    bool farOnlyMesh = false;
    bool omitTerrainCollisionInHlod = false;
    const char* GetTypeName() const { return "WorldHlodSource"; }
    void Reflect(IReflector& r)
    {
        r.Field("includeHlod", includeHlod);
        r.Field("farOnlyMesh", farOnlyMesh);
        r.Tooltip("Use a standalone MeshRenderer as an authored cell HLOD proxy. The near payload hides this mesh.");
        r.Field("omitTerrainCollisionInHlod", omitTerrainCollisionInHlod);
    }
};

struct WorldBiomeVolumeComponent {
    bool enabled = true;
    std::string biome = "forest";
    math::Vector3 boundsMin{}, boundsMax{256,100000,256};
    float density = 1.0f;
    int priority = 0;
    const char* GetTypeName() const { return "WorldBiomeVolume"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled); r.Field("biome", biome);
        r.Field("boundsMin", boundsMin); r.Field("boundsMax", boundsMax);
        r.Field("density", density); r.Field("priority", priority);
    }
};

struct WorldPcgVolumeComponent {
    bool enabled = true;
    std::string profilePath;
    bool overrideBounds = false;
    /// @note Bounds are world-space half-open XZ intervals; Y is ignored.
    math::Vector3 boundsMin{}, boundsMax{256,0,256};
    bool overrideSeed = false;
    int seed = 1;
    bool protectManual = true;
    const char* GetTypeName() const { return "WorldPcgVolume"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("profilePath", profilePath);
        r.Field("overrideBounds", overrideBounds);
        r.Field("boundsMin", boundsMin); r.Field("boundsMax", boundsMax);
        r.Field("overrideSeed", overrideSeed); r.Field("seed", seed);
        r.Field("protectManual", protectManual);
    }
};

struct WorldPcgGeneratedComponent {
    std::string volumeId, ruleKey, signature;
    int cellX = 0, cellZ = 0;
    /// @note Protected generated outputs require explicit detach before their owner can be regenerated.
    bool manualOverride = false;
    bool includeHlod = true;
    const char* GetTypeName() const { return "WorldPcgGenerated"; }
    void Reflect(IReflector& r)
    {
        r.Field("volumeId", volumeId); r.Field("ruleKey", ruleKey);
        r.Field("cellX", cellX); r.Field("cellZ", cellZ);
        r.Field("signature", signature); r.Field("manualOverride", manualOverride);
        r.Field("includeHlod", includeHlod);
    }
};

} /// @note namespace fbzz::scene
