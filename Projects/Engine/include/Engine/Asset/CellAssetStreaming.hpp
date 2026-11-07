/// @file    CellAssetStreaming.hpp
/// @brief   Asynchronous CPU placement assets and lease-safe cell residency retirement.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Engine/Api.hpp>
#include <cstdint>
#include <memory>
#include <string>

namespace fbzz::asset {
class IAssetStreamChannel;
/// @note Construct inside Engine DLL so synchronous and streamed placement handles share one store.
[[nodiscard]] FBZZ_ENGINE_API std::shared_ptr<IAssetStreamChannel> CreateMeshInstancesStreamChannel();
/// @note Main-thread-only. Retire Scene owners before releasing their leases and calling this function.
/// @return False if any lease, pending publication or synchronous pin still protects the asset.
[[nodiscard]] FBZZ_ENGINE_API bool EvictUnusedMeshInstances(const std::string& reference);
/// @note Main-thread-only estimate of placement snapshots and their retained authoring columns; never loads or pins.
/// @return Zero when the placement asset is not published in the shared store.
[[nodiscard]] FBZZ_ENGINE_API uint64_t MeshInstancesResidentBytes(const std::string& reference);
} /// @note namespace fbzz::asset
