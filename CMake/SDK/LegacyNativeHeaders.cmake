# @file    LegacyNativeHeaders.cmake
# @brief   SDK 用の旧ネイティブ include パスを生成する。
# @author  Hasegawa Jin
# @date    2026-11-24

function(fbzz_install_legacy_headers OLD_DIRECTORY NEW_DIRECTORY)
    set(ALIAS_ROOT "${CMAKE_CURRENT_BINARY_DIR}/sdk-native-include")
    string(REGEX REPLACE "/.*" "" LIBRARY "${NEW_DIRECTORY}")
    set(FORWARDERS)
    foreach(HEADER IN LISTS ARGN)
        if(NOT EXISTS "${CMAKE_SOURCE_DIR}/Projects/${LIBRARY}/include/${NEW_DIRECTORY}/${HEADER}")
            message(FATAL_ERROR "Missing SDK header ${NEW_DIRECTORY}/${HEADER}")
        endif()
        set(FORWARDER "${ALIAS_ROOT}/${OLD_DIRECTORY}/${HEADER}")
        file(CONFIGURE OUTPUT "${FORWARDER}" CONTENT "/// @file    ${HEADER}
/// @brief   SDK の旧 include パスとの互換ヘッダー。
/// @author  Hasegawa Jin
/// @date    2026-11-24
#pragma once
#include <${NEW_DIRECTORY}/${HEADER}>
" @ONLY)
        list(APPEND FORWARDERS "${FORWARDER}")
    endforeach()
    install(FILES ${FORWARDERS} DESTINATION "include/${OLD_DIRECTORY}")
endfunction()

# @note 旧 SDK で公開したファイル名だけを転送する。新規のヘッダーは機能別のパスで配布する。
fbzz_install_legacy_headers(Math Math/LinearAlgebra
    Matrix3.hpp Matrix4.hpp Quaternion.hpp Vector2.hpp Vector3.hpp Vector4.hpp)

fbzz_install_legacy_headers(Math Math/Geometry
    Frustum.hpp Plane.hpp Ray.hpp Segment.hpp)

fbzz_install_legacy_headers(Math Math/Scalar
    MathContract.hpp MathUtils.hpp)

fbzz_install_legacy_headers(Math Math/Noise
    CurlNoise.hpp)

fbzz_install_legacy_headers(Math Math/Simd
    Simd.hpp)

fbzz_install_legacy_headers(Core Core/Logger
    ILogSink.hpp Logger.hpp)

fbzz_install_legacy_headers(Core Core/Util
    ExternalCallbackBoundary.hpp HResult.hpp)

fbzz_install_legacy_headers(Core/Memory Core/Memory/Allocators
    Allocator.hpp FrameAllocator.hpp LinearAllocator.hpp MakeUnique.hpp PoolAllocator.hpp StackAllocator.hpp)

fbzz_install_legacy_headers(Core/Memory Core/Memory/Diagnostics
    AllocationInfo.hpp MemoryDebug.hpp MemoryTracker.hpp)

fbzz_install_legacy_headers(Physics Physics/Collision/BroadPhase
    BroadPhaseDebugSnapshot.hpp BVHBounds.hpp BVHNode.hpp CollisionPair.hpp)

fbzz_install_legacy_headers(Physics Physics/Collision/NarrowPhase
    CCDSolver.hpp EPA.hpp GJK.hpp PhysicsSolver.hpp)

fbzz_install_legacy_headers(Physics Physics/Collision/Shapes
    AABBCollider.hpp CapsuleCollider.hpp Collider.hpp ConvexHullCollider.hpp CylinderCollider.hpp HeightFieldCollider.hpp OBBCollider.hpp SphereCollider.hpp TriangleMeshCollider.hpp)

fbzz_install_legacy_headers(Physics Physics/Collision/Filtering
    Layer.hpp)

fbzz_install_legacy_headers(Physics Physics/Constraints
    ChainConstraint.hpp Constraint.hpp DistanceConstraint.hpp FixedConstraint.hpp HingeConstraint.hpp RopeConstraint.hpp SliderConstraint.hpp SpringConstraint.hpp)

fbzz_install_legacy_headers(Physics Physics/Debug
    ColliderDebugGeometry.hpp ConstraintDebugGeometry.hpp)

fbzz_install_legacy_headers(Physics Physics/Dynamics
    PhysicsMaterial.hpp RigidBody.hpp RigidBodySerializer.hpp)

fbzz_install_legacy_headers(Physics Physics/Solver
    ContactCache.hpp ContactPoint.hpp)

fbzz_install_legacy_headers(Physics Physics/Solver/XPBD
    XPBDBodyDrive.hpp XPBDConstraint.hpp XPBDContact.hpp XPBDGrip.hpp XPBDJoint.hpp XPBDPlaneContact.hpp XPBDPoseAnchor.hpp XPBDSolver.hpp)

fbzz_install_legacy_headers(Physics Physics/Volumes
    ColliderVolume.hpp FlowDrag.hpp FlowVolume.hpp FluidVolume.hpp Volume.hpp)

fbzz_install_legacy_headers(Physics Physics/World
    BodyHandle.hpp RegistrationLifetime.hpp World.hpp)

fbzz_install_legacy_headers(Fluid Fluid/Fields
    GerstnerWaves.hpp VectorFieldAsset.hpp)

fbzz_install_legacy_headers(Fluid Fluid/Operators
    FluidOperatorEval.hpp FluidSourceMask.hpp)

fbzz_install_legacy_headers(Fluid Fluid/Recipe
    FluidRecipe.hpp FluidBakeSettings.hpp)

fbzz_install_legacy_headers(Fluid Fluid/Solver
    FluidSolver.hpp)

fbzz_install_legacy_headers(Fluid Fluid/Stepping
    FluidGpuStep.hpp FluidStepping.hpp)

fbzz_install_legacy_headers(Graphics/Renderer Graphics/Renderer/Commands
    ComputeCall.hpp DrawCall.hpp RenderBindingGuard.hpp RenderThread.hpp)

fbzz_install_legacy_headers(Graphics/Renderer Graphics/Renderer/Diagnostics
    DebugDraw.hpp DebugDrawBatch.hpp Gizmo.hpp GpuProfiler.hpp GpuProfilerLedger.hpp PipelineDiagnostics.hpp RenderBvhDebug.hpp RenderCpuTrace.hpp RenderDebugOverlay.hpp)

fbzz_install_legacy_headers(Graphics/Renderer Graphics/Renderer/Geometry
    FiberGeometry.hpp FiberMotionHistory.hpp FiberPlacement.hpp GeometryRoute.hpp Mesh.hpp MeshletGeometry.hpp PrimitiveMesh.hpp RenderGeometryInput.hpp SkinningPolicy.hpp SkinningTrace.hpp SurfaceFarMesh.hpp SurfacePatchLod.hpp SurfacePatchMorph.hpp)

fbzz_install_legacy_headers(Graphics/Renderer Graphics/Renderer/Materials
    Material.hpp MaterialTransferScope.hpp RenderMaterial.hpp)

fbzz_install_legacy_headers(Graphics/Renderer Graphics/Renderer/Platform
    BackendEntry.hpp GraphicsCapabilities.hpp RendererBackend.hpp RuntimePackageValidation.hpp)

fbzz_install_legacy_headers(Graphics/Renderer Graphics/Renderer/Resources
    AssetPathService.hpp BindlessIndices.hpp Format.hpp OwnedResource.hpp ResourceHandle.hpp ResourceManager.hpp ResourcePool.hpp)

fbzz_install_legacy_headers(Graphics/Renderer Graphics/Renderer/Shaders
    BuiltinShader.hpp IShader.hpp ShaderCapabilities.hpp ShaderCompileDiagnostics.hpp ShaderDependencyTracker.hpp ShaderDescriptor.hpp ShaderPathResolver.hpp)

fbzz_install_legacy_headers(Graphics/Renderer Graphics/Renderer/State
    IPipelineState.hpp RenderState.hpp)

fbzz_install_legacy_headers(Graphics/Renderer Graphics/Renderer/Text
    DynamicFontSource.hpp FontAtlas.hpp)

fbzz_install_legacy_headers(Graphics/Renderer Graphics/Renderer/UI
    IImGuiRenderer.hpp ImGuiContextConfig.hpp)

fbzz_install_legacy_headers(Graphics/Renderer Graphics/Renderer/View
    Camera.hpp HybridQualitySettings.hpp OpaqueRenderPlan.hpp PostProcessBlend.hpp RenderMode.hpp RenderSettings.hpp)

fbzz_install_legacy_headers(Graphics/Renderer Graphics/Renderer/Visibility
    GpuGeometrySelection.hpp GpuInstanceCulling.hpp RenderLayer.hpp RenderObjectGeometry.hpp RenderObjectSelection.hpp RenderVisibility.hpp)

fbzz_install_legacy_headers(Graphics/Renderer Graphics/Renderer/Resources/Buffers
    BufferReadScope.hpp DynamicBufferPool.hpp IBuffer.hpp IConstantBuffer.hpp IStructuredBuffer.hpp)

fbzz_install_legacy_headers(Graphics/Renderer Graphics/Renderer/Resources/Textures
    IRenderTarget.hpp ITexture.hpp SamplerMode.hpp TextureFileDecoder.hpp)

fbzz_install_legacy_headers(Graphics/Renderer Graphics/Renderer/View/Lighting
    ColorTemperature.hpp IIblBaker.hpp LightSystem.hpp ProbeCaptureBudget.hpp RenderEnvironment.hpp RenderLightingInput.hpp)

fbzz_install_legacy_headers(Graphics/Renderer Graphics/RenderGraph
    RenderGraph.hpp RenderGraphAccess.hpp)

fbzz_install_legacy_headers(Graphics/Renderer Graphics/RayTracing
    AccelerationStructure.hpp)

fbzz_install_legacy_headers(Graphics/Renderer Graphics/Scene
    RenderScene.hpp)

fbzz_install_legacy_headers(Graphics/Effects Graphics/Effects/Animation
    BakedBoneReplay.hpp ComputedSkinningOutput.hpp PreparedSkinning.hpp RenderSkinningInput.hpp)

fbzz_install_legacy_headers(Graphics/Effects Graphics/Effects/Decals
    RenderDecalInput.hpp)

fbzz_install_legacy_headers(Graphics/Effects Graphics/Effects/Fiber
    FiberMaterialSettings.hpp RenderFiberInput.hpp)

fbzz_install_legacy_headers(Graphics/Effects Graphics/Effects/Particles
    ParticleBillboards.hpp ParticleColorSpace.hpp ParticleCurve.hpp ParticleDrawTypes.hpp ParticleOverdrawStats.hpp RenderParticleInput.hpp)

fbzz_install_legacy_headers(Graphics/Effects Graphics/Effects/PostProcess
    RenderCustomPostInput.hpp)

fbzz_install_legacy_headers(Graphics/Effects Graphics/Effects/Surfaces
    RenderTerrainInput.hpp RenderWaterInput.hpp WaterGrid.hpp)

fbzz_install_legacy_headers(Graphics/Effects Graphics/Effects/Trails
    RenderMeshTrailInput.hpp RenderTrailInput.hpp)

fbzz_install_legacy_headers(Graphics/Effects Graphics/Scene
    RenderInstanceInput.hpp RenderProbeInput.hpp)

fbzz_install_legacy_headers(Graphics/Pipeline Graphics/Pipeline/Capture
    DirectionalShadowBake.hpp RenderPassCapture.hpp)

fbzz_install_legacy_headers(Graphics/Pipeline Graphics/Pipeline/Culling
    GpuHiZ.hpp OcclusionCuller.hpp)

fbzz_install_legacy_headers(Graphics/Pipeline Graphics/Pipeline/Geometry
    GeometryPipeline.hpp InstanceBatch.hpp MeshletDrawHelpers.hpp)

fbzz_install_legacy_headers(Graphics/Pipeline Graphics/Pipeline/Passes
    IRenderPass.hpp RenderPassContext.hpp RenderPassInjectionPoint.hpp)

fbzz_install_legacy_headers(Graphics/Pipeline Graphics/Pipeline/Resources
    EnvironmentResources.hpp PassResources.hpp RenderConstants.hpp RenderResources.hpp)

fbzz_install_legacy_headers(Graphics/Pipeline Graphics/Pipeline/View
    ResolvedRenderPlan.hpp ViewPipeline.hpp ViewPreparation.hpp)

fbzz_install_legacy_headers(Graphics/Pipeline Graphics/RayTracing
    RayTracingPipeline.hpp)

fbzz_install_legacy_headers(Graphics/Passes/Geometry Graphics/Passes/Geometry/Animation
    ComputedSkinningDemand.hpp)

fbzz_install_legacy_headers(Graphics/Passes/Geometry Graphics/Passes/Geometry/Effects
    MeshTrailRenderPass.hpp TrailRenderPass.hpp)

fbzz_install_legacy_headers(Graphics/Passes/Geometry Graphics/Passes/Geometry/Instancing
    InstanceRenderPasses.hpp)

fbzz_install_legacy_headers(Graphics/Passes/Geometry Graphics/Passes/Geometry/Surfaces
    FiberRenderPass.hpp TerrainRenderPass.hpp WaterRenderPass.hpp)

fbzz_install_legacy_headers(Graphics/RayTracing Graphics/RayTracing/PathTracing
    RayPathHistory.hpp RayPathScene.hpp RayPathTraceResources.hpp)

fbzz_install_legacy_headers(Graphics/RayTracing Graphics/RayTracing/Reflection
    RayReflectionMediumCache.hpp RayReflectionReconstructionResources.hpp RayReflectionResources.hpp)

fbzz_install_legacy_headers(Graphics/RayTracing Graphics/RayTracing/Scene
    RayEnvironment.hpp RayGameResources.hpp RayGeometryCache.hpp RayLightInput.hpp RayMaterialCapabilities.hpp RayScene.hpp SurfaceMaterialData.hpp)
