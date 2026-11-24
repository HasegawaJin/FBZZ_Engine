/// @file    RenderCaptureFrame.hpp
/// @brief   ビューポートの描画時点と撮影結果の対応。
/// @author  Hasegawa Jin
/// @date    2026-11-17
#pragma once
#include <Graphics/Renderer/View/Camera.hpp>
#include <Graphics/Renderer/Resources/ResourceHandle.hpp>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::editor {
struct EditorContext;

struct RenderCaptureFrame {
    struct Object {
        std::string id;
        std::string name;
        std::string parent;
        math::Vector3 position;
    };
    renderer::ResourceHandle<renderer::RenderTargetTag> target;
    std::uint64_t frame = 0;
    std::uint64_t sceneGeneration = 0;
    std::uint64_t revision = 0;
    std::uint64_t resourceOwner = 0;
    std::uint64_t resourceEpoch = 0;
    std::string scenePath;
    std::string sceneName;
    renderer::Camera camera;
    std::string cameraId;
    bool gameView = false;
    bool hasSemantic = false;
    std::vector<Object> objects;
    std::vector<std::string> passes;

    /// @note コマンド応答と Operator 応答で同じ撮影情報を返す。
    template<class Data>
    [[nodiscard]] Data Describe(std::string pass, std::string output, std::int64_t occurrence = 0) const
    {
        Data data = Data::MakeObject();
        data.Set("frame", static_cast<std::int64_t>(frame));
        data.Set("sceneGeneration", static_cast<std::int64_t>(sceneGeneration));
        data.Set("scenePath", scenePath);
        data.Set("sceneName", sceneName);
        data.Set("view", gameView ? "game" : "scene");
        data.Set("cameraKind", !cameraId.empty() ? "node" : gameView ? "editorFallback" : "editor");
        if (!cameraId.empty()) data.Set("cameraId", cameraId);
        data.Set("renderPass", std::move(pass));
        data.Set("occurrence", occurrence);
        data.Set("output", std::move(output));
        Data executed = Data::MakeArray();
        for (const auto& name : passes) executed.Push(Data(name));
        data.Set("passes", std::move(executed));
        return data;
    }
};

/// @note 世代・描画先・編集境界の照合を読み戻しより先に行う。
[[nodiscard]] bool CanReadCaptureFrame(const EditorContext& context, const RenderCaptureFrame& capture,
    renderer::ResourceHandle<renderer::RenderTargetTag> expectedTarget, std::uint64_t minFrame);
} /// @note namespace fbzz::editor
