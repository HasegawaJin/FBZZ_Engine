/// @file    WaterAuthoringValues.cpp
/// @brief   Waterの作者設定だけを移し、別ownerの実行状態を混入させない。
/// @author  Hasegawa Jin
/// @date    2026-10-09
#include <Editor/Util/WaterAuthoringValues.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>

namespace fbzz::editor {
void CopyWaterAuthoringValues(scene::WaterComponent& target, const scene::WaterComponent& values)
{
    target.enabled = values.enabled;
    target.resolutionX = values.resolutionX;
    target.resolutionZ = values.resolutionZ;
    target.extentX = values.extentX;
    target.extentZ = values.extentZ;
    target.surfaceShape = values.surfaceShape;
    target.splinePoints = values.splinePoints;
    target.pointSourceMode = values.pointSourceMode;
    target.pointRuntimeMode = values.pointRuntimeMode;
    target.pointBindings = values.pointBindings;
    target.lakePlaneHeight = values.lakePlaneHeight;
    target.bakedSplinePoints = values.bakedSplinePoints;
    target.bakedInputSignature = values.bakedInputSignature;
    target.bakedSourceMode = values.bakedSourceMode;
    target.bakedSurfaceShape = values.bakedSurfaceShape;
    target.bakedLakePlaneHeight = values.bakedLakePlaneHeight;
    target.bakedAdapterVersion = values.bakedAdapterVersion;
    target.bakeValid = values.bakeValid;
    target.splineSamplesPerSegment = values.splineSamplesPerSegment;
    target.splineMeshCellSize = values.splineMeshCellSize;
    target.chunkCount = values.chunkCount;
    target.cameraFocusedGrid = values.cameraFocusedGrid;
    target.nearCellSize = values.nearCellSize;
    target.adaptivePatchLod = values.adaptivePatchLod;
    target.lodPatchResolution = values.lodPatchResolution;
    target.lodMaxDepth = values.lodMaxDepth;
    target.lodMaxPatches = values.lodMaxPatches;
    target.lodTargetPixels = values.lodTargetPixels;
    target.lodNearDistance = values.lodNearDistance;
    target.lodNearCellSize = values.lodNearCellSize;
    target.lodMorphEnabled = values.lodMorphEnabled;
    target.lodMorphTransitionWidth = values.lodMorphTransitionWidth;
    target.farOceanEnabled = values.farOceanEnabled;
    target.farOceanCoverageScale = values.farOceanCoverageScale;
    target.farOceanRippleFadeDistance = values.farOceanRippleFadeDistance;
    target.materialPath = values.materialPath;
    target.wavesAssetRef = values.wavesAssetRef;
    target.waveAuthoring = values.waveAuthoring;
    target.enableGerstnerWaves = values.enableGerstnerWaves;
    target.waveAmplitudeScale = values.waveAmplitudeScale;
    target.buoyancyEnabled = values.buoyancyEnabled;
    target.buoyancy = values.buoyancy;
    target.waterDrag = values.waterDrag;
    target.buoyancyDepth = values.buoyancyDepth;
    target.waveAttenuationDepth = values.waveAttenuationDepth;
    target.splashEnabled = values.splashEnabled;
    target.interactionSettings = values.interactionSettings;
}

void ApplyWaterAuthoringValues(scene::WaterComponent& target, const scene::WaterComponent& values)
{
    CopyWaterAuthoringValues(target, values);
    const auto volumeHandle = target.volumeHandle;
    target.ResetSurfaceRuntimeState();
    target.volumeHandle = volumeHandle;
    target.meshDirty = target.foamDirty = target.texDirty = true;
}

scene::WaterComponent CaptureWaterAuthoringValues(const scene::WaterComponent& source)
{
    scene::WaterComponent values;
    CopyWaterAuthoringValues(values, source);
    return values;
}
} /// @note namespace fbzz::editor
