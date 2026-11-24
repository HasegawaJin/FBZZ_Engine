/// @file    ManagedBatchTransfers.hpp
/// @brief   全件一括操作の未公開入力・確定出力を同期 call が所有する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#pragma once
#include <Engine/Api.hpp>
#include "ManagedTransformBatch.hpp"
#include <memory>
#include <string>
namespace fbzz::scene::managed_detail {
class ManagedBatchTransfers final {
public:
    FBZZ_ENGINE_API ManagedBatchTransfers();
    FBZZ_ENGINE_API ~ManagedBatchTransfers();
    /// @note phase1 begin/2 stage/3 prepare/4 page/5 close/6 publish は flags 上位8bit。低位は元の操作 option。
    FBZZ_ENGINE_API ManagedBridgeStatus Run(Scene& scene, const ManagedBridgeRequest& request, ManagedBridgeReply& reply,
        ManagedTransformBatch& transforms, ManagedTransformBatch::ObjectResolver resolver, void* context);
    /// @note 1 回だけ実行した query の全結果を所有し、page 転送の容量と総結果数を分離する。
    FBZZ_ENGINE_API ManagedBridgeStatus OpenRead(Scene& scene, const ManagedBridgeRequest& request, std::string&& bytes,
        uint32_t count, ManagedBridgeReply& reply);
    /// @note flags 上位 phase2 page / phase3 close。Reserved0/1 は64bit byte offset、低位 option は open 時と同じ。
    FBZZ_ENGINE_API ManagedBridgeStatus ReadSnapshot(Scene& scene, const ManagedBridgeRequest& request, ManagedBridgeReply& reply);
    FBZZ_ENGINE_API void EndCall(uint64_t call);
    FBZZ_ENGINE_API void CancelOwner(uint64_t owner);
    FBZZ_ENGINE_API void RetireGeneration(uint64_t generation);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} /// @note namespace fbzz::scene::managed_detail
