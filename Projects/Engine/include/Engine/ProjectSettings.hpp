/// @file    ProjectSettings.hpp
/// @brief   プロジェクト共通設定の定義と永続化。
/// @author  Hasegawa Jin
/// @date    2026-05-23
///
/// タグ名・レイヤー名など、エディタとランタイムで共有する軽量設定。
/// 読み書きは bool で成否を返し、例外は使わない。
#pragma once
#include <array>
#include <Math/Vector3.hpp>
#include <Engine/Audio/AudioBus.hpp>
#include <Engine/Core/Cursor.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <Engine/Renderer/RendererBackend.hpp>
#include <string>
#include <vector>

namespace fbzz {

struct PhysicsSettings {
    int           hz       = 60;
    int           substeps = 1;
    math::Vector3 gravity  = { 0.0f, -9.81f, 0.0f };
};

struct AudioSettings {
    float masterVolume = 1.0f;

    /// 同時に鳴らせる voice の上限。超えた状態で鳴らそうとすると、優先度の低い音を
    /// 畳んで場所を空ける。上げすぎると同種の音が重なって音量が飽和する。
    int voiceLimit = 48;

    /// ミキサーバス構成。先頭は必ず Master (AudioManager 側で正規化される)。
    /// 旧形式の bgmVolume / seVolume を持つ設定ファイルは、読み込み時に
    /// 既定構成の BGM / SE バスの音量へ移行する。
    std::vector<audio::BusDesc> buses = audio::DefaultBusLayout();

    /// masterVolume を反映した、AudioManager::ApplyBusLayout へ渡す構成。
    [[nodiscard]] std::vector<audio::BusDesc> BuildBusLayout() const;
};

struct ScreenSettings {
    int width  = 1920;
    int height = 1080;
};

struct AppSettings {
    int targetFps = 60;  // 0 = unlimited
    // 描画バックエンド (dx11 / dx12)。Standalone / Editor とも「起動時プロジェクト」のこの値で
    // レンダラーを生成する (EditorLauncher が Application::Init より前に先読みして渡す)。
    // WHY: プロジェクトごとに DX11 / DX12 を選べるようにする。コマンドライン (--renderer=) があれば優先。
    //      レンダラーは起動時に一度だけ生成するため、Editor 起動後に別プロジェクトを開いても
    //      バックエンドは切り替わらない (起動時プロジェクト基準)。
    renderer::RendererBackend rendererBackend = renderer::RendererBackend::DX12;
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

struct UISettings {
    std::string defaultFontPath = "Assets/Fonts/Default/Roboto/Roboto-VariableFont_wdth,wght";
};

// カーソルの «絵» だけを持つ設定。拘束と表示はここには置かない。
//
// WHY 拘束モードを設定から外したか (2026-09-06): 以前はここが «起動時の初期値» を
//     持っていたが、実行中はスクリプトが正本という二重構造になっていた。同じ 1 つの値を
//     «初期値» と «今の要求» で共有していたため、Play 中に設定を触るとスクリプトの
//     要求が黙って消える。カーソルを取るかどうかは画面ごとに変わるゲームの都合で、
//     プロジェクト全体の設定として持てるものではない。
//     残したのは «どの絵を使うか» — こちらは差し替え可能なプロジェクトの資産であり、
//     スクリプトは種類 (CursorShape) だけを指せばよくなる。
struct CursorAppearance {
    struct ShapeImage {
        std::string path;              // プロジェクトルートからの相対パス。空なら OS の既定矢印
        float       hotspotX = 0.0f;   // 画像左上から «実際に指す点» までの画素
        float       hotspotY = 0.0f;
    };

    // OS カーソルの絵を差し替えるか。false なら常に既定の矢印を使う。
    bool hardwareCursor = true;
    std::array<ShapeImage, core::kCursorShapeCount> shapes{};

    // 画像を読み込んで OS へ適用する。projectRoot は絶対パス。
    // Play 開始時と Standalone の起動時に 1 回だけ呼ぶ。
    void Apply(const std::string& projectRoot) const;
};

struct ProjectMetadataSettings {
    std::string name;
    std::string defaultScene = "Assets/Scenes/Main.scene";
};

struct RuntimeSettings {
    std::string startScene = "Assets/Scenes/Main.scene";
};

// ゲーム固有の設定をひとまとめにするサブ構造体。
// WHY: ProjectSettings には「エンジン共通設定」と「ゲーム固有設定」が混在していた。
//      タグ・レイヤー名・シーンパス等ゲームが書き換えるフィールドを game に集約することで、
//      エンジン側コードが誤ってゲーム設定を参照する依存を防ぐ。
struct GameProjectConfig {
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
};

struct ProjectSettings {
    GameProjectConfig            game;     // ゲーム固有設定 (タグ・レイヤー・シーンパス等)
    PhysicsSettings              physics;  // エンジン設定 (以下同様)
    renderer::RenderSettings     render;
    AudioSettings                audio;
    UISettings                   ui;
    ScreenSettings               screen;
    AppSettings                  app;
    WindowSettings               window;
    CursorAppearance             cursor;   // カーソルの絵だけ。拘束と表示はスクリプトが持つ

    static ProjectSettings Default();
    bool Load(const std::string& path);
    bool Save(const std::string& path) const;
};

} // namespace fbzz
