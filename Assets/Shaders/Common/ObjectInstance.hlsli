/// @file    ObjectInstance.hlsli
/// @brief   Shared ordinary and resident instance access for color, depth and motion passes.
/// @author  Hasegawa Jin
/// @date    2026-09-20
#ifndef FBZZ_COMMON_OBJECT_INSTANCE_HLSLI
#define FBZZ_COMMON_OBJECT_INSTANCE_HLSLI
#include "Binding.hlsli"
#include "BindlessIndices.hlsli"
#ifdef FBZZ_INSTANCED
struct ObjectInstance { float4x4 world; float4x4 worldInvTranspose; };
struct ObjectInstanceMotion { float4x4 world; float4x4 prevWorld; };
#ifdef FBZZ_RESIDENT_INSTANCES
/// @note 208-byte layout matches ResidentInstanceRecord; IDs are view-owned, source rows are persistent.
struct ResidentInstanceRecord { float4x4 world; float4x4 normal; float4x4 previousWorld; float4 bounds; };
FBZZ_VS_SBUFFER(ResidentInstanceRecord,g_residentInstances,VS_SB_INSTANCE_SLOT);
FBZZ_VS_SBUFFER(uint,g_visibleInstanceIds,VS_SB_BUFFER0_SLOT);
cbuffer InstanceDrawOffset : register(b15) { uint g_instanceIdOffset; };
ObjectInstance FbzzObjectInstance(uint id)
{
    const ResidentInstanceRecord record=g_residentInstances[g_visibleInstanceIds[g_instanceIdOffset+id]];
    ObjectInstance result; result.world=record.world; result.worldInvTranspose=record.normal; return result;
}
ObjectInstanceMotion FbzzObjectInstanceMotion(uint id)
{
    const ResidentInstanceRecord record=g_residentInstances[g_visibleInstanceIds[g_instanceIdOffset+id]];
    ObjectInstanceMotion result; result.world=record.world; result.prevWorld=record.previousWorld; return result;
}
#else
/// @note Ordinary 128-byte rows retain the existing instance and velocity batch contracts.
FBZZ_VS_SBUFFER(ObjectInstance,gObjectInstances,VS_SB_INSTANCE_SLOT);
FBZZ_VS_SBUFFER(ObjectInstanceMotion,gObjectInstancesMotion,VS_SB_INSTANCE_SLOT);
ObjectInstance FbzzObjectInstance(uint id) { return gObjectInstances[id]; }
ObjectInstanceMotion FbzzObjectInstanceMotion(uint id) { return gObjectInstancesMotion[id]; }
#endif
#endif
#endif
