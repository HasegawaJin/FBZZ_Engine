// FBZZ Engine
// ProjectSettings.hpp | fbzz
// プロジェクト共通設定の定義と永続化
// タグ名・レイヤー名など、エディタとランタイムで共有する軽量設定。
// 読み書きは bool で成否を返し、例外は使わない。
#pragma once
#include <array>
#include <Math/Vector3.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <string>
#include <vector>

namespace fbzz {

struct PhysicsSettings {
    int           hz       = 60;
    int           substeps = 4;
    math::Vector3 gravity  = { 0.0f, -9.81f, 0.0f };
};

struct AudioSettings {
    float bgmVolume = 1.0f;
    float seVolume  = 1.0f;
};

struct ScreenSettings {
    int width  = 1920;
    int height = 1080;
};

struct AppSettings {
    int targetFps = 60;  // 0 = unlimited
};

// Standalone モード (配布ゲーム) のウィンドウ設定。
// Application::Init() より前に ProjectSettings を読み込み、
// 正しいサイズ・タイトルでウィンドウを生成するために使う。
struct WindowSettings {
    std::string title      = "FBZZ Game";
    int         width      = 1920;
    int         height     = 1080;
    bool        fullscreen = false;
};

struct ProjectMetadataSettings {
    std::string name;
    std::string defaultScene = "Assets/Scenes/Main.fbzz";
};

struct RuntimeSettings {
    std::string startScene = "Assets/Scenes/Main.fbzz";
};

struct ProjectSettings {
    ProjectMetadataSettings      project;
    RuntimeSettings              runtime;
    std::vector<std::string>    tags = {
        "Untagged", "Respawn", "Finish", "EditorOnly",
        "MainCamera", "Player", "GameController"
    };
    std::array<std::string, 32> layerNames = {
        "Default", "TransparentFX", "Ignore Raycast", "", "Water", "UI",
        "", "", "", "", "", "", "", "", "", "",
        "", "", "", "", "", "", "", "", "", "",
        "", "", "", "", "", ""
    };
    PhysicsSettings             physics;
    renderer::RenderSettings    render;
    AudioSettings               audio;
    ScreenSettings              screen;
    AppSettings                 app;
    WindowSettings              window;

    static ProjectSettings Default();
    bool Load(const std::string& path);
    bool Save(const std::string& path) const;
};

} // namespace fbzz
