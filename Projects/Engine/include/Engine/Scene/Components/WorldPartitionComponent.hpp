/// @file    WorldPartitionComponent.hpp
/// @brief   Saved world manifest, streaming distances, budgets and source selection.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Engine/Scene/Reflection.hpp>
#include <Engine/Scene/ObjectReference.hpp>
#include <string>

namespace fbzz::scene {
struct WorldPartitionComponent {
    bool enabled = true;
    std::string manifestPath;
    math::Vector3 gridOrigin{};
    float cellSize = 256.0f;
    bool streamInEditor = false;
    bool useMainCamera = true;
    ObjectReference sourceCamera;
    math::Vector3 sourcePosition{};
    float nearDistance = 384.0f;
    float hlodDistance = 1536.0f;
    float hysteresis = 64.0f;
    int maxConcurrentLoads = 2;
    int maxPublishesPerFrame = 1;
    int maxResidentCells = 256;
    int maxCellMegabytes = 16;
    int maxResidentMegabytes = 512;
    const char* GetTypeName() const { return "World Partition"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("manifestPath", manifestPath);
        r.Field("gridOrigin", gridOrigin);
        r.Field("cellSize", cellSize);
        r.Field("streamInEditor", streamInEditor);
        r.Field("useMainCamera", useMainCamera);
        r.TypedObjectReferenceField("sourceCamera", sourceCamera, "CameraComponent");
        r.Field("sourcePosition", sourcePosition);
        r.Field("nearDistance", nearDistance);
        r.Field("hlodDistance", hlodDistance);
        r.Field("hysteresis", hysteresis);
        r.Field("maxConcurrentLoads", maxConcurrentLoads);
        r.Field("maxPublishesPerFrame", maxPublishesPerFrame);
        r.Field("maxResidentCells", maxResidentCells);
        r.Field("maxCellMegabytes", maxCellMegabytes);
        r.Field("maxResidentMegabytes", maxResidentMegabytes);
    }
};
} /// @note namespace fbzz::scene
