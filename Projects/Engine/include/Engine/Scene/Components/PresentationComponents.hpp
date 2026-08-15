// FBZZ Engine
// PresentationComponents.hpp | fbzz::scene
// 2Dスプライト、線、ビルボード、投影表示に使う汎用描画Component
#pragma once

#include <Engine/Scene/EntityRef.hpp>
#include <Engine/Scene/Script.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <memory>
#include <cstddef>
#include <string>
#include <vector>

namespace fbzz::scene {

enum class SpriteDrawMode : int { Simple = 0, Sliced = 1, Tiled = 2 };

struct SpriteRendererComponent {
    bool enabled = true;
    std::string spritePath;
    std::string materialPath = "Assets/Materials/Fallback/SpriteFallback.mat";
    math::Vector4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
    math::Vector2 size = { 1.0f, 1.0f };
    math::Vector2 pivot = { 0.5f, 0.5f };
    SpriteDrawMode drawMode = SpriteDrawMode::Simple;
    int sortingLayer = 0;
    int orderInLayer = 0;
    bool flipX = false;
    bool flipY = false;
    bool receiveLighting = false;
    std::shared_ptr<renderer::Mesh> runtimeMesh;
    std::size_t runtimeSignature = 0;

    const char* GetTypeName() const { return "Sprite Renderer"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("spritePath", spritePath);
        r.Field("materialPath", materialPath);
        r.Field("color", color);
        r.Field("size", size);
        r.Field("pivot", pivot);
        int mode = static_cast<int>(drawMode);
        static constexpr const char* MODES[] = { "Simple", "Sliced", "Tiled" };
        r.Enum("drawMode", mode, MODES);
        drawMode = static_cast<SpriteDrawMode>(mode < 0 || mode > 2 ? 0 : mode);
        r.Field("sortingLayer", sortingLayer);
        r.Field("orderInLayer", orderInLayer);
        r.Field("flipX", flipX);
        r.Field("flipY", flipY);
        r.Field("receiveLighting", receiveLighting);
    }
};

struct SortingGroupComponent {
    bool enabled = true;
    int sortingLayer = 0;
    int orderInLayer = 0;
    std::shared_ptr<renderer::Mesh> runtimeMesh;
    std::size_t runtimeSignature = 0;

    const char* GetTypeName() const { return "Sorting Group"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("sortingLayer", sortingLayer);
        r.Field("orderInLayer", orderInLayer);
    }
};

enum class LineSpace : int { Local = 0, World = 1 };

struct LineRendererComponent {
    bool enabled = true;
    std::vector<math::Vector3> points;
    std::string materialPath = "Assets/Materials/Fallback/VFXMeshFallback.mat";
    math::Vector4 startColor = { 1.0f, 1.0f, 1.0f, 1.0f };
    math::Vector4 endColor = { 1.0f, 1.0f, 1.0f, 1.0f };
    float startWidth = 0.05f;
    float endWidth = 0.05f;
    LineSpace space = LineSpace::Local;
    bool loop = false;
    bool billboard = true;
    int sortingLayer = 0;
    int orderInLayer = 0;
    std::shared_ptr<renderer::Mesh> runtimeMesh;
    std::size_t runtimeSignature = 0;

    const char* GetTypeName() const { return "Line Renderer"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.ListField("points", points);
        r.Field("materialPath", materialPath);
        r.Field("startColor", startColor);
        r.Field("endColor", endColor);
        r.Field("startWidth", startWidth);
        r.Field("endWidth", endWidth);
        int value = static_cast<int>(space);
        static constexpr const char* SPACES[] = { "Local", "World" };
        r.Enum("space", value, SPACES);
        space = static_cast<LineSpace>(value < 0 || value > 1 ? 0 : value);
        r.Field("loop", loop);
        r.Field("billboard", billboard);
        r.Field("sortingLayer", sortingLayer);
        r.Field("orderInLayer", orderInLayer);
    }
};

enum class BillboardMode : int { FaceCamera = 0, YAxisOnly = 1, MatchCamera = 2 };

struct BillboardComponent {
    bool enabled = true;
    BillboardMode mode = BillboardMode::FaceCamera;
    bool reverseForward = false;

    const char* GetTypeName() const { return "Billboard"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        int value = static_cast<int>(mode);
        static constexpr const char* MODES[] = { "Face Camera", "Y Axis Only", "Match Camera" };
        r.Enum("mode", value, MODES);
        mode = static_cast<BillboardMode>(value < 0 || value > 2 ? 0 : value);
        r.Field("reverseForward", reverseForward);
    }
};

enum class ProjectorShape : int { Box = 0, Perspective = 1 };

struct ProjectorComponent {
    bool enabled = true;
    ProjectorShape shape = ProjectorShape::Box;
    std::string materialPath;
    math::Vector4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
    float fieldOfView = 45.0f;
    float nearClip = 0.01f;
    float farClip = 10.0f;
    int receiverLayerMask = -1;
    std::string runtimeLoadedMaterialPath;

    const char* GetTypeName() const { return "Projector"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        int value = static_cast<int>(shape);
        static constexpr const char* SHAPES[] = { "Box", "Perspective" };
        r.Enum("shape", value, SHAPES);
        shape = static_cast<ProjectorShape>(value < 0 || value > 1 ? 0 : value);
        r.Field("materialPath", materialPath);
        r.Field("color", color);
        r.FloatRange("fieldOfView", fieldOfView, 1.0f, 179.0f);
        r.Field("nearClip", nearClip);
        r.Field("farClip", farClip);
        r.Field("receiverLayerMask", receiverLayerMask);
    }
};

} // namespace fbzz::scene
