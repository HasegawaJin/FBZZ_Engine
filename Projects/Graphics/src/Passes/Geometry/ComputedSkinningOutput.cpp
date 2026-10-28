/// @file    ComputedSkinningOutput.cpp
/// @brief   遅延した GPU 姿勢を元入力のまま後段描画へ引き渡す。
/// @author  Hasegawa Jin
/// @date    2026-10-16
#include <Graphics/Effects/ComputedSkinningOutput.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Core/ExternalCallbackBoundary.hpp>

namespace fbzz::renderer {
bool ResolvePreparedSkinningOutput(const std::shared_ptr<PreparedSkinningTicket>& ticket,
    uint32_t row, ResourceHandle<BufferTag>& output, uint64_t* contentVersion)
{
    if (!ticket || row >= ticket->Writes().size()) return false;
    if (!ticket->Writes()[row].recorded) {
        /// @note 初回だけ同じ view の公開 snapshot を完了する。提出済み行は自分の世代だけを検証する。
        (void)core::NotifyBeforeExternalCallback();
        if (!ticket->Writes()[row].recorded && !SubmitPreparedSkinning(ticket)) return false;
    }
    const auto writes = ticket->Writes();
    if (row >= writes.size() || !writes[row].recorded || !writes[row].output.IsValid()
        || writes[row].resultingVersion == 0 || !ticket->IsWriteCurrent(row)) return false;
    output = writes[row].output;
    if (contentVersion) *contentVersion = writes[row].resultingVersion;
    return true;
}

bool BindPreparedSkinningOutput(const RenderMeshItem& item, DrawCall& call)
{
    if (!item.preparedSkinning) return true;
    auto output = item.deformedVertexBuffer;
    if (!ResolvePreparedSkinningOutput(item.preparedSkinning, item.preparedSkinningWrite, output)) return false;
    call.vertexBuffer = output;
    call.deformationSource = item.skinningVertexBuffer;
    call.preparedSkinning = item.preparedSkinning;
    call.preparedSkinningRow = item.preparedSkinningWrite;
    return true;
}
}
