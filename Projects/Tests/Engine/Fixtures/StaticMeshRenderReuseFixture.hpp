/// @file    StaticMeshRenderReuseFixture.hpp
/// @brief   静的Meshの再利用と動的fallbackを公開snapshot・Profilerで観測する共通fixture。
/// @author  Hasegawa Jin
/// @date    2026-10-23
#pragma once
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Core/Profiler/Profiler.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneUtils.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Systems/RenderSceneExtractor.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Scene/Script.hpp>
#include "../../../Engine/src/Scene/Managed/ManagedRenderComponentServices.hpp"
#include "../../../Engine/src/Scene/Managed/ManagedServicesBridge.hpp"
#include <algorithm>
#include <array>
#include <memory>
#include <string_view>

namespace fbzz::tests {
class StaticMeshRenderReuseTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        m_playing = scene::Script::IsInPlayMode();
        scene::Script::SetInPlayMode(true);
        m_enabled = profiler::Profiler::IsEnabled();
        m_recording = profiler::Profiler::IsRecordingRequested();
        m_filter = profiler::Profiler::GetRequestedCaptureFilter();
        profiler::Profiler::SetEnabled(false); profiler::Profiler::SetEnabled(true);
        profiler::Profiler::RequestRecording(true); profiler::Profiler::RequestCaptureFilter({});
        m_mesh.vertexBuffer = {11, 2}; m_mesh.indexBuffer = {12, 3};
        m_mesh.vertexCount = 24; m_mesh.indexCount = 36;
        m_mesh.boundsCenter = {1, 0, 0}; m_mesh.boundsRadius = 2; m_mesh.boundsExtents = {1, 1, 1};
        profiler::Profiler::BeginFrame();
        (void)scene::ExtractRenderSceneGeometry(m_scene, 0);
        profiler::Profiler::EndFrame();
    }
    void TearDown() override
    {
        m_scene.Clear();
        scene::Script::SetInPlayMode(m_playing);
        profiler::Profiler::RequestCaptureFilter(m_filter);
        profiler::Profiler::RequestRecording(m_recording);
        profiler::Profiler::SetEnabled(false); profiler::Profiler::SetEnabled(m_enabled);
        EngineFixture::TearDown();
    }
    scene::GameObject& Object(scene::Scene& owner)
    {
        auto& object = owner.CreateGameObject("Static reuse");
        object.AddComponent<scene::MeshRenderer>().mesh = &m_mesh;
        return object;
    }
    renderer::RenderScene Extract(uint64_t frame)
    {
        profiler::Profiler::BeginFrame();
        auto snapshot = scene::ExtractRenderSceneGeometry(m_scene, frame);
        profiler::Profiler::EndFrame();
        return snapshot;
    }
    size_t Rebuilds() const
    {
        return SampleCount("RenderSceneExtractor::StaticGeometryRebuild");
    }
    size_t SampleCount(std::string_view name) const
    {
        const auto& records = profiler::Profiler::GetLastFrameRecords();
        return static_cast<size_t>(std::count_if(records.begin(), records.end(), [name](const auto& record) {
            return std::string_view(record.name) == name;
        }));
    }
    scene::Scene m_scene;
    renderer::Mesh m_mesh;
private:
    bool m_enabled = false, m_recording = false, m_playing = false;
    profiler::PerformanceCaptureFilter m_filter;
};
}
