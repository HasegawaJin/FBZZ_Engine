/// @file    RayDebugPass.hpp
/// @brief   カメラレイの距離・幾何法線・完全な object ID の診断表示。
/// @author  Hasegawa Jin
/// @date    2026-09-30
#pragma once
#include <Graphics/Pipeline/IRenderPass.hpp>
#include <Graphics/RayTracing/RayGeometryCache.hpp>

namespace fbzz::renderer {
class Camera;
/// @note LAYOUT: RayDebug.cs.hlsl b0。right/up の w は投影面の半幅/半高さ。
struct RayDebugConstants {
    math::Vector4 cameraPosition;
    math::Vector4 cameraRight;
    math::Vector4 cameraUp;
    math::Vector4 cameraForward;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mode = 0;
    uint32_t instanceCount = 0;
    uint32_t orthographic = 0;
    uint32_t incomplete = 0;
    float nearDistance = 0;
    float farDistance = 0;
};
static_assert(sizeof(RayDebugConstants) == 96);
/// @note 一般 VP 逆行列の特異判定と遠方面の斉次除算を避け、Camera と同じ投影パラメーターを使う。
/// @see https://pbr-book.org/4ed/Cameras_and_Film/Projective_Camera_Models §5.2 Orthographic / Perspective cameras
[[nodiscard]] bool MakeRayDebugCameraConstants(const Camera& camera, RayDebugConstants& output);

class RayDebugPass final : public IRenderPass {
public:
    RayDebugPass(const RaySceneGpu& scene, ResourceHandle<ConstantBufferTag> constants,
        ResourceHandle<ShaderTag> shader, bool incomplete)
        : m_scene(scene), m_constants(constants), m_shader(shader), m_incomplete(incomplete) {}
    std::string_view Name() const override { return "RayDebug"; }
    void Setup(PassBuilder& builder, const RenderPassContext&) const override;
    void Execute(PassResources& resources, RenderPassContext& context) override;
private:
    RaySceneGpu m_scene;
    ResourceHandle<ConstantBufferTag> m_constants;
    ResourceHandle<ShaderTag> m_shader;
    bool m_incomplete = false;
};

class RayDebugCopyPass final : public IRenderPass {
public:
    RayDebugCopyPass(std::string outputName, ResourceHandle<ShaderTag> shader,
        ResourceHandle<PipelineStateTag> state) : m_outputName(std::move(outputName)), m_shader(shader), m_state(state) {}
    std::string_view Name() const override { return "RayDebugCopy"; }
    void Setup(PassBuilder& builder, const RenderPassContext&) const override;
    void Execute(PassResources& resources, RenderPassContext& context) override;
private:
    std::string m_outputName;
    ResourceHandle<ShaderTag> m_shader;
    ResourceHandle<PipelineStateTag> m_state;
};
} /// @note namespace fbzz::renderer
