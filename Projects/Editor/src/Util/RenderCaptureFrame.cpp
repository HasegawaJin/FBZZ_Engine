/// @file    RenderCaptureFrame.cpp
/// @brief   撮影前の描画元と現在の Editor の照合。
/// @author  Hasegawa Jin
/// @date    2026-11-17
#include <Editor/Util/RenderCaptureFrame.hpp>
#include <Editor/EditorContext.hpp>
#include <Engine/Renderer/ResourceManager.hpp>

namespace fbzz::editor {
bool CanReadCaptureFrame(const EditorContext& context, const RenderCaptureFrame& capture,
    renderer::ResourceHandle<renderer::RenderTargetTag> expectedTarget, std::uint64_t minFrame)
{
    return capture.frame != 0 && capture.frame >= minFrame && capture.target == expectedTarget
        && context.activeScene && capture.sceneGeneration == context.activeScene->GetRenderSceneGeneration()
        && capture.revision == context.captureRevision && context.resources
        && capture.resourceOwner == context.resources->GetInstanceId()
        && capture.resourceEpoch == context.resources->GetResetVersion();
}
} /// @note namespace fbzz::editor
