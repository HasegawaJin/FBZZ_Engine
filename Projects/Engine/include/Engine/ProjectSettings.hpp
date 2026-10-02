/// @file    ProjectSettings.hpp
/// @brief   プロジェクト共通設定の定義と永続化。
/// @author  Hasegawa Jin
/// @date    2026-05-23
///
/// @note タグ名・レイヤー名など、エディタとランタイムで共有する軽量設定。
/// @note 読み書きは bool で成否を返し、例外は使わない。
#pragma once
#include <array>
#include <Math/Vector3.hpp>
#include <Engine/Audio/AudioBus.hpp>
#include <Engine/Core/Cursor.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Renderer/RendererBackend.hpp>
#include <Physics/Layer.hpp>
#include <string>
#include <vector>

namespace fbzz {

struct PhysicsSettings {
    int           hz       = 60;
    int           substeps = 1;
    math::Vector3 gravity  = { 0.0f, -9.81f, 0.0f };

    /// @brief レイヤー同士がぶつかるか。既定は «全部ぶつかる»。
    /// @note 親の剛体だけを除外する当たりにも使うため、BroadPhase の layerFilter へ渡す値を保持する。
    /// @note 衝突解決は 1 組に 1 回のため、除外関係は対称に保持する。
    LayerCollisionMatrix collisionMatrix;
};

struct AudioSettings {
    float masterVolume = 1.0f;

    /// @note 同時 voice の上限。超過時は優先度の低い音を畳んで場所を空ける。
    int voiceLimit = 48;

    /// @note 先頭は必ず Master。旧 bgmVolume / seVolume は既定 BGM / SE バスへ移行する。
    std::vector<audio::BusDesc> buses = audio::DefaultBusLayout();

    /// @brief masterVolume を反映した AudioManager::ApplyBusLayout 用の構成。
    [[nodiscard]] std::vector<audio::BusDesc> BuildBusLayout() const;
};

struct ScreenSettings {
    int width  = 1920;
    int height = 1080;
};

struct AppSettings {
    int targetFps = 60;  ///< @note 0 は無制限。
    /// @note 描画バックエンドは EditorLauncher が Application::Init より前に起動プロジェクトから読む。
    /// @note --renderer= があれば優先。起動後のプロジェクト切り替えではバックエンドを変更しない。
    renderer::RendererBackend rendererBackend = renderer::RendererBackend::DX12;
};

/// @brief 配布ゲームのウィンドウ設定。
/// @note Application::Init より前に読み、正しいサイズとタイトルで生成する。
struct WindowSettings {
    std::string title      = "FBZZ Game";
    int         width      = 1920;
    int         height     = 1080;
    bool        fullscreen = false;
};

struct UISettings {
    std::string defaultFontPath = "Assets/Fonts/Default/Roboto/Roboto-VariableFont_wdth,wght";
};

/// @brief カーソルの «絵» だけを持つ設定。拘束と表示はここには置かない。
/// @note 実行中の cursor.Push 要求はスクリプトが正本。ここには差し替え可能な絵だけを残す。
struct CursorAppearance {
    struct ShapeImage {
        std::string path;              ///< @note プロジェクト相対パス。空なら OS の既定矢印。
        float       hotspotX = 0.0f;   ///< @note 画像左上から実際に指す点までの画素。
        float       hotspotY = 0.0f;
    };

    /// @note false なら OS 既定の矢印を使う。
    bool hardwareCursor = true;
    std::array<ShapeImage, core::kCursorShapeCount> shapes{};

    /// @brief 絶対 projectRoot から画像を読み、Play 開始時と配布ゲーム起動時に OS へ適用する。
    void Apply(const std::string& projectRoot) const;
};

struct ProjectMetadataSettings {
    std::string name;
    std::string defaultScene = "Assets/Scenes/Main.scene";
};

struct RuntimeSettings {
    std::string startScene = "Assets/Scenes/Main.scene";
};

/// @brief ゲーム固有の設定をひとまとめにするサブ構造体。
/// @note タグ・レイヤー名・シーンパス等を game に集約し、エンジン側の誤った依存を防ぐ。
struct GameProjectConfig {
    ProjectMetadataSettings      project;
    RuntimeSettings              runtime;
    std::vector<std::string>    tags = {
        "Untagged", "Respawn", "Finish", "EditorOnly",
        "MainCamera", "Player", "GameController"
    };
    std::array<std::string, 32> layerNames = {
        "Default", "TransparentFX", "Ignore Raycast", "", "Water", "UI",
        "", "", "", "Static", "", "", "", "", "", "",
        "", "", "", "", "", "", "", "", "", "",
        "", "", "", "", "", ""
    };
};

struct ProjectSettings {
    GameProjectConfig            game;     ///< @note タグ・レイヤー・シーンパス等のゲーム固有設定。
    PhysicsSettings              physics;
    renderer::RenderSettings     render;
    /// @note Load keeps this reference unresolved until AssetManager initialization; render remains the detach/missing-asset fallback.
    std::string                  renderPipelineAssetPath;
    AudioSettings                audio;
    UISettings                   ui;
    ScreenSettings               screen;
    AppSettings                  app;
    WindowSettings               window;
    CursorAppearance             cursor;   ///< @note カーソルの絵だけ。拘束と表示はスクリプトが持つ。

    static ProjectSettings Default();
    bool Load(const std::string& path);
    bool Save(const std::string& path) const;
    /// @brief Save が書く TOML 本文。
    /// @note エディターの自動保存が «前回保存した内容と違うか» の比較に使う (構造体に == が無いため)。
    [[nodiscard]] std::string ToToml() const;
};

} /// @note namespace fbzz
