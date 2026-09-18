/// @file    FlowFieldSystem.cpp
/// @brief   流れのフレームキャッシュの更新。Scene::FlowFrame() の遅延更新もここに置く。
/// @author  Hasegawa Jin
/// @date    2026-09-16
#include <Engine/Scene/Fields/FlowFieldSystem.hpp>

#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Environment/SceneEnvironment.hpp>
#include <Engine/Scene/Fields/FlowFieldEval.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Transform.hpp>

namespace fbzz::scene {

/// @brief 遅延更新の実体を Scene.cpp ではなくここへ置くのは、収集の式と «いつ集め直すか» を
/// @brief 1 ファイルに並べておくため。
FlowFieldFrame& Scene::FlowFrame()
{
    if (m_flowFrame.frame != Time::frameCount) {
        m_flowFrame.frame = Time::frameCount;
        auto fields = std::make_shared<std::vector<ActiveFlowField>>();
        GatherFlowFields(*this, *fields);
        m_flowFrame.sceneFieldCount = fields->size();
        m_flowFrame.ambient = ResolveAmbientWind(m_environment);
        AppendEnvironmentFlow(m_flowFrame.ambient, *fields);
        m_flowFrame.fields = std::move(fields);
    }
    return m_flowFrame;
}

void Scene::InvalidateFlowFrame()
{
    m_flowFrame.frame = FlowFieldFrame::kNeverFilled;
}

ComponentAccess FlowFieldSystem::GetAccess() const
{
    return ComponentAccess{}.Reads<Transform>().Reads<FlowField>();
}

OrderingHints FlowFieldSystem::GetOrder() const
{
    /// @note TransformPrePhysics は RunMode::SimOnly。編集中はこのヒントの相手が走らず、
    ///       代わりに TransformEditorPreview (PreScript) がワールド姿勢を確定させている。
    return OrderingHints{}.After<TransformPrePhysics>();
}

void FlowFieldSystem::Update(SystemContext& ctx)
{
    /// @note 遅延更新を «物理より前» で強制的に踏む。剛体が読む時点で確定していることが仕事。
    (void)ctx.scene.FlowFrame();
}

} // namespace fbzz::scene
