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
#include <Physics/Layer.hpp>
#include <string>
#include <vector>

namespace fbzz {

struct PhysicsSettings {
    int           hz       = 60;
    int           substeps = 1;
    math::Vector3 gravity  = { 0.0f, -9.81f, 0.0f };

    /// @brief レイヤー同士がぶつかるか。既定は «全部ぶつかる»。
    /// @note ボーンに生やした当たり (BossHitboxRigComponent) のように「親の剛体とは当たらないがプレイヤーとは
    ///       当たる」形があるため、BroadPhase の layerFilter へ渡す値の置き場として持つ。
    /// @note 対称行列で持つ。«A は B を無視するが B は A を見る» は解けない要求で (衝突解決は 1 組に 1 回)、
    ///       片側だけ書けると必ず食い違う。
    LayerCollisionMatrix collisionMatrix;
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
    int targetFps = 60;  ///< 0 = unlimited
    /// @brief 描画バックエンド (dx12)。Standalone / Editor とも「起動時プロジェクト」のこの値でレンダラーを生成する
    ///        (EditorLauncher が Application::Init より前に先読みして渡す)。
    /// @note コマンドライン (--renderer=) があれば優先。レンダラーは起動時に一度だけ生成するため、Editor 起動後に
    ///       別プロジェクトを開いてもバックエンドは切り替わらない (起動時プロジェクト基準)。
    renderer::RendererBackend rendererBackend = renderer::RendererBackend::DX12;
};

/// Standalone モード (配布ゲーム) のウィンドウ設定。
/// Application::Init() より前に ProjectSettings を読み込み、
/// 正しいサイズ・タイトルでウィンドウを生成するために使う。
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
/// @note 実行中の要求はスクリプト (cursor.Push) が正本。設定と二重に持つと Play 中の変更が黙って消えるため、
///       差し替え可能な «どの絵を使うか» だけをここに残す。
struct CursorAppearance {
    struct ShapeImage {
        std::string path;              ///< プロジェクトルートからの相対パス。空なら OS の既定矢印
        float       hotspotX = 0.0f;   ///< 画像左上から «実際に指す点» までの画素
        float       hotspotY = 0.0f;
    };

    /// OS カーソルの絵を差し替えるか。false なら常に既定の矢印を使う。
    bool hardwareCursor = true;
    std::array<ShapeImage, core::kCursorShapeCount> shapes{};

    /// 画像を読み込んで OS へ適用する。projectRoot は絶対パス。
    /// Play 開始時と Standalone の起動時に 1 回だけ呼ぶ。
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
/// @note タグ・レイヤー名・シーンパス等ゲームが書き換えるフィールドを game に集約し、エンジン側コードが誤って
///       ゲーム設定を参照する依存を防ぐ。
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
    GameProjectConfig            game;     ///< ゲーム固有設定 (タグ・レイヤー・シーンパス等)
    PhysicsSettings              physics;  ///< エンジン設定 (以下同様)
    renderer::RenderSettings     render;
    AudioSettings                audio;
    UISettings                   ui;
    ScreenSettings               screen;
    AppSettings                  app;
    WindowSettings               window;
    CursorAppearance             cursor;   ///< カーソルの絵だけ。拘束と表示はスクリプトが持つ

    static ProjectSettings Default();
    bool Load(const std::string& path);
    bool Save(const std::string& path) const;
    /// @brief Save が書く TOML 本文。
    /// @note エディターの自動保存が «前回保存した内容と違うか» の比較に使う (構造体に == が無いため)。
    [[nodiscard]] std::string ToToml() const;
};

} // namespace fbzz
