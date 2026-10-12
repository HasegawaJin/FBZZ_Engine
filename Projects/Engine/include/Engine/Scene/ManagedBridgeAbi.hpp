/// @file    ManagedBridgeAbi.hpp
/// @brief   常駐 managed Host と native Engine の値・ID 境界。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace fbzz::scene {

/// @note Windows x64 / Pack=8。input は長さ付き UTF-8、output は呼出元の借用バッファ。文字列 WRITE_FIELD の input は field key + NUL + value。INITIALIZE は generation=schema hash、integer=native Dispatch のビット列、index=sizeof(Request)、flags=sizeof(Reply)。
/// @see https://learn.microsoft.com/en-us/dotnet/core/tutorials/netcore-hosting .NET native hosting。
/// @note Live native 操作の generation/instanceId/ownerId/callId は caller の権限、integer は対象 object ID。対象 0 は欠損。BIND_OWNER のみ callId が Scene lifetime、input が所有者 GUID。
/// @note 配列 payload は little endian uint32 count の後、float32/int32/bool8/Vector3(float32*3)、文字列と GUID は uint32 UTF-8 byte length + bytes。length=UINT32_MAX は null スロットで GameObject[] だけに許可。
/// @note 複合 WRITE_FIELD は key + NUL + payload、GameObject 単体は GUID。VALIDATE_FIELDS/WRITE_FIELDS は JSON object、MANAGED_COMPONENTS は uint32 count + uint64 managed instance IDs。
/// @note outputBytes は終端 NUL を含まない必要長。容量不足は出力なしで BUFFER_TOO_SMALL。GET_TYPE / GET_FIELD の index は全件数。FLOAT / VECTOR3 は x/y/z/w、INT / BOOL は integer を使う。
/// @note Objects batch の index は入力数、input は ObjectId + Scene lifetime の little endian uint64 行。位置設定は XYZ float32 を続ける。read は status uint32 + XYZ、set は status uint32 を output へ返し、reply index/flags は適用数/スキップ数。変更前に全件を検証し、再送しない。
/// @see Engine/Scene/ManagedBridgeContract.inl 両言語の宣言と全 field offset の正本。
#define FBZZ_MANAGED_CONST(nativeType, csType, nativeName, csName, value) inline constexpr nativeType nativeName = static_cast<nativeType>(value);
#define FBZZ_MANAGED_ENUM_BEGIN(nativeName, csName, nativeType, csType, target, access) enum class nativeName : nativeType {
#define FBZZ_MANAGED_ENUM_VALUE(nativeName, csName, value) nativeName = value,
#define FBZZ_MANAGED_ENUM_END() };
#define FBZZ_MANAGED_STRUCT_BEGIN(nativeName, csName) struct alignas(8) nativeName {
#define FBZZ_MANAGED_FIELD(nativeType, csType, nativeName, csName, initial) nativeType nativeName = initial;
#define FBZZ_MANAGED_STRUCT_END() };
#define FBZZ_MANAGED_SIZE(nativeName, csName, size) static_assert(sizeof(nativeName) == size);
#define FBZZ_MANAGED_OFFSET(nativeName, csName, nativeField, csField, offset) static_assert(offsetof(nativeName, nativeField) == offset);
#define FBZZ_MANAGED_CALL(...)
#define FBZZ_MANAGED_CALLBACK(...)
#include "ManagedBridgeContract.inl"
#undef FBZZ_MANAGED_CONST
#undef FBZZ_MANAGED_ENUM_BEGIN
#undef FBZZ_MANAGED_ENUM_VALUE
#undef FBZZ_MANAGED_ENUM_END
#undef FBZZ_MANAGED_STRUCT_BEGIN
#undef FBZZ_MANAGED_FIELD
#undef FBZZ_MANAGED_STRUCT_END
#undef FBZZ_MANAGED_SIZE
#undef FBZZ_MANAGED_OFFSET
#undef FBZZ_MANAGED_CALL
#undef FBZZ_MANAGED_CALLBACK

/// @note The shared callback declarations also govern native access, managed discovery and authoring diagnostics.
inline constexpr const char* ManagedCallbackPolicy(ManagedCallback callback)
{
    switch (callback) {
    case ManagedCallback::EVENT:
    case ManagedCallback::JOB: return "live";
#define FBZZ_MANAGED_CONST(...)
#define FBZZ_MANAGED_ENUM_BEGIN(...)
#define FBZZ_MANAGED_ENUM_VALUE(...)
#define FBZZ_MANAGED_ENUM_END(...)
#define FBZZ_MANAGED_STRUCT_BEGIN(...)
#define FBZZ_MANAGED_FIELD(...)
#define FBZZ_MANAGED_STRUCT_END(...)
#define FBZZ_MANAGED_SIZE(...)
#define FBZZ_MANAGED_OFFSET(...)
#define FBZZ_MANAGED_CALL(...)
#define FBZZ_MANAGED_CALLBACK(nativeName, methodName, parameterType, policy) case ManagedCallback::nativeName: return policy;
#include "ManagedBridgeContract.inl"
#undef FBZZ_MANAGED_CONST
#undef FBZZ_MANAGED_ENUM_BEGIN
#undef FBZZ_MANAGED_ENUM_VALUE
#undef FBZZ_MANAGED_ENUM_END
#undef FBZZ_MANAGED_STRUCT_BEGIN
#undef FBZZ_MANAGED_FIELD
#undef FBZZ_MANAGED_STRUCT_END
#undef FBZZ_MANAGED_SIZE
#undef FBZZ_MANAGED_OFFSET
#undef FBZZ_MANAGED_CALL
#undef FBZZ_MANAGED_CALLBACK
    default: return "no_engine_api";
    }
}

using ManagedBridgeDispatch = int32_t (__cdecl*)(const ManagedBridgeRequest*, ManagedBridgeReply*);

static_assert(sizeof(void*) == 8);
static_assert(std::is_standard_layout_v<ManagedBridgeRequest> && std::is_trivially_copyable_v<ManagedBridgeRequest>);
static_assert(std::is_standard_layout_v<ManagedBridgeReply> && std::is_trivially_copyable_v<ManagedBridgeReply>);

} /// @note namespace fbzz::scene
