/// @file    CellAssetStreaming.hpp
/// @brief   CPU 向け配置アセットの非同期読み込みと、リースを考慮したセル常駐の解除。
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Engine/Api.hpp>
#include <cstdint>
#include <memory>
#include <string>

namespace fbzz::asset {
class IAssetStreamChannel;
/// @note 同期・非同期の配置ハンドルでストアを共有できるよう Engine DLL 内で構築する。
[[nodiscard]] FBZZ_ENGINE_API std::shared_ptr<IAssetStreamChannel> CreateMeshInstancesStreamChannel();
/// @note メインスレッド専用。Scene 側の所有者を退役させ、そのリースを解放してから呼ぶ。
/// @return リース、公開待ち処理、同期ピンが資産を保護している場合は false。
[[nodiscard]] FBZZ_ENGINE_API bool EvictUnusedMeshInstances(const std::string& reference);
/// @note メインスレッド専用。配置スナップショットと保持中の編集列の容量を見積もる。読み込みやピン留めは行わない。
/// @return 配置アセットが共有ストアに未公開なら 0。
[[nodiscard]] FBZZ_ENGINE_API uint64_t MeshInstancesResidentBytes(const std::string& reference);
} /// @note namespace fbzz::asset
