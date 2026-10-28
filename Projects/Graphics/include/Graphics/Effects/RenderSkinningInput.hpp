/// @file    RenderSkinningInput.hpp
/// @brief   解決済みバッファへの GPU スキニング要求。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Renderer/ResourceHandle.hpp>
#include <cstdint>
#include <vector>
namespace fbzz::renderer {
struct RenderSkinningInput {
    ResourceHandle<StructuredBufferTag> sourceVertices;
    ResourceHandle<StructuredBufferTag> bonePalette;
    ResourceHandle<BufferTag> outputVertices;
    uint32_t vertexCount = 0;
    /// @note Packed matrices are view-owned; zero boneCount retains the standalone palette contract.
    uint32_t boneOffset = 0;
    uint32_t boneCount = 0;
    /// @note External GPU palette uses boneOffset/boneCount without a CPU matrix upload; zero preserves existing callers.
    bool gpuBonePalette = false;
};
/// @note Versions describe completed CPU recording of DIRECT writes, never GPU fence completion or accepted CPU updates.
struct RenderSkinningWriteReceipt {
    ResourceHandle<BufferTag> output;
    uint64_t previousVersion = 0, resultingVersion = 0;
    bool recorded = false;
};
/// @note Owned results are consumed only by the matching resource instance/epoch and the caller's current scene owner.
struct RenderSkinningBatchReceipt {
    uint64_t resourceInstance = 0, resourceEpoch = 0, resourceFrame = 0;
    bool ownedPath = false;
    std::vector<RenderSkinningWriteReceipt> writes;
};
}
